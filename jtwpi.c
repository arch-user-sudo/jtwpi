/*
 * jtwpi - a desktop icon layer for wlroots Wayland compositors
 *
 * GTK3 + gtk-layer-shell. Icons are read from the XDG desktop directory and
 * rendered with the icon theme configured in GTK (gtk-icon-theme-name), so it
 * follows whatever the rest of the desktop is using.
 *
 * Build: make
 * Run:   ./jtwpi          (layer-shell desktop layer)
 *        ./jtwpi --windowed
 */

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <gtk-layer-shell/gtk-layer-shell.h>
#include <gtk/gtk.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_ID            "org.jtl.jtwpi"
#define APP_NAME          "jtwpi"
#define APP_VERSION       "1.0.0"

#define DEFAULT_ICON_SIZE 48
#define MIN_ICON_SIZE     24
#define MAX_ICON_SIZE     128
#define CELL_PAD_X        12
#define CELL_PAD_TOP      8
#define CELL_LABEL_GAP    3
#define LABEL_HEIGHT      34
#define CELL_PAD_BOTTOM   8
#define GRID_MARGIN       24
#define BOTTOM_MARGIN     56
#define DRAG_THRESHOLD    4
#define MAX_THUMB_BYTES   (64LL * 1024 * 1024)

/* Startup waits.  Polling sleeps between checks, so a compositor that is
 * still coming up does not cost any CPU while we wait. */
#define STARTUP_POLL_MS          100
#define STARTUP_POLL_US          (STARTUP_POLL_MS * 1000UL)
#define WAYLAND_SOCKET_WAIT_MS   10000
#define LAYER_SHELL_WAIT_MS      5000
#define WAIT_ATTEMPTS(ms)        ((ms) / STARTUP_POLL_MS)

#define FILE_ATTRS \
    G_FILE_ATTRIBUTE_STANDARD_NAME "," \
    G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," \
    G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
    G_FILE_ATTRIBUTE_STANDARD_ICON "," \
    G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," \
    G_FILE_ATTRIBUTE_STANDARD_SIZE "," \
    G_FILE_ATTRIBUTE_TIME_MODIFIED

typedef struct App App;
typedef struct Canvas Canvas;
typedef struct Icon Icon;

struct Icon {
    Canvas *canvas;
    char *path;
    char *key;                 /* basename, used for saved positions */
    char *name;                /* display name */
    char *content_type;
    gboolean is_dir;
    gboolean is_desktop;
    gboolean is_image;
    GIcon *icon;
    GDesktopAppInfo *app_info;
    GdkPixbuf *pixbuf;
    int x, y;                  /* -1 means "not placed yet" */
    int want_x, want_y;        /* intended position, before edge clamping */
    gboolean auto_placed;      /* placed by the grid, not by the user */
    int drag_ox, drag_oy;
    gboolean selected;
    gboolean hover;
    goffset size;
    GDateTime *mtime;
};

struct Canvas {
    App *app;
    GtkWidget *window;
    GtkWidget *area;
    char *monitor_key;
    int width, height, scale;
    GPtrArray *icons;
    Icon *hover;
    Icon *pressed;
    gboolean dragging;
    gboolean drag_moved;
    gboolean dnd_active;
    double press_x, press_y;
    double drag_dx, drag_dy;
    gboolean selecting;
    double sel_x0, sel_y0, sel_x1, sel_y1;
    GtkWidget *menu;
    GdkCursor *cursor_default;
};

struct App {
    GtkApplication *gtk_app;
    GPtrArray *canvases;
    char *desktop_dir;
    GFile *desktop_file;
    GtkIconTheme *icon_theme;
    GtkSettings *settings;
    GKeyFile *state;
    char *state_path;
    GFileMonitor *dir_monitor;
    guint reload_timeout;
    guint save_timeout;
    guint rebuild_timeout;
    int icon_size;
    gboolean snap;
    gboolean show_icons;
    gboolean windowed;
    gboolean rebuilding;
    char *opt_desktop;
    int opt_icon_size;
    gboolean opt_hidden;
    PangoFontDescription *label_font;
    GList *clip_files;
    gboolean clip_cut;
};

static App *g_app = NULL;
static Canvas *g_menu_canvas = NULL;
static char *g_menu_target = NULL;
static gboolean g_menu_multi = FALSE;

/* ---- forward declarations ------------------------------------------------- */

static void canvas_queue_draw(Canvas *c);
static void app_save_soon(void);
static void app_state_save(void);
static void app_reload_all(void);
static void app_reload_icons(void);
static void app_arrange_icons(void);
static void app_select_all(Canvas *c);
static void app_clear_selection(Canvas *c);
static void app_set_icon_size(int size);
static void app_toggle_snap(void);
static void app_toggle_show_icons(void);
static void app_open_icon(Canvas *c, Icon *ic);
static void app_open_folder(Canvas *c);
static void app_new_folder(Canvas *c);
static void app_new_launcher(Canvas *c);
static void app_copy_cut(Canvas *c, gboolean cut);
static void app_paste(Canvas *c, const char *dest_dir);
static void app_delete_selected(Canvas *c, gboolean permanent);
static void app_rename_selected(Canvas *c);
static void app_show_properties(Canvas *c);
static void app_show_error(Canvas *c, const char *title, const char *message);
static gboolean app_confirm(Canvas *c, const char *title, const char *message);
static int canvas_selected_count(Canvas *c);
static GList *canvas_selected_paths(Canvas *c);
static void canvas_select_key(Canvas *c, const char *key);
static void canvas_select_paths(Canvas *c, GList *paths);
static Icon *canvas_hit_test(Canvas *c, double x, double y);
static int canvas_cell_w(void);
static int canvas_cell_h(void);
static void canvas_snap_xy(int *x, int *y);
static void canvas_clamp_icon(Canvas *c, Icon *ic);
static void canvas_auto_place(Canvas *c, Icon *ic);
static void canvas_relayout(Canvas *c, gboolean reflow_auto);
static void canvas_find_free_cell(Canvas *c, int *x, int *y, Icon *except);
static void canvas_resolve_overlaps(Canvas *c, gboolean selected_only,
                                    gboolean update_want);
static void canvas_load(Canvas *c);
static void canvas_reload(Canvas *c);
static void canvas_load_icon(Icon *ic);
static void menu_open_icon(Canvas *c, Icon *ic, GdkEvent *event);
static void menu_open_desktop(Canvas *c, GdkEvent *event);
static Canvas *canvas_from_area(GtkWidget *area);
static gboolean canvas_begin_dnd(Canvas *c, GdkEvent *event, double x, double y);
static void app_create_windows(void);
static void app_destroy_windows(void);
static void app_schedule_rebuild_windows(void);

/* ---- small helpers -------------------------------------------------------- */

static void
rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI_2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI_2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI_2);
    cairo_close_path(cr);
}

static char *
state_escape(const char *name)
{
    return g_uri_escape_string(name, NULL, TRUE);
}

