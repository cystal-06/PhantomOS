#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <cairo.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <ifaddrs.h>
#include <linux/rfkill.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#define PANEL_HEIGHT 28
#define HIST_LEN     28
#define MAX_WIFI_NETS 16

typedef struct {
    char ssid[64];
    int signal_dbm;
    int signal_pct;
    int secured;
    int connected;
} WifiNet;

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

    /* RAM, ZRAM Swap & Disk */
    double ram_pct;
    double ram_used_mb;
    double ram_total_mb;
    double ram_avail_mb;
    double ram_cache_mb;
    double swap_pct;
    double swap_used_mb;
    double swap_total_mb;
    double zram_orig_mb;
    double zram_compr_mb;
    double zram_mem_mb;
    char zram_algo[16];
    double ram_hist[HIST_LEN];
    double swap_hist[HIST_LEN];
    double disk_pct;
    double disk_used_mb;
    double disk_total_mb;

    /* Memory / ZRAM Popup */
    GtkWidget *ram_popup;
    GtkWidget *ram_popup_area;
    Rect r_popup_mon;
    double popup_hx;
    double popup_hy;
    int popup_hover;
    gint64 popup_hidden_us;

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

    /* Battery / Power */
    int has_bat;
    int bat_pct;
    int bat_charging;
    int ac_online;
    char bat_status[32];

    /* Backlight / Display */
    int has_hw_bl;
    char bl_dir[128];
    int bl_max;
    int bright_pct;
    int bright_init;
    int night_light;

    /* Wi-Fi, LAN & RFKILL */
    int has_wifi;
    char wifi_iface[32];
    int wifi_up;
    int wifi_connected;
    int wifi_signal_pct;
    char wifi_ssid[64];
    char eth_ip[32];
    int has_rfkill;
    int airplane_mode;
    WifiNet wifi_nets[MAX_WIFI_NETS];
    int wifi_net_count;
    int wifi_scanning;

    /* Quick Settings Popup */
    GtkWidget *qs_popup;
    GtkWidget *qs_vbox;
    GtkWidget *qs_area;
    GtkWidget *qs_pass_box;
    GtkWidget *qs_pass_label;
    GtkWidget *qs_pass_entry;
    int qs_page; /* 0 = main, 1 = wifi list, 2 = wifi password */
    int qs_power_confirm;
    char qs_target_ssid[64];
    char qs_toast[96];
    gint64 qs_toast_until_us;
    int dragging_slider; /* 0 = none, 1 = vol, 2 = bright */
    double qs_hx;
    double qs_hy;
    int qs_hover;
    gint64 qs_hidden_us;
    gint64 last_slider_cmd_us;

    Rect r_qs_bat_pill;
    Rect r_qs_shot;
    Rect r_qs_mon;
    Rect r_qs_lock;
    Rect r_qs_pwr;
    Rect r_qs_logout;
    Rect r_qs_reboot;
    Rect r_qs_poweroff;
    Rect r_qs_pwr_cancel;
    Rect r_qs_vol_ico;
    Rect r_qs_vol_trk;
    Rect r_qs_bright_trk;
    Rect r_qs_wifi_toggle;
    Rect r_qs_wifi_arrow;
    Rect r_qs_lan;
    Rect r_qs_night;
    Rect r_qs_plane;
    Rect r_qs_back;
    Rect r_qs_scan;
    Rect r_qs_wifi_rows[6];

    /* Workspace */
    long cur_ws;
    long num_ws;

    /* Clock */
    char clock_str[32];

    /* Hover & Hitboxes */
    double hover_x;
    double hover_y;
    int hovering;
    Rect r_brand;
    Rect r_ws;
    double qs_anim_t;
    double ram_anim_t;
    guint anim_id;
    Rect r_term;
    Rect r_browser;
    Rect r_files;
    Rect r_cpu;
    Rect r_ram;
    Rect r_disk;
    Rect r_net;
    Rect r_clk;
    Rect r_qs;
    Rect r_qs_bar_vol;
    Rect r_qs_bar_bright;
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
        unsigned long swap_tot=0, swap_free=0;
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
            else if (strcmp(k, "SwapTotal:") == 0) swap_tot = v;
            else if (strcmp(k, "SwapFree:") == 0) swap_free = v;
        }
        fclose(fp);
        if (!has_avail) avail = fre + buf + cac + sre;
        unsigned long reclaim = fre + buf + cac + sre;
        unsigned long used = (tot > reclaim) ? (tot - reclaim) : 0;
        app->ram_total_mb = (double)tot / 1024.0;
        app->ram_used_mb = (double)used / 1024.0;
        app->ram_avail_mb = (double)avail / 1024.0;
        app->ram_cache_mb = (double)(buf + cac + sre) / 1024.0;
        app->ram_pct = (tot > 0) ? (100.0 * (double)used / (double)tot) : 0.0;
        push_hist(app->ram_hist, app->ram_pct);

        unsigned long swap_used = (swap_tot > swap_free) ? (swap_tot - swap_free) : 0;
        app->swap_total_mb = (double)swap_tot / 1024.0;
        app->swap_used_mb = (double)swap_used / 1024.0;
        app->swap_pct = (swap_tot > 0) ? (100.0 * (double)swap_used / (double)swap_tot) : 0.0;
        push_hist(app->swap_hist, app->swap_pct);
    }

    app->zram_orig_mb = 0.0;
    app->zram_compr_mb = 0.0;
    app->zram_mem_mb = 0.0;
    app->zram_algo[0] = '\0';
    FILE *zm = fopen("/sys/block/zram0/mm_stat", "r");
    if (zm) {
        unsigned long long orig_b = 0, compr_b = 0, mem_b = 0;
        if (fscanf(zm, "%llu %llu %llu", &orig_b, &compr_b, &mem_b) == 3) {
            app->zram_orig_mb = (double)orig_b / (1024.0 * 1024.0);
            app->zram_compr_mb = (double)compr_b / (1024.0 * 1024.0);
            app->zram_mem_mb = (double)mem_b / (1024.0 * 1024.0);
        }
        fclose(zm);
    }
    FILE *za = fopen("/sys/block/zram0/comp_algorithm", "r");
    if (za) {
        char buf[128];
        if (fgets(buf, sizeof(buf), za)) {
            char *lb = strchr(buf, '[');
            char *rb = lb ? strchr(lb + 1, ']') : NULL;
            if (lb && rb && (size_t)(rb - lb - 1) < sizeof(app->zram_algo)) {
                size_t len = (size_t)(rb - lb - 1);
                memcpy(app->zram_algo, lb + 1, len);
                app->zram_algo[len] = '\0';
            }
        }
        fclose(za);
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

static int read_sys_str(const char *path, char *buf, size_t n) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    int ok = (fgets(buf, (int)n, fp) != NULL);
    fclose(fp);
    if (ok) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
            buf[--len] = '\0';
        }
    }
    return ok;
}

static int read_sys_int(const char *path, int *out) {
    char buf[64];
    if (!read_sys_str(path, buf, sizeof(buf))) return 0;
    *out = atoi(buf);
    return 1;
}

static void sample_battery(PanelApp *app) {
    app->has_bat = 0;
    app->bat_charging = 0;
    app->ac_online = 0;
    app->bat_status[0] = '\0';

    DIR *dir = opendir("/sys/class/power_supply");
    if (!dir) return;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char p_type[384], p_val[384], typ[64];
        snprintf(p_type, sizeof(p_type), "/sys/class/power_supply/%s/type", de->d_name);
        if (!read_sys_str(p_type, typ, sizeof(typ))) continue;
        if (strcmp(typ, "Battery") == 0 && !app->has_bat) {
            int cap = 0;
            snprintf(p_val, sizeof(p_val), "/sys/class/power_supply/%s/capacity", de->d_name);
            if (read_sys_int(p_val, &cap)) {
                app->has_bat = 1;
                if (cap < 0) cap = 0;
                if (cap > 100) cap = 100;
                app->bat_pct = cap;
                snprintf(p_val, sizeof(p_val), "/sys/class/power_supply/%s/status", de->d_name);
                if (read_sys_str(p_val, app->bat_status, sizeof(app->bat_status))) {
                    if (strcmp(app->bat_status, "Charging") == 0 ||
                        strcmp(app->bat_status, "Full") == 0) {
                        app->bat_charging = 1;
                    }
                }
            }
        } else if (strcmp(typ, "Mains") == 0) {
            int online = 0;
            snprintf(p_val, sizeof(p_val), "/sys/class/power_supply/%s/online", de->d_name);
            if (read_sys_int(p_val, &online) && online > 0) {
                app->ac_online = 1;
            }
        }
    }
    closedir(dir);
    if (app->has_bat && app->ac_online && strcmp(app->bat_status, "Not charging") == 0) {
        app->bat_charging = 1;
    }
}

static void sample_backlight(PanelApp *app) {
    if (!app->bright_init) {
        app->bright_pct = 100;
        app->bright_init = 1;
    }
    DIR *dir = opendir("/sys/class/backlight");
    if (!dir) {
        app->has_hw_bl = 0;
        return;
    }
    struct dirent *de;
    app->has_hw_bl = 0;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char p_cur[384], p_max[384];
        int cur = 0, mx = 0;
        snprintf(p_max, sizeof(p_max), "/sys/class/backlight/%s/max_brightness", de->d_name);
        snprintf(p_cur, sizeof(p_cur), "/sys/class/backlight/%s/brightness", de->d_name);
        if (read_sys_int(p_max, &mx) && mx > 0 && read_sys_int(p_cur, &cur)) {
            app->has_hw_bl = 1;
            g_strlcpy(app->bl_dir, de->d_name, sizeof(app->bl_dir));
            app->bl_max = mx;
            if (!app->dragging_slider) {
                int pct = (int)((100.0 * (double)cur / (double)mx) + 0.5);
                if (pct < 10) pct = 10;
                if (pct > 100) pct = 100;
                app->bright_pct = pct;
            }
            break;
        }
    }
    closedir(dir);
}

