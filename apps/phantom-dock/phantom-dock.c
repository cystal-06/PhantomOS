#define _GNU_SOURCE
#include <cairo.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ICON_DIR48 "/usr/share/icons/PhantomGNOME/48x48"
#define WALL_PATH  "/usr/share/backgrounds/phantomos.png"
#define MAX_ITEMS  8

typedef struct {
    double x, y, w, h;
} Rect;

typedef struct {
    const char *name;
    const char *match_key;
    const char *icon_name;
    const char *cmd;
    GdkPixbuf *pb48;
    int running;
    Window match_win;
    long match_ws;
    double scale;
} DockItem;

typedef struct {
    GtkWidget *window;
    GtkWidget *area;
    Window edge_win;
    int edge_x0, edge_x1, edge_y0;
    cairo_surface_t *bg_surf;

    int scr_w;
    int scr_h;
    int win_w;
    int win_h;
    int dock_x;
    int shown_y;
    int hidden_y;
    int cur_y;

    DockItem items[MAX_ITEMS];
    int item_count;
    Rect r_items[MAX_ITEMS];
    Rect r_dots;

    int has_open_tabs;
    int overview_open;
    int pointer_hot;
    int sliding_up;
    gint64 hot_until_us;

    double show_t;       /* 0.0 = hidden, 1.0 = shown */
    double hx, hy;
    int hovering;
    int hover_idx;       /* -1 none, 0..item_count-1 item, 99 dots */
    guint anim_id;
    gint64 last_tick_us;
} DockApp;

static int pt_in_rect(double px, double py, Rect r) {
    return r.w > 0 && px >= r.x && px <= (r.x + r.w) && py >= r.y && py <= (r.y + r.h);
}

static double ease_out_cubic(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    double inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
}

static double smoothstep(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return t * t * (3.0 - 2.0 * t);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w * 0.5) r = w * 0.5;
    if (r > h * 0.5) r = h * 0.5;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

static void spawn_cmd(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    GError *err = NULL;
    if (!g_spawn_command_line_async(cmd, &err)) {
        if (err) g_error_free(err);
    }
}

static long x11_get_card(Display *dpy, Window win, Atom prop, long fallback) {
    Atom at = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    long val = fallback;
    if (XGetWindowProperty(dpy, win, prop, 0, 1, False, AnyPropertyType,
                           &at, &fmt, &n, &after, &data) == Success && data) {
        if (n >= 1 && fmt == 32) val = *(long *)data;
        XFree(data);
    }
    return val;
}

static int window_has_atom(Display *dpy, Window win, Atom prop, Atom target) {
    Atom at = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    int found = 0;
    if (XGetWindowProperty(dpy, win, prop, 0, 32, False, XA_ATOM,
                           &at, &fmt, &n, &after, &data) == Success && data) {
        Atom *arr = (Atom *)data;
        for (unsigned long i = 0; i < n; i++) {
            if (arr[i] == target) { found = 1; break; }
        }
        XFree(data);
    }
    return found;
}

static void focus_x11_window(Display *dpy, Window win, long target_ws) {
    if (!dpy || win == None) return;
    Window root = DefaultRootWindow(dpy);
    Atom a_cur = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    long cur_ws = x11_get_card(dpy, root, a_cur, 0);

    if (target_ws >= 0 && target_ws != cur_ws) {
        XEvent wev;
        memset(&wev, 0, sizeof(wev));
        wev.xclient.type = ClientMessage;
        wev.xclient.window = root;
        wev.xclient.message_type = a_cur;
        wev.xclient.format = 32;
        wev.xclient.data.l[0] = target_ws;
        wev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &wev);
    }

    XMapRaised(dpy, win);
    XEvent aev;
    memset(&aev, 0, sizeof(aev));
    aev.xclient.type = ClientMessage;
    aev.xclient.window = win;
    aev.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    aev.xclient.format = 32;
    aev.xclient.data.l[0] = 2;
    aev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &aev);
    XFlush(dpy);
}

