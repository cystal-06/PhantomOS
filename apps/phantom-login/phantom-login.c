#define _GNU_SOURCE
#include <cairo.h>
#include <crypt.h>
#include <ctype.h>
#include <errno.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <grp.h>
#include <gtk/gtk.h>
#include <math.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>

#define PHANTOM_LOGIN_TITLE "PhantomOS Display Manager"
#define SESSION_RUNNER "/usr/libexec/phantom-session"

static volatile sig_atomic_t g_terminate_requested = 0;

static void on_term_signal(int sig) {
    (void)sig;
    g_terminate_requested = 1;
}

typedef struct {
    double x, y, w, h;
} Rect;

typedef struct {
    GtkWidget *window;
    GtkWidget *fixed;
    GtkWidget *user_entry;
    GtkWidget *pass_entry;

    int scr_w;
    int scr_h;

    /* System metadata */
    char hostname[128];
    char kernel_ver[128];
    char root_mode[48];
    char ip_addr[64];
    char mem_str[64];
    char clock_str[64];

    /* UI State */
    int stage;             /* 0 = User Picker, 1 = Password + Inputs */
    int selected_user_idx; /* 0 = phantom, 1 = root, 2 = custom */
    int session_mode_idx;  /* 0 = Fluxbox Desktop, 1 = Terminal Console */
    int power_menu_open;
    int show_password;
    int auth_locked;
    int is_error;
    char status_msg[192];

    double hx, hy;
    int hovering;
    double anim_t;
    guint anim_id;
    gint64 last_tick_us;
    cairo_surface_t *bg_surf;
    int bg_w, bg_h;

    /* Interactive Hitboxes */
    Rect r_card_phantom;
    Rect r_card_root;
    Rect r_card_other;
    Rect r_back_btn;
    Rect r_eye_btn;
    Rect r_signin_btn;
    Rect r_session_btn;
    Rect r_power_btn;
    Rect r_reboot_btn;
    Rect r_poweroff_btn;

    pid_t session_pid;
    char active_x_user[64];
} LoginApp;

static int pt_in_rect(double px, double py, Rect r) {
    return r.w > 0 && px >= r.x && px <= (r.x + r.w) && py >= r.y && py <= (r.y + r.h);
}

static void secure_wipe(void *ptr, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len--) {
        *p++ = 0;
    }
}

static int constant_time_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la != lb || la == 0) return 0;
    unsigned char diff = 0;
    for (size_t i = 0; i < la; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    }
    return diff == 0;
}

static void read_system_metadata(LoginApp *app) {
    struct utsname u;
    if (gethostname(app->hostname, sizeof(app->hostname) - 1) != 0) {
        snprintf(app->hostname, sizeof(app->hostname), "phantomos");
    }
    app->hostname[sizeof(app->hostname) - 1] = '\0';

    if (uname(&u) == 0) {
        snprintf(app->kernel_ver, sizeof(app->kernel_ver), "%s", u.release);
    } else {
        snprintf(app->kernel_ver, sizeof(app->kernel_ver), "Linux");
    }

    snprintf(app->root_mode, sizeof(app->root_mode), "Ext4 RW");
    FILE *fp = fopen("/proc/mounts", "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, " / overlay ")) {
                snprintf(app->root_mode, sizeof(app->root_mode), "OverlayFS Stateless");
                break;
            }
        }
        fclose(fp);
    }

    snprintf(app->ip_addr, sizeof(app->ip_addr), "offline");
    FILE *ipf = popen("ip -4 addr show eth0 2>/dev/null | awk '/inet /{print $2; exit}'", "r");
    if (ipf) {
        char buf[64];
        if (fgets(buf, sizeof(buf), ipf)) {
            buf[strcspn(buf, "\r\n")] = '\0';
            if (buf[0] != '\0') {
                snprintf(app->ip_addr, sizeof(app->ip_addr), "%s", buf);
            }
        }
        pclose(ipf);
    }
}

static void read_memory_summary(char *out, size_t out_sz) {
    unsigned long total_kb = 0, free_kb = 0, buf_kb = 0, cache_kb = 0, sreclaim_kb = 0;
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char key[64];
        unsigned long val;
        while (fscanf(fp, "%63s %lu kB%*[^\n]", key, &val) == 2) {
            if (strcmp(key, "MemTotal:") == 0) total_kb = val;
            else if (strcmp(key, "MemFree:") == 0) free_kb = val;
            else if (strcmp(key, "Buffers:") == 0) buf_kb = val;
            else if (strcmp(key, "Cached:") == 0) cache_kb = val;
            else if (strcmp(key, "SReclaimable:") == 0) sreclaim_kb = val;
        }
        fclose(fp);
    }
    unsigned long avail_like = free_kb + buf_kb + cache_kb + sreclaim_kb;
    unsigned long used_kb = (total_kb > avail_like) ? (total_kb - avail_like) : 0;
    snprintf(out, out_sz, "%lu MB / %lu MB", used_kb / 1024, total_kb / 1024);
}

