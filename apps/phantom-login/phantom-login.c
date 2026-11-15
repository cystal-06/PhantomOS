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

typedef struct {
    GtkWidget *window;
    GtkWidget *top_title_label;
    GtkWidget *clock_label;
    GtkWidget *bottom_info_label;
    GtkWidget *avatar_area;
    GtkWidget *role_label;
    GtkWidget *btn_user_phantom;
    GtkWidget *btn_user_root;
    GtkWidget *user_entry;
    GtkWidget *pass_entry;
    GtkWidget *toggle_pass_btn;
    GtkWidget *session_combo;
    GtkWidget *login_btn;
    GtkWidget *status_label;
    GtkWidget *reboot_btn;
    GtkWidget *poweroff_btn;

    char hostname[128];
    char kernel_ver[128];
    char root_mode[48];
    char ip_addr[64];

    gboolean is_root_selected;
    gboolean show_password;
    gboolean auth_locked;
    pid_t session_pid;
    char active_x_user[64];
} LoginApp;

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

static void draw_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    double rad = r;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - rad, y + rad, rad, -M_PI / 2.0, 0);
    cairo_arc(cr, x + w - rad, y + h - rad, rad, 0, M_PI / 2.0);
    cairo_arc(cr, x + rad, y + h - rad, rad, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + rad, y + rad, rad, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

static gboolean on_window_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
    (void)event;
    LoginApp *app = (LoginApp *)data;
    cairo_t *cr = gdk_cairo_create(widget->window);
    int w = widget->allocation.width;
    int h = widget->allocation.height;

    /* Deep Catppuccin Crust/Mantle vertical gradient */
    cairo_pattern_t *bg_pat = cairo_pattern_create_linear(0, 0, w, h);
    cairo_pattern_add_color_stop_rgb(bg_pat, 0.0, 0.055, 0.062, 0.095);
    cairo_pattern_add_color_stop_rgb(bg_pat, 0.55, 0.082, 0.086, 0.133);
    cairo_pattern_add_color_stop_rgb(bg_pat, 1.0, 0.067, 0.067, 0.106);
    cairo_set_source(cr, bg_pat);
    cairo_paint(cr);
    cairo_pattern_destroy(bg_pat);

    /* Subtle top-left blue radial ambient glow */
    cairo_pattern_t *glow1 = cairo_pattern_create_radial(w * 0.22, h * 0.22, 10, w * 0.22, h * 0.22, w * 0.45);
    cairo_pattern_add_color_stop_rgba(glow1, 0.0, 0.537, 0.706, 0.980, 0.14);
    cairo_pattern_add_color_stop_rgba(glow1, 1.0, 0.537, 0.706, 0.980, 0.0);
    cairo_set_source(cr, glow1);
    cairo_paint(cr);
    cairo_pattern_destroy(glow1);

    /* Subtle bottom-right mauve ambient glow */
    cairo_pattern_t *glow2 = cairo_pattern_create_radial(w * 0.80, h * 0.78, 10, w * 0.80, h * 0.78, w * 0.42);
    cairo_pattern_add_color_stop_rgba(glow2, 0.0, 0.796, 0.651, 0.969, 0.12);
    cairo_pattern_add_color_stop_rgba(glow2, 1.0, 0.796, 0.651, 0.969, 0.0);
    cairo_set_source(cr, glow2);
    cairo_paint(cr);
    cairo_pattern_destroy(glow2);

    /* Subtle tech dot grid */
    cairo_set_source_rgba(cr, 0.576, 0.600, 0.702, 0.08);
    for (int gx = 24; gx < w; gx += 32) {
        for (int gy = 56; gy < h - 56; gy += 32) {
            cairo_rectangle(cr, gx, gy, 1.5, 1.5);
        }
    }
    cairo_fill(cr);

    /* Top & Bottom translucent bars */
    cairo_set_source_rgba(cr, 0.067, 0.067, 0.106, 0.78);
    cairo_rectangle(cr, 0, 0, w, 44);
    cairo_rectangle(cr, 0, h - 42, w, 42);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 0.271, 0.278, 0.353, 0.55);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, 44.5);
    cairo_line_to(cr, w, 44.5);
    cairo_move_to(cr, 0, h - 42.5);
    cairo_line_to(cr, w, h - 42.5);
    cairo_stroke(cr);

    /* Center Login Glass Card */
    double cw = 436.0;
    double ch = 468.0;
    double cx = (w - cw) / 2.0;
    double cy = (h - ch) / 2.0;

    /* Outer soft shadow */
    draw_rounded_rect(cr, cx - 4, cy + 6, cw + 8, ch + 8, 18.0);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.42);
    cairo_fill(cr);

    /* Card fill */
    draw_rounded_rect(cr, cx, cy, cw, ch, 16.0);
    cairo_set_source_rgba(cr, 0.118, 0.118, 0.180, 0.94);
    cairo_fill_preserve(cr);

    /* Accent top border highlight */
    if (app->is_root_selected) {
        cairo_set_source_rgba(cr, 0.953, 0.545, 0.659, 0.55);
    } else {
        cairo_set_source_rgba(cr, 0.537, 0.706, 0.980, 0.45);
    }
    cairo_set_line_width(cr, 1.3);
    cairo_stroke(cr);

    cairo_destroy(cr);
    return FALSE;
}

