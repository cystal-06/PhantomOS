#define _GNU_SOURCE
#include <cairo.h>
#include <ctype.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PID_FILE   "/tmp/.phantom-overview.pid"
#define MAX_APPS   16
#define MAX_WS     8
#define MAX_WINS   32
#define ICON_DIR48 "/usr/share/icons/PhantomGNOME/48x48"
#define ICON_DIR64 "/usr/share/icons/PhantomGNOME/64x64"

typedef struct {
    double x, y, w, h;
} Rect;

typedef struct {
    const char *name;
    const char *sub;
    const char *icon_cat;
    const char *icon_name;
    const char *cmd;
    int pinned;
    GdkPixbuf *pb64;
    GdkPixbuf *pb48;
} AppEntry;

typedef struct {
    Window xid;
    long desktop;
    int x, y, w, h;
    char title[64];
} WinCard;

typedef struct {
    GtkWidget *window;
    cairo_surface_t *blur_surf;
    int blur_w;
    int blur_h;

    int scr_w;
    int scr_h;

    /* Animations (60fps) */
    double anim_t;        /* 0.0 -> 1.0 open/close */
    int closing;          /* 1 = playing close animation before quit */
    double page_pos;      /* animated grid page offset */
    int target_page;
    double ws_sel_pos;    /* animated active workspace highlight */
    double hover_scale[MAX_APPS];
    double dock_scale[MAX_APPS];
    int hover_app_idx;
    int hover_dock_idx;
    int hover_ws_idx;
    int hover_dots;

    double hx, hy;
    int hovering;

    /* Data */
    AppEntry apps[MAX_APPS];
    int app_count;
    int filtered[MAX_APPS];
    int filtered_count;
    char search[64];

    long num_ws;
    long cur_ws;
    WinCard wins[MAX_WINS];
    int win_count;

    /* Interactive hitboxes */
    Rect r_search;
    Rect r_ws[MAX_WS];
    Rect r_app[MAX_APPS];
    int r_app_idx[MAX_APPS];
    int r_app_n;
    Rect r_dock[MAX_APPS];
    int r_dock_idx[MAX_APPS];
    int r_dock_n;
    Rect r_dots;
    Rect r_prev_pg;
    Rect r_next_pg;

    char pending_cmd[256];
} OverviewApp;

static volatile sig_atomic_t g_sig_close = 0;

static void on_sig_usr1(int sig) {
    (void)sig;
    g_sig_close = 1;
}

static int pt_in_rect(double px, double py, Rect r) {
    return r.w > 0 && px >= r.x && px <= (r.x + r.w) && py >= r.y && py <= (r.y + r.h);
}