static void grant_x11_localuser(const char *username, gboolean add) {
    if (!username || !username[0]) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (!dpy) return;

    XServerInterpretedAddress si;
    si.type = "localuser";
    si.typelength = 9;
    si.value = (char *)username;
    si.valuelength = (int)strlen(username);

    XHostAddress ha;
    ha.family = FamilyServerInterpreted;
    ha.length = (int)sizeof(si);
    ha.address = (char *)&si;

    if (add) {
        XAddHost(dpy, &ha);
    } else {
        XRemoveHost(dpy, &ha);
    }
    XSync(dpy, False);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

static void draw_text(cairo_t *cr, double x, double y, const char *txt,
                      double r, double g, double b, int bold, double sz) {
    cairo_select_font_face(cr, "Adwaita Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, sz);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, txt);
    cairo_new_path(cr);
}

static void draw_text_centered(cairo_t *cr, double cx, double y, const char *txt,
                               double r, double g, double b, int bold, double sz) {
    cairo_select_font_face(cr, "Adwaita Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, sz);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, txt, &ext);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, cx - ext.width * 0.5 - ext.x_bearing, y);
    cairo_show_text(cr, txt);
    cairo_new_path(cr);
}

static void draw_avatar_disc(cairo_t *cr, double cx, double cy, double r, int is_root) {
    /* Subtle outer ring */
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r + 1.5, 0, 2.0 * M_PI);
    if (is_root) cairo_set_source_rgba(cr, 0.953, 0.545, 0.659, 0.55);
    else cairo_set_source_rgba(cr, 0.353, 0.569, 0.910, 0.48);
    cairo_set_line_width(cr, 1.8);
    cairo_stroke(cr);

    /* Clean dark neutral GNOME-style avatar disc */
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2.0 * M_PI);
    if (is_root) cairo_set_source_rgb(cr, 0.212, 0.165, 0.212);
    else cairo_set_source_rgb(cr, 0.165, 0.192, 0.275);
    cairo_fill(cr);

    /* Clipped clean user silhouette (head + shoulders) */
    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r - 1.0, 0, 2.0 * M_PI);
    cairo_clip(cr);

    if (is_root) cairo_set_source_rgb(cr, 0.961, 0.718, 0.780);
    else cairo_set_source_rgb(cr, 0.816, 0.855, 0.945);

    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy - r * 0.16, r * 0.27, 0, 2.0 * M_PI);
    cairo_fill(cr);

    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy + r * 0.74, r * 0.56, M_PI, 2.0 * M_PI);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void draw_user_glyph(cairo_t *cr, double cx, double cy) {
    cairo_set_source_rgb(cr, 0.580, 0.620, 0.745);
    cairo_set_line_width(cr, 1.4);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy - 3.0, 2.8, 0, 2.0 * M_PI);
    cairo_stroke(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy + 5.5, 5.0, M_PI * 1.1, M_PI * 1.9);
    cairo_stroke(cr);
}

static void draw_lock_glyph(cairo_t *cr, double cx, double cy) {
    cairo_set_source_rgb(cr, 0.580, 0.620, 0.745);
    cairo_set_line_width(cr, 1.4);
    rounded_rect(cr, cx - 4.5, cy - 0.5, 9.0, 6.5, 1.5);
    cairo_stroke(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy - 1.0, 2.8, M_PI, 2.0 * M_PI);
    cairo_stroke(cr);
}

static void draw_eye_glyph(cairo_t *cr, double cx, double cy, int active) {
    if (active) cairo_set_source_rgb(cr, 0.537, 0.706, 0.980);
    else cairo_set_source_rgb(cr, 0.580, 0.620, 0.745);
    cairo_set_line_width(cr, 1.3);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy + 3.0, 6.5, -2.35, -0.79);
    cairo_stroke(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy - 3.0, 6.5, 0.79, 2.35);
    cairo_stroke(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, 1.8, 0, 2.0 * M_PI);
    cairo_fill(cr);
    if (!active) {
        cairo_move_to(cr, cx - 5.0, cy + 4.5);
        cairo_line_to(cr, cx + 5.0, cy - 4.5);
        cairo_stroke(cr);
    }
}

static void draw_power_glyph(cairo_t *cr, double cx, double cy, double r, double red, double grn, double blu) {
    cairo_set_source_rgb(cr, red, grn, blu);
    cairo_set_line_width(cr, 1.5);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, -1.0, 4.14);
    cairo_stroke(cr);
    cairo_move_to(cr, cx, cy - r - 1.5);
    cairo_line_to(cr, cx, cy - 0.5);
    cairo_stroke(cr);
}

static void layout_entries(LoginApp *app) {
    int w = app->scr_w;
    int h = app->scr_h;
    if (app->stage == 1) {
        double cw = 396.0, ch = 392.0;
        double cx = (w - cw) * 0.5;
        double cy = (h - ch) * 0.5;
        int ix = (int)(cx + 76.0);
        int iw = 248;
        gtk_fixed_move(GTK_FIXED(app->fixed), app->user_entry, ix, (int)(cy + 168.0));
        gtk_widget_set_size_request(app->user_entry, iw, 24);
        gtk_fixed_move(GTK_FIXED(app->fixed), app->pass_entry, ix, (int)(cy + 224.0));
        gtk_widget_set_size_request(app->pass_entry, iw - 28, 24);
    }
}

static gboolean on_login_anim_tick(gpointer data) {
    LoginApp *app = (LoginApp *)data;
    gint64 now = g_get_monotonic_time();
    double dt = (app->last_tick_us > 0) ? ((double)(now - app->last_tick_us) / 1000000.0) : (1.0 / 60.0);
    app->last_tick_us = now;
    if (dt < 0.004) dt = 0.004;
    if (dt > 0.045) dt = 0.045;

    if (app->anim_t < 1.0) {
        app->anim_t += dt / 0.18;
        if (app->anim_t >= 1.0) {
            app->anim_t = 1.0;
        }
        if (app->stage == 1 && app->anim_t >= 0.72 && !GTK_WIDGET_VISIBLE(app->pass_entry)) {
            layout_entries(app);
            gtk_widget_show(app->user_entry);
            gtk_widget_show(app->pass_entry);
            gtk_widget_grab_focus((app->selected_user_idx == 2) ? app->user_entry : app->pass_entry);
        }
        gtk_widget_queue_draw(app->window);
        if (app->anim_t < 1.0) return TRUE;
    }
    app->anim_id = 0;
    app->last_tick_us = 0;
    return FALSE;
}

static void start_login_anim(LoginApp *app) {
    app->anim_t = 0.08;
    app->last_tick_us = g_get_monotonic_time();
    if (!app->anim_id) {
        app->anim_id = g_timeout_add_full(G_PRIORITY_HIGH, 10, on_login_anim_tick, app, NULL);
    }
}