static char *
sanitize_key(const char *in)
{
    char *s = g_strdup(in ? in : "monitor");
    for (char *p = s; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
            *p = '-';
    if (!s[0]) {
        g_free(s);
        s = g_strdup("monitor");
    }
    return s;
}

static char *
monitor_key_for(GdkMonitor *mon, int index)
{
    const char *model = gdk_monitor_get_model(mon);
    const char *man = gdk_monitor_get_manufacturer(mon);
    char *raw = g_strdup_printf("%s-%s-%d", man ? man : "output",
                                model ? model : "monitor", index);
    char *key = sanitize_key(raw);
    g_free(raw);
    return key;
}

static char *
expand_home(const char *path)
{
    if (!path || !path[0])
        return NULL;
    if (g_str_has_prefix(path, "~/"))
        return g_build_filename(g_get_home_dir(), path + 2, NULL);
    if (g_strcmp0(path, "~") == 0)
        return g_strdup(g_get_home_dir());
    return g_strdup(path);
}

static char *
desktop_directory(void)
{
    char *dir = NULL;
    if (g_app->opt_desktop && g_app->opt_desktop[0]) {
        dir = expand_home(g_app->opt_desktop);
    } else {
        char *cfg = g_build_filename(g_get_user_config_dir(), "user-dirs.dirs", NULL);
        char *data = NULL;
        if (g_file_get_contents(cfg, &data, NULL, NULL)) {
            char **lines = g_strsplit(data, "\n", -1);
            for (int i = 0; lines[i]; i++) {
                char *line = g_strstrip(lines[i]);
                if (g_str_has_prefix(line, "XDG_DESKTOP_DIR=")) {
                    char *v = line + strlen("XDG_DESKTOP_DIR=");
                    while (*v == ' ' || *v == '\t') v++;
                    char *end = v + strlen(v);
                    while (end > v && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
                    if (*v == '"' && end > v + 1 && end[-1] == '"') {
                        v++;
                        end[-1] = '\0';
                    }
                    if (g_str_has_prefix(v, "$HOME/"))
                        dir = g_build_filename(g_get_home_dir(), v + 6, NULL);
                    else if (g_strcmp0(v, "$HOME") == 0)
                        dir = g_strdup(g_get_home_dir());
                    else if (v[0] == '/')
                        dir = g_strdup(v);
                    break;
                }
            }
            g_strfreev(lines);
            g_free(data);
        }
        g_free(cfg);
        if (!dir)
            dir = g_build_filename(g_get_home_dir(), "Desktop", NULL);
    }
    g_mkdir_with_parents(dir, 0755);
    return dir;
}

/* ---- state --------------------------------------------------------------- */

static gboolean
key_bool(const char *group, const char *key, gboolean fallback)
{
    GError *err = NULL;
    gboolean v = g_key_file_get_boolean(g_app->state, group, key, &err);
    if (err) {
        g_clear_error(&err);
        return fallback;
    }
    return v;
}

static int
key_int(const char *group, const char *key, int fallback)
{
    GError *err = NULL;
    int v = g_key_file_get_integer(g_app->state, group, key, &err);
    if (err) {
        g_clear_error(&err);
        return fallback;
    }
    return v;
}

static void
app_state_load(void)
{
    g_app->state = g_key_file_new();
    g_key_file_load_from_file(g_app->state, g_app->state_path,
                              G_KEY_FILE_NONE, NULL);
    g_app->show_icons = key_bool("general", "show_icons", TRUE);
    g_app->snap = key_bool("general", "snap", TRUE);
    g_app->icon_size = key_int("general", "icon_size", DEFAULT_ICON_SIZE);
    if (g_app->opt_icon_size > 0)
        g_app->icon_size = g_app->opt_icon_size;
    if (g_app->icon_size < MIN_ICON_SIZE)
        g_app->icon_size = MIN_ICON_SIZE;
    if (g_app->icon_size > MAX_ICON_SIZE)
        g_app->icon_size = MAX_ICON_SIZE;
    if (g_app->opt_hidden)
        g_app->show_icons = FALSE;
}

static void
app_state_save(void)
{
    if (!g_app || !g_app->state)
        return;
    if (g_app->save_timeout) {
        g_source_remove(g_app->save_timeout);
        g_app->save_timeout = 0;
    }

    g_key_file_set_boolean(g_app->state, "general", "show_icons",
                           g_app->show_icons);
    g_key_file_set_boolean(g_app->state, "general", "snap", g_app->snap);
    g_key_file_set_integer(g_app->state, "general", "icon_size",
                           g_app->icon_size);

    for (guint ci = 0; ci < g_app->canvases->len; ci++) {
        Canvas *c = g_ptr_array_index(g_app->canvases, ci);
        char *group = g_strdup_printf("monitor-%s", c->monitor_key);
        for (guint i = 0; i < c->icons->len; i++) {
            Icon *ic = g_ptr_array_index(c->icons, i);
            int sx = ic->x;
            int sy = ic->y;
            if (!ic->auto_placed && ic->want_x >= 0 && ic->want_y >= 0) {
                sx = ic->want_x;
                sy = ic->want_y;
            }
            if (sx < 0 || sy < 0)
                continue;
            char *k = state_escape(ic->key);
            char *v = g_strdup_printf("%d,%d,%d", sx, sy,
                                      ic->auto_placed ? 1 : 0);
            g_key_file_set_string(g_app->state, group, k, v);
            g_free(v);
            g_free(k);
        }
        g_free(group);
    }

    gsize len = 0;
    char *data = g_key_file_to_data(g_app->state, &len, NULL);
    if (data) {
        char *parent = g_path_get_dirname(g_app->state_path);
        g_mkdir_with_parents(parent, 0755);
        g_file_set_contents(g_app->state_path, data, (gssize)len, NULL);
        g_free(parent);
        g_free(data);
    }
}

static gboolean
save_timeout_cb(gpointer data)
{
    (void)data;
    g_app->save_timeout = 0;
    app_state_save();
    return G_SOURCE_REMOVE;
}

static void
app_save_soon(void)
{
    if (!g_app || !g_app->state)
        return;
    if (g_app->save_timeout)
        g_source_remove(g_app->save_timeout);
    g_app->save_timeout = g_timeout_add(250, save_timeout_cb, NULL);
}

static gboolean
parse_position_value(const char *v, int *x, int *y, gboolean *auto_placed)
{
    if (!v || !v[0])
        return FALSE;

    char *end = NULL;
    gint64 px = g_ascii_strtoll(v, &end, 10);
    if (end == v || *end != ',')
        return FALSE;

    const char *p = end + 1;
    gint64 py = g_ascii_strtoll(p, &end, 10);
    if (end == p)
        return FALSE;

    gboolean pauto = FALSE;
    if (*end == ',') {
        const char *q = end + 1;
        gint64 pv = g_ascii_strtoll(q, &end, 10);
        if (end != q)
            pauto = pv != 0;
    }

    *x = (int)px;
    *y = (int)py;
    if (auto_placed)
        *auto_placed = pauto;
    return TRUE;
}

static gboolean
state_get_position(Canvas *c, const char *key, int *x, int *y,
                   gboolean *auto_placed)
{
    char *group = g_strdup_printf("monitor-%s", c->monitor_key);
    char *k = state_escape(key);
    char *v = g_key_file_get_string(g_app->state, group, k, NULL);
    g_free(k);
    g_free(group);
    if (!v)
        return FALSE;
    int px = -1;
    int py = -1;
    gboolean pauto = FALSE;
    gboolean ok = parse_position_value(v, &px, &py, &pauto);
    g_free(v);
    if (!ok)
        return FALSE;
    *x = px;
    *y = py;
    if (auto_placed)
        *auto_placed = pauto;
    return TRUE;
}

static void
state_forget_position(Canvas *c, const char *key)
{
    char *group = g_strdup_printf("monitor-%s", c->monitor_key);
    char *k = state_escape(key);
    g_key_file_remove_key(g_app->state, group, k, NULL);
    g_free(k);
    g_free(group);
}

static void
state_move_position(Canvas *c, const char *old_key, const char *new_key,
                    int x, int y, gboolean auto_placed)
{
    state_forget_position(c, old_key);
    char *group = g_strdup_printf("monitor-%s", c->monitor_key);
    char *k = state_escape(new_key);
    char *v = g_strdup_printf("%d,%d,%d", x, y, auto_placed ? 1 : 0);
    g_key_file_set_string(g_app->state, group, k, v);
    g_free(v);
    g_free(k);
    g_free(group);
}

/* ---- icons --------------------------------------------------------------- */

static void
icon_free(Icon *ic)
{
    if (!ic) return;
    g_free(ic->path);
    g_free(ic->key);
    g_free(ic->name);
    g_free(ic->content_type);
    g_clear_object(&ic->icon);
    g_clear_object(&ic->app_info);
    g_clear_object(&ic->pixbuf);
    if (ic->mtime) g_date_time_unref(ic->mtime);
    g_free(ic);
}

static void
canvas_load_icon(Icon *ic)
{
    g_clear_object(&ic->pixbuf);
    int size = g_app->icon_size;
    int scale = ic->canvas->scale > 0 ? ic->canvas->scale : 1;

    if (ic->is_image) {
        GError *err = NULL;
        ic->pixbuf = gdk_pixbuf_new_from_file_at_scale(ic->path,
                                                       size * scale,
                                                       size * scale,
                                                       TRUE, &err);
        if (!ic->pixbuf)
            g_clear_error(&err);
    }
    if (!ic->pixbuf && ic->icon) {
        GtkIconInfo *info = gtk_icon_theme_lookup_by_gicon_for_scale(
            g_app->icon_theme, ic->icon, size, scale,
            GTK_ICON_LOOKUP_FORCE_SIZE);
        if (info) {
            GError *err = NULL;
            ic->pixbuf = gtk_icon_info_load_icon(info, &err);
            if (!ic->pixbuf)
                g_clear_error(&err);
            g_object_unref(info);
        }
    }
    if (!ic->pixbuf)
        ic->pixbuf = gtk_icon_theme_load_icon_for_scale(
            g_app->icon_theme, "text-x-generic", size, scale, 0, NULL);
}

static Icon *
icon_new(Canvas *c, const char *path)
{
    Icon *ic = g_new0(Icon, 1);
    ic->canvas = c;
    ic->path = g_strdup(path);
    ic->key = g_path_get_basename(path);
    ic->x = ic->y = -1;
    ic->want_x = ic->want_y = -1;

    GFile *file = g_file_new_for_path(path);
    GFileInfo *info = g_file_query_info(file, FILE_ATTRS,
                                        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (info) {
        const char *dn = g_file_info_get_display_name(info);
        ic->name = g_strdup(dn && dn[0] ? dn : ic->key);
        ic->size = g_file_info_get_size(info);
        ic->is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
        const char *ct = g_file_info_get_content_type(info);
        ic->content_type = g_strdup(ct ? ct : "");
        GIcon *gi = g_file_info_get_icon(info);
        if (gi)
            ic->icon = g_object_ref(gi);
        guint64 mt = g_file_info_get_attribute_uint64(
            info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        if (mt)
            ic->mtime = g_date_time_new_from_unix_local((gint64)mt);
    }
    if (!ic->name)
        ic->name = g_strdup(ic->key);

    ic->is_desktop = g_str_has_suffix(ic->key, ".desktop");
    if (ic->is_desktop) {
        ic->app_info = g_desktop_app_info_new_from_filename(path);
        if (ic->app_info) {
            const char *an = g_app_info_get_name(G_APP_INFO(ic->app_info));
            if (an && an[0]) {
                g_free(ic->name);
                ic->name = g_strdup(an);
            }
            GIcon *ai = g_app_info_get_icon(G_APP_INFO(ic->app_info));
            if (ai) {
                g_clear_object(&ic->icon);
                ic->icon = g_object_ref(ai);
            }
        }
    }
    if (ic->is_dir) {
        g_clear_object(&ic->icon);
        ic->icon = g_themed_icon_new("folder");
    }
    if (!ic->icon)
        ic->icon = g_content_type_get_icon(ic->content_type);

    ic->is_image = !ic->is_desktop && !ic->is_dir &&
                   ic->content_type && g_str_has_prefix(ic->content_type, "image/") &&
                   ic->size > 0 && ic->size <= MAX_THUMB_BYTES;

    canvas_load_icon(ic);
    g_clear_object(&file);
    g_clear_object(&info);
    return ic;
}

static gint
icon_name_cmp(gconstpointer a, gconstpointer b)
{
    const Icon *ia = *(const Icon * const *)a;
    const Icon *ib = *(const Icon * const *)b;
    return g_utf8_collate(ia->name, ib->name);
}

static void
canvas_clear_icons(Canvas *c)
{
    if (!c->icons) return;
    for (guint i = 0; i < c->icons->len; i++)
        icon_free(g_ptr_array_index(c->icons, i));
    g_ptr_array_set_size(c->icons, 0);
    c->hover = NULL;
    c->pressed = NULL;
}

/* Two icons can never share a grid cell.  This also heals old state files
 * that were written before initial auto-placement was re-flowed. */
static void
canvas_dedup_positions(Canvas *c)
{
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *a = g_ptr_array_index(c->icons, i);
        if (a->x < 0 || a->y < 0)
            continue;
        for (guint j = 0; j < i; j++) {
            Icon *b = g_ptr_array_index(c->icons, j);
            if (b->x == a->x && b->y == a->y) {
                a->x = a->y = -1;
                a->want_x = a->want_y = -1;
                a->auto_placed = TRUE;
                break;
            }
        }
    }
}

static void
canvas_load(Canvas *c)
{
    if (!c->icons)
        c->icons = g_ptr_array_new();
    canvas_clear_icons(c);

    GDir *dir = g_dir_open(g_app->desktop_dir, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir)) != NULL) {
            if (name[0] == '.')
                continue;
            char *path = g_build_filename(g_app->desktop_dir, name, NULL);
            Icon *ic = icon_new(c, path);
            g_free(path);
            g_ptr_array_add(c->icons, ic);
        }
        g_dir_close(dir);
    }
    g_ptr_array_sort(c->icons, icon_name_cmp);

    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        int x;
        int y;
        gboolean was_auto = FALSE;
        if (state_get_position(c, ic->key, &x, &y, &was_auto)) {
            ic->x = x;
            ic->y = y;
            ic->want_x = was_auto ? -1 : x;
            ic->want_y = was_auto ? -1 : y;
            ic->auto_placed = was_auto;
        } else {
            ic->auto_placed = TRUE;
        }
    }
    canvas_dedup_positions(c);
    canvas_relayout(c, TRUE);
}

static void
canvas_reload(Canvas *c)
{
    /* Remember positions of items that are still around, then reload.  The
     * state file is the canonical source, so this is just a convenience for
     * items that appeared since the last save. */
    GHashTable *old = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, g_free);
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        if (ic->x >= 0 && ic->y >= 0) {
            int sx = ic->x;
            int sy = ic->y;
            if (!ic->auto_placed && ic->want_x >= 0 && ic->want_y >= 0) {
                sx = ic->want_x;
                sy = ic->want_y;
            }
            char *v = g_strdup_printf("%d,%d,%d", sx, sy,
                                      ic->auto_placed ? 1 : 0);
            g_hash_table_replace(old, g_strdup(ic->key), v);
        }
    }
    canvas_clear_icons(c);

    GDir *dir = g_dir_open(g_app->desktop_dir, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir)) != NULL) {
            if (name[0] == '.')
                continue;
            char *path = g_build_filename(g_app->desktop_dir, name, NULL);
            Icon *ic = icon_new(c, path);
            g_free(path);
            g_ptr_array_add(c->icons, ic);
        }
        g_dir_close(dir);
    }
    g_ptr_array_sort(c->icons, icon_name_cmp);

    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        int x;
        int y;
        gboolean was_auto = FALSE;
        gboolean saved_auto = FALSE;
        if (state_get_position(c, ic->key, &x, &y, &saved_auto)) {
            ic->x = x;
            ic->y = y;
            ic->want_x = saved_auto ? -1 : x;
            ic->want_y = saved_auto ? -1 : y;
            ic->auto_placed = saved_auto;
        } else {
            const char *v = g_hash_table_lookup(old, ic->key);
            if (parse_position_value(v, &x, &y, &was_auto)) {
                ic->x = x;
                ic->y = y;
                ic->want_x = was_auto ? -1 : x;
                ic->want_y = was_auto ? -1 : y;
                ic->auto_placed = was_auto;
            } else {
                ic->auto_placed = TRUE;
            }
        }
    }
    g_hash_table_destroy(old);
    canvas_dedup_positions(c);
    canvas_relayout(c, FALSE);
    canvas_queue_draw(c);
}

/* ---- geometry ------------------------------------------------------------ */

static void
canvas_queue_draw(Canvas *c)
{
    if (c && c->area)
        gtk_widget_queue_draw(c->area);
}

static int
canvas_cell_w(void)
{
    return g_app->icon_size + (2 * CELL_PAD_X);
}

static int
canvas_cell_h(void)
{
    return CELL_PAD_TOP + g_app->icon_size + CELL_LABEL_GAP +
           LABEL_HEIGHT + CELL_PAD_BOTTOM;
}

static void
canvas_snap_xy(int *x, int *y)
{
    if (!g_app->snap)
        return;
    int cw = canvas_cell_w();
    int ch = canvas_cell_h();
    *x = GRID_MARGIN + ((int)lround((*x - GRID_MARGIN) / (double)cw) * cw);
    *y = GRID_MARGIN + ((int)lround((*y - GRID_MARGIN) / (double)ch) * ch);
}

static void
canvas_clamp_icon(Canvas *c, Icon *ic)
{
    int cw = canvas_cell_w();
    int ch = canvas_cell_h();
    int max_x = c->width - cw;
    int max_y = c->height - ch;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;

    /* Keep edge-clamped icons on the grid too, instead of letting them sit at
     * a non-grid position just because the surface is a little too small. */
    if (g_app->snap) {
        if (max_x >= GRID_MARGIN)
            max_x = GRID_MARGIN + (((max_x - GRID_MARGIN) / cw) * cw);
        else
            max_x = 0;
        if (max_y >= GRID_MARGIN)
            max_y = GRID_MARGIN + (((max_y - GRID_MARGIN) / ch) * ch);
        else
            max_y = 0;
    }

    if (ic->x < 0) ic->x = 0;
    if (ic->y < 0) ic->y = 0;
    if (ic->x > max_x) ic->x = max_x;
    if (ic->y > max_y) ic->y = max_y;
}

static gboolean
cells_overlap(int ax, int ay, int bx, int by, int cw, int ch)
{
    return ax < bx + cw && ax + cw > bx &&
           ay < by + ch && ay + ch > by;
}

static gboolean
cell_occupied(Canvas *c, int x, int y, Icon *except)
{
    int cw = canvas_cell_w();
    int ch = canvas_cell_h();
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        if (ic == except || ic->x < 0 || ic->y < 0)
            continue;
        if (cells_overlap(x, y, ic->x, ic->y, cw, ch))
            return TRUE;
    }
    return FALSE;
}

static int
icon_index(Canvas *c, Icon *ic)
{
    for (guint i = 0; i < c->icons->len; i++) {
        if (g_ptr_array_index(c->icons, i) == ic)
            return (int)i;
    }
    return 0;
}

static void
canvas_auto_place(Canvas *c, Icon *ic)
{
    ic->auto_placed = TRUE;
    ic->want_x = ic->want_y = -1;
    int cw = canvas_cell_w();
    int ch = canvas_cell_h();
    int left = GRID_MARGIN;
    int top = GRID_MARGIN;
    int max_x = c->width - cw - GRID_MARGIN;
    int max_y = c->height - ch - BOTTOM_MARGIN;
    if (max_x < left) max_x = left;
    if (max_y < top) max_y = top;
    int cols = ((max_x - left) / cw) + 1;
    int rows = ((max_y - top) / ch) + 1;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    for (int col = 0; col < cols; col++) {
        for (int row = 0; row < rows; row++) {
            int x = left + (col * cw);
            int y = top + (row * ch);
            if (!cell_occupied(c, x, y, ic)) {
                ic->x = x;
                ic->y = y;
                return;
            }
        }
    }
    /* The grid is full.  Cascade by icon index instead of stacking every
     * overflow icon on the same cell. */
    int idx = icon_index(c, ic);
    ic->x = left + ((idx % 5) * cw);
    ic->y = top + ((idx % 7) * ch);
}

