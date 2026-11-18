#define _POSIX_C_SOURCE 200809L
#include <cairo.h>
#include <ctype.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#define PANEL_HEIGHT 28
#define HIST_LEN     28

typedef struct {
    double x, y, w, h;
} Rect;

typedef struct {
    GtkWidget *window;
    GtkWidget *area;
    int width;
    int height;

    /* CPU stats */
    unsigned long long prev_total;
    unsigned long long prev_idle;
    unsigned long long prev_sys;
    int cpu_init;
    double cpu_total_hist[HIST_LEN];
    double cpu_sys_hist[HIST_LEN];
    double cpu_pct;
    double cpu_ghz;

    /* RAM & Disk */
    double ram_pct;
    double ram_used_mb;
    double ram_total_mb;
    double disk_pct;
    double disk_used_mb;
    double disk_total_mb;

    /* Network */
    unsigned long long prev_rx;
    unsigned long long prev_tx;
    int net_init;
    double rx_hist[HIST_LEN];
    double tx_hist[HIST_LEN];
    double rx_kbps;
    double tx_kbps;

    /* Audio */
    int vol_pct;
    int vol_muted;
    int has_audio;

    /* Workspace */
    long cur_ws;
    long num_ws;

    /* Clock */
    char clock_str[32];

    /* Hover & Hitboxes */
    double hover_x;
    double hover_y;
    int hovering;
    Rect r_ws;
    Rect r_term;
    Rect r_browser;
    Rect r_files;
    Rect r_cpu;
    Rect r_ram;
    Rect r_disk;
    Rect r_net;
    Rect r_vol;
    Rect r_power;
} PanelApp;

static void push_hist(double *arr, double v) {
    memmove(&arr[0], &arr[1], sizeof(double) * (HIST_LEN - 1));
    arr[HIST_LEN - 1] = v;
}

static int pt_in_rect(double px, double py, Rect r) {
    return px >= r.x && px <= (r.x + r.w) && py >= r.y && py <= (r.y + r.h);
}

static void spawn_cmd(const char *cmd) {
    GError *err = NULL;
    if (!g_spawn_command_line_async(cmd, &err)) {
        if (err) g_error_free(err);
    }
}

static long x11_get_cardinal(Display *dpy, Window root, const char *name, long fallback) {
    Atom prop = XInternAtom(dpy, name, False);
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    long val = fallback;
    if (XGetWindowProperty(dpy, root, prop, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success && data) {
        if (nitems >= 1 && actual_fmt == 32) val = *(long *)data;
        XFree(data);
    }
    return val;
}

static void switch_workspace_delta(int delta) {
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (!dpy) return;
    Window root = DefaultRootWindow(dpy);
    long count = x11_get_cardinal(dpy, root, "_NET_NUMBER_OF_DESKTOPS", 4);
    long cur = x11_get_cardinal(dpy, root, "_NET_CURRENT_DESKTOP", 0);
    if (count <= 1) return;
    long next = (cur + delta) % count;
    if (next < 0) next += count;

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = root;
    ev.xclient.message_type = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = next;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

static void sample_cpu(PanelApp *app) {
    FILE *fp = fopen("/proc/stat", "r");
    if (fp) {
        char line[256];
        if (fgets(line, sizeof(line), fp)) {
            unsigned long long u=0, n=0, s=0, id=0, io=0, irq=0, sirq=0, st=0;
            if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &u, &n, &s, &id, &io, &irq, &sirq, &st) >= 4) {
                unsigned long long total = u + n + s + id + io + irq + sirq + st;
                unsigned long long idle = id + io;
                unsigned long long sys = s + irq + sirq;
                if (app->cpu_init && total > app->prev_total) {
                    double dt = (double)(total - app->prev_total);
                    double didle = (double)(idle - app->prev_idle);
                    double dsys = (double)(sys - app->prev_sys);
                    double pct = 100.0 * (dt - didle) / dt;
                    double spct = 100.0 * dsys / dt;
                    if (pct < 0.0) pct = 0.0;
                    if (pct > 100.0) pct = 100.0;
                    if (spct < 0.0) spct = 0.0;
                    if (spct > pct) spct = pct;
                    app->cpu_pct = pct;
                    push_hist(app->cpu_total_hist, pct);
                    push_hist(app->cpu_sys_hist, spct);
                }
                app->prev_total = total;
                app->prev_idle = idle;
                app->prev_sys = sys;
                app->cpu_init = 1;
            }
        }
        fclose(fp);
    }

    /* Frequency: cpufreq first, fallback /proc/cpuinfo */
    FILE *cf = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (cf) {
        unsigned long khz = 0;
        if (fscanf(cf, "%lu", &khz) == 1 && khz > 0) {
            app->cpu_ghz = (double)khz / 1000000.0;
        }
        fclose(cf);
    } else {
        FILE *ci = fopen("/proc/cpuinfo", "r");
        if (ci) {
            char line[256];
            while (fgets(line, sizeof(line), ci)) {
                double mhz = 0.0;
                if (sscanf(line, "cpu MHz : %lf", &mhz) == 1 && mhz > 0.0) {
                    app->cpu_ghz = mhz / 1000.0;
                    break;
                }
            }
            fclose(ci);
        }
    }
}