static void enter_stage(LoginApp *app, int stage, int user_idx) {
    app->stage = stage;
    app->selected_user_idx = user_idx;
    app->power_menu_open = 0;
    app->is_error = 0;
    app->status_msg[0] = '\0';

    if (stage == 0) {
        gtk_widget_hide(app->user_entry);
        gtk_widget_hide(app->pass_entry);
        gtk_entry_set_text(GTK_ENTRY(app->pass_entry), "");
    } else {
        if (user_idx == 0) {
            gtk_entry_set_text(GTK_ENTRY(app->user_entry), "phantom");
        } else if (user_idx == 1) {
            gtk_entry_set_text(GTK_ENTRY(app->user_entry), "root");
        } else {
            gtk_entry_set_text(GTK_ENTRY(app->user_entry), "");
        }
        gtk_entry_set_text(GTK_ENTRY(app->pass_entry), "");
        gtk_widget_hide(app->user_entry);
        gtk_widget_hide(app->pass_entry);
    }
    start_login_anim(app);
    gtk_widget_queue_draw(app->window);
}

static void ensure_login_bg(LoginApp *app, int w, int h) {
    if (app->bg_surf && app->bg_w == w && app->bg_h == h) return;
    if (app->bg_surf) cairo_surface_destroy(app->bg_surf);
    app->bg_surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    app->bg_w = w;
    app->bg_h = h;
    cairo_t *cr = cairo_create(app->bg_surf);

    cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, w, h);
    cairo_pattern_add_color_stop_rgb(bg, 0.0, 0.043, 0.055, 0.090);
    cairo_pattern_add_color_stop_rgb(bg, 0.5, 0.071, 0.086, 0.141);
    cairo_pattern_add_color_stop_rgb(bg, 1.0, 0.055, 0.063, 0.106);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    cairo_pattern_t *g1 = cairo_pattern_create_radial(w * 0.25, h * 0.22, 10, w * 0.25, h * 0.22, w * 0.48);
    cairo_pattern_add_color_stop_rgba(g1, 0.0, 0.208, 0.518, 0.894, 0.16);
    cairo_pattern_add_color_stop_rgba(g1, 1.0, 0.208, 0.518, 0.894, 0.0);
    cairo_set_source(cr, g1);
    cairo_paint(cr);
    cairo_pattern_destroy(g1);

    cairo_pattern_t *g2 = cairo_pattern_create_radial(w * 0.78, h * 0.76, 10, w * 0.78, h * 0.76, w * 0.44);
    cairo_pattern_add_color_stop_rgba(g2, 0.0, 0.545, 0.478, 0.949, 0.13);
    cairo_pattern_add_color_stop_rgba(g2, 1.0, 0.545, 0.478, 0.949, 0.0);
    cairo_set_source(cr, g2);
    cairo_paint(cr);
    cairo_pattern_destroy(g2);

    cairo_set_source_rgba(cr, 0.039, 0.047, 0.078, 0.72);
    cairo_rectangle(cr, 0, 0, w, 36);
    cairo_fill(cr);

    cairo_destroy(cr);
}

static void on_login_realize(GtkWidget *w, gpointer data) {
    (void)data;
    if (w->window) {
        gdk_window_set_back_pixmap(w->window, NULL, FALSE);
    }
}