static void
canvas_relayout(Canvas *c, gboolean reflow_auto)
{
    if (c->width <= 0 || c->height <= 0)
        return;

    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        if (ic->x < 0 || ic->y < 0) {
            ic->auto_placed = TRUE;
            ic->want_x = ic->want_y = -1;
        } else if (ic->auto_placed) {
            /* Auto-placed icons only re-flow when the grid itself changes
             * (surface size or icon size).  A directory reload must leave
             * them alone, otherwise every new file makes the whole desktop
             * shuffle around. */
            if (reflow_auto) {
                ic->x = ic->y = -1;
                ic->want_x = ic->want_y = -1;
            } else {
                canvas_clamp_icon(c, ic);
            }
        } else {
            /* Snap the intended position, not the clamped on-screen one, so
             * a temporary small allocation cannot permanently move icons. */
            int bx = ic->want_x >= 0 ? ic->want_x : ic->x;
            int by = ic->want_y >= 0 ? ic->want_y : ic->y;
            canvas_snap_xy(&bx, &by);
            ic->want_x = bx;
            ic->want_y = by;
            ic->x = bx;
            ic->y = by;
            canvas_clamp_icon(c, ic);
        }
    }

    /* Icons without a position yet (brand new entries) get the first free
     * cell.  This never moves icons that already have a position. */
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        if (ic->x < 0 || ic->y < 0)
            canvas_auto_place(c, ic);
    }

    /* Growing the icon size, shrinking the surface or clamping at an edge
     * can make two icons land on the same spot.  Push the later one into the
     * nearest free cell.  This deliberately leaves want_x/want_y alone so the
     * temporary 200x200 allocation GTK reports before layer-shell delivers
     * the real size cannot permanently relocate a manually placed icon. */
    canvas_resolve_overlaps(c, FALSE, FALSE);
    app_save_soon();
}

static Icon *
canvas_hit_test(Canvas *c, double x, double y)
{
    int cw = canvas_cell_w();
    int ch = canvas_cell_h();
    for (int i = (int)c->icons->len - 1; i >= 0; i--) {
        Icon *ic = g_ptr_array_index(c->icons, (guint)i);
        if (x >= ic->x && x < ic->x + cw &&
            y >= ic->y && y < ic->y + ch)
            return ic;
    }
    return NULL;
}

/* ---- selection ----------------------------------------------------------- */

static int
canvas_selected_count(Canvas *c)
{
    int n = 0;
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        if (ic->selected) n++;
    }
    return n;
}

static GList *
canvas_selected_paths(Canvas *c)
{
    GList *list = NULL;
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        if (ic->selected)
            list = g_list_prepend(list, g_strdup(ic->path));
    }
    return g_list_reverse(list);
}

static void
app_clear_selection(Canvas *c)
{
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        ic->selected = FALSE;
    }
    canvas_queue_draw(c);
}

static void
app_select_all(Canvas *c)
{
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        ic->selected = TRUE;
    }
    canvas_queue_draw(c);
}

static void
canvas_select_key(Canvas *c, const char *key)
{
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        ic->selected = (g_strcmp0(ic->key, key) == 0);
    }
    canvas_queue_draw(c);
}

static void
canvas_select_paths(Canvas *c, GList *paths)
{
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        ic->selected = FALSE;
        for (GList *l = paths; l; l = l->next) {
            if (g_strcmp0(ic->path, (const char *)l->data) == 0) {
                ic->selected = TRUE;
                break;
            }
        }
    }
    canvas_queue_draw(c);
}

static void
app_reload_all(void)
{
    for (guint i = 0; i < g_app->canvases->len; i++)
        canvas_reload(g_ptr_array_index(g_app->canvases, i));
}

static void
app_reload_icons(void)
{
    for (guint ci = 0; ci < g_app->canvases->len; ci++) {
        Canvas *c = g_ptr_array_index(g_app->canvases, ci);
        for (guint i = 0; i < c->icons->len; i++)
            canvas_load_icon(g_ptr_array_index(c->icons, i));
        canvas_queue_draw(c);
    }
}

static void
app_arrange_icons(void)
{
    for (guint ci = 0; ci < g_app->canvases->len; ci++) {
        Canvas *c = g_ptr_array_index(g_app->canvases, ci);
        for (guint i = 0; i < c->icons->len; i++) {
            Icon *ic = g_ptr_array_index(c->icons, i);
            ic->x = ic->y = -1;
            ic->want_x = ic->want_y = -1;
            ic->auto_placed = TRUE;
        }
        canvas_relayout(c, TRUE);
        canvas_queue_draw(c);
    }
    app_save_soon();
}

/* ---- drawing ------------------------------------------------------------- */

static void
draw_icon(cairo_t *cr, Canvas *c, Icon *ic, double dx, double dy)
{
    int cw = canvas_cell_w();
    int ch = canvas_cell_h();
    double x = ic->x + dx;
    double y = ic->y + dy;
    int scale = c->scale > 0 ? c->scale : 1;

    if (ic->selected) {
        cairo_set_source_rgba(cr, 0.20, 0.55, 1.0, 0.34);
        rounded_rect(cr, x + 2, y + 2, cw - 4, ch - 4, 7);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.55, 0.78, 1.0, 0.95);
        cairo_set_line_width(cr, 1.2);
        rounded_rect(cr, x + 2, y + 2, cw - 4, ch - 4, 7);
        cairo_stroke(cr);
    } else if (ic->hover) {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.11);
        rounded_rect(cr, x + 2, y + 2, cw - 4, ch - 4, 7);
        cairo_fill(cr);
    }

    if (ic->pixbuf) {
        int pw = gdk_pixbuf_get_width(ic->pixbuf);
        int ph = gdk_pixbuf_get_height(ic->pixbuf);
        double base = 1.0 / scale;
        double fit = MIN(g_app->icon_size / (pw * base),
                         g_app->icon_size / (ph * base));
        double s = base * fit;
        double dw = pw * s;
        double dh = ph * s;
        double ix = x + ((cw - dw) / 2.0);
        double iy = y + CELL_PAD_TOP + ((g_app->icon_size - dh) / 2.0);

        cairo_save(cr);
        cairo_translate(cr, ix, iy);
        cairo_scale(cr, s, s);
        gdk_cairo_set_source_pixbuf(cr, ic->pixbuf, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    if (ic->name && ic->name[0] && g_app->label_font) {
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, g_app->label_font);
        pango_layout_set_width(layout, (cw - 10) * PANGO_SCALE);
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_height(layout, -2);
        pango_layout_set_text(layout, ic->name, -1);

        double tx = x + 5;
        double ty = y + CELL_PAD_TOP + g_app->icon_size + CELL_LABEL_GAP;

        cairo_set_source_rgba(cr, 1, 1, 1, 1);
        cairo_move_to(cr, tx, ty);
        pango_cairo_update_layout(cr, layout);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }
}

static gboolean
on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    Canvas *c = data;
    (void)widget;

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (g_app->show_icons) {
        for (guint i = 0; i < c->icons->len; i++) {
            Icon *ic = g_ptr_array_index(c->icons, i);
            if (c->dragging && ic->selected)
                continue;
            draw_icon(cr, c, ic, 0, 0);
        }
        if (c->dragging) {
            for (guint i = 0; i < c->icons->len; i++) {
                Icon *ic = g_ptr_array_index(c->icons, i);
                if (ic->selected)
                    draw_icon(cr, c, ic, c->drag_dx, c->drag_dy);
            }
        }
    }

    if (c->selecting) {
        double x = MIN(c->sel_x0, c->sel_x1);
        double y = MIN(c->sel_y0, c->sel_y1);
        double w = fabs(c->sel_x1 - c->sel_x0);
        double h = fabs(c->sel_y1 - c->sel_y0);
        cairo_set_source_rgba(cr, 0.20, 0.55, 1.0, 0.16);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.55, 0.78, 1.0, 0.95);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, x, y, w, h);
        cairo_stroke(cr);
    }
    return TRUE;
}

/* ---- file helpers -------------------------------------------------------- */

static char *
unique_destination(const char *dir, const char *base)
{
    char *path = g_build_filename(dir, base, NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        return path;
    g_free(path);

    char *dot = strrchr(base, '.');
    char *stem = (dot && dot != base) ? g_strndup(base, (gsize)(dot - base))
                                      : g_strdup(base);
    char *ext = (dot && dot != base) ? g_strdup(dot) : g_strdup("");
    for (int i = 1; i < 10000; i++) {
        char *cand = (i == 1)
            ? g_strdup_printf("%s (copy)%s", stem, ext)
            : g_strdup_printf("%s (copy %d)%s", stem, i, ext);
        path = g_build_filename(dir, cand, NULL);
        g_free(cand);
        if (!g_file_test(path, G_FILE_TEST_EXISTS))
            break;
        g_free(path);
        path = NULL;
    }
    g_free(stem);
    g_free(ext);
    if (!path)
        path = g_build_filename(dir, base, NULL);
    return path;
}

static gboolean
delete_recursive(GFile *file, GError **err)
{
    GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                        G_FILE_QUERY_INFO_NONE, NULL, err);
    if (!info)
        return FALSE;
    gboolean ok = TRUE;
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *e = g_file_enumerate_children(
            file, G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NONE, NULL, err);
        if (!e) {
            g_object_unref(info);
            return FALSE;
        }
        GFileInfo *child;
        while ((child = g_file_enumerator_next_file(e, NULL, err)) != NULL) {
            const char *name = g_file_info_get_name(child);
            GFile *c = g_file_get_child(file, name);
            if (!delete_recursive(c, err))
                ok = FALSE;
            g_object_unref(c);
            g_object_unref(child);
            if (!ok)
                break;
        }
        g_object_unref(e);
    }
    g_object_unref(info);
    if (ok && !g_file_delete(file, NULL, err))
        ok = FALSE;
    return ok;
}

static gboolean
copy_recursive(GFile *src, GFile *dest, GError **err)
{
    GFileInfo *info = g_file_query_info(src, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                        G_FILE_QUERY_INFO_NONE, NULL, err);
    if (!info)
        return FALSE;

    gboolean ok = TRUE;
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        if (!g_file_make_directory(dest, NULL, err)) {
            if (!g_error_matches(*err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
                g_object_unref(info);
                return FALSE;
            }
            g_clear_error(err);
        }
        GFileEnumerator *e = g_file_enumerate_children(
            src, G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NONE, NULL, err);
        if (!e) {
            g_object_unref(info);
            return FALSE;
        }
        GFileInfo *child;
        while ((child = g_file_enumerator_next_file(e, NULL, err)) != NULL) {
            const char *name = g_file_info_get_name(child);
            GFile *cs = g_file_get_child(src, name);
            GFile *cd = g_file_get_child(dest, name);
            if (!copy_recursive(cs, cd, err))
                ok = FALSE;
            g_object_unref(cs);
            g_object_unref(cd);
            g_object_unref(child);
            if (!ok)
                break;
        }
        g_object_unref(e);
    } else {
        ok = g_file_copy(src, dest,
                         G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_OVERWRITE,
                         NULL, NULL, NULL, err);
    }
    g_object_unref(info);
    return ok;
}

static gboolean
move_file(GFile *src, GFile *dest, GError **err)
{
    if (g_file_move(src, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                    NULL, NULL, NULL, err))
        return TRUE;
    g_clear_error(err);
    if (!copy_recursive(src, dest, err))
        return FALSE;
    return delete_recursive(src, err);
}

/* ---- clipboard ----------------------------------------------------------- */

static void
clipboard_get_cb(GtkClipboard *clipboard, GtkSelectionData *sel,
                 guint info, gpointer data)
{
    (void)clipboard;
    (void)info;
    (void)data;
    GString *uris = g_string_new(NULL);
    GString *gnome = g_string_new(g_app->clip_cut ? "cut\n" : "copy\n");

    for (GList *l = g_app->clip_files; l; l = l->next) {
        char *uri = g_file_get_uri(G_FILE(l->data));
        if (!uri) continue;
        g_string_append(uris, uri);
        g_string_append(uris, "\r\n");
        g_string_append(gnome, uri);
        g_string_append_c(gnome, '\n');
        g_free(uri);
    }

    GdkAtom target = gtk_selection_data_get_target(sel);
    if (target == gdk_atom_intern_static_string("x-special/gnome-copied-files")) {
        gtk_selection_data_set(sel, target, 8,
                               (const guchar *)gnome->str,
                               (gint)gnome->len);
    } else {
        gtk_selection_data_set(sel, target, 8,
                               (const guchar *)uris->str,
                               (gint)uris->len);
    }
    g_string_free(uris, TRUE);
    g_string_free(gnome, TRUE);
}

static void
clipboard_clear_cb(GtkClipboard *clipboard, gpointer data)
{
    (void)clipboard;
    (void)data;
    /* The list is freed when the next copy/cut starts or at shutdown. */
}

static void
app_copy_cut(Canvas *c, gboolean cut)
{
    GList *paths = canvas_selected_paths(c);
    if (!paths)
        return;

    g_list_free_full(g_app->clip_files, g_object_unref);
    g_app->clip_files = NULL;
    for (GList *l = paths; l; l = l->next) {
        g_app->clip_files = g_list_prepend(
            g_app->clip_files, g_file_new_for_path((const char *)l->data));
    }
    g_app->clip_files = g_list_reverse(g_app->clip_files);
    g_app->clip_cut = cut;
    g_list_free_full(paths, g_free);

    GtkTargetEntry targets[] = {
        { (gchar *)"text/uri-list", 0, 0 },
        { (gchar *)"x-special/gnome-copied-files", 0, 1 },
    };
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_with_data(cb, targets, G_N_ELEMENTS(targets),
                                clipboard_get_cb, clipboard_clear_cb, NULL);
}

