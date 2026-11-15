/*
 * PhantomOS System Monitor (phantom-monitor)
 * Lightweight native GTK2 + Cairo resource & process monitor for PhantomOS.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cairo.h>
#include <gtk/gtk.h>

#define HISTORY_LEN 60
#define MAX_CORES 64
#define MAX_PROCS 1024

typedef struct {
    unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
} CpuSample;

typedef struct {
    int pid;
    unsigned long long proc_ticks;
} ProcPrev;

typedef struct {
    /* CPU */
    int num_cores;
    CpuSample prev_total;
    CpuSample prev_cores[MAX_CORES];
    double cpu_history[HISTORY_LEN];
    double core_pct[MAX_CORES];
    double current_cpu;
    char load_avg[64];

    /* Memory & Storage */
    double mem_history[HISTORY_LEN];
    double mem_used_mb;
    double mem_total_mb;
    double mem_cache_mb;
    double mem_pct;
    double ramdisk_used_mb;
    double ramdisk_total_mb;
    double overlay_used_mb;
    double overlay_total_mb;

    /* Network */
    unsigned long long prev_rx_bytes;
    unsigned long long prev_tx_bytes;
    int net_initialized;
    double rx_history[HISTORY_LEN];
    double tx_history[HISTORY_LEN];
    double current_rx_kbps;
    double current_tx_kbps;
    char ip_addr[64];

    /* System info */
    char hostname[128];
    char kernel_ver[128];
    char root_mode[48];
    unsigned long uptime_sec;

    /* Process delta tracking */
    unsigned long long prev_total_jiffies;
    ProcPrev prev_procs[MAX_PROCS];
    int prev_proc_count;
    gboolean paused;

    /* Widgets */
    GtkWidget *window;
    GtkWidget *hdr_label;
    GtkWidget *cpu_label;
    GtkWidget *mem_label;
    GtkWidget *net_label;
    GtkWidget *disk_label;
    GtkWidget *cpu_area;
    GtkWidget *mem_area;
    GtkWidget *net_area;
    GtkWidget *pause_btn;
    GtkListStore *proc_store;
    GtkTreeView *proc_view;
} AppState;

enum {
    COL_PID = 0,
    COL_NAME,
    COL_STATE,
    COL_CPU,
    COL_MEM,
    COL_CMD,
    NUM_COLS
};

static void push_history(double *arr, double val) {
    memmove(&arr[0], &arr[1], sizeof(double) * (HISTORY_LEN - 1));
    arr[HISTORY_LEN - 1] = val;
}

static unsigned long long cpu_total_ticks(const CpuSample *s) {
    return s->user + s->nice + s->sys + s->idle + s->iowait + s->irq + s->softirq + s->steal;
}

static unsigned long long cpu_idle_ticks(const CpuSample *s) {
    return s->idle + s->iowait;
}

static void read_static_sysinfo(AppState *st) {
    struct utsname u;
    if (gethostname(st->hostname, sizeof(st->hostname) - 1) != 0) {
        snprintf(st->hostname, sizeof(st->hostname), "phantomos");
    }
    if (uname(&u) == 0) {
        snprintf(st->kernel_ver, sizeof(st->kernel_ver), "%s", u.release);
    } else {
        snprintf(st->kernel_ver, sizeof(st->kernel_ver), "Linux");
    }

    snprintf(st->root_mode, sizeof(st->root_mode), "Read-Write (ext4)");
    FILE *fp = fopen("/proc/mounts", "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, " / overlay ")) {
                snprintf(st->root_mode, sizeof(st->root_mode), "Stateless (SquashFS + OverlayFS)");
                break;
            }
        }
        fclose(fp);
    }
}