static gboolean on_avatar_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
    (void)event;
    LoginApp *app = (LoginApp *)data;
    cairo_t *cr = gdk_cairo_create(widget->window);
    int w = widget->allocation.width;
    int h = widget->allocation.height;
    double cx = w / 2.0;
    double cy = h / 2.0;
    double r = (w < h ? w : h) / 2.0 - 4.0;

    /* Outer glow ring */
    cairo_arc(cr, cx, cy, r + 2.5, 0, 2.0 * M_PI);
    if (app->is_root_selected) {
        cairo_set_source_rgba(cr, 0.953, 0.545, 0.659, 0.35);
    } else {
        cairo_set_source_rgba(cr, 0.537, 0.706, 0.980, 0.35);
    }
    cairo_set_line_width(cr, 3.0);
    cairo_stroke(cr);

    /* Avatar disc gradient */
    cairo_pattern_t *pat = cairo_pattern_create_linear(cx - r, cy - r, cx + r, cy + r);
    if (app->is_root_selected) {
        cairo_pattern_add_color_stop_rgb(pat, 0.0, 0.953, 0.545, 0.659);
        cairo_pattern_add_color_stop_rgb(pat, 1.0, 0.980, 0.702, 0.529);
    } else {
        cairo_pattern_add_color_stop_rgb(pat, 0.0, 0.537, 0.706, 0.980);
        cairo_pattern_add_color_stop_rgb(pat, 1.0, 0.796, 0.651, 0.969);
    }
    cairo_arc(cr, cx, cy, r, 0, 2.0 * M_PI);
    cairo_set_source(cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);

    /* Minimal user silhouette or initial */
    const char *uname = gtk_entry_get_text(GTK_ENTRY(app->user_entry));
    char ch[2] = { 'P', '\0' };
    if (uname && uname[0]) {
        ch[0] = (char)toupper((unsigned char)uname[0]);
    }

    cairo_set_source_rgb(cr, 0.067, 0.067, 0.106);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 34.0);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, ch, &ext);
    cairo_move_to(cr, cx - ext.width / 2.0 - ext.x_bearing, cy - ext.height / 2.0 - ext.y_bearing);
    cairo_show_text(cr, ch);

    cairo_destroy(cr);
    return TRUE;
}

static void style_button(GtkWidget *btn, const char *bg_hex, const char *fg_hex, const char *hover_hex) {
    GdkColor bg, fg, hover;
    if (gdk_color_parse(bg_hex, &bg)) {
        gtk_widget_modify_bg(btn, GTK_STATE_NORMAL, &bg);
        gtk_widget_modify_bg(btn, GTK_STATE_INSENSITIVE, &bg);
    }
    if (gdk_color_parse(hover_hex, &hover)) {
        gtk_widget_modify_bg(btn, GTK_STATE_PRELIGHT, &hover);
        gtk_widget_modify_bg(btn, GTK_STATE_ACTIVE, &hover);
    }
    if (gdk_color_parse(fg_hex, &fg)) {
        gtk_widget_modify_fg(btn, GTK_STATE_NORMAL, &fg);
        gtk_widget_modify_fg(btn, GTK_STATE_PRELIGHT, &fg);
        gtk_widget_modify_fg(btn, GTK_STATE_ACTIVE, &fg);
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(btn));
        if (child) {
            gtk_widget_modify_fg(child, GTK_STATE_NORMAL, &fg);
            gtk_widget_modify_fg(child, GTK_STATE_PRELIGHT, &fg);
            gtk_widget_modify_fg(child, GTK_STATE_ACTIVE, &fg);
        }
    }
}