static void apply_display_settings(PanelApp *app) {
    if (app->bright_pct < 15) app->bright_pct = 15;
    if (app->bright_pct > 100) app->bright_pct = 100;

    if (app->has_hw_bl && app->bl_max > 0 && app->bl_dir[0]) {
        char p_cur[384];
        snprintf(p_cur, sizeof(p_cur), "/sys/class/backlight/%s/brightness", app->bl_dir);
        int val = (app->bl_max * app->bright_pct) / 100;
        int min_v = app->bl_max / 20;
        if (min_v < 1) min_v = 1;
        if (val < min_v) val = min_v;
        FILE *fp = fopen(p_cur, "w");
        if (fp) {
            fprintf(fp, "%d\n", val);
            fclose(fp);
        }
    }

    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (!dpy) return;
    Window root = DefaultRootWindow(dpy);
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) return;

    double lum = app->has_hw_bl ? 1.0 : (0.25 + 0.75 * ((double)app->bright_pct / 100.0));
    double mr = lum;
    double mg = lum * (app->night_light ? 0.84 : 1.0);
    double mb = lum * (app->night_light ? 0.65 : 1.0);

    for (int i = 0; i < res->ncrtc; i++) {
        int sz = XRRGetCrtcGammaSize(dpy, res->crtcs[i]);
        if (sz <= 1) continue;
        XRRCrtcGamma *g = XRRAllocGamma(sz);
        if (!g) continue;
        for (int j = 0; j < sz; j++) {
            double base = (65535.0 * (double)j) / (double)(sz - 1);
            g->red[j]   = (unsigned short)(base * mr);
            g->green[j] = (unsigned short)(base * mg);
            g->blue[j]  = (unsigned short)(base * mb);
        }
        XRRSetCrtcGamma(dpy, res->crtcs[i], g);
        XRRFreeGamma(g);
    }
    XRRFreeScreenResources(res);
    XFlush(dpy);
}

static void sample_rfkill(PanelApp *app) {
    DIR *dir = opendir("/sys/class/rfkill");
    if (!dir) {
        app->has_rfkill = 0;
        return;
    }
    struct dirent *de;
    int found = 0, blocked = 1;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "rfkill", 6) != 0) continue;
        char p_soft[384];
        int soft = 0;
        snprintf(p_soft, sizeof(p_soft), "/sys/class/rfkill/%s/soft", de->d_name);
        if (read_sys_int(p_soft, &soft)) {
            found = 1;
            if (soft == 0) blocked = 0;
        }
    }
    closedir(dir);
    app->has_rfkill = found;
    if (found) app->airplane_mode = blocked;
}

static void set_airplane_mode(PanelApp *app, int block) {
    int fd = open("/dev/rfkill", O_WRONLY);
    if (fd >= 0) {
        struct rfkill_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = RFKILL_TYPE_ALL;
        ev.op = RFKILL_OP_CHANGE_ALL;
        ev.soft = block ? 1 : 0;
        ssize_t wr = write(fd, &ev, sizeof(ev));
        (void)wr;
        close(fd);
    }
    app->airplane_mode = block ? 1 : 0;
    if (app->has_wifi && app->wifi_iface[0]) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "sudo -n /usr/libexec/phantom-net %s %s",
                 block ? "wifi-down" : "wifi-up", app->wifi_iface);
        spawn_cmd(cmd);
    }
}

static void sample_wifi_and_lan(PanelApp *app) {
    /* Ethernet (eth0) IPv4 via getifaddrs */
    app->eth_ip[0] = '\0';
    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) == 0) {
        for (struct ifaddrs *ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (strcmp(ifa->ifa_name, "eth0") == 0) {
                struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
                inet_ntop(AF_INET, &sa->sin_addr, app->eth_ip, sizeof(app->eth_ip));
                break;
            }
        }
        freeifaddrs(ifap);
    }

    /* Detect first wireless interface */
    app->has_wifi = 0;
    app->wifi_up = 0;
    app->wifi_connected = 0;
    app->wifi_signal_pct = 0;
    app->wifi_iface[0] = '\0';
    app->wifi_ssid[0] = '\0';

    DIR *dir = opendir("/sys/class/net");
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char p_w[384];
            snprintf(p_w, sizeof(p_w), "/sys/class/net/%s/wireless", de->d_name);
            if (access(p_w, F_OK) == 0) {
                app->has_wifi = 1;
                g_strlcpy(app->wifi_iface, de->d_name, sizeof(app->wifi_iface));
                char p_flg[384], flg_s[32];
                snprintf(p_flg, sizeof(p_flg), "/sys/class/net/%s/flags", de->d_name);
                if (read_sys_str(p_flg, flg_s, sizeof(flg_s))) {
                    unsigned long flags = strtoul(flg_s, NULL, 0);
                    app->wifi_up = (flags & 1UL) ? 1 : 0; /* IFF_UP */
                }
                break;
            }
        }
        closedir(dir);
    }

    if (!app->has_wifi || !app->wifi_iface[0]) return;

    char sock[128];
    snprintf(sock, sizeof(sock), "/var/run/wpa_supplicant/%s", app->wifi_iface);
    if (access(sock, F_OK) == 0) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "wpa_cli -i %s status 2>/dev/null", app->wifi_iface);
        FILE *fp = popen(cmd, "r");
        if (fp) {
            char line[256];
            int completed = 0;
            while (fgets(line, sizeof(line), fp)) {
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
                if (strcmp(line, "wpa_state=COMPLETED") == 0) completed = 1;
                else if (strncmp(line, "ssid=", 5) == 0) {
                    g_strlcpy(app->wifi_ssid, line + 5, sizeof(app->wifi_ssid));
                }
            }
            pclose(fp);
            app->wifi_connected = (completed && app->wifi_ssid[0]) ? 1 : 0;
        }
    }

    if (app->wifi_connected) {
        FILE *wf = fopen("/proc/net/wireless", "r");
        if (wf) {
            char line[256];
            while (fgets(line, sizeof(line), wf)) {
                char *c = strchr(line, ':');
                if (!c) continue;
                *c = '\0';
                char *ifn = line;
                while (*ifn == ' ') ifn++;
                if (strcmp(ifn, app->wifi_iface) == 0) {
                    double link = 0.0, level = -70.0;
                    int status = 0;
                    if (sscanf(c + 1, "%d %lf %lf", &status, &link, &level) >= 3) {
                        int dbm = (level > 0.0) ? (int)(level - 256.0) : (int)level;
                        int pct = (dbm + 95) * 2;
                        if (pct < 10) pct = 10;
                        if (pct > 100) pct = 100;
                        app->wifi_signal_pct = pct;
                    }
                    break;
                }
            }
            fclose(wf);
        }
        if (app->wifi_signal_pct == 0) app->wifi_signal_pct = 75;
    }
}

static void read_wifi_scan_results(PanelApp *app) {
    app->wifi_net_count = 0;
    if (!app->has_wifi || !app->wifi_iface[0]) return;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wpa_cli -i %s scan_results 2>/dev/null", app->wifi_iface);
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "bssid", 5) == 0) continue;
        char bssid[64], flags[192], ssid[128];
        int freq = 0, sig = -80;
        ssid[0] = '\0';
        if (sscanf(line, "%63s %d %d %191s %127[^\n]", bssid, &freq, &sig, flags, ssid) >= 4) {
            if (!ssid[0] || strncmp(ssid, "\\x00", 4) == 0) continue;
            int dbm = (sig > 0) ? (sig - 256) : sig;
            int pct = (dbm + 95) * 2;
            if (pct < 5) pct = 5;
            if (pct > 100) pct = 100;
            int sec = (strstr(flags, "WPA") || strstr(flags, "WEP") || strstr(flags, "SAE")) ? 1 : 0;
            int dup = -1;
            for (int i = 0; i < app->wifi_net_count; i++) {
                if (strcmp(app->wifi_nets[i].ssid, ssid) == 0) {
                    dup = i;
                    break;
                }
            }
            if (dup >= 0) {
                if (dbm > app->wifi_nets[dup].signal_dbm) {
                    app->wifi_nets[dup].signal_dbm = dbm;
                    app->wifi_nets[dup].signal_pct = pct;
                }
                continue;
            }
            if (app->wifi_net_count < MAX_WIFI_NETS) {
                WifiNet *n = &app->wifi_nets[app->wifi_net_count++];
                snprintf(n->ssid, sizeof(n->ssid), "%s", ssid);
                n->signal_dbm = dbm;
                n->signal_pct = pct;
                n->secured = sec;
                n->connected = (app->wifi_connected && strcmp(app->wifi_ssid, ssid) == 0) ? 1 : 0;
            }
        }
    }
    pclose(fp);
    for (int i = 0; i < app->wifi_net_count; i++) {
        for (int j = i + 1; j < app->wifi_net_count; j++) {
            int score_i = app->wifi_nets[i].connected * 1000 + app->wifi_nets[i].signal_pct;
            int score_j = app->wifi_nets[j].connected * 1000 + app->wifi_nets[j].signal_pct;
            if (score_j > score_i) {
                WifiNet tmp = app->wifi_nets[i];
                app->wifi_nets[i] = app->wifi_nets[j];
                app->wifi_nets[j] = tmp;
            }
        }
    }
}

static gboolean on_wifi_scan_done(gpointer data) {
    PanelApp *app = (PanelApp *)data;
    app->wifi_scanning = 0;
    read_wifi_scan_results(app);
    if (app->qs_popup && GTK_WIDGET_VISIBLE(app->qs_popup)) {
        gtk_widget_queue_draw(app->qs_area);
    }
    return FALSE;
}

static void trigger_wifi_scan(PanelApp *app) {
    if (!app->has_wifi || !app->wifi_iface[0]) return;
    read_wifi_scan_results(app);
    app->wifi_scanning = 1;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wpa_cli -i %s scan >/dev/null 2>&1", app->wifi_iface);
    spawn_cmd(cmd);
    g_timeout_add(1600, on_wifi_scan_done, app);
}

static gboolean on_wifi_connect_poll(gpointer data) {
    PanelApp *app = (PanelApp *)data;
    sample_wifi_and_lan(app);
    gtk_widget_queue_draw(app->area);
    if (app->qs_popup && GTK_WIDGET_VISIBLE(app->qs_popup)) {
        gtk_widget_queue_draw(app->qs_area);
    }
    return FALSE;
}