static double ease_out_cubic(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    double inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
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

static void draw_text_left(cairo_t *cr, double x, double y, const char *txt,
                           double r, double g, double b, double a, int bold, double sz) {
    cairo_select_font_face(cr, "Adwaita Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, sz);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, txt);
    cairo_new_path(cr);
}

static void draw_text_center(cairo_t *cr, double cx, double y, const char *txt,
                             double r, double g, double b, double a, int bold, double sz) {
    cairo_select_font_face(cr, "Adwaita Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, sz);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, txt, &ext);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, cx - ext.width * 0.5 - ext.x_bearing, y);
    cairo_show_text(cr, txt);
    cairo_new_path(cr);
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

static void probe_x11_state(OverviewApp *app) {
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    app->num_ws = 4;
    app->cur_ws = 0;
    app->win_count = 0;
    if (!dpy) return;

    Window root = DefaultRootWindow(dpy);
    Atom a_num = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    Atom a_cur = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    Atom a_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    Atom a_desk = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    Atom a_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom a_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    Atom a_dt = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    Atom a_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);

    app->num_ws = x11_get_card(dpy, root, a_num, 4);
    if (app->num_ws < 1) app->num_ws = 1;
    if (app->num_ws > MAX_WS) app->num_ws = MAX_WS;
    app->cur_ws = x11_get_card(dpy, root, a_cur, 0);
    if (app->cur_ws < 0) app->cur_ws = 0;
    if (app->cur_ws >= app->num_ws) app->cur_ws = 0;

    Atom at = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, a_list, 0, 64, False, XA_WINDOW,
                           &at, &fmt, &n, &after, &data) == Success && data) {
        Window *wins = (Window *)data;
        for (unsigned long i = 0; i < n && app->win_count < MAX_WINS; i++) {
            Atom wt = (Atom)x11_get_card(dpy, wins[i], a_type, None);
            if (wt == a_dock || wt == a_dt) continue;

            XWindowAttributes attr;
            if (!XGetWindowAttributes(dpy, wins[i], &attr)) continue;
            if (attr.map_state != IsViewable && attr.width <= 1) continue;

            long d = x11_get_card(dpy, wins[i], a_desk, app->cur_ws);
            WinCard *wc = &app->wins[app->win_count];
            wc->xid = wins[i];
            wc->desktop = d;
            wc->x = attr.x;
            wc->y = attr.y;
            wc->w = attr.width;
            wc->h = attr.height;
            wc->title[0] = '\0';

            Atom nt = None;
            int nfmt = 0;
            unsigned long nn = 0, nafter = 0;
            unsigned char *ndata = NULL;
            if (XGetWindowProperty(dpy, wins[i], a_name, 0, 64, False, a_utf8,
                                   &nt, &nfmt, &nn, &nafter, &ndata) == Success && ndata) {
                g_strlcpy(wc->title, (const char *)ndata, sizeof(wc->title));
                XFree(ndata);
            }
            if (!wc->title[0]) {
                char *wmname = NULL;
                if (XFetchName(dpy, wins[i], &wmname) && wmname) {
                    g_strlcpy(wc->title, wmname, sizeof(wc->title));
                    XFree(wmname);
                }
            }
            if (strcmp(wc->title, "phantom-panel") == 0 ||
                strcmp(wc->title, "phantom-panel-qs") == 0 ||
                strcmp(wc->title, "phantom-overview") == 0 ||
                (strcmp(wc->title, "pcmanfm") == 0 && wc->w == app->scr_w)) {
                continue;
            }
            app->win_count++;
        }
        XFree(data);
    }
}

static void switch_workspace(OverviewApp *app, long ws) {
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (!dpy || ws < 0 || ws >= app->num_ws) return;
    Window root = DefaultRootWindow(dpy);
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = root;
    ev.xclient.message_type = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = ws;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
    app->cur_ws = ws;
}

static void capture_blurred_desktop(OverviewApp *app) {
    GdkWindow *root = gdk_get_default_root_window();
    if (!root) return;
    int rw = 0, rh = 0;
    gdk_drawable_get_size(GDK_DRAWABLE(root), &rw, &rh);
    if (rw < 100 || rh < 100) {
        rw = app->scr_w;
        rh = app->scr_h;
    }

    GdkPixbuf *shot = gdk_pixbuf_get_from_drawable(NULL, GDK_DRAWABLE(root), NULL,
                                                   0, 0, 0, 0, rw, rh);
    if (!shot) return;

    /* Fast multi-pass downsample for soft GNOME-Shell-style Gaussian blur */
    int sw1 = rw / 6;
    int sh1 = rh / 6;
    int sw2 = rw / 12;
    int sh2 = rh / 12;
    if (sw2 < 64) sw2 = 64;
    if (sh2 < 40) sh2 = 40;

    GdkPixbuf *p1 = gdk_pixbuf_scale_simple(shot, sw1, sh1, GDK_INTERP_BILINEAR);
    g_object_unref(shot);
    if (!p1) return;

    GdkPixbuf *p2 = gdk_pixbuf_scale_simple(p1, sw2, sh2, GDK_INTERP_BILINEAR);
    g_object_unref(p1);
    if (!p2) return;

    app->blur_w = sw2;
    app->blur_h = sh2;
    app->blur_surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, sw2, sh2);
    cairo_t *bcr = cairo_create(app->blur_surf);
    gdk_cairo_set_source_pixbuf(bcr, p2, 0, 0);
    cairo_paint(bcr);
    cairo_destroy(bcr);
    g_object_unref(p2);
}

static void rebuild_filter(OverviewApp *app) {
    app->filtered_count = 0;
    char q[64];
    size_t qi = 0;
    for (; app->search[qi] && qi < sizeof(q) - 1; qi++) {
        q[qi] = (char)tolower((unsigned char)app->search[qi]);
    }
    q[qi] = '\0';

    for (int i = 0; i < app->app_count; i++) {
        if (!q[0]) {
            app->filtered[app->filtered_count++] = i;
            continue;
        }
        char n1[64], n2[64];
        size_t j;
        for (j = 0; app->apps[i].name[j] && j < sizeof(n1) - 1; j++) {
            n1[j] = (char)tolower((unsigned char)app->apps[i].name[j]);
        }
        n1[j] = '\0';
        for (j = 0; app->apps[i].sub[j] && j < sizeof(n2) - 1; j++) {
            n2[j] = (char)tolower((unsigned char)app->apps[i].sub[j]);
        }
        n2[j] = '\0';
        if (strstr(n1, q) || strstr(n2, q)) {
            app->filtered[app->filtered_count++] = i;
        }
    }
    app->target_page = 0;
}