static GList *
clipboard_read_files(gboolean *cut)
{
    *cut = FALSE;
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gchar **uris = gtk_clipboard_wait_for_uris(cb);
    if (!uris)
        return NULL;

    GList *files = NULL;
    for (int i = 0; uris[i]; i++) {
        GFile *f = g_file_new_for_uri(uris[i]);
        if (f)
            files = g_list_prepend(files, f);
    }
    files = g_list_reverse(files);
    g_strfreev(uris);

    GtkSelectionData *sd = gtk_clipboard_wait_for_contents(
        cb, gdk_atom_intern_static_string("x-special/gnome-copied-files"));
    if (sd) {
        const guchar *d = gtk_selection_data_get_data(sd);
        if (d && g_str_has_prefix((const char *)d, "cut"))
            *cut = TRUE;
        gtk_selection_data_free(sd);
    }
    return files;
}

static void
app_paste(Canvas *c, const char *dest_dir)
{
    if (!dest_dir)
        dest_dir = g_app->desktop_dir;

    gboolean cut = FALSE;
    GList *files = clipboard_read_files(&cut);
    if (!files) {
        app_show_error(c, "Paste", "The clipboard does not contain any files.");
        return;
    }

    GList *new_paths = NULL;
    GError *first_error = NULL;
    for (GList *l = files; l; l = l->next) {
        GFile *src = G_FILE(l->data);
        char *base = g_file_get_basename(src);
        if (!base || !base[0]) {
            g_free(base);
            continue;
        }
        char *dest_path = unique_destination(dest_dir, base);
        GFile *dest = g_file_new_for_path(dest_path);
        GError *err = NULL;
        gboolean ok = cut ? move_file(src, dest, &err)
                          : copy_recursive(src, dest, &err);
        if (ok)
            new_paths = g_list_prepend(new_paths, g_strdup(dest_path));
        else if (!first_error && err)
            first_error = g_error_copy(err);
        g_clear_error(&err);
        g_object_unref(dest);
        g_free(dest_path);
        g_free(base);
    }
    g_list_free_full(files, g_object_unref);
    new_paths = g_list_reverse(new_paths);

    if (cut)
        gtk_clipboard_clear(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));

    app_reload_all();
    canvas_select_paths(c, new_paths);
    g_list_free_full(new_paths, g_free);

    if (first_error) {
        app_show_error(c, "Paste", first_error->message);
        g_error_free(first_error);
    }
}

/* ---- file actions -------------------------------------------------------- */

static void
app_open_icon(Canvas *c, Icon *ic)
{
    GError *err = NULL;
    if (ic->is_desktop && ic->app_info) {
        if (!g_app_info_launch(G_APP_INFO(ic->app_info), NULL, NULL, &err)) {
            app_show_error(c, "Open", err ? err->message : "Could not launch application.");
            g_clear_error(&err);
        }
        return;
    }
    GFile *f = g_file_new_for_path(ic->path);
    char *uri = g_file_get_uri(f);
    if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
        app_show_error(c, "Open", err ? err->message : "Could not open file.");
        g_clear_error(&err);
    }
    g_free(uri);
    g_object_unref(f);
}

static void
app_open_folder(Canvas *c)
{
    GError *err = NULL;
    char *uri = g_filename_to_uri(g_app->desktop_dir, NULL, NULL);
    if (!uri || !g_app_info_launch_default_for_uri(uri, NULL, &err)) {
        app_show_error(c, "Open Desktop Folder",
                       err ? err->message : "Could not open the desktop folder.");
        g_clear_error(&err);
    }
    g_free(uri);
}

static void
app_new_folder(Canvas *c)
{
    char *path = unique_destination(g_app->desktop_dir, "New Folder");
    GFile *f = g_file_new_for_path(path);
    GError *err = NULL;
    if (!g_file_make_directory(f, NULL, &err)) {
        app_show_error(c, "New Folder",
                       err ? err->message : "Could not create folder.");
        g_clear_error(&err);
    } else {
        char *key = g_path_get_basename(path);
        app_reload_all();
        canvas_select_key(c, key);
        g_free(key);
    }
    g_object_unref(f);
    g_free(path);
}

static void
app_delete_selected(Canvas *c, gboolean permanent)
{
    int n = canvas_selected_count(c);
    if (n == 0)
        return;

    if (permanent) {
        char *msg = g_strdup_printf(
            "Permanently delete %d item%s? This cannot be undone.",
            n, n == 1 ? "" : "s");
        gboolean ok = app_confirm(c, "Delete Permanently", msg);
        g_free(msg);
        if (!ok)
            return;
    }

    GList *paths = canvas_selected_paths(c);
    GError *first_error = NULL;
    for (GList *l = paths; l; l = l->next) {
        GFile *f = g_file_new_for_path((const char *)l->data);
        GError *err = NULL;
        gboolean ok = permanent ? delete_recursive(f, &err)
                                : g_file_trash(f, NULL, &err);
        if (!ok && !first_error && err)
            first_error = g_error_copy(err);
        g_clear_error(&err);
        g_object_unref(f);
    }
    g_list_free_full(paths, g_free);

    app_clear_selection(c);
    app_reload_all();
    app_save_soon();

    if (first_error) {
        app_show_error(c, permanent ? "Delete Permanently" : "Move to Trash",
                       first_error->message);
        g_error_free(first_error);
    }
}

static void
app_show_error(Canvas *c, const char *title, const char *message)
{
    GtkWidget *d = gtk_message_dialog_new(
        NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
        "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", message);
    if (g_app->windowed && c && c->window)
        gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(c->window));
    gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER_ALWAYS);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static gboolean
app_confirm(Canvas *c, const char *title, const char *message)
{
    GtkWidget *d = gtk_message_dialog_new(
        NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", message);
    if (g_app->windowed && c && c->window)
        gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(c->window));
    gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER_ALWAYS);
    gint r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return r == GTK_RESPONSE_YES;
}

/* ---- rename -------------------------------------------------------------- */

static void
app_rename_selected(Canvas *c)
{
    int n = canvas_selected_count(c);
    if (n != 1)
        return;

    Icon *ic = NULL;
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *cur = g_ptr_array_index(c->icons, i);
        if (cur->selected) {
            ic = cur;
            break;
        }
    }
    if (!ic)
        return;

    char *old_key = g_strdup(ic->key);
    char *old_path = g_strdup(ic->path);

    GtkWidget *d = gtk_dialog_new_with_buttons(
        "Rename", NULL, GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Rename", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER_ALWAYS);
    gtk_window_set_default_size(GTK_WINDOW(d), 360, -1);
    if (g_app->windowed && c->window)
        gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(c->window));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))),
                       box, TRUE, TRUE, 0);

    GtkWidget *label = gtk_label_new("New name:");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), ic->key);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);

    gtk_widget_show_all(d);
    gtk_widget_grab_focus(entry);
    gint r = gtk_dialog_run(GTK_DIALOG(d));
    char *new_name = NULL;
    if (r == GTK_RESPONSE_ACCEPT)
        new_name = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
    gtk_widget_destroy(d);

    if (!new_name || !new_name[0] || g_strcmp0(new_name, old_key) == 0) {
        g_free(new_name);
        g_free(old_key);
        g_free(old_path);
        return;
    }
    if (strchr(new_name, '/')) {
        app_show_error(c, "Rename", "A file name cannot contain '/'.");
        g_free(new_name);
        g_free(old_key);
        g_free(old_path);
        return;
    }

    char *new_path = g_build_filename(g_app->desktop_dir, new_name, NULL);
    if (g_file_test(new_path, G_FILE_TEST_EXISTS)) {
        app_show_error(c, "Rename",
                       "A file with that name already exists on the desktop.");
        g_free(new_path);
        g_free(new_name);
        g_free(old_key);
        g_free(old_path);
        return;
    }

    GFile *src = g_file_new_for_path(old_path);
    GFile *dest = g_file_new_for_path(new_path);
    GError *err = NULL;
    if (!g_file_move(src, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, &err)) {
        app_show_error(c, "Rename", err ? err->message : "Could not rename the file.");
        g_clear_error(&err);
    } else {
        int mx = (!ic->auto_placed && ic->want_x >= 0) ? ic->want_x : ic->x;
        int my = (!ic->auto_placed && ic->want_y >= 0) ? ic->want_y : ic->y;
        state_move_position(c, old_key, new_name, mx, my, ic->auto_placed);
        app_reload_all();
        canvas_select_key(c, new_name);
        app_save_soon();
    }
    g_object_unref(src);
    g_object_unref(dest);
    g_free(new_path);
    g_free(new_name);
    g_free(old_key);
    g_free(old_path);
}

/* ---- properties ---------------------------------------------------------- */

static char *
format_size(goffset bytes)
{
    if (bytes < 0)
        return g_strdup("unknown");
    const char *units[] = { "bytes", "kB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1000.0 && u < 4) {
        v /= 1000.0;
        u++;
    }
    if (u == 0)
        return g_strdup_printf("%" G_GINT64_FORMAT " bytes", (gint64)bytes);
    return g_strdup_printf("%.1f %s", v, units[u]);
}

static void
app_show_properties(Canvas *c)
{
    int n = canvas_selected_count(c);
    if (n != 1)
        return;

    Icon *ic = NULL;
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *cur = g_ptr_array_index(c->icons, i);
        if (cur->selected) {
            ic = cur;
            break;
        }
    }
    if (!ic)
        return;

    GtkWidget *d = gtk_dialog_new_with_buttons(
        "Properties", NULL, GTK_DIALOG_MODAL,
        "_Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER_ALWAYS);
    gtk_window_set_default_size(GTK_WINDOW(d), 420, -1);
    if (g_app->windowed && c->window)
        gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(c->window));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))),
                       box, TRUE, TRUE, 0);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);

    GtkWidget *img = gtk_image_new_from_pixbuf(ic->pixbuf);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 48);
    gtk_box_pack_start(GTK_BOX(head), img, FALSE, FALSE, 0);

    GtkWidget *title = gtk_label_new(NULL);
    char *markup = g_markup_printf_escaped("<b>%s</b>", ic->name);
    gtk_label_set_markup(GTK_LABEL(title), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(head), title, TRUE, TRUE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_box_pack_start(GTK_BOX(box), grid, TRUE, TRUE, 0);

    int row = 0;
    const char *type;
    if (ic->is_dir)
        type = "Folder";
    else if (ic->is_desktop)
        type = "Application launcher";
    else if (ic->content_type && ic->content_type[0])
        type = ic->content_type;
    else
        type = "File";
    char *size_str = ic->is_dir ? NULL : format_size(ic->size);
    char *mtime_str = ic->mtime
        ? g_date_time_format(ic->mtime, "%Y-%m-%d %H:%M:%S") : NULL;

    struct { const char *label; const char *value; } rows[] = {
        { "Name", ic->key },
        { "Type", type },
        { "Size", size_str },
        { "Modified", mtime_str },
        { "Location", g_app->desktop_dir },
        { "Full path", ic->path },
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        if (!rows[i].value)
            continue;
        GtkWidget *l = gtk_label_new(rows[i].label);
        gtk_widget_set_halign(l, GTK_ALIGN_END);
        gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
        GtkWidget *v = gtk_label_new(rows[i].value);
        gtk_label_set_xalign(GTK_LABEL(v), 0.0F);
        gtk_label_set_selectable(GTK_LABEL(v), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_MIDDLE);
        gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
        row++;
    }

    g_free(size_str);
    g_free(mtime_str);

    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

/* ---- new launcher -------------------------------------------------------- */

typedef struct {
    GtkTreeModelFilter *filter;
    char *needle;
} LauncherFilter;

static gboolean
launcher_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    LauncherFilter *lf = data;
    if (!lf->needle || !lf->needle[0])
        return TRUE;
    char *name = NULL;
    char *id = NULL;
    gtk_tree_model_get(model, iter, 1, &id, 2, &name, -1);
    char *lower = name ? g_utf8_strdown(name, -1) : NULL;
    gboolean visible =
        (lower && strstr(lower, lf->needle)) ||
        (id && strstr(id, lf->needle));
    g_free(lower);
    g_free(name);
    g_free(id);
    return visible;
}

static void
launcher_search_changed(GtkEditable *editable, gpointer data)
{
    LauncherFilter *lf = data;
    g_free(lf->needle);
    char *text = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(editable)), -1);
    lf->needle = text;
    gtk_tree_model_filter_refilter(lf->filter);
}