static void build_bg_surface(DockApp *app) {
    if (app->bg_surf) {
        cairo_surface_destroy(app->bg_surf);
        app->bg_surf = NULL;
    }
    app->bg_surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, app->win_w, app->win_h);
    cairo_t *cr = cairo_create(app->bg_surf);

    GdkPixbuf *wp = gdk_pixbuf_new_from_file_at_scale(WALL_PATH, app->scr_w, app->scr_h, FALSE, NULL);
    if (wp) {
        int sy = app->shown_y;
        if (sy < 0) sy = 0;
        if (sy + app->win_h > app->scr_h) sy = app->scr_h - app->win_h;
        GdkPixbuf *strip = gdk_pixbuf_new_subpixbuf(wp, app->dock_x, sy, app->win_w, app->win_h);
        if (strip) {
            int dw = app->win_w / 10;
            int dh = app->win_h / 6;
            if (dw < 24) dw = 24;
            if (dh < 8) dh = 8;
            GdkPixbuf *small = gdk_pixbuf_scale_simple(strip, dw, dh, GDK_INTERP_BILINEAR);
            g_object_unref(strip);
            if (small) {
                cairo_save(cr);
                cairo_scale(cr, (double)app->win_w / dw, (double)app->win_h / dh);
                gdk_cairo_set_source_pixbuf(cr, small, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
                cairo_paint(cr);
                cairo_restore(cr);
                g_object_unref(small);
            }
        }
        g_object_unref(wp);
    } else {
        cairo_set_source_rgb(cr, 0.08, 0.09, 0.14);
        cairo_paint(cr);
    }

    /* Frosted translucent glass tint + crisp top highlight */
    cairo_pattern_t *glass = cairo_pattern_create_linear(0, 0, 0, app->win_h);
    cairo_pattern_add_color_stop_rgba(glass, 0.0, 0.157, 0.173, 0.247, 0.78);
    cairo_pattern_add_color_stop_rgba(glass, 1.0, 0.086, 0.094, 0.141, 0.90);
    cairo_set_source(cr, glass);
    cairo_paint(cr);
    cairo_pattern_destroy(glass);

    rounded_rect(cr, 0.75, 0.75, app->win_w - 1.5, app->win_h - 1.5, 20.5);
    cairo_set_source_rgba(cr, 0.85, 0.90, 1.0, 0.18);
    cairo_set_line_width(cr, 1.1);
    cairo_stroke(cr);

    double div_x = app->win_w - 54.0;
    cairo_set_source_rgba(cr, 0.80, 0.84, 0.96, 0.18);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, div_x, 15.0);
    cairo_line_to(cr, div_x, app->win_h - 15.0);
    cairo_stroke(cr);

    cairo_destroy(cr);
}

static void apply_pill_shape(DockApp *app) {
    if (!app->window || !app->window->window) return;
    GdkPixmap *mask = gdk_pixmap_new(NULL, app->win_w, app->win_h, 1);
    if (!mask) return;
    cairo_t *cr = gdk_cairo_create(GDK_DRAWABLE(mask));
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    rounded_rect(cr, 0, 0, app->win_w, app->win_h, 21.0);
    cairo_fill(cr);
    cairo_destroy(cr);
    gdk_window_shape_combine_mask(app->window->window, mask, 0, 0);
    g_object_unref(mask);
}

static void update_hover_idx(DockApp *app, double x, double y) {
    app->hover_idx = -1;
    for (int i = 0; i < app->item_count; i++) {
        if (pt_in_rect(x, y, app->r_items[i])) {
            app->hover_idx = i;
            return;
        }
    }
    if (pt_in_rect(x, y, app->r_dots)) {
        app->hover_idx = 99;
    }
}

static void start_anim(DockApp *app);