static void init_apps(OverviewApp *app) {
    struct {
        const char *name;
        const char *sub;
        const char *cat;
        const char *icon;
        const char *cmd;
        int pinned;
    } defs[] = {
        { "Files",          "File Manager",       "apps", "system-file-manager",     "/usr/bin/pcmanfm", 1 },
        { "Web Browser",    "Firefox ESR",        "apps", "web-browser",             "/usr/bin/firefox", 1 },
        { "Terminal",       "XTerm UTF-8 Shell",  "apps", "utilities-terminal",      "xterm -u8 -ls -bg \"#181825\" -fg \"#cdd6f4\" -cr \"#89b4fa\" -bd \"#313244\"", 1 },
        { "Editor",         "Leafpad Notes",      "apps", "accessories-text-editor", "/usr/bin/leafpad", 1 },
        { "System Monitor", "CPU, RAM & ZRAM",    "apps", "applications-system",     "/usr/bin/phantom-monitor", 1 },
        { "Settings",       "System & Wi-Fi",     "apps", "preferences-system",      "/usr/bin/phantom-settings", 1 },
        { "Backend Lab",    "Server & Dev TUI",   "apps", "system-software-install", "xterm -u8 -bg \"#181825\" -fg \"#cdd6f4\" -title \"Backend Lab\" -e /opt/tui-menu/menu.sh", 1 },
        { "Display & Wi-Fi","Quick Config",       "apps", "preferences-system",      "/usr/bin/phantom-settings", 0 },
        { "Log Out",        "End X11 Session",    "places", "user-desktop",          "sh -c 'killall -TERM fluxbox 2>/dev/null; sleep 1; killall -KILL fluxbox 2>/dev/null'", 0 },
        { "Power Off",      "Shutdown Machine",   "places", "user-trash",            "sudo -n /sbin/poweroff", 0 },
    };

    int count = (int)(sizeof(defs) / sizeof(defs[0]));
    app->app_count = 0;
    for (int i = 0; i < count && app->app_count < MAX_APPS; i++) {
        AppEntry *e = &app->apps[app->app_count];
        e->name = defs[i].name;
        e->sub = defs[i].sub;
        e->icon_cat = defs[i].cat;
        e->icon_name = defs[i].icon;
        e->cmd = defs[i].cmd;
        e->pinned = defs[i].pinned;

        char p64[256], p48[256];
        snprintf(p64, sizeof(p64), "%s/%s/%s.png", ICON_DIR64, e->icon_cat, e->icon_name);
        snprintf(p48, sizeof(p48), "%s/%s/%s.png", ICON_DIR48, e->icon_cat, e->icon_name);
        e->pb64 = gdk_pixbuf_new_from_file(p64, NULL);
        e->pb48 = gdk_pixbuf_new_from_file(p48, NULL);
        app->hover_scale[app->app_count] = 1.0;
        app->dock_scale[app->app_count] = 1.0;
        app->app_count++;
    }
    rebuild_filter(app);
}

static int app_is_running(OverviewApp *app, const AppEntry *e) {
    const char *key = NULL;
    if (strcmp(e->name, "Files") == 0) key = "pcmanfm";
    else if (strcmp(e->name, "Web Browser") == 0) key = "Firefox";
    else if (strcmp(e->name, "Terminal") == 0) key = "Terminal";
    else if (strcmp(e->name, "Editor") == 0) key = "Leafpad";
    else if (strcmp(e->name, "System Monitor") == 0) key = "Monitor";
    else if (strcmp(e->name, "Settings") == 0) key = "Settings";
    else if (strcmp(e->name, "Backend Lab") == 0) key = "Backend Lab";
    if (!key) return 0;

    for (int i = 0; i < app->win_count; i++) {
        if (strcasestr(app->wins[i].title, key)) return 1;
    }
    return 0;
}

static void start_close(OverviewApp *app, const char *cmd) {
    if (app->closing) return;
    app->closing = 1;
    if (cmd) g_strlcpy(app->pending_cmd, cmd, sizeof(app->pending_cmd));
    else app->pending_cmd[0] = '\0';
}

static void draw_pixbuf_scaled(cairo_t *cr, GdkPixbuf *pb, double cx, double cy, double scale, double alpha) {
    if (!pb) return;
    int pw = gdk_pixbuf_get_width(pb);
    int ph = gdk_pixbuf_get_height(pb);
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, scale, scale);
    gdk_cairo_set_source_pixbuf(cr, pb, -pw * 0.5, -ph * 0.5);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
    cairo_new_path(cr);
}

