#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#define SWIPE_THRESHOLD 45.0
#define AXIS_RATIO      1.3

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static int open_restricted(const char *path, int flags, void *user_data) {
    (void)user_data;
    int fd = open(path, flags);
    return fd < 0 ? -errno : fd;
}

static void close_restricted(int fd, void *user_data) {
    (void)user_data;
    close(fd);
}

static const struct libinput_interface li_iface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

static long x11_get_cardinal(Display *dpy, Window root, Atom prop, long fallback) {
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    long val = fallback;

    if (XGetWindowProperty(dpy, root, prop, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success && data) {
        if (nitems >= 1 && actual_fmt == 32) {
            val = *(long *)data;
        }
        XFree(data);
    }
    return val;
}

static void switch_workspace(Display *dpy, int delta) {
    Window root = DefaultRootWindow(dpy);
    Atom a_num = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    Atom a_cur = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);

    long count = x11_get_cardinal(dpy, root, a_num, 4);
    long cur = x11_get_cardinal(dpy, root, a_cur, 0);
    if (count <= 1) return;

    long next = (cur + delta) % count;
    if (next < 0) next += count;

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = root;
    ev.xclient.message_type = a_cur;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = next;
    ev.xclient.data.l[1] = CurrentTime;

    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

static void set_active_maximized(Display *dpy, int maximize) {
    Window root = DefaultRootWindow(dpy);
    Atom a_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    Window active_win = None;

    if (XGetWindowProperty(dpy, root, a_active, 0, 1, False, XA_WINDOW,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success && data) {
        if (nitems >= 1 && actual_fmt == 32) {
            active_win = *(Window *)data;
        }
        XFree(data);
    }
    if (active_win == None) return;

    Atom a_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom a_max_v = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    Atom a_max_h = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = active_win;
    ev.xclient.message_type = a_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = maximize ? 1 : 0; /* 1 = _NET_WM_STATE_ADD, 0 = REMOVE */
    ev.xclient.data.l[1] = (long)a_max_v;
    ev.xclient.data.l[2] = (long)a_max_h;
    ev.xclient.data.l[3] = 1;

    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

int main(void) {
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[phantom-gestures] Cannot open X display\n");
        return 1;
    }

    struct udev *udev = udev_new();
    if (!udev) {
        XCloseDisplay(dpy);
        return 1;
    }

    struct libinput *li = libinput_udev_create_context(&li_iface, NULL, udev);
    if (!li || libinput_udev_assign_seat(li, "seat0") != 0) {
        if (li) libinput_unref(li);
        udev_unref(udev);
        XCloseDisplay(dpy);
        return 1;
    }

    int gesture_devices = 0;
    int active_fingers = 0;
    double accum_dx = 0.0;
    double accum_dy = 0.0;

    libinput_dispatch(li);
    struct libinput_event *ev;
    while ((ev = libinput_get_event(li)) != NULL) {
        if (libinput_event_get_type(ev) == LIBINPUT_EVENT_DEVICE_ADDED) {
            struct libinput_device *dev = libinput_event_get_device(ev);
            if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_GESTURE)) {
                gesture_devices++;
            }
        }
        libinput_event_destroy(ev);
    }

    /* Exit immediately with 0 memory footprint if no gesture touchpad is attached (e.g., QEMU) */
    if (gesture_devices == 0) {
        libinput_unref(li);
        udev_unref(udev);
        XCloseDisplay(dpy);
        return 0;
    }

    struct pollfd pfd;
    pfd.fd = libinput_get_fd(li);
    pfd.events = POLLIN;

    while (g_running) {
        int rc = poll(&pfd, 1, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (libinput_dispatch(li) < 0) break;

        while ((ev = libinput_get_event(li)) != NULL) {
            enum libinput_event_type type = libinput_event_get_type(ev);
            if (type == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN) {
                struct libinput_event_gesture *g = libinput_event_get_gesture_event(ev);
                active_fingers = libinput_event_gesture_get_finger_count(g);
                accum_dx = 0.0;
                accum_dy = 0.0;
            } else if (type == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE) {
                struct libinput_event_gesture *g = libinput_event_get_gesture_event(ev);
                accum_dx += libinput_event_gesture_get_dx(g);
                accum_dy += libinput_event_gesture_get_dy(g);
            } else if (type == LIBINPUT_EVENT_GESTURE_SWIPE_END) {
                struct libinput_event_gesture *g = libinput_event_get_gesture_event(ev);
                if (!libinput_event_gesture_get_cancelled(g) && active_fingers >= 3) {
                    double ax = fabs(accum_dx);
                    double ay = fabs(accum_dy);
                    if (ax >= SWIPE_THRESHOLD && ax > ay * AXIS_RATIO) {
                        switch_workspace(dpy, (accum_dx < 0.0) ? 1 : -1);
                    } else if (ay >= SWIPE_THRESHOLD && ay > ax * AXIS_RATIO) {
                        set_active_maximized(dpy, (accum_dy < 0.0) ? 1 : 0);
                    }
                }
                active_fingers = 0;
            }
            libinput_event_destroy(ev);
        }
    }

    libinput_unref(li);
    udev_unref(udev);
    XCloseDisplay(dpy);
    return 0;
}