static void style_entry(GtkWidget *entry) {
    GdkColor base, text, bg;
    gdk_color_parse("#11111b", &base);
    gdk_color_parse("#cdd6f4", &text);
    gdk_color_parse("#313244", &bg);
    gtk_widget_modify_base(entry, GTK_STATE_NORMAL, &base);
    gtk_widget_modify_text(entry, GTK_STATE_NORMAL, &text);
    gtk_widget_modify_bg(entry, GTK_STATE_NORMAL, &bg);
    gtk_widget_modify_bg(entry, GTK_STATE_SELECTED, &bg);
}

static void refresh_user_visuals(LoginApp *app) {
    const char *uname = gtk_entry_get_text(GTK_ENTRY(app->user_entry));
    app->is_root_selected = (uname && strcmp(uname, "root") == 0);

    if (app->is_root_selected) {
        gtk_label_set_markup(GTK_LABEL(app->role_label),
            "<span foreground='#f38ba8' weight='bold'>root</span>"
            "<span foreground='#a6adc8'>  •  Administrator (UID 0)</span>");
        style_button(app->btn_user_phantom, "#181825", "#a6adc8", "#313244");
        style_button(app->btn_user_root, "#f38ba8", "#11111b", "#eba0ac");
        style_button(app->login_btn, "#f38ba8", "#11111b", "#eba0ac");
    } else {
        char esc[128];
        snprintf(esc, sizeof(esc), "%s", (uname && uname[0]) ? uname : "phantom");
        char *markup = g_markup_printf_escaped(
            "<span foreground='#89b4fa' weight='bold'>%s</span>"
            "<span foreground='#a6adc8'>  •  Standard User (sudo)</span>",
            esc);
        gtk_label_set_markup(GTK_LABEL(app->role_label), markup);
        g_free(markup);
        style_button(app->btn_user_phantom, "#89b4fa", "#11111b", "#b4befe");
        style_button(app->btn_user_root, "#181825", "#a6adc8", "#313244");
        style_button(app->login_btn, "#89b4fa", "#11111b", "#b4befe");
    }

    gtk_widget_queue_draw(app->avatar_area);
    gtk_widget_queue_draw(app->window);
}

static gboolean on_clock_tick(gpointer data) {
    LoginApp *app = (LoginApp *)data;
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char tbuf[96];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S  |  %a %d/%m/%Y", &tm_now);
    char *markup = g_markup_printf_escaped(
        "<span foreground='#cdd6f4' weight='bold'>%s</span>", tbuf);
    gtk_label_set_markup(GTK_LABEL(app->clock_label), markup);
    g_free(markup);

    char mem[64];
    read_memory_summary(mem, sizeof(mem));
    char *bmarkup = g_markup_printf_escaped(
        "<span foreground='#9399b2'>RAM: </span><span foreground='#a6e3a1'>%s</span>"
        "<span foreground='#585b70'>   •   </span>"
        "<span foreground='#9399b2'>Mode: </span><span foreground='#cba6f7'>%s</span>",
        mem, app->root_mode);
    gtk_label_set_markup(GTK_LABEL(app->bottom_info_label), bmarkup);
    g_free(bmarkup);

    return TRUE;
}

static void on_select_phantom(GtkButton *btn, gpointer data) {
    (void)btn;
    LoginApp *app = (LoginApp *)data;
    gtk_entry_set_text(GTK_ENTRY(app->user_entry), "phantom");
    gtk_entry_set_text(GTK_ENTRY(app->pass_entry), "");
    gtk_label_set_markup(GTK_LABEL(app->status_label), "");
    refresh_user_visuals(app);
    gtk_widget_grab_focus(app->pass_entry);
}