static void read_cpu_stats(AppState *st) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return;

    char line[512];
    int core_idx = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu ", 4) == 0) {
            CpuSample cur = {0};
            sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &cur.user, &cur.nice, &cur.sys, &cur.idle,
                   &cur.iowait, &cur.irq, &cur.softirq, &cur.steal);
            unsigned long long tot_diff = cpu_total_ticks(&cur) - cpu_total_ticks(&st->prev_total);
            unsigned long long idle_diff = cpu_idle_ticks(&cur) - cpu_idle_ticks(&st->prev_total);
            if (tot_diff > 0 && cpu_total_ticks(&st->prev_total) > 0) {
                st->current_cpu = 100.0 * (double)(tot_diff - idle_diff) / (double)tot_diff;
                if (st->current_cpu < 0.0) st->current_cpu = 0.0;
                if (st->current_cpu > 100.0) st->current_cpu = 100.0;
            }
            st->prev_total = cur;
            push_history(st->cpu_history, st->current_cpu);
        } else if (strncmp(line, "cpu", 3) == 0 && isdigit((unsigned char)line[3]) && core_idx < MAX_CORES) {
            CpuSample cur = {0};
            int id = 0;
            sscanf(line + 3, "%d %llu %llu %llu %llu %llu %llu %llu %llu",
                   &id, &cur.user, &cur.nice, &cur.sys, &cur.idle,
                   &cur.iowait, &cur.irq, &cur.softirq, &cur.steal);
            unsigned long long tot_diff = cpu_total_ticks(&cur) - cpu_total_ticks(&st->prev_cores[core_idx]);
            unsigned long long idle_diff = cpu_idle_ticks(&cur) - cpu_idle_ticks(&st->prev_cores[core_idx]);
            if (tot_diff > 0 && cpu_total_ticks(&st->prev_cores[core_idx]) > 0) {
                double pct = 100.0 * (double)(tot_diff - idle_diff) / (double)tot_diff;
                st->core_pct[core_idx] = pct < 0.0 ? 0.0 : (pct > 100.0 ? 100.0 : pct);
            }
            st->prev_cores[core_idx] = cur;
            core_idx++;
        }
    }
    if (core_idx > 0) st->num_cores = core_idx;
    fclose(fp);

    fp = fopen("/proc/loadavg", "r");
    if (fp) {
        double l1 = 0, l5 = 0, l15 = 0;
        if (fscanf(fp, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
            snprintf(st->load_avg, sizeof(st->load_avg), "%.2f %.2f %.2f", l1, l5, l15);
        }
        fclose(fp);
    }

    fp = fopen("/proc/uptime", "r");
    if (fp) {
        double up = 0;
        if (fscanf(fp, "%lf", &up) == 1) {
            st->uptime_sec = (unsigned long)up;
        }
        fclose(fp);
    }
}

static void read_mem_and_disks(AppState *st) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char key[64];
        unsigned long long val = 0;
        unsigned long long total = 0, mfree = 0, buffers = 0, cached = 0, srecl = 0;
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "%63[^:]: %llu", key, &val) == 2) {
                if (strcmp(key, "MemTotal") == 0) total = val;
                else if (strcmp(key, "MemFree") == 0) mfree = val;
                else if (strcmp(key, "Buffers") == 0) buffers = val;
                else if (strcmp(key, "Cached") == 0) cached = val;
                else if (strcmp(key, "SReclaimable") == 0) srecl = val;
            }
        }
        fclose(fp);

        long long used = (long long)total - (long long)mfree - (long long)buffers - (long long)cached - (long long)srecl;
        if (used < 0) used = 0;
        st->mem_total_mb = (double)total / 1024.0;
        st->mem_used_mb = (double)used / 1024.0;
        st->mem_cache_mb = (double)(buffers + cached + srecl) / 1024.0;
        st->mem_pct = st->mem_total_mb > 0 ? (100.0 * st->mem_used_mb / st->mem_total_mb) : 0.0;
        push_history(st->mem_history, st->mem_pct);
    }

    struct statvfs vfs;
    if (statvfs("/var/lib/data", &vfs) == 0 && vfs.f_blocks > 0) {
        double tot = (double)vfs.f_blocks * vfs.f_frsize / (1024.0 * 1024.0);
        double avail = (double)vfs.f_bavail * vfs.f_frsize / (1024.0 * 1024.0);
        st->ramdisk_total_mb = tot;
        st->ramdisk_used_mb = tot - avail;
    }
    if (statvfs("/", &vfs) == 0 && vfs.f_blocks > 0) {
        double tot = (double)vfs.f_blocks * vfs.f_frsize / (1024.0 * 1024.0);
        double avail = (double)vfs.f_bavail * vfs.f_frsize / (1024.0 * 1024.0);
        st->overlay_total_mb = tot;
        st->overlay_used_mb = tot - avail;
    }
}

