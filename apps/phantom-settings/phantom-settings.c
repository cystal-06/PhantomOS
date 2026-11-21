#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <cairo.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <ifaddrs.h>
#include <linux/rfkill.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#define MAX_MODES     32
#define MAX_WIFI_NETS 12
#define MAX_ACTIONS   32
#define SIDEBAR_W     236.0

typedef struct {
    double x, y, w, h;
} Rect;

typedef enum {
    PAGE_WIFI = 0,
    PAGE_NETWORK,
    PAGE_DISPLAYS,
    PAGE_SOUND,
    PAGE_POWER,
    PAGE_MOUSE,
    PAGE_MULTITASK,
    PAGE_APPEARANCE,
    PAGE_APPS,
    PAGE_SYSTEM,
    PAGE_COUNT
} SettingsPage;

typedef enum {
    ACT_NONE = 0,
    ACT_WIFI_TOGGLE,
    ACT_WIFI_AIRPLANE,
    ACT_WIFI_SCAN,
    ACT_WIFI_ROW,
    ACT_LAN_DHCP,
    ACT_DISP_MODE,
    ACT_DISP_NIGHT,
    ACT_SOUND_MUTE,
    ACT_POWER_BLANK,
    ACT_POWER_MON,
    ACT_WS_COUNT,
    ACT_APPEAR_WALL,
    ACT_APPEAR_THEME,
    ACT_LAUNCH_CMD,
    ACT_REBOOT,
    ACT_POWEROFF
} ActionKind;

typedef struct {
    Rect r;
    ActionKind kind;
    int idx;
    const char *str;
} ClickAction;

typedef struct {
    int width;
    int height;
    int size_idx;
} DispMode;

typedef struct {
    char ssid[64];
    int signal_pct;
    int secured;
    int connected;
} WifiNet;

typedef struct {
    GtkWidget *window;
    GtkWidget *fixed;
    GtkWidget *search_entry;
    GtkWidget *wifi_pass_entry;

    int win_w;
    int win_h;
    SettingsPage page;
    int search_open;
    char search_query[64];
    int wifi_pass_open;
    char wifi_target_ssid[64];

    double hx, hy;
    int hovering;
    int drag_slider; /* 0=none, 1=bright, 2=vol, 3=mouse */
    gint64 last_cmd_us;

    /* 60fps UI Animations */
    double page_anim_t;
    double nav_anim_y;
    double nav_target_y;
    guint anim_id;

    Rect r_search_btn;
    Rect r_about_btn;
    Rect r_nav[PAGE_COUNT];
    Rect r_bright_trk;
    Rect r_vol_trk;
    Rect r_mouse_trk;
    ClickAction actions[MAX_ACTIONS];
    int action_count;

    /* Live System State */
    char hostname[128];
    char kernel[128];
    char cpu_model[128];
    int cpu_cores;
    char root_mode[48];
    unsigned long ram_used_mb;
    unsigned long ram_total_mb;
    unsigned long zram_used_mb;
    unsigned long zram_total_mb;
    double zram_mem_mb;

    /* Wi-Fi & LAN */
    int has_wifi;
    char wifi_iface[32];
    int wifi_up;
    int wifi_connected;
    int wifi_scanning;
    char wifi_ssid[64];
    WifiNet wifi_nets[MAX_WIFI_NETS];
    int wifi_net_count;
    int airplane_mode;

    char eth_ip[32];
    char eth_mac[32];
    char eth_gw[32];
    char eth_dns[64];
    int eth_up;

    /* Display */
    DispMode modes[MAX_MODES];
    int mode_count;
    int cur_mode_w;
    int cur_mode_h;
    int has_hw_bl;
    char bl_dir[128];
    int bl_max;
    int bright_pct;
    int night_light;

    /* Sound */
    int has_audio;
    int vol_pct;
    int vol_muted;
    char sound_card[96];

    /* Power */
    int has_bat;
    int bat_pct;
    int bat_charging;
    int ac_online;
    char bat_status[32];
    int blank_minutes; /* 0 = Never, 5, 10 */

    /* Mouse & Desktop */
    int mouse_accel; /* 1..6 */
    long num_ws;
    int wall_preset; /* 0=Default, 1=Midnight, 2=DeepBlue */
    char cur_theme[64];

    char toast[128];
    gint64 toast_until_us;
} SettingsApp;

static int pt_in_rect(double px, double py, Rect r) {
    return r.w > 0 && px >= r.x && px <= (r.x + r.w) && py >= r.y && py <= (r.y + r.h);
}

static void spawn_cmd(const char *cmd) {
    GError *err = NULL;
    if (!g_spawn_command_line_async(cmd, &err)) {
        if (err) g_error_free(err);
    }
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

static void show_toast(SettingsApp *app, const char *msg) {
    g_strlcpy(app->toast, msg, sizeof(app->toast));
    app->toast_until_us = g_get_monotonic_time() + 4000000;
}

static void add_action(SettingsApp *app, Rect r, ActionKind kind, int idx, const char *str) {
    if (app->action_count >= MAX_ACTIONS) return;
    app->actions[app->action_count++] = (ClickAction){ r, kind, idx, str };
}

/* ---------- Hardware & OS Probes ---------- */

static void probe_system_info(SettingsApp *app) {
    if (gethostname(app->hostname, sizeof(app->hostname) - 1) != 0) {
        g_strlcpy(app->hostname, "phantomos", sizeof(app->hostname));
    }
    struct utsname u;
    if (uname(&u) == 0) g_strlcpy(app->kernel, u.release, sizeof(app->kernel));
    else g_strlcpy(app->kernel, "Linux", sizeof(app->kernel));

    g_strlcpy(app->cpu_model, "x86_64 Processor", sizeof(app->cpu_model));
    app->cpu_cores = 0;
    FILE *cf = fopen("/proc/cpuinfo", "r");
    if (cf) {
        char line[256];
        while (fgets(line, sizeof(line), cf)) {
            if (strncmp(line, "processor", 9) == 0) app->cpu_cores++;
            else if (strncmp(line, "model name", 10) == 0) {
                char *c = strchr(line, ':');
                if (c) {
                    c++;
                    while (*c == ' ') c++;
                    size_t l = strlen(c);
                    while (l > 0 && (c[l - 1] == '\n' || c[l - 1] == ' ')) c[--l] = '\0';
                    g_strlcpy(app->cpu_model, c, sizeof(app->cpu_model));
                }
            }
        }
        fclose(cf);
    }
    if (app->cpu_cores < 1) app->cpu_cores = 1;

    g_strlcpy(app->root_mode, "Ext4 RW", sizeof(app->root_mode));
    FILE *mf = fopen("/proc/mounts", "r");
    if (mf) {
        char line[512];
        while (fgets(line, sizeof(line), mf)) {
            if (strstr(line, " / overlay ")) {
                g_strlcpy(app->root_mode, "Stateless OverlayFS + SquashFS", sizeof(app->root_mode));
                break;
            }
        }
        fclose(mf);
    }

    FILE *pf = fopen("/proc/meminfo", "r");
    if (pf) {
        char k[64];
        unsigned long v = 0, tot = 0, fre = 0, buf = 0, cac = 0, sre = 0, stot = 0, sfre = 0;
        while (fscanf(pf, "%63s %lu kB%*[^\n]", k, &v) == 2) {
            if (strcmp(k, "MemTotal:") == 0) tot = v;
            else if (strcmp(k, "MemFree:") == 0) fre = v;
            else if (strcmp(k, "Buffers:") == 0) buf = v;
            else if (strcmp(k, "Cached:") == 0) cac = v;
            else if (strcmp(k, "SReclaimable:") == 0) sre = v;
            else if (strcmp(k, "SwapTotal:") == 0) stot = v;
            else if (strcmp(k, "SwapFree:") == 0) sfre = v;
        }
        fclose(pf);
        unsigned long rec = fre + buf + cac + sre;
        app->ram_total_mb = tot / 1024;
        app->ram_used_mb = (tot > rec) ? (tot - rec) / 1024 : 0;
        app->zram_total_mb = stot / 1024;
        app->zram_used_mb = (stot > sfre) ? (stot - sfre) / 1024 : 0;
    }

    app->zram_mem_mb = 0.0;
    FILE *zm = fopen("/sys/block/zram0/mm_stat", "r");
    if (zm) {
        unsigned long long o = 0, c = 0, m = 0;
        if (fscanf(zm, "%llu %llu %llu", &o, &c, &m) == 3) {
            app->zram_mem_mb = (double)m / (1024.0 * 1024.0);
        }
        fclose(zm);
    }
}

static void probe_display(SettingsApp *app) {
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (!dpy) return;
    Window root = DefaultRootWindow(dpy);
    XRRScreenConfiguration *sc = XRRGetScreenInfo(dpy, root);
    if (sc) {
        int nsizes = 0;
        Rotation rot = 0;
        SizeID cur = XRRConfigCurrentConfiguration(sc, &rot);
        XRRScreenSize *sizes = XRRConfigSizes(sc, &nsizes);
        app->mode_count = 0;
        for (int i = 0; i < nsizes && app->mode_count < MAX_MODES; i++) {
            if (i == cur) {
                app->cur_mode_w = sizes[i].width;
                app->cur_mode_h = sizes[i].height;
            }
            int is_cur = (i == cur);
            if (is_cur || (sizes[i].width >= 800 && sizes[i].width <= 1920 &&
                           sizes[i].height >= 600 && sizes[i].height <= 1200)) {
                int dup = 0;
                for (int j = 0; j < app->mode_count; j++) {
                    if (app->modes[j].width == sizes[i].width &&
                        app->modes[j].height == sizes[i].height) {
                        dup = 1; break;
                    }
                }
                if (!dup) {
                    app->modes[app->mode_count++] = (DispMode){ sizes[i].width, sizes[i].height, i };
                }
            }
        }
        /* Put standard resolutions in clean ascending order */
        for (int i = 0; i < app->mode_count - 1; i++) {
            for (int j = i + 1; j < app->mode_count; j++) {
                long ai = (long)app->modes[i].width * app->modes[i].height;
                long aj = (long)app->modes[j].width * app->modes[j].height;
                if (aj < ai) {
                    DispMode tmp = app->modes[i];
                    app->modes[i] = app->modes[j];
                    app->modes[j] = tmp;
                }
            }
        }
        XRRFreeScreenConfigInfo(sc);
    }

    DIR *dir = opendir("/sys/class/backlight");
    app->has_hw_bl = 0;
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char pm[384], pc[384];
            int mx = 0, cur = 0;
            snprintf(pm, sizeof(pm), "/sys/class/backlight/%s/max_brightness", de->d_name);
            snprintf(pc, sizeof(pc), "/sys/class/backlight/%s/brightness", de->d_name);
            if (read_sys_int(pm, &mx) && mx > 0 && read_sys_int(pc, &cur)) {
                app->has_hw_bl = 1;
                g_strlcpy(app->bl_dir, de->d_name, sizeof(app->bl_dir));
                app->bl_max = mx;
                if (!app->drag_slider) {
                    int pct = (int)(100.0 * cur / mx + 0.5);
                    if (pct < 15) pct = 15;
                    if (pct > 100) pct = 100;
                    app->bright_pct = pct;
                }
                break;
            }
        }
        closedir(dir);
    }
    if (app->bright_pct == 0) app->bright_pct = 100;
}