static void probe_pointer_state(DockApp *app) {
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (!dpy) return;
    Window root = DefaultRootWindow(dpy);
    Window r_ret = None, c_ret = None;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask_ret = 0;
    if (!XQueryPointer(dpy, root, &r_ret, &c_ret, &rx, &ry, &wx, &wy, &mask_ret)) return;

    int in_edge = (rx >= app->edge_x0 && rx <= app->edge_x1 && ry >= app->scr_h - 14);
    int in_dock = (app->show_t > 0.05 &&
                   rx >= app->dock_x - 16 && rx <= app->dock_x + app->win_w + 16 &&
                   ry >= app->cur_y - 10 && ry <= app->cur_y + app->win_h);

    if (in_edge || in_dock) {
        app->pointer_hot = 1;
        app->hot_until_us = g_get_monotonic_time() + 260000;
        if (in_dock && ry >= app->cur_y && ry < app->cur_y + app->win_h &&
            app->cur_y <= app->shown_y + 4) {
            app->hx = rx - app->dock_x;
            app->hy = ry - app->cur_y;
            app->hovering = 1;
            update_hover_idx(app, app->hx, app->hy);
        } else {
            if (app->hovering || app->hover_idx != -1) {
                app->hovering = 0;
                app->hover_idx = -1;
            }
        }
        start_anim(app);
    } else {
        app->pointer_hot = 0;
        if (app->hovering || app->hover_idx != -1) {
            app->hovering = 0;
            app->hover_idx = -1;
            start_anim(app);
        }
    }
}

static int probe_x11_windows(DockApp *app) {
    int prev_ov = app->overview_open;
    int prev_tabs = app->has_open_tabs;
    int prev_run = 0;
    for (int i = 0; i < app->item_count; i++) {
        if (app->items[i].running) prev_run |= (1 << i);
        app->items[i].running = 0;
        app->items[i].match_win = None;
        app->items[i].match_ws = 0;
    }

    app->overview_open = (access("/tmp/.phantom-overview.pid", F_OK) == 0);
    app->has_open_tabs = 0;

    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (dpy) {
        Window root = DefaultRootWindow(dpy);
        Atom a_cur = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
        Atom a_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
        Atom a_desk = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
        Atom a_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
        Atom a_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
        Atom a_dt = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
        Atom a_state = XInternAtom(dpy, "_NET_WM_STATE", False);
        Atom a_hidden = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
        Atom a_name = XInternAtom(dpy, "_NET_WM_NAME", False);
        Atom a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);

        long cur_ws = x11_get_card(dpy, root, a_cur, 0);

        Atom at = None;
        int fmt = 0;
        unsigned long n = 0, after = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, root, a_list, 0, 64, False, XA_WINDOW,
                               &at, &fmt, &n, &after, &data) == Success && data) {
            Window *wins = (Window *)data;
            for (unsigned long i = 0; i < n; i++) {
                if (window_has_atom(dpy, wins[i], a_type, a_dock) ||
                    window_has_atom(dpy, wins[i], a_type, a_dt)) {
                    continue;
                }
                XWindowAttributes attr;
                if (!XGetWindowAttributes(dpy, wins[i], &attr)) continue;
                if (attr.map_state != IsViewable && !window_has_atom(dpy, wins[i], a_state, a_hidden)) {
                    continue;
                }

                char title[128] = "";
                Atom nt = None;
                int nfmt = 0;
                unsigned long nn = 0, nafter = 0;
                unsigned char *ndata = NULL;
                if (XGetWindowProperty(dpy, wins[i], a_name, 0, 64, False, a_utf8,
                                       &nt, &nfmt, &nn, &nafter, &ndata) == Success && ndata) {
                    g_strlcpy(title, (const char *)ndata, sizeof(title));
                    XFree(ndata);
                }
                if (!title[0]) {
                    char *wmname = NULL;
                    if (XFetchName(dpy, wins[i], &wmname) && wmname) {
                        g_strlcpy(title, wmname, sizeof(title));
                        XFree(wmname);
                    }
                }
                if (strcmp(title, "phantom-panel") == 0 ||
                    strcmp(title, "phantom-panel-qs") == 0 ||
                    strcmp(title, "phantom-panel-ram") == 0 ||
                    strcmp(title, "phantom-dock") == 0 ||
                    strcmp(title, "phantom-overview") == 0 ||
                    (strcmp(title, "pcmanfm") == 0 && attr.width == app->scr_w)) {
                    continue;
                }

                long ws = x11_get_card(dpy, wins[i], a_desk, cur_ws);
                int minimized = window_has_atom(dpy, wins[i], a_state, a_hidden);
                if (!minimized && attr.map_state == IsViewable && (ws == cur_ws || ws == -1)) {
                    app->has_open_tabs = 1;
                }

                for (int k = 0; k < app->item_count; k++) {
                    if (title[0] && strcasestr(title, app->items[k].match_key)) {
                        app->items[k].running = 1;
                        if (app->items[k].match_win == None || ws == cur_ws) {
                            app->items[k].match_win = wins[i];
                            app->items[k].match_ws = ws;
                        }
                    }
                }
            }
            XFree(data);
        }
    }

    int cur_run = 0;
    for (int i = 0; i < app->item_count; i++) {
        if (app->items[i].running) cur_run |= (1 << i);
    }
    return (prev_ov != app->overview_open || prev_tabs != app->has_open_tabs || prev_run != cur_run);
}