static void
app_new_launcher(Canvas *c)
{
    GList *apps = g_app_info_get_all();
    GList *desktop_apps = NULL;
    for (GList *l = apps; l; l = l->next) {
        GAppInfo *ai = G_APP_INFO(l->data);
        if (!g_app_info_should_show(ai))
            continue;
        const char *id = g_app_info_get_id(ai);
        if (!id || !g_str_has_suffix(id, ".desktop"))
            continue;
        desktop_apps = g_list_prepend(desktop_apps, ai);
    }
    desktop_apps = g_list_reverse(desktop_apps);
    g_list_free(apps);

    if (!desktop_apps) {
        app_show_error(c, "New Launcher", "No applications were found.");
        return;
    }

    GtkWidget *d = gtk_dialog_new_with_buttons(
        "New Launcher", NULL, GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Add to Desktop", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER_ALWAYS);
    gtk_window_set_default_size(GTK_WINDOW(d), 520, 460);
    if (g_app->windowed && c->window)
        gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(c->window));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))),
                       box, TRUE, TRUE, 0);

    GtkWidget *search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Search applications…");
    gtk_box_pack_start(GTK_BOX(box), search, FALSE, FALSE, 0);

    GtkListStore *store = gtk_list_store_new(3, G_TYPE_ICON, G_TYPE_STRING,
                                             G_TYPE_STRING);
    for (GList *l = desktop_apps; l; l = l->next) {
        GAppInfo *ai = G_APP_INFO(l->data);
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
                           0, g_app_info_get_icon(ai),
                           1, g_app_info_get_id(ai),
                           2, g_app_info_get_name(ai), -1);
    }

    LauncherFilter lf = { NULL, NULL };
    lf.filter = GTK_TREE_MODEL_FILTER(
        gtk_tree_model_filter_new(GTK_TREE_MODEL(store), NULL));
    gtk_tree_model_filter_set_visible_func(lf.filter, launcher_visible, &lf, NULL);

    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(lf.filter));
    g_object_unref(lf.filter);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(view), FALSE);

    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    g_object_set(pix, "width", 32, "height", 32, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "Icon", pix, "gicon", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);

    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    col = gtk_tree_view_column_new_with_attributes(
        "Application", text, "text", 2, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    char *chosen_id = NULL;

    g_signal_connect(search, "search-changed",
                     G_CALLBACK(launcher_search_changed), &lf);
    g_signal_connect_swapped(view, "row-activated",
                             G_CALLBACK(gtk_dialog_response), d);

    gtk_widget_show_all(d);
    gtk_widget_grab_focus(search);

    gint r = gtk_dialog_run(GTK_DIALOG(d));

    if (r == GTK_RESPONSE_ACCEPT) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
        GtkTreeModel *model;
        GtkTreeIter iter;
        if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
            gtk_tree_model_get(model, &iter, 1, &chosen_id, -1);
        }
    }

    gtk_widget_destroy(d);
    g_free(lf.needle);
    g_object_unref(store);
    g_list_free(desktop_apps);

    if (!chosen_id || !chosen_id[0]) {
        g_free(chosen_id);
        return;
    }

    /* Find the source .desktop file in the XDG data dirs and copy it. */
    char *source = NULL;
    const char *dirs = g_getenv("XDG_DATA_DIRS");
    char *default_dirs = g_strdup("~/.local/share:/usr/local/share:/usr/share");
    const char *search_dirs = (dirs && dirs[0]) ? dirs : default_dirs;
    char **parts = g_strsplit(search_dirs, ":", -1);
    for (int i = 0; parts[i] && !source; i++) {
        char *base = expand_home(parts[i]);
        char *cand = g_build_filename(base, "applications", chosen_id, NULL);
        if (g_file_test(cand, G_FILE_TEST_EXISTS))
            source = cand;
        else
            g_free(cand);
        g_free(base);
    }
    g_strfreev(parts);
    g_free(default_dirs);

    if (!source) {
        char *user = g_build_filename(g_get_user_data_dir(), "applications",
                                      chosen_id, NULL);
        if (g_file_test(user, G_FILE_TEST_EXISTS))
            source = user;
        else
            g_free(user);
    }

    if (!source) {
        app_show_error(c, "New Launcher",
                       "Could not find the application's .desktop file.");
        g_free(chosen_id);
        return;
    }

    char *dest = unique_destination(g_app->desktop_dir, chosen_id);
    GFile *src = g_file_new_for_path(source);
    GFile *dst = g_file_new_for_path(dest);
    GError *err = NULL;
    if (!g_file_copy(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &err)) {
        app_show_error(c, "New Launcher",
                       err ? err->message : "Could not copy the launcher.");
        g_clear_error(&err);
    } else {
        /* Make sure the copy is executable and trusted enough to launch. */
        g_file_set_attribute_uint32(dst, G_FILE_ATTRIBUTE_UNIX_MODE, 0755,
                                    G_FILE_QUERY_INFO_NONE, NULL, NULL);
        char *key = g_path_get_basename(dest);
        app_reload_all();
        canvas_select_key(c, key);
        g_free(key);
        app_save_soon();
    }
    g_object_unref(src);
    g_object_unref(dst);
    g_free(dest);
    g_free(source);
    g_free(chosen_id);
}

/* ---- context menus ------------------------------------------------------- */

enum {
    ACT_OPEN,
    ACT_CUT,
    ACT_COPY,
    ACT_PASTE,
    ACT_RENAME,
    ACT_TRASH,
    ACT_DELETE,
    ACT_PROPERTIES,
    ACT_NEW_FOLDER,
    ACT_NEW_LAUNCHER,
    ACT_ARRANGE,
    ACT_SNAP,
    ACT_TOGGLE_ICONS,
    ACT_REFRESH,
    ACT_OPEN_FOLDER
};

static GtkWidget *
menu_item_new(const char *label, GCallback cb, gpointer data)
{
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    if (cb)
        g_signal_connect(item, "activate", cb, data);
    return item;
}

static GtkWidget *
menu_separator(void)
{
    return gtk_separator_menu_item_new();
}

static void
menu_activate(GtkMenuItem *item, gpointer data)
{
    (void)item;
    Canvas *c = g_menu_canvas;
    if (!c)
        return;
    switch (GPOINTER_TO_INT(data)) {
    case ACT_OPEN:
        if (g_menu_target) {
            for (guint i = 0; i < c->icons->len; i++) {
                Icon *ic = g_ptr_array_index(c->icons, i);
                if (ic->selected) {
                    app_open_icon(c, ic);
                    break;
                }
            }
        }
        break;
    case ACT_CUT:
        app_copy_cut(c, TRUE);
        break;
    case ACT_COPY:
        app_copy_cut(c, FALSE);
        break;
    case ACT_PASTE:
        app_paste(c, g_app->desktop_dir);
        break;
    case ACT_RENAME:
        app_rename_selected(c);
        break;
    case ACT_TRASH:
        app_delete_selected(c, FALSE);
        break;
    case ACT_DELETE:
        app_delete_selected(c, TRUE);
        break;
    case ACT_PROPERTIES:
        app_show_properties(c);
        break;
    case ACT_NEW_FOLDER:
        app_new_folder(c);
        break;
    case ACT_NEW_LAUNCHER:
        app_new_launcher(c);
        break;
    case ACT_ARRANGE:
        app_arrange_icons();
        break;
    case ACT_SNAP:
        app_toggle_snap();
        break;
    case ACT_TOGGLE_ICONS:
        app_toggle_show_icons();
        break;
    case ACT_REFRESH:
        app_reload_all();
        break;
    case ACT_OPEN_FOLDER:
        app_open_folder(c);
        break;
    default:
        break;
    }
}

static void
menu_size_toggled(GtkCheckMenuItem *item, gpointer data)
{
    /* Deactivating a radio item also emits "activate"/"toggled" in GTK3, so
     * only react when this item is the one being turned on. */
    if (!gtk_check_menu_item_get_active(item))
        return;
    app_set_icon_size(GPOINTER_TO_INT(data));
}

static gint
int_cmp(gconstpointer a, gconstpointer b)
{
    return *(const int *)a - *(const int *)b;
}

static GtkWidget *
build_size_menu(void)
{
    static const int defaults[] = { 32, 48, 64, 96, 128 };
    int sizes[G_N_ELEMENTS(defaults) + 1];
    int n = 0;
    gboolean found = FALSE;

    for (guint i = 0; i < G_N_ELEMENTS(defaults); i++) {
        sizes[n++] = defaults[i];
        if (defaults[i] == g_app->icon_size)
            found = TRUE;
    }
    if (!found && g_app->icon_size >= MIN_ICON_SIZE &&
        g_app->icon_size <= MAX_ICON_SIZE)
        sizes[n++] = g_app->icon_size;
    qsort(sizes, (size_t)n, sizeof(int), int_cmp);

    GtkWidget *menu = gtk_menu_new();
    GSList *group = NULL;
    for (int i = 0; i < n; i++) {
        char *label = g_strdup_printf("%d px", sizes[i]);
        GtkWidget *item = gtk_radio_menu_item_new_with_label(group, label);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        if (g_app->icon_size == sizes[i])
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
        g_signal_connect(item, "toggled", G_CALLBACK(menu_size_toggled),
                         GINT_TO_POINTER(sizes[i]));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        g_free(label);
    }
    return menu;
}

static gboolean
clipboard_has_files(void)
{
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    return gtk_clipboard_wait_is_uris_available(cb);
}

static void
menu_popup(GtkWidget *menu, GdkEvent *event)
{
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
}

static void
menu_open_icon(Canvas *c, Icon *ic, GdkEvent *event)
{
    if (!ic->selected) {
        app_clear_selection(c);
        ic->selected = TRUE;
        canvas_queue_draw(c);
    }

    g_menu_canvas = c;
    g_free(g_menu_target);
    g_menu_target = g_strdup(ic->path);
    g_menu_multi = canvas_selected_count(c) > 1;

    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item;

    if (!g_menu_multi) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
            menu_item_new("_Open", G_CALLBACK(menu_activate),
                          GINT_TO_POINTER(ACT_OPEN)));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_separator());
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        menu_item_new("Cu_t", G_CALLBACK(menu_activate), GINT_TO_POINTER(ACT_CUT)));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        menu_item_new("_Copy", G_CALLBACK(menu_activate), GINT_TO_POINTER(ACT_COPY)));
    item = menu_item_new("_Paste", G_CALLBACK(menu_activate),
                         GINT_TO_POINTER(ACT_PASTE));
    gtk_widget_set_sensitive(item, clipboard_has_files());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_separator());

    if (!g_menu_multi) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
            menu_item_new("_Rename…", G_CALLBACK(menu_activate),
                          GINT_TO_POINTER(ACT_RENAME)));
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        menu_item_new("Move to _Trash", G_CALLBACK(menu_activate),
                      GINT_TO_POINTER(ACT_TRASH)));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        menu_item_new("Delete _Permanently…", G_CALLBACK(menu_activate),
                      GINT_TO_POINTER(ACT_DELETE)));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_separator());

    if (!g_menu_multi) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
            menu_item_new("P_roperties", G_CALLBACK(menu_activate),
                          GINT_TO_POINTER(ACT_PROPERTIES)));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_separator());
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        menu_item_new("New _Folder", G_CALLBACK(menu_activate),
                      GINT_TO_POINTER(ACT_NEW_FOLDER)));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        menu_item_new("New _Launcher…", G_CALLBACK(menu_activate),
                      GINT_TO_POINTER(ACT_NEW_LAUNCHER)));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_separator());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        menu_item_new("_Arrange Icons", G_CALLBACK(menu_activate),
                      GINT_TO_POINTER(ACT_ARRANGE)));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        menu_item_new("_Refresh", G_CALLBACK(menu_activate),
                      GINT_TO_POINTER(ACT_REFRESH)));

    menu_popup(menu, event);
}

static void
menu_open_desktop(Canvas *c, GdkEvent *event)
{
    app_clear_selection(c);
    g_menu_canvas = c;
    g_free(g_menu_target);
    g_menu_target = NULL;
    g_menu_multi = FALSE;

    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item;

    item = menu_item_new("_Paste", G_CALLBACK(menu_activate),
                         GINT_TO_POINTER(ACT_PASTE));
    gtk_widget_set_sensitive(item, clipboard_has_files());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_separator());

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        menu_item_new("New _Folder", G_CALLBACK(menu_activate),
                      GINT_TO_POINTER(ACT_NEW_FOLDER)));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        menu_item_new("New _Launcher…", G_CALLBACK(menu_activate),
                      GINT_TO_POINTER(ACT_NEW_LAUNCHER)));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_separator());

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        menu_item_new("_Arrange Icons", G_CALLBACK(menu_activate),
                      GINT_TO_POINTER(ACT_ARRANGE)));

    item = gtk_check_menu_item_new_with_mnemonic("_Snap to Grid");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), g_app->snap);
    g_signal_connect(item, "activate", G_CALLBACK(menu_activate),
                     GINT_TO_POINTER(ACT_SNAP));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    GtkWidget *size_item = gtk_menu_item_new_with_mnemonic("Icon _Size");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(size_item), build_size_menu());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), size_item);

    item = gtk_check_menu_item_new_with_mnemonic("Show Desktop _Icons");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item),
                                   g_app->show_icons);
    g_signal_connect(item, "activate", G_CALLBACK(menu_activate),
                     GINT_TO_POINTER(ACT_TOGGLE_ICONS));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_separator());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        menu_item_new("_Refresh", G_CALLBACK(menu_activate),
                      GINT_TO_POINTER(ACT_REFRESH)));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        menu_item_new("Open Desktop _Folder", G_CALLBACK(menu_activate),
                      GINT_TO_POINTER(ACT_OPEN_FOLDER)));

    menu_popup(menu, event);
}

/* ---- canvas events ------------------------------------------------------- */

static void
canvas_set_cursor(Canvas *c, GdkCursor *cursor)
{
    if (c->area && gtk_widget_get_window(c->area))
        gdk_window_set_cursor(gtk_widget_get_window(c->area), cursor);
}

static void
app_set_icon_size(int size)
{
    if (size < MIN_ICON_SIZE) size = MIN_ICON_SIZE;
    if (size > MAX_ICON_SIZE) size = MAX_ICON_SIZE;
    if (size == g_app->icon_size)
        return;
    g_app->icon_size = size;
    for (guint ci = 0; ci < g_app->canvases->len; ci++) {
        Canvas *c = g_ptr_array_index(g_app->canvases, ci);
        for (guint i = 0; i < c->icons->len; i++)
            canvas_load_icon(g_ptr_array_index(c->icons, i));
        canvas_relayout(c, TRUE);
        canvas_queue_draw(c);
    }
    app_save_soon();
}