static gboolean on_window_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
    (void)event;
    LoginApp *app = (LoginApp *)data;
    cairo_t *cr = gdk_cairo_create(widget->window);
    int w = widget->allocation.width;
    int h = widget->allocation.height;
    app->scr_w = w;
    app->scr_h = h;

    /* Blit cached atmospheric gradient backdrop in <1ms instead of recomputing radial gradients */
    ensure_login_bg(app, w, h);
    if (app->bg_surf) {
        cairo_set_source_surface(cr, app->bg_surf, 0, 0);
        cairo_paint(cr);
    }

    draw_text(cr, 18, 22, "PhantomOS", 0.537, 0.706, 0.980, 1, 11.0);
    draw_text(cr, 102, 22, app->hostname, 0.580, 0.620, 0.745, 0, 10.0);
    draw_text_centered(cr, w * 0.5, 22, app->clock_str, 0.902, 0.922, 0.969, 1, 10.5);

    /* Top-right Power button */
    app->r_power_btn = (Rect){ w - 46, 5, 32, 26 };
    int pwr_hov = app->hovering && pt_in_rect(app->hx, app->hy, app->r_power_btn);
    if (pwr_hov || app->power_menu_open) {
        rounded_rect(cr, app->r_power_btn.x, app->r_power_btn.y, app->r_power_btn.w, app->r_power_btn.h, 13.0);
        cairo_set_source_rgb(cr, 0.200, 0.220, 0.302);
        cairo_fill(cr);
    }
    draw_power_glyph(cr, w - 30, 18.5, 4.8, 0.902, 0.922, 0.969);

    /* Optional floating Power submenu */
    if (app->power_menu_open) {
        double pmx = w - 156.0, pmy = 42.0, pmw = 142.0, pmh = 84.0;
        rounded_rect(cr, pmx, pmy, pmw, pmh, 12.0);
        cairo_set_source_rgb(cr, 0.118, 0.133, 0.192);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.251, 0.282, 0.388);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        app->r_reboot_btn   = (Rect){ pmx + 8, pmy + 8, pmw - 16, 30 };
        app->r_poweroff_btn = (Rect){ pmx + 8, pmy + 44, pmw - 16, 30 };
        int rh = app->hovering && pt_in_rect(app->hx, app->hy, app->r_reboot_btn);
        int ph = app->hovering && pt_in_rect(app->hx, app->hy, app->r_poweroff_btn);

        rounded_rect(cr, app->r_reboot_btn.x, app->r_reboot_btn.y, app->r_reboot_btn.w, app->r_reboot_btn.h, 8.0);
        cairo_set_source_rgb(cr, rh ? 0.22 : 0.15, rh ? 0.24 : 0.17, rh ? 0.33 : 0.24);
        cairo_fill(cr);
        draw_text(cr, pmx + 22, pmy + 27, "Reboot", 0.976, 0.702, 0.529, 1, 10.5);

        rounded_rect(cr, app->r_poweroff_btn.x, app->r_poweroff_btn.y, app->r_poweroff_btn.w, app->r_poweroff_btn.h, 8.0);
        cairo_set_source_rgb(cr, ph ? 0.28 : 0.15, ph ? 0.18 : 0.17, ph ? 0.22 : 0.24);
        cairo_fill(cr);
        draw_text(cr, pmx + 22, pmy + 63, "Power Off", 0.953, 0.545, 0.659, 1, 10.5);
    } else {
        app->r_reboot_btn = app->r_poweroff_btn = (Rect){0,0,0,0};
    }

    double at = app->anim_t > 0.0 ? app->anim_t : 1.0;
    double ae = 1.0 - pow(1.0 - at, 3.0);
    if (ae < 0.999) {
        cairo_push_group(cr);
        cairo_translate(cr, 0.0, (1.0 - ae) * 16.0);
    }

    if (app->stage == 0) {
        /* STAGE 0: Arch/GNOME GDM User Picker */
        app->r_back_btn = app->r_eye_btn = app->r_signin_btn = (Rect){0,0,0,0};

        double cx = w * 0.5;
        double start_y = h * 0.5 - 125.0;

        draw_text_centered(cr, cx, start_y, "Sign In", 0.922, 0.941, 0.980, 1, 20.0);
        draw_text_centered(cr, cx, start_y + 24, "Choose an account to continue", 0.580, 0.620, 0.745, 0, 10.5);

        double pw = 332.0, ph = 68.0;
        double px = cx - pw * 0.5;
        app->r_card_phantom = (Rect){ px, start_y + 48.0, pw, ph };
        app->r_card_root    = (Rect){ px, start_y + 128.0, pw, ph };
        app->r_card_other   = (Rect){ cx - 72.0, start_y + 214.0, 144.0, 28.0 };

        struct {
            Rect r;
            const char *name;
            const char *sub;
            int is_root;
            int idx;
        } cards[2] = {
            { app->r_card_phantom, "phantom", "Standard User  •  sudo", 0, 0 },
            { app->r_card_root,    "root",    "Administrator  •  UID 0", 1, 1 }
        };

        for (int i = 0; i < 2; i++) {
            int hov = app->hovering && pt_in_rect(app->hx, app->hy, cards[i].r);
            int sel = (app->selected_user_idx == cards[i].idx);

            /* Soft shadow */
            rounded_rect(cr, cards[i].r.x - 1, cards[i].r.y + 3, cards[i].r.w + 2, cards[i].r.h + 2, 18.0);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.30);
            cairo_fill(cr);

            rounded_rect(cr, cards[i].r.x, cards[i].r.y, cards[i].r.w, cards[i].r.h, 16.0);
            cairo_set_source_rgba(cr, hov ? 0.165 : 0.118, hov ? 0.188 : 0.133, hov ? 0.275 : 0.196, 0.92);
            cairo_fill_preserve(cr);
            if (hov || sel) {
                if (cards[i].is_root) cairo_set_source_rgba(cr, 0.953, 0.545, 0.659, hov ? 0.85 : 0.45);
                else cairo_set_source_rgba(cr, 0.537, 0.706, 0.980, hov ? 0.85 : 0.45);
                cairo_set_line_width(cr, hov ? 1.5 : 1.1);
            } else {
                cairo_set_source_rgba(cr, 0.251, 0.282, 0.388, 0.55);
                cairo_set_line_width(cr, 1.0);
            }
            cairo_stroke(cr);

            draw_avatar_disc(cr, cards[i].r.x + 36.0, cards[i].r.y + ph * 0.5, 20.0,
                             cards[i].is_root);

            draw_text(cr, cards[i].r.x + 70.0, cards[i].r.y + 30.0, cards[i].name,
                      0.922, 0.941, 0.980, 1, 13.0);
            draw_text(cr, cards[i].r.x + 70.0, cards[i].r.y + 48.0, cards[i].sub,
                      0.580, 0.620, 0.745, 0, 9.5);

            /* Chevron > */
            double ax = cards[i].r.x + pw - 24.0, ay = cards[i].r.y + ph * 0.5;
            cairo_set_source_rgb(cr, 0.651, 0.690, 0.800);
            cairo_set_line_width(cr, 1.6);
            cairo_move_to(cr, ax - 3.0, ay - 5.0);
            cairo_line_to(cr, ax + 3.0, ay);
            cairo_line_to(cr, ax - 3.0, ay + 5.0);
            cairo_stroke(cr);
        }

        int ohov = app->hovering && pt_in_rect(app->hx, app->hy, app->r_card_other);
        rounded_rect(cr, app->r_card_other.x, app->r_card_other.y, app->r_card_other.w, app->r_card_other.h, 14.0);
        cairo_set_source_rgba(cr, ohov ? 0.20 : 0.13, ohov ? 0.22 : 0.15, ohov ? 0.31 : 0.22, ohov ? 0.9 : 0.6);
        cairo_fill(cr);
        draw_text_centered(cr, cx, app->r_card_other.y + 18.0, "Not listed? Enter user",
                           0.651, 0.690, 0.800, 0, 9.5);

        if (app->status_msg[0]) {
            draw_text_centered(cr, cx, start_y + 266.0, app->status_msg,
                               app->is_error ? 0.953 : 0.651,
                               app->is_error ? 0.545 : 0.890,
                               app->is_error ? 0.659 : 0.631, 0, 10.0);
        }
    } else {
        /* STAGE 1: Revealed sleek 2-input Card (User + Password + Sign In) */
        app->r_card_phantom = app->r_card_root = app->r_card_other = (Rect){0,0,0,0};

        double cw = 396.0, ch = 392.0;
        double cx = (w - cw) * 0.5;
        double cy = (h - ch) * 0.5;

        const char *u_txt = gtk_entry_get_text(GTK_ENTRY(app->user_entry));
        int is_root = (u_txt && strcmp(u_txt, "root") == 0);

        /* Card Shadow */
        rounded_rect(cr, cx - 4, cy + 8, cw + 8, ch + 8, 24.0);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.45);
        cairo_fill(cr);

        /* Glass Card Surface */
        rounded_rect(cr, cx, cy, cw, ch, 22.0);
        cairo_set_source_rgba(cr, 0.098, 0.110, 0.165, 0.95);
        cairo_fill_preserve(cr);
        if (is_root) cairo_set_source_rgba(cr, 0.953, 0.545, 0.659, 0.45);
        else cairo_set_source_rgba(cr, 0.329, 0.569, 0.945, 0.45);
        cairo_set_line_width(cr, 1.2);
        cairo_stroke(cr);

        /* Top-left Back Circle Button */
        app->r_back_btn = (Rect){ cx + 18, cy + 18, 32, 32 };
        int bhov = app->hovering && pt_in_rect(app->hx, app->hy, app->r_back_btn);
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx + 34, cy + 34, 16.0, 0, 2.0 * M_PI);
        cairo_set_source_rgb(cr, bhov ? 0.220 : 0.145, bhov ? 0.240 : 0.161, bhov ? 0.330 : 0.235);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.851, 0.878, 0.961);
        cairo_set_line_width(cr, 1.6);
        cairo_move_to(cr, cx + 39, cy + 34);
        cairo_line_to(cr, cx + 29, cy + 34);
        cairo_move_to(cr, cx + 33, cy + 29.5);
        cairo_line_to(cr, cx + 28.5, cy + 34);
        cairo_line_to(cr, cx + 33, cy + 38.5);
        cairo_stroke(cr);

        /* Avatar + User Name */
        draw_avatar_disc(cr, cx + cw * 0.5, cy + 68.0, 36.0, is_root);
        const char *disp_name = (u_txt && u_txt[0]) ? u_txt : "Enter Username";
        draw_text_centered(cr, cx + cw * 0.5, cy + 128.0, disp_name,
                           0.922, 0.941, 0.980, 1, 14.5);
        draw_text_centered(cr, cx + cw * 0.5, cy + 145.0,
                           is_root ? "Administrator (UID 0)" : "Standard User (sudo)",
                           0.580, 0.620, 0.745, 0, 9.5);

        /* Input 1: Username Pill Frame */
        double ix = cx + 38.0, iw = cw - 76.0, ih = 40.0;
        double y1 = cy + 160.0;
        int u_foc = gtk_widget_has_focus(app->user_entry);
        rounded_rect(cr, ix, y1, iw, ih, 12.0);
        cairo_set_source_rgb(cr, 0.063, 0.071, 0.110);
        cairo_fill_preserve(cr);
        if (u_foc) cairo_set_source_rgb(cr, is_root ? 0.953 : 0.329, is_root ? 0.545 : 0.569, is_root ? 0.659 : 0.945);
        else cairo_set_source_rgb(cr, 0.212, 0.239, 0.333);
        cairo_set_line_width(cr, u_foc ? 1.6 : 1.1);
        cairo_stroke(cr);
        draw_user_glyph(cr, ix + 19.0, y1 + ih * 0.5);

        /* Input 2: Password Pill Frame */
        double y2 = cy + 216.0;
        int p_foc = gtk_widget_has_focus(app->pass_entry);
        rounded_rect(cr, ix, y2, iw, ih, 12.0);
        cairo_set_source_rgb(cr, 0.063, 0.071, 0.110);
        cairo_fill_preserve(cr);
        if (p_foc) cairo_set_source_rgb(cr, is_root ? 0.953 : 0.329, is_root ? 0.545 : 0.569, is_root ? 0.659 : 0.945);
        else cairo_set_source_rgb(cr, 0.212, 0.239, 0.333);
        cairo_set_line_width(cr, p_foc ? 1.6 : 1.1);
        cairo_stroke(cr);
        draw_lock_glyph(cr, ix + 19.0, y2 + ih * 0.5);

        app->r_eye_btn = (Rect){ ix + iw - 34.0, y2 + 4.0, 28.0, 32.0 };
        draw_eye_glyph(cr, ix + iw - 19.0, y2 + ih * 0.5, app->show_password);

        /* Sign In Pill CTA */
        double y3 = cy + 278.0;
        app->r_signin_btn = (Rect){ ix, y3, iw, 42.0 };
        int shov = app->hovering && pt_in_rect(app->hx, app->hy, app->r_signin_btn);
        rounded_rect(cr, ix, y3, iw, 42.0, 21.0);
        if (app->auth_locked) {
            cairo_set_source_rgb(cr, 0.23, 0.25, 0.34);
        } else if (is_root) {
            cairo_set_source_rgb(cr, shov ? 0.980 : 0.922, shov ? 0.600 : 0.478, shov ? 0.700 : 0.588);
        } else {
            cairo_set_source_rgb(cr, shov ? 0.267 : 0.208, shov ? 0.584 : 0.518, shov ? 0.961 : 0.894);
        }
        cairo_fill(cr);
        draw_text_centered(cr, cx + cw * 0.5, y3 + 25.5,
                           app->auth_locked ? "Checking..." : "Sign In",
                           1.0, 1.0, 1.0, 1, 11.5);

        /* Status message */
        const char *smsg = app->status_msg[0] ? app->status_msg : "Press Enter to unlock";
        draw_text_centered(cr, cx + cw * 0.5, cy + 354.0, smsg,
                           app->is_error ? 0.953 : 0.580,
                           app->is_error ? 0.545 : 0.620,
                           app->is_error ? 0.659 : 0.745,
                           app->is_error, 9.8);
    }

    if (ae < 0.999) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.15 + 0.85 * ae);
    }

    /* Minimal Bottom Bar */
    char foot[384];
    snprintf(foot, sizeof(foot), "Kernel %s  •  RAM %s  •  eth0: %s  •  %s",
             app->kernel_ver, app->mem_str, app->ip_addr, app->root_mode);
    draw_text(cr, 18, h - 14, foot, 0.520, 0.557, 0.667, 0, 9.5);

    /* Bottom-right Session Pill (GDM Gear) */
    const char *sess_txt = (app->session_mode_idx == 1) ? "Session: Terminal" : "Session: Fluxbox";
    app->r_session_btn = (Rect){ w - 160, h - 34, 144, 24 };
    int sghov = app->hovering && pt_in_rect(app->hx, app->hy, app->r_session_btn);
    rounded_rect(cr, app->r_session_btn.x, app->r_session_btn.y, app->r_session_btn.w, app->r_session_btn.h, 12.0);
    cairo_set_source_rgba(cr, sghov ? 0.20 : 0.13, sghov ? 0.22 : 0.15, sghov ? 0.31 : 0.21, 0.85);
    cairo_fill(cr);
    draw_text_centered(cr, w - 88, h - 18, sess_txt, 0.804, 0.839, 0.957, 1, 9.5);

    cairo_destroy(cr);
    return FALSE;
}