static void apply_display_gamma(SettingsApp *app) {
    if (app->bright_pct < 15) app->bright_pct = 15;
    if (app->bright_pct > 100) app->bright_pct = 100;
    if (app->has_hw_bl && app->bl_max > 0 && app->bl_dir[0]) {
        char pc[384];
        snprintf(pc, sizeof(pc), "/sys/class/backlight/%s/brightness", app->bl_dir);
        int val = (app->bl_max * app->bright_pct) / 100;
        if (val < 1) val = 1;
        FILE *fp = fopen(pc, "w");
        if (fp) { fprintf(fp, "%d\n", val); fclose(fp); }
    }
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (!dpy) return;
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, DefaultRootWindow(dpy));
    if (!res) return;
    double lum = app->has_hw_bl ? 1.0 : (0.25 + 0.75 * (app->bright_pct / 100.0));
    double mr = lum, mg = lum * (app->night_light ? 0.84 : 1.0), mb = lum * (app->night_light ? 0.65 : 1.0);
    for (int i = 0; i < res->ncrtc; i++) {
        int sz = XRRGetCrtcGammaSize(dpy, res->crtcs[i]);
        if (sz <= 1) continue;
        XRRCrtcGamma *g = XRRAllocGamma(sz);
        if (!g) continue;
        for (int j = 0; j < sz; j++) {
            double base = (65535.0 * j) / (sz - 1);
            g->red[j] = (unsigned short)(base * mr);
            g->green[j] = (unsigned short)(base * mg);
            g->blue[j] = (unsigned short)(base * mb);
        }
        XRRSetCrtcGamma(dpy, res->crtcs[i], g);
        XRRFreeGamma(g);
    }
    XRRFreeScreenResources(res);
    XFlush(dpy);
}

static void apply_resolution(SettingsApp *app, int size_idx) {
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (!dpy) return;
    Window root = DefaultRootWindow(dpy);
    XRRScreenConfiguration *sc = XRRGetScreenInfo(dpy, root);
    if (!sc) return;
    Rotation rot = 0;
    XRRConfigCurrentConfiguration(sc, &rot);
    XRRSetScreenConfig(dpy, sc, root, size_idx, rot, CurrentTime);
    XRRFreeScreenConfigInfo(sc);
    XFlush(dpy);
    probe_display(app);
    show_toast(app, "Resolution updated");
}

static void probe_network(SettingsApp *app) {
    app->eth_ip[0] = '\0';
    app->eth_mac[0] = '\0';
    app->eth_gw[0] = '\0';
    app->eth_dns[0] = '\0';
    app->eth_up = 0;

    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) == 0) {
        for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (strcmp(ifa->ifa_name, "eth0") == 0) {
                struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
                inet_ntop(AF_INET, &sa->sin_addr, app->eth_ip, sizeof(app->eth_ip));
                app->eth_up = 1;
                break;
            }
        }
        freeifaddrs(ifap);
    }
    read_sys_str("/sys/class/net/eth0/address", app->eth_mac, sizeof(app->eth_mac));

    FILE *rf = fopen("/proc/net/route", "r");
    if (rf) {
        char line[256];
        while (fgets(line, sizeof(line), rf)) {
            char iface[32];
            unsigned long dest = 0, gw = 0;
            if (sscanf(line, "%31s %lx %lx", iface, &dest, &gw) == 3 &&
                strcmp(iface, "eth0") == 0 && dest == 0 && gw != 0) {
                struct in_addr in;
                in.s_addr = (in_addr_t)gw;
                g_strlcpy(app->eth_gw, inet_ntoa(in), sizeof(app->eth_gw));
                break;
            }
        }
        fclose(rf);
    }

    FILE *df = fopen("/etc/resolv.conf", "r");
    if (df) {
        char line[256];
        while (fgets(line, sizeof(line), df)) {
            char ns[64];
            if (sscanf(line, "nameserver %63s", ns) == 1) {
                g_strlcpy(app->eth_dns, ns, sizeof(app->eth_dns));
                break;
            }
        }
        fclose(df);
    }

    /* RFKILL */
    DIR *rdir = opendir("/sys/class/rfkill");
    if (rdir) {
        struct dirent *de;
        int found = 0, blocked = 1;
        while ((de = readdir(rdir)) != NULL) {
            if (strncmp(de->d_name, "rfkill", 6) != 0) continue;
            char ps[384];
            int soft = 0;
            snprintf(ps, sizeof(ps), "/sys/class/rfkill/%s/soft", de->d_name);
            if (read_sys_int(ps, &soft)) {
                found = 1;
                if (soft == 0) blocked = 0;
            }
        }
        closedir(rdir);
        if (found) app->airplane_mode = blocked;
    }

    /* Wi-Fi */
    app->has_wifi = 0;
    app->wifi_up = 0;
    app->wifi_connected = 0;
    app->wifi_iface[0] = '\0';
    app->wifi_ssid[0] = '\0';
    DIR *nd = opendir("/sys/class/net");
    if (nd) {
        struct dirent *de;
        while ((de = readdir(nd)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char pw[384], pf[384], flg[32];
            snprintf(pw, sizeof(pw), "/sys/class/net/%s/wireless", de->d_name);
            if (access(pw, F_OK) == 0) {
                app->has_wifi = 1;
                g_strlcpy(app->wifi_iface, de->d_name, sizeof(app->wifi_iface));
                snprintf(pf, sizeof(pf), "/sys/class/net/%s/flags", de->d_name);
                if (read_sys_str(pf, flg, sizeof(flg))) {
                    app->wifi_up = (strtoul(flg, NULL, 0) & 1UL) ? 1 : 0;
                }
                break;
            }
        }
        closedir(nd);
    }

    if (app->has_wifi && app->wifi_iface[0]) {
        char sock[128];
        snprintf(sock, sizeof(sock), "/var/run/wpa_supplicant/%s", app->wifi_iface);
        if (access(sock, F_OK) == 0) {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "wpa_cli -i %s status 2>/dev/null", app->wifi_iface);
            FILE *fp = popen(cmd, "r");
            if (fp) {
                char line[256];
                int comp = 0;
                while (fgets(line, sizeof(line), fp)) {
                    size_t l = strlen(line);
                    while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
                    if (strcmp(line, "wpa_state=COMPLETED") == 0) comp = 1;
                    else if (strncmp(line, "ssid=", 5) == 0) g_strlcpy(app->wifi_ssid, line + 5, sizeof(app->wifi_ssid));
                }
                pclose(fp);
                app->wifi_connected = (comp && app->wifi_ssid[0]) ? 1 : 0;
            }
        }
    }
}

static void read_wifi_scan(SettingsApp *app) {
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
            int dup = 0;
            for (int i = 0; i < app->wifi_net_count; i++) {
                if (strcmp(app->wifi_nets[i].ssid, ssid) == 0) { dup = 1; break; }
            }
            if (dup) continue;
            if (app->wifi_net_count < MAX_WIFI_NETS) {
                WifiNet *n = &app->wifi_nets[app->wifi_net_count++];
                g_strlcpy(n->ssid, ssid, sizeof(n->ssid));
                n->signal_pct = pct;
                n->secured = sec;
                n->connected = (app->wifi_connected && strcmp(app->wifi_ssid, ssid) == 0) ? 1 : 0;
            }
        }
    }
    pclose(fp);
}