static void on_select_root(GtkButton *btn, gpointer data) {
    (void)btn;
    LoginApp *app = (LoginApp *)data;
    gtk_entry_set_text(GTK_ENTRY(app->user_entry), "root");
    gtk_entry_set_text(GTK_ENTRY(app->pass_entry), "");
    gtk_label_set_markup(GTK_LABEL(app->status_label), "");
    refresh_user_visuals(app);
    gtk_widget_grab_focus(app->pass_entry);
}

static void on_user_entry_changed(GtkEditable *editable, gpointer data) {
    (void)editable;
    refresh_user_visuals((LoginApp *)data);
}

static void on_user_activate(GtkEntry *entry, gpointer data) {
    (void)entry;
    LoginApp *app = (LoginApp *)data;
    gtk_widget_grab_focus(app->pass_entry);
}

static void on_toggle_password(GtkButton *btn, gpointer data) {
    (void)btn;
    LoginApp *app = (LoginApp *)data;
    app->show_password = !app->show_password;
    gtk_entry_set_visibility(GTK_ENTRY(app->pass_entry), app->show_password);
    gtk_button_set_label(GTK_BUTTON(app->toggle_pass_btn), app->show_password ? "Hide" : "Show");
    gtk_widget_grab_focus(app->pass_entry);
}

static gboolean on_unlock_auth(gpointer data) {
    LoginApp *app = (LoginApp *)data;
    app->auth_locked = FALSE;
    gtk_widget_set_sensitive(app->login_btn, TRUE);
    gtk_widget_set_sensitive(app->pass_entry, TRUE);
    gtk_widget_grab_focus(app->pass_entry);
    return FALSE;
}

static void set_error_status(LoginApp *app, const char *msg) {
    char *markup = g_markup_printf_escaped(
        "<span foreground='#f38ba8' weight='bold'>%s</span>", msg);
    gtk_label_set_markup(GTK_LABEL(app->status_label), markup);
    g_free(markup);
}

static gboolean verify_credentials(const char *username, const char *password, struct passwd **out_pw) {
    if (!username || !username[0] || !password) return FALSE;

    struct passwd *pw = getpwnam(username);
    if (!pw) return FALSE;

    struct spwd *sp = getspnam(username);
    const char *hash = (sp && sp->sp_pwdp) ? sp->sp_pwdp : pw->pw_passwd;
    if (!hash || hash[0] == '\0' || hash[0] == '!' || hash[0] == '*') {
        return FALSE;
    }

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

static gboolean focus_password_idle(gpointer data) {
    LoginApp *app = (LoginApp *)data;
    if (app->window && app->window->window) {
        Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
        Window xwin = GDK_WINDOW_XID(app->window->window);
        XRaiseWindow(dpy, xwin);
        XSetInputFocus(dpy, xwin, RevertToPointerRoot, CurrentTime);
        XFlush(dpy);
    }
    gtk_window_present(GTK_WINDOW(app->window));
    gtk_widget_grab_focus(app->pass_entry);
    return FALSE;
}

static void on_session_exit(GPid pid, gint status, gpointer data) {
    (void)status;
    LoginApp *app = (LoginApp *)data;

    /* Clean up remaining processes inside the ended user session */
    if (pid > 0) {
        kill(-pid, SIGTERM);
        usleep(150000);
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, WNOHANG);
    }
    g_spawn_close_pid(pid);
    app->session_pid = -1;

    /* Revoke temporary X11 access for non-root session */
    if (app->active_x_user[0] && strcmp(app->active_x_user, "root") != 0) {
        grant_x11_localuser(app->active_x_user, FALSE);
    }
    app->active_x_user[0] = '\0';

    /* Restore polished login screen */
    gtk_entry_set_text(GTK_ENTRY(app->pass_entry), "");
    gtk_label_set_markup(GTK_LABEL(app->status_label),
        "<span foreground='#a6e3a1'>Session closed. Ready to sign in.</span>");
    gtk_widget_set_sensitive(app->login_btn, TRUE);
    gtk_widget_set_sensitive(app->pass_entry, TRUE);
    gtk_widget_show_all(app->window);
    g_idle_add(focus_password_idle, app);
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

    /* Ensure the login UI hides before starting the window manager */
    gtk_widget_hide(app->window);
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }
    gdk_flush();

    /* Capture fields before fork */
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;
    char uname[64], uhome[256], ushell[128];
    snprintf(uname, sizeof(uname), "%s", pw->pw_name);
    snprintf(uhome, sizeof(uhome), "%s", (pw->pw_dir && pw->pw_dir[0]) ? pw->pw_dir : "/");
    snprintf(ushell, sizeof(ushell), "%s", (pw->pw_shell && pw->pw_shell[0]) ? pw->pw_shell : "/bin/sh");

    pid_t pid = fork();
    if (pid < 0) {
        grant_x11_localuser(uname, FALSE);
        gtk_widget_show_all(app->window);
        set_error_status(app, "Failed to fork session process.");
        return;
    }

    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);

        setsid();

        if (setgid(gid) != 0 || initgroups(uname, gid) != 0 || setuid(uid) != 0) {
            _exit(126);
        }

        if (chdir(uhome) != 0) {
            if (chdir("/") != 0) {
                _exit(126);
            }
        }

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