static void connect_wifi_async(PanelApp *app, const char *ssid, const char *pass) {
    if (!app->has_wifi || !app->wifi_iface[0] || !ssid[0]) return;
    char *argv[] = {
        "/usr/bin/sudo", "-n", "/usr/libexec/phantom-net",
        "wifi-connect", app->wifi_iface, (char *)ssid, NULL
    };
    gint in_fd = -1;
    GError *err = NULL;
    if (g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                                 NULL, &in_fd, NULL, NULL, &err)) {
        if (in_fd >= 0) {
            if (pass && pass[0]) {
                ssize_t w1 = write(in_fd, pass, strlen(pass));
                (void)w1;
            }
            ssize_t w2 = write(in_fd, "\n", 1);
            (void)w2;
            close(in_fd);
        }
        snprintf(app->qs_toast, sizeof(app->qs_toast), "Connecting to %s...", ssid);
        app->qs_toast_until_us = g_get_monotonic_time() + 5000000;
        g_timeout_add(2500, on_wifi_connect_poll, app);
        g_timeout_add(5500, on_wifi_connect_poll, app);
    } else if (err) {
        g_error_free(err);
    }
}

static void sample_all(PanelApp *app) {
    sample_cpu(app);
    sample_mem_disk(app);
    sample_net(app);
    sample_audio(app);
    sample_battery(app);
    sample_backlight(app);
    sample_rfkill(app);
    sample_wifi_and_lan(app);

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

static void draw_wifi_icon(cairo_t *cr, double x, double y, int active, int connected, int pct, int white) {
    double cx = x + 8.0, cy = y + 12.5;
    if (white) cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    else if (active && connected) cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    else if (active) cairo_set_source_rgb(cr, 0.651, 0.690, 0.800);
    else cairo_set_source_rgb(cr, 0.451, 0.478, 0.580);

    cairo_arc(cr, cx, cy, 1.4, 0, 2.0 * G_PI);
    cairo_fill(cr);

    cairo_set_line_width(cr, 1.4);
    int bars = (active && connected) ? ((pct > 70) ? 3 : (pct > 40 ? 2 : 1)) : (active ? 3 : 0);
    double radii[3] = { 4.0, 6.8, 9.6 };
    for (int i = 0; i < 3; i++) {
        if (!active) cairo_set_source_rgba(cr, 0.451, 0.478, 0.580, 0.45);
        else if (i < bars) {
            if (white) cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            else cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
        } else {
            cairo_set_source_rgba(cr, 0.651, 0.690, 0.800, 0.35);
        }
        cairo_arc(cr, cx, cy, radii[i], -2.35, -0.79);
        cairo_stroke(cr);
    }
    if (!active) {
        cairo_set_source_rgb(cr, 0.953, 0.545, 0.659);
        cairo_move_to(cr, x + 2.5, y + 3.0);
        cairo_line_to(cr, x + 13.5, y + 13.5);
        cairo_stroke(cr);
    }
}

static void draw_sun_icon(cairo_t *cr, double x, double y, int warm) {
    double cx = x + 8.0, cy = y + 8.0;
    if (warm) cairo_set_source_rgb(cr, 0.976, 0.886, 0.686);
    else cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    cairo_set_line_width(cr, 1.3);
    cairo_arc(cr, cx, cy, 3.0, 0, 2.0 * G_PI);
    cairo_stroke(cr);
    for (int i = 0; i < 8; i++) {
        double a = i * (G_PI / 4.0);
        cairo_move_to(cr, cx + cos(a) * 4.7, cy + sin(a) * 4.7);
        cairo_line_to(cr, cx + cos(a) * 6.5, cy + sin(a) * 6.5);
    }
    cairo_stroke(cr);
}

static void draw_battery_icon(cairo_t *cr, double x, double y, int has_bat, int pct, int charging) {
    if (!has_bat) {
        /* Compact AC Plug / Laptop Power glyph */
        cairo_set_source_rgb(cr, 0.651, 0.890, 0.631);
        cairo_set_line_width(cr, 1.3);
        rounded_rect(cr, x + 3.5, y + 5.0, 9.0, 6.0, 2.0);
        cairo_stroke(cr);
        cairo_move_to(cr, x + 6.0, y + 2.5); cairo_line_to(cr, x + 6.0, y + 5.0);
        cairo_move_to(cr, x + 10.0, y + 2.5); cairo_line_to(cr, x + 10.0, y + 5.0);
        cairo_move_to(cr, x + 8.0, y + 11.0); cairo_line_to(cr, x + 8.0, y + 14.0);
        cairo_stroke(cr);
        return;
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    cairo_set_line_width(cr, 1.2);
    rounded_rect(cr, x + 2.0, y + 4.0, 11.0, 8.0, 1.6);
    cairo_stroke(cr);
    cairo_rectangle(cr, x + 13.0, y + 6.2, 1.8, 3.6);
    cairo_fill(cr);

    double fw = 8.4 * ((double)pct / 100.0);
    if (fw < 1.5 && pct > 0) fw = 1.5;
    if (charging) cairo_set_source_rgb(cr, 0.651, 0.890, 0.631);
    else if (pct <= 20) cairo_set_source_rgb(cr, 0.953, 0.545, 0.659);
    else cairo_set_source_rgb(cr, 0.651, 0.890, 0.631);
    cairo_rectangle(cr, x + 3.3, y + 5.3, fw, 5.4);
    cairo_fill(cr);
}

static void draw_power_glyph(cairo_t *cr, double cx, double cy, double r,
                             double red, double grn, double blu) {
    cairo_set_source_rgb(cr, red, grn, blu);
    cairo_set_line_width(cr, 1.5);
    cairo_arc(cr, cx, cy, r, -1.0, 4.14);
    cairo_stroke(cr);
    cairo_move_to(cr, cx, cy - r - 1.5);
    cairo_line_to(cr, cx, cy - 0.5);
    cairo_stroke(cr);
}

static void draw_camera_glyph(cairo_t *cr, double cx, double cy) {
    cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    cairo_set_line_width(cr, 1.4);
    /* Viewfinder frame + dot */
    cairo_move_to(cr, cx - 5.5, cy - 2.0); cairo_line_to(cr, cx - 5.5, cy - 5.0); cairo_line_to(cr, cx - 2.5, cy - 5.0);
    cairo_move_to(cr, cx + 2.5, cy - 5.0); cairo_line_to(cr, cx + 5.5, cy - 5.0); cairo_line_to(cr, cx + 5.5, cy - 2.0);
    cairo_move_to(cr, cx - 5.5, cy + 2.0); cairo_line_to(cr, cx - 5.5, cy + 5.0); cairo_line_to(cr, cx - 2.5, cy + 5.0);
    cairo_move_to(cr, cx + 2.5, cy + 5.0); cairo_line_to(cr, cx + 5.5, cy + 5.0); cairo_line_to(cr, cx + 5.5, cy + 2.0);
    cairo_stroke(cr);
    cairo_arc(cr, cx, cy, 2.2, 0, 2.0 * G_PI);
    cairo_fill(cr);
}

static void draw_gear_glyph(cairo_t *cr, double cx, double cy) {
    cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    cairo_set_line_width(cr, 1.5);
    cairo_arc(cr, cx, cy, 3.2, 0, 2.0 * G_PI);
    cairo_stroke(cr);
    for (int i = 0; i < 6; i++) {
        double a = i * (G_PI / 3.0);
        cairo_move_to(cr, cx + cos(a) * 3.8, cy + sin(a) * 3.8);
        cairo_line_to(cr, cx + cos(a) * 6.0, cy + sin(a) * 6.0);
    }
    cairo_stroke(cr);
}

static void draw_lock_glyph(cairo_t *cr, double cx, double cy) {
    cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    cairo_set_line_width(cr, 1.4);
    rounded_rect(cr, cx - 4.5, cy - 0.5, 9.0, 6.5, 1.5);
    cairo_stroke(cr);
    cairo_arc(cr, cx, cy - 1.0, 2.8, G_PI, 2.0 * G_PI);
    cairo_stroke(cr);
}

static void draw_plane_glyph(cairo_t *cr, double cx, double cy, int white) {
    if (white) cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    else cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    cairo_set_line_width(cr, 1.4);
    cairo_move_to(cr, cx - 5.5, cy);
    cairo_line_to(cr, cx + 5.5, cy);
    cairo_move_to(cr, cx - 1.0, cy - 5.5);
    cairo_line_to(cr, cx + 2.0, cy);
    cairo_line_to(cr, cx - 1.0, cy + 5.5);
    cairo_move_to(cr, cx - 4.8, cy - 2.5);
    cairo_line_to(cr, cx - 3.3, cy);
    cairo_line_to(cr, cx - 4.8, cy + 2.5);
    cairo_stroke(cr);
}

static void draw_night_glyph(cairo_t *cr, double cx, double cy, int white) {
    if (white) cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    else cairo_set_source_rgb(cr, 0.804, 0.839, 0.957);
    cairo_set_line_width(cr, 1.4);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, 5.2, 0.45 * G_PI, 1.55 * G_PI);
    cairo_arc_negative(cr, cx + 2.6, cy - 0.6, 4.0, 1.38 * G_PI, 0.62 * G_PI);
    cairo_close_path(cr);
    cairo_stroke(cr);
}

static void draw_vertical_gauge_col(cairo_t *cr, double x, double y, double pct,
                                    double r, double g, double b) {
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
        else cairo_set_source_rgb(cr, r, g, b);
        rounded_rect(cr, x + 1.0, y + h - 1.0 - fh, w - 2.0, fh, 1.2);
        cairo_fill(cr);
    }
}