static void probe_sound_and_power(SettingsApp *app) {
    app->has_audio = (access("/proc/asound/card0", F_OK) == 0 && access("/usr/bin/amixer", X_OK) == 0);
    g_strlcpy(app->sound_card, app->has_audio ? "Intel HDA / ALSA Audio" : "No Audio Card", sizeof(app->sound_card));
    if (app->has_audio) {
        FILE *cf = fopen("/proc/asound/cards", "r");
        if (cf) {
            char line[256];
            while (fgets(line, sizeof(line), cf)) {
                char *br = strchr(line, '-');
                if (br && line[0] == ' ') {
                    br++;
                    while (*br == ' ') br++;
                    size_t l = strlen(br);
                    while (l > 0 && br[l - 1] == '\n') br[--l] = '\0';
                    g_strlcpy(app->sound_card, br, sizeof(app->sound_card));
                    break;
                }
            }
            fclose(cf);
        }
        if (!app->drag_slider && (app->page == PAGE_SOUND || app->vol_pct == 0)) {
            FILE *fp = popen("amixer get Master 2>/dev/null", "r");
            if (fp) {
                char line[256];
                while (fgets(line, sizeof(line), fp)) {
                    char *p = strchr(line, '[');
                    int pct = 0;
                    if (p && strchr(line, '%') && sscanf(p, "[%d%%]", &pct) == 1) {
                        app->vol_pct = pct;
                        app->vol_muted = (strstr(line, "[off]") != NULL);
                        break;
                    }
                }
                pclose(fp);
            }
        }
    }

    app->has_bat = 0;
    app->bat_charging = 0;
    app->ac_online = 0;
    DIR *dir = opendir("/sys/class/power_supply");
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char pt[384], pv[384], typ[64];
            snprintf(pt, sizeof(pt), "/sys/class/power_supply/%s/type", de->d_name);
            if (!read_sys_str(pt, typ, sizeof(typ))) continue;
            if (strcmp(typ, "Battery") == 0 && !app->has_bat) {
                int cap = 0;
                snprintf(pv, sizeof(pv), "/sys/class/power_supply/%s/capacity", de->d_name);
                if (read_sys_int(pv, &cap)) {
                    app->has_bat = 1;
                    app->bat_pct = cap < 0 ? 0 : (cap > 100 ? 100 : cap);
                    snprintf(pv, sizeof(pv), "/sys/class/power_supply/%s/status", de->d_name);
                    read_sys_str(pv, app->bat_status, sizeof(app->bat_status));
                    app->bat_charging = (strcmp(app->bat_status, "Charging") == 0 ||
                                         strcmp(app->bat_status, "Full") == 0);
                }
            } else if (strcmp(typ, "Mains") == 0) {
                int on = 0;
                snprintf(pv, sizeof(pv), "/sys/class/power_supply/%s/online", de->d_name);
                if (read_sys_int(pv, &on) && on > 0) app->ac_online = 1;
            }
        }
        closedir(dir);
    }
}

static void probe_desktop(SettingsApp *app) {
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (dpy) {
        Window root = DefaultRootWindow(dpy);
        Atom prop = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
        Atom atype = None;
        int afmt = 0;
        unsigned long nitems = 0, after = 0;
        unsigned char *data = NULL;
        app->num_ws = 4;
        if (XGetWindowProperty(dpy, root, prop, 0, 1, False, XA_CARDINAL,
                               &atype, &afmt, &nitems, &after, &data) == Success && data) {
            if (nitems >= 1 && afmt == 32) app->num_ws = *(long *)data;
            XFree(data);
        }

        int num = 2, den = 1, thr = 4;
        XGetPointerControl(dpy, &num, &den, &thr);
        int accel = (den > 0) ? (num / den) : 2;
        if (accel < 1) accel = 1;
        if (accel > 6) accel = 6;
        if (!app->drag_slider) app->mouse_accel = accel;
    }
    if (!app->cur_theme[0]) g_strlcpy(app->cur_theme, "PhantomDark", sizeof(app->cur_theme));
}

static void probe_all(SettingsApp *app) {
    probe_system_info(app);
    probe_display(app);
    probe_network(app);
    probe_sound_and_power(app);
    probe_desktop(app);
}

/* ---------- Actions ---------- */

static void set_workspaces(SettingsApp *app, int count) {
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (dpy) {
        Window root = DefaultRootWindow(dpy);
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type = ClientMessage;
        ev.xclient.window = root;
        ev.xclient.message_type = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = count;
        XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(dpy);
    }
    app->num_ws = count;
    char msg[64];
    snprintf(msg, sizeof(msg), "Workspaces set to %d", count);
    show_toast(app, msg);
}

static void set_wallpaper_preset(SettingsApp *app, int idx) {
    app->wall_preset = idx;
    if (idx == 0) {
        spawn_cmd("pcmanfm --wallpaper-mode=stretch --set-wallpaper=/usr/share/backgrounds/phantomos.png");
        show_toast(app, "Wallpaper: PhantomOS Default");
    } else if (idx == 1) {
        spawn_cmd("pcmanfm --wallpaper-mode=color");
        show_toast(app, "Wallpaper: Midnight Solid");
    } else {
        /* Switch wallpaper file or stretch color cleanly */
        const char *home = getenv("HOME");
        if (home && home[0]) {
            char cmd[384];
            snprintf(cmd, sizeof(cmd),
                     "sh -c 'sed -i \"s/^desktop_bg=.*/desktop_bg=#0b1220/\" \"%s/.config/pcmanfm/default/desktop-items-0.conf\" 2>/dev/null; pcmanfm --wallpaper-mode=color'",
                     home);
            spawn_cmd(cmd);
        } else {
            spawn_cmd("pcmanfm --wallpaper-mode=color");
        }
        show_toast(app, "Wallpaper: Deep Ocean Dark");
    }
}

static void set_fluxbox_theme(SettingsApp *app, const char *style_name) {
    g_strlcpy(app->cur_theme, style_name, sizeof(app->cur_theme));
    const char *home = getenv("HOME");
    if (home && home[0]) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "sh -c 'mkdir -p \"%s/.fluxbox\"; [ -f \"%s/.fluxbox/init\" ] || cp /usr/share/fluxbox/init \"%s/.fluxbox/init\"; "
                 "sed -i \"s|^session.styleFile:.*|session.styleFile: /usr/share/fluxbox/styles/%s|\" \"%s/.fluxbox/init\"; "
                 "killall -USR2 fluxbox 2>/dev/null || true'",
                 home, home, home, style_name, home);
        spawn_cmd(cmd);
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "Window Theme: %s", style_name);
    show_toast(app, msg);
}

static void toggle_airplane(SettingsApp *app) {
    int block = !app->airplane_mode;
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
    app->airplane_mode = block;
    if (app->has_wifi && app->wifi_iface[0]) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "sudo -n /usr/libexec/phantom-net %s %s",
                 block ? "wifi-down" : "wifi-up", app->wifi_iface);
        spawn_cmd(cmd);
    }
    show_toast(app, block ? "Airplane mode enabled" : "Airplane mode disabled");
}

static gboolean on_wifi_refresh_cb(gpointer data) {
    SettingsApp *app = (SettingsApp *)data;
    app->wifi_scanning = 0;
    probe_network(app);
    read_wifi_scan(app);
    gtk_widget_queue_draw(app->window);
    return FALSE;
}

static void connect_wifi_async(SettingsApp *app, const char *ssid, const char *pass) {
    if (!app->has_wifi || !app->wifi_iface[0]) return;
    char *argv[] = {
        "/usr/bin/sudo", "-n", "/usr/libexec/phantom-net",
        "wifi-connect", app->wifi_iface, (char *)ssid, NULL
    };
    gint in_fd = -1;
    GError *err = NULL;
    if (g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                                 NULL, &in_fd, NULL, NULL, &err)) {
        if (in_fd >= 0) {
            if (pass && pass[0]) { ssize_t r1 = write(in_fd, pass, strlen(pass)); (void)r1; }
            ssize_t r2 = write(in_fd, "\n", 1); (void)r2;
            close(in_fd);
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "Connecting to %s...", ssid);
        show_toast(app, msg);
        g_timeout_add(2500, on_wifi_refresh_cb, app);
        g_timeout_add(5500, on_wifi_refresh_cb, app);
    } else if (err) {
        g_error_free(err);
    }
}

/* ---------- Cairo Primitives ---------- */

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
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, sz);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, txt);
    cairo_new_path(cr);
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