static void init_items(DockApp *app) {
    struct {
        const char *name;
        const char *key;
        const char *icon;
        const char *cmd;
    } defs[] = {
        { "Files",          "pcmanfm",     "system-file-manager",     "/usr/bin/pcmanfm" },
        { "Web Browser",    "Firefox",     "web-browser",             "/usr/bin/firefox" },
        { "Terminal",       "Terminal",    "utilities-terminal",      "xterm -u8 -ls -bg \"#181825\" -fg \"#cdd6f4\" -cr \"#89b4fa\" -bd \"#313244\"" },
        { "Editor",         "Leafpad",     "accessories-text-editor", "/usr/bin/leafpad" },
        { "System Monitor", "Monitor",     "applications-system",     "/usr/bin/phantom-monitor" },
        { "Settings",       "Settings",    "preferences-system",      "/usr/bin/phantom-settings" },
        { "Backend Lab",    "Backend Lab", "system-software-install", "xterm -u8 -bg \"#181825\" -fg \"#cdd6f4\" -title \"Backend Lab\" -e /opt/tui-menu/menu.sh" },
    };
    app->item_count = (int)(sizeof(defs) / sizeof(defs[0]));
    for (int i = 0; i < app->item_count && i < MAX_ITEMS; i++) {
        DockItem *it = &app->items[i];
        it->name = defs[i].name;
        it->match_key = defs[i].key;
        it->icon_name = defs[i].icon;
        it->cmd = defs[i].cmd;
        char path[256];
        snprintf(path, sizeof(path), "%s/apps/%s.png", ICON_DIR48, it->icon_name);
        it->pb48 = gdk_pixbuf_new_from_file(path, NULL);
        it->scale = 1.0;
    }
}