static void style_entry(GtkWidget *entry) {
    GdkColor base, text;
    gdk_color_parse("#10121c", &base);
    gdk_color_parse("#e4e7f5", &text);
    gtk_entry_set_has_frame(GTK_ENTRY(entry), FALSE);
    gtk_widget_modify_base(entry, GTK_STATE_NORMAL, &base);
    gtk_widget_modify_text(entry, GTK_STATE_NORMAL, &text);
    PangoFontDescription *fd = pango_font_description_from_string("Adwaita Sans 11");
    gtk_widget_modify_font(entry, fd);
    pango_font_description_free(fd);
}

static gboolean on_clock_tick(gpointer data) {
    LoginApp *app = (LoginApp *)data;
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(app->clock_str, sizeof(app->clock_str), "%a %b %d   %H:%M", &tm_now);
    read_memory_summary(app->mem_str, sizeof(app->mem_str));
    gtk_widget_queue_draw(app->window);
    return TRUE;
}

static gboolean on_unlock_auth(gpointer data) {
    LoginApp *app = (LoginApp *)data;
    app->auth_locked = FALSE;
    gtk_widget_set_sensitive(app->pass_entry, TRUE);
    gtk_widget_grab_focus(app->pass_entry);
    gtk_widget_queue_draw(app->window);
    return FALSE;
}

