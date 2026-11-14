/*
 * Conservation Mode for Linux (Lenovo IdeaPad)
 *
 * Native GTK 3 app. At runtime it needs only libraries every Linux desktop
 * already ships (glibc + GTK 3) and the in-kernel ideapad_laptop driver.
 *
 * Firmware commands are the same as Lenovo Vantage / the Windows build:
 *   ON = 0x03, OFF = 0x05, status = bit 5. The ideapad_laptop driver sends
 *   them when /sys/bus/platform/drivers/ideapad_acpi/<dev>/conservation_mode
 *   is written.
 *
 *   ConservationMode                 open the app
 *   ConservationMode status|on|off   command line
 */
#include <gtk/gtk.h>
#include <glob.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define APP_ID      "io.github.conservationmode"
#define APP_VERSION "1.1.0"
#define SYSFS_GLOB  "/sys/bus/platform/drivers/ideapad_acpi/*/conservation_mode"
#define SERVICE     "/etc/systemd/system/battery-conservation.service"

/* ------------------------------------------------------------- backend -- */

static gchar *read_file(const char *path)
{
    gchar *s = NULL;
    if (!g_file_get_contents(path, &s, NULL, NULL))
        return NULL;
    return g_strstrip(s);
}

/* 1 = on, 0 = off, -1 = not available */
static int read_state(void)
{
    glob_t g;
    int r = -1;
    if (glob(SYSFS_GLOB, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
        gchar *v = read_file(g.gl_pathv[0]);
        if (v)
            r = strcmp(v, "1") == 0;
        g_free(v);
    }
    globfree(&g);
    return r;
}

static const char *SERVICE_TEXT =
    "[Unit]\n"
    "Description=Enable Lenovo IdeaPad battery conservation mode\n"
    "After=multi-user.target\n\n"
    "[Service]\n"
    "Type=oneshot\n"
    "ExecStart=/bin/sh -c 'for f in " SYSFS_GLOB "; do echo 1 > \"$f\"; done'\n\n"
    "[Install]\n"
    "WantedBy=multi-user.target\n";

/* Runs as root. Writes the setting and keeps it after reboot. */
static int apply_as_root(int on)
{
    glob_t g;
    int written = 0;
    if (glob(SYSFS_GLOB, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            FILE *f = fopen(g.gl_pathv[i], "w");
            if (!f)
                continue;
            if (fputs(on ? "1" : "0", f) >= 0 && fclose(f) == 0)
                written++;
        }
    }
    globfree(&g);
    if (!written) {
        fprintf(stderr, "Could not write conservation_mode. "
                        "Is the ideapad_laptop module loaded? (sudo modprobe ideapad_laptop)\n");
        return 1;
    }

    if (access("/run/systemd/system", F_OK) != 0)
        return 0; /* no systemd: applied now, just not re-applied at boot */

    if (on) {
        if (access(SERVICE, F_OK) != 0) {
            FILE *f = fopen(SERVICE, "w");
            if (f) {
                fputs(SERVICE_TEXT, f);
                fclose(f);
                if (system("systemctl daemon-reload >/dev/null 2>&1") != 0)
                    fprintf(stderr, "Warning: systemctl daemon-reload failed\n");
            }
        }
        if (system("systemctl enable battery-conservation.service >/dev/null 2>&1") != 0)
            fprintf(stderr, "Warning: could not enable the boot service\n");
    } else if (access(SERVICE, F_OK) == 0) {
        if (system("systemctl disable battery-conservation.service >/dev/null 2>&1") != 0)
            fprintf(stderr, "Warning: could not disable the boot service\n");
    }
    return 0;
}

/* argv that changes the setting: runs this binary as root through pkexec. */
static gchar **apply_argv(int on)
{
    gchar **argv = g_new0(gchar *, 5);
    int i = 0;
    if (geteuid() != 0)
        argv[i++] = g_strdup("pkexec");
    argv[i++] = g_file_read_link("/proc/self/exe", NULL);
    argv[i++] = g_strdup("--apply");
    argv[i++] = g_strdup(on ? "on" : "off");
    return argv;
}

static gboolean pkexec_cancelled(int status)
{
    return WIFEXITED(status) && (WEXITSTATUS(status) == 126 || WEXITSTATUS(status) == 127);
}

/* ------------------------------------------------------------- battery -- */

typedef struct {
    int present, level, health, cycles, plugged;
    char status[32];
} Battery;

static void read_battery(Battery *b)
{
    memset(b, 0, sizeof *b);
    b->level = b->health = b->cycles = -1;
    glob_t g;

    if (glob("/sys/class/power_supply/*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            gchar *tp = g_build_filename(g.gl_pathv[i], "type", NULL);
            gchar *op = g_build_filename(g.gl_pathv[i], "online", NULL);
            gchar *t = read_file(tp), *o = read_file(op);
            if (t && o && !strcmp(t, "Mains") && !strcmp(o, "1"))
                b->plugged = 1;
            g_free(t); g_free(o); g_free(tp); g_free(op);
        }
    }
    globfree(&g);

    if (glob("/sys/class/power_supply/BAT*", 0, NULL, &g) != 0 || g.gl_pathc == 0) {
        globfree(&g);
        return;
    }
    const char *bat = g.gl_pathv[0];
    const char *names[] = { "capacity", "status", "cycle_count",
                            "energy_full", "energy_full_design",
                            "charge_full", "charge_full_design" };
    gchar *v[7];
    for (int i = 0; i < 7; i++) {
        gchar *p = g_build_filename(bat, names[i], NULL);
        v[i] = read_file(p);
        g_free(p);
    }
    globfree(&g);

    b->present = 1;
    if (v[0]) b->level = atoi(v[0]);
    g_strlcpy(b->status, v[1] ? v[1] : "Unknown", sizeof b->status);
    if (v[2] && atoi(v[2]) > 0) b->cycles = atoi(v[2]);
    const char *full = v[3] ? v[3] : v[5], *design = v[4] ? v[4] : v[6];
    if (full && design && atof(design) > 0)
        b->health = (int)lround(100.0 * atof(full) / atof(design));
    for (int i = 0; i < 7; i++)
        g_free(v[i]);
}

/* ----------------------------------------------------------------- CLI -- */

static int run_cli(const char *arg)
{
    if (!strcmp(arg, "--version")) {
        printf("ConservationMode %s\n", APP_VERSION);
        return 0;
    }
    if (!strcmp(arg, "on") || !strcmp(arg, "off")) {
        gchar **argv = apply_argv(!strcmp(arg, "on"));
        gint status = 0;
        GError *err = NULL;
        gboolean ok = g_spawn_sync(NULL, argv, NULL,
                                   G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                                   NULL, NULL, NULL, NULL, &status, &err);
        g_strfreev(argv);
        if (!ok) {
            fprintf(stderr, "Error: %s\n", err->message);
            g_error_free(err);
            return 1;
        }
        if (pkexec_cancelled(status)) {
            fprintf(stderr, "Cancelled.\n");
            return 1;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return 1;
    } else if (strcmp(arg, "status") != 0) {
        fprintf(stderr, "Usage: ConservationMode [status|on|off|--version]\n");
        return 2;
    }
    int s = read_state();
    if (s < 0) {
        fprintf(stderr, "Error: conservation_mode not found (ideapad_laptop module not loaded).\n");
        return 1;
    }
    printf("Conservation Mode: %s\n", s ? "ON" : "OFF");
    return 0;
}

/* ----------------------------------------------------------------- GUI -- */

typedef struct {
    GtkWidget *window, *art, *title, *sw, *spinner, *sub, *msg;
    GtkWidget *level, *state, *health;
    gboolean syncing, working;
    int wanted, mode, level_pct;
} Ui;

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    r = fmin(r, fmin(w, h) / 2);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

/* Battery picture, same geometry as the Windows build (1024-unit design). */
static gboolean draw_battery(GtkWidget *w, cairo_t *cr, gpointer data)
{
    Ui *ui = data;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double s = 72.0 / 1024.0;
    cairo_translate(cr, (W - 1024 * s) / 2, (H - 1024 * s) / 2);
    cairo_scale(cr, s, s);

    GdkRGBA fg;
    gtk_style_context_get_color(gtk_widget_get_style_context(w), GTK_STATE_FLAG_NORMAL, &fg);
    gdk_cairo_set_source_rgba(cr, &fg);
    rounded_rect(cr, 432, 90, 160, 100, 40);
    cairo_fill(cr);
    rounded_rect(cr, 262, 170, 500, 760, 90);
    rounded_rect(cr, 302, 210, 420, 680, 60);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_fill(cr);

    int lvl = ui->level_pct < 0 ? 0 : ui->level_pct > 100 ? 100 : ui->level_pct;
    double top = 860 - 620.0 * lvl / 100.0;
    if (lvl > 0) {
        if (ui->mode == 1) cairo_set_source_rgb(cr, 0x2e / 255.0, 0xc2 / 255.0, 0x7e / 255.0);
        else               cairo_set_source_rgb(cr, 0x35 / 255.0, 0x84 / 255.0, 0xe4 / 255.0);
        rounded_rect(cr, 332, top, 360, 860 - top, 40);
        cairo_fill(cr);
    }
    if (ui->mode == 1) { /* 60% cap marker */
        cairo_set_source_rgb(cr, 0x26 / 255.0, 0xa2 / 255.0, 0x69 / 255.0);
        rounded_rect(cr, 322, 860 - 620 * 0.6 - 30, 380, 20, 10);
        cairo_fill(cr);
    }
    return FALSE;
}

static void refresh_battery(Ui *ui)
{
    Battery b;
    read_battery(&b);
    ui->level_pct = b.level;

    if (!b.present) {
        gtk_label_set_text(GTK_LABEL(ui->level), "No battery found");
        gtk_label_set_text(GTK_LABEL(ui->state), "-");
        gtk_label_set_text(GTK_LABEL(ui->health), "-");
        return;
    }
    gchar *s = g_strdup_printf("%d%%", b.level);
    gtk_label_set_text(GTK_LABEL(ui->level), s);
    g_free(s);

    const char *suffix = "";
    if (b.plugged)
        suffix = !strcmp(b.status, "Not charging") ? " (plugged in, holding level)" : " (plugged in)";
    s = g_strdup_printf("%s%s", b.status, suffix);
    gtk_label_set_text(GTK_LABEL(ui->state), s);
    g_free(s);

    if (b.health < 0)
        s = g_strdup("Unknown");
    else if (b.cycles > 0)
        s = g_strdup_printf("%d%% of original capacity · %d cycles", b.health, b.cycles);
    else
        s = g_strdup_printf("%d%% of original capacity", b.health);
    gtk_label_set_text(GTK_LABEL(ui->health), s);
    g_free(s);
}

static gboolean refresh(gpointer data)
{
    Ui *ui = data;
    int s = read_state();
    ui->mode = s;

    if (!ui->working) {
        ui->syncing = TRUE;
        gtk_switch_set_active(GTK_SWITCH(ui->sw), s == 1);
        ui->syncing = FALSE;
        gtk_widget_set_sensitive(ui->sw, s >= 0);
    }
    gtk_label_set_text(GTK_LABEL(ui->title),
        s < 0 ? "Not available" : s ? "Conservation Mode is ON" : "Conservation Mode is OFF");
    gtk_label_set_text(GTK_LABEL(ui->sub),
        s < 0 ? "ideapad_laptop driver is not loaded"
        : s   ? "Battery is held at about 60%"
              : "Battery charges to 100%");
    refresh_battery(ui);
    gtk_widget_queue_draw(ui->art);
    return G_SOURCE_CONTINUE;
}

static void show_msg(Ui *ui, const char *text, gboolean error)
{
    GtkStyleContext *c = gtk_widget_get_style_context(ui->msg);
    if (error) gtk_style_context_add_class(c, "error");
    else       gtk_style_context_remove_class(c, "error");
    gtk_label_set_text(GTK_LABEL(ui->msg), text);
}

static void on_apply_done(GPid pid, gint status, gpointer data)
{
    Ui *ui = data;
    g_spawn_close_pid(pid);
    ui->working = FALSE;
    gtk_spinner_stop(GTK_SPINNER(ui->spinner));
    gtk_widget_hide(ui->spinner);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        show_msg(ui, ui->wanted ? "Conservation Mode turned ON." : "Conservation Mode turned OFF.", FALSE);
    else if (pkexec_cancelled(status))
        show_msg(ui, "Cancelled. Nothing was changed.", FALSE);
    else
        show_msg(ui, "Could not change the setting.", TRUE);
    refresh(ui);
}

static void on_toggled(GObject *obj, GParamSpec *ps, gpointer data)
{
    (void)ps;
    Ui *ui = data;
    if (ui->syncing || ui->working)
        return;
    int want = gtk_switch_get_active(GTK_SWITCH(obj));
    if (want == read_state())
        return;

    gchar **argv = apply_argv(want);
    GPid pid;
    GError *err = NULL;
    gboolean ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                NULL, NULL, &pid, &err);
    g_strfreev(argv);
    if (!ok) {
        show_msg(ui, err->message, TRUE);
        g_error_free(err);
        refresh(ui);
        return;
    }
    ui->working = TRUE;
    ui->wanted = want;
    show_msg(ui, "", FALSE);
    gtk_widget_show(ui->spinner);
    gtk_spinner_start(GTK_SPINNER(ui->spinner));
    g_child_watch_add(pid, on_apply_done, ui);
}

static GtkWidget *label(const char *text, const char *css_class, float xalign)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    if (css_class)
        gtk_style_context_add_class(gtk_widget_get_style_context(l), css_class);
    return l;
}