static gboolean on_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
    (void)event;
    DockApp *app = (DockApp *)data;
    cairo_t *cr = gdk_cairo_create(widget->window);
    double w = (double)widget->allocation.width;
    double h = (double)widget->allocation.height;

    if (app->bg_surf) {
        cairo_set_source_surface(cr, app->bg_surf, 0, 0);
        cairo_paint(cr);
    } else {
        cairo_set_source_rgb(cr, 0.09, 0.10, 0.15);
        cairo_paint(cr);
    }

    double sp = 56.0;
    double x = 16.0 + sp * 0.5;
    for (int i = 0; i < app->item_count; i++) {
        DockItem *it = &app->items[i];
        Rect r = { x - 26, 4, 52, h - 8 };
        app->r_items[i] = r;

        int hov = (app->hovering && app->hover_idx == i);
        if (hov) {
            rounded_rect(cr, r.x + 2, 7, r.w - 4, h - 14, 14.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14);
            cairo_fill(cr);
        }

        if (it->pb48) {
            double sc = it->scale;
            int pw = gdk_pixbuf_get_width(it->pb48);
            int ph = gdk_pixbuf_get_height(it->pb48);
            double lift = (sc - 1.0) * 18.0;
            cairo_save(cr);
            cairo_translate(cr, x, h * 0.5 - 2.0 - lift);
            cairo_scale(cr, sc, sc);
            gdk_cairo_set_source_pixbuf(cr, it->pb48, -pw * 0.5, -ph * 0.5);
            cairo_paint(cr);
            cairo_restore(cr);
        }

        if (it->running) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, x, h - 7.5, 2.5, 0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, 0.537, 0.706, 0.980, 0.95);
            cairo_fill(cr);
        }
        x += sp;
    }

    double dcx = w - 29.0;
    double dcy = h * 0.5;
    app->r_dots = (Rect){ w - 48.0, 6.0, 38.0, h - 12.0 };
    if (app->hovering && app->hover_idx == 99) {
        rounded_rect(cr, app->r_dots.x, 8.0, app->r_dots.w, h - 16.0, 13.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14);
        cairo_fill(cr);
    }
    cairo_set_source_rgba(cr, 0.92, 0.95, 1.0, 0.92);
    for (int gy = -1; gy <= 1; gy++) {
        for (int gx = -1; gx <= 1; gx++) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, dcx + gx * 6.8, dcy + gy * 6.8, 2.0, 0, 2.0 * M_PI);
            cairo_fill(cr);
        }
    }

    cairo_destroy(cr);
    return FALSE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    (void)w;
    DockApp *app = (DockApp *)data;
    app->hx = ev->x;
    app->hy = ev->y;
    app->hovering = 1;
    app->pointer_hot = 1;
    app->hot_until_us = g_get_monotonic_time() + 260000;
    int prev = app->hover_idx;
    update_hover_idx(app, ev->x, ev->y);
    if (prev != app->hover_idx) start_anim(app);
    return FALSE;
}

static gboolean on_enter(GtkWidget *w, GdkEventCrossing *ev, gpointer data) {
    (void)w;
    DockApp *app = (DockApp *)data;
    app->hx = ev->x;
    app->hy = ev->y;
    app->hovering = 1;
    app->pointer_hot = 1;
    app->hot_until_us = g_get_monotonic_time() + 260000;
    update_hover_idx(app, ev->x, ev->y);
    start_anim(app);
    return FALSE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer data) {
    (void)w;
    (void)ev;
    DockApp *app = (DockApp *)data;
    probe_pointer_state(app);
    return FALSE;
}

static void hide_dock_immediately(DockApp *app) {
    app->pointer_hot = 0;
    app->hot_until_us = 0;
    app->hovering = 0;
    app->hover_idx = -1;
    app->sliding_up = 0;
    app->show_t = 0.0;
    app->cur_y = app->hidden_y;
    for (int i = 0; i < app->item_count; i++) {
        app->items[i].scale = 1.0;
    }
    if (app->anim_id) {
        g_source_remove(app->anim_id);
        app->anim_id = 0;
        app->last_tick_us = 0;
    }
    if (app->window && GTK_WIDGET_VISIBLE(app->window)) {
        gtk_widget_hide(app->window);
    }
    if (app->edge_win) {
        Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
        if (dpy) XRaiseWindow(dpy, app->edge_win);
    }
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    DockApp *app = (DockApp *)data;
    if (ev->button != 1 || ev->type != GDK_BUTTON_PRESS) return FALSE;
    update_hover_idx(app, ev->x, ev->y);

    if (app->hover_idx == 99) {
        app->overview_open = 1;
        hide_dock_immediately(app);
        gdk_flush();
        g_usleep(15000);
        spawn_cmd("/usr/bin/phantom-overview");
        return TRUE;
    }
    if (app->hover_idx >= 0 && app->hover_idx < app->item_count) {
        DockItem *it = &app->items[app->hover_idx];
        if (it->running && it->match_win != None && !(ev->state & GDK_SHIFT_MASK)) {
            Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
            focus_x11_window(dpy, it->match_win, it->match_ws);
        } else {
            spawn_cmd(it->cmd);
        }
        return TRUE;
    }
    return FALSE;
}