static gboolean verify_credentials(const char *username, const char *password, struct passwd **out_pw) {
    if (!username || !username[0] || !password) return FALSE;
    struct passwd *pw = getpwnam(username);
    if (!pw) return FALSE;

    struct spwd *sp = getspnam(username);
    const char *hash = (sp && sp->sp_pwdp) ? sp->sp_pwdp : pw->pw_passwd;
    if (!hash || hash[0] == '\0' || hash[0] == '!' || hash[0] == '*') return FALSE;

    struct crypt_data cdata;
    memset(&cdata, 0, sizeof(cdata));
    char *computed = crypt_r(password, hash, &cdata);
    int ok = (computed && constant_time_streq(computed, hash));
    secure_wipe(&cdata, sizeof(cdata));
    if (ok) {
        *out_pw = pw;
        return TRUE;
    }
    return FALSE;
}

static gboolean focus_login_idle(gpointer data) {
    LoginApp *app = (LoginApp *)data;
    if (app->session_pid > 0 || !GTK_WIDGET_VISIBLE(app->window)) {
        return FALSE;
    }
    if (!app->window || !app->window->window || !GTK_WIDGET_MAPPED(app->window)) {
        return TRUE;
    }
    GdkCursor *cursor = gdk_cursor_new(GDK_LEFT_PTR);
    if (cursor) {
        GdkWindow *root = gdk_get_default_root_window();
        if (root) gdk_window_set_cursor(root, cursor);
        gdk_window_set_cursor(app->window->window, cursor);
        gdk_cursor_unref(cursor);
    }
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    Window xwin = GDK_WINDOW_XID(app->window->window);
    XUngrabPointer(dpy, CurrentTime);
    XUngrabKeyboard(dpy, CurrentTime);
    XRaiseWindow(dpy, xwin);
    XSetInputFocus(dpy, xwin, RevertToPointerRoot, CurrentTime);
    XFlush(dpy);
    gtk_window_present(GTK_WINDOW(app->window));
    if (app->stage == 1) gtk_widget_grab_focus(app->pass_entry);
    return FALSE;
}

static gboolean check_term_signal(gpointer data) {
    LoginApp *app = (LoginApp *)data;
    if (!g_terminate_requested) return TRUE;
    if (app->session_pid > 0) {
        kill(-app->session_pid, SIGTERM);
        usleep(150000);
        kill(-app->session_pid, SIGKILL);
        waitpid(app->session_pid, NULL, WNOHANG);
        app->session_pid = -1;
    }
    if (app->active_x_user[0] && strcmp(app->active_x_user, "root") != 0) {
        grant_x11_localuser(app->active_x_user, FALSE);
        app->active_x_user[0] = '\0';
    }
    gtk_main_quit();
    return FALSE;
}

static void on_session_exit(GPid pid, gint status, gpointer data) {
    (void)status;
    LoginApp *app = (LoginApp *)data;
    if (pid > 0) {
        kill(-pid, SIGTERM);
        usleep(150000);
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, WNOHANG);
    }
    g_spawn_close_pid(pid);
    app->session_pid = -1;

    if (app->active_x_user[0] && strcmp(app->active_x_user, "root") != 0) {
        grant_x11_localuser(app->active_x_user, FALSE);
    }
    app->active_x_user[0] = '\0';

    app->auth_locked = FALSE;
    gtk_widget_set_sensitive(app->pass_entry, TRUE);
    enter_stage(app, 0, 0);
    app->is_error = 0;
    snprintf(app->status_msg, sizeof(app->status_msg), "Session closed cleanly.");
    gtk_window_move(GTK_WINDOW(app->window), 0, 0);
    gtk_widget_show(app->window);
    if (app->window->window) gdk_window_raise(app->window->window);
    gdk_flush();
    g_timeout_add(60, focus_login_idle, app);
}