static GtkWidget *info_row(GtkWidget *grid, int row, const char *name)
{
    GtkWidget *v = label("", NULL, 0);
    gtk_grid_attach(GTK_GRID(grid), label(name, "dim-label", 0), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
    return v;
}

static const char *CSS =
    ".hero-title { font-size: 17pt; font-weight: bold; }\n"
    ".section { font-weight: bold; }\n"
    ".card { background-color: @theme_base_color; border: 1px solid alpha(@borders, 0.8);"
    "        border-radius: 12px; padding: 14px 16px; }\n"
    ".error { color: #c01c28; }\n";

static void on_activate(GtkApplication *app, gpointer data)
{
    Ui *ui = data;
    GList *wins = gtk_application_get_windows(app);
    if (wins) {
        gtk_window_present(GTK_WINDOW(wins->data));
        return;
    }

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    ui->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ui->window), "Conservation Mode");
    gtk_window_set_icon_name(GTK_WINDOW(ui->window), "conservation-mode");
    gtk_window_set_resizable(GTK_WINDOW(ui->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(ui->window), 420, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_set(box, "margin", 22, NULL);
    gtk_container_add(GTK_CONTAINER(ui->window), box);

    /* hero: battery picture, title, description */
    ui->art = gtk_drawing_area_new();
    gtk_widget_set_size_request(ui->art, 72, 72);
    g_signal_connect(ui->art, "draw", G_CALLBACK(draw_battery), ui);
    gtk_box_pack_start(GTK_BOX(box), ui->art, FALSE, FALSE, 0);

    ui->title = label("", "hero-title", 0.5);
    gtk_widget_set_margin_top(ui->title, 10);
    gtk_box_pack_start(GTK_BOX(box), ui->title, FALSE, FALSE, 0);

    GtkWidget *desc = label("When ON, the battery stops charging at about 60%\n"
                            "to extend its lifespan. Same setting as Lenovo Vantage.",
                            "dim-label", 0.5);
    gtk_label_set_justify(GTK_LABEL(desc), GTK_JUSTIFY_CENTER);
    gtk_widget_set_margin_top(desc, 6);
    gtk_box_pack_start(GTK_BOX(box), desc, FALSE, FALSE, 0);

    /* card with the switch */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "card");
    gtk_widget_set_margin_top(card, 20);

    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(texts), label("Conservation Mode", "section", 0), FALSE, FALSE, 0);
    ui->sub = label("", "dim-label", 0);
    gtk_box_pack_start(GTK_BOX(texts), ui->sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), texts, TRUE, TRUE, 0);

    ui->spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(card), ui->spinner, FALSE, FALSE, 0);
    ui->sw = gtk_switch_new();
    gtk_widget_set_valign(ui->sw, GTK_ALIGN_CENTER);
    g_signal_connect(ui->sw, "notify::active", G_CALLBACK(on_toggled), ui);
    gtk_box_pack_start(GTK_BOX(card), ui->sw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);

    /* battery details */
    GtkWidget *bt = label("Battery", "section", 0);
    gtk_widget_set_margin_top(bt, 20);
    gtk_box_pack_start(GTK_BOX(box), bt, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_widget_set_margin_top(grid, 8);
    ui->level  = info_row(grid, 0, "Level");
    ui->state  = info_row(grid, 1, "Status");
    ui->health = info_row(grid, 2, "Health");
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    ui->msg = label("", NULL, 0);
    gtk_label_set_line_wrap(GTK_LABEL(ui->msg), TRUE);
    gtk_widget_set_margin_top(ui->msg, 14);
    gtk_box_pack_start(GTK_BOX(box), ui->msg, FALSE, FALSE, 0);

    gtk_widget_show_all(ui->window);
    gtk_widget_hide(ui->spinner);
    refresh(ui);
    g_timeout_add_seconds(5, refresh, ui);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--apply")) {
        if (geteuid() != 0) {
            fprintf(stderr, "--apply must run as root\n");
            return 1;
        }
        if (!strcmp(argv[2], "on"))  return apply_as_root(1);
        if (!strcmp(argv[2], "off")) return apply_as_root(0);
        return 2;
    }
    if (argc == 2)
        return run_cli(argv[1]);

    Ui ui = { .mode = -1, .level_pct = -1 };
#if GLIB_CHECK_VERSION(2, 74, 0)
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
#else
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_FLAGS_NONE);
#endif
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &ui);
    int r = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return r;
}