static void sample_mem_disk(PanelApp *app) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp) {
        unsigned long tot=0, avail=0, fre=0, buf=0, cac=0, sre=0;
        int has_avail = 0;
        char k[64];
        unsigned long v;
        while (fscanf(fp, "%63s %lu kB%*[^\n]", k, &v) == 2) {
            if (strcmp(k, "MemTotal:") == 0) tot = v;
            else if (strcmp(k, "MemAvailable:") == 0) { avail = v; has_avail = 1; }
            else if (strcmp(k, "MemFree:") == 0) fre = v;
            else if (strcmp(k, "Buffers:") == 0) buf = v;
            else if (strcmp(k, "Cached:") == 0) cac = v;
            else if (strcmp(k, "SReclaimable:") == 0) sre = v;
        }
        fclose(fp);
        if (!has_avail) avail = fre + buf + cac + sre;
        unsigned long used = (tot > avail) ? (tot - avail) : 0;
        app->ram_total_mb = (double)tot / 1024.0;
        app->ram_used_mb = (double)used / 1024.0;
        app->ram_pct = (tot > 0) ? (100.0 * (double)used / (double)tot) : 0.0;
    }

    struct statvfs st;
    double used_mb = 0.0, total_mb = 0.0;
    if (statvfs("/mnt/.overlay/lower", &st) == 0 && st.f_blocks > 0) {
        double lb = ((double)(st.f_blocks - st.f_bfree) * st.f_frsize) / (1024.0 * 1024.0);
        used_mb += lb;
        total_mb += ((double)st.f_blocks * st.f_frsize) / (1024.0 * 1024.0);
    }
    if (statvfs("/opt/firefox", &st) == 0 && st.f_blocks > 0 && st.f_bfree == 0) {
        double fb = ((double)st.f_blocks * st.f_frsize) / (1024.0 * 1024.0);
        used_mb += fb;
        total_mb += fb;
    }
    if (statvfs("/", &st) == 0 && st.f_blocks > 0) {
        double ru = ((double)(st.f_blocks - st.f_bfree) * st.f_frsize) / (1024.0 * 1024.0);
        double rt = ((double)st.f_blocks * st.f_frsize) / (1024.0 * 1024.0);
        used_mb += ru;
        if (total_mb <= 0.0) total_mb = rt;
        else total_mb += rt;
    }
    if (statvfs("/var/lib/data", &st) == 0 && st.f_blocks > 0) {
        used_mb += ((double)(st.f_blocks - st.f_bfree) * st.f_frsize) / (1024.0 * 1024.0);
        total_mb += ((double)st.f_blocks * st.f_frsize) / (1024.0 * 1024.0);
    }
    app->disk_used_mb = used_mb;
    app->disk_total_mb = total_mb;
    app->disk_pct = (total_mb > 0.0) ? (100.0 * used_mb / total_mb) : 0.0;
}