static void draw_nav_icon(cairo_t *cr, SettingsPage pg, double x, double y, int active) {
    if (active) cairo_set_source_rgb(cr, 0.537, 0.706, 0.980);
    else cairo_set_source_rgb(cr, 0.745, 0.776, 0.878);
    cairo_set_line_width(cr, 1.4);

    switch (pg) {
    case PAGE_WIFI:
        cairo_new_sub_path(cr); cairo_arc(cr, x + 8, y + 12.5, 1.3, 0, 2 * M_PI); cairo_fill(cr);
        cairo_new_sub_path(cr); cairo_arc(cr, x + 8, y + 12.5, 4.3, -2.35, -0.79); cairo_stroke(cr);
        cairo_new_sub_path(cr); cairo_arc(cr, x + 8, y + 12.5, 7.6, -2.35, -0.79); cairo_stroke(cr);
        break;
    case PAGE_NETWORK:
        cairo_rectangle(cr, x + 5.5, y + 2, 5, 3.5);
        cairo_rectangle(cr, x + 1.5, y + 10.5, 4.5, 3.5);
        cairo_rectangle(cr, x + 10, y + 10.5, 4.5, 3.5);
        cairo_stroke(cr);
        cairo_move_to(cr, x + 8, y + 5.5); cairo_line_to(cr, x + 8, y + 8);
        cairo_move_to(cr, x + 3.8, y + 8); cairo_line_to(cr, x + 12.2, y + 8);
        cairo_move_to(cr, x + 3.8, y + 8); cairo_line_to(cr, x + 3.8, y + 10.5);
        cairo_move_to(cr, x + 12.2, y + 8); cairo_line_to(cr, x + 12.2, y + 10.5);
        cairo_stroke(cr);
        break;
    case PAGE_DISPLAYS:
        rounded_rect(cr, x + 1.5, y + 2.5, 13, 9, 1.5); cairo_stroke(cr);
        cairo_move_to(cr, x + 8, y + 11.5); cairo_line_to(cr, x + 8, y + 14);
        cairo_move_to(cr, x + 4.5, y + 14); cairo_line_to(cr, x + 11.5, y + 14);
        cairo_stroke(cr);
        break;
    case PAGE_SOUND:
        cairo_move_to(cr, x + 2, y + 6); cairo_line_to(cr, x + 5, y + 6);
        cairo_line_to(cr, x + 8.5, y + 3); cairo_line_to(cr, x + 8.5, y + 13);
        cairo_line_to(cr, x + 5, y + 10); cairo_line_to(cr, x + 2, y + 10);
        cairo_close_path(cr); cairo_fill(cr);
        cairo_new_sub_path(cr); cairo_arc(cr, x + 9.2, y + 8, 3.0, -0.7, 0.7); cairo_stroke(cr);
        cairo_new_sub_path(cr); cairo_arc(cr, x + 9.2, y + 8, 5.2, -0.7, 0.7); cairo_stroke(cr);
        break;
    case PAGE_POWER:
        rounded_rect(cr, x + 2, y + 4, 10.5, 8, 1.5); cairo_stroke(cr);
        cairo_rectangle(cr, x + 12.5, y + 6, 1.8, 4); cairo_fill(cr);
        cairo_rectangle(cr, x + 3.8, y + 5.8, 5.5, 4.4); cairo_fill(cr);
        break;
    case PAGE_MOUSE:
        rounded_rect(cr, x + 4, y + 2, 8, 12, 4.0); cairo_stroke(cr);
        cairo_move_to(cr, x + 8, y + 4); cairo_line_to(cr, x + 8, y + 7); cairo_stroke(cr);
        break;
    case PAGE_MULTITASK:
        rounded_rect(cr, x + 1.5, y + 4.5, 8.5, 7.5, 1.5); cairo_stroke(cr);
        rounded_rect(cr, x + 6, y + 2.5, 8.5, 7.5, 1.5); cairo_stroke(cr);
        break;
    case PAGE_APPEARANCE:
        rounded_rect(cr, x + 2, y + 2.5, 12, 11, 2.0); cairo_stroke(cr);
        cairo_move_to(cr, x + 3.5, y + 11.5); cairo_line_to(cr, x + 7, y + 7.5);
        cairo_line_to(cr, x + 9.5, y + 10); cairo_line_to(cr, x + 12.5, y + 6.5);
        cairo_stroke(cr);
        break;
    case PAGE_APPS:
        rounded_rect(cr, x + 2.5, y + 2.5, 4.5, 4.5, 1.0);
        rounded_rect(cr, x + 9.0, y + 2.5, 4.5, 4.5, 1.0);
        rounded_rect(cr, x + 2.5, y + 9.0, 4.5, 4.5, 1.0);
        rounded_rect(cr, x + 9.0, y + 9.0, 4.5, 4.5, 1.0);
        cairo_stroke(cr);
        break;
    case PAGE_SYSTEM:
        cairo_new_sub_path(cr); cairo_arc(cr, x + 8, y + 8, 6.0, 0, 2 * M_PI); cairo_stroke(cr);
        cairo_new_sub_path(cr); cairo_arc(cr, x + 8, y + 5.2, 0.9, 0, 2 * M_PI); cairo_fill(cr);
        cairo_move_to(cr, x + 8, y + 7.4); cairo_line_to(cr, x + 8, y + 11.5); cairo_stroke(cr);
        break;
    default: break;
    }
}

static void draw_switch(cairo_t *cr, double rx, double cy, int on) {
    double sw = 38.0, sh = 20.0;
    double sx = rx - sw, sy = cy - sh * 0.5;
    rounded_rect(cr, sx, sy, sw, sh, 10.0);
    if (on) cairo_set_source_rgb(cr, 0.208, 0.518, 0.894);
    else cairo_set_source_rgb(cr, 0.243, 0.263, 0.349);
    cairo_fill(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, on ? (sx + sw - 10.0) : (sx + 10.0), cy, 7.5, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0.95, 0.97, 1.0);
    cairo_fill(cr);
}

static void draw_pill_btn(cairo_t *cr, SettingsApp *app, Rect r, const char *txt, int accent) {
    int hov = app->hovering && pt_in_rect(app->hx, app->hy, r);
    rounded_rect(cr, r.x, r.y, r.w, r.h, 8.0);
    if (accent) cairo_set_source_rgb(cr, hov ? 0.26 : 0.21, hov ? 0.57 : 0.52, hov ? 0.94 : 0.89);
    else cairo_set_source_rgb(cr, hov ? 0.24 : 0.18, hov ? 0.26 : 0.20, hov ? 0.35 : 0.28);
    cairo_fill(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 9.5);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, txt, &ext);
    cairo_set_source_rgb(cr, accent ? 1.0 : 0.85, accent ? 1.0 : 0.88, accent ? 1.0 : 0.96);
    cairo_move_to(cr, r.x + (r.w - ext.width) * 0.5 - ext.x_bearing,
                  r.y + (r.h + ext.height) * 0.5);
    cairo_show_text(cr, txt);
    cairo_new_path(cr);
}

static void draw_slider(cairo_t *cr, double x, double cy, double w, double frac, Rect *out_trk) {
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    *out_trk = (Rect){ x, cy - 12.0, w, 24.0 };
    rounded_rect(cr, x, cy - 2.5, w, 5.0, 2.5);
    cairo_set_source_rgb(cr, 0.23, 0.25, 0.33);
    cairo_fill(cr);
    double fw = w * frac;
    if (fw > 1.0) {
        rounded_rect(cr, x, cy - 2.5, fw, 5.0, 2.5);
        cairo_set_source_rgb(cr, 0.208, 0.518, 0.894);
        cairo_fill(cr);
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + fw, cy, 7.0, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0.95, 0.97, 1.0);
    cairo_fill(cr);
}

static void draw_row_hover(cairo_t *cr, SettingsApp *app, Rect r) {
    if (app->hovering && pt_in_rect(app->hx, app->hy, r)) {
        rounded_rect(cr, r.x + 3, r.y + 3, r.w - 6, r.h - 6, 8.0);
        cairo_set_source_rgb(cr, 0.145, 0.161, 0.227);
        cairo_fill(cr);
    }
}

static double draw_card_box(cairo_t *cr, double x, double y, double w, int rows, double row_h) {
    double h = rows * row_h;
    rounded_rect(cr, x, y, w, h, 12.0);
    cairo_set_source_rgb(cr, 0.114, 0.125, 0.173);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.176, 0.192, 0.267);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.157, 0.173, 0.239);
    for (int i = 1; i < rows; i++) {
        double ly = y + i * row_h + 0.5;
        cairo_move_to(cr, x + 14.0, ly);
        cairo_line_to(cr, x + w - 14.0, ly);
    }
    cairo_stroke(cr);
    return h;
}

static const char *PAGE_LABELS[PAGE_COUNT] = {
    "Wi-Fi", "Network", "Displays", "Sound", "Power",
    "Mouse & Input", "Multitasking", "Appearance", "Apps", "System"
};

static int nav_matches(const char *label, const char *q) {
    if (!q || !q[0]) return 1;
    char l1[64], l2[64];
    size_t i;
    for (i = 0; label[i] && i < sizeof(l1) - 1; i++) l1[i] = (char)tolower((unsigned char)label[i]);
    l1[i] = '\0';
    for (i = 0; q[i] && i < sizeof(l2) - 1; i++) l2[i] = (char)tolower((unsigned char)q[i]);
    l2[i] = '\0';
    return strstr(l1, l2) != NULL;
}