static void read_net_stats(AppState *st) {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (fp) {
        char line[512];
        unsigned long long rx_sum = 0, tx_sum = 0;
        while (fgets(line, sizeof(line), fp)) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = '\0';
            char *iface = line;
            while (*iface == ' ') iface++;
            if (strcmp(iface, "lo") == 0) continue;
            unsigned long long rx = 0, tx = 0, dummy = 0;
            if (sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &rx, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &tx) >= 9) {
                rx_sum += rx;
                tx_sum += tx;
            }
        }
        fclose(fp);

        if (st->net_initialized) {
            st->current_rx_kbps = (double)(rx_sum - st->prev_rx_bytes) / 1024.0;
            st->current_tx_kbps = (double)(tx_sum - st->prev_tx_bytes) / 1024.0;
        } else {
            st->net_initialized = 1;
        }
        st->prev_rx_bytes = rx_sum;
        st->prev_tx_bytes = tx_sum;
        push_history(st->rx_history, st->current_rx_kbps);
        push_history(st->tx_history, st->current_tx_kbps);
    }

    FILE *ipf = popen("ip -4 addr show eth0 2>/dev/null | awk '/inet /{print $2; exit}'", "r");
    if (ipf) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), ipf)) {
            buf[strcspn(buf, "\r\n")] = '\0';
            snprintf(st->ip_addr, sizeof(st->ip_addr), "%s", buf[0] ? buf : "unassigned");
        } else {
            snprintf(st->ip_addr, sizeof(st->ip_addr), "unassigned");
        }
        pclose(ipf);
    }
}

static unsigned long long lookup_prev_proc_ticks(const AppState *st, int pid) {
    for (int i = 0; i < st->prev_proc_count; i++) {
        if (st->prev_procs[i].pid == pid) {
            return st->prev_procs[i].proc_ticks;
        }
    }
    return 0;
}

static void refresh_processes(AppState *st) {
    DIR *dir = opendir("/proc");
    if (!dir) return;

    unsigned long long total_jiffies = cpu_total_ticks(&st->prev_total);
    unsigned long long jiffy_diff = total_jiffies > st->prev_total_jiffies
                                        ? (total_jiffies - st->prev_total_jiffies)
                                        : 0;
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (page_kb <= 0) page_kb = 4;

    /* Remember selected PID across updates */
    int selected_pid = -1;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(st->proc_view);
    GtkTreeModel *model = NULL;
    GtkTreeIter sel_iter;
    if (gtk_tree_selection_get_selected(sel, &model, &sel_iter)) {
        gtk_tree_model_get(model, &sel_iter, COL_PID, &selected_pid, -1);
    }

    gtk_list_store_clear(st->proc_store);

    ProcPrev new_prev[MAX_PROCS];
    int new_count = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        int pid = atoi(ent->d_name);
        if (pid <= 0) continue;

        char path[128], buf[1024];
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        if (n == 0) continue;
        buf[n] = '\0';

        char *lp = strchr(buf, '(');
        char *rp = strrchr(buf, ')');
        if (!lp || !rp || rp <= lp) continue;

        char name[128] = {0};
        size_t namelen = (size_t)(rp - lp - 1);
        if (namelen >= sizeof(name)) namelen = sizeof(name) - 1;
        memcpy(name, lp + 1, namelen);

        char state_ch = '?';
        unsigned long long utime = 0, stime = 0;
        long rss_pages = 0;
        /* Fields after ") ": state(3), ppid(4)..minflt..utime(14) stime(15) .. vsize(23) rss(24) */
        char *p = rp + 2;
        int field = 3;
        char *saveptr = NULL;
        for (char *tok = strtok_r(p, " ", &saveptr); tok; tok = strtok_r(NULL, " ", &saveptr)) {
            if (field == 3) state_ch = tok[0];
            else if (field == 14) utime = strtoull(tok, NULL, 10);
            else if (field == 15) stime = strtoull(tok, NULL, 10);
            else if (field == 24) {
                rss_pages = strtol(tok, NULL, 10);
                break;
            }
            field++;
        }

        unsigned long long proc_ticks = utime + stime;
        if (new_count < MAX_PROCS) {
            new_prev[new_count].pid = pid;
            new_prev[new_count].proc_ticks = proc_ticks;
            new_count++;
        }

        double cpu_pct = 0.0;
        if (jiffy_diff > 0) {
            unsigned long long prev_t = lookup_prev_proc_ticks(st, pid);
            if (prev_t > 0 && proc_ticks >= prev_t) {
                cpu_pct = 100.0 * (double)(proc_ticks - prev_t) / (double)jiffy_diff * (double)(st->num_cores > 0 ? st->num_cores : 1);
            }
        }
        double rss_mb = (double)(rss_pages * page_kb) / 1024.0;

        char cmdline[256] = {0};
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        fp = fopen(path, "r");
        if (fp) {
            size_t cn = fread(cmdline, 1, sizeof(cmdline) - 1, fp);
            fclose(fp);
            for (size_t i = 0; i < cn; i++) {
                if (cmdline[i] == '\0') cmdline[i] = ' ';
            }
            while (cn > 0 && cmdline[cn - 1] == ' ') cmdline[--cn] = '\0';
        }
        if (cmdline[0] == '\0') {
            snprintf(cmdline, sizeof(cmdline), "[%s]", name);
        }

        char state_str[4] = {state_ch, '\0', '\0', '\0'};
        GtkTreeIter iter;
        gtk_list_store_append(st->proc_store, &iter);
        gtk_list_store_set(st->proc_store, &iter,
                           COL_PID, pid,
                           COL_NAME, name,
                           COL_STATE, state_str,
                           COL_CPU, cpu_pct,
                           COL_MEM, rss_mb,
                           COL_CMD, cmdline,
                           -1);
        if (pid == selected_pid) {
            gtk_tree_selection_select_iter(sel, &iter);
        }
    }
    closedir(dir);

    memcpy(st->prev_procs, new_prev, sizeof(ProcPrev) * (size_t)new_count);
    st->prev_proc_count = new_count;
    st->prev_total_jiffies = total_jiffies;
}