static void sample_net(PanelApp *app) {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) return;
    char line[512];
    unsigned long long rx_sum = 0, tx_sum = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *iface = line;
        while (*iface == ' ') iface++;
        if (strcmp(iface, "lo") == 0) continue;
        unsigned long long rx=0, tx=0, d=0;
        if (sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &rx, &d, &d, &d, &d, &d, &d, &d, &tx) == 9) {
            rx_sum += rx;
            tx_sum += tx;
        }
    }
    fclose(fp);

    if (app->net_init) {
        double rxk = (rx_sum >= app->prev_rx) ? ((double)(rx_sum - app->prev_rx) / 1024.0) : 0.0;
        double txk = (tx_sum >= app->prev_tx) ? ((double)(tx_sum - app->prev_tx) / 1024.0) : 0.0;
        app->rx_kbps = rxk;
        app->tx_kbps = txk;
        push_hist(app->rx_hist, rxk);
        push_hist(app->tx_hist, txk);
    }
    app->prev_rx = rx_sum;
    app->prev_tx = tx_sum;
    app->net_init = 1;
}

static void sample_audio(PanelApp *app) {
    if (access("/proc/asound/card0", F_OK) != 0 || access("/usr/bin/amixer", X_OK) != 0) {
        app->has_audio = 0;
        return;
    }
    app->has_audio = 1;
    FILE *fp = popen("amixer get Master 2>/dev/null", "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *p = strchr(line, '[');
        if (p && strchr(line, '%')) {
            int pct = 0;
            if (sscanf(p, "[%d%%]", &pct) == 1) {
                app->vol_pct = pct;
                app->vol_muted = (strstr(line, "[off]") != NULL) ? 1 : 0;
                break;
            }
        }
    }
    pclose(fp);
}

static void sample_all(PanelApp *app) {
    sample_cpu(app);
    sample_mem_disk(app);
    sample_net(app);
    sample_audio(app);

    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (dpy) {
        Window root = DefaultRootWindow(dpy);
        app->num_ws = x11_get_cardinal(dpy, root, "_NET_NUMBER_OF_DESKTOPS", 4);
        app->cur_ws = x11_get_cardinal(dpy, root, "_NET_CURRENT_DESKTOP", 0);
    }

    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(app->clock_str, sizeof(app->clock_str), "%H:%M:%S", &t);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2.0, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3.0 * G_PI / 2.0);
    cairo_close_path(cr);
}

static void draw_hover_pill(cairo_t *cr, PanelApp *app, Rect r) {
    if (app->hovering && pt_in_rect(app->hover_x, app->hover_y, r)) {
        rounded_rect(cr, r.x, r.y, r.w, r.h, 5.0);
        cairo_set_source_rgba(cr, 0.537, 0.706, 0.980, 0.14);
        cairo_fill(cr);
    }
}

static void draw_cpu_icon(cairo_t *cr, double x, double y) {
    cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    cairo_set_line_width(cr, 1.3);
    rounded_rect(cr, x + 3, y + 3, 10, 10, 1.5);
    cairo_stroke(cr);
    cairo_rectangle(cr, x + 6, y + 6, 4, 4);
    cairo_fill(cr);
    /* Pins */
    for (int i = 0; i < 3; i++) {
        double p = x + 5 + i * 3;
        cairo_move_to(cr, p, y + 1); cairo_line_to(cr, p, y + 3);
        cairo_move_to(cr, p, y + 13); cairo_line_to(cr, p, y + 15);
        double q = y + 5 + i * 3;
        cairo_move_to(cr, x + 1, q); cairo_line_to(cr, x + 3, q);
        cairo_move_to(cr, x + 13, q); cairo_line_to(cr, x + 15, q);
    }
    cairo_stroke(cr);
}

static void draw_ram_icon(cairo_t *cr, double x, double y) {
    cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    cairo_set_line_width(cr, 1.3);
    rounded_rect(cr, x + 1, y + 4, 14, 7, 1.5);
    cairo_stroke(cr);
    for (int i = 0; i < 3; i++) {
        cairo_rectangle(cr, x + 3.2 + i * 3.8, y + 6, 2.2, 3);
    }
    cairo_fill(cr);
    for (int i = 0; i < 4; i++) {
        double p = x + 3 + i * 3.2;
        cairo_move_to(cr, p, y + 11); cairo_line_to(cr, p, y + 13.5);
    }
    cairo_stroke(cr);
}

static void draw_disk_icon(cairo_t *cr, double x, double y) {
    cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    cairo_set_line_width(cr, 1.3);
    rounded_rect(cr, x + 2, y + 3, 12, 10, 2.0);
    cairo_stroke(cr);
    cairo_move_to(cr, x + 2, y + 9.5);
    cairo_line_to(cr, x + 14, y + 9.5);
    cairo_stroke(cr);
    cairo_arc(cr, x + 11.5, y + 11.2, 0.9, 0, 2.0 * G_PI);
    cairo_fill(cr);
}