static void
app_toggle_snap(void)
{
    g_app->snap = !g_app->snap;
    if (g_app->snap) {
        for (guint ci = 0; ci < g_app->canvases->len; ci++) {
            Canvas *c = g_ptr_array_index(g_app->canvases, ci);
            canvas_relayout(c, TRUE);
            canvas_queue_draw(c);
        }
    }
    app_save_soon();
}

static void
app_toggle_show_icons(void)
{
    g_app->show_icons = !g_app->show_icons;
    for (guint ci = 0; ci < g_app->canvases->len; ci++) {
        Canvas *c = g_ptr_array_index(g_app->canvases, ci);
        canvas_queue_draw(c);
    }
    app_save_soon();
}

static void
canvas_commit_drag(Canvas *c)
{
    if (!c->drag_moved) {
        c->dragging = FALSE;
        return;
    }
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        if (!ic->selected)
            continue;
        ic->x += (int)lround(c->drag_dx);
        ic->y += (int)lround(c->drag_dy);
        ic->auto_placed = FALSE;
        canvas_snap_xy(&ic->x, &ic->y);
        ic->want_x = ic->x;
        ic->want_y = ic->y;
        canvas_clamp_icon(c, ic);
    }
    c->dragging = FALSE;
    c->drag_moved = FALSE;
    c->drag_dx = c->drag_dy = 0;
    canvas_queue_draw(c);
    app_save_soon();
}

static void
canvas_select_rect(Canvas *c)
{
    double x0 = MIN(c->sel_x0, c->sel_x1);
    double y0 = MIN(c->sel_y0, c->sel_y1);
    double x1 = MAX(c->sel_x0, c->sel_x1);
    double y1 = MAX(c->sel_y0, c->sel_y1);
    int cw = canvas_cell_w();
    int ch = canvas_cell_h();
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        double ix0 = ic->x;
        double iy0 = ic->y;
        double ix1 = ic->x + cw;
        double iy1 = ic->y + ch;
        ic->selected = !(ix1 < x0 || ix0 > x1 || iy1 < y0 || iy0 > y1);
    }
}

static gboolean
on_button_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    Canvas *c = data;
    (void)widget;
    gtk_widget_grab_focus(c->area);

    double x;
    double y;
    gdk_event_get_coords((GdkEvent *)event, &x, &y);
    Icon *ic = g_app->show_icons ? canvas_hit_test(c, x, y) : NULL;
    gboolean ctrl = (event->state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (event->state & GDK_SHIFT_MASK) != 0;

    if (event->button == 3) {
        if (ic)
            menu_open_icon(c, ic, (GdkEvent *)event);
        else
            menu_open_desktop(c, (GdkEvent *)event);
        return TRUE;
    }

    if (event->button != 1)
        return FALSE;

    if (event->type == GDK_2BUTTON_PRESS) {
        if (ic)
            app_open_icon(c, ic);
        return TRUE;
    }

    if (ic) {
        if (ctrl) {
            ic->selected = !ic->selected;
        } else if (shift) {
            ic->selected = TRUE;
        } else if (!ic->selected) {
            app_clear_selection(c);
            ic->selected = TRUE;
        }
        c->pressed = ic;
        c->press_x = x;
        c->press_y = y;
        c->dragging = TRUE;
        c->drag_moved = FALSE;
        c->drag_dx = c->drag_dy = 0;
        canvas_queue_draw(c);
    } else {
        if (!ctrl && !shift)
            app_clear_selection(c);
        c->pressed = NULL;
        c->selecting = TRUE;
        c->sel_x0 = c->sel_x1 = x;
        c->sel_y0 = c->sel_y1 = y;
    }
    return TRUE;
}

static gboolean
on_button_release(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    Canvas *c = data;
    (void)widget;
    if (event->button != 1)
        return FALSE;

    /* GTK owns the pointer while a drag is in flight.  drag-end clears the
     * press state, and a self-drop needs c->pressed to still be valid. */
    if (c->dnd_active)
        return TRUE;

    if (c->selecting) {
        c->selecting = FALSE;
        canvas_select_rect(c);
        canvas_queue_draw(c);
    }
    if (c->dragging)
        canvas_commit_drag(c);
    c->pressed = NULL;
    return TRUE;
}

static gboolean
on_motion(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
    Canvas *c = data;
    (void)widget;
    double x;
    double y;
    gdk_event_get_coords((GdkEvent *)event, &x, &y);

    if (c->selecting) {
        c->sel_x1 = x;
        c->sel_y1 = y;
        canvas_queue_draw(c);
        return TRUE;
    }

    if (c->dnd_active)
        return TRUE;

    if (c->dragging) {
        double dx = x - c->press_x;
        double dy = y - c->press_y;
        if (!c->drag_moved &&
            gtk_drag_check_threshold(c->area, (int)c->press_x,
                                     (int)c->press_y, (int)x, (int)y)) {
            /* Hand the drag to GTK so it can be dropped on other apps.  If
             * that is not possible, fall back to moving the icon locally. */
            if (canvas_begin_dnd(c, (GdkEvent *)event, x, y))
                return TRUE;
            c->drag_moved = TRUE;
        }
        if (c->drag_moved) {
            c->drag_dx = dx;
            c->drag_dy = dy;
            canvas_queue_draw(c);
        }
        return TRUE;
    }

    Icon *hit = g_app->show_icons ? canvas_hit_test(c, x, y) : NULL;
    if (hit != c->hover) {
        if (c->hover) c->hover->hover = FALSE;
        c->hover = hit;
        if (c->hover) c->hover->hover = TRUE;
        canvas_queue_draw(c);
    }
    canvas_set_cursor(c, c->cursor_default);
    return TRUE;
}

static gboolean
on_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer data)
{
    (void)widget;
    (void)event;
    Canvas *c = data;
    if (c->hover) {
        c->hover->hover = FALSE;
        c->hover = NULL;
        canvas_queue_draw(c);
    }
    return FALSE;
}

static gboolean
on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    Canvas *c = data;
    (void)widget;
    gboolean ctrl = (event->state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (event->state & GDK_SHIFT_MASK) != 0;
    int sel = canvas_selected_count(c);

    switch (event->keyval) {
    case GDK_KEY_Escape:
        app_clear_selection(c);
        return TRUE;
    case GDK_KEY_a:
    case GDK_KEY_A:
        if (ctrl) {
            app_select_all(c);
            return TRUE;
        }
        break;
    case GDK_KEY_c:
    case GDK_KEY_C:
        if (ctrl) {
            app_copy_cut(c, FALSE);
            return TRUE;
        }
        break;
    case GDK_KEY_x:
    case GDK_KEY_X:
        if (ctrl) {
            app_copy_cut(c, TRUE);
            return TRUE;
        }
        break;
    case GDK_KEY_v:
    case GDK_KEY_V:
        if (ctrl) {
            app_paste(c, g_app->desktop_dir);
            return TRUE;
        }
        break;
    case GDK_KEY_h:
    case GDK_KEY_H:
        if (ctrl) {
            app_toggle_show_icons();
            return TRUE;
        }
        break;
    case GDK_KEY_r:
    case GDK_KEY_R:
        if (ctrl) {
            app_reload_all();
            return TRUE;
        }
        break;
    case GDK_KEY_F2:
        if (sel == 1) {
            app_rename_selected(c);
            return TRUE;
        }
        break;
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
        if (sel > 0) {
            app_delete_selected(c, shift);
            return TRUE;
        }
        break;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        if (sel == 1) {
            for (guint i = 0; i < c->icons->len; i++) {
                Icon *ic = g_ptr_array_index(c->icons, i);
                if (ic->selected) {
                    app_open_icon(c, ic);
                    break;
                }
            }
            return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

static void
on_size_allocate(GtkWidget *widget, GtkAllocation *alloc, gpointer data)
{
    (void)widget;
    Canvas *c = data;
    if (c->width == alloc->width && c->height == alloc->height)
        return;
    c->width = alloc->width;
    c->height = alloc->height;
    canvas_relayout(c, TRUE);
    canvas_queue_draw(c);
}

/* ---- dragging icons out to other applications ---------------------------- */

static Canvas *
canvas_from_area(GtkWidget *area)
{
    if (!area || !g_app || !g_app->canvases)
        return NULL;
    for (guint i = 0; i < g_app->canvases->len; i++) {
        Canvas *c = g_ptr_array_index(g_app->canvases, i);
        if (c->area == area)
            return c;
    }
    return NULL;
}

/* The folder icon under the pointer, if any.  Selected icons are skipped so
 * dropping onto the icon you are dragging (or onto another item in a
 * multi-selection) is a reposition, not a move into a folder. */
static Icon *
canvas_folder_at(Canvas *c, double x, double y)
{
    int cw = canvas_cell_w();
    int ch = canvas_cell_h();
    for (int i = (int)c->icons->len - 1; i >= 0; i--) {
        Icon *ic = g_ptr_array_index(c->icons, (guint)i);
        if (ic->selected)
            continue;
        if (x >= ic->x && x < ic->x + cw &&
            y >= ic->y && y < ic->y + ch &&
            g_file_test(ic->path, G_FILE_TEST_IS_DIR))
            return ic;
    }
    return NULL;
}

/* TRUE when `inner` is `outer` or lives underneath it.  realpath() resolves
 * symlinks, which a plain string prefix test cannot, so a folder reached
 * through a symlink cannot masquerade as an unrelated destination. */
static gboolean
path_is_within(const char *inner, const char *outer)
{
    char *in = realpath(inner, NULL);
    char *out = realpath(outer, NULL);
    gboolean within = FALSE;
    if (in && out) {
        size_t n = strlen(out);
        within = strcmp(in, out) == 0 ||
                 (strncmp(in, out, n) == 0 && in[n] == G_DIR_SEPARATOR);
    }
    free(in);
    free(out);
    return within;
}

static gboolean
show_move_error_idle(gpointer data)
{
    char *message = data;
    app_show_error(NULL, "Move", message);
    g_free(message);
    return G_SOURCE_REMOVE;
}

/* Move every selected icon into the folder at (x, y).  Returns TRUE when a
 * folder was under the pointer and the drop was handled, so the caller does
 * not also reposition the icons on top of it. */
static gboolean
canvas_drop_into_folder(Canvas *c, double x, double y)
{
    Icon *folder = canvas_folder_at(c, x, y);
    if (!folder)
        return FALSE;

    const char *dest_dir = folder->path;
    gboolean handled = FALSE;
    GError *first_error = NULL;

    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        if (!ic->selected)
            continue;

        /* A folder cannot be moved into itself or into one of its own
         * descendants; leave those items where they are. */
        if (path_is_within(dest_dir, ic->path))
            continue;

        handled = TRUE;
        char *base = g_path_get_basename(ic->path);
        char *dest = unique_destination(dest_dir, base);
        GFile *src = g_file_new_for_path(ic->path);
        GFile *dst = g_file_new_for_path(dest);
        GError *err = NULL;
        if (!move_file(src, dst, &err) && err && !first_error)
            first_error = g_error_copy(err);
        g_clear_error(&err);
        g_object_unref(src);
        g_object_unref(dst);
        g_free(dest);
        g_free(base);
    }

    if (!handled)
        return FALSE;

    /* The moved entries are gone from the desktop directory now, so rebuild
     * the icon list instead of leaving stale icons behind. */
    app_reload_all();
    if (first_error) {
        /* Don't open a modal dialog from inside the drop callback; the drag
         * context is still live at this point.  Let GTK finish the drag
         * first. */
        g_idle_add(show_move_error_idle, g_strdup(first_error->message));
        g_error_free(first_error);
    }
    return TRUE;
}

static void
canvas_self_drop(Canvas *c, double x, double y)
{
    Icon *anchor = c->pressed;
    if (!anchor)
        return;

    /* Dropping onto a folder already on the desktop files the dragged items
     * inside it rather than stacking them on top of its icon. */
    if (canvas_drop_into_folder(c, x, y))
        return;

    double off_x = c->press_x - anchor->x;
    double off_y = c->press_y - anchor->y;
    int nx = (int)lround(x - off_x);
    int ny = (int)lround(y - off_y);
    canvas_snap_xy(&nx, &ny);

    int dx = nx - anchor->x;
    int dy = ny - anchor->y;
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        if (!ic->selected)
            continue;
        ic->x += dx;
        ic->y += dy;
        if (g_app->snap)
            canvas_snap_xy(&ic->x, &ic->y);
        ic->want_x = ic->x;
        ic->want_y = ic->y;
        ic->auto_placed = FALSE;
        canvas_clamp_icon(c, ic);
    }
    /* The drop point may already be occupied.  Move the dragged icons (not
     * the ones that were sitting there) to the nearest free cells. */
    canvas_resolve_overlaps(c, TRUE, TRUE);
    canvas_queue_draw(c);
    app_save_soon();
}

static void
on_drag_data_get(GtkWidget *widget, GdkDragContext *context,
                 GtkSelectionData *sel, guint info, guint time, gpointer data)
{
    (void)widget;
    (void)context;
    (void)info;
    (void)time;
    Canvas *c = data;

    GString *uris = g_string_new(NULL);
    GString *gnome = g_string_new("copy\n");
    GString *plain = g_string_new(NULL);

    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        if (!ic->selected)
            continue;
        GFile *f = g_file_new_for_path(ic->path);
        char *uri = g_file_get_uri(f);
        g_object_unref(f);
        if (!uri)
            continue;
        g_string_append(uris, uri);
        g_string_append(uris, "\r\n");
        g_string_append(gnome, uri);
        g_string_append_c(gnome, '\n');
        g_string_append(plain, ic->path);
        g_string_append_c(plain, '\n');
        g_free(uri);
    }

    GdkAtom target = gtk_selection_data_get_target(sel);
    if (target == gdk_atom_intern_static_string("x-special/gnome-copied-files")) {
        gtk_selection_data_set(sel, target, 8,
                               (const guchar *)gnome->str, (gint)gnome->len);
    } else if (target == gdk_atom_intern_static_string("text/plain")) {
        gtk_selection_data_set_text(sel, plain->str, (gint)plain->len);
    } else {
        gtk_selection_data_set(sel, target, 8,
                               (const guchar *)uris->str, (gint)uris->len);
    }

    g_string_free(uris, TRUE);
    g_string_free(gnome, TRUE);
    g_string_free(plain, TRUE);
}