static void draw_vertical_gauge(cairo_t *cr, double x, double y, double pct) {
    draw_vertical_gauge_col(cr, x, y, pct, 0.133, 0.827, 0.933);
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

static void fmt_size_mb(char *buf, size_t n, double mb, int zero_dash) {
    if (zero_dash && mb < 0.05) {
        snprintf(buf, n, "0 B");
    } else if (mb >= 1024.0) {
        snprintf(buf, n, "%.2f GB", mb / 1024.0);
    } else if (mb >= 1.0) {
        snprintf(buf, n, "%.1f MB", mb);
    } else {
        snprintf(buf, n, "%.0f KB", mb * 1024.0);
    }
}

static void fmt_short_mb(char *buf, size_t n, double mb) {
    if (mb >= 1024.0) snprintf(buf, n, "%.1fG", mb / 1024.0);
    else snprintf(buf, n, "%.0fM", mb);
}

static void draw_text(cairo_t *cr, double x, double y, const char *txt,
                      double r, double g, double b, int bold, double sz) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, sz);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, txt);
    cairo_new_path(cr);
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

    /* LEFT SIDE: Activities Overview Pill + Workspace + Quick App Icons */
    double lx = 8.0;
    app->r_brand = (Rect){ lx, 3, 90, 21 };
    draw_hover_pill(cr, app, app->r_brand);
    cairo_new_sub_path(cr);
    cairo_arc(cr, lx + 9, 13.5, 3.0, 0, 2.0 * G_PI);
    cairo_set_source_rgb(cr, 0.537, 0.706, 0.980);
    cairo_fill(cr);
    draw_text(cr, lx + 16, 18.0, "PhantomOS", 0.537, 0.706, 0.980, 1, 11.0);
    lx += 94.0;

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

    /* RIGHT SIDE: Astra-Monitor-style Telemetry HUD + Top-Right Quick Settings Pill */
    double rx = w - 10.0;

    /* Top-Right Unified Quick Settings Pill (Wi-Fi | Brightness | Volume | Battery) */
    char btxt[16] = "";
    double btxt_w = 0.0;
    if (app->has_bat) {
        snprintf(btxt, sizeof(btxt), "%d%%", app->bat_pct);
        cairo_text_extents_t be;
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 9.5);
        cairo_text_extents(cr, btxt, &be);
        btxt_w = be.width + 4.0;
    }
    double qs_w = (app->has_audio ? 82.0 : 62.0) + btxt_w;
    rx -= qs_w;
    app->r_qs = (Rect){ rx, 3, qs_w, 21 };

    int qs_hov = (app->hovering && pt_in_rect(app->hover_x, app->hover_y, app->r_qs)) ||
                 (app->qs_popup && GTK_WIDGET_VISIBLE(app->qs_popup));
    rounded_rect(cr, rx, 3, qs_w, 21, 10.5);
    cairo_set_source_rgb(cr, qs_hov ? 0.192 : 0.137, qs_hov ? 0.200 : 0.141, qs_hov ? 0.282 : 0.208);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.247, 0.259, 0.349);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    double qx = rx + 6.0;
    draw_wifi_icon(cr, qx, 5.5,
                   !app->airplane_mode && app->has_wifi && app->wifi_up,
                   app->wifi_connected, app->wifi_signal_pct, 0);
    qx += 20.0;

    app->r_qs_bar_bright = (Rect){ qx - 1, 3, 18, 21 };
    draw_sun_icon(cr, qx, 5.5, app->night_light);
    qx += 19.0;

    if (app->has_audio) {
        app->r_qs_bar_vol = (Rect){ qx - 1, 3, 18, 21 };
        draw_speaker_icon(cr, qx, 5.5, app->vol_muted);
        qx += 19.0;
    } else {
        app->r_qs_bar_vol = (Rect){ 0, 0, 0, 0 };
    }

    draw_battery_icon(cr, qx, 5.5, app->has_bat, app->bat_pct, app->bat_charging);
    if (app->has_bat) {
        draw_text(cr, qx + 17.0, 17.2, btxt,
                  app->bat_charging ? 0.651 : 0.804,
                  app->bat_charging ? 0.890 : 0.839,
                  app->bat_charging ? 0.631 : 0.957, 1, 9.5);
    }

    /* Clock Pill */
    double clk_w = 66.0;
    rx -= (clk_w + 6.0);
    app->r_clk = (Rect){ rx, 3, clk_w, 21 };
    draw_hover_pill(cr, app, app->r_clk);
    draw_text(cr, rx + 7, 17.5, app->clock_str, 0.804, 0.839, 0.957, 1, 10.5);

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

    /* RAM + ZRAM Widget */
    char rtxt[64], ztxt[64], rb[16], rtb[16], zb[16], ztb[16];
    fmt_short_mb(rb, sizeof(rb), app->ram_used_mb);
    fmt_short_mb(rtb, sizeof(rtb), app->ram_total_mb);
    snprintf(rtxt, sizeof(rtxt), "%s/%s", rb, rtb);

    cairo_text_extents_t r_ext;
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10.0);
    cairo_text_extents(cr, rtxt, &r_ext);

    double ram_w = 34.0 + r_ext.width + 6.0;
    cairo_text_extents_t z_ext = { 0 };
    if (app->swap_total_mb > 0.0) {
        fmt_short_mb(zb, sizeof(zb), app->swap_used_mb);
        fmt_short_mb(ztb, sizeof(ztb), app->swap_total_mb);
        snprintf(ztxt, sizeof(ztxt), "Z:%s/%s", zb, ztb);
        cairo_text_extents(cr, ztxt, &z_ext);
        ram_w = 42.0 + r_ext.width + 8.0 + z_ext.width + 6.0;
    }

    rx -= (ram_w + 6.0);
    app->r_ram = (Rect){ rx, 3, ram_w, 21 };
    draw_hover_pill(cr, app, app->r_ram);
    draw_ram_icon(cr, rx + 4, 6);
    draw_vertical_gauge(cr, rx + 22, 5.5, app->ram_pct);
    if (app->swap_total_mb > 0.0) {
        draw_vertical_gauge_col(cr, rx + 30, 5.5, app->swap_pct, 0.796, 0.651, 0.969);
        draw_text(cr, rx + 40, 17.5, rtxt, 0.651, 0.890, 0.631, 1, 10.0);
        draw_text(cr, rx + 40 + r_ext.width + 7.0, 17.5, ztxt, 0.796, 0.651, 0.969, 1, 10.0);
    } else {
        draw_text(cr, rx + 32, 17.5, rtxt, 0.651, 0.890, 0.631, 1, 10.0);
    }

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

static void draw_text_right(cairo_t *cr, double rx, double y, const char *txt,
                            double r, double g, double b, int bold, double sz) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, sz);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, txt, &ext);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, rx - ext.width - ext.x_bearing, y);
    cairo_show_text(cr, txt);
    cairo_new_path(cr);
}

static void draw_hbar(cairo_t *cr, double x, double y, double w, double h, double pct,
                      double r, double g, double b) {
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    rounded_rect(cr, x, y, w, h, 3.0);
    cairo_set_source_rgb(cr, 0.094, 0.094, 0.145);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.271, 0.278, 0.353);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    double fw = (w - 2.0) * (pct / 100.0);
    if (fw < 3.0 && pct > 0.0) fw = 3.0;
    if (fw > 0.0) {
        rounded_rect(cr, x + 1.0, y + 1.0, fw, h - 2.0, 2.0);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_fill(cr);
    }
}

static gboolean panel_anim_tick(gpointer data) {
    PanelApp *app = (PanelApp *)data;
    int active = 0;
    if (app->qs_popup && GTK_WIDGET_VISIBLE(app->qs_popup) && app->qs_anim_t < 1.0) {
        app->qs_anim_t += 0.10;
        if (app->qs_anim_t > 1.0) app->qs_anim_t = 1.0;
        gtk_widget_queue_draw(app->qs_area);
        active = 1;
    }
    if (app->ram_popup && GTK_WIDGET_VISIBLE(app->ram_popup) && app->ram_anim_t < 1.0) {
        app->ram_anim_t += 0.10;
        if (app->ram_anim_t > 1.0) app->ram_anim_t = 1.0;
        gtk_widget_queue_draw(app->ram_popup_area);
        active = 1;
    }
    if (!active) {
        app->anim_id = 0;
        return FALSE;
    }
    return TRUE;
}

static void start_panel_anim(PanelApp *app) {
    if (!app->anim_id) {
        app->anim_id = g_timeout_add(16, panel_anim_tick, app);
    }
}