static void draw_graph(GtkWidget *widget, const double *series1, const double *series2,
                       double max_val, const char *unit_label,
                       double r1, double g1, double b1,
                       double r2, double g2, double b2) {
    if (!widget->window) return;
    cairo_t *cr = gdk_cairo_create(widget->window);
    int w = widget->allocation.width;
    int h = widget->allocation.height;

    /* Catppuccin Mocha / PhantomDark background */
    cairo_set_source_rgb(cr, 0.094, 0.094, 0.145); /* #181825 */
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);

    /* Subtle grid */
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb(cr, 0.192, 0.196, 0.267); /* #313244 */
    for (int i = 1; i < 4; i++) {
        double y = (double)h * i / 4.0;
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, w, y);
    }
    for (int i = 1; i < 6; i++) {
        double x = (double)w * i / 6.0;
        cairo_move_to(cr, x, 0);
        cairo_line_to(cr, x, h);
    }
    cairo_stroke(cr);

    /* Border */
    cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
    cairo_stroke(cr);

    if (max_val <= 0.0) max_val = 100.0;

    /* Series 1 area + line */
    double step_x = (double)w / (double)(HISTORY_LEN - 1);
    cairo_move_to(cr, 0, h);
    for (int i = 0; i < HISTORY_LEN; i++) {
        double val = series1[i] > max_val ? max_val : series1[i];
        double x = i * step_x;
        double y = h - (val / max_val) * (h - 8) - 4;
        cairo_line_to(cr, x, y);
    }
    cairo_line_to(cr, w, h);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, r1, g1, b1, 0.22);
    cairo_fill(cr);

    cairo_set_line_width(cr, 2.0);
    cairo_set_source_rgb(cr, r1, g1, b1);
    for (int i = 0; i < HISTORY_LEN; i++) {
        double val = series1[i] > max_val ? max_val : series1[i];
        double x = i * step_x;
        double y = h - (val / max_val) * (h - 8) - 4;
        if (i == 0) cairo_move_to(cr, x, y);
        else cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);

    /* Optional Series 2 line */
    if (series2) {
        cairo_set_line_width(cr, 1.8);
        cairo_set_source_rgb(cr, r2, g2, b2);
        for (int i = 0; i < HISTORY_LEN; i++) {
            double val = series2[i] > max_val ? max_val : series2[i];
            double x = i * step_x;
            double y = h - (val / max_val) * (h - 8) - 4;
            if (i == 0) cairo_move_to(cr, x, y);
            else cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
    }

    /* Scale label */
    cairo_set_source_rgb(cr, 0.65, 0.68, 0.78);
    cairo_select_font_face(cr, "Adwaita Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);
    cairo_move_to(cr, 6, 13);
    cairo_show_text(cr, unit_label);

    cairo_destroy(cr);
}

static gboolean on_cpu_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data) {
    (void)ev;
    AppState *st = (AppState *)data;
    draw_graph(w, st->cpu_history, NULL, 100.0, "100%",
               0.537, 0.706, 0.980, 0, 0, 0); /* #89b4fa */
    return FALSE;
}