static void launch_user_session(LoginApp *app, struct passwd *pw, const char *session_mode) {
    char runtime_dir[128];
    snprintf(runtime_dir, sizeof(runtime_dir), "/tmp/runtime-%lu", (unsigned long)pw->pw_uid);
    mkdir(runtime_dir, 0700);
    chmod(runtime_dir, 0700);
    if (chown(runtime_dir, pw->pw_uid, pw->pw_gid) != 0) {
        /* non-fatal fallback */
    }

    if (strcmp(pw->pw_name, "root") != 0) {
        grant_x11_localuser(pw->pw_name, TRUE);
    }
    snprintf(app->active_x_user, sizeof(app->active_x_user), "%s", pw->pw_name);

    gtk_widget_hide(app->window);
    while (gtk_events_pending()) gtk_main_iteration();
    gdk_flush();

    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;
    char uname[64], uhome[256], ushell[128];
    snprintf(uname, sizeof(uname), "%s", pw->pw_name);
    snprintf(uhome, sizeof(uhome), "%s", (pw->pw_dir && pw->pw_dir[0]) ? pw->pw_dir : "/");
    snprintf(ushell, sizeof(ushell), "%s", (pw->pw_shell && pw->pw_shell[0]) ? pw->pw_shell : "/bin/sh");

    pid_t pid = fork();
    if (pid < 0) {
        grant_x11_localuser(uname, FALSE);
        gtk_widget_show(app->window);
        app->is_error = 1;
        snprintf(app->status_msg, sizeof(app->status_msg), "Failed to fork session process.");
        gtk_widget_queue_draw(app->window);
        return;
    }

    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        setsid();

        if (setgid(gid) != 0 || initgroups(uname, gid) != 0 || setuid(uid) != 0) _exit(126);
        if (chdir(uhome) != 0 && chdir("/") != 0) _exit(126);

        clearenv();
        setenv("HOME", uhome, 1);
        setenv("USER", uname, 1);
        setenv("LOGNAME", uname, 1);
        setenv("SHELL", ushell, 1);
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        setenv("DISPLAY", ":0", 1);
        setenv("LANG", "C.UTF-8", 1);
        setenv("LC_ALL", "C.UTF-8", 1);
        setenv("XDG_RUNTIME_DIR", runtime_dir, 1);

        char cmd[512];
        snprintf(cmd, sizeof(cmd), "%s %s", SESSION_RUNNER, session_mode);
        execl(ushell, ushell, "-l", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    app->session_pid = pid;
    g_child_watch_add((GPid)pid, on_session_exit, app);
}

static void attempt_login(LoginApp *app) {
    if (app->auth_locked || app->stage != 1) return;

    const char *raw_user = gtk_entry_get_text(GTK_ENTRY(app->user_entry));
    const char *raw_pass = gtk_entry_get_text(GTK_ENTRY(app->pass_entry));

    char username[64];
    snprintf(username, sizeof(username), "%s", raw_user ? raw_user : "");
    size_t len = strlen(username);
    while (len > 0 && isspace((unsigned char)username[len - 1])) username[--len] = '\0';

    char *password = g_strdup(raw_pass ? raw_pass : "");
    gtk_entry_set_text(GTK_ENTRY(app->pass_entry), "");

    struct passwd *pw = NULL;
    if (!verify_credentials(username, password, &pw)) {
        if (password) {
            secure_wipe(password, strlen(password));
            g_free(password);
        }
        app->is_error = 1;
        snprintf(app->status_msg, sizeof(app->status_msg), "Authentication failed. Check password.");
        app->auth_locked = TRUE;
        gtk_widget_set_sensitive(app->pass_entry, FALSE);
        gtk_widget_queue_draw(app->window);
        g_timeout_add(1000, on_unlock_auth, app);
        return;
    }

    secure_wipe(password, strlen(password));
    g_free(password);

    const char *session_mode = (app->session_mode_idx == 1) ? "terminal" : "desktop";
    app->is_error = 0;
    app->status_msg[0] = '\0';
    launch_user_session(app, pw, session_mode);
}

static void on_entry_activate(GtkEntry *e, gpointer data) {
    LoginApp *app = (LoginApp *)data;
    if (GTK_WIDGET(e) == app->user_entry) {
        gtk_widget_grab_focus(app->pass_entry);
    } else {
        attempt_login(app);
    }
}

static gboolean on_entry_changed_or_focus(GtkWidget *w, gpointer data) {
    (void)w;
    gtk_widget_queue_draw(((LoginApp *)data)->window);
    return FALSE;
}

static gboolean on_entry_focus_ev(GtkWidget *w, GdkEventFocus *ev, gpointer data) {
    (void)w;
    (void)ev;
    gtk_widget_queue_draw(((LoginApp *)data)->window);
    return FALSE;
}

static gboolean on_window_button(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    LoginApp *app = (LoginApp *)data;
    if (ev->button != 1) return FALSE;

    if (pt_in_rect(ev->x, ev->y, app->r_power_btn)) {
        app->power_menu_open = !app->power_menu_open;
        gtk_widget_queue_draw(app->window);
        return TRUE;
    }
    if (app->power_menu_open) {
        if (pt_in_rect(ev->x, ev->y, app->r_reboot_btn)) {
            sync();
            execl("/sbin/reboot", "/sbin/reboot", (char *)NULL);
            return TRUE;
        }
        if (pt_in_rect(ev->x, ev->y, app->r_poweroff_btn)) {
            sync();
            execl("/sbin/poweroff", "/sbin/poweroff", (char *)NULL);
            return TRUE;
        }
        app->power_menu_open = 0;
        gtk_widget_queue_draw(app->window);
        return TRUE;
    }

    if (pt_in_rect(ev->x, ev->y, app->r_session_btn)) {
        app->session_mode_idx = !app->session_mode_idx;
        gtk_widget_queue_draw(app->window);
        return TRUE;
    }

    if (app->stage == 0) {
        if (pt_in_rect(ev->x, ev->y, app->r_card_phantom)) {
            enter_stage(app, 1, 0);
        } else if (pt_in_rect(ev->x, ev->y, app->r_card_root)) {
            enter_stage(app, 1, 1);
        } else if (pt_in_rect(ev->x, ev->y, app->r_card_other)) {
            enter_stage(app, 1, 2);
        }
    } else {
        if (pt_in_rect(ev->x, ev->y, app->r_back_btn)) {
            enter_stage(app, 0, app->selected_user_idx < 2 ? app->selected_user_idx : 0);
        } else if (pt_in_rect(ev->x, ev->y, app->r_eye_btn)) {
            app->show_password = !app->show_password;
            gtk_entry_set_visibility(GTK_ENTRY(app->pass_entry), app->show_password);
            gtk_widget_grab_focus(app->pass_entry);
            gtk_widget_queue_draw(app->window);
        } else if (pt_in_rect(ev->x, ev->y, app->r_signin_btn)) {
            attempt_login(app);
        }
    }
    return TRUE;
}

static gboolean on_window_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    (void)w;
    LoginApp *app = (LoginApp *)data;
    app->hx = ev->x;
    app->hy = ev->y;
    app->hovering = 1;
    gtk_widget_queue_draw(app->window);
    return FALSE;
}