static gboolean on_ram_popup_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
    (void)event;
    PanelApp *app = (PanelApp *)data;
    cairo_t *cr = gdk_cairo_create(widget->window);
    double w = (double)widget->allocation.width;
    double h = (double)widget->allocation.height;

    double rt = app->ram_anim_t > 0.0 ? app->ram_anim_t : 1.0;
    double re = 1.0 - pow(1.0 - rt, 3.0);
    if (re < 0.999) {
        cairo_set_source_rgb(cr, 0.09, 0.095, 0.14);
        cairo_paint(cr);
        cairo_push_group(cr);
        cairo_translate(cr, 0.0, -(1.0 - re) * 12.0);
    }

    rounded_rect(cr, 0.5, 0.5, w - 1.0, h - 1.0, 8.0);
    cairo_set_source_rgb(cr, 0.118, 0.118, 0.180);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.271, 0.278, 0.353);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    double pad = 14.0;
    double rw = w - 2.0 * pad;
    double y = 22.0;

    draw_text(cr, w * 0.5 - 24.0, y, "Memory", 0.804, 0.839, 0.957, 1, 11.5);
    y += 18.0;

    char vbuf[64];
    const char *lbls[4] = { "Total:", "Used:", "Cache / Buf:", "Available:" };
    double vals[4] = { app->ram_total_mb, app->ram_used_mb, app->ram_cache_mb, app->ram_avail_mb };
    for (int i = 0; i < 4; i++) {
        draw_text(cr, pad, y, lbls[i], 0.729, 0.761, 0.871, 0, 10.5);
        fmt_size_mb(vbuf, sizeof(vbuf), vals[i], 0);
        draw_text_right(cr, w - pad, y, vbuf, 0.804, 0.839, 0.957, i == 1, 10.5);
        y += 17.0;
    }

    draw_hbar(cr, pad, y - 6.0, rw - 38.0, 10.0, app->ram_pct, 0.133, 0.827, 0.933);
    snprintf(vbuf, sizeof(vbuf), "%.0f%%", app->ram_pct);
    draw_text_right(cr, w - pad, y + 3.0, vbuf, 0.651, 0.890, 0.631, 1, 10.0);
    y += 14.0;

    draw_sparkline(cr, pad, y, rw - 34.0, 56.0, app->ram_hist,
                   app->swap_total_mb > 0.0 ? app->swap_hist : NULL, 100.0);
    draw_text_right(cr, w - pad, y + 10.0, "100%", 0.580, 0.616, 0.725, 0, 9.0);
    draw_text_right(cr, w - pad, y + 32.0, "50%", 0.580, 0.616, 0.725, 0, 9.0);
    draw_text_right(cr, w - pad, y + 54.0, "0%", 0.580, 0.616, 0.725, 0, 9.0);
    y += 66.0;

    cairo_set_source_rgb(cr, 0.192, 0.196, 0.267);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, pad, y);
    cairo_line_to(cr, w - pad, y);
    cairo_stroke(cr);
    y += 18.0;

    char title[48];
    if (app->zram_algo[0]) snprintf(title, sizeof(title), "ZRAM Swap (%s)", app->zram_algo);
    else snprintf(title, sizeof(title), "Swap");
    draw_text(cr, w * 0.5 - 48.0, y, title, 0.796, 0.651, 0.969, 1, 11.0);
    y += 14.0;

    draw_hbar(cr, pad, y, rw - 38.0, 10.0, app->swap_pct, 0.796, 0.651, 0.969);
    snprintf(vbuf, sizeof(vbuf), "%.0f%%", app->swap_pct);
    draw_text_right(cr, w - pad, y + 9.0, vbuf, 0.796, 0.651, 0.969, 1, 10.0);
    y += 25.0;

    draw_text(cr, pad, y, "Used:", 0.729, 0.761, 0.871, 0, 10.5);
    fmt_size_mb(vbuf, sizeof(vbuf), app->swap_used_mb, 1);
    draw_text_right(cr, w - pad, y, vbuf, 0.804, 0.839, 0.957, 1, 10.5);
    y += 17.0;

    draw_text(cr, pad, y, "Total:", 0.729, 0.761, 0.871, 0, 10.5);
    fmt_size_mb(vbuf, sizeof(vbuf), app->swap_total_mb, 1);
    draw_text_right(cr, w - pad, y, vbuf, 0.804, 0.839, 0.957, 0, 10.5);
    y += 17.0;

    draw_text(cr, pad, y, "In RAM (compr):", 0.729, 0.761, 0.871, 0, 10.5);
    if (app->zram_mem_mb > 0.05 && app->zram_orig_mb > 0.05) {
        char mbuf[24];
        fmt_size_mb(mbuf, sizeof(mbuf), app->zram_mem_mb, 0);
        snprintf(vbuf, sizeof(vbuf), "%s (%.2fx)", mbuf, app->zram_orig_mb / app->zram_mem_mb);
    } else {
        snprintf(vbuf, sizeof(vbuf), "0 B (Idle)");
    }
    draw_text_right(cr, w - pad, y, vbuf, 0.580, 0.886, 0.835, 0, 10.0);
    y += 18.0;

    /* System Monitor quick button */
    double bw = 136.0, bh = 24.0;
    double bx = (w - bw) * 0.5;
    app->r_popup_mon = (Rect){ bx, y, bw, bh };
    int hov = app->popup_hover && pt_in_rect(app->popup_hx, app->popup_hy, app->r_popup_mon);
    rounded_rect(cr, bx, y, bw, bh, 6.0);
    cairo_set_source_rgb(cr, hov ? 0.271 : 0.192, hov ? 0.278 : 0.196, hov ? 0.353 : 0.267);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.537, 0.706, 0.980);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    draw_text(cr, bx + 16.0, y + 16.0, "System Monitor", 0.804, 0.839, 0.957, 1, 10.0);

    if (re < 0.999) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.15 + 0.85 * re);
    }

    cairo_destroy(cr);
    return TRUE;
}

static gboolean on_ram_popup_button(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    PanelApp *app = (PanelApp *)data;
    if (ev->button == 1 && pt_in_rect(ev->x, ev->y, app->r_popup_mon)) {
        gtk_widget_hide(app->ram_popup);
        spawn_cmd("/usr/bin/phantom-monitor");
        return TRUE;
    }
    return FALSE;
}

static gboolean on_ram_popup_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    (void)w;
    PanelApp *app = (PanelApp *)data;
    app->popup_hx = ev->x;
    app->popup_hy = ev->y;
    app->popup_hover = 1;
    gtk_widget_queue_draw(app->ram_popup_area);
    return FALSE;
}

static gboolean on_ram_popup_focus_out(GtkWidget *w, GdkEventFocus *ev, gpointer data) {
    (void)w;
    (void)ev;
    PanelApp *app = (PanelApp *)data;
    app->popup_hidden_us = g_get_monotonic_time();
    gtk_widget_hide(app->ram_popup);
    return FALSE;
}

static gboolean on_ram_popup_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    (void)w;
    PanelApp *app = (PanelApp *)data;
    if (ev->keyval == 0xff1b) {
        gtk_widget_hide(app->ram_popup);
        return TRUE;
    }
    return FALSE;
}

static void toggle_ram_popup(PanelApp *app) {
    const int pw = 268;
    const int ph = 334;
    if (!app->ram_popup) {
        app->ram_popup = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(app->ram_popup), "phantom-panel-ram");
        gtk_window_set_decorated(GTK_WINDOW(app->ram_popup), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(app->ram_popup), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(app->ram_popup), TRUE);
        gtk_window_set_type_hint(GTK_WINDOW(app->ram_popup), GDK_WINDOW_TYPE_HINT_UTILITY);
        gtk_window_set_default_size(GTK_WINDOW(app->ram_popup), pw, ph);
        gtk_widget_set_size_request(app->ram_popup, pw, ph);
        gtk_window_stick(GTK_WINDOW(app->ram_popup));

        app->ram_popup_area = gtk_drawing_area_new();
        gtk_widget_add_events(app->ram_popup_area, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK);
        g_signal_connect(app->ram_popup_area, "expose-event", G_CALLBACK(on_ram_popup_expose), app);
        g_signal_connect(app->ram_popup_area, "button-press-event", G_CALLBACK(on_ram_popup_button), app);
        g_signal_connect(app->ram_popup_area, "motion-notify-event", G_CALLBACK(on_ram_popup_motion), app);
        g_signal_connect(app->ram_popup, "focus-out-event", G_CALLBACK(on_ram_popup_focus_out), app);
        g_signal_connect(app->ram_popup, "key-press-event", G_CALLBACK(on_ram_popup_key), app);
        gtk_container_add(GTK_CONTAINER(app->ram_popup), app->ram_popup_area);
        gtk_widget_show(app->ram_popup_area);
    }

    if (GTK_WIDGET_VISIBLE(app->ram_popup)) {
        gtk_widget_hide(app->ram_popup);
        return;
    }
    if ((g_get_monotonic_time() - app->popup_hidden_us) < 220000) {
        return;
    }

    if (app->qs_popup && GTK_WIDGET_VISIBLE(app->qs_popup)) {
        gtk_widget_hide(app->qs_popup);
    }

    int px = (int)(app->r_ram.x + (app->r_ram.w - pw) * 0.5);
    if (px < 6) px = 6;
    if (px + pw > app->width - 6) px = app->width - pw - 6;
    app->ram_anim_t = 0.05;
    gtk_window_move(GTK_WINDOW(app->ram_popup), px, PANEL_HEIGHT + 2);
    gtk_widget_show_all(app->ram_popup);
    gtk_window_move(GTK_WINDOW(app->ram_popup), px, PANEL_HEIGHT + 2);
    gtk_window_present(GTK_WINDOW(app->ram_popup));
    start_panel_anim(app);
    gtk_widget_queue_draw(app->ram_popup_area);
}

static void draw_qs_circle_btn(cairo_t *cr, PanelApp *app, double cx, double cy,
                               double r, Rect *out_r) {
    *out_r = (Rect){ cx - r, cy - r, r * 2.0, r * 2.0 };
    int hov = app->qs_hover && pt_in_rect(app->qs_hx, app->qs_hy, *out_r);
    cairo_arc(cr, cx, cy, r, 0, 2.0 * G_PI);
    cairo_set_source_rgb(cr, hov ? 0.259 : 0.180, hov ? 0.275 : 0.188, hov ? 0.365 : 0.255);
    cairo_fill(cr);
}

static void draw_qs_slider(cairo_t *cr, double x, double y, double w, int pct,
                           int active, Rect *out_trk) {
    *out_trk = (Rect){ x, y - 10.0, w, 20.0 };
    double frac = (double)pct / 100.0;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    /* Track bg */
    rounded_rect(cr, x, y - 2.5, w, 5.0, 2.5);
    cairo_set_source_rgb(cr, 0.231, 0.239, 0.314);
    cairo_fill(cr);

    /* Track fill */
    double fw = w * frac;
    if (fw > 1.0) {
        rounded_rect(cr, x, y - 2.5, fw, 5.0, 2.5);
        if (active) cairo_set_source_rgb(cr, 0.208, 0.518, 0.894);
        else cairo_set_source_rgb(cr, 0.396, 0.420, 0.518);
        cairo_fill(cr);
    }

    /* Knob */
    double kx = x + fw;
    cairo_arc(cr, kx, y, 6.5, 0, 2.0 * G_PI);
    cairo_set_source_rgb(cr, 0.945, 0.961, 0.988);
    cairo_fill(cr);
}

static void draw_qs_tile(cairo_t *cr, PanelApp *app, Rect r, int active,
                         const char *title, const char *sub, int split_arrow) {
    int hov = app->qs_hover && pt_in_rect(app->qs_hx, app->qs_hy, r);
    rounded_rect(cr, r.x, r.y, r.w, r.h, 16.0);
    if (active) {
        cairo_set_source_rgb(cr, hov ? 0.259 : 0.208, hov ? 0.565 : 0.518, hov ? 0.925 : 0.894);
    } else {
        cairo_set_source_rgb(cr, hov ? 0.220 : 0.165, hov ? 0.231 : 0.173, hov ? 0.310 : 0.239);
    }
    cairo_fill(cr);

    if (split_arrow) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, active ? 0.22 : 0.12);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, r.x + r.w - 30.0, r.y + 8.0);
        cairo_line_to(cr, r.x + r.w - 30.0, r.y + r.h - 8.0);
        cairo_stroke(cr);

        /* Chevron > */
        double ax = r.x + r.w - 17.0, ay = r.y + r.h * 0.5;
        cairo_set_source_rgb(cr, active ? 1.0 : 0.804, active ? 1.0 : 0.839, active ? 1.0 : 0.957);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, ax - 2.0, ay - 4.0);
        cairo_line_to(cr, ax + 2.5, ay);
        cairo_line_to(cr, ax - 2.0, ay + 4.0);
        cairo_stroke(cr);
    }

    if (active) draw_text(cr, r.x + 36.0, r.y + 20.5, title, 1.0, 1.0, 1.0, 1, 10.5);
    else draw_text(cr, r.x + 36.0, r.y + 20.5, title, 0.851, 0.878, 0.961, 1, 10.5);

    char sub_clip[20];
    snprintf(sub_clip, sizeof(sub_clip), "%.14s", sub ? sub : "");
    if (strlen(sub ? sub : "") > 14) {
        sub_clip[11] = '.'; sub_clip[12] = '.'; sub_clip[13] = '.'; sub_clip[14] = '\0';
    }
    if (active) draw_text(cr, r.x + 36.0, r.y + 34.5, sub_clip, 0.880, 0.940, 1.0, 0, 9.0);
    else draw_text(cr, r.x + 36.0, r.y + 34.5, sub_clip, 0.620, 0.655, 0.765, 0, 9.0);
}