static gboolean on_mem_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data) {
    (void)ev;
    AppState *st = (AppState *)data;
    draw_graph(w, st->mem_history, NULL, 100.0, "100%",
               0.651, 0.890, 0.631, 0, 0, 0); /* #a6e3a1 */
    return FALSE;
}

static gboolean on_net_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data) {
    (void)ev;
    AppState *st = (AppState *)data;
    double max_kb = 64.0;
    for (int i = 0; i < HISTORY_LEN; i++) {
        if (st->rx_history[i] > max_kb) max_kb = st->rx_history[i];
        if (st->tx_history[i] > max_kb) max_kb = st->tx_history[i];
    }
    max_kb *= 1.2;
    char scale[32];
    if (max_kb >= 1024.0) snprintf(scale, sizeof(scale), "%.1f MB/s", max_kb / 1024.0);
    else snprintf(scale, sizeof(scale), "%.0f KB/s", max_kb);
    draw_graph(w, st->rx_history, st->tx_history, max_kb, scale,
               0.976, 0.886, 0.686,   /* #f9e2af RX */
               0.796, 0.651, 0.969);  /* #cba6f7 TX */
    return FALSE;
}

static void update_labels(AppState *st) {
    char buf[512];
    unsigned long h = st->uptime_sec / 3600;
    unsigned long m = (st->uptime_sec % 3600) / 60;
    unsigned long s = st->uptime_sec % 60;

    snprintf(buf, sizeof(buf),
             "<b>PhantomOS Monitor</b>  |  Host: %s  |  Kernel: %s  |  Mode: %s  |  Uptime: %02lu:%02lu:%02lu",
             st->hostname, st->kernel_ver, st->root_mode, h, m, s);
    gtk_label_set_markup(GTK_LABEL(st->hdr_label), buf);

    snprintf(buf, sizeof(buf),
             "<b>CPU Usage:</b> <span foreground='#89b4fa'><b>%.1f%%</b></span>   |   Cores: %d   |   Load Avg: %s",
             st->current_cpu, st->num_cores, st->load_avg);
    gtk_label_set_markup(GTK_LABEL(st->cpu_label), buf);

    snprintf(buf, sizeof(buf),
             "<b>Memory:</b> <span foreground='#a6e3a1'><b>%.0f / %.0f MB (%.1f%%)</b></span>   |   Buffers/Cache: %.0f MB",
             st->mem_used_mb, st->mem_total_mb, st->mem_pct, st->mem_cache_mb);
    gtk_label_set_markup(GTK_LABEL(st->mem_label), buf);

    snprintf(buf, sizeof(buf),
             "<b>RAM Disk (/var/lib/data):</b> %.1f / %.0f MB   |   <b>Root (/):</b> %.1f / %.0f MB",
             st->ramdisk_used_mb, st->ramdisk_total_mb, st->overlay_used_mb, st->overlay_total_mb);
    gtk_label_set_markup(GTK_LABEL(st->disk_label), buf);

    snprintf(buf, sizeof(buf),
             "<b>Network (eth0: %s):</b>   <span foreground='#f9e2af'>RX %.1f KB/s</span>   |   <span foreground='#cba6f7'>TX %.1f KB/s</span>",
             st->ip_addr, st->current_rx_kbps, st->current_tx_kbps);
    gtk_label_set_markup(GTK_LABEL(st->net_label), buf);
}