static void draw_net_icon(cairo_t *cr, double x, double y) {
    cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    cairo_set_line_width(cr, 1.3);
    cairo_rectangle(cr, x + 6, y + 2, 4, 3.5);
    cairo_rectangle(cr, x + 1.5, y + 10.5, 4, 3.5);
    cairo_rectangle(cr, x + 10.5, y + 10.5, 4, 3.5);
    cairo_stroke(cr);
    cairo_move_to(cr, x + 8, y + 5.5); cairo_line_to(cr, x + 8, y + 8);
    cairo_move_to(cr, x + 3.5, y + 8); cairo_line_to(cr, x + 12.5, y + 8);
    cairo_move_to(cr, x + 3.5, y + 8); cairo_line_to(cr, x + 3.5, y + 10.5);
    cairo_move_to(cr, x + 12.5, y + 8); cairo_line_to(cr, x + 12.5, y + 10.5);
    cairo_stroke(cr);
}

static void draw_speaker_icon(cairo_t *cr, double x, double y, int muted) {
    if (muted) cairo_set_source_rgb(cr, 0.953, 0.545, 0.659);
    else cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    cairo_move_to(cr, x + 2, y + 6);
    cairo_line_to(cr, x + 5, y + 6);
    cairo_line_to(cr, x + 8.5, y + 3);
    cairo_line_to(cr, x + 8.5, y + 13);
    cairo_line_to(cr, x + 5, y + 10);
    cairo_line_to(cr, x + 2, y + 10);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_set_line_width(cr, 1.3);
    if (muted) {
        cairo_move_to(cr, x + 10.5, y + 5.5); cairo_line_to(cr, x + 14.5, y + 10.5);
        cairo_move_to(cr, x + 14.5, y + 5.5); cairo_line_to(cr, x + 10.5, y + 10.5);
        cairo_stroke(cr);
    } else {
        cairo_arc(cr, x + 9.2, y + 8, 3.0, -0.7, 0.7);
        cairo_stroke(cr);
        cairo_arc(cr, x + 9.2, y + 8, 5.2, -0.7, 0.7);
        cairo_stroke(cr);
    }
}

static void draw_vertical_gauge(cairo_t *cr, double x, double y, double pct) {
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    double w = 6.0, h = 16.0;
    rounded_rect(cr, x, y, w, h, 2.0);
    cairo_set_source_rgb(cr, 0.118, 0.118, 0.180);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.271, 0.278, 0.353);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    double fh = (h - 2.0) * (pct / 100.0);
    if (fh < 2.0 && pct > 0.0) fh = 2.0;
    if (fh > 0.0) {
        if (pct > 85.0) cairo_set_source_rgb(cr, 0.953, 0.545, 0.659);
        else if (pct > 65.0) cairo_set_source_rgb(cr, 0.980, 0.702, 0.529);
        else cairo_set_source_rgb(cr, 0.133, 0.827, 0.933);
        rounded_rect(cr, x + 1.0, y + h - 1.0 - fh, w - 2.0, fh, 1.2);
        cairo_fill(cr);
    }
}