static gboolean take_screenshot_cb(gpointer data) {
    PanelApp *app = (PanelApp *)data;
    GdkWindow *root = gdk_get_default_root_window();
    if (!root) return FALSE;
    int sw = gdk_screen_get_width(gdk_screen_get_default());
    int sh = gdk_screen_get_height(gdk_screen_get_default());
    GdkPixbuf *pb = gdk_pixbuf_get_from_drawable(NULL, root, NULL, 0, 0, 0, 0, sw, sh);
    if (pb) {
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        const char *home = getenv("HOME");
        if (!home || !home[0]) home = "/tmp";
        char path[256], fname[64];
        strftime(fname, sizeof(fname), "PhantomOS_%Y%m%d_%H%M%S.png", &t);
        snprintf(path, sizeof(path), "%s/%s", home, fname);
        if (gdk_pixbuf_save(pb, path, "png", NULL, NULL)) {
            snprintf(app->qs_toast, sizeof(app->qs_toast), "Saved ~/%s", fname);
            app->qs_toast_until_us = g_get_monotonic_time() + 5000000;
        }
        g_object_unref(pb);
    }
    return FALSE;
}

static gboolean on_qs_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
    (void)event;
    PanelApp *app = (PanelApp *)data;
    cairo_t *cr = gdk_cairo_create(widget->window);
    double w = (double)widget->allocation.width;
    double h = (double)widget->allocation.height;

    double qt = app->qs_anim_t > 0.0 ? app->qs_anim_t : 1.0;
    double qe = 1.0 - pow(1.0 - qt, 3.0);
    if (qe < 0.999) {
        cairo_set_source_rgb(cr, 0.09, 0.095, 0.14);
        cairo_paint(cr);
        cairo_push_group(cr);
        cairo_translate(cr, 0.0, -(1.0 - qe) * 14.0);
    }

    rounded_rect(cr, 0.5, 0.5, w - 1.0, h - 1.0, 14.0);
    cairo_set_source_rgb(cr, 0.110, 0.114, 0.165);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.235, 0.247, 0.341);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    if (app->qs_page == 0) {
        /* Header row: Battery pill + 4 quick icon buttons */
        app->r_qs_bat_pill = (Rect){ 14, 14, 88, 30 };
        rounded_rect(cr, 14, 14, 88, 30, 15.0);
        cairo_set_source_rgb(cr, 0.180, 0.188, 0.255);
        cairo_fill(cr);
        draw_battery_icon(cr, 23, 21, app->has_bat, app->bat_pct, app->bat_charging);
        char bstr[24];
        if (app->has_bat) snprintf(bstr, sizeof(bstr), "%d%%", app->bat_pct);
        else snprintf(bstr, sizeof(bstr), "AC");
        draw_text(cr, 43, 33.0, bstr, 0.851, 0.878, 0.961, 1, 11.0);

        draw_qs_circle_btn(cr, app, 174, 29, 15, &app->r_qs_shot);
        draw_camera_glyph(cr, 174, 29);

        draw_qs_circle_btn(cr, app, 210, 29, 15, &app->r_qs_mon);
        draw_gear_glyph(cr, 210, 29);

        draw_qs_circle_btn(cr, app, 246, 29, 15, &app->r_qs_lock);
        draw_lock_glyph(cr, 246, 29);

        draw_qs_circle_btn(cr, app, 282, 29, 15, &app->r_qs_pwr);
        draw_power_glyph(cr, 282, 29.5, 4.8,
                         app->qs_power_confirm ? 0.953 : 0.804,
                         app->qs_power_confirm ? 0.545 : 0.839,
                         app->qs_power_confirm ? 0.659 : 0.957);

        double slider_y0 = 68.0;
        if (app->qs_power_confirm) {
            /* Inline Power / Logout strip */
            rounded_rect(cr, 14, 52, w - 28, 30, 10.0);
            cairo_set_source_rgb(cr, 0.153, 0.157, 0.220);
            cairo_fill(cr);
            app->r_qs_logout   = (Rect){ 20, 56, 66, 22 };
            app->r_qs_reboot   = (Rect){ 92, 56, 66, 22 };
            app->r_qs_poweroff = (Rect){ 164, 56, 78, 22 };
            app->r_qs_pwr_cancel = (Rect){ 248, 56, 44, 22 };

            Rect *prs[4] = { &app->r_qs_logout, &app->r_qs_reboot, &app->r_qs_poweroff, &app->r_qs_pwr_cancel };
            const char *ptxt[4] = { "Logout", "Reboot", "Power Off", "Esc" };
            for (int i = 0; i < 4; i++) {
                int h2 = app->qs_hover && pt_in_rect(app->qs_hx, app->qs_hy, *prs[i]);
                rounded_rect(cr, prs[i]->x, prs[i]->y, prs[i]->w, prs[i]->h, 7.0);
                if (i == 2) cairo_set_source_rgb(cr, h2 ? 0.92 : 0.78, h2 ? 0.38 : 0.30, h2 ? 0.46 : 0.38);
                else cairo_set_source_rgb(cr, h2 ? 0.28 : 0.22, h2 ? 0.30 : 0.23, h2 ? 0.40 : 0.31);
                cairo_fill(cr);
                draw_text(cr, prs[i]->x + 10.0, prs[i]->y + 15.0, ptxt[i], 0.94, 0.96, 0.99, 1, 9.5);
            }
            slider_y0 = 98.0;
        } else {
            app->r_qs_logout = app->r_qs_reboot = app->r_qs_poweroff = app->r_qs_pwr_cancel = (Rect){0,0,0,0};
        }

        /* Volume slider */
        app->r_qs_vol_ico = (Rect){ 14, slider_y0 - 4, 24, 24 };
        draw_speaker_icon(cr, 17, slider_y0, app->vol_muted);
        draw_qs_slider(cr, 46, slider_y0 + 8.0, w - 62.0,
                       app->vol_muted ? 0 : app->vol_pct, app->has_audio && !app->vol_muted,
                       &app->r_qs_vol_trk);

        /* Brightness slider */
        double br_y = slider_y0 + 34.0;
        draw_sun_icon(cr, 17, br_y, app->night_light);
        draw_qs_slider(cr, 46, br_y + 8.0, w - 62.0,
                       app->bright_pct, 1, &app->r_qs_bright_trk);

        /* 2x2 Quick Settings Tiles */
        double ty0 = br_y + 30.0;
        double tw = 136.0, th = 48.0, gap = 10.0;
        Rect r_wifi_full = { 14, ty0, tw, th };
        Rect r_lan       = { 14 + tw + gap, ty0, tw, th };
        Rect r_night     = { 14, ty0 + th + gap, tw, th };
        Rect r_plane     = { 14 + tw + gap, ty0 + th + gap, tw, th };

        app->r_qs_wifi_toggle = (Rect){ r_wifi_full.x, r_wifi_full.y, tw - 30.0, th };
        app->r_qs_wifi_arrow  = (Rect){ r_wifi_full.x + tw - 30.0, r_wifi_full.y, 30.0, th };
        app->r_qs_lan         = r_lan;
        app->r_qs_night       = r_night;
        app->r_qs_plane       = r_plane;

        int wifi_active = !app->airplane_mode && app->has_wifi && app->wifi_up;
        const char *wsub = !app->has_wifi ? "No Adapter"
                         : (app->airplane_mode ? "Off (Airplane)"
                         : (app->wifi_connected ? app->wifi_ssid
                         : (app->wifi_up ? "Disconnected" : "Off")));
        draw_qs_tile(cr, app, r_wifi_full, wifi_active, "Wi-Fi", wsub, 1);
        draw_wifi_icon(cr, r_wifi_full.x + 12, r_wifi_full.y + 16,
                       wifi_active, app->wifi_connected, app->wifi_signal_pct, wifi_active);

        int lan_active = (app->eth_ip[0] != '\0');
        draw_qs_tile(cr, app, r_lan, lan_active, "Wired LAN",
                     lan_active ? app->eth_ip : "Disconnected", 0);
        draw_net_icon(cr, r_lan.x + 12, r_lan.y + 16);

        draw_qs_tile(cr, app, r_night, app->night_light, "Night Light",
                     app->night_light ? "Warm 4200K" : "Off", 0);
        draw_night_glyph(cr, r_night.x + 20, r_night.y + 24, app->night_light);

        draw_qs_tile(cr, app, r_plane, app->airplane_mode, "Airplane",
                     app->airplane_mode ? "On" : "Off", 0);
        draw_plane_glyph(cr, r_plane.x + 20, r_plane.y + 24, app->airplane_mode);

        /* Status Footer */
        double fy = h - 14.0;
        if (app->qs_toast[0] && g_get_monotonic_time() < app->qs_toast_until_us) {
            draw_text(cr, 16, fy, app->qs_toast, 0.651, 0.890, 0.631, 0, 9.5);
        } else {
            char ftxt[96];
            snprintf(ftxt, sizeof(ftxt), "Vol %d%%  |  Bright %d%% (%.24s)",
                     app->vol_pct, app->bright_pct, app->has_hw_bl ? app->bl_dir : "XRandR");
            draw_text(cr, 16, fy, ftxt, 0.560, 0.595, 0.705, 0, 9.0);
        }
    } else if (app->qs_page == 1) {
        /* Page 1: Inline Wi-Fi Network List */
        app->r_qs_back = (Rect){ 14, 14, 54, 26 };
        rounded_rect(cr, 14, 14, 54, 26, 8.0);
        cairo_set_source_rgb(cr, 0.180, 0.188, 0.255);
        cairo_fill(cr);
        draw_text(cr, 24, 31, "< Back", 0.804, 0.839, 0.957, 1, 9.5);

        draw_text(cr, 88, 31, "Wi-Fi Networks", 0.851, 0.878, 0.961, 1, 11.0);

        app->r_qs_scan = (Rect){ w - 68, 14, 54, 26 };
        rounded_rect(cr, w - 68, 14, 54, 26, 8.0);
        cairo_set_source_rgb(cr, 0.208, 0.518, 0.894);
        cairo_fill(cr);
        draw_text(cr, w - 55, 31, app->wifi_scanning ? "..." : "Scan", 1.0, 1.0, 1.0, 1, 9.5);

        if (!app->has_wifi) {
            draw_text(cr, 24, 96, "No Wi-Fi adapter detected.", 0.953, 0.545, 0.659, 1, 10.5);
            draw_text(cr, 24, 118, "Attach a Wi-Fi card or USB dongle on real hardware", 0.620, 0.655, 0.765, 0, 9.0);
            draw_text(cr, 24, 134, "(Intel, Realtek rtw88/89, Atheros, MediaTek ready).", 0.620, 0.655, 0.765, 0, 9.0);
        } else if (app->wifi_net_count == 0) {
            draw_text(cr, 24, 96, app->wifi_scanning ? "Scanning nearby networks..." : "No networks found. Click Scan.",
                      0.729, 0.761, 0.871, 0, 10.0);
        } else {
            int max_rows = app->wifi_net_count < 6 ? app->wifi_net_count : 6;
            for (int i = 0; i < 6; i++) app->r_qs_wifi_rows[i] = (Rect){0,0,0,0};
            for (int i = 0; i < max_rows; i++) {
                double ry = 54.0 + i * 42.0;
                Rect rr = { 14, ry, w - 28, 36 };
                app->r_qs_wifi_rows[i] = rr;
                int hov = app->qs_hover && pt_in_rect(app->qs_hx, app->qs_hy, rr);
                rounded_rect(cr, rr.x, rr.y, rr.w, rr.h, 10.0);
                if (app->wifi_nets[i].connected) cairo_set_source_rgb(cr, 0.165, 0.340, 0.580);
                else cairo_set_source_rgb(cr, hov ? 0.220 : 0.165, hov ? 0.231 : 0.173, hov ? 0.310 : 0.239);
                cairo_fill(cr);

                draw_wifi_icon(cr, rr.x + 10, rr.y + 10, 1, 1, app->wifi_nets[i].signal_pct, 1);
                draw_text(cr, rr.x + 34, rr.y + 22, app->wifi_nets[i].ssid, 0.90, 0.92, 0.98, 1, 10.5);
                if (app->wifi_nets[i].connected) {
                    draw_text_right(cr, rr.x + rr.w - 10, rr.y + 22, "Connected", 0.651, 0.890, 0.631, 1, 9.0);
                } else {
                    char sb[24];
                    snprintf(sb, sizeof(sb), "%s %d%%",
                             app->wifi_nets[i].secured ? "WPA" : "Open",
                             app->wifi_nets[i].signal_pct);
                    draw_text_right(cr, rr.x + rr.w - 10, rr.y + 22, sb, 0.620, 0.655, 0.765, 0, 9.0);
                }
            }
        }
    } else if (app->qs_page == 2) {
        /* Page 2: Wi-Fi Password header (GtkEntry sits directly below qs_area) */
        app->r_qs_back = (Rect){ 14, 14, 54, 26 };
        rounded_rect(cr, 14, 14, 54, 26, 8.0);
        cairo_set_source_rgb(cr, 0.180, 0.188, 0.255);
        cairo_fill(cr);
        draw_text(cr, 24, 31, "< Back", 0.804, 0.839, 0.957, 1, 9.5);
        draw_text(cr, 80, 31, "Connect to Wi-Fi", 0.851, 0.878, 0.961, 1, 11.0);
        draw_text(cr, 18, 68, "Network SSID:", 0.620, 0.655, 0.765, 0, 9.5);
        draw_text(cr, 18, 88, app->qs_target_ssid, 0.537, 0.706, 0.980, 1, 11.5);
        draw_text(cr, 18, 114, "Enter WPA/WPA2/WPA3 passphrase below:", 0.729, 0.761, 0.871, 0, 9.5);
    }

    if (qe < 0.999) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.15 + 0.85 * qe);
    }

    cairo_destroy(cr);
    return TRUE;
}