static void sync_dock_window_pos(DockApp *app) {
    if (!app->window || !app->window->window) return;
    if (app->show_t <= 0.001) {
        if (GTK_WIDGET_VISIBLE(app->window)) {
            gtk_widget_hide(app->window);
            if (app->edge_win) {
                Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
                if (dpy) XRaiseWindow(dpy, app->edge_win);
            }
        }
        app->cur_y = app->hidden_y;
        return;
    }

    double e = app->sliding_up ? ease_out_cubic(app->show_t) : smoothstep(app->show_t);
    int ny = (int)(app->hidden_y + (app->shown_y - app->hidden_y) * e);

    if (!GTK_WIDGET_VISIBLE(app->window)) {
        app->cur_y = ny;
        gtk_window_move(GTK_WINDOW(app->window), app->dock_x, ny);
        gtk_widget_show_all(app->window);
        if (app->area && app->area->window) {
            gdk_window_set_back_pixmap(app->area->window, NULL, FALSE);
        }
        gdk_window_move(app->window->window, app->dock_x, ny);
        gdk_window_raise(app->window->window);
        if (app->edge_win) XRaiseWindow(GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), app->edge_win);
        gdk_window_process_updates(app->window->window, TRUE);
        gdk_flush();
    } else if (ny != app->cur_y) {
        app->cur_y = ny;
        /* Direct XMoveWindow via override-redirect GdkWindow = zero WM lag/flicker */
        gdk_window_move(app->window->window, app->dock_x, ny);
        gdk_flush();
    }
}

static gboolean on_anim_tick(gpointer data) {
    DockApp *app = (DockApp *)data;
    gint64 now = g_get_monotonic_time();
    double dt = (app->last_tick_us > 0) ? ((double)(now - app->last_tick_us) / 1000000.0) : (1.0 / 60.0);
    app->last_tick_us = now;
    if (dt < 0.004) dt = 0.004;
    if (dt > 0.045) dt = 0.045;

    int grace = (now < app->hot_until_us);
    if (app->overview_open || access("/tmp/.phantom-overview.pid", F_OK) == 0) {
        app->overview_open = 1;
        app->anim_id = 0; /* Returning FALSE destroys the active timer source cleanly */
        app->last_tick_us = 0;
        hide_dock_immediately(app);
        return FALSE;
    }
    int want_visible = !app->has_open_tabs || app->pointer_hot || grace;
    app->sliding_up = want_visible;

    double target = want_visible ? 1.0 : 0.0;
    int active = 0;
    int dirty = 0;

    if (!want_visible) {
        app->hovering = 0;
        app->hover_idx = -1;
        for (int i = 0; i < app->item_count; i++) {
            if (app->items[i].scale != 1.0) {
                app->items[i].scale = 1.0;
                dirty = 1;
            }
        }
    } else if (app->show_t >= 0.99) {
        double k = 1.0 - exp(-22.0 * dt);
        for (int i = 0; i < app->item_count; i++) {
            double ts = (app->hovering && app->hover_idx == i) ? 1.15 : 1.0;
            if (fabs(app->items[i].scale - ts) > 0.004) {
                app->items[i].scale += (ts - app->items[i].scale) * k;
                dirty = 1;
                active = 1;
            } else if (app->items[i].scale != ts) {
                app->items[i].scale = ts;
                dirty = 1;
                active = 1;
            }
        }
    }

    if (dirty && GTK_WIDGET_VISIBLE(app->window)) {
        gtk_widget_queue_draw(app->area);
        if (!want_visible) {
            /* Flush static scale=1.0 frame before XMoveWindow slides down */
            gdk_window_process_updates(app->area->window, FALSE);
        }
    }

    if (fabs(app->show_t - target) > 0.003) {
        double step = (target > app->show_t) ? (dt / 0.18) : -(dt / 0.25);
        app->show_t += step;
        if ((step > 0 && app->show_t > target) || (step < 0 && app->show_t < target)) {
            app->show_t = target;
        }
        sync_dock_window_pos(app);
        active = 1;
    } else if (app->show_t != target) {
        app->show_t = target;
        sync_dock_window_pos(app);
        active = 1;
    }

    if (!active && !grace) {
        app->anim_id = 0;
        app->last_tick_us = 0;
        return FALSE;
    }
    return TRUE;
}