static void draw_sparkline(cairo_t *cr, double x, double y, double w, double h,
                           const double *hi, const double *lo, double max_val) {
    rounded_rect(cr, x, y, w, h, 3.0);
    cairo_set_source_rgb(cr, 0.094, 0.094, 0.145);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.220, 0.231, 0.302);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    if (max_val <= 0.0) max_val = 1.0;

    /* Primary line + fill */
    cairo_save(cr);
    rounded_rect(cr, x + 1, y + 1, w - 2, h - 2, 2.0);
    cairo_clip(cr);

    cairo_move_to(cr, x + 1, y + h - 1);
    for (int i = 0; i < HIST_LEN; i++) {
        double px = x + 1 + ((w - 2) * i) / (double)(HIST_LEN - 1);
        double frac = hi[i] / max_val;
        if (frac > 1.0) frac = 1.0;
        if (frac < 0.0) frac = 0.0;
        double py = y + h - 1 - frac * (h - 3);
        cairo_line_to(cr, px, py);
    }
    cairo_line_to(cr, x + w - 1, y + h - 1);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.133, 0.827, 0.933, 0.28);
    cairo_fill(cr);

    cairo_set_line_width(cr, 1.2);
    cairo_set_source_rgb(cr, 0.220, 0.741, 0.973);
    for (int i = 0; i < HIST_LEN; i++) {
        double px = x + 1 + ((w - 2) * i) / (double)(HIST_LEN - 1);
        double frac = hi[i] / max_val;
        if (frac > 1.0) frac = 1.0;
        if (frac < 0.0) frac = 0.0;
        double py = y + h - 1 - frac * (h - 3);
        if (i == 0) cairo_move_to(cr, px, py);
        else cairo_line_to(cr, px, py);
    }
    cairo_stroke(cr);

    if (lo) {
        cairo_set_source_rgb(cr, 0.953, 0.545, 0.659);
        for (int i = 0; i < HIST_LEN; i++) {
            double px = x + 1 + ((w - 2) * i) / (double)(HIST_LEN - 1);
            double frac = lo[i] / max_val;
            if (frac > 1.0) frac = 1.0;
            if (frac < 0.0) frac = 0.0;
            double py = y + h - 1 - frac * (h - 3);
            if (i == 0) cairo_move_to(cr, px, py);
            else cairo_line_to(cr, px, py);
        }
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void draw_text(cairo_t *cr, double x, double y, const char *txt,
                      double r, double g, double b, int bold, double sz) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, sz);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, txt);
}