static void qs_set_page(PanelApp *app, int page) {
    app->qs_page = page;
    app->qs_power_confirm = 0;
    app->qs_anim_t = 0.05;
    start_panel_anim(app);
    if (app->qs_pass_box) {
        if (page == 2) {
            gtk_widget_set_size_request(app->qs_area, 312, 128);
            gtk_widget_set_size_request(app->qs_popup, 312, 182);
            gtk_window_resize(GTK_WINDOW(app->qs_popup), 312, 182);
            gtk_widget_show_all(app->qs_pass_box);
            gtk_entry_set_text(GTK_ENTRY(app->qs_pass_entry), "");
            gtk_widget_grab_focus(app->qs_pass_entry);
        } else {
            gtk_widget_hide(app->qs_pass_box);
            gtk_widget_set_size_request(app->qs_area, 312, 326);
            gtk_widget_set_size_request(app->qs_popup, 312, 326);
            gtk_window_resize(GTK_WINDOW(app->qs_popup), 312, 326);
        }
    }
    if (app->qs_area) gtk_widget_queue_draw(app->qs_area);
}

static void on_wifi_pass_submit(GtkWidget *w, gpointer data) {
    (void)w;
    PanelApp *app = (PanelApp *)data;
    const char *pass = gtk_entry_get_text(GTK_ENTRY(app->qs_pass_entry));
    connect_wifi_async(app, app->qs_target_ssid, pass ? pass : "");
    gtk_entry_set_text(GTK_ENTRY(app->qs_pass_entry), "");
    qs_set_page(app, 0);
}

static void update_slider_at(PanelApp *app, int which, double x) {
    Rect trk = (which == 1) ? app->r_qs_vol_trk : app->r_qs_bright_trk;
    if (trk.w <= 1.0) return;
    double frac = (x - trk.x) / trk.w;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    int pct = (int)(frac * 100.0 + 0.5);
    gint64 now = g_get_monotonic_time();

    if (which == 1) {
        app->vol_pct = pct;
        app->vol_muted = 0;
        if (app->has_audio && (now - app->last_slider_cmd_us) > 65000) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "amixer -q sset Master unmute %d%%", pct);
            spawn_cmd(cmd);
            app->last_slider_cmd_us = now;
        }
    } else if (which == 2) {
        if (pct < 15) pct = 15;
        app->bright_pct = pct;
        apply_display_settings(app);
    }
    gtk_widget_queue_draw(app->qs_area);
    gtk_widget_queue_draw(app->area);
}

static gboolean on_qs_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    PanelApp *app = (PanelApp *)data;
    if (ev->button != 1) return FALSE;

    if (app->qs_page == 0) {
        if (pt_in_rect(ev->x, ev->y, app->r_qs_shot)) {
            gtk_widget_hide(app->qs_popup);
            g_timeout_add(220, take_screenshot_cb, app);
        } else if (pt_in_rect(ev->x, ev->y, app->r_qs_mon)) {
            gtk_widget_hide(app->qs_popup);
            spawn_cmd("/usr/bin/phantom-settings");
        } else if (pt_in_rect(ev->x, ev->y, app->r_qs_lock) ||
                   pt_in_rect(ev->x, ev->y, app->r_qs_pwr)) {
            app->qs_power_confirm = !app->qs_power_confirm;
            gtk_widget_queue_draw(app->qs_area);
        } else if (app->qs_power_confirm && pt_in_rect(ev->x, ev->y, app->r_qs_logout)) {
            gtk_widget_hide(app->qs_popup);
            gdk_flush();
            spawn_cmd("sh -c 'killall -TERM fluxbox 2>/dev/null; sleep 1; killall -KILL fluxbox 2>/dev/null'");
        } else if (app->qs_power_confirm && pt_in_rect(ev->x, ev->y, app->r_qs_reboot)) {
            gtk_widget_hide(app->qs_popup);
            spawn_cmd("sudo -n /sbin/reboot");
        } else if (app->qs_power_confirm && pt_in_rect(ev->x, ev->y, app->r_qs_poweroff)) {
            gtk_widget_hide(app->qs_popup);
            spawn_cmd("sudo -n /sbin/poweroff");
        } else if (app->qs_power_confirm && pt_in_rect(ev->x, ev->y, app->r_qs_pwr_cancel)) {
            app->qs_power_confirm = 0;
            gtk_widget_queue_draw(app->qs_area);
        } else if (app->has_audio && pt_in_rect(ev->x, ev->y, app->r_qs_vol_ico)) {
            spawn_cmd("amixer -q sset Master toggle");
            sample_audio(app);
            gtk_widget_queue_draw(app->qs_area);
            gtk_widget_queue_draw(app->area);
        } else if (pt_in_rect(ev->x, ev->y, app->r_qs_vol_trk)) {
            app->dragging_slider = 1;
            update_slider_at(app, 1, ev->x);
        } else if (pt_in_rect(ev->x, ev->y, app->r_qs_bright_trk)) {
            app->dragging_slider = 2;
            update_slider_at(app, 2, ev->x);
        } else if (pt_in_rect(ev->x, ev->y, app->r_qs_wifi_arrow)) {
            qs_set_page(app, 1);
            trigger_wifi_scan(app);
        } else if (pt_in_rect(ev->x, ev->y, app->r_qs_wifi_toggle)) {
            if (!app->has_wifi) {
                qs_set_page(app, 1);
            } else {
                char cmd[128];
                snprintf(cmd, sizeof(cmd), "sudo -n /usr/libexec/phantom-net %s %s",
                         app->wifi_up ? "wifi-down" : "wifi-up", app->wifi_iface);
                spawn_cmd(cmd);
                g_timeout_add(700, on_wifi_connect_poll, app);
            }
        } else if (pt_in_rect(ev->x, ev->y, app->r_qs_lan)) {
            spawn_cmd("sudo -n /usr/libexec/phantom-net dhcp-eth0");
            g_timeout_add(1200, on_wifi_connect_poll, app);
        } else if (pt_in_rect(ev->x, ev->y, app->r_qs_night)) {
            app->night_light = !app->night_light;
            apply_display_settings(app);
            gtk_widget_queue_draw(app->qs_area);
            gtk_widget_queue_draw(app->area);
        } else if (pt_in_rect(ev->x, ev->y, app->r_qs_plane)) {
            set_airplane_mode(app, !app->airplane_mode);
            gtk_widget_queue_draw(app->qs_area);
            gtk_widget_queue_draw(app->area);
        }
    } else if (app->qs_page == 1) {
        if (pt_in_rect(ev->x, ev->y, app->r_qs_back)) {
            qs_set_page(app, 0);
        } else if (pt_in_rect(ev->x, ev->y, app->r_qs_scan)) {
            trigger_wifi_scan(app);
            gtk_widget_queue_draw(app->qs_area);
        } else {
            for (int i = 0; i < 6 && i < app->wifi_net_count; i++) {
                if (pt_in_rect(ev->x, ev->y, app->r_qs_wifi_rows[i])) {
                    if (app->wifi_nets[i].connected) {
                        char cmd[128];
                        snprintf(cmd, sizeof(cmd), "sudo -n /usr/libexec/phantom-net wifi-disconnect %s",
                                 app->wifi_iface);
                        spawn_cmd(cmd);
                        g_timeout_add(600, on_wifi_connect_poll, app);
                        qs_set_page(app, 0);
                    } else if (!app->wifi_nets[i].secured) {
                        connect_wifi_async(app, app->wifi_nets[i].ssid, "");
                        qs_set_page(app, 0);
                    } else {
                        snprintf(app->qs_target_ssid, sizeof(app->qs_target_ssid), "%s",
                                 app->wifi_nets[i].ssid);
                        qs_set_page(app, 2);
                    }
                    break;
                }
            }
        }
    } else if (app->qs_page == 2) {
        if (pt_in_rect(ev->x, ev->y, app->r_qs_back)) {
            qs_set_page(app, 1);
        }
    }
    return TRUE;
}