static gboolean on_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
    (void)event;
    OverviewApp *app = (OverviewApp *)data;
    cairo_t *cr = gdk_cairo_create(widget->window);
    int w = widget->allocation.width;
    int h = widget->allocation.height;
    app->scr_w = w;
    app->scr_h = h;

    double e = ease_out_cubic(app->anim_t);

    /* 1. Blurred Live Desktop Background + Smooth Dimming */
    if (app->blur_surf && app->blur_w > 0 && app->blur_h > 0) {
        cairo_save(cr);
        cairo_scale(cr, (double)w / app->blur_w, (double)h / app->blur_h);
        cairo_set_source_surface(cr, app->blur_surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        cairo_set_source_rgb(cr, 0.082, 0.094, 0.145);
        cairo_paint(cr);
    }

    /* Atmospheric GNOME dark slate tint interpolating with anim_t */
    cairo_pattern_t *dim = cairo_pattern_create_linear(0, 0, w, h);
    cairo_pattern_add_color_stop_rgba(dim, 0.0, 0.082, 0.098, 0.165, 0.72 * e);
    cairo_pattern_add_color_stop_rgba(dim, 0.5, 0.102, 0.125, 0.212, 0.68 * e);
    cairo_pattern_add_color_stop_rgba(dim, 1.0, 0.075, 0.090, 0.153, 0.76 * e);
    cairo_set_source(cr, dim);
    cairo_paint(cr);
    cairo_pattern_destroy(dim);

    /* 2. Search Pill ("Type to search") */
    double search_y = 42.0 - (1.0 - e) * 26.0;
    double sw = 320.0;
    double sh = 38.0;
    double sx = (w - sw) * 0.5;
    app->r_search = (Rect){ sx, search_y, sw, sh };

    rounded_rect(cr, sx, search_y, sw, sh, 19.0);
    cairo_set_source_rgba(cr, 0.220, 0.251, 0.365, 0.55 * e);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.537, 0.706, 0.980, (app->search[0] ? 0.65 : 0.22) * e);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);

    /* Magnifying glass glyph */
    cairo_set_source_rgba(cr, 0.80, 0.84, 0.96, 0.85 * e);
    cairo_set_line_width(cr, 1.7);
    cairo_new_sub_path(cr);
    cairo_arc(cr, sx + 22, search_y + 18, 5.2, 0, 2 * M_PI);
    cairo_move_to(cr, sx + 26, search_y + 22);
    cairo_line_to(cr, sx + 30, search_y + 26);
    cairo_stroke(cr);

    if (app->search[0]) {
        draw_text_left(cr, sx + 40, search_y + 24, app->search, 0.95, 0.97, 1.0, e, 1, 12.0);
    } else {
        draw_text_left(cr, sx + 40, search_y + 24, "Type to search", 0.76, 0.80, 0.92, 0.78 * e, 0, 11.5);
    }

    /* 3. Workspace Thumbnails Strip */
    int nws = (int)app->num_ws;
    if (nws > 4) nws = 4;
    double ws_w = 186.0;
    double ws_h = 114.0;
    double ws_gap = 18.0;
    double tot_ws_w = nws * ws_w + (nws - 1) * ws_gap;
    double ws_x0 = (w - tot_ws_w) * 0.5;
    double ws_y = 98.0 + (1.0 - e) * -20.0;
    double ws_scale = 0.86 + 0.14 * e;

    for (int i = 0; i < nws; i++) {
        double cx = ws_x0 + i * (ws_w + ws_gap) + ws_w * 0.5;
        double cy = ws_y + ws_h * 0.5;
        double rx = cx - (ws_w * ws_scale) * 0.5;
        double ry = cy - (ws_h * ws_scale) * 0.5;
        double rw = ws_w * ws_scale;
        double rh = ws_h * ws_scale;
        app->r_ws[i] = (Rect){ rx, ry, rw, rh };

        double dist = fabs(app->ws_sel_pos - i);
        double active_amt = dist < 1.0 ? (1.0 - dist) : 0.0;
        int hov = app->hovering && app->hover_ws_idx == i;

        rounded_rect(cr, rx, ry, rw, rh, 12.0);
        cairo_set_source_rgba(cr, 0.118, 0.137, 0.212, (0.78 + 0.14 * active_amt) * e);
        cairo_fill_preserve(cr);
        if (active_amt > 0.05) {
            cairo_set_source_rgba(cr, 0.537, 0.706, 0.980, (0.35 + 0.65 * active_amt) * e);
            cairo_set_line_width(cr, 2.2);
        } else {
            cairo_set_source_rgba(cr, 0.80, 0.84, 0.96, (hov ? 0.42 : 0.16) * e);
            cairo_set_line_width(cr, 1.2);
        }
        cairo_stroke(cr);

        /* Mini window thumbnails inside this workspace */
        int any_win = 0;
        for (int wi = 0; wi < app->win_count; wi++) {
            WinCard *wc = &app->wins[wi];
            if (wc->desktop != i && wc->desktop != -1) continue;
            any_win = 1;
            double wx = rx + 6.0 + (double)wc->x / w * (rw - 12.0);
            double wy = ry + 6.0 + (double)wc->y / h * (rh - 12.0);
            double ww = (double)wc->w / w * (rw - 12.0);
            double wh = (double)wc->h / h * (rh - 12.0);
            if (ww < 18.0) ww = 18.0;
            if (wh < 14.0) wh = 14.0;
            if (wx + ww > rx + rw - 4.0) ww = (rx + rw - 4.0) - wx;
            if (wy + wh > ry + rh - 4.0) wh = (ry + rh - 4.0) - wy;
            if (ww > 6.0 && wh > 6.0) {
                rounded_rect(cr, wx, wy, ww, wh, 4.0);
                cairo_set_source_rgba(cr, 0.20, 0.24, 0.36, 0.92 * e);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, 0.54, 0.71, 0.98, 0.55 * e);
                cairo_set_line_width(cr, 0.9);
                cairo_stroke(cr);
                rounded_rect(cr, wx, wy, ww, 4.5, 2.0);
                cairo_set_source_rgba(cr, 0.32, 0.37, 0.54, 0.9 * e);
                cairo_fill(cr);
            }
        }
        if (!any_win) {
            char wl[16];
            snprintf(wl, sizeof(wl), "WS %d", i + 1);
            draw_text_center(cr, cx, cy + 4, wl, 0.65, 0.70, 0.85, 0.55 * e, 1, 10.5);
        } else {
            char wl[16];
            snprintf(wl, sizeof(wl), "%d", i + 1);
            rounded_rect(cr, rx + 7, ry + rh - 20, 18, 14, 5.0);
            cairo_set_source_rgba(cr, 0.08, 0.09, 0.14, 0.78 * e);
            cairo_fill(cr);
            draw_text_center(cr, rx + 16, ry + rh - 9.5, wl, 0.85, 0.90, 1.0, 0.9 * e, 1, 8.8);
        }
    }

    /* 4. GNOME Paginated App Grid */
    const int cols = 5;
    const int rows = 2;
    const int per_page = cols * rows;
    int total_pages = (app->filtered_count + per_page - 1) / per_page;
    if (total_pages < 1) total_pages = 1;
    if (app->target_page >= total_pages) app->target_page = total_pages - 1;

    double cell_w = 136.0;
    double cell_h = 122.0;
    double grid_w = cols * cell_w;
    double grid_x0 = (w - grid_w) * 0.5;
    double grid_y0 = 246.0;

    app->r_app_n = 0;
    int p = app->target_page;
    int start_idx = p * per_page;
    int end_idx = start_idx + per_page;
    if (end_idx > app->filtered_count) end_idx = app->filtered_count;

    if (app->filtered_count == 0) {
        draw_text_center(cr, w * 0.5, grid_y0 + 100, "No matching applications",
                         0.75, 0.78, 0.90, 0.8 * e, 0, 13.0);
    } else {
        for (int k = start_idx; k < end_idx; k++) {
            int idx = app->filtered[k];
            int local = k - start_idx;
            int col = local % cols;
            int row = local / cols;

            /* Subtle staggered entrance for each icon */
            double stagger = local * 0.025;
            double ie = ease_out_cubic((app->anim_t - stagger) / (1.0 - stagger));
            double slide_off = (1.0 - ie) * 24.0;

            double cx = grid_x0 + col * cell_w + cell_w * 0.5;
            double cy = grid_y0 + row * cell_h + cell_h * 0.5 + slide_off;
            Rect rc = { cx - 54, cy - 52, 108, 106 };
            app->r_app[app->r_app_n] = rc;
            app->r_app_idx[app->r_app_n] = idx;
            app->r_app_n++;

            int hov = app->hovering && app->hover_app_idx == idx;
            if (hov) {
                rounded_rect(cr, rc.x, rc.y, rc.w, rc.h, 16.0);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.13 * ie);
                cairo_fill(cr);
            }

            double icon_sc = (0.78 + 0.22 * ie) * app->hover_scale[idx];
            draw_pixbuf_scaled(cr, app->apps[idx].pb64, cx, cy - 12.0, icon_sc, ie);
            draw_text_center(cr, cx, cy + 38.0, app->apps[idx].name,
                             0.94, 0.96, 1.0, ie, hov ? 1 : 0, 10.6);
        }
    }

    /* Pagination Dots & Arrows (when >1 page or decorative GNOME dots) */
    double dots_y = grid_y0 + rows * cell_h + 16.0;
    double dots_w = total_pages * 18.0;
    app->r_dots = (Rect){ (w - dots_w) * 0.5 - 10, dots_y - 10, dots_w + 20, 20 };
    for (int pi = 0; pi < total_pages; pi++) {
        double dx = (w - dots_w) * 0.5 + pi * 18.0 + 9.0;
        cairo_new_sub_path(cr);
        cairo_arc(cr, dx, dots_y, pi == app->target_page ? 4.2 : 3.2, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (pi == app->target_page ? 0.92 : 0.32) * e);
        cairo_fill(cr);
    }
    app->r_prev_pg = (Rect){0,0,0,0};
    app->r_next_pg = (Rect){0,0,0,0};
    if (total_pages > 1) {
        if (app->target_page > 0) {
            app->r_prev_pg = (Rect){ grid_x0 - 52, grid_y0 + cell_h - 20, 38, 38 };
            rounded_rect(cr, app->r_prev_pg.x, app->r_prev_pg.y, 38, 38, 19.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14 * e);
            cairo_fill(cr);
            draw_text_center(cr, app->r_prev_pg.x + 19, app->r_prev_pg.y + 24, "<", 0.95, 0.97, 1.0, e, 1, 14.0);
        }
        if (app->target_page + 1 < total_pages) {
            app->r_next_pg = (Rect){ grid_x0 + grid_w + 14, grid_y0 + cell_h - 20, 38, 38 };
            rounded_rect(cr, app->r_next_pg.x, app->r_next_pg.y, 38, 38, 19.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14 * e);
            cairo_fill(cr);
            draw_text_center(cr, app->r_next_pg.x + 19, app->r_next_pg.y + 24, ">", 0.95, 0.97, 1.0, e, 1, 14.0);
        }
    }

    /* 5. Floating GNOME Dock at Bottom */
    int pinned_indices[MAX_APPS];
    int pn = 0;
    for (int i = 0; i < app->app_count; i++) {
        if (app->apps[i].pinned) pinned_indices[pn++] = i;
    }
    double item_sp = 58.0;
    double dock_pad = 16.0;
    double dock_w = dock_pad * 2 + pn * item_sp + 46.0; /* + show-apps 9-dots button */
    double dock_h = 68.0;
    double dock_x = (w - dock_w) * 0.5;
    double dock_y = h - dock_h - 20.0 + (1.0 - e) * 44.0;

    rounded_rect(cr, dock_x, dock_y, dock_w, dock_h, 22.0);
    cairo_set_source_rgba(cr, 0.118, 0.129, 0.184, 0.92 * e);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.80, 0.84, 0.96, 0.18 * e);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    app->r_dock_n = 0;
    double ix = dock_x + dock_pad + item_sp * 0.5;
    for (int i = 0; i < pn; i++) {
        int idx = pinned_indices[i];
        Rect rd = { ix - 26, dock_y + 4, 52, 60 };
        app->r_dock[app->r_dock_n] = rd;
        app->r_dock_idx[app->r_dock_n] = idx;
        app->r_dock_n++;

        int hov = app->hovering && app->hover_dock_idx == idx;
        if (hov) {
            rounded_rect(cr, rd.x + 2, dock_y + 7, rd.w - 4, 50, 14.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12 * e);
            cairo_fill(cr);
        }
        double sc = app->dock_scale[idx];
        double lift = (sc - 1.0) * 26.0;
        draw_pixbuf_scaled(cr, app->apps[idx].pb48, ix, dock_y + 31.0 - lift, sc, e);

        if (app_is_running(app, &app->apps[idx])) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, ix, dock_y + 60.5, 2.5, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 0.537, 0.706, 0.980, e);
            cairo_fill(cr);
        }
        ix += item_sp;
    }

    /* Divider + 9-dots Show Apps icon */
    double div_x = dock_x + dock_w - 50.0;
    cairo_set_source_rgba(cr, 0.80, 0.84, 0.96, 0.18 * e);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, div_x, dock_y + 16);
    cairo_line_to(cr, div_x, dock_y + dock_h - 16);
    cairo_stroke(cr);

    double dcx = dock_x + dock_w - 27.0;
    double dcy = dock_y + dock_h * 0.5;
    app->r_dots = (Rect){ dcx - 18, dock_y + 10, 36, 48 };
    if (app->hovering && app->hover_dots) {
        rounded_rect(cr, dcx - 18, dock_y + 12, 36, 44, 12.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14 * e);
        cairo_fill(cr);
    }
    cairo_set_source_rgba(cr, 0.92, 0.95, 1.0, 0.92 * e);
    for (int gy = -1; gy <= 1; gy++) {
        for (int gx = -1; gx <= 1; gx++) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, dcx + gx * 7.0, dcy + gy * 7.0, 2.1, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }

    /* Tooltip label over hovered Dock icon */
    if (app->hovering && app->hover_dock_idx >= 0) {
        for (int i = 0; i < app->r_dock_n; i++) {
            if (app->r_dock_idx[i] == app->hover_dock_idx) {
                double tx = app->r_dock[i].x + app->r_dock[i].w * 0.5;
                double ty = dock_y - 10.0;
                const char *lbl = app->apps[app->hover_dock_idx].name;
                rounded_rect(cr, tx - 56, ty - 24, 112, 22, 8.0);
                cairo_set_source_rgba(cr, 0.09, 0.10, 0.15, 0.94 * e);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, 0.35, 0.40, 0.58, 0.5 * e);
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);
                draw_text_center(cr, tx, ty - 9, lbl, 0.95, 0.97, 1.0, e, 1, 9.8);
                break;
            }
        }
    }

    cairo_destroy(cr);
    return FALSE;
}