static gboolean on_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
    (void)event;
    PanelApp *app = (PanelApp *)data;
    cairo_t *cr = gdk_cairo_create(widget->window);
    int w = widget->allocation.width;
    int h = widget->allocation.height;

    /* Bar background */
    cairo_set_source_rgb(cr, 0.067, 0.067, 0.106);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.192, 0.196, 0.267);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, h - 0.5);
    cairo_line_to(cr, w, h - 0.5);
    cairo_stroke(cr);

    /* LEFT SIDE: Brand + Workspace + Quick App Icons */
    double lx = 10.0;
    draw_text(cr, lx, 18.5, "PhantomOS", 0.537, 0.706, 0.980, 1, 11.5);
    lx += 82.0;

    char ws_txt[64];
    snprintf(ws_txt, sizeof(ws_txt), "WS %ld/%ld", app->cur_ws + 1, app->num_ws > 0 ? app->num_ws : 4);
    app->r_ws = (Rect){ lx, 3, 56, 21 };
    draw_hover_pill(cr, app, app->r_ws);
    rounded_rect(cr, lx, 4, 56, 19, 4.0);
    cairo_set_source_rgb(cr, 0.118, 0.118, 0.180);
    cairo_fill(cr);
    draw_text(cr, lx + 8, 17.5, ws_txt, 0.796, 0.651, 0.969, 1, 10.5);
    lx += 64.0;

    /* Quick Launcher: Terminal */
    app->r_term = (Rect){ lx, 3, 28, 21 };
    draw_hover_pill(cr, app, app->r_term);
    rounded_rect(cr, lx + 4, 6, 20, 15, 3.0);
    cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
    draw_text(cr, lx + 8, 17, ">_", 0.537, 0.706, 0.980, 1, 9.5);
    lx += 32.0;

    /* Quick Launcher: Browser */
    app->r_browser = (Rect){ lx, 3, 28, 21 };
    draw_hover_pill(cr, app, app->r_browser);
    cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    cairo_arc(cr, lx + 14, 13.5, 6.5, 0, 2.0 * G_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, lx + 7.5, 13.5); cairo_line_to(cr, lx + 20.5, 13.5);
    cairo_move_to(cr, lx + 14, 7.0); cairo_line_to(cr, lx + 14, 20.0);
    cairo_stroke(cr);
    lx += 32.0;

    /* Quick Launcher: Files */
    app->r_files = (Rect){ lx, 3, 28, 21 };
    draw_hover_pill(cr, app, app->r_files);
    cairo_set_source_rgb(cr, 0.976, 0.886, 0.686);
    rounded_rect(cr, lx + 6, 8, 16, 11, 2.0);
    cairo_stroke(cr);
    cairo_move_to(cr, lx + 6, 8); cairo_line_to(cr, lx + 9, 6);
    cairo_line_to(cr, lx + 13, 6); cairo_line_to(cr, lx + 14, 8);
    cairo_stroke(cr);

    /* RIGHT SIDE: Astra-Monitor-style Telemetry HUD */
    double rx = w - 10.0;

    /* Power / Clock Pill */
    double clk_w = 104.0;
    rx -= clk_w;
    app->r_power = (Rect){ rx, 3, clk_w, 21 };
    draw_hover_pill(cr, app, app->r_power);
    draw_text(cr, rx + 8, 17.5, app->clock_str, 0.804, 0.839, 0.957, 1, 11.0);
    /* Power glyph (with clear margin from clock digits) */
    double px = rx + clk_w - 13.0;
    cairo_set_source_rgb(cr, 0.953, 0.545, 0.659);
    cairo_set_line_width(cr, 1.4);
    cairo_arc(cr, px, 14.0, 4.5, -1.0, 4.14);
    cairo_stroke(cr);
    cairo_move_to(cr, px, 8.5);
    cairo_line_to(cr, px, 13.5);
    cairo_stroke(cr);

    /* Audio Widget */
    if (app->has_audio) {
        rx -= 58.0;
        app->r_vol = (Rect){ rx, 3, 54, 21 };
        draw_hover_pill(cr, app, app->r_vol);
        draw_speaker_icon(cr, rx + 4, 6, app->vol_muted);
        char vtxt[16];
        snprintf(vtxt, sizeof(vtxt), "%d%%", app->vol_pct);
        draw_text(cr, rx + 22, 17.5, vtxt, 0.729, 0.761, 0.871, 0, 10.0);
    } else {
        app->r_vol = (Rect){ 0, 0, 0, 0 };
    }

    /* Network Widget */
    double net_w = 112.0;
    rx -= (net_w + 6.0);
    app->r_net = (Rect){ rx, 3, net_w, 21 };
    draw_hover_pill(cr, app, app->r_net);
    draw_net_icon(cr, rx + 4, 6);
    double net_peak = 20.0;
    for (int i = 0; i < HIST_LEN; i++) {
        if (app->rx_hist[i] > net_peak) net_peak = app->rx_hist[i];
        if (app->tx_hist[i] > net_peak) net_peak = app->tx_hist[i];
    }
    draw_sparkline(cr, rx + 22, 5, 36, 17, app->rx_hist, app->tx_hist, net_peak);
    char ntxt[24];
    double net_tot = app->rx_kbps + app->tx_kbps;
    if (net_tot >= 1024.0) snprintf(ntxt, sizeof(ntxt), "%.1fM", net_tot / 1024.0);
    else if (net_tot >= 1.0) snprintf(ntxt, sizeof(ntxt), "%.0fK", net_tot);
    else snprintf(ntxt, sizeof(ntxt), "%.0fB", net_tot * 1024.0);
    draw_text(cr, rx + 62, 17.5, ntxt, 0.580, 0.886, 0.835, 0, 10.0);

    /* Disk / Storage Widget */
    double dsk_w = 78.0;
    rx -= (dsk_w + 6.0);
    app->r_disk = (Rect){ rx, 3, dsk_w, 21 };
    draw_hover_pill(cr, app, app->r_disk);
    draw_disk_icon(cr, rx + 4, 6);
    draw_vertical_gauge(cr, rx + 22, 5.5, app->disk_pct);
    char dtxt[24];
    if (app->disk_used_mb >= 1024.0) snprintf(dtxt, sizeof(dtxt), "%.1fG", app->disk_used_mb / 1024.0);
    else snprintf(dtxt, sizeof(dtxt), "%.0fM", app->disk_used_mb);
    draw_text(cr, rx + 32, 17.5, dtxt, 0.729, 0.761, 0.871, 0, 10.5);

    /* RAM Widget */
    double ram_w = 108.0;
    rx -= (ram_w + 6.0);
    app->r_ram = (Rect){ rx, 3, ram_w, 21 };
    draw_hover_pill(cr, app, app->r_ram);
    draw_ram_icon(cr, rx + 4, 6);
    draw_vertical_gauge(cr, rx + 22, 5.5, app->ram_pct);
    char rtxt[32];
    snprintf(rtxt, sizeof(rtxt), "%.0fM/%.1fG", app->ram_used_mb, app->ram_total_mb / 1024.0);
    draw_text(cr, rx + 32, 17.5, rtxt, 0.651, 0.890, 0.631, 1, 10.0);

    /* CPU Widget */
    double cpu_w = 158.0;
    rx -= (cpu_w + 6.0);
    app->r_cpu = (Rect){ rx, 3, cpu_w, 21 };
    draw_hover_pill(cr, app, app->r_cpu);
    draw_cpu_icon(cr, rx + 4, 6);
    draw_sparkline(cr, rx + 22, 5, 42, 17, app->cpu_total_hist, app->cpu_sys_hist, 100.0);
    char ctxt[32];
    if (app->cpu_ghz > 0.05)
        snprintf(ctxt, sizeof(ctxt), "%.1f%% %.2fGHz", app->cpu_pct, app->cpu_ghz);
    else
        snprintf(ctxt, sizeof(ctxt), "%.1f%% CPU", app->cpu_pct);
    draw_text(cr, rx + 68, 17.5, ctxt, 0.804, 0.839, 0.957, 1, 10.0);

    cairo_destroy(cr);
    return TRUE;
}