static gboolean on_tick(gpointer data) {
    AppState *st = (AppState *)data;
    if (st->paused) return TRUE;

    read_cpu_stats(st);
    read_mem_and_disks(st);
    read_net_stats(st);
    refresh_processes(st);
    update_labels(st);

    gtk_widget_queue_draw(st->cpu_area);
    gtk_widget_queue_draw(st->mem_area);
    gtk_widget_queue_draw(st->net_area);
    return TRUE;
}

static void send_signal_to_selected(AppState *st, int sig) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(st->proc_view);
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return;

    int pid = 0;
    gchar *name = NULL;
    gtk_tree_model_get(model, &iter, COL_PID, &pid, COL_NAME, &name, -1);
    if (pid <= 1) {
        g_free(name);
        return;
    }

    if (kill(pid, sig) != 0) {
        GtkWidget *dlg = gtk_message_dialog_new(
            GTK_WINDOW(st->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Failed to signal PID %d (%s): %s", pid, name ? name : "", strerror(errno));
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
    } else {
        refresh_processes(st);
    }
    g_free(name);
}

static void on_term_clicked(GtkWidget *w, gpointer data) {
    (void)w;
    send_signal_to_selected((AppState *)data, SIGTERM);
}

static void on_kill_clicked(GtkWidget *w, gpointer data) {
    (void)w;
    send_signal_to_selected((AppState *)data, SIGKILL);
}

static void on_pause_clicked(GtkWidget *w, gpointer data) {
    (void)w;
    AppState *st = (AppState *)data;
    st->paused = !st->paused;
    gtk_button_set_label(GTK_BUTTON(st->pause_btn), st->paused ? "Resume" : "Pause");
}

static void on_htop_clicked(GtkWidget *w, gpointer data) {
    (void)w;
    (void)data;
    g_spawn_command_line_async(
        "xterm -u8 -bg '#181825' -fg '#cdd6f4' -title 'System Monitor (htop)' -e htop",
        NULL);
}

static GtkWidget *make_card(const char *label_text, GtkWidget **out_label, GtkWidget **out_area,
                            GCallback expose_cb, AppState *st) {
    GtkWidget *box = gtk_vbox_new(FALSE, 4);
    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_misc_set_alignment(GTK_MISC(lbl), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);

    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, 640, 105);
    g_signal_connect(area, "expose-event", expose_cb, st);
    gtk_box_pack_start(GTK_BOX(box), area, TRUE, TRUE, 0);

    *out_label = lbl;
    *out_area = area;
    return box;
}

static void add_text_column(GtkTreeView *tv, const char *title, int col_id, int width, gboolean expand) {
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(title, r, "text", col_id, NULL);
    gtk_tree_view_column_set_sort_column_id(col, col_id);
    gtk_tree_view_column_set_resizable(col, TRUE);
    if (width > 0) gtk_tree_view_column_set_min_width(col, width);
    if (expand) gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(tv, col);
}

static void format_float_cell(GtkTreeViewColumn *col, GtkCellRenderer *r,
                              GtkTreeModel *m, GtkTreeIter *iter, gpointer data) {
    (void)col;
    int col_id = GPOINTER_TO_INT(data);
    double val = 0.0;
    char text[32];
    gtk_tree_model_get(m, iter, col_id, &val, -1);
    snprintf(text, sizeof(text), "%.1f", val);
    g_object_set(r, "text", text, NULL);
}