static gboolean
canvas_begin_dnd(Canvas *c, GdkEvent *event, double x, double y)
{
    Icon *anchor = c->pressed;
    if (!anchor)
        return FALSE;

    GtkTargetEntry targets[] = {
        { (gchar *)"text/uri-list", 0, 0 },
        { (gchar *)"x-special/gnome-copied-files", 0, 1 },
        { (gchar *)"text/plain", 0, 2 },
    };
    GtkTargetList *list = gtk_target_list_new(targets, G_N_ELEMENTS(targets));

    c->dnd_active = TRUE;
    GdkDragContext *ctx = gtk_drag_begin_with_coordinates(
        c->area, list, GDK_ACTION_COPY | GDK_ACTION_MOVE, 1, event,
        (int)x, (int)y);
    gtk_target_list_unref(list);

    if (!ctx) {
        c->dnd_active = FALSE;
        return FALSE;
    }

    if (anchor->pixbuf) {
        int pw = gdk_pixbuf_get_width(anchor->pixbuf);
        int ph = gdk_pixbuf_get_height(anchor->pixbuf);
        gtk_drag_set_icon_pixbuf(ctx, anchor->pixbuf, pw / 2, ph / 2);
    }

    c->dragging = FALSE;
    c->drag_moved = FALSE;
    c->drag_dx = c->drag_dy = 0;
    canvas_queue_draw(c);
    return TRUE;
}

static void
on_drag_end(GtkWidget *widget, GdkDragContext *context, gpointer data)
{
    (void)widget;
    (void)context;
    Canvas *c = data;
    c->dnd_active = FALSE;
    c->dragging = FALSE;
    c->drag_moved = FALSE;
    c->drag_dx = c->drag_dy = 0;
    c->pressed = NULL;
    canvas_queue_draw(c);
}

/* ---- drag and drop from file managers ------------------------------------ */

static void
on_drag_leave(GtkWidget *widget, GdkDragContext *context, guint time,
              gpointer data)
{
    (void)widget;
    (void)context;
    (void)time;
    (void)data;
}

static gboolean
on_drag_motion(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
               guint time, gpointer data)
{
    (void)x;
    (void)y;
    (void)data;

    /* A drag that started on one of our own canvases is an icon
     * rearrangement, not an import. */
    if (gtk_drag_get_source_widget(context) != NULL) {
        gdk_drag_status(context, GDK_ACTION_MOVE, time);
        return TRUE;
    }

    GdkAtom target = gtk_drag_dest_find_target(widget, context, NULL);
    if (target == GDK_NONE) {
        gdk_drag_status(context, 0, time);
        return FALSE;
    }
    GdkDragAction action = gdk_drag_context_get_suggested_action(context);
    if (action == 0)
        action = GDK_ACTION_COPY;
    gdk_drag_status(context, action, time);
    return TRUE;
}

static gboolean
on_drag_drop(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
             guint time, gpointer data)
{
    Canvas *c = data;

    /* Dropping back onto the desktop only moves the icons; the file itself
     * stays where it is. */
    Canvas *src = canvas_from_area(gtk_drag_get_source_widget(context));
    if (c && src == c && c->pressed) {
        canvas_self_drop(c, x, y);
        gtk_drag_finish(context, TRUE, FALSE, time);
        canvas_queue_draw(c);
        return TRUE;
    }

    GdkAtom target = gtk_drag_dest_find_target(widget, context, NULL);
    if (target == GDK_NONE) {
        gtk_drag_finish(context, FALSE, FALSE, time);
        return FALSE;
    }
    gtk_drag_get_data(widget, context, target, time);
    return TRUE;
}

static gboolean
import_one_path(Canvas *c, const char *path, gboolean move, GError **first_error,
                GPtrArray *imported)
{
    (void)c;
    char *dir = g_path_get_dirname(path);
    char *base = g_path_get_basename(path);
    gboolean ok = FALSE;

    /* Already on the desktop: nothing to copy. */
    if (g_strcmp0(dir, g_app->desktop_dir) != 0) {
        char *dest = unique_destination(g_app->desktop_dir, base);
        GFile *src = g_file_new_for_path(path);
        GFile *dst = g_file_new_for_path(dest);
        GError *err = NULL;

        ok = move ? move_file(src, dst, &err)
                  : copy_recursive(src, dst, &err);
        if (ok && imported)
            g_ptr_array_add(imported, g_strdup(dest));
        else if (!ok && err && first_error && !*first_error)
            *first_error = g_error_copy(err);
        g_clear_error(&err);
        g_object_unref(src);
        g_object_unref(dst);
        g_free(dest);
    }

    g_free(dir);
    g_free(base);
    return ok;
}

static Icon *
canvas_find_icon(Canvas *c, const char *key)
{
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        if (g_strcmp0(ic->key, key) == 0)
            return ic;
    }
    return NULL;
}

static void
canvas_find_free_cell(Canvas *c, int *x, int *y, Icon *except)
{
    int cw = canvas_cell_w();
    int ch = canvas_cell_h();
    int min_x = GRID_MARGIN;
    int min_y = GRID_MARGIN;
    int max_x = c->width - cw;
    int max_y = c->height - ch;
    if (max_x < min_x) max_x = min_x;
    if (max_y < min_y) max_y = min_y;

    /* Clamp first: an off-screen drop must not fall through the bounds checks
     * below and then get clamped onto a neighbour later. */
    if (*x < min_x) *x = min_x;
    if (*y < min_y) *y = min_y;
    if (*x > max_x) *x = max_x;
    if (*y > max_y) *y = max_y;

    if (!cell_occupied(c, *x, *y, except))
        return;

    int ox = *x;
    int oy = *y;

    /* Search outwards from the drop cell so the icon stays as close to the
     * drop point as possible without landing on top of another icon. */
    for (int ring = 1; ring <= 12; ring++) {
        for (int dcol = -ring; dcol <= ring; dcol++) {
            for (int drow = -ring; drow <= ring; drow++) {
                if (abs(dcol) != ring && abs(drow) != ring)
                    continue;
                int tx = ox + (dcol * cw);
                int ty = oy + (drow * ch);
                if (tx < min_x || ty < min_y)
                    continue;
                if (tx > max_x || ty > max_y)
                    continue;
                if (!cell_occupied(c, tx, ty, except)) {
                    *x = tx;
                    *y = ty;
                    return;
                }
            }
        }
    }

    /* The ring search keeps the wanted cell's offset.  With snap off that
     * offset may never line up with an empty cell, so fall back to the plain
     * grid before giving up. */
    for (int ty = min_y; ty <= max_y; ty += ch) {
        for (int tx = min_x; tx <= max_x; tx += cw) {
            if (!cell_occupied(c, tx, ty, except)) {
                *x = tx;
                *y = ty;
                return;
            }
        }
    }
}

/* Nudge icons that overlap another icon into the nearest free cell.  Icons
 * earlier in the list keep their spot and later ones yield.  With
 * selected_only, only selected icons move, so a drag resolves around the
 * icons that were already on the desktop. */
static void
canvas_resolve_overlaps(Canvas *c, gboolean selected_only,
                        gboolean update_want)
{
    int cw = canvas_cell_w();
    int ch = canvas_cell_h();
    for (guint i = 0; i < c->icons->len; i++) {
        Icon *ic = g_ptr_array_index(c->icons, i);
        if (ic->x < 0 || ic->y < 0)
            continue;
        if (selected_only && !ic->selected)
            continue;

        gboolean clash = FALSE;
        for (guint j = 0; j < c->icons->len; j++) {
            if (j == i)
                continue;
            Icon *o = g_ptr_array_index(c->icons, j);
            if (o->x < 0 || o->y < 0)
                continue;
            if (!selected_only && j > i)
                continue;
            if (cells_overlap(ic->x, ic->y, o->x, o->y, cw, ch)) {
                clash = TRUE;
                break;
            }
        }
        if (!clash)
            continue;

        int x = ic->x;
        int y = ic->y;
        if (g_app->snap)
            canvas_snap_xy(&x, &y);
        canvas_find_free_cell(c, &x, &y, ic);
        if (x == ic->x && y == ic->y)
            continue;              /* no room anywhere; leave it alone */
        ic->x = x;
        ic->y = y;
        if (update_want) {
            ic->want_x = x;
            ic->want_y = y;
        }
        canvas_clamp_icon(c, ic);
    }
}

static void
canvas_place_dropped(Canvas *c, GPtrArray *paths, int dx, int dy)
{
    if (!paths || paths->len == 0)
        return;

    int cw = canvas_cell_w();
    int ch = canvas_cell_h();
    int base_x = dx - (cw / 2);
    int base_y = dy - (ch / 2);

    for (guint i = 0; i < paths->len; i++) {
        const char *path = g_ptr_array_index(paths, i);
        char *key = g_path_get_basename(path);
        Icon *ic = canvas_find_icon(c, key);
        g_free(key);
        if (!ic)
            continue;

        int x = base_x + ((int)(i % 4) * cw);
        int y = base_y + ((int)(i / 4) * ch);
        if (g_app->snap)
            canvas_snap_xy(&x, &y);
        canvas_find_free_cell(c, &x, &y, ic);
        ic->x = x;
        ic->y = y;
        ic->want_x = x;
        ic->want_y = y;
        ic->auto_placed = FALSE;
        canvas_clamp_icon(c, ic);
    }
    canvas_queue_draw(c);
    app_save_soon();
}

static void
on_drag_data(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
             GtkSelectionData *sel, guint info, guint time, gpointer data)
{
    (void)widget;
    (void)info;
    Canvas *c = data;
    gboolean ok = FALSE;
    gboolean move = gdk_drag_context_get_selected_action(context) ==
                    GDK_ACTION_MOVE;
    GError *first_error = NULL;
    GPtrArray *imported = g_ptr_array_new_with_free_func(g_free);

    char **uris = gtk_selection_data_get_uris(sel);
    if (uris) {
        for (int i = 0; uris[i]; i++) {
            char *path = g_filename_from_uri(uris[i], NULL, NULL);
            if (!path)
                continue;
            if (import_one_path(c, path, move, &first_error, imported))
                ok = TRUE;
            g_free(path);
        }
        g_strfreev(uris);
    } else {
        /* Some sources only provide x-special/gnome-copied-files or
         * text/plain instead of a URI list. */
        const guchar *raw = gtk_selection_data_get_data(sel);
        int len = gtk_selection_data_get_length(sel);
        if (raw && len > 0) {
            char *text = g_strndup((const char *)raw, (gsize)len);
            char **lines = g_strsplit(text, "\n", -1);
            for (int i = 0; lines[i]; i++) {
                char *line = g_strstrip(lines[i]);
                if (!line[0])
                    continue;
                if (g_ascii_strcasecmp(line, "copy") == 0) {
                    move = FALSE;
                    continue;
                }
                if (g_ascii_strcasecmp(line, "cut") == 0) {
                    move = TRUE;
                    continue;
                }
                char *path = g_str_has_prefix(line, "file://")
                                 ? g_filename_from_uri(line, NULL, NULL)
                                 : g_strdup(line);
                if (path && import_one_path(c, path, move, &first_error,
                                            imported))
                    ok = TRUE;
                g_free(path);
            }
            g_strfreev(lines);
            g_free(text);
        }
    }

    if (first_error) {
        app_show_error(c, "Drop", first_error->message);
        g_error_free(first_error);
    }
    if (ok) {
        app_reload_all();
        canvas_place_dropped(c, imported, x, y);
        app_save_soon();
    }
    g_ptr_array_free(imported, TRUE);
    gtk_drag_finish(context, ok, FALSE, time);
}

/* ---- windows ------------------------------------------------------------- */

static void
install_css(void)
{
    static const char *css =
        "window.jtwpi {\n"
        "  background-color: transparent;\n"
        "  background-image: none;\n"
        "}\n"
        "window.jtwpi drawingarea {\n"
        "  background-color: transparent;\n"
        "}\n"
        "menu {\n"
        "  padding: 4px 0;\n"
        "}\n"
        "menuitem {\n"
        "  padding: 5px 10px;\n"
        "}\n";
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, css, -1, NULL);
    GdkScreen *screen = gdk_screen_get_default();
    if (screen)
        gtk_style_context_add_provider_for_screen(
            screen, GTK_STYLE_PROVIDER(p),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

static void
on_window_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    if (g_app && g_app->rebuilding)
        return;
    gtk_main_quit();
}