static void show_power_menu(PanelApp *app, GdkEventButton *ev) {
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *m_mon = gtk_menu_item_new_with_label("System Monitor");
    GtkWidget *m_term = gtk_menu_item_new_with_label("Open Terminal");
    GtkWidget *m_sep = gtk_separator_menu_item_new();
    GtkWidget *m_reb = gtk_menu_item_new_with_label("Reboot");
    GtkWidget *m_pwr = gtk_menu_item_new_with_label("Power Off");

    g_signal_connect_swapped(m_mon, "activate", G_CALLBACK(spawn_cmd), (gpointer)"/usr/bin/phantom-monitor");
    g_signal_connect_swapped(m_term, "activate", G_CALLBACK(spawn_cmd),
        (gpointer)"xterm -u8 -ls -bg \"#181825\" -fg \"#cdd6f4\" -cr \"#89b4fa\" -bd \"#313244\"");
    g_signal_connect_swapped(m_reb, "activate", G_CALLBACK(spawn_cmd), (gpointer)"sudo /sbin/reboot");
    g_signal_connect_swapped(m_pwr, "activate", G_CALLBACK(spawn_cmd), (gpointer)"sudo /sbin/poweroff");

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), m_mon);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), m_term);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), m_sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), m_reb);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), m_pwr);
    gtk_widget_show_all(menu);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, ev->button, ev->time);
    (void)app;
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    PanelApp *app = (PanelApp *)data;
    if (ev->button != 1) return FALSE;

    if (pt_in_rect(ev->x, ev->y, app->r_ws)) {
        switch_workspace_delta(1);
    } else if (pt_in_rect(ev->x, ev->y, app->r_term)) {
        spawn_cmd("xterm -u8 -ls -bg \"#181825\" -fg \"#cdd6f4\" -cr \"#89b4fa\" -bd \"#313244\"");
    } else if (pt_in_rect(ev->x, ev->y, app->r_browser)) {
        spawn_cmd("/usr/bin/firefox");
    } else if (pt_in_rect(ev->x, ev->y, app->r_files)) {
        spawn_cmd("/usr/bin/pcmanfm");
    } else if (pt_in_rect(ev->x, ev->y, app->r_cpu) ||
               pt_in_rect(ev->x, ev->y, app->r_ram) ||
               pt_in_rect(ev->x, ev->y, app->r_disk) ||
               pt_in_rect(ev->x, ev->y, app->r_net)) {
        spawn_cmd("/usr/bin/phantom-monitor");
    } else if (app->has_audio && pt_in_rect(ev->x, ev->y, app->r_vol)) {
        spawn_cmd("amixer -q sset Master toggle");
        sample_audio(app);
        gtk_widget_queue_draw(app->area);
    } else if (pt_in_rect(ev->x, ev->y, app->r_power)) {
        show_power_menu(app, ev);
    }
    return TRUE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data) {
    (void)w;
    PanelApp *app = (PanelApp *)data;
    if (pt_in_rect(ev->x, ev->y, app->r_ws)) {
        switch_workspace_delta((ev->direction == GDK_SCROLL_UP) ? -1 : 1);
    } else if (app->has_audio && pt_in_rect(ev->x, ev->y, app->r_vol)) {
        spawn_cmd((ev->direction == GDK_SCROLL_UP)
                  ? "amixer -q sset Master 5%+"
                  : "amixer -q sset Master 5%-");
        sample_audio(app);
        gtk_widget_queue_draw(app->area);
    }
    return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    (void)w;
    PanelApp *app = (PanelApp *)data;
    app->hover_x = ev->x;
    app->hover_y = ev->y;
    app->hovering = 1;
    gtk_widget_queue_draw(app->area);
    return FALSE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer data) {
    (void)w;
    (void)ev;
    PanelApp *app = (PanelApp *)data;
    app->hovering = 0;
    gtk_widget_queue_draw(app->area);
    return FALSE;
}