static gboolean on_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
    (void)event;
    SettingsApp *app = (SettingsApp *)data;
    cairo_t *cr = gdk_cairo_create(widget->window);
    int w = widget->allocation.width;
    int h = widget->allocation.height;
    app->win_w = w;
    app->win_h = h;
    app->action_count = 0;
    app->r_bright_trk = app->r_vol_trk = app->r_mouse_trk = (Rect){0,0,0,0};

    /* Background: Content dark + Sidebar surface */
    cairo_set_source_rgb(cr, 0.075, 0.082, 0.114);
    cairo_rectangle(cr, SIDEBAR_W, 0, w - SIDEBAR_W, h);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.102, 0.110, 0.153);
    cairo_rectangle(cr, 0, 0, SIDEBAR_W, h);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.165, 0.180, 0.251);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, SIDEBAR_W + 0.5, 0);
    cairo_line_to(cr, SIDEBAR_W + 0.5, h);
    cairo_stroke(cr);

    /* Sidebar Header */
    app->r_search_btn = (Rect){ 12, 10, 28, 28 };
    int shov = app->hovering && pt_in_rect(app->hx, app->hy, app->r_search_btn);
    if (shov || app->search_open) {
        rounded_rect(cr, 12, 10, 28, 28, 8.0);
        cairo_set_source_rgb(cr, 0.19, 0.21, 0.29);
        cairo_fill(cr);
    }
    cairo_set_source_rgb(cr, 0.80, 0.84, 0.95);
    cairo_set_line_width(cr, 1.5);
    cairo_new_sub_path(cr);
    cairo_arc(cr, 24.5, 22.5, 4.5, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, 27.8, 25.8); cairo_line_to(cr, 31.0, 29.0); cairo_stroke(cr);

    draw_text(cr, 84, 29, "Settings", 0.90, 0.92, 0.97, 1, 12.5);

    app->r_about_btn = (Rect){ SIDEBAR_W - 40, 10, 28, 28 };
    int ahov = app->hovering && pt_in_rect(app->hx, app->hy, app->r_about_btn);
    if (ahov) {
        rounded_rect(cr, SIDEBAR_W - 40, 10, 28, 28, 8.0);
        cairo_set_source_rgb(cr, 0.19, 0.21, 0.29);
        cairo_fill(cr);
    }
    cairo_set_source_rgb(cr, 0.75, 0.78, 0.88);
    cairo_set_line_width(cr, 1.5);
    for (int i = 0; i < 3; i++) {
        cairo_move_to(cr, SIDEBAR_W - 32, 19 + i * 5);
        cairo_line_to(cr, SIDEBAR_W - 20, 19 + i * 5);
    }
    cairo_stroke(cr);

    double ny = app->search_open ? 88.0 : 52.0;
    if (app->search_open) {
        rounded_rect(cr, 12, 46, SIDEBAR_W - 24, 30, 8.0);
        cairo_set_source_rgb(cr, 0.065, 0.072, 0.102);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.208, 0.518, 0.894);
        cairo_stroke(cr);
    }

    /* Grouped Sidebar Navigation */
    double active_ny = -1.0;
    Rect nav_rects[PAGE_COUNT];
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (!nav_matches(PAGE_LABELS[i], app->search_query)) {
            app->r_nav[i] = (Rect){0,0,0,0};
            nav_rects[i] = (Rect){0,0,0,0};
            continue;
        }
        if (!app->search_open && (i == PAGE_DISPLAYS || i == PAGE_MULTITASK || i == PAGE_APPS)) {
            cairo_set_source_rgb(cr, 0.157, 0.173, 0.239);
            cairo_move_to(cr, 14, ny + 4.5);
            cairo_line_to(cr, SIDEBAR_W - 14, ny + 4.5);
            cairo_stroke(cr);
            ny += 10.0;
        }
        Rect nr = { 10, ny, SIDEBAR_W - 20, 36 };
        app->r_nav[i] = nr;
        nav_rects[i] = nr;
        if (app->page == (SettingsPage)i) active_ny = nr.y;
        ny += 40.0;
    }

    if (active_ny >= 0.0) {
        app->nav_target_y = active_ny;
        if (app->nav_anim_y <= 0.0) app->nav_anim_y = active_ny;
        rounded_rect(cr, 10, app->nav_anim_y, SIDEBAR_W - 20, 36, 8.0);
        cairo_set_source_rgb(cr, 0.180, 0.204, 0.286);
        cairo_fill(cr);
    }

    for (int i = 0; i < PAGE_COUNT; i++) {
        Rect nr = nav_rects[i];
        if (nr.w <= 0) continue;
        int active = (app->page == (SettingsPage)i);
        int hov = app->hovering && pt_in_rect(app->hx, app->hy, nr);
        if (hov && !active) {
            rounded_rect(cr, nr.x, nr.y, nr.w, nr.h, 8.0);
            cairo_set_source_rgb(cr, 0.141, 0.157, 0.220);
            cairo_fill(cr);
        }
        draw_nav_icon(cr, (SettingsPage)i, nr.x + 12, nr.y + 10, active);
        draw_text(cr, nr.x + 38, nr.y + 22.5, PAGE_LABELS[i],
                  active ? 0.94 : 0.80, active ? 0.96 : 0.84, active ? 0.99 : 0.92,
                  active, 10.8);
    }

    /* Content Area (with smooth 60fps slide & fade on tab switch) */
    double area_w = w - SIDEBAR_W;
    double cw = area_w - 64.0;
    if (cw > 520.0) cw = 520.0;
    if (cw < 320.0) cw = area_w - 24.0;
    double cx = SIDEBAR_W + (area_w - cw) * 0.5;
    double y = 38.0;

    double pt = (app->page_anim_t > 0.0) ? app->page_anim_t : 1.0;
    double pe = 1.0 - pow(1.0 - pt, 3.0);
    if (pe < 0.999) {
        cairo_save(cr);
        cairo_rectangle(cr, SIDEBAR_W + 1, 0, w - SIDEBAR_W - 1, h);
        cairo_clip(cr);
        cairo_push_group(cr);
        cairo_translate(cr, 0.0, (1.0 - pe) * 14.0);
    }

    draw_text(cr, cx, y, PAGE_LABELS[app->page], 0.93, 0.95, 0.99, 1, 16.5);
    y += 28.0;

    double rh = 48.0;

    switch (app->page) {
    case PAGE_WIFI: {
        draw_card_box(cr, cx, y, cw, 2, rh);
        if (app->has_wifi) {
            Rect rt = { cx, y, cw, rh };
            draw_row_hover(cr, app, rt);
            draw_switch(cr, cx + cw - 16, y + 24, app->wifi_up && !app->airplane_mode);
            add_action(app, rt, ACT_WIFI_TOGGLE, 0, NULL);
        } else {
            draw_text_right(cr, cx + cw - 16, y + 29, "No Wi-Fi Adapter", 0.62, 0.65, 0.76, 0, 10.0);
        }
        draw_text(cr, cx + 16, y + 29, "Wi-Fi Radio", 0.88, 0.90, 0.96, 1, 10.5);

        double y2 = y + rh;
        Rect ra = { cx, y2, cw, rh };
        draw_row_hover(cr, app, ra);
        draw_text(cr, cx + 16, y2 + 29, "Airplane Mode", 0.88, 0.90, 0.96, 1, 10.5);
        draw_switch(cr, cx + cw - 16, y2 + 24, app->airplane_mode);
        add_action(app, ra, ACT_WIFI_AIRPLANE, 0, NULL);

        y += 2 * rh + 18.0;
        draw_text(cr, cx + 2, y, "Visible Networks", 0.68, 0.72, 0.82, 1, 10.5);
        Rect rscan = { cx + cw - 82, y - 16, 82, 26 };
        draw_pill_btn(cr, app, rscan, app->wifi_scanning ? "Scanning..." : "Scan", 1);
        add_action(app, rscan, ACT_WIFI_SCAN, 0, NULL);
        y += 12.0;

        if (!app->has_wifi) {
            draw_card_box(cr, cx, y, cw, 1, 64.0);
            draw_text(cr, cx + 16, y + 27, "No wireless card detected inside current machine.", 0.86, 0.88, 0.94, 0, 10.2);
            draw_text(cr, cx + 16, y + 46, "Realtek rtw88/89, Intel iwlwifi, Atheros & MediaTek modules are ready.", 0.56, 0.60, 0.70, 0, 9.2);
        } else if (app->wifi_net_count == 0) {
            draw_card_box(cr, cx, y, cw, 1, 52.0);
            draw_text(cr, cx + 16, y + 31, "Click Scan to search for nearby Wi-Fi networks.", 0.65, 0.69, 0.80, 0, 10.0);
        } else {
            int n = app->wifi_net_count < 6 ? app->wifi_net_count : 6;
            draw_card_box(cr, cx, y, cw, n, 44.0);
            for (int i = 0; i < n; i++) {
                double ry = y + i * 44.0;
                Rect rr = { cx, ry, cw, 44 };
                draw_row_hover(cr, app, rr);
                draw_text(cr, cx + 18, ry + 26, app->wifi_nets[i].ssid, 0.90, 0.93, 0.98, 1, 10.5);
                if (app->wifi_nets[i].connected) {
                    draw_text_right(cr, cx + cw - 16, ry + 26, "Connected", 0.65, 0.89, 0.63, 1, 9.8);
                } else {
                    char sb[24];
                    snprintf(sb, sizeof(sb), "%s  %d%%", app->wifi_nets[i].secured ? "WPA" : "Open", app->wifi_nets[i].signal_pct);
                    draw_text_right(cr, cx + cw - 16, ry + 26, sb, 0.62, 0.66, 0.77, 0, 9.5);
                }
                add_action(app, rr, ACT_WIFI_ROW, i, NULL);
            }
        }
        break;
    }
    case PAGE_NETWORK: {
        draw_card_box(cr, cx, y, cw, 5, rh);
        const char *k[5] = { "Interface", "IPv4 Address", "MAC Address", "Default Gateway", "DNS Server" };
        const char *v[5] = {
            app->eth_up ? "eth0 (Connected)" : "eth0 (Disconnected)",
            app->eth_ip[0] ? app->eth_ip : "None",
            app->eth_mac[0] ? app->eth_mac : "Unknown",
            app->eth_gw[0] ? app->eth_gw : "-",
            app->eth_dns[0] ? app->eth_dns : "-"
        };
        for (int i = 0; i < 5; i++) {
            double ry = y + i * rh;
            draw_text(cr, cx + 16, ry + 29, k[i], 0.85, 0.88, 0.95, 1, 10.5);
            draw_text_right(cr, cx + cw - 16, ry + 29, v[i],
                            i == 0 && app->eth_up ? 0.65 : 0.72,
                            i == 0 && app->eth_up ? 0.89 : 0.76,
                            i == 0 && app->eth_up ? 0.63 : 0.87, i == 1, 10.2);
        }
        y += 5 * rh + 16.0;
        Rect rdhcp = { cx, y, 134, 34 };
        draw_pill_btn(cr, app, rdhcp, "Renew DHCP", 1);
        add_action(app, rdhcp, ACT_LAN_DHCP, 0, NULL);
        break;
    }
    case PAGE_DISPLAYS: {
        draw_card_box(cr, cx, y, cw, 3, 52.0);
        /* Brightness */
        draw_text(cr, cx + 16, y + 31, "Brightness", 0.88, 0.90, 0.96, 1, 10.5);
        draw_slider(cr, cx + 135, y + 26, cw - 196, (app->bright_pct - 15) / 85.0, &app->r_bright_trk);
        char bbuf[16]; snprintf(bbuf, sizeof(bbuf), "%d%%", app->bright_pct);
        draw_text_right(cr, cx + cw - 16, y + 31, bbuf, 0.73, 0.76, 0.87, 0, 10.0);

        /* Night Light */
        double y2 = y + 52.0;
        Rect rnl = { cx, y2, cw, 52.0 };
        draw_row_hover(cr, app, rnl);
        draw_text(cr, cx + 16, y2 + 31, "Night Light (Warm 4200K)", 0.88, 0.90, 0.96, 1, 10.5);
        draw_switch(cr, cx + cw - 16, y2 + 26, app->night_light);
        add_action(app, rnl, ACT_DISP_NIGHT, 0, NULL);

        /* Current info */
        double y3 = y2 + 52.0;
        draw_text(cr, cx + 16, y3 + 31, "Backlight Backend", 0.88, 0.90, 0.96, 1, 10.5);
        draw_text_right(cr, cx + cw - 16, y3 + 31, app->has_hw_bl ? app->bl_dir : "XRandR Gamma", 0.65, 0.70, 0.82, 0, 10.0);

        y += 3 * 52.0 + 18.0;
        draw_text(cr, cx + 2, y, "Resolution", 0.68, 0.72, 0.82, 1, 10.5);
        y += 12.0;
        int show_m = app->mode_count < 6 ? app->mode_count : 6;
        if (show_m > 0) {
            draw_card_box(cr, cx, y, cw, show_m, 42.0);
            for (int i = 0; i < show_m; i++) {
                double ry = y + i * 42.0;
                Rect rm = { cx, ry, cw, 42.0 };
                draw_row_hover(cr, app, rm);
                char rb[32];
                snprintf(rb, sizeof(rb), "%d x %d", app->modes[i].width, app->modes[i].height);
                int cur = (app->modes[i].width == app->cur_mode_w && app->modes[i].height == app->cur_mode_h);
                draw_text(cr, cx + 16, ry + 26, rb, 0.90, 0.92, 0.97, cur, 10.5);
                if (cur) draw_text_right(cr, cx + cw - 16, ry + 26, "Active", 0.54, 0.71, 0.98, 1, 9.8);
                add_action(app, rm, ACT_DISP_MODE, app->modes[i].size_idx, NULL);
            }
        }
        break;
    }
    case PAGE_SOUND: {
        draw_card_box(cr, cx, y, cw, 3, 52.0);
        draw_text(cr, cx + 16, y + 31, "Output Device", 0.88, 0.90, 0.96, 1, 10.5);
        draw_text_right(cr, cx + cw - 16, y + 31, app->sound_card, 0.68, 0.73, 0.85, 0, 10.0);

        double y2 = y + 52.0;
        draw_text(cr, cx + 16, y2 + 31, "Master Volume", 0.88, 0.90, 0.96, 1, 10.5);
        draw_slider(cr, cx + 145, y2 + 26, cw - 205, (app->vol_muted ? 0 : app->vol_pct) / 100.0, &app->r_vol_trk);
        char vbuf[16]; snprintf(vbuf, sizeof(vbuf), "%d%%", app->vol_muted ? 0 : app->vol_pct);
        draw_text_right(cr, cx + cw - 16, y2 + 31, vbuf, 0.73, 0.76, 0.87, 0, 10.0);

        double y3 = y2 + 52.0;
        Rect rm = { cx, y3, cw, 52.0 };
        draw_row_hover(cr, app, rm);
        draw_text(cr, cx + 16, y3 + 31, "Mute Output", 0.88, 0.90, 0.96, 1, 10.5);
        draw_switch(cr, cx + cw - 16, y3 + 26, app->vol_muted);
        add_action(app, rm, ACT_SOUND_MUTE, 0, NULL);
        break;
    }
    case PAGE_POWER: {
        draw_card_box(cr, cx, y, cw, 4, rh);
        char batv[64], zrv[64], scv[32];
        if (app->has_bat) snprintf(batv, sizeof(batv), "%d%% (%s)", app->bat_pct, app->bat_status);
        else g_strlcpy(batv, "AC Power (Desktop / VM)", sizeof(batv));

        if (app->zram_total_mb > 0) snprintf(zrv, sizeof(zrv), "%lu / %lu MB (zstd %.1fM RAM)", app->zram_used_mb, app->zram_total_mb, app->zram_mem_mb);
        else g_strlcpy(zrv, "Disabled", sizeof(zrv));

        if (app->blank_minutes == 0) g_strlcpy(scv, "Never", sizeof(scv));
        else snprintf(scv, sizeof(scv), "%d minutes", app->blank_minutes);

        draw_text(cr, cx + 16, y + 29, "Power Supply", 0.88, 0.90, 0.96, 1, 10.5);
        draw_text_right(cr, cx + cw - 16, y + 29, batv, 0.65, 0.89, 0.63, 1, 10.0);

        draw_text(cr, cx + 16, y + rh + 29, "ZRAM Swap (100% RAM)", 0.88, 0.90, 0.96, 1, 10.5);
        draw_text_right(cr, cx + cw - 16, y + rh + 29, zrv, 0.80, 0.65, 0.97, 0, 10.0);

        Rect rbl_row = { cx, y + 2 * rh, cw, rh };
        draw_row_hover(cr, app, rbl_row);
        draw_text(cr, cx + 16, y + 2 * rh + 29, "Screen Blank Timeout", 0.88, 0.90, 0.96, 1, 10.5);
        Rect rbl = { cx + cw - 110, y + 2 * rh + 11, 96, 26 };
        draw_pill_btn(cr, app, rbl, scv, 0);
        add_action(app, rbl_row, ACT_POWER_BLANK, 0, NULL);

        Rect rmon_row = { cx, y + 3 * rh, cw, rh };
        draw_row_hover(cr, app, rmon_row);
        draw_text(cr, cx + 16, y + 3 * rh + 29, "Live Resource Usage", 0.88, 0.90, 0.96, 1, 10.5);
        Rect rmon = { cx + cw - 134, y + 3 * rh + 11, 120, 26 };
        draw_pill_btn(cr, app, rmon, "System Monitor", 1);
        add_action(app, rmon_row, ACT_POWER_MON, 0, NULL);

        y += 4 * rh + 18.0;
        Rect rreb = { cx, y, 104, 34 };
        Rect rpwr = { cx + 114, y, 110, 34 };
        draw_pill_btn(cr, app, rreb, "Reboot", 0);
        draw_pill_btn(cr, app, rpwr, "Power Off", 0);
        add_action(app, rreb, ACT_REBOOT, 0, NULL);
        add_action(app, rpwr, ACT_POWEROFF, 0, NULL);
        break;
    }
    case PAGE_MOUSE: {
        draw_card_box(cr, cx, y, cw, 3, 52.0);
        draw_text(cr, cx + 16, y + 31, "Pointer Speed", 0.88, 0.90, 0.96, 1, 10.5);
        draw_slider(cr, cx + 145, y + 26, cw - 200, (app->mouse_accel - 1) / 5.0, &app->r_mouse_trk);
        char mb[16]; snprintf(mb, sizeof(mb), "%dx", app->mouse_accel);
        draw_text_right(cr, cx + cw - 16, y + 31, mb, 0.73, 0.76, 0.87, 1, 10.0);

        draw_text(cr, cx + 16, y + 52 + 31, "Workspace Gestures", 0.88, 0.90, 0.96, 1, 10.5);
        draw_text_right(cr, cx + cw - 16, y + 52 + 31, "Win+Scroll / 3-Finger Swipe", 0.65, 0.89, 0.63, 0, 9.8);

        draw_text(cr, cx + 16, y + 104 + 31, "Window Drag & Maximize", 0.88, 0.90, 0.96, 1, 10.5);
        draw_text_right(cr, cx + cw - 16, y + 104 + 31, "Alt+Drag / Double-Click Title", 0.68, 0.72, 0.84, 0, 9.8);
        break;
    }
    case PAGE_MULTITASK: {
        draw_card_box(cr, cx, y, cw, 2, 54.0);
        draw_text(cr, cx + 16, y + 32, "Workspaces", 0.88, 0.90, 0.96, 1, 10.5);
        int vals[3] = { 2, 4, 6 };
        const char *lbls[3] = { "2", "4", "6" };
        for (int i = 0; i < 3; i++) {
            Rect rw = { cx + cw - 156 + i * 48, y + 14, 40, 26 };
            draw_pill_btn(cr, app, rw, lbls[i], app->num_ws == vals[i]);
            add_action(app, rw, ACT_WS_COUNT, vals[i], NULL);
        }

        draw_text(cr, cx + 16, y + 54 + 32, "Workspace Bar Indicator", 0.88, 0.90, 0.96, 1, 10.5);
        draw_text_right(cr, cx + cw - 16, y + 54 + 32, "Active in Top & Bottom Bars", 0.65, 0.89, 0.63, 0, 9.8);
        break;
    }
    case PAGE_APPEARANCE: {
        draw_card_box(cr, cx, y, cw, 3, 48.0);
        const char *wname[3] = { "PhantomOS Default", "Midnight Solid Dark", "Deep Ocean Slate" };
        for (int i = 0; i < 3; i++) {
            double ry = y + i * 48.0;
            Rect rrow = { cx, ry, cw, 48.0 };
            draw_row_hover(cr, app, rrow);
            draw_text(cr, cx + 16, ry + 29, wname[i], 0.88, 0.90, 0.96, app->wall_preset == i, 10.5);
            Rect rwp = { cx + cw - 92, ry + 11, 76, 26 };
            draw_pill_btn(cr, app, rwp, app->wall_preset == i ? "Applied" : "Apply", app->wall_preset == i);
            add_action(app, rrow, ACT_APPEAR_WALL, i, NULL);
        }

        y += 3 * 48.0 + 18.0;
        draw_text(cr, cx + 2, y, "Window Theme (Fluxbox)", 0.68, 0.72, 0.82, 1, 10.5);
        y += 12.0;
        const char *themes[4] = { "PhantomDark", "BlueNight", "arch", "bora_black" };
        draw_card_box(cr, cx, y, cw, 4, 44.0);
        for (int i = 0; i < 4; i++) {
            double ry = y + i * 44.0;
            Rect rrow = { cx, ry, cw, 44.0 };
            draw_row_hover(cr, app, rrow);
            int act = (strcmp(app->cur_theme, themes[i]) == 0);
            draw_text(cr, cx + 16, ry + 27, themes[i], 0.88, 0.90, 0.96, act, 10.5);
            Rect rth = { cx + cw - 92, ry + 9, 76, 26 };
            draw_pill_btn(cr, app, rth, act ? "Active" : "Use", act);
            add_action(app, rrow, ACT_APPEAR_THEME, i, themes[i]);
        }
        break;
    }
    case PAGE_APPS: {
        struct { const char *name; const char *sub; const char *cmd; } apps[5] = {
            { "Firefox ESR",     "Low-RAM Stateless Browser",   "/usr/bin/firefox" },
            { "PCManFM",         "File Manager & Desktop",      "/usr/bin/pcmanfm" },
            { "Phantom Terminal","XTerm UTF-8 Shell",           "xterm -u8 -ls -bg \"#181825\" -fg \"#cdd6f4\" -cr \"#89b4fa\" -bd \"#313244\"" },
            { "Leafpad",         "Lightweight Text Editor",     "/usr/bin/leafpad" },
            { "System Monitor",  "CPU / RAM / ZRAM / Processes","/usr/bin/phantom-monitor" }
        };
        draw_card_box(cr, cx, y, cw, 5, 52.0);
        for (int i = 0; i < 5; i++) {
            double ry = y + i * 52.0;
            Rect rrow = { cx, ry, cw, 52.0 };
            draw_row_hover(cr, app, rrow);
            draw_text(cr, cx + 16, ry + 22, apps[i].name, 0.90, 0.92, 0.98, 1, 10.5);
            draw_text(cr, cx + 16, ry + 39, apps[i].sub, 0.58, 0.62, 0.74, 0, 9.0);
            Rect rl = { cx + cw - 86, ry + 13, 70, 26 };
            draw_pill_btn(cr, app, rl, "Open", 0);
            add_action(app, rrow, ACT_LAUNCH_CMD, i, apps[i].cmd);
        }
        break;
    }
    case PAGE_SYSTEM: {
        /* Hero badge */
        rounded_rect(cr, cx, y, cw, 64, 14.0);
        cairo_set_source_rgb(cr, 0.114, 0.125, 0.173);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.208, 0.518, 0.894);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        draw_text(cr, cx + 18, y + 29, "PhantomOS", 0.537, 0.706, 0.980, 1, 15.0);
        draw_text(cr, cx + 18, y + 48, app->root_mode, 0.68, 0.72, 0.83, 0, 9.8);
        y += 76.0;

        char mem_s[64], cpu_s[160];
        snprintf(mem_s, sizeof(mem_s), "%lu MB used / %lu MB RAM (+ %lu MB ZRAM)",
                 app->ram_used_mb, app->ram_total_mb, app->zram_total_mb);
        snprintf(cpu_s, sizeof(cpu_s), "%.38s (%dT)", app->cpu_model, app->cpu_cores);

        draw_card_box(cr, cx, y, cw, 4, 46.0);
        const char *sk[4] = { "Device Name", "Kernel", "Processor", "Memory" };
        const char *sv[4] = { app->hostname, app->kernel, cpu_s, mem_s };
        for (int i = 0; i < 4; i++) {
            double ry = y + i * 46.0;
            draw_text(cr, cx + 16, ry + 28, sk[i], 0.85, 0.88, 0.95, 1, 10.5);
            draw_text_right(cr, cx + cw - 16, ry + 28, sv[i], 0.68, 0.72, 0.84, 0, 9.8);
        }
        y += 4 * 46.0 + 16.0;
        Rect rmon = { cx, y, 134, 34 };
        draw_pill_btn(cr, app, rmon, "System Monitor", 1);
        add_action(app, rmon, ACT_POWER_MON, 0, NULL);
        break;
    }
    default: break;
    }

    if (app->wifi_pass_open) {
        double py = h - 56.0;
        rounded_rect(cr, cx, py - 8.0, cw, 46.0, 10.0);
        cairo_set_source_rgb(cr, 0.133, 0.149, 0.212);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.208, 0.518, 0.894);
        cairo_stroke(cr);
        char lbl[96];
        snprintf(lbl, sizeof(lbl), "Password for %s (press Enter):", app->wifi_target_ssid);
        draw_text(cr, cx + 12, py + 7, lbl, 0.85, 0.88, 0.96, 1, 9.0);
        gtk_fixed_move(GTK_FIXED(app->fixed), app->wifi_pass_entry, (int)(cx + 12), (int)(py + 12));
        gtk_widget_set_size_request(app->wifi_pass_entry, (int)(cw - 24), 22);
    }

    if (app->toast[0] && g_get_monotonic_time() < app->toast_until_us) {
        draw_text(cr, SIDEBAR_W + 24, h - 14, app->toast, 0.651, 0.890, 0.631, 1, 9.8);
    }

    if (pe < 0.999) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.15 + 0.85 * pe);
        cairo_restore(cr);
    }

    cairo_destroy(cr);
    return FALSE;
}