static void on_attempt_login(GtkWidget *widget, gpointer data) {
    (void)widget;
    LoginApp *app = (LoginApp *)data;
    if (app->auth_locked) return;

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
        set_error_status(app, "Invalid username or password. Try again.");
        app->auth_locked = TRUE;
        gtk_widget_set_sensitive(app->login_btn, FALSE);
        gtk_widget_set_sensitive(app->pass_entry, FALSE);
        g_timeout_add(1000, on_unlock_auth, app);
        return;
    }

    secure_wipe(password, strlen(password));
    g_free(password);

    int active_session = gtk_combo_box_get_active(GTK_COMBO_BOX(app->session_combo));
    const char *session_mode = (active_session == 1) ? "terminal" : "desktop";

    gtk_label_set_markup(GTK_LABEL(app->status_label), "");
    launch_user_session(app, pw, session_mode);
}

static void confirm_power_action(LoginApp *app, const char *title, const char *question, const char *bin_path) {
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_YES_NO,
        "%s", question);
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (resp == GTK_RESPONSE_YES) {
        sync();
        execl(bin_path, bin_path, (char *)NULL);
    }
}

static void on_reboot_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    confirm_power_action((LoginApp *)data, "Reboot PhantomOS",
                         "Reboot PhantomOS now? Unsaved RAM changes will reset.", "/sbin/reboot");
}

static void on_poweroff_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    confirm_power_action((LoginApp *)data, "Power Off PhantomOS",
                         "Power off the system now? Unsaved RAM changes will reset.", "/sbin/poweroff");
}