static void start_anim(DockApp *app) {
    if (!app->anim_id) {
        app->last_tick_us = g_get_monotonic_time();
        /* 10ms high-priority driver tick avoids beat-frequency judder against 60Hz scanout */
        app->anim_id = g_timeout_add_full(G_PRIORITY_HIGH, 10, on_anim_tick, app, NULL);
    }
}

static void on_screen_resized(GdkScreen *scr, gpointer data);

static gboolean on_probe_tick(gpointer data) {
    DockApp *app = (DockApp *)data;
    static int tick_n = 0;
    GdkScreen *scr = gdk_screen_get_default();
    if (scr) {
        on_screen_resized(scr, app);
    }
    probe_pointer_state(app);
    int ov = (access("/tmp/.phantom-overview.pid", F_OK) == 0);
    if (ov) {
        if (!app->overview_open || (app->window && GTK_WIDGET_VISIBLE(app->window))) {
            app->overview_open = 1;
            hide_dock_immediately(app);
        }
        return TRUE;
    } else if (app->overview_open) {
        app->overview_open = 0;
        app->hot_until_us = 0;
    }
    tick_n++;
    /* Defer synchronous Xlib property polling while actively sliding */
    if ((app->show_t <= 0.01 || app->show_t >= 0.99) && (tick_n % 6) == 0) {
        int changed = probe_x11_windows(app);
        if (changed) {
            if (app->edge_win && !app->overview_open) {
                Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
                if (dpy) XRaiseWindow(dpy, app->edge_win);
            }
            if (GTK_WIDGET_VISIBLE(app->window)) gtk_widget_queue_draw(app->area);
            start_anim(app);
        }
    }
    if (!app->overview_open && !app->has_open_tabs && app->show_t < 1.0) {
        start_anim(app);
    }
    return TRUE;
}

static GdkFilterReturn on_x11_filter(GdkXEvent *xev, GdkEvent *ev, gpointer data) {
    (void)ev;
    DockApp *app = (DockApp *)data;
    XEvent *e = (XEvent *)xev;
    if (!app->overview_open && app->edge_win && e->xany.window == app->edge_win &&
        (e->type == EnterNotify || e->type == MotionNotify)) {
        app->pointer_hot = 1;
        app->hot_until_us = g_get_monotonic_time() + 260000;
        start_anim(app);
    }
    return GDK_FILTER_CONTINUE;
}

static void compute_edge_bounds(DockApp *app) {
    int pad = (app->scr_w > 100) ? 16 : 0;
    app->edge_x0 = pad;
    app->edge_x1 = app->scr_w - pad;
    app->edge_y0 = app->scr_h - 6;
}

static void create_bottom_edge_trigger(DockApp *app) {
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (!dpy) return;
    Window root = DefaultRootWindow(dpy);

    compute_edge_bounds(app);

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.event_mask = EnterWindowMask | PointerMotionMask;
    app->edge_win = XCreateWindow(
        dpy, root,
        app->edge_x0, app->edge_y0,
        (unsigned int)(app->edge_x1 - app->edge_x0), 6,
        0, CopyFromParent, InputOnly, CopyFromParent,
        CWOverrideRedirect | CWEventMask, &swa);
    XMapRaised(dpy, app->edge_win);
    XFlush(dpy);

    gdk_window_add_filter(NULL, on_x11_filter, app);
}

static void on_realize(GtkWidget *w, gpointer data) {
    (void)w;
    DockApp *app = (DockApp *)data;
    if (app->window->window) {
        gdk_window_set_override_redirect(app->window->window, TRUE);
        gdk_window_set_back_pixmap(app->window->window, NULL, FALSE);
    }
    apply_pill_shape(app);
}

static void on_area_realize(GtkWidget *w, gpointer data) {
    (void)data;
    if (w->window) {
        gdk_window_set_back_pixmap(w->window, NULL, FALSE);
    }
}