static gboolean on_window_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    (void)w;
    LoginApp *app = (LoginApp *)data;
    if (app->stage == 0) {
        if (ev->keyval == GDK_Up || ev->keyval == GDK_Down || ev->keyval == GDK_Tab) {
            app->selected_user_idx = (app->selected_user_idx == 0) ? 1 : 0;
            gtk_widget_queue_draw(app->window);
            return TRUE;
        }
        if (ev->keyval == GDK_Return || ev->keyval == GDK_KP_Enter || ev->keyval == GDK_space) {
            enter_stage(app, 1, app->selected_user_idx);
            return TRUE;
        }
    } else {
        if (ev->keyval == GDK_Escape) {
            enter_stage(app, 0, app->selected_user_idx < 2 ? app->selected_user_idx : 0);
            return TRUE;
        }
    }
    return FALSE;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    LoginApp app;
    memset(&app, 0, sizeof(app));
    app.session_pid = -1;
    read_system_metadata(&app);

    GdkScreen *scr = gdk_screen_get_default();
    app.scr_w = gdk_screen_get_width(scr);
    app.scr_h = gdk_screen_get_height(scr);
    if (app.scr_w < 800) app.scr_w = 1920;
    if (app.scr_h < 600) app.scr_h = 1080;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), PHANTOM_LOGIN_TITLE);
    gtk_window_set_decorated(GTK_WINDOW(app.window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(app.window), app.scr_w, app.scr_h);
    gtk_widget_set_size_request(app.window, app.scr_w, app.scr_h);
    gtk_window_move(GTK_WINDOW(app.window), 0, 0);
    gtk_widget_set_app_paintable(app.window, TRUE);
    gtk_widget_add_events(app.window,
        GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK);

    app.fixed = gtk_fixed_new();
    gtk_container_add(GTK_CONTAINER(app.window), app.fixed);

    app.user_entry = gtk_entry_new();
    style_entry(app.user_entry);
    gtk_fixed_put(GTK_FIXED(app.fixed), app.user_entry, 0, 0);

    app.pass_entry = gtk_entry_new();
    style_entry(app.pass_entry);
    gtk_entry_set_visibility(GTK_ENTRY(app.pass_entry), FALSE);
    gtk_entry_set_invisible_char(GTK_ENTRY(app.pass_entry), 0x2022);
    gtk_fixed_put(GTK_FIXED(app.fixed), app.pass_entry, 0, 0);

    g_signal_connect(app.window, "realize", G_CALLBACK(on_login_realize), &app);
    g_signal_connect(app.window, "expose-event", G_CALLBACK(on_window_expose), &app);
    g_signal_connect(app.window, "button-press-event", G_CALLBACK(on_window_button), &app);
    g_signal_connect(app.window, "motion-notify-event", G_CALLBACK(on_window_motion), &app);
    g_signal_connect(app.window, "key-press-event", G_CALLBACK(on_window_key), &app);

    g_signal_connect(app.user_entry, "changed", G_CALLBACK(on_entry_changed_or_focus), &app);
    g_signal_connect(app.user_entry, "activate", G_CALLBACK(on_entry_activate), &app);
    g_signal_connect(app.user_entry, "focus-in-event", G_CALLBACK(on_entry_focus_ev), &app);
    g_signal_connect(app.user_entry, "focus-out-event", G_CALLBACK(on_entry_focus_ev), &app);

    g_signal_connect(app.pass_entry, "activate", G_CALLBACK(on_entry_activate), &app);
    g_signal_connect(app.pass_entry, "focus-in-event", G_CALLBACK(on_entry_focus_ev), &app);
    g_signal_connect(app.pass_entry, "focus-out-event", G_CALLBACK(on_entry_focus_ev), &app);

    on_clock_tick(&app);
    g_timeout_add(1000, on_clock_tick, &app);
    signal(SIGTERM, on_term_signal);
    signal(SIGINT, on_term_signal);
    g_timeout_add(250, check_term_signal, &app);

    gtk_widget_show(app.fixed);
    gtk_widget_show(app.window);
    enter_stage(&app, 0, 0);
    g_idle_add(focus_login_idle, &app);

    gtk_main();
    if (app.bg_surf) cairo_surface_destroy(app.bg_surf);
    return 0;
}