static gboolean on_tick(gpointer data) {
    PanelApp *app = (PanelApp *)data;
    sample_all(app);
    gtk_widget_queue_draw(app->area);
    return TRUE;
}

static void apply_dock_struts(PanelApp *app) {
    if (!app->window || !app->window->window) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    Window xwin = GDK_WINDOW_XID(app->window->window);

    Atom a_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom a_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(dpy, xwin, a_type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&a_dock, 1);

    long strut[4] = { 0, 0, PANEL_HEIGHT, 0 };
    Atom a_strut = XInternAtom(dpy, "_NET_WM_STRUT", False);
    XChangeProperty(dpy, xwin, a_strut, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)strut, 4);

    long strut_p[12] = { 0, 0, PANEL_HEIGHT, 0, 0, 0, 0, 0, 0, app->width - 1, 0, 0 };
    Atom a_strut_p = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    XChangeProperty(dpy, xwin, a_strut_p, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)strut_p, 12);
}

static void on_realize(GtkWidget *widget, gpointer data) {
    (void)widget;
    PanelApp *app = (PanelApp *)data;
    apply_dock_struts(app);
    gtk_window_stick(GTK_WINDOW(app->window));
}

static void on_screen_resized(GdkScreen *scr, gpointer data) {
    PanelApp *app = (PanelApp *)data;
    int nw = gdk_screen_get_width(scr);
    if (nw >= 800 && nw != app->width) {
        app->width = nw;
        gtk_widget_set_size_request(app->window, app->width, PANEL_HEIGHT);
        gtk_window_resize(GTK_WINDOW(app->window), app->width, PANEL_HEIGHT);
        gtk_window_move(GTK_WINDOW(app->window), 0, 0);
        apply_dock_struts(app);
        gtk_widget_queue_draw(app->area);
    }
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    PanelApp app;
    memset(&app, 0, sizeof(app));
    GdkScreen *scr = gdk_screen_get_default();
    app.width = gdk_screen_get_width(scr);
    if (app.width < 800) app.width = 1280;
    app.height = PANEL_HEIGHT;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "phantom-panel");
    gtk_window_set_type_hint(GTK_WINDOW(app.window), GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_window_set_decorated(GTK_WINDOW(app.window), FALSE);
    gtk_window_set_accept_focus(GTK_WINDOW(app.window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(app.window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(app.window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(app.window), app.width, PANEL_HEIGHT);
    gtk_widget_set_size_request(app.window, app.width, PANEL_HEIGHT);
    gtk_window_move(GTK_WINDOW(app.window), 0, 0);
    g_signal_connect(app.window, "realize", G_CALLBACK(on_realize), &app);
    g_signal_connect(scr, "size-changed", G_CALLBACK(on_screen_resized), &app);

    app.area = gtk_drawing_area_new();
    gtk_widget_add_events(app.area,
        GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK |
        GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK);
    g_signal_connect(app.area, "expose-event", G_CALLBACK(on_expose), &app);
    g_signal_connect(app.area, "button-press-event", G_CALLBACK(on_button_press), &app);
    g_signal_connect(app.area, "scroll-event", G_CALLBACK(on_scroll), &app);
    g_signal_connect(app.area, "motion-notify-event", G_CALLBACK(on_motion), &app);
    g_signal_connect(app.area, "leave-notify-event", G_CALLBACK(on_leave), &app);
    gtk_container_add(GTK_CONTAINER(app.window), app.area);

    sample_all(&app);
    g_timeout_add(1000, on_tick, &app);

    gtk_widget_show_all(app.window);
    gtk_main();
    return 0;
}