static gboolean on_settings_anim_tick(gpointer data) {
    SettingsApp *app = (SettingsApp *)data;
    int active = 0;
    if (app->page_anim_t < 1.0) {
        app->page_anim_t += 0.105;
        if (app->page_anim_t > 1.0) app->page_anim_t = 1.0;
        active = 1;
    }
    if (app->nav_target_y > 0.0 && fabs(app->nav_anim_y - app->nav_target_y) > 0.6) {
        app->nav_anim_y += (app->nav_target_y - app->nav_anim_y) * 0.34;
        active = 1;
    } else if (app->nav_target_y > 0.0 && app->nav_anim_y != app->nav_target_y) {
        app->nav_anim_y = app->nav_target_y;
        active = 1;
    }
    gtk_widget_queue_draw(app->window);
    if (!active) {
        app->anim_id = 0;
        return FALSE;
    }
    return TRUE;
}

static void start_settings_anim(SettingsApp *app) {
    app->page_anim_t = 0.08;
    if (!app->anim_id) {
        app->anim_id = g_timeout_add(16, on_settings_anim_tick, app);
    }
}

static void update_slider_val(SettingsApp *app, int which, double x) {
    Rect trk = (which == 1) ? app->r_bright_trk : (which == 2 ? app->r_vol_trk : app->r_mouse_trk);
    if (trk.w <= 1.0) return;
    double frac = (x - trk.x) / trk.w;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    gint64 now = g_get_monotonic_time();
    if (which == 1) {
        app->bright_pct = 15 + (int)(frac * 85.0 + 0.5);
        apply_display_gamma(app);
    } else if (which == 2) {
        app->vol_pct = (int)(frac * 100.0 + 0.5);
        app->vol_muted = 0;
        if (app->has_audio && (now - app->last_cmd_us) > 65000) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "amixer -q sset Master unmute %d%%", app->vol_pct);
            spawn_cmd(cmd);
            app->last_cmd_us = now;
        }
    } else if (which == 3) {
        int acc = 1 + (int)(frac * 5.0 + 0.5);
        app->mouse_accel = acc;
        Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
        if (dpy) {
            XChangePointerControl(dpy, True, True, acc, 1, 4);
            XFlush(dpy);
        }
    }
    gtk_widget_queue_draw(app->window);
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    SettingsApp *app = (SettingsApp *)data;
    if (ev->button != 1) return FALSE;

    if (pt_in_rect(ev->x, ev->y, app->r_search_btn)) {
        app->search_open = !app->search_open;
        if (app->search_open) {
            gtk_fixed_move(GTK_FIXED(app->fixed), app->search_entry, 20, 50);
            gtk_widget_set_size_request(app->search_entry, (int)(SIDEBAR_W - 40), 22);
            gtk_widget_show(app->search_entry);
            gtk_widget_grab_focus(app->search_entry);
        } else {
            gtk_widget_hide(app->search_entry);
            gtk_entry_set_text(GTK_ENTRY(app->search_entry), "");
            app->search_query[0] = '\0';
        }
        gtk_widget_queue_draw(app->window);
        return TRUE;
    }

    if (pt_in_rect(ev->x, ev->y, app->r_about_btn)) {
        if (app->page != PAGE_SYSTEM) {
            app->page = PAGE_SYSTEM;
            probe_system_info(app);
            start_settings_anim(app);
        }
        gtk_widget_queue_draw(app->window);
        return TRUE;
    }

    for (int i = 0; i < PAGE_COUNT; i++) {
        if (pt_in_rect(ev->x, ev->y, app->r_nav[i])) {
            if (app->page != (SettingsPage)i) {
                app->page = (SettingsPage)i;
                app->wifi_pass_open = 0;
                gtk_widget_hide(app->wifi_pass_entry);
                probe_all(app);
                start_settings_anim(app);
            }
            gtk_widget_queue_draw(app->window);
            return TRUE;
        }
    }

    if (pt_in_rect(ev->x, ev->y, app->r_bright_trk)) {
        app->drag_slider = 1; update_slider_val(app, 1, ev->x); return TRUE;
    }
    if (pt_in_rect(ev->x, ev->y, app->r_vol_trk)) {
        app->drag_slider = 2; update_slider_val(app, 2, ev->x); return TRUE;
    }
    if (pt_in_rect(ev->x, ev->y, app->r_mouse_trk)) {
        app->drag_slider = 3; update_slider_val(app, 3, ev->x); return TRUE;
    }

    for (int i = 0; i < app->action_count; i++) {
        ClickAction a = app->actions[i];
        if (!pt_in_rect(ev->x, ev->y, a.r)) continue;
        switch (a.kind) {
        case ACT_WIFI_TOGGLE: {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "sudo -n /usr/libexec/phantom-net %s %s",
                     app->wifi_up ? "wifi-down" : "wifi-up", app->wifi_iface);
            spawn_cmd(cmd);
            g_timeout_add(700, on_wifi_refresh_cb, app);
            break;
        }
        case ACT_WIFI_AIRPLANE:
            toggle_airplane(app);
            break;
        case ACT_WIFI_SCAN:
            if (app->has_wifi && app->wifi_iface[0]) {
                app->wifi_scanning = 1;
                char cmd[128];
                snprintf(cmd, sizeof(cmd), "wpa_cli -i %s scan >/dev/null 2>&1", app->wifi_iface);
                spawn_cmd(cmd);
                g_timeout_add(1600, on_wifi_refresh_cb, app);
            }
            break;
        case ACT_WIFI_ROW: {
            WifiNet *n = &app->wifi_nets[a.idx];
            if (n->connected) {
                char cmd[128];
                snprintf(cmd, sizeof(cmd), "sudo -n /usr/libexec/phantom-net wifi-disconnect %s", app->wifi_iface);
                spawn_cmd(cmd);
                show_toast(app, "Disconnecting Wi-Fi...");
                g_timeout_add(800, on_wifi_refresh_cb, app);
            } else if (!n->secured) {
                connect_wifi_async(app, n->ssid, "");
            } else {
                g_strlcpy(app->wifi_target_ssid, n->ssid, sizeof(app->wifi_target_ssid));
                app->wifi_pass_open = 1;
                gtk_entry_set_text(GTK_ENTRY(app->wifi_pass_entry), "");
                gtk_widget_show(app->wifi_pass_entry);
                gtk_widget_grab_focus(app->wifi_pass_entry);
            }
            break;
        }
        case ACT_LAN_DHCP:
            spawn_cmd("sudo -n /usr/libexec/phantom-net dhcp-eth0");
            show_toast(app, "Renewing DHCP on eth0...");
            g_timeout_add(1500, on_wifi_refresh_cb, app);
            break;
        case ACT_DISP_MODE:
            apply_resolution(app, a.idx);
            break;
        case ACT_DISP_NIGHT:
            app->night_light = !app->night_light;
            apply_display_gamma(app);
            show_toast(app, app->night_light ? "Night Light enabled (4200K)" : "Night Light disabled");
            break;
        case ACT_SOUND_MUTE:
            if (app->has_audio) {
                spawn_cmd("amixer -q sset Master toggle");
                app->vol_muted = !app->vol_muted;
            }
            break;
        case ACT_POWER_BLANK: {
            app->blank_minutes = (app->blank_minutes == 0) ? 5 : (app->blank_minutes == 5 ? 10 : 0);
            Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
            if (dpy) {
                XSetScreenSaver(dpy, app->blank_minutes * 60, 0, PreferBlanking, DefaultExposures);
                XFlush(dpy);
            }
            show_toast(app, app->blank_minutes ? "Screen blank timer updated" : "Screen blank disabled");
            break;
        }
        case ACT_POWER_MON:
            spawn_cmd("/usr/bin/phantom-monitor");
            break;
        case ACT_WS_COUNT:
            set_workspaces(app, a.idx);
            break;
        case ACT_APPEAR_WALL:
            set_wallpaper_preset(app, a.idx);
            break;
        case ACT_APPEAR_THEME:
            if (a.str) set_fluxbox_theme(app, a.str);
            break;
        case ACT_LAUNCH_CMD:
            if (a.str) spawn_cmd(a.str);
            break;
        case ACT_REBOOT:
            spawn_cmd("sudo -n /sbin/reboot");
            break;
        case ACT_POWEROFF:
            spawn_cmd("sudo -n /sbin/poweroff");
            break;
        default: break;
        }
        gtk_widget_queue_draw(app->window);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_button_release(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w; (void)ev;
    SettingsApp *app = (SettingsApp *)data;
    if (app->drag_slider == 2 && app->has_audio) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "amixer -q sset Master unmute %d%%", app->vol_pct);
        spawn_cmd(cmd);
    }
    app->drag_slider = 0;
    return FALSE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    (void)w;
    SettingsApp *app = (SettingsApp *)data;
    app->hx = ev->x;
    app->hy = ev->y;
    app->hovering = 1;
    if (app->drag_slider) {
        update_slider_val(app, app->drag_slider, ev->x);
    } else {
        gtk_widget_queue_draw(app->window);
    }
    return FALSE;
}