static gboolean on_anim_tick(gpointer data) {
    OverviewApp *app = (OverviewApp *)data;
    int dirty = 0;

    if (g_sig_close) {
        g_sig_close = 0;
        start_close(app, NULL);
    }

    if (!app->closing && app->anim_t < 1.0) {
        app->anim_t += 0.085; /* ~190ms open @ 60fps */
        if (app->anim_t > 1.0) app->anim_t = 1.0;
        dirty = 1;
    } else if (app->closing) {
        app->anim_t -= 0.115; /* ~140ms close @ 60fps */
        if (app->anim_t <= 0.0) {
            app->anim_t = 0.0;
            if (app->pending_cmd[0]) spawn_cmd(app->pending_cmd);
            gtk_main_quit();
            return FALSE;
        }
        dirty = 1;
    }

    if (fabs(app->ws_sel_pos - (double)app->cur_ws) > 0.005) {
        app->ws_sel_pos += ((double)app->cur_ws - app->ws_sel_pos) * 0.28;
        dirty = 1;
    } else if (app->ws_sel_pos != (double)app->cur_ws) {
        app->ws_sel_pos = (double)app->cur_ws;
        dirty = 1;
    }

    for (int i = 0; i < app->app_count; i++) {
        double target_g = (app->hovering && app->hover_app_idx == i) ? 1.11 : 1.0;
        if (fabs(app->hover_scale[i] - target_g) > 0.004) {
            app->hover_scale[i] += (target_g - app->hover_scale[i]) * 0.30;
            dirty = 1;
        } else if (app->hover_scale[i] != target_g) {
            app->hover_scale[i] = target_g;
            dirty = 1;
        }

        double target_d = (app->hovering && app->hover_dock_idx == i) ? 1.18 : 1.0;
        if (fabs(app->dock_scale[i] - target_d) > 0.004) {
            app->dock_scale[i] += (target_d - app->dock_scale[i]) * 0.30;
            dirty = 1;
        } else if (app->dock_scale[i] != target_d) {
            app->dock_scale[i] = target_d;
            dirty = 1;
        }
    }

    if (dirty) gtk_widget_queue_draw(app->window);
    return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    (void)w;
    OverviewApp *app = (OverviewApp *)data;
    app->hx = ev->x;
    app->hy = ev->y;
    app->hovering = 1;

    int prev_app = app->hover_app_idx;
    int prev_dock = app->hover_dock_idx;
    int prev_ws = app->hover_ws_idx;
    int prev_dots = app->hover_dots;

    app->hover_app_idx = -1;
    app->hover_dock_idx = -1;
    app->hover_ws_idx = -1;
    app->hover_dots = pt_in_rect(ev->x, ev->y, app->r_dots);

    for (int i = 0; i < app->r_app_n; i++) {
        if (pt_in_rect(ev->x, ev->y, app->r_app[i])) {
            app->hover_app_idx = app->r_app_idx[i];
            break;
        }
    }
    for (int i = 0; i < app->r_dock_n; i++) {
        if (pt_in_rect(ev->x, ev->y, app->r_dock[i])) {
            app->hover_dock_idx = app->r_dock_idx[i];
            break;
        }
    }
    for (int i = 0; i < app->num_ws && i < 4; i++) {
        if (pt_in_rect(ev->x, ev->y, app->r_ws[i])) {
            app->hover_ws_idx = i;
            break;
        }
    }

    if (prev_app != app->hover_app_idx || prev_dock != app->hover_dock_idx ||
        prev_ws != app->hover_ws_idx || prev_dots != app->hover_dots) {
        gtk_widget_queue_draw(app->window);
    }
    return FALSE;
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    OverviewApp *app = (OverviewApp *)data;
    if (ev->button != 1 || app->closing) return FALSE;

    for (int i = 0; i < app->num_ws && i < 4; i++) {
        if (pt_in_rect(ev->x, ev->y, app->r_ws[i])) {
            switch_workspace(app, i);
            start_close(app, NULL);
            return TRUE;
        }
    }
    for (int i = 0; i < app->r_app_n; i++) {
        if (pt_in_rect(ev->x, ev->y, app->r_app[i])) {
            start_close(app, app->apps[app->r_app_idx[i]].cmd);
            return TRUE;
        }
    }
    for (int i = 0; i < app->r_dock_n; i++) {
        if (pt_in_rect(ev->x, ev->y, app->r_dock[i])) {
            start_close(app, app->apps[app->r_dock_idx[i]].cmd);
            return TRUE;
        }
    }
    if (pt_in_rect(ev->x, ev->y, app->r_prev_pg) && app->target_page > 0) {
        app->target_page--;
        gtk_widget_queue_draw(app->window);
        return TRUE;
    }
    if (pt_in_rect(ev->x, ev->y, app->r_next_pg)) {
        app->target_page++;
        gtk_widget_queue_draw(app->window);
        return TRUE;
    }
    if (pt_in_rect(ev->x, ev->y, app->r_dots)) {
        start_close(app, NULL);
        return TRUE;
    }
    if (pt_in_rect(ev->x, ev->y, app->r_search)) {
        return TRUE;
    }

    /* Click empty backdrop -> smooth close */
    start_close(app, NULL);
    return TRUE;
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    (void)w;
    OverviewApp *app = (OverviewApp *)data;
    if (app->closing) return TRUE;

    if (ev->keyval == GDK_Escape || ev->keyval == GDK_Super_L || ev->keyval == GDK_Super_R) {
        start_close(app, NULL);
        return TRUE;
    }
    if (ev->keyval == GDK_Return || ev->keyval == GDK_KP_Enter) {
        if (app->filtered_count > 0) {
            start_close(app, app->apps[app->filtered[0]].cmd);
        } else {
            start_close(app, NULL);
        }
        return TRUE;
    }
    if (ev->keyval == GDK_Left && !app->search[0]) {
        if (app->cur_ws > 0) switch_workspace(app, app->cur_ws - 1);
        return TRUE;
    }
    if (ev->keyval == GDK_Right && !app->search[0]) {
        if (app->cur_ws + 1 < app->num_ws) switch_workspace(app, app->cur_ws + 1);
        return TRUE;
    }
    if (ev->keyval == GDK_BackSpace) {
        size_t len = strlen(app->search);
        if (len > 0) {
            app->search[len - 1] = '\0';
            rebuild_filter(app);
            gtk_widget_queue_draw(app->window);
        }
        return TRUE;
    }
    if (ev->keyval >= 0x20 && ev->keyval <= 0x7e && !(ev->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK))) {
        size_t len = strlen(app->search);
        if (len + 1 < sizeof(app->search)) {
            app->search[len] = (char)ev->keyval;
            app->search[len + 1] = '\0';
            rebuild_filter(app);
            gtk_widget_queue_draw(app->window);
        }
        return TRUE;
    }
    return FALSE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data) {
    (void)w;
    OverviewApp *app = (OverviewApp *)data;
    int per_page = 10;
    int total_pages = (app->filtered_count + per_page - 1) / per_page;
    if (ev->direction == GDK_SCROLL_DOWN || ev->direction == GDK_SCROLL_RIGHT) {
        if (app->target_page + 1 < total_pages) {
            app->target_page++;
            gtk_widget_queue_draw(app->window);
        }
    } else if (ev->direction == GDK_SCROLL_UP || ev->direction == GDK_SCROLL_LEFT) {
        if (app->target_page > 0) {
            app->target_page--;
            gtk_widget_queue_draw(app->window);
        }
    }
    return TRUE;
}