static Canvas *
canvas_new(App *app, GtkWidget *window, GdkMonitor *mon, int index)
{
    Canvas *c = g_new0(Canvas, 1);
    c->app = app;
    c->window = window;
    c->monitor_key = monitor_key_for(mon, index);
    c->width = 0;
    c->height = 0;
    c->scale = mon ? gdk_monitor_get_scale_factor(mon) : 1;
    if (c->scale < 1)
        c->scale = 1;
    c->icons = g_ptr_array_new();

    GdkDisplay *display = gdk_display_get_default();
    c->cursor_default = gdk_cursor_new_for_display(display, GDK_LEFT_PTR);

    c->area = gtk_drawing_area_new();
    gtk_widget_add_events(c->area,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK | GDK_ENTER_NOTIFY_MASK |
                          GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK |
                          GDK_KEY_PRESS_MASK);
    gtk_widget_set_can_focus(c->area, TRUE);
    gtk_widget_set_hexpand(c->area, TRUE);
    gtk_widget_set_vexpand(c->area, TRUE);
    gtk_container_add(GTK_CONTAINER(window), c->area);

    g_signal_connect(c->area, "draw", G_CALLBACK(on_draw), c);
    g_signal_connect(c->area, "size-allocate", G_CALLBACK(on_size_allocate), c);
    g_signal_connect(c->area, "button-press-event", G_CALLBACK(on_button_press), c);
    g_signal_connect(c->area, "button-release-event", G_CALLBACK(on_button_release), c);
    g_signal_connect(c->area, "motion-notify-event", G_CALLBACK(on_motion), c);
    g_signal_connect(c->area, "leave-notify-event", G_CALLBACK(on_leave), c);
    g_signal_connect(c->area, "key-press-event", G_CALLBACK(on_key_press), c);

    GtkTargetEntry drop_types[] = {
        { (gchar *)"text/uri-list", 0, 0 },
        { (gchar *)"x-special/gnome-copied-files", 0, 1 },
        { (gchar *)"text/plain", 0, 2 },
    };
    gtk_drag_dest_set(c->area, 0, drop_types, G_N_ELEMENTS(drop_types),
                      GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(c->area, "drag-motion", G_CALLBACK(on_drag_motion), c);
    g_signal_connect(c->area, "drag-leave", G_CALLBACK(on_drag_leave), c);
    g_signal_connect(c->area, "drag-drop", G_CALLBACK(on_drag_drop), c);
    g_signal_connect(c->area, "drag-data-received", G_CALLBACK(on_drag_data), c);
    g_signal_connect(c->area, "drag-data-get", G_CALLBACK(on_drag_data_get), c);
    g_signal_connect(c->area, "drag-end", G_CALLBACK(on_drag_end), c);

    gtk_widget_show_all(window);
    canvas_load(c);
    return c;
}

static void
app_destroy_windows(void)
{
    if (!g_app || !g_app->canvases)
        return;
    app_state_save();
    for (guint i = 0; i < g_app->canvases->len; i++) {
        Canvas *c = g_ptr_array_index(g_app->canvases, i);
        if (c->window)
            gtk_widget_destroy(c->window);
        if (c->cursor_default) g_object_unref(c->cursor_default);
        canvas_clear_icons(c);
        g_ptr_array_free(c->icons, TRUE);
        g_free(c->monitor_key);
        g_free(c);
    }
    g_ptr_array_set_size(g_app->canvases, 0);
}

static void
app_create_windows(void)
{
    if (!g_app)
        return;
    g_app->rebuilding = TRUE;
    app_destroy_windows();
    g_app->rebuilding = FALSE;

    GdkDisplay *display = gdk_display_get_default();
    GdkScreen *screen = gdk_screen_get_default();
    GdkVisual *visual = screen ? gdk_screen_get_rgba_visual(screen) : NULL;

    if (g_app->windowed) {
        GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(win), "jtwpi");
        gtk_window_set_default_size(GTK_WINDOW(win), 1000, 680);
        gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
        gtk_widget_set_app_paintable(win, TRUE);
        if (visual)
            gtk_widget_set_visual(win, visual);
        gtk_style_context_add_class(gtk_widget_get_style_context(win), "jtwpi");
        GdkMonitor *mon = display ? gdk_display_get_primary_monitor(display) : NULL;
        if (!mon && display)
            mon = gdk_display_get_monitor(display, 0);
        Canvas *c = canvas_new(g_app, win, mon, 0);
        g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroy), NULL);
        g_ptr_array_add(g_app->canvases, c);
        return;
    }

    int n = display ? gdk_display_get_n_monitors(display) : 1;
    if (n < 1)
        n = 1;
    for (int i = 0; i < n; i++) {
        GdkMonitor *mon = display ? gdk_display_get_monitor(display, i) : NULL;
        GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_widget_set_app_paintable(win, TRUE);
        if (visual)
            gtk_widget_set_visual(win, visual);
        gtk_style_context_add_class(gtk_widget_get_style_context(win), "jtwpi");
        gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
        gtk_window_set_resizable(GTK_WINDOW(win), FALSE);

        gtk_layer_init_for_window(GTK_WINDOW(win));
        gtk_layer_set_namespace(GTK_WINDOW(win), "jtwpi");
        gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_BOTTOM);
        gtk_layer_set_monitor(GTK_WINDOW(win), mon);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        /* Reserve no space of our own, but still obey the space other layer
         * surfaces have reserved (the bar's exclusive zone).  An exclusive
         * zone of -1 would ignore those and hand us the whole output, which
         * puts the bottom row of icons underneath the bar. */
        gtk_layer_set_exclusive_zone(GTK_WINDOW(win), 0);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(win),
                                    GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

        Canvas *c = canvas_new(g_app, win, mon, i);
        g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroy), NULL);
        g_ptr_array_add(g_app->canvases, c);
    }
}

static gboolean
rebuild_timeout_cb(gpointer data)
{
    (void)data;
    g_app->rebuild_timeout = 0;
    app_create_windows();
    return G_SOURCE_REMOVE;
}

static void
app_schedule_rebuild_windows(void)
{
    if (!g_app)
        return;
    if (g_app->rebuild_timeout)
        g_source_remove(g_app->rebuild_timeout);
    g_app->rebuild_timeout = g_timeout_add(300, rebuild_timeout_cb, NULL);
}

static void
on_monitor_added(GdkDisplay *display, GdkMonitor *mon, gpointer data)
{
    (void)display;
    (void)mon;
    (void)data;
    app_schedule_rebuild_windows();
}

static void
on_monitor_removed(GdkDisplay *display, GdkMonitor *mon, gpointer data)
{
    (void)display;
    (void)mon;
    (void)data;
    app_schedule_rebuild_windows();
}

/* ---- directory monitoring ------------------------------------------------- */

static gboolean
reload_timeout_cb(gpointer data)
{
    (void)data;
    g_app->reload_timeout = 0;
    app_reload_all();
    return G_SOURCE_REMOVE;
}

static void
app_reload_soon(void)
{
    if (!g_app)
        return;
    if (g_app->reload_timeout)
        g_source_remove(g_app->reload_timeout);
    g_app->reload_timeout = g_timeout_add(400, reload_timeout_cb, NULL);
}

static void
on_dir_changed(GFileMonitor *monitor, GFile *file, GFile *other,
               GFileMonitorEvent event, gpointer data)
{
    (void)monitor;
    (void)file;
    (void)other;
    (void)data;
    if (event == G_FILE_MONITOR_EVENT_CHANGED ||
        event == G_FILE_MONITOR_EVENT_CREATED ||
        event == G_FILE_MONITOR_EVENT_DELETED ||
        event == G_FILE_MONITOR_EVENT_MOVED ||
        event == G_FILE_MONITOR_EVENT_RENAMED ||
        event == G_FILE_MONITOR_EVENT_MOVED_IN ||
        event == G_FILE_MONITOR_EVENT_MOVED_OUT)
        app_reload_soon();
}

static void
on_icon_theme_changed(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)object;
    (void)pspec;
    (void)data;
    app_reload_icons();
}

/* ---- lifecycle ------------------------------------------------------------ */

static App *
app_new(void)
{
    App *a = g_new0(App, 1);
    a->canvases = g_ptr_array_new();
    a->icon_size = DEFAULT_ICON_SIZE;
    a->snap = TRUE;
    a->show_icons = TRUE;
    a->state_path = g_build_filename(g_get_user_config_dir(), "jtwpi",
                                     "state.ini", NULL);
    return a;
}

static void
app_free(App *a)
{
    if (!a)
        return;
    app_destroy_windows();
    if (a->reload_timeout)
        g_source_remove(a->reload_timeout);
    if (a->save_timeout)
        g_source_remove(a->save_timeout);
    if (a->rebuild_timeout)
        g_source_remove(a->rebuild_timeout);
    if (a->dir_monitor)
        g_object_unref(a->dir_monitor);
    if (a->state)
        g_key_file_free(a->state);
    if (a->label_font)
        pango_font_description_free(a->label_font);
    g_list_free_full(a->clip_files, g_object_unref);
    g_ptr_array_free(a->canvases, TRUE);
    g_free(a->desktop_dir);
    if (a->desktop_file)
        g_object_unref(a->desktop_file);
    g_free(a->state_path);
    g_free(a->opt_desktop);
    g_free(a);
}

static void
print_usage(const char *prog)
{
    g_print(
        "Usage: %s [OPTIONS]\n"
        "\n"
        "A desktop icon layer for wlroots Wayland compositors.\n"
        "\n"
        "Options:\n"
        "  --desktop PATH     Use PATH instead of the XDG desktop directory\n"
        "  --icon-size N      Icon size in pixels (24-128, default %d)\n"
        "  --windowed         Run in a normal window instead of a layer surface\n"
        "  --hidden           Start with desktop icons hidden\n"
        "  --version          Print version and exit\n"
        "  --help             Show this help\n",
        prog, DEFAULT_ICON_SIZE);
}

static int
parse_int_arg(const char *s)
{
    if (!s || !s[0])
        return 0;
    return (int)g_ascii_strtoll(s, NULL, 10);
}

static void
wait_for_wayland_socket(void)
{
    const char *display = g_getenv("WAYLAND_DISPLAY");
    if (!display || !display[0])
        return;

    char *path;
    if (display[0] == '/') {
        path = g_strdup(display);
    } else {
        const char *runtime = g_getenv("XDG_RUNTIME_DIR");
        if (!runtime || !runtime[0])
            return;
        path = g_build_filename(runtime, display, NULL);
    }

    /* When started from autostart alongside the compositor the socket may
     * not exist yet.  Wait for it with sleeps, not a spin, so the process
     * uses no CPU while the session comes up. */
    for (int i = 0; i < WAIT_ATTEMPTS(WAYLAND_SOCKET_WAIT_MS); i++) {
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            break;
        g_usleep(STARTUP_POLL_US);
    }
    g_free(path);
}

int
main(int argc, char **argv)
{
    gboolean want_version = FALSE;
    gboolean want_help = FALSE;

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--version") == 0) {
            want_version = TRUE;
        } else if (g_strcmp0(argv[i], "--help") == 0 ||
                   g_strcmp0(argv[i], "-h") == 0) {
            want_help = TRUE;
        }
    }
    if (want_version) {
        g_print("%s %s\n", APP_NAME, APP_VERSION);
        return 0;
    }
    if (want_help) {
        print_usage(argv[0]);
        return 0;
    }

    App *a = app_new();
    g_app = a;

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--windowed") == 0) {
            a->windowed = TRUE;
        } else if (g_strcmp0(argv[i], "--hidden") == 0) {
            a->opt_hidden = TRUE;
        } else if (g_strcmp0(argv[i], "--desktop") == 0 && i + 1 < argc) {
            g_free(a->opt_desktop);
            a->opt_desktop = g_strdup(argv[++i]);
        } else if (g_str_has_prefix(argv[i], "--desktop=")) {
            g_free(a->opt_desktop);
            a->opt_desktop = g_strdup(argv[i] + strlen("--desktop="));
        } else if (g_strcmp0(argv[i], "--icon-size") == 0 && i + 1 < argc) {
            a->opt_icon_size = parse_int_arg(argv[++i]);
        } else if (g_str_has_prefix(argv[i], "--icon-size=")) {
            a->opt_icon_size = parse_int_arg(argv[i] + strlen("--icon-size="));
        }
    }

    wait_for_wayland_socket();
    gtk_init(&argc, &argv);
    g_set_application_name(APP_NAME);

    /* The socket can exist slightly before the compositor advertises the
     * layer-shell global.  Give it a short grace period; the loop sleeps, so
     * it does not burn CPU. */
    if (!a->windowed) {
        for (int i = 0; i < WAIT_ATTEMPTS(LAYER_SHELL_WAIT_MS); i++) {
            if (gtk_layer_is_supported())
                break;
            g_main_context_iteration(NULL, FALSE);
            g_usleep(STARTUP_POLL_US);
        }
    }

    a->desktop_dir = desktop_directory();
    a->desktop_file = g_file_new_for_path(a->desktop_dir);
    a->icon_theme = gtk_icon_theme_get_default();
    a->settings = gtk_settings_get_default();

    a->label_font = pango_font_description_from_string("Sans");
    pango_font_description_set_absolute_size(a->label_font, 11 * PANGO_SCALE);

    app_state_load();
    install_css();

    a->dir_monitor = g_file_monitor_directory(a->desktop_file,
                                              G_FILE_MONITOR_NONE, NULL, NULL);
    if (a->dir_monitor)
        g_signal_connect(a->dir_monitor, "changed",
                         G_CALLBACK(on_dir_changed), NULL);

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        g_signal_connect(display, "monitor-added",
                         G_CALLBACK(on_monitor_added), NULL);
        g_signal_connect(display, "monitor-removed",
                         G_CALLBACK(on_monitor_removed), NULL);
    }
    if (a->settings)
        g_signal_connect(a->settings, "notify::gtk-icon-theme-name",
                         G_CALLBACK(on_icon_theme_changed), NULL);

    app_create_windows();

    gtk_main();

    app_state_save();
    app_free(a);
    g_app = NULL;
    return 0;
}