static void on_search_changed(GtkEditable *ed, gpointer data) {
    SettingsApp *app = (SettingsApp *)data;
    const char *txt = gtk_entry_get_text(GTK_ENTRY(ed));
    g_strlcpy(app->search_query, txt ? txt : "", sizeof(app->search_query));
    gtk_widget_queue_draw(app->window);
}

static void on_wifi_pass_activate(GtkEntry *e, gpointer data) {
    SettingsApp *app = (SettingsApp *)data;
    const char *pass = gtk_entry_get_text(e);
    connect_wifi_async(app, app->wifi_target_ssid, pass ? pass : "");
    app->wifi_pass_open = 0;
    gtk_widget_hide(app->wifi_pass_entry);
    gtk_entry_set_text(e, "");
    gtk_widget_queue_draw(app->window);
}

static void style_entry(GtkWidget *entry) {
    GdkColor base, text;
    gdk_color_parse("#11131b", &base);
    gdk_color_parse("#e4e7f5", &text);
    gtk_entry_set_has_frame(GTK_ENTRY(entry), FALSE);
    gtk_widget_modify_base(entry, GTK_STATE_NORMAL, &base);
    gtk_widget_modify_text(entry, GTK_STATE_NORMAL, &text);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 10");
    gtk_widget_modify_font(entry, fd);
    pango_font_description_free(fd);
}