static gboolean on_qs_button_release(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    PanelApp *app = (PanelApp *)data;
    if (app->dragging_slider == 1 && app->has_audio) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "amixer -q sset Master unmute %d%%", app->vol_pct);
        spawn_cmd(cmd);
    }
    (void)ev;
    app->dragging_slider = 0;
    return FALSE;
}

static gboolean on_qs_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    (void)w;
    PanelApp *app = (PanelApp *)data;
    app->qs_hx = ev->x;
    app->qs_hy = ev->y;
    app->qs_hover = 1;
    if (app->dragging_slider) {
        update_slider_at(app, app->dragging_slider, ev->x);
    } else {
        gtk_widget_queue_draw(app->qs_area);
    }
    return FALSE;
}

static gboolean on_qs_focus_out(GtkWidget *w, GdkEventFocus *ev, gpointer data) {
    (void)w;
    (void)ev;
    PanelApp *app = (PanelApp *)data;
    app->dragging_slider = 0;
    app->qs_hidden_us = g_get_monotonic_time();
    gtk_widget_hide(app->qs_popup);
    return FALSE;
}

static gboolean on_qs_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    (void)w;
    PanelApp *app = (PanelApp *)data;
    if (ev->keyval == 0xff1b) {
        if (app->qs_page > 0) {
            qs_set_page(app, app->qs_page - 1);
        } else {
            gtk_widget_hide(app->qs_popup);
        }
        return TRUE;
    }
    return FALSE;
}

static void toggle_qs_popup(PanelApp *app) {
    const int pw = 312;
    const int ph = 326;

    if (app->ram_popup && GTK_WIDGET_VISIBLE(app->ram_popup)) {
        gtk_widget_hide(app->ram_popup);
    }

    if (!app->qs_popup) {
        app->qs_popup = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(app->qs_popup), "phantom-panel-qs");
        gtk_window_set_decorated(GTK_WINDOW(app->qs_popup), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(app->qs_popup), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(app->qs_popup), TRUE);
        gtk_window_set_type_hint(GTK_WINDOW(app->qs_popup), GDK_WINDOW_TYPE_HINT_UTILITY);
        gtk_window_set_default_size(GTK_WINDOW(app->qs_popup), pw, ph);
        gtk_widget_set_size_request(app->qs_popup, pw, ph);
        gtk_window_stick(GTK_WINDOW(app->qs_popup));

        app->qs_vbox = gtk_vbox_new(FALSE, 4);
        gtk_container_add(GTK_CONTAINER(app->qs_popup), app->qs_vbox);

        app->qs_area = gtk_drawing_area_new();
        gtk_widget_set_size_request(app->qs_area, pw, ph);
        gtk_widget_add_events(app->qs_area,
            GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
        g_signal_connect(app->qs_area, "expose-event", G_CALLBACK(on_qs_expose), app);
        g_signal_connect(app->qs_area, "button-press-event", G_CALLBACK(on_qs_button_press), app);
        g_signal_connect(app->qs_area, "button-release-event", G_CALLBACK(on_qs_button_release), app);
        g_signal_connect(app->qs_area, "motion-notify-event", G_CALLBACK(on_qs_motion), app);
        gtk_box_pack_start(GTK_BOX(app->qs_vbox), app->qs_area, TRUE, TRUE, 0);
        gtk_widget_show(app->qs_area);

        app->qs_pass_box = gtk_hbox_new(FALSE, 6);
        gtk_container_set_border_width(GTK_CONTAINER(app->qs_pass_box), 8);
        app->qs_pass_entry = gtk_entry_new();
        gtk_entry_set_visibility(GTK_ENTRY(app->qs_pass_entry), FALSE);
        gtk_widget_set_size_request(app->qs_pass_entry, 200, 28);
        GtkWidget *btn_ok = gtk_button_new_with_label("Connect");
        g_signal_connect(app->qs_pass_entry, "activate", G_CALLBACK(on_wifi_pass_submit), app);
        g_signal_connect(btn_ok, "clicked", G_CALLBACK(on_wifi_pass_submit), app);
        gtk_box_pack_start(GTK_BOX(app->qs_pass_box), app->qs_pass_entry, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(app->qs_pass_box), btn_ok, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(app->qs_vbox), app->qs_pass_box, FALSE, FALSE, 0);

        g_signal_connect(app->qs_popup, "focus-out-event", G_CALLBACK(on_qs_focus_out), app);
        g_signal_connect(app->qs_popup, "key-press-event", G_CALLBACK(on_qs_key), app);
        gtk_widget_show(app->qs_vbox);
    }

    if (GTK_WIDGET_VISIBLE(app->qs_popup)) {
        gtk_widget_hide(app->qs_popup);
        return;
    }
    if ((g_get_monotonic_time() - app->qs_hidden_us) < 220000) {
        return;
    }

    qs_set_page(app, 0);
    app->qs_anim_t = 0.05;
    int px = app->width - pw - 6;
    if (px < 6) px = 6;
    gtk_window_move(GTK_WINDOW(app->qs_popup), px, PANEL_HEIGHT + 2);
    gtk_widget_show(app->qs_popup);
    gtk_window_move(GTK_WINDOW(app->qs_popup), px, PANEL_HEIGHT + 2);
    gtk_window_present(GTK_WINDOW(app->qs_popup));
    start_panel_anim(app);
    gtk_widget_queue_draw(app->qs_area);
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    PanelApp *app = (PanelApp *)data;
    if (ev->button != 1) return FALSE;

    if (pt_in_rect(ev->x, ev->y, app->r_brand)) {
        spawn_cmd("/usr/bin/phantom-overview");
    } else if (pt_in_rect(ev->x, ev->y, app->r_ws)) {
        switch_workspace_delta(1);
    } else if (pt_in_rect(ev->x, ev->y, app->r_term)) {
        spawn_cmd("xterm -u8 -ls -bg \"#181825\" -fg \"#cdd6f4\" -cr \"#89b4fa\" -bd \"#313244\"");
    } else if (pt_in_rect(ev->x, ev->y, app->r_browser)) {
        spawn_cmd("/usr/bin/firefox");
    } else if (pt_in_rect(ev->x, ev->y, app->r_files)) {
        spawn_cmd("/usr/bin/pcmanfm");
    } else if (pt_in_rect(ev->x, ev->y, app->r_ram)) {
        toggle_ram_popup(app);
    } else if (pt_in_rect(ev->x, ev->y, app->r_cpu) ||
               pt_in_rect(ev->x, ev->y, app->r_disk) ||
               pt_in_rect(ev->x, ev->y, app->r_net)) {
        spawn_cmd("/usr/bin/phantom-monitor");
    } else if (pt_in_rect(ev->x, ev->y, app->r_qs) ||
               pt_in_rect(ev->x, ev->y, app->r_clk)) {
        toggle_qs_popup(app);
    }
    return TRUE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data) {
    (void)w;
    PanelApp *app = (PanelApp *)data;
    if (pt_in_rect(ev->x, ev->y, app->r_ws)) {
        switch_workspace_delta((ev->direction == GDK_SCROLL_UP) ? -1 : 1);
    } else if (app->has_audio && pt_in_rect(ev->x, ev->y, app->r_qs_bar_vol)) {
        spawn_cmd((ev->direction == GDK_SCROLL_UP)
                  ? "amixer -q sset Master 5%+"
                  : "amixer -q sset Master 5%-");
        sample_audio(app);
        gtk_widget_queue_draw(app->area);
    } else if (pt_in_rect(ev->x, ev->y, app->r_qs_bar_bright)) {
        app->bright_pct += (ev->direction == GDK_SCROLL_UP) ? 5 : -5;
        apply_display_settings(app);
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
    if (app->ram_popup && GTK_WIDGET_VISIBLE(app->ram_popup)) {
        gtk_widget_queue_draw(app->ram_popup_area);
    }
    if (app->qs_popup && GTK_WIDGET_VISIBLE(app->qs_popup)) {
        gtk_widget_queue_draw(app->qs_area);
    }
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