static void on_screen_resized(GdkScreen *scr, gpointer data) {
    DockApp *app = (DockApp *)data;
    int nw = gdk_screen_get_width(scr);
    int nh = gdk_screen_get_height(scr);
    if (nw >= 800 && nh >= 600 && (nw != app->scr_w || nh != app->scr_h)) {
        app->scr_w = nw;
        app->scr_h = nh;
        app->dock_x = (app->scr_w - app->win_w) / 2;
        app->shown_y = app->scr_h - app->win_h - 14;
        app->hidden_y = app->scr_h + 2;
        build_bg_surface(app);
        if (app->edge_win) {
            Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
            compute_edge_bounds(app);
            XMoveResizeWindow(dpy, app->edge_win, app->edge_x0, app->edge_y0,
                              (unsigned int)(app->edge_x1 - app->edge_x0), 6);
            XRaiseWindow(dpy, app->edge_win);
            XFlush(dpy);
        }
        sync_dock_window_pos(app);
        gtk_widget_queue_draw(app->area);
    }
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    DockApp app;
    memset(&app, 0, sizeof(app));
    app.hover_idx = -1;
    init_items(&app);

    GdkScreen *scr = gdk_screen_get_default();
    app.scr_w = gdk_screen_get_width(scr);
    app.scr_h = gdk_screen_get_height(scr);
    if (app.scr_w < 800) app.scr_w = 1920;
    if (app.scr_h < 600) app.scr_h = 1080;

    app.win_w = 32 + app.item_count * 56 + 50;
    app.win_h = 66;
    app.dock_x = (app.scr_w - app.win_w) / 2;
    app.shown_y = app.scr_h - app.win_h - 14;
    app.hidden_y = app.scr_h + 2;

    probe_x11_windows(&app);
    app.show_t = (!app.has_open_tabs && !app.overview_open) ? 1.0 : 0.0;
    app.sliding_up = (app.show_t > 0.5);
    app.cur_y = (app.show_t > 0.5) ? app.shown_y : app.hidden_y;

    build_bg_surface(&app);
    create_bottom_edge_trigger(&app);

    /* GTK_WINDOW_POPUP = override-redirect X11 window for zero-WM-lag 60fps slide */
    app.window = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_title(GTK_WINDOW(app.window), "phantom-dock");
    gtk_widget_set_app_paintable(app.window, TRUE);
    gtk_window_set_default_size(GTK_WINDOW(app.window), app.win_w, app.win_h);
    gtk_widget_set_size_request(app.window, app.win_w, app.win_h);

    g_signal_connect(app.window, "realize", G_CALLBACK(on_realize), &app);
    g_signal_connect(scr, "size-changed", G_CALLBACK(on_screen_resized), &app);

    app.area = gtk_drawing_area_new();
    gtk_widget_set_app_paintable(app.area, TRUE);
    gtk_widget_set_double_buffered(app.area, TRUE);
    gtk_widget_set_size_request(app.area, app.win_w, app.win_h);
    gtk_widget_add_events(app.area,
        GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK |
        GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(app.area, "realize", G_CALLBACK(on_area_realize), &app);
    g_signal_connect(app.area, "expose-event", G_CALLBACK(on_expose), &app);
    g_signal_connect(app.area, "button-press-event", G_CALLBACK(on_button_press), &app);
    g_signal_connect(app.area, "motion-notify-event", G_CALLBACK(on_motion), &app);
    g_signal_connect(app.area, "enter-notify-event", G_CALLBACK(on_enter), &app);
    g_signal_connect(app.area, "leave-notify-event", G_CALLBACK(on_leave), &app);
    gtk_container_add(GTK_CONTAINER(app.window), app.area);

    gtk_widget_realize(app.window);
    sync_dock_window_pos(&app);

    g_timeout_add(25, on_probe_tick, &app);
    gtk_main();

    if (app.bg_surf) cairo_surface_destroy(app.bg_surf);
    return 0;
}