static gboolean on_tick(gpointer data) {
    SettingsApp *app = (SettingsApp *)data;
    probe_all(app);
    gtk_widget_queue_draw(app->window);
    return TRUE;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    SettingsApp app;
    memset(&app, 0, sizeof(app));
    app.win_w = 840;
    app.win_h = 560;
    app.page = PAGE_NETWORK;
    app.bright_pct = 100;
    app.mouse_accel = 2;
    probe_all(&app);
    if (app.has_wifi) app.page = PAGE_WIFI;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "PhantomOS Settings");
    gtk_window_set_default_size(GTK_WINDOW(app.window), app.win_w, app.win_h);
    gtk_widget_set_size_request(app.window, 720, 480);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    gtk_widget_set_app_paintable(app.window, TRUE);
    gtk_widget_add_events(app.window,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);

    app.fixed = gtk_fixed_new();
    gtk_container_add(GTK_CONTAINER(app.window), app.fixed);

    app.search_entry = gtk_entry_new();
    style_entry(app.search_entry);
    gtk_fixed_put(GTK_FIXED(app.fixed), app.search_entry, 20, 50);

    app.wifi_pass_entry = gtk_entry_new();
    style_entry(app.wifi_pass_entry);
    gtk_entry_set_visibility(GTK_ENTRY(app.wifi_pass_entry), FALSE);
    gtk_fixed_put(GTK_FIXED(app.fixed), app.wifi_pass_entry, 300, 480);

    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(app.window, "expose-event", G_CALLBACK(on_expose), &app);
    g_signal_connect(app.window, "button-press-event", G_CALLBACK(on_button_press), &app);
    g_signal_connect(app.window, "button-release-event", G_CALLBACK(on_button_release), &app);
    g_signal_connect(app.window, "motion-notify-event", G_CALLBACK(on_motion), &app);
    g_signal_connect(app.search_entry, "changed", G_CALLBACK(on_search_changed), &app);
    g_signal_connect(app.wifi_pass_entry, "activate", G_CALLBACK(on_wifi_pass_activate), &app);

    g_timeout_add(2000, on_tick, &app);

    gtk_widget_show(app.fixed);
    gtk_widget_show(app.window);
    start_settings_anim(&app);
    gtk_main();
    return 0;
}