static void on_realize(GtkWidget *w, gpointer data) {
    (void)data;
    if (!w || !w->window) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    Window xid = GDK_WINDOW_XID(w->window);
    Atom a_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom a_above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    Atom a_full = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    Atom states[2] = { a_full, a_above };
    XChangeProperty(dpy, xid, a_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)states, 2);
}

int main(int argc, char *argv[]) {
    /* Toggle existing instance smoothly via SIGUSR1 */
    FILE *pf = fopen(PID_FILE, "r");
    if (pf) {
        int old_pid = 0;
        if (fscanf(pf, "%d", &old_pid) == 1 && old_pid > 1) {
            if (kill(old_pid, SIGUSR1) == 0) {
                fclose(pf);
                return 0;
            }
        }
        fclose(pf);
    }

    gtk_init(&argc, &argv);

    OverviewApp app;
    memset(&app, 0, sizeof(app));
    app.hover_app_idx = -1;
    app.hover_dock_idx = -1;
    app.hover_ws_idx = -1;

    GdkScreen *scr = gdk_screen_get_default();
    app.scr_w = gdk_screen_get_width(scr);
    app.scr_h = gdk_screen_get_height(scr);
    if (app.scr_w < 800) app.scr_w = 1280;
    if (app.scr_h < 600) app.scr_h = 800;

    capture_blurred_desktop(&app);
    probe_x11_state(&app);
    app.ws_sel_pos = (double)app.cur_ws;
    init_apps(&app);

    pf = fopen(PID_FILE, "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    signal(SIGUSR1, on_sig_usr1);

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "phantom-overview");
    gtk_window_set_decorated(GTK_WINDOW(app.window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(app.window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(app.window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(app.window), app.scr_w, app.scr_h);
    gtk_widget_set_size_request(app.window, app.scr_w, app.scr_h);
    gtk_window_move(GTK_WINDOW(app.window), 0, 0);
    gtk_window_fullscreen(GTK_WINDOW(app.window));
    gtk_window_stick(GTK_WINDOW(app.window));
    gtk_window_set_keep_above(GTK_WINDOW(app.window), TRUE);
    gtk_widget_set_app_paintable(app.window, TRUE);
    gtk_widget_add_events(app.window,
        GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK |
        GDK_KEY_PRESS_MASK | GDK_SCROLL_MASK);

    g_signal_connect(app.window, "realize", G_CALLBACK(on_realize), &app);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(app.window, "expose-event", G_CALLBACK(on_expose), &app);
    g_signal_connect(app.window, "button-press-event", G_CALLBACK(on_button_press), &app);
    g_signal_connect(app.window, "motion-notify-event", G_CALLBACK(on_motion), &app);
    g_signal_connect(app.window, "key-press-event", G_CALLBACK(on_key_press), &app);
    g_signal_connect(app.window, "scroll-event", G_CALLBACK(on_scroll), &app);

    g_timeout_add(16, on_anim_tick, &app);

    gtk_widget_show(app.window);
    gtk_window_present(GTK_WINDOW(app.window));
    gdk_keyboard_grab(app.window->window, TRUE, GDK_CURRENT_TIME);
    gtk_main();

    gdk_keyboard_ungrab(GDK_CURRENT_TIME);
    unlink(PID_FILE);
    if (app.blur_surf) cairo_surface_destroy(app.blur_surf);
    return 0;
}