static void add_float_column(GtkTreeView *tv, const char *title, int col_id, int width) {
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", 1.0, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, title);
    gtk_tree_view_column_pack_start(col, r, TRUE);
    gtk_tree_view_column_set_cell_data_func(col, r, format_float_cell, GINT_TO_POINTER(col_id), NULL);
    gtk_tree_view_column_set_sort_column_id(col, col_id);
    gtk_tree_view_column_set_min_width(col, width);
    gtk_tree_view_append_column(tv, col);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    AppState st;
    memset(&st, 0, sizeof(st));
    st.num_cores = 1;
    snprintf(st.load_avg, sizeof(st.load_avg), "0.00 0.00 0.00");
    snprintf(st.ip_addr, sizeof(st.ip_addr), "checking...");
    read_static_sysinfo(&st);

    st.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(st.window), "PhantomOS System Monitor");
    gtk_window_set_default_size(GTK_WINDOW(st.window), 820, 560);
    gtk_window_set_position(GTK_WINDOW(st.window), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(st.window), 8);
    g_signal_connect(st.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *root_vbox = gtk_vbox_new(FALSE, 8);
    gtk_container_add(GTK_CONTAINER(st.window), root_vbox);

    st.hdr_label = gtk_label_new("PhantomOS Monitor");
    gtk_misc_set_alignment(GTK_MISC(st.hdr_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(root_vbox), st.hdr_label, FALSE, FALSE, 2);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(root_vbox), notebook, TRUE, TRUE, 0);

    /* Tab 1: Resources */
    GtkWidget *res_box = gtk_vbox_new(FALSE, 10);
    gtk_container_set_border_width(GTK_CONTAINER(res_box), 10);

    gtk_box_pack_start(GTK_BOX(res_box),
                       make_card("CPU", &st.cpu_label, &st.cpu_area, G_CALLBACK(on_cpu_expose), &st),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(res_box),
                       make_card("Memory", &st.mem_label, &st.mem_area, G_CALLBACK(on_mem_expose), &st),
                       TRUE, TRUE, 0);

    st.disk_label = gtk_label_new("RAM Disk & Root");
    gtk_misc_set_alignment(GTK_MISC(st.disk_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(res_box), st.disk_label, FALSE, FALSE, 2);

    gtk_box_pack_start(GTK_BOX(res_box),
                       make_card("Network", &st.net_label, &st.net_area, G_CALLBACK(on_net_expose), &st),
                       TRUE, TRUE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), res_box, gtk_label_new("  Resources  "));

    /* Tab 2: Processes */
    GtkWidget *proc_box = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(proc_box), 8);

    st.proc_store = gtk_list_store_new(NUM_COLS,
                                       G_TYPE_INT,
                                       G_TYPE_STRING,
                                       G_TYPE_STRING,
                                       G_TYPE_DOUBLE,
                                       G_TYPE_DOUBLE,
                                       G_TYPE_STRING);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(st.proc_store), COL_MEM, GTK_SORT_DESCENDING);

    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(st.proc_store));
    st.proc_view = GTK_TREE_VIEW(tv);
    gtk_tree_view_set_rules_hint(st.proc_view, TRUE);

    add_text_column(st.proc_view, "PID", COL_PID, 60, FALSE);
    add_text_column(st.proc_view, "Name", COL_NAME, 140, FALSE);
    add_text_column(st.proc_view, "S", COL_STATE, 35, FALSE);
    add_float_column(st.proc_view, "CPU %", COL_CPU, 70);
    add_float_column(st.proc_view, "RAM (MB)", COL_MEM, 85);
    add_text_column(st.proc_view, "Command", COL_CMD, 220, TRUE);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tv);
    gtk_box_pack_start(GTK_BOX(proc_box), scroll, TRUE, TRUE, 0);

    GtkWidget *btn_hbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *term_btn = gtk_button_new_with_label("End Process (SIGTERM)");
    GtkWidget *kill_btn = gtk_button_new_with_label("Force Kill (SIGKILL)");
    st.pause_btn = gtk_button_new_with_label("Pause");
    GtkWidget *htop_btn = gtk_button_new_with_label("Open htop");

    g_signal_connect(term_btn, "clicked", G_CALLBACK(on_term_clicked), &st);
    g_signal_connect(kill_btn, "clicked", G_CALLBACK(on_kill_clicked), &st);
    g_signal_connect(st.pause_btn, "clicked", G_CALLBACK(on_pause_clicked), &st);
    g_signal_connect(htop_btn, "clicked", G_CALLBACK(on_htop_clicked), &st);

    gtk_box_pack_start(GTK_BOX(btn_hbox), term_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_hbox), kill_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btn_hbox), htop_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btn_hbox), st.pause_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(proc_box), btn_hbox, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), proc_box, gtk_label_new("  Processes  "));

    /* Initial sample + 1s timer */
    on_tick(&st);
    g_timeout_add(1000, on_tick, &st);

    gtk_widget_show_all(st.window);
    gtk_main();
    return 0;
}