static void setup_dark_rc(void) {
    const char *rc =
        "style \"phantom-login-dark\" {\n"
        "  bg[NORMAL]   = \"#1e1e2e\"\n"
        "  bg[PRELIGHT] = \"#313244\"\n"
        "  bg[ACTIVE]   = \"#45475a\"\n"
        "  bg[SELECTED] = \"#45475a\"\n"
        "  fg[NORMAL]   = \"#cdd6f4\"\n"
        "  fg[PRELIGHT] = \"#cdd6f4\"\n"
        "  fg[ACTIVE]   = \"#cdd6f4\"\n"
        "  fg[SELECTED] = \"#ffffff\"\n"
        "  base[NORMAL] = \"#11111b\"\n"
        "  text[NORMAL] = \"#cdd6f4\"\n"
        "}\n"
        "class \"GtkWidget\" style \"phantom-login-dark\"\n";
    gtk_rc_parse_string(rc);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    setup_dark_rc();

    LoginApp app;
    memset(&app, 0, sizeof(app));
    app.session_pid = -1;
    read_system_metadata(&app);

    GdkScreen *screen = gdk_screen_get_default();
    int scr_w = gdk_screen_get_width(screen);
    int scr_h = gdk_screen_get_height(screen);
    if (scr_w < 800) scr_w = 1280;
    if (scr_h < 600) scr_h = 800;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), PHANTOM_LOGIN_TITLE);
    gtk_window_set_decorated(GTK_WINDOW(app.window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(app.window), scr_w, scr_h);
    gtk_window_move(GTK_WINDOW(app.window), 0, 0);
    gtk_widget_set_size_request(app.window, scr_w, scr_h);
    gtk_widget_set_app_paintable(app.window, TRUE);
    g_signal_connect(app.window, "expose-event", G_CALLBACK(on_window_expose), &app);

    GtkWidget *root_vbox = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app.window), root_vbox);

    /* Top Bar (44px) */
    GtkWidget *top_hbox = gtk_hbox_new(FALSE, 12);
    gtk_widget_set_size_request(top_hbox, -1, 44);
    gtk_container_set_border_width(GTK_CONTAINER(top_hbox), 10);

    app.top_title_label = gtk_label_new(NULL);
    char *tmarkup = g_markup_printf_escaped(
        "<span foreground='#89b4fa' weight='bold' size='large'>PhantomOS</span>"
        "<span foreground='#585b70'>  •  </span>"
        "<span foreground='#a6adc8'>%s</span>"
        "<span foreground='#585b70'>  •  </span>"
        "<span foreground='#9399b2'>Kernel %s</span>"
        "<span foreground='#585b70'>  •  </span>"
        "<span foreground='#a6e3a1'>eth0: %s</span>",
        app.hostname, app.kernel_ver, app.ip_addr);
    gtk_label_set_markup(GTK_LABEL(app.top_title_label), tmarkup);
    g_free(tmarkup);
    gtk_box_pack_start(GTK_BOX(top_hbox), app.top_title_label, FALSE, FALSE, 8);

    app.clock_label = gtk_label_new(NULL);
    gtk_box_pack_end(GTK_BOX(top_hbox), app.clock_label, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(root_vbox), top_hbox, FALSE, FALSE, 0);

    /* Centering area for Login Card */
    GtkWidget *center_align = gtk_alignment_new(0.5f, 0.5f, 0.0f, 0.0f);
    gtk_box_pack_start(GTK_BOX(root_vbox), center_align, TRUE, TRUE, 0);

    GtkWidget *card_vbox = gtk_vbox_new(FALSE, 10);
    gtk_widget_set_size_request(card_vbox, 384, -1);
    gtk_container_set_border_width(GTK_CONTAINER(card_vbox), 20);
    gtk_container_add(GTK_CONTAINER(center_align), card_vbox);

    /* Avatar */
    GtkWidget *av_align = gtk_alignment_new(0.5f, 0.5f, 0.0f, 0.0f);
    app.avatar_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.avatar_area, 86, 86);
    g_signal_connect(app.avatar_area, "expose-event", G_CALLBACK(on_avatar_expose), &app);
    gtk_container_add(GTK_CONTAINER(av_align), app.avatar_area);
    gtk_box_pack_start(GTK_BOX(card_vbox), av_align, FALSE, FALSE, 2);

    app.role_label = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(card_vbox), app.role_label, FALSE, FALSE, 2);

    /* Account quick switcher */
    GtkWidget *switch_hbox = gtk_hbox_new(TRUE, 8);
    app.btn_user_phantom = gtk_button_new_with_label("phantom");
    app.btn_user_root = gtk_button_new_with_label("root");
    gtk_widget_set_size_request(app.btn_user_phantom, -1, 30);
    gtk_widget_set_size_request(app.btn_user_root, -1, 30);
    g_signal_connect(app.btn_user_phantom, "clicked", G_CALLBACK(on_select_phantom), &app);
    g_signal_connect(app.btn_user_root, "clicked", G_CALLBACK(on_select_root), &app);
    gtk_box_pack_start(GTK_BOX(switch_hbox), app.btn_user_phantom, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(switch_hbox), app.btn_user_root, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(card_vbox), switch_hbox, FALSE, FALSE, 4);

    /* Username */
    GtkWidget *user_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(user_lbl), "<span foreground='#bac2de' weight='bold'>Username</span>");
    gtk_misc_set_alignment(GTK_MISC(user_lbl), 0.0f, 0.5f);
    gtk_box_pack_start(GTK_BOX(card_vbox), user_lbl, FALSE, FALSE, 0);

    app.user_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(app.user_entry), "phantom");
    gtk_widget_set_size_request(app.user_entry, -1, 34);
    style_entry(app.user_entry);
    g_signal_connect(app.user_entry, "changed", G_CALLBACK(on_user_entry_changed), &app);
    g_signal_connect(app.user_entry, "activate", G_CALLBACK(on_user_activate), &app);
    gtk_box_pack_start(GTK_BOX(card_vbox), app.user_entry, FALSE, FALSE, 0);

    /* Password + toggle */
    GtkWidget *pass_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(pass_lbl), "<span foreground='#bac2de' weight='bold'>Password</span>");
    gtk_misc_set_alignment(GTK_MISC(pass_lbl), 0.0f, 0.5f);
    gtk_box_pack_start(GTK_BOX(card_vbox), pass_lbl, FALSE, FALSE, 0);

    GtkWidget *pass_hbox = gtk_hbox_new(FALSE, 6);
    app.pass_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(app.pass_entry), FALSE);
    gtk_entry_set_invisible_char(GTK_ENTRY(app.pass_entry), 0x2022);
    gtk_widget_set_size_request(app.pass_entry, -1, 34);
    style_entry(app.pass_entry);
    g_signal_connect(app.pass_entry, "activate", G_CALLBACK(on_attempt_login), &app);

    app.toggle_pass_btn = gtk_button_new_with_label("Show");
    gtk_widget_set_size_request(app.toggle_pass_btn, 62, 34);
    style_button(app.toggle_pass_btn, "#313244", "#cdd6f4", "#45475a");
    g_signal_connect(app.toggle_pass_btn, "clicked", G_CALLBACK(on_toggle_password), &app);

    gtk_box_pack_start(GTK_BOX(pass_hbox), app.pass_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(pass_hbox), app.toggle_pass_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_vbox), pass_hbox, FALSE, FALSE, 0);

    /* Session selector */
    GtkWidget *sess_hbox = gtk_hbox_new(FALSE, 8);
    GtkWidget *sess_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(sess_lbl), "<span foreground='#bac2de'>Session:</span>");
    app.session_combo = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(app.session_combo), "Fluxbox Desktop (Full GUI)");
    gtk_combo_box_append_text(GTK_COMBO_BOX(app.session_combo), "Terminal Console (Minimal)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.session_combo), 0);
    gtk_box_pack_start(GTK_BOX(sess_hbox), sess_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sess_hbox), app.session_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(card_vbox), sess_hbox, FALSE, FALSE, 4);

    /* Sign In CTA */
    app.login_btn = gtk_button_new_with_label("Sign In");
    gtk_widget_set_size_request(app.login_btn, -1, 38);
    g_signal_connect(app.login_btn, "clicked", G_CALLBACK(on_attempt_login), &app);
    gtk_box_pack_start(GTK_BOX(card_vbox), app.login_btn, FALSE, FALSE, 4);

    app.status_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(app.status_label),
        "<span foreground='#9399b2'>Default lab login: phantom / phantom</span>");
    gtk_box_pack_start(GTK_BOX(card_vbox), app.status_label, FALSE, FALSE, 2);

    /* Bottom Bar (42px) */
    GtkWidget *bot_hbox = gtk_hbox_new(FALSE, 10);
    gtk_widget_set_size_request(bot_hbox, -1, 42);
    gtk_container_set_border_width(GTK_CONTAINER(bot_hbox), 8);

    app.bottom_info_label = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(bot_hbox), app.bottom_info_label, FALSE, FALSE, 8);

    app.poweroff_btn = gtk_button_new_with_label("Power Off");
    app.reboot_btn = gtk_button_new_with_label("Reboot");
    style_button(app.poweroff_btn, "#313244", "#f38ba8", "#45475a");
    style_button(app.reboot_btn, "#313244", "#fab387", "#45475a");
    g_signal_connect(app.poweroff_btn, "clicked", G_CALLBACK(on_poweroff_clicked), &app);
    g_signal_connect(app.reboot_btn, "clicked", G_CALLBACK(on_reboot_clicked), &app);
    gtk_box_pack_end(GTK_BOX(bot_hbox), app.poweroff_btn, FALSE, FALSE, 4);
    gtk_box_pack_end(GTK_BOX(bot_hbox), app.reboot_btn, FALSE, FALSE, 4);
    gtk_box_pack_end(GTK_BOX(root_vbox), bot_hbox, FALSE, FALSE, 0);

    refresh_user_visuals(&app);
    on_clock_tick(&app);
    g_timeout_add(1000, on_clock_tick, &app);

    gtk_widget_show_all(app.window);
    g_idle_add(focus_password_idle, &app);

    gtk_main();
    return 0;
}
