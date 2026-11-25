/*
 * jtwpi - a desktop icon layer for wlroots Wayland compositors
 *
 * Raw Wayland (zwlr_layer_shell_v1) + Cairo + Pango + xkbcommon.  No GTK,
 * no gtk-layer-shell, no GIO, no icon themes: icons are drawn with the same
 * built-in Cairo set as jtfiles (see jticons.h) and follow the shared accent.
 *
 * Build: make
 * Run:   ./jtwpi                       desktop layer
 *        ./jtwpi '#RRGGBB'             initial accent override
 *        ./jtwpi --accent '#RRGGBB'
 *        ./jtwpi --desktop ~/Desktop
 *        ./jtwpi --hidden
 *
 * $XDG_RUNTIME_DIR/jtwpi-ctl.sock accepts "accent #RRGGBB", "#RRGGBB" and
 * "quit" (see jtwpictl).  The accent is also read from /etc/jt/accent.conf
 * at startup so the first frame already matches the rest of the desktop.
 */
#define _GNU_SOURCE
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "wlr-layer-shell-client-protocol.h"
#include "cursor-shape-client-protocol.h"
#include "jticons.h"

#include <cairo.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define APP_NAME    "jtwpi"
#define APP_VERSION "2.0.0"

#define DEFAULT_ICON_SIZE 48
#define MIN_ICON_SIZE     24
#define MAX_ICON_SIZE     128
#define CELL_PAD_X        12
#define CELL_PAD_TOP      8
#define CELL_LABEL_GAP    3
#define LABEL_HEIGHT      34
#define CELL_PAD_BOTTOM   8
#define GRID_MARGIN       12
#define BOTTOM_MARGIN     12
#define DRAG_THRESHOLD    4

#define DEFAULT_ACCENT 0xE8A03C
#define ACCENT_CONF    "/etc/jt/accent.conf"

/* Shared appearance.conf border-animation toggle (as read by jtlabwc). */
#define APPEARANCE_CONFIG "appearance.conf"

/* Desktop vignette: a small pre-rendered sprite stretched over the output.
 * Rasterizing a full-screen radial gradient on every repaint is expensive;
 * this soft, low-alpha gradient looks identical when scaled up. */
#define VIGNETTE_SIZE 512

/* Post-drop selection highlight: one accent segment does a full lap of each
 * newly selected icon's border before the selected border is shown. */
#define SEL_LAP_MS       900.0
#define SEL_LAP_TICK_MS  16
#define SEL_LAP_WIDTH    3.0
#define SEL_LAP_MIN_SEG  14.0
#define SEL_LAP_MAX_SEG  48.0

#define MAX_OUTPUTS   8
#define MAX_ICONS     1024
#define MAX_MENU_ITEMS 32
#define MENU_INSET    5
#define NBUFS         2

#define BTN_LEFT  0x110
#define BTN_RIGHT 0x111

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

enum {
    ACT_NONE, ACT_OPEN, ACT_CUT, ACT_COPY, ACT_PASTE, ACT_RENAME,
    ACT_TRASH, ACT_DELETE, ACT_PROPERTIES, ACT_NEW_FOLDER,
    ACT_NEW_LAUNCHER, ACT_ARRANGE, ACT_SNAP, ACT_TOGGLE_ICONS,
    ACT_REFRESH, ACT_OPEN_FOLDER, ACT_SIZE
};

/* Filesystem operation kinds, shared with the re-exec'd progress children. */
enum { XOP_COPY = 0, XOP_MOVE = 1, XOP_TRASH = 2, XOP_DELETE = 3 };

typedef struct App App;
typedef struct Out Out;
typedef struct Menu Menu;
typedef struct Dialog Dialog;

/* Where a Paste request should land on the desktop.  Captured before a menu
 * closes or an async clipboard read begins, then carried to xfer_finish(). */
typedef struct {
    Out *out;
    double x, y;
    int valid;
    int armed;                      /* explicitly set by menu activation */
} PastePlace;

typedef struct {
    struct wl_buffer *buf;
    void *data;
    size_t map_size;
    int w, h, busy;
} Buf;

typedef struct {
    struct wl_surface *wl;
    Buf bufs[NBUFS];
    struct wl_callback *frame;
    /* A queued repaint is executed from the wl_surface.frame callback, so
     * every commit lands on the compositor's presentation clock. */
    int queued;
    int painted;                /* a buffer has been attached at least once */
    int *dirty_flag;            /* owner's dirty bit, cleared after a real paint */
    int draw_w, draw_h, draw_scale;
    void (*paint)(cairo_t *, int, int, void *);
    void *paint_data;
} Surf;

typedef struct {
    char *path, *key, *name, *icon, *exec;
    int is_dir, is_desktop;
    off_t size;
    time_t mtime;
    int x, y, want_x, want_y, auto_placed;
    int selected, hover;
    cairo_surface_t *sf;
    int sf_px;
    unsigned sf_accent;
    cairo_surface_t *label_sf;      /* cached rendered label */
    int label_px;                   /* icon size the label was rendered for */
    char label_name[256];           /* name the label was rendered for */
} DIcon;

typedef enum { MI_ACTION, MI_SEP, MI_SUB, MI_CHECK } MenuKind;

typedef struct {
    MenuKind kind;
    int id;
    char label[64];
    char shortcut[24];
    char icon[64];
    int enabled, checked;
    Menu *sub;
} MenuItem;

struct Menu {
    App *app;
    Out *out;
    Menu *parent;
    int parent_idx;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *popup;
    Surf surf;
    Menu *sub;              /* currently-open submenu */
    int configured, open, done, x, y, w, h;
    MenuItem items[MAX_MENU_ITEMS];
    int n, hover, dirty;
    double popup_anim_start; /* start time of the popup border lap */
    int popup_anim_active;
};

typedef enum { DLG_TEXT, DLG_INFO, DLG_CONFIRM } DialogKind;

struct Dialog {
    DialogKind kind;
    Out *out;                       /* originating output for callbacks */
    char title[96], prompt[192], text[512], info[2048];
    int cursor;
    int ok_id;
    void (*on_ok)(Out *o, const char *text, int id);
};

/* A generic xdg_toplevel wrapping jtwpi's existing Surf/Buf paint engine.
 * Both the dialog and the transfer progress window use one of these. */
typedef struct Win Win;
struct Win {
    App *app;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    Surf surf;
    int configured, dirty, w, h, scale;
    void (*on_close)(App *a);
};

/* One live copy/move/trash/delete operation and its themed progress window. */
typedef struct {
    pid_t pid;
    int fd;
    char buf[4096];
    size_t len;
    int active, cut, op;
    int show_pending;                       /* progress window is delayed */
    double show_at;                         /* monotonic time to show it */
    long long total_items, items_done;
    long long total_bytes, bytes_done;
    int done_items, failed;
    double speed, speed_t0, speed_bytes0;
    char current[NAME_MAX + 1];
    /* Destination basenames for a paste, kept until completion so the new
     * desktop icons can be selected after the final rescan. */
    char dst_names[64][NAME_MAX + 1];
    int n_dst;
    /* Captured Paste location; applied after the final rescan. */
    PastePlace place;
    Win win;
} Transfer;

struct Out {
    App *app;
    struct wl_output *output;
    struct zwlr_layer_surface_v1 *layer;
    Surf surf;
    int id, configured, width, height, scale;
    char key[128];

    DIcon icons[MAX_ICONS];
    int n;
    int hover, press;
    int dragging, drag_moved;
    double press_x, press_y, drag_dx, drag_dy;
    uint32_t last_click_time;
    int last_click_idx;
    double last_click_x, last_click_y;
    int selecting;
    double sel_x0, sel_y0, sel_x1, sel_y1;

    Menu *menu, *submenu;
    int dirty;
};

/* One context per wl_data_offer.  MIME events arrive before the enter()
 * or selection() event that says whether an offer is for DnD or clipboard,
 * so the flags have to live on the offer itself, not in a single slot. */
typedef struct DataOffer {
    App *app;
    struct wl_data_offer *offer;
    struct DataOffer *next;
    unsigned uris : 1;
    unsigned gnome : 1;
    unsigned text : 1;
    char text_mime[64];
} DataOffer;

/* Frozen selection payload.  wl_data_source.send is a callback from the
 * compositor and must never read live app state that can change later. */
typedef struct {
    char paths[64][PATH_MAX];
    int n;
    int cut;
    char text[16384];
    int has_text;
} ClipSnapshot;

/* Nonblocking wl_data_offer.receive() in progress. */
typedef struct {
    int fd;
    size_t len;
    int gnome;
    int overflow;
    int attempt;
    double deadline;
    /* 64 paths at PATH_MAX plus the gnome verb and separators. */
    char buf[64 * PATH_MAX + 4096];
} ClipRead;

/* Nonblocking wl_data_source.send() writes, like GTK's GOutputStream writer. */
typedef struct ClipWrite {
    struct ClipWrite *next;
    int fd;
    char *buf;
    size_t len, off;
} ClipWrite;

struct App {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *deco_manager;
    struct wp_cursor_shape_manager_v1 *cursor_manager;
    struct wp_cursor_shape_device_v1 *cursor_device;
    uint32_t cursor_serial, cursor_shape, cursor_applied;

    /* Drag and drop (wl_data_device) */
    struct wl_data_device_manager *data_device_manager;
    struct wl_data_device *data_device;
    struct wl_data_source *dnd_source;      /* outgoing drag */
    struct wl_data_offer *dnd_offer;        /* incoming drag */
    DataOffer *dnd_ctx;                     /* context for dnd_offer */
    DataOffer *offers;                      /* all live offer contexts */
    struct wl_surface *dnd_icon_surface;
    Buf dnd_icon_buf;
    uint32_t dnd_press_serial, dnd_enter_serial;
    int dnd_pending, dnd_accept, dnd_accepted;
    uint32_t dnd_action;                    /* negotiated drag action (source) */
    uint32_t dnd_offer_action;              /* action reported by wl_data_offer.action */
    int dnd_have_gnome;                     /* incoming drag cut/copy verb */
    Out *dnd_src_out;                       /* output where drag started */
    Out *dnd_drop_out;                      /* internal drop target */
    double dnd_drop_x, dnd_drop_y;          /* internal drop position */

    /* Wayland clipboard (wl_data_device selection) */
    struct wl_data_source *clip_source;     /* we own the selection */
    struct wl_data_offer *clip_offer;       /* another client owns it */
    DataOffer *clip_ctx;                    /* context for clip_offer */
    ClipSnapshot clip_snap;                 /* payload we published */
    ClipRead clip_read;                     /* async remote paste */
    ClipWrite *clip_writes;                 /* pending source.send writes */
    int clip_cache_valid;                   /* clip[] belongs to clip_ctx */
    uint32_t clip_serial;                   /* last input serial */

    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    uint32_t mods_dep, mods_lat, mods_lock, mods_group;

    Out outs[MAX_OUTPUTS];
    int n_outs;
    struct wl_surface *ptr_surface;
    Out *ptr_out;
    Menu *ptr_menu;
    double ptr_x, ptr_y;
    uint32_t ptr_serial;
    struct wl_surface *kb_surface;

    JtiPalette pal;
    unsigned accent;

    char *desktop_dir;
    char *state_path;
    int icon_size, snap, show_icons;
    int opt_hidden;
    char *opt_desktop;
    int opt_size;

    char clip[64][PATH_MAX];
    int n_clip, clip_cut;

    /* Basenames created by the last paste/drop.  Re-applied after every icon
     * rescan until the user selects something, so background refreshes cannot
     * wipe the highlight on newly created desktop icons. */
    char post_dir[PATH_MAX];
    char post_names[64][NAME_MAX + 1];
    int n_post_names, post_armed;
    /* Shared appearance.conf toggle for all accent-border laps. */
    int border_anim_enabled;
    /* >0: lap running; -1: lap already played for this batch. */
    double post_sel_anim_start;

    /* Pending paste location, valid from the Paste action until do_paste(). */
    PastePlace paste_place;

    /* Separate toplevel windows (dialog + transfer progress) */
    Dialog *dialog;
    Win dlg_win;
    Transfer xfer;

    char status[256];
    time_t status_until;

    int ctl_fd, ctl_created;
    char ctl_path[256];

    time_t dir_mtime;
    int quit;
};

static App *g_app;

/* ---- forward declarations ----------------------------------------------- */
static void out_mark(Out *o);
static void all_mark(App *a);
static double now_sec(void);
static int post_sel_icon_pending(const App *a, const DIcon *ic);
static int cell_w(void);
static int cell_h(void);
static void out_load_icons(Out *o, int preserve);
static void post_sel_disarm(App *a);
static void post_sel_apply(Out *o);
static void out_relayout(Out *o, int reflow_auto);
static void menu_close(Menu *m);
static void menu_close_root(Out *o);
static void menu_open(Out *o, int x, int y, int on_icon);
static void menu_open_size_sub(Out *o, Menu *parent, int idx, int x, int y);
static void action_run(Out *o, int id, int arg);
static int surf_draw(App *a, Surf *s, int w, int h, int scale,
                     void (*paint)(cairo_t *, int, int, void *), void *data,
                     int *dirty_flag);
static void surf_cancel(Surf *s);
static void check_dir(App *a);
static void ctl_accept(App *a);
static int  ctl_init(App *a);
static void clipboard_publish(App *a);
static int  clipboard_fetch(App *a, int *cut);
static int  clipboard_has(App *a);
static void clip_read_cancel(App *a);
static void clip_write_cancel_all(App *a);
static void clip_write_poll(App *a, int fd);
static void ctl_cleanup(App *a);
static void dnd_icon_destroy(App *a);
static void dnd_begin(App *a, Out *o);
static void dialog_close(void);
static void dialog_key(xkb_keysym_t sym);
static void xfer_poll(App *a);
static void xfer_finish(App *a, int code);
static void fs_op_start(App *a, int op, char (*paths)[PATH_MAX], int n);

/* ---- tiny helpers -------------------------------------------------------- */

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "jtwpi: out of memory\n"); exit(1); }
    return p;
}

static void *xcalloc(size_t n, size_t s)
{
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) { fprintf(stderr, "jtwpi: out of memory\n"); exit(1); }
    return p;
}

static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static const char *path_base(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

/* Basenames of files just created by paste/drop.  The desktop directory is
 * rescanned from several places (transfer completion, drop, check_dir), and
 * every rescan wipes icon selection, so the pending set lives on App and is
 * re-applied here until the user takes over selection. */
static void post_sel_disarm(App *a)
{
    a->post_armed = 0;
    a->n_post_names = 0;
    a->post_dir[0] = 0;
    a->post_sel_anim_start = 0;
}

static void post_sel_arm(App *a, const char *dir,
                         char (*names)[NAME_MAX + 1], int n)
{
    post_sel_disarm(a);
    if (!dir || !*dir || n <= 0) return;
    snprintf(a->post_dir, sizeof a->post_dir, "%s", dir);
    for (int i = 0; i < n && a->n_post_names < 64; i++) {
        if (!names[i][0]) continue;
        snprintf(a->post_names[a->n_post_names], NAME_MAX + 1, "%s", names[i]);
        a->n_post_names++;
    }
    if (a->n_post_names) a->post_armed = 1;
    a->post_sel_anim_start = 0; /* starts when the icons actually appear */
}

static void post_sel_apply(Out *o)
{
    App *a = o->app;
    if (!a || !a->post_armed || !a->post_dir[0]) return;
    if (strcmp(a->desktop_dir, a->post_dir)) return;
    int any = 0;
    for (int i = 0; i < o->n; i++) {
        DIcon *ic = &o->icons[i];
        for (int k = 0; k < a->n_post_names; k++) {
            if (strcmp(ic->key, a->post_names[k])) continue;
            ic->selected = 1;
            any = 1;
            break;
        }
    }
    if (any) {
        if (a->post_sel_anim_start == 0.0)
            a->post_sel_anim_start = now_sec();
        out_mark(o);
    }
}

static void paste_place_clear(App *a)
{
    memset(&a->paste_place, 0, sizeof a->paste_place);
}

static void paste_place_set(App *a, Out *o, double x, double y)
{
    paste_place_clear(a);
    if (!o) return;
    a->paste_place.out = o;
    a->paste_place.x = x;
    a->paste_place.y = y;
    a->paste_place.valid = 1;
}

/* Menu activation closes the menu before action_run(), so record the root
 * menu's anchor while it is still alive. */
static void paste_place_from_menu(App *a, Menu *m, int id)
{
    if (id != ACT_PASTE || !m) return;
    Menu *root = m;
    while (root->parent) root = root->parent;
    paste_place_set(a, root->out, root->x, root->y);
    a->paste_place.armed = 1;
}

static void path_join(char *out, size_t n, const char *a, const char *b)
{
    int r = snprintf(out, n, "%s/%s", a, b);
    if (r < 0) out[0] = 0;
}

static void trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
}

static char *expand_home(const char *p)
{
    if (!p || !*p) return NULL;
    if (!strcmp(p, "~")) return xstrdup(getenv("HOME") ? getenv("HOME") : "/");
    if (!strncmp(p, "~/", 2)) {
        const char *home = getenv("HOME") ? getenv("HOME") : "/";
        size_t n = strlen(home) + strlen(p) + 2;
        char *o = xmalloc(n);
        snprintf(o, n, "%s/%s", home, p + 2);
        return o;
    }
    return xstrdup(p);
}

static void spawn_detached(const char *const av[])
{
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        int dev = open("/dev/null", O_RDWR);
        if (dev >= 0) {
            dup2(dev, 0); dup2(dev, 1); dup2(dev, 2);
            if (dev > 2) close(dev);
        }
        execvp(av[0], (char *const *)av);
        _exit(127);
    }
    signal(SIGCHLD, SIG_IGN);
}

static int remove_tree_cb(const char *path, const struct stat *st, int type,
                          struct FTW *ftw)
{
    (void)st; (void)ftw;
    if (type == FTW_DP) return rmdir(path);
    return unlink(path);
}

static void remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode))
        nftw(path, remove_tree_cb, 32, FTW_DEPTH | FTW_PHYS);
    else
        unlink(path);
}

static void copy_recursive(const char *src, const char *dst)
{
    struct stat st;
    if (lstat(src, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 0777) != 0) return;
        DIR *d = opendir(src);
        if (!d) return;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char s[PATH_MAX], t[PATH_MAX];
            path_join(s, sizeof s, src, de->d_name);
            path_join(t, sizeof t, dst, de->d_name);
            copy_recursive(s, t);
        }
        closedir(d);
    } else {
        int in = open(src, O_RDONLY);
        if (in < 0) return;
        int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 0777);
        if (out < 0) { close(in); return; }
        char buf[32768];
        ssize_t n;
        while ((n = read(in, buf, sizeof buf)) > 0) {
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(out, buf + off, (size_t)(n - off));
                if (w <= 0) break;
                off += w;
            }
        }
        close(in); close(out);
    }
}

static void unique_path(const char *dir, const char *base, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", dir, base);
    if (access(out, F_OK) != 0) return;
    for (int i = 1; i < 10000; i++) {
        const char *dot = strrchr(base, '.');
        if (dot && dot != base)
            snprintf(out, n, "%s/%.*s-%d%s", dir, (int)(dot - base), base, i, dot);
        else
            snprintf(out, n, "%s/%s-%d", dir, base, i);
        if (access(out, F_OK) != 0) return;
    }
}

/* ---- transfer worker helpers (run in the re-exec'd child) ----------------- */

typedef struct {
    int fd;
    long long items, bytes;
} XferCtx;

static int xfer_write_all(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (w == 0) return 0;
        p += w;
        n -= (size_t)w;
    }
    return 1;
}

static int xfer_write_u32(int fd, uint32_t v)
{
    return xfer_write_all(fd, &v, sizeof v);
}

static int xfer_read_path(char *out, size_t cap)
{
    uint32_t len;
    if (fread(&len, sizeof len, 1, stdin) != 1 || len == 0 || len > cap)
        return 0;
    return fread(out, 1, len, stdin) == len;
}

static void xfer_sendf(int fd, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;
    line[n] = '\n';
    ssize_t ignored = write(fd, line, (size_t)n + 1);
    (void)ignored;
}

static void xfer_scan(const char *path, long long *items, long long *bytes)
{
    struct stat st;
    if (lstat(path, &st) != 0) return;
    (*items)++;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char p[PATH_MAX];
            path_join(p, sizeof p, path, de->d_name);
            xfer_scan(p, items, bytes);
        }
        closedir(d);
    } else if (S_ISREG(st.st_mode)) {
        *bytes += (long long)st.st_size;
    }
}

static void xfer_name(char *out, size_t n, const char *name)
{
    size_t k = 0;
    for (const char *p = name; *p && k + 1 < n; p++)
        out[k++] = (unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E ?
                   '_' : *p;
    out[k] = '\0';
}

static int xfer_copy_file(const char *src, const char *dst, XferCtx *c)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    struct stat st;
    if (fstat(in, &st) != 0) { close(in); return -1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                   st.st_mode & 0777);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    long long since_report = 0;
    ssize_t r;
    while ((r = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0) { close(in); close(out); return -1; }
            off += w;
            c->bytes += (long long)w;
            since_report += (long long)w;
        }
        if (since_report >= 1048576) {
            xfer_sendf(c->fd, "B %lld", c->bytes);
            since_report = 0;
        }
    }
    int bad = r < 0;
    close(in);
    close(out);
    if (bad) return -1;
    c->items++;
    xfer_sendf(c->fd, "P %lld %lld", c->items, c->bytes);
    return 0;
}

static int xfer_copy_tree(const char *src, const char *dst, XferCtx *c)
{
    struct stat st;
    if (lstat(src, &st) != 0) return -1;
    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t n = readlink(src, target, sizeof target - 1);
        if (n < 0) return -1;
        target[n] = '\0';
        if (symlink(target, dst) != 0) return -1;
        c->items++;
        xfer_sendf(c->fd, "P %lld %lld", c->items, c->bytes);
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 0777) != 0) return -1;
        c->items++;
        xfer_sendf(c->fd, "P %lld %lld", c->items, c->bytes);
        DIR *d = opendir(src);
        if (!d) return -1;
        struct dirent *de;
        int bad = 0;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char s[PATH_MAX], t[PATH_MAX];
            path_join(s, sizeof s, src, de->d_name);
            path_join(t, sizeof t, dst, de->d_name);
            if (xfer_copy_tree(s, t, c) != 0) bad = 1;
        }
        closedir(d);
        return bad ? -1 : 0;
    }
    return xfer_copy_file(src, dst, c);
}

static int xfer_delete_tree(const char *path, XferCtx *c)
{
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *de;
        int bad = 0;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char child[PATH_MAX];
            path_join(child, sizeof child, path, de->d_name);
            if (xfer_delete_tree(child, c) != 0) bad = 1;
        }
        closedir(d);
        if (rmdir(path) != 0) return -1;
        c->items++;
        xfer_sendf(c->fd, "P %lld %lld", c->items, c->bytes);
        return bad ? -1 : 0;
    }
    if (unlink(path) != 0) return -1;
    c->items++;
    if (S_ISREG(st.st_mode)) c->bytes += (long long)st.st_size;
    xfer_sendf(c->fd, "P %lld %lld", c->items, c->bytes);
    return 0;
}

/* Copy/move worker: length-framed request on stdin, progress on stdout. */
static void xfer_child(int fd, int cut,
                       char (*srcs)[PATH_MAX], char (*dsts)[PATH_MAX], int n)
{
    long long item_bytes[64] = { 0 };
    long long total_items = 0, total_bytes = 0;
    for (int i = 0; i < n; i++) {
        xfer_scan(srcs[i], &total_items, &item_bytes[i]);
        total_bytes += item_bytes[i];
    }
    xfer_sendf(fd, "T %lld %lld", total_items, total_bytes);

    XferCtx ctx = { .fd = fd };
    int done = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        char name[NAME_MAX + 1];
        xfer_name(name, sizeof name, path_base(dsts[i]));
        xfer_sendf(fd, "N %s", name);

        if (cut && rename(srcs[i], dsts[i]) == 0) {
            ctx.items++;
            ctx.bytes += item_bytes[i];
            xfer_sendf(fd, "P %lld %lld", ctx.items, ctx.bytes);
            done++;
            continue;
        }
        if (xfer_copy_tree(srcs[i], dsts[i], &ctx) == 0 &&
            (!cut || xfer_delete_tree(srcs[i], &ctx) == 0)) {
            done++;
        } else {
            failed++;
        }
    }
    xfer_sendf(fd, "D %d %d", done, failed);
    _exit(failed ? 1 : 0);
}

static int trash_one(const char *path)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execlp("gio", "gio", "trash", path, (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Trash/permanent-delete worker. jtwpi uses `gio trash` for the trash case
 * today; the child still reports progress for permanent deletes and moves. */
static void fs_child(int fd, int op, char (*paths)[PATH_MAX], int n)
{
    long long counts[64] = { 0 }, sizes[64] = { 0 };
    long long total_items = 0, total_bytes = 0;
    for (int i = 0; i < n; i++) {
        xfer_scan(paths[i], &counts[i], &sizes[i]);
        total_items += counts[i];
        total_bytes += sizes[i];
    }
    xfer_sendf(fd, "T %lld %lld", total_items, total_bytes);

    XferCtx ctx = { .fd = fd };
    int done = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        char name[NAME_MAX + 1];
        xfer_name(name, sizeof name, path_base(paths[i]));
        xfer_sendf(fd, "N %s", name);
        int ok;
        if (op == XOP_TRASH) {
            ok = trash_one(paths[i]) == 0;
            if (ok) {
                ctx.items += counts[i];
                ctx.bytes += sizes[i];
                xfer_sendf(fd, "P %lld %lld", ctx.items, ctx.bytes);
            }
        } else {
            ok = xfer_delete_tree(paths[i], &ctx) == 0;
        }
        if (ok) done++; else failed++;
    }
    xfer_sendf(fd, "D %d %d", done, failed);
    _exit(failed ? 1 : 0);
}

static int appearance_config_bool(const char *value)
{
    if (!strcasecmp(value, "true")) return 1;
    if (!strcasecmp(value, "false")) return 0;
    return 0; /* off unless explicitly enabled */
}

static void appearance_menu_stop_lap(Menu *m)
{
    if (!m) return;
    m->popup_anim_active = 0;
    m->popup_anim_start = 0.0;
    m->dirty = 1;
    for (int i = 0; i < m->n; i++)
        appearance_menu_stop_lap(m->items[i].sub);
}

/* Read ~/.config/jt/appearance.conf with the same keys/default as jtlabwc:
 * off unless the file says otherwise. */
static void appearance_config_load(App *a)
{
    a->border_anim_enabled = 0;
    const char *home = getenv("HOME");
    if (!home || !*home) return;
    char path[512];
    snprintf(path, sizeof path, "%s/.config/jt/%s", home, APPEARANCE_CONFIG);
    FILE *file = fopen(path, "r");
    if (!file) return;
    char line[256];
    while (fgets(line, sizeof line, file)) {
        char *text = line;
        while (*text == ' ' || *text == '\t') text++;
        char *end = text + strlen(text);
        while (end > text && (end[-1] == '\n' || end[-1] == '\r' ||
                              end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (!*text || *text == '#') continue;
        char *key = "";
        char *value = text;
        char *equals = strchr(text, '=');
        if (equals) {
            *equals = '\0';
            key = text;
            value = equals + 1;
            char *key_end = key + strlen(key);
            while (key_end > key && (key_end[-1] == ' ' ||
                                     key_end[-1] == '\t'))
                *--key_end = '\0';
            while (*value == ' ' || *value == '\t') value++;
        }
        if (!*value) continue;
        if (!*key || !strcmp(key, "accent_border_animation") ||
            !strcmp(key, "jtlabwc_border_anim_enabled"))
            a->border_anim_enabled = appearance_config_bool(value);
    }
    fclose(file);
    if (!a->border_anim_enabled) {
        a->post_sel_anim_start = -1;
        for (int i = 0; i < a->n_outs; i++)
            appearance_menu_stop_lap(a->outs[i].menu);
        all_mark(a);
    }
}

/* ---- accent -------------------------------------------------------------- */

static int accent_parse(const char *s, unsigned *out)
{
    return jti_parse_color(s, out);
}

static void accent_set(App *a, unsigned rgb)
{
    if (a->accent == rgb) return;
    a->accent = rgb;
    jti_palette_set(&a->pal, rgb);
    /* Icon surfaces embed the accent, so drop every cached one. */
    for (int i = 0; i < a->n_outs; i++) {
        Out *o = &a->outs[i];
        for (int k = 0; k < o->n; k++) {
            if (o->icons[k].sf) cairo_surface_destroy(o->icons[k].sf);
            o->icons[k].sf = NULL;
        }
        out_mark(o);
    }
}

static void accent_load_default(App *a)
{
    FILE *f = fopen(ACCENT_CONF, "r");
    if (!f) return;
    char buf[64];
    if (fgets(buf, sizeof buf, f)) {
        trim(buf);
        unsigned rgb;
        if (accent_parse(buf, &rgb)) accent_set(a, rgb);
    }
    fclose(f);
}

/* ---- state --------------------------------------------------------------- */

/* All per-monitor entries live in "monitor-<output key>" sections. Keep the
 * name in one place: save and load must agree or positions are written but
 * never restored. */
static void state_mon_section(const char *key, char *buf, size_t n)
{
    snprintf(buf, n, "monitor-%s", key);
}

typedef struct { char section[160], key[256], value[128]; } StateEnt;
static StateEnt *g_state;
static int g_nstate, g_cstate;

static const char *state_get(const char *sec, const char *key)
{
    for (int i = 0; i < g_nstate; i++)
        if (!strcmp(g_state[i].section, sec) && !strcmp(g_state[i].key, key))
            return g_state[i].value;
    return NULL;
}

static void state_set(const char *sec, const char *key, const char *val)
{
    for (int i = 0; i < g_nstate; i++)
        if (!strcmp(g_state[i].section, sec) && !strcmp(g_state[i].key, key)) {
            snprintf(g_state[i].value, sizeof g_state[i].value, "%s", val);
            return;
        }
    if (g_nstate == g_cstate) {
        g_cstate = g_cstate ? g_cstate * 2 : 128;
        g_state = realloc(g_state, (size_t)g_cstate * sizeof *g_state);
        if (!g_state) exit(1);
    }
    StateEnt *e = &g_state[g_nstate++];
    snprintf(e->section, sizeof e->section, "%s", sec);
    snprintf(e->key, sizeof e->key, "%s", key);
    snprintf(e->value, sizeof e->value, "%s", val);
}

static int state_int(const char *sec, const char *key, int fallback)
{
    const char *v = state_get(sec, key);
    return v ? (int)strtol(v, NULL, 10) : fallback;
}

static int state_bool(const char *sec, const char *key, int fallback)
{
    const char *v = state_get(sec, key);
    if (!v) return fallback;
    return !strcmp(v, "true") || !strcmp(v, "1") || !strcasecmp(v, "yes");
}

static void state_load(App *a)
{
    const char *dir = getenv("XDG_CONFIG_HOME");
    char cfg[PATH_MAX];
    if (dir && *dir) snprintf(cfg, sizeof cfg, "%s/jtwpi", dir);
    else snprintf(cfg, sizeof cfg, "%s/.config/jtwpi",
                  getenv("HOME") ? getenv("HOME") : "/");
    mkdir(cfg, 0755);
    a->state_path = xstrdup(cfg);
    char path[PATH_MAX];
    path_join(path, sizeof path, cfg, "state.ini");

    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512], sec[160] = "";
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!*line || *line == '#' || *line == ';') continue;
        if (*line == '[') {
            char *e = strchr(line, ']');
            if (e) { *e = 0; snprintf(sec, sizeof sec, "%s", line + 1); }
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        state_set(sec, k, v);
    }
    fclose(f);
}

static void state_save(App *a)
{
    if (!a->state_path) return;
    /* Never persist an in-flight drag. Icon coordinates become final only
     * on drop, which clears the dragging flag before calling state_save(). */
    for (int i = 0; i < a->n_outs; i++)
        if (a->outs[i].dragging) return;
    char path[PATH_MAX];
    path_join(path, sizeof path, a->state_path, "state.ini");
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;

    char v[64], sec[200];
    const char *show = a->show_icons ? "true" : "false";
    const char *snap = a->snap ? "true" : "false";
    fprintf(f, "[general]\n");
    fprintf(f, "show_icons=%s\n", show);
    fprintf(f, "snap=%s\n", snap);
    fprintf(f, "icon_size=%d\n", a->icon_size);
    fprintf(f, "pos_pixels=true\n");

    /* Mirror every write into the in-memory state table as well: refresh and
     * directory-change reloads read positions from there, so leaving it stale
     * makes moved icons jump back to their old cells. */
    state_set("general", "show_icons", show);
    state_set("general", "snap", snap);
    snprintf(v, sizeof v, "%d", a->icon_size);
    state_set("general", "icon_size", v);
    state_set("general", "pos_pixels", "true");

    for (int i = 0; i < a->n_outs; i++) {
        Out *o = &a->outs[i];
        if (!o->key[0]) continue;
        state_mon_section(o->key, sec, sizeof sec);
        fprintf(f, "\n[%s]\n", sec);
        for (int k = 0; k < o->n; k++) {
            DIcon *ic = &o->icons[k];
            int x = ic->x, y = ic->y;
            if (!ic->auto_placed && ic->want_x >= 0 && ic->want_y >= 0) {
                x = ic->want_x; y = ic->want_y;
            }
            if (x < 0 || y < 0) continue;
            snprintf(v, sizeof v, "%d,%d,%d", x, y, ic->auto_placed ? 1 : 0);
            fprintf(f, "%s=%s\n", ic->key, v);
            state_set(sec, ic->key, v);
        }
    }
    fclose(f);
    rename(tmp, path);
}

/* ---- desktop directory and entries -------------------------------------- */

static char *desktop_directory(App *a)
{
    if (a->opt_desktop && *a->opt_desktop)
        return expand_home(a->opt_desktop);

    const char *cfgdir = getenv("XDG_CONFIG_HOME");
    char cfg[PATH_MAX];
    if (cfgdir && *cfgdir) snprintf(cfg, sizeof cfg, "%s/user-dirs.dirs", cfgdir);
    else snprintf(cfg, sizeof cfg, "%s/.config/user-dirs.dirs",
                  getenv("HOME") ? getenv("HOME") : "/");
    FILE *f = fopen(cfg, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            trim(line);
            if (strncmp(line, "XDG_DESKTOP_DIR=", 16)) continue;
            char *v = line + 16;
            trim(v);
            if (*v == '"') {
                char *e = strrchr(v, '"');
                if (e && e > v) *e = 0;
                v++;
            }
            char *out = NULL;
            if (!strncmp(v, "$HOME/", 6))
                out = expand_home(v + 5);
            else if (!strcmp(v, "$HOME"))
                out = xstrdup(getenv("HOME") ? getenv("HOME") : "/");
            else if (*v == '/')
                out = xstrdup(v);
            if (out) { fclose(f); return out; }
        }
        fclose(f);
    }
    return expand_home("~/Desktop");
}

static void parse_desktop_file(DIcon *ic)
{
    FILE *f = fopen(ic->path, "r");
    if (!f) return;
    char line[1024], group[64] = "";
    int in_entry = 0;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (*line == '[') {
            char *e = strchr(line, ']');
            if (e) *e = 0;
            snprintf(group, sizeof group, "%s", line + 1);
            in_entry = !strcmp(group, "Desktop Entry");
            continue;
        }
        if (!in_entry || !*line || *line == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        if (!strcmp(k, "Name") && !ic->name) ic->name = xstrdup(v);
        else if (!strcmp(k, "Icon") && !ic->icon) ic->icon = xstrdup(v);
        else if (!strcmp(k, "Exec") && !ic->exec) ic->exec = xstrdup(v);
        else if ((!strcmp(k, "NoDisplay") || !strcmp(k, "Hidden")) &&
                 !strcasecmp(v, "true")) {
            ic->is_desktop = -1; /* hidden */
        }
    }
    fclose(f);
}

static const char *icon_for_path(const DIcon *ic)
{
    if (ic->is_dir) return "folder";
    if (ic->is_desktop) {
        if (ic->icon && *ic->icon) return ic->icon;
        return "application-x-desktop";
    }
    const char *b = path_base(ic->path);
    const char *dot = strrchr(b, '.');
    if (dot) {
        const char *e = dot + 1;
        if (!strcasecmp(e, "png") || !strcasecmp(e, "jpg") ||
            !strcasecmp(e, "jpeg") || !strcasecmp(e, "gif") ||
            !strcasecmp(e, "webp") || !strcasecmp(e, "svg") ||
            !strcasecmp(e, "bmp")) return "image-x-generic";
        if (!strcasecmp(e, "mp4") || !strcasecmp(e, "mkv") ||
            !strcasecmp(e, "webm") || !strcasecmp(e, "avi") ||
            !strcasecmp(e, "mov")) return "video-x-generic";
        if (!strcasecmp(e, "mp3") || !strcasecmp(e, "wav") ||
            !strcasecmp(e, "ogg") || !strcasecmp(e, "flac") ||
            !strcasecmp(e, "m4a")) return "audio-x-generic";
        if (!strcasecmp(e, "pdf")) return "application-pdf";
        /* Same disc image glyph jtfiles uses for .iso files. */
        if (!strcasecmp(e, "iso")) return "application-x-cd-image";
        if (!strcasecmp(e, "zip") || !strcasecmp(e, "tar") ||
            !strcasecmp(e, "gz") || !strcasecmp(e, "xz") ||
            !strcasecmp(e, "7z") || !strcasecmp(e, "rar")) return "archive-zip";
        if (!strcasecmp(e, "html") || !strcasecmp(e, "htm")) return "text-html";
        if (!strcasecmp(e, "c") || !strcasecmp(e, "h") ||
            !strcasecmp(e, "py") || !strcasecmp(e, "js") ||
            !strcasecmp(e, "rs") || !strcasecmp(e, "sh")) return "text-x-script";
        if (!strcasecmp(e, "txt") || !strcasecmp(e, "md")) return "text-x-generic";
    }
    struct stat st;
    if (stat(ic->path, &st) == 0 && (st.st_mode & S_IXUSR) && S_ISREG(st.st_mode))
        return "application-x-executable";
    return "text-x-generic";
}

static void icon_free(DIcon *ic)
{
    free(ic->path); free(ic->key); free(ic->name);
    free(ic->icon); free(ic->exec);
    if (ic->sf) cairo_surface_destroy(ic->sf);
    if (ic->label_sf) cairo_surface_destroy(ic->label_sf);
    memset(ic, 0, sizeof *ic);
}

static int icon_cmp(const void *A, const void *B)
{
    const DIcon *a = A, *b = B;
    return strcasecmp(a->name ? a->name : "", b->name ? b->name : "");
}

static void state_get_position(Out *o, DIcon *ic, int *x, int *y, int *aut)
{
    char sec[160];
    state_mon_section(o->key, sec, sizeof sec);
    const char *v = state_get(sec, ic->key);
    *x = *y = -1; *aut = 1;
    if (!v) return;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", v);
    *x = (int)strtol(buf, NULL, 10);
    char *c = strchr(buf, ',');
    if (!c) { *x = -1; return; }
    *y = (int)strtol(c + 1, NULL, 10);
    c = strchr(c + 1, ',');
    if (c) *aut = (int)strtol(c + 1, NULL, 10) != 0;
    if (*x < 0 || *y < 0) *x = *y = -1;
}

/* preserve=1 keeps existing on-screen positions (including auto-placed ones)
 * when re-reading the Desktop directory.  Refresh, directory-change reloads
 * and file operations use it so an icon never shuffles unless the user asked
 * for Arrange.  New files still get the first free cell. */
static void out_load_icons(Out *o, int preserve)
{
    for (int i = 0; i < o->n; i++) icon_free(&o->icons[i]);
    o->n = 0;
    o->hover = o->press = -1;

    DIR *d = opendir(g_app->desktop_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && o->n < MAX_ICONS) {
        if (de->d_name[0] == '.') continue;
        char path[PATH_MAX];
        path_join(path, sizeof path, g_app->desktop_dir, de->d_name);
        struct stat st;
        if (lstat(path, &st) != 0) continue;

        DIcon *ic = &o->icons[o->n];
        memset(ic, 0, sizeof *ic);
        ic->path = xstrdup(path);
        ic->key = xstrdup(de->d_name);
        ic->is_dir = S_ISDIR(st.st_mode) != 0;
        ic->size = st.st_size;
        ic->mtime = st.st_mtime;
        ic->x = ic->y = ic->want_x = ic->want_y = -1;
        ic->auto_placed = 1;

        size_t len = strlen(de->d_name);
        if (len > 8 && !strcasecmp(de->d_name + len - 8, ".desktop")) {
            ic->is_desktop = 1;
            parse_desktop_file(ic);
            if (ic->is_desktop < 0) { icon_free(ic); continue; }
        }
        if (!ic->name) ic->name = xstrdup(de->d_name);
        ic->icon = xstrdup(icon_for_path(ic));
        o->n++;
    }
    closedir(d);
    qsort(o->icons, (size_t)o->n, sizeof *o->icons, icon_cmp);

    /* Older state.ini files stored grid indices; convert them to pixels
     * once, then write the pos_pixels marker so this runs only once. */
    int pixels = state_bool("general", "pos_pixels", 0);
    int cw = cell_w(), ch = cell_h();
    for (int i = 0; i < o->n; i++) {
        DIcon *ic = &o->icons[i];
        int x, y, aut;
        state_get_position(o, ic, &x, &y, &aut);
        if (x >= 0 && y >= 0) {
            if (!pixels) { x = x * cw + GRID_MARGIN; y = y * ch + GRID_MARGIN; }
            ic->x = x; ic->y = y;
            ic->auto_placed = aut;
            ic->want_x = aut ? -1 : x;
            ic->want_y = aut ? -1 : y;
        }
    }
    /* de-duplicate overlapping entries */
    for (int i = 0; i < o->n; i++)
        for (int j = 0; j < i; j++)
            if (o->icons[i].x >= 0 && o->icons[j].x >= 0 &&
                o->icons[i].x == o->icons[j].x &&
                o->icons[i].y == o->icons[j].y) {
                o->icons[i].x = o->icons[i].y = -1;
                o->icons[i].want_x = o->icons[i].want_y = -1;
                o->icons[i].auto_placed = 1;
                break;
            }
    out_relayout(o, !preserve);
    post_sel_apply(o);
}

/* ---- geometry / layout --------------------------------------------------- */
/*
 * Icon positions are stored as pixels (the top-left corner of the icon's
 * grid cell).  When "Snap to Grid" is enabled, positions are kept aligned to
 * the grid by snap_xy(); when it is disabled icons may sit anywhere.  This
 * matches the old GTK behaviour and is what makes free placement possible.
 */

static int cell_w(void) { return g_app->icon_size + 2 * CELL_PAD_X; }
static int cell_h(void)
{
    return CELL_PAD_TOP + g_app->icon_size + CELL_LABEL_GAP +
           LABEL_HEIGHT + CELL_PAD_BOTTOM;
}

/* Round a pixel position to the nearest grid point, but only when snap is on. */
static void snap_xy(int *x, int *y)
{
    if (!g_app->snap) return;
    int cw = cell_w(), ch = cell_h();
    double sx = (double)lround((*x - GRID_MARGIN) / (double)cw) * cw + GRID_MARGIN;
    double sy = (double)lround((*y - GRID_MARGIN) / (double)ch) * ch + GRID_MARGIN;
    if (sx > INT_MAX) sx = INT_MAX; else if (sx < INT_MIN) sx = INT_MIN;
    if (sy > INT_MAX) sy = INT_MAX; else if (sy < INT_MIN) sy = INT_MIN;
    *x = (int)sx; *y = (int)sy;
}

/* Usable pixel area for the top-left corner of an icon cell. */
static void grid_bounds(Out *o, int *minx, int *miny, int *maxx, int *maxy)
{
    int cw = cell_w(), ch = cell_h();
    *minx = GRID_MARGIN;
    *miny = GRID_MARGIN;
    *maxx = o->width - cw - GRID_MARGIN;
    *maxy = o->height - ch - BOTTOM_MARGIN;
    if (*maxx < *minx) *maxx = *minx;
    if (*maxy < *miny) *maxy = *miny;
}

/* True when a cell-sized box at (x,y) overlaps any other icon. */
static int cell_occupied(Out *o, int x, int y, DIcon *except)
{
    int cw = cell_w(), ch = cell_h();
    for (int i = 0; i < o->n; i++) {
        DIcon *ic = &o->icons[i];
        if (ic == except || ic->x < 0 || ic->y < 0) continue;
        if (x < ic->x + cw && x + cw > ic->x &&
            y < ic->y + ch && y + ch > ic->y) return 1;
    }
    return 0;
}

static void clamp_icon(Out *o, DIcon *ic)
{
    int minx, miny, maxx, maxy;
    grid_bounds(o, &minx, &miny, &maxx, &maxy);
    /* When snapping, keep the extreme positions on the grid too. */
    if (g_app->snap) {
        int cw = cell_w(), ch = cell_h();
        maxx = minx + ((maxx - minx) / cw) * cw;
        maxy = miny + ((maxy - miny) / ch) * ch;
    }
    if (ic->x < minx) ic->x = minx;
    if (ic->y < miny) ic->y = miny;
    if (ic->x > maxx) ic->x = maxx;
    if (ic->y > maxy) ic->y = maxy;
}

/* Find the first free grid cell (column-major, like the old GTK version).
 * Out parameters are pixel coordinates. */
static void find_free_cell(Out *o, int *fx, int *fy, DIcon *except)
{
    int minx, miny, maxx, maxy;
    grid_bounds(o, &minx, &miny, &maxx, &maxy);
    int cw = cell_w(), ch = cell_h();
    for (int x = minx; x <= maxx; x += cw)
        for (int y = miny; y <= maxy; y += ch)
            if (!cell_occupied(o, x, y, except)) { *fx = x; *fy = y; return; }
    *fx = minx; *fy = miny;
}

/* Nudge overlapping icons into the nearest free cell. */
static void resolve_overlaps(Out *o, int update_want)
{
    int cw = cell_w(), ch = cell_h();
    for (int i = 0; i < o->n; i++) {
        DIcon *ic = &o->icons[i];
        if (ic->x < 0 || ic->y < 0) continue;
        int clash = 0;
        for (int j = 0; j < o->n; j++) {
            if (i == j) continue;
            DIcon *ot = &o->icons[j];
            if (ot->x < 0 || ot->y < 0) continue;
            if (ic->x < ot->x + cw && ic->x + cw > ot->x &&
                ic->y < ot->y + ch && ic->y + ch > ot->y) { clash = 1; break; }
        }
        if (!clash) continue;
        int x = ic->x, y = ic->y;
        if (g_app->snap) snap_xy(&x, &y);
        /* Search outward from the current position for a free spot. */
        int ox = x, oy = y;
        int minx, miny, maxx, maxy;
        grid_bounds(o, &minx, &miny, &maxx, &maxy);
        int found = 0;
        for (int ring = 1; ring <= 16 && !found; ring++) {
            for (int dcol = -ring; dcol <= ring && !found; dcol++) {
                for (int drow = -ring; drow <= ring; drow++) {
                    if (abs(dcol) != ring && abs(drow) != ring) continue;
                    int tx = ox + dcol * cw, ty = oy + drow * ch;
                    if (tx < minx || ty < miny || tx > maxx || ty > maxy) continue;
                    if (!cell_occupied(o, tx, ty, ic)) {
                        x = tx; y = ty; found = 1; break;
                    }
                }
            }
        }
        if (!found) {
            for (int ty = miny; ty <= maxy; ty += ch)
                for (int tx = minx; tx <= maxx; tx += cw)
                    if (!cell_occupied(o, tx, ty, ic)) {
                        x = tx; y = ty; found = 1; break;
                    }
            if (!found) continue;
        }
        ic->x = x; ic->y = y;
        if (update_want) { ic->want_x = x; ic->want_y = y; }
        clamp_icon(o, ic);
    }
}

static void out_relayout(Out *o, int reflow_auto)
{
    if (o->width <= 0 || o->height <= 0) return;
    for (int i = 0; i < o->n; i++) {
        DIcon *ic = &o->icons[i];
        if (ic->x < 0 || ic->y < 0) {
            ic->auto_placed = 1;
            ic->want_x = ic->want_y = -1;
        } else if (ic->auto_placed) {
            if (reflow_auto) {
                ic->x = ic->y = -1;
                ic->want_x = ic->want_y = -1;
            } else {
                clamp_icon(o, ic);
            }
        } else {
            /* Snap the intended position, not the clamped on-screen one. */
            int bx = ic->want_x >= 0 ? ic->want_x : ic->x;
            int by = ic->want_y >= 0 ? ic->want_y : ic->y;
            snap_xy(&bx, &by);
            ic->want_x = bx; ic->want_y = by;
            ic->x = bx; ic->y = by;
            clamp_icon(o, ic);
        }
    }
    /* Icons without a position get the first free cell. */
    for (int i = 0; i < o->n; i++) {
        DIcon *ic = &o->icons[i];
        if (ic->x < 0 || ic->y < 0) {
            int x, y;
            find_free_cell(o, &x, &y, ic);
            ic->x = x; ic->y = y;
            ic->auto_placed = 1;
        }
    }
    resolve_overlaps(o, 0);
    out_mark(o);
}

/* Put newly created icons around a desktop point.  Used by external drops
 * and by paste, so both land where the user pointed instead of taking the
 * first free cell.  A group starts in one cell; resolve_overlaps() then
 * spreads it into nearby cells instead of stacking the icons. */
static void place_new_icons(App *a, Out *dst,
                            char (*names)[NAME_MAX + 1], int n,
                            double px, double py)
{
    if (!dst || dst->width <= 0 || dst->height <= 0 || n <= 0) return;
    int cw = cell_w(), ch = cell_h();
    int base_x = (int)lround(px - cw / 2.0);
    int base_y = (int)lround(py - ch / 2.0);
    DIcon *placed[64];
    int np = 0;
    for (int i = 0; i < dst->n && np < n; i++) {
        for (int k = 0; k < n; k++) {
            if (strcmp(dst->icons[i].key, names[k])) continue;
            placed[np++] = &dst->icons[i];
            break;
        }
    }
    for (int i = 0; i < np; i++) {
        DIcon *ic = placed[i];
        ic->x = base_x; ic->y = base_y;
        ic->want_x = base_x; ic->want_y = base_y;
        ic->auto_placed = 0;
        snap_xy(&ic->x, &ic->y);
        snap_xy(&ic->want_x, &ic->want_y);
        clamp_icon(dst, ic);
        ic->want_x = ic->x; ic->want_y = ic->y;
    }
    resolve_overlaps(dst, 1);
    state_save(a);
    for (int i = 0; i < a->n_outs; i++) out_mark(&a->outs[i]);
}

static DIcon *out_hit(Out *o, double mx, double my)
{
    if (!g_app->show_icons) return NULL;
    int cw = cell_w(), ch = cell_h();
    for (int i = o->n - 1; i >= 0; i--) {
        DIcon *ic = &o->icons[i];
        if (ic->x < 0 || ic->y < 0) continue;
        if (mx >= ic->x && mx < ic->x + cw &&
            my >= ic->y && my < ic->y + ch) return ic;
    }
    return NULL;
}

static void select_in_rect(Out *o, double x0, double y0, double x1, double y1,
                           int add)
{
    post_sel_disarm(o->app);
    double x = x0 < x1 ? x0 : x1, y = y0 < y1 ? y0 : y1;
    double w = fabs(x1 - x0), h = fabs(y1 - y0);
    int cw = cell_w(), ch = cell_h();
    for (int i = 0; i < o->n; i++) {
        DIcon *ic = &o->icons[i];
        if (ic->x < 0 || ic->y < 0) continue;
        if (ic->x < x + w && ic->x + cw > x &&
            ic->y < y + h && ic->y + ch > y)
            ic->selected = 1;
        else if (!add) ic->selected = 0;
    }
}

/* ---- icon rendering ------------------------------------------------------ */

static void icon_surface_for(App *a, DIcon *ic)
{
    int px = a->icon_size;
    if (ic->sf && ic->sf_px == px && ic->sf_accent == a->accent) return;
    if (ic->sf) cairo_surface_destroy(ic->sf);
    ic->sf = jti_icon_surface(ic->icon, px, &a->pal, 1, ic->name);
    ic->sf_px = px;
    ic->sf_accent = a->accent;
}

/* Render the icon label once into a cached surface.  Rebuilding a
 * PangoLayout for every icon on every pointer-motion frame was the main
 * source of drag lag; a cached surface makes drawing just a composite. */
static void label_surface_for(App *a, DIcon *ic)
{
    int cw = cell_w();
    int lw = cw - 10, lh = LABEL_HEIGHT;
    const char *name = ic->name ? ic->name : "";
    if (ic->label_sf && ic->label_px == a->icon_size &&
        !strncmp(ic->label_name, name, sizeof ic->label_name))
        return;
    if (ic->label_sf) { cairo_surface_destroy(ic->label_sf); ic->label_sf = NULL; }
    if (lw <= 0 || lh <= 0) return;

    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, lw, lh);
    if (cairo_surface_status(sf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(sf); return;
    }
    cairo_t *lcr = cairo_create(sf);
    PangoLayout *l = pango_cairo_create_layout(lcr);
    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, "Sans");
    pango_font_description_set_absolute_size(fd, 11 * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_width(l, lw * PANGO_SCALE);
    pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    pango_layout_set_height(l, -2);
    pango_layout_set_text(l, name, -1);
    cairo_set_source_rgba(lcr, 1, 1, 1, 1);
    cairo_move_to(lcr, 0, 0);
    pango_cairo_show_layout(lcr, l);
    g_object_unref(l);
    cairo_destroy(lcr);

    ic->label_sf = sf;
    ic->label_px = a->icon_size;
    snprintf(ic->label_name, sizeof ic->label_name, "%s", name);
}

static void draw_icon(App *a, cairo_t *cr, Out *o, DIcon *ic,
                      double dx, double dy)
{
    int cw = cell_w(), ch = cell_h();
    double x = ic->x + dx, y = ic->y + dy;

    if (ic->selected) {
        cairo_set_source_rgba(cr, JTI_R(a->pal.accent), JTI_G(a->pal.accent),
                              JTI_B(a->pal.accent), 0.34);
        jti_rounded_rect(cr, x + 2, y + 2, cw - 4, ch - 4, 7);
        cairo_fill(cr);
        /* Keep the fill, but let the travelling segment stand in for the
         * static border until the lap finishes. */
        if (!post_sel_icon_pending(a, ic)) {
            cairo_set_source_rgba(cr, JTI_R(a->pal.accent_hi),
                                  JTI_G(a->pal.accent_hi), JTI_B(a->pal.accent_hi), 0.95);
            cairo_set_line_width(cr, 1.2);
            jti_rounded_rect(cr, x + 2, y + 2, cw - 4, ch - 4, 7);
            cairo_stroke(cr);
        }
    } else if (ic->hover) {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.11);
        jti_rounded_rect(cr, x + 2, y + 2, cw - 4, ch - 4, 7);
        cairo_fill(cr);
    }

    icon_surface_for(a, ic);
    double ix = x + (cw - a->icon_size) / 2.0;  /* center icon in the cell */
    double iy = y + CELL_PAD_TOP;               /* same top as the old GTK version */
    if (ic->sf) {
        cairo_save(cr);
        cairo_pattern_t *pat = cairo_pattern_create_for_surface(ic->sf);
        cairo_matrix_t m;
        double sc = (double)ic->sf_px / (double)a->icon_size;
        cairo_matrix_init_scale(&m, sc, sc);
        cairo_matrix_translate(&m, -ix, -iy);
        cairo_pattern_set_matrix(pat, &m);
        cairo_set_source(cr, pat);
        cairo_rectangle(cr, ix, iy, a->icon_size, a->icon_size);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);
        cairo_restore(cr);
    }

    /* cached label, horizontally centered to match the icon */
    label_surface_for(a, ic);
    if (ic->label_sf) {
        double ly = y + CELL_PAD_TOP + a->icon_size + CELL_LABEL_GAP;
        cairo_set_source_surface(cr, ic->label_sf, x + 5, ly);
        cairo_rectangle(cr, x + 5, ly, cw - 10, LABEL_HEIGHT);
        cairo_fill(cr);
    }
}

/* Darken the wallpaper toward the edges of each output.  The sprite is a
 * square radial gradient: stretching it to the output's aspect ratio turns it
 * into an ellipse, so the corners are the darkest part of the vignette. */
static cairo_surface_t *vignette_sprite(void)
{
    static cairo_surface_t *sprite;
    if (sprite) return sprite;

    sprite = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                        VIGNETTE_SIZE, VIGNETTE_SIZE);
    if (cairo_surface_status(sprite) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(sprite);
        sprite = NULL;
        return NULL;
    }

    double c = VIGNETTE_SIZE / 2.0;
    double diag = c * sqrt(2.0);
    double edge = 1.0 / sqrt(2.0); /* where the gradient meets a screen edge */
    cairo_t *cr = cairo_create(sprite);
    cairo_pattern_t *p = cairo_pattern_create_radial(c, c, 0.0, c, c, diag);
    cairo_pattern_add_color_stop_rgba(p, 0.0, 0, 0, 0, 0.0);
    cairo_pattern_add_color_stop_rgba(p, 0.45, 0, 0, 0, 0.0);
    cairo_pattern_add_color_stop_rgba(p, edge, 0, 0, 0, 0.30);
    cairo_pattern_add_color_stop_rgba(p, 0.88, 0, 0, 0, 0.50);
    cairo_pattern_add_color_stop_rgba(p, 1.0, 0, 0, 0, 0.66);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
    cairo_destroy(cr);
    cairo_surface_flush(sprite);
    return sprite;
}

static void paint_vignette(cairo_t *cr, int w, int h)
{
    cairo_surface_t *sprite = vignette_sprite();
    if (!sprite || w <= 0 || h <= 0) return;

    cairo_pattern_t *pat = cairo_pattern_create_for_surface(sprite);
    cairo_matrix_t m;
    cairo_matrix_init_scale(&m, (double)VIGNETTE_SIZE / w,
                            (double)VIGNETTE_SIZE / h);
    cairo_pattern_set_matrix(pat, &m); /* user pixels -> sprite pixels */
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);
}

/* The post-drop selection batch is identified by name, so a rescan during
 * the lap cannot leave us drawing around a stale icon pointer. */
static int post_sel_icon_pending(const App *a, const DIcon *ic)
{
    if (!a || !a->border_anim_enabled || !a->post_armed || !ic || !ic->key)
        return 0;
    if (a->post_sel_anim_start <= 0.0) return 0;
    for (int k = 0; k < a->n_post_names; k++)
        if (!strcmp(ic->key, a->post_names[k])) return 1;
    return 0;
}

static double post_sel_elapsed_ms(const App *a)
{
    if (!a || a->post_sel_anim_start <= 0.0) return SEL_LAP_MS;
    return (now_sec() - a->post_sel_anim_start) * 1000.0;
}

static int post_sel_anim_active(const App *a)
{
    return a && a->border_anim_enabled &&
           post_sel_elapsed_ms(a) < SEL_LAP_MS;
}

/* One accent segment runs clockwise from the bottom centre, all the way
 * around a rounded rectangle, then fades out as the border appears.  A dashed
 * stroke of the same path the border uses keeps the segment aligned through
 * the arcs without hand-rolling arc/line interpolation. */
static void draw_lap_rect(App *a, cairo_t *cr, double x, double y,
                          double w, double h, double r, double progress)
{
    double top = w - 2.0 * r, side = h - 2.0 * r;
    double perimeter = 2.0 * (top + side) + 2.0 * PI * r;
    if (perimeter <= 2.0) return;

    /* jti_rounded_rect() starts just past the top-left radius and goes
     * clockwise.  This is the distance from there to the bottom centre. */
    double start = 1.5 * top + side + PI * r;
    double travelled = progress * perimeter;
    double seg = MIN(SEL_LAP_MAX_SEG,
                     MAX(SEL_LAP_MIN_SEG, perimeter / 10.0));
    seg = MIN(seg, perimeter / 2.0);

    double alpha = 1.0;
    if (progress < 0.08) alpha = progress / 0.08;
    else if (progress > 0.92) alpha = (1.0 - progress) / 0.08;
    if (alpha <= 0.0) return;

    cairo_save(cr);
    double dash[2] = { seg, perimeter - seg };
    cairo_set_dash(cr, dash, 2, -fmod(start + travelled, perimeter));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    /* Soft halo under the crisp segment, as on jtlabwc's focus animation.
     * cairo_stroke() clears the path, so rebuild it for the second stroke. */
    jti_rounded_rect(cr, x, y, w, h, r);
    cairo_set_line_width(cr, SEL_LAP_WIDTH + 5.0);
    cairo_set_source_rgba(cr, JTI_R(a->pal.accent_hi), JTI_G(a->pal.accent_hi),
                          JTI_B(a->pal.accent_hi), alpha * 0.18);
    cairo_stroke(cr);

    jti_rounded_rect(cr, x, y, w, h, r);
    cairo_set_line_width(cr, SEL_LAP_WIDTH);
    cairo_set_source_rgba(cr, JTI_R(a->pal.accent_hi), JTI_G(a->pal.accent_hi),
                          JTI_B(a->pal.accent_hi), alpha);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void draw_sel_lap(App *a, cairo_t *cr, DIcon *ic, double progress)
{
    int cw = cell_w(), ch = cell_h();
    draw_lap_rect(a, cr, ic->x + 2.0, ic->y + 2.0,
                  cw - 4.0, ch - 4.0,
                  MIN(7.0, MIN(cw - 4.0, ch - 4.0) / 2.0), progress);
}

/* Menu popups use the same timing for a lap around their own border. */
static double menu_popup_elapsed_ms(const Menu *m)
{
    if (!m || m->popup_anim_start <= 0.0) return SEL_LAP_MS;
    return (now_sec() - m->popup_anim_start) * 1000.0;
}

static int menu_popup_anim_active(const Menu *m)
{
    return m && m->app && m->app->border_anim_enabled &&
           m->popup_anim_active && menu_popup_elapsed_ms(m) < SEL_LAP_MS;
}

static int menu_tree_anim_active(const Menu *m)
{
    if (!m) return 0;
    if (menu_popup_anim_active(m)) return 1;
    for (int i = 0; i < m->n; i++)
        if (menu_tree_anim_active(m->items[i].sub)) return 1;
    return 0;
}

static void menu_tree_tick(Menu *m, double now)
{
    if (!m) return;
    if (m->popup_anim_active && m->popup_anim_start > 0.0) {
        if ((now - m->popup_anim_start) * 1000.0 >= SEL_LAP_MS) {
            m->popup_anim_active = 0;
            m->popup_anim_start = 0.0;
        }
        /* Keep repainting while the segment moves, and once more for the
         * clean static border after it finishes. */
        m->dirty = 1;
    }
    for (int i = 0; i < m->n; i++) menu_tree_tick(m->items[i].sub, now);
}

static void paint_out(cairo_t *cr, int w, int h, void *data)
{
    Out *o = data;
    App *a = o->app;
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    paint_vignette(cr, w, h);

    if (a->show_icons) {
        for (int i = 0; i < o->n; i++) {
            DIcon *ic = &o->icons[i];
            if (o->dragging && ic->selected) continue;
            draw_icon(a, cr, o, ic, 0, 0);
        }
        if (o->dragging)
            for (int i = 0; i < o->n; i++)
                if (o->icons[i].selected)
                    draw_icon(a, cr, o, &o->icons[i], o->drag_dx, o->drag_dy);
    }

    /* Draw the lap over the selected icon's existing highlight background. */
    if (post_sel_anim_active(a) && a->show_icons) {
        double progress = post_sel_elapsed_ms(a) / SEL_LAP_MS;
        for (int i = 0; i < o->n; i++) {
            DIcon *ic = &o->icons[i];
            if (ic->selected && post_sel_icon_pending(a, ic))
                draw_sel_lap(a, cr, ic, progress);
        }
    }

    if (o->selecting) {
        double x = MIN(o->sel_x0, o->sel_x1), y = MIN(o->sel_y0, o->sel_y1);
        double ww = fabs(o->sel_x1 - o->sel_x0);
        double hh = fabs(o->sel_y1 - o->sel_y0);
        cairo_set_source_rgba(cr, JTI_R(a->pal.accent), JTI_G(a->pal.accent),
                              JTI_B(a->pal.accent), 0.16);
        cairo_rectangle(cr, x, y, ww, hh);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, JTI_R(a->pal.accent_hi),
                              JTI_G(a->pal.accent_hi), JTI_B(a->pal.accent_hi), 0.95);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, x, y, ww, hh);
        cairo_stroke(cr);
    }

    if (a->status[0] && a->status_until > time(NULL)) {
        jti_text_draw(cr, a->status, 11, 500, a->pal.fg, 0.85,
                      16, h - 16, JTI_ALIGN_LEFT, 0.0);
    }
}

static void out_mark(Out *o) { if (o) o->dirty = 1; }

/* Clear both halves of hover state.  Resetting only o->hover (as pointer_leave
 * used to) leaves the per-icon flag set, so the icon keeps its highlight after
 * a drag when pointer motion is suppressed. */
static void out_clear_hover(Out *o)
{
    if (!o) return;
    for (int i = 0; i < o->n; i++) o->icons[i].hover = 0;
    o->hover = -1;
    out_mark(o);
}

static void all_mark(App *a)
{
    for (int i = 0; i < a->n_outs; i++) out_mark(&a->outs[i]);
}

/* ---- menu ---------------------------------------------------------------- */

static MenuItem *mitem(Menu *m, MenuKind kind, int id, const char *label,
                       const char *shortcut, const char *icon, int enabled)
{
    if (m->n >= MAX_MENU_ITEMS) return NULL;
    MenuItem *it = &m->items[m->n++];
    memset(it, 0, sizeof *it);
    it->kind = kind; it->id = id; it->enabled = enabled;
    snprintf(it->label, sizeof it->label, "%s", label ? label : "");
    snprintf(it->shortcut, sizeof it->shortcut, "%s", shortcut ? shortcut : "");
    snprintf(it->icon, sizeof it->icon, "%s", icon ? icon : "");
    return it;
}

static void msep(Menu *m) { mitem(m, MI_SEP, 0, NULL, NULL, NULL, 1); }

static void menu_measure(App *a, Menu *m)
{
    cairo_surface_t *ms = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t *mc = cairo_create(ms);
    double w = 160.0;
    for (int i = 0; i < m->n; i++) {
        MenuItem *it = &m->items[i];
        if (it->kind == MI_SEP) continue;
        double x = 14.0 + jti_text_width(mc, it->label, 13, 400, 0.0);
        if (it->icon[0]) x += 22.0;
        if (it->kind == MI_SUB) x += 22.0;
        else if (it->shortcut[0])
            x += 14.0 + jti_text_width(mc, it->shortcut, 11, 400, 0.0);
        if (x > w) w = x;
    }
    cairo_destroy(mc);
    cairo_surface_destroy(ms);
    m->w = (int)ceil(w + 14.0);
    m->h = 10;
    for (int i = 0; i < m->n; i++)
        m->h += m->items[i].kind == MI_SEP ? 9 : 26;
    (void)a;
}

/* Y offset of item idx relative to the menu's top edge. */
static int menu_item_offset_y(Menu *m, int idx)
{
    int y = 5;
    for (int i = 0; i < idx && i < m->n; i++)
        y += m->items[i].kind == MI_SEP ? 9 : 26;
    return y;
}

static int menu_hit(Menu *m, double mx, double my)
{
    /* Hitboxes match the drawn rows: 5px inset, no vertical overlap. */
    if (mx < MENU_INSET || my < MENU_INSET ||
        mx > m->w - MENU_INSET || my > m->h - MENU_INSET) return -1;
    int cy = MENU_INSET;
    for (int i = 0; i < m->n; i++) {
        int hh = m->items[i].kind == MI_SEP ? 9 : 26;
        if (my >= cy && my < cy + hh) return i;
        cy += hh;
    }
    return -1;
}

/* Same nested-popup behavior as jtlabwc: moving to another row closes the
 * current submenu, and hovering a submenu row opens it on the row anchor. */
static void menu_pointer_update(Menu *m, double mx, double my)
{
    int hit = menu_hit(m, mx, my);
    if (hit == m->hover) return;
    if (m->sub) menu_close(m->sub);
    m->hover = hit;
    if (hit >= 0 && m->items[hit].kind == MI_SUB && m->items[hit].enabled) {
        menu_open_size_sub(m->out, m, hit,
                           m->w - MENU_INSET, menu_item_offset_y(m, hit));
    }
    m->dirty = 1;
}

static void paint_menu(cairo_t *cr, int w, int h, void *data)
{
    (void)w; (void)h;
    Menu *m = data;
    App *a = m->app;
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    double popup_ms = menu_popup_elapsed_ms(m);
    int popup_lap = menu_popup_anim_active(m);
    double popup_progress = popup_lap ? popup_ms / SEL_LAP_MS : 1.0;

    /* Clip the offset shadow to the panel path: otherwise it fills the
     * transparent wedges outside the bottom rounded corners, which show up
     * as black artifacts under the popup. */
    cairo_save(cr);
    jti_rounded_rect(cr, 0, 0, m->w, m->h, 8);
    cairo_clip(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    jti_rounded_rect(cr, 2, 4, m->w, m->h, 8);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, m->h);
    cairo_pattern_add_color_stop_rgba(g, 0, 0x1A / 255.0, 0x1B / 255.0,
                                      0x1D / 255.0, 0.98);
    cairo_pattern_add_color_stop_rgba(g, 1, 0x11 / 255.0, 0x11 / 255.0,
                                      0x13 / 255.0, 0.98);
    jti_rounded_rect(cr, 0, 0, m->w, m->h, 8);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    jti_rgba(cr, a->pal.border, 0.85);
    cairo_set_line_width(cr, 1);
    jti_rounded_rect(cr, 0.5, 0.5, m->w - 1, m->h - 1, 7.5);
    cairo_stroke(cr);
    if (popup_lap) {
        /* Inset enough for the 3px lap stroke to stay inside the surface. */
        draw_lap_rect(a, cr, 2.5, 2.5, m->w - 5.0, m->h - 5.0, 6.0,
                      popup_progress);
    }

    int cy = 5;
    for (int i = 0; i < m->n; i++) {
        MenuItem *it = &m->items[i];
        int hh = it->kind == MI_SEP ? 9 : 26;
        if (it->kind == MI_SEP) {
            jti_rgba(cr, a->pal.fg, 0.09);
            cairo_set_line_width(cr, 1);
            cairo_move_to(cr, 8, cy + 4);
            cairo_line_to(cr, m->w - 8, cy + 4);
            cairo_stroke(cr);
        } else {
            int hot = i == m->hover && it->enabled;
            if (hot) {
                jti_rgba(cr, a->pal.accent, 0.28);
                jti_rounded_rect(cr, 5, cy, m->w - 10, hh, 4);
                cairo_fill(cr);
            }
            unsigned col = it->enabled ? (hot ? a->pal.fg : 0xE8E8E8)
                                       : a->pal.faint;
            if (it->kind == MI_CHECK && it->checked) {
                jti_rgba(cr, a->pal.accent, 0.95);
                cairo_move_to(cr, 11, cy + 12);
                cairo_line_to(cr, 14.5, cy + 16);
                cairo_line_to(cr, 21, cy + 7);
                cairo_set_line_width(cr, 2);
                cairo_stroke(cr);
            }
            double lx = it->kind == MI_CHECK ? 29 : 14;
            if (it->icon[0]) {
                jti_icon_draw(cr, it->icon, 16, lx, cy + 5, &a->pal, 0, NULL);
                lx += 22;
            }
            jti_text_draw(cr, it->label, 13, 400, col, 0.97,
                          lx, cy + 5, JTI_ALIGN_LEFT, 0.0);
            if (it->shortcut[0])
                jti_text_draw(cr, it->shortcut, 11, 400, a->pal.faint, 0.9,
                              m->w - (it->kind == MI_SUB ? 30 : 14), cy + 6,
                              JTI_ALIGN_RIGHT, 0.0);
            if (it->kind == MI_SUB) {
                jti_rgba(cr, col, 0.9);
                cairo_move_to(cr, m->w - 15, cy + 9);
                cairo_line_to(cr, m->w - 10, cy + 13);
                cairo_line_to(cr, m->w - 15, cy + 17);
                cairo_close_path(cr);
                cairo_fill(cr);
            }
        }
        cy += hh;
    }
}

static void menu_destroy(Menu *m)
{
    if (!m) return;
    if (m->sub) {
        m->sub->parent = NULL;
        if (m->sub->parent_idx >= 0 && m->sub->parent_idx < m->n)
            m->items[m->sub->parent_idx].sub = NULL;
        menu_destroy(m->sub);
        m->sub = NULL;
    }
    if (m->popup) {
        xdg_popup_destroy(m->popup);
        m->popup = NULL;
    }
    if (m->xdg_surface) {
        xdg_surface_destroy(m->xdg_surface);
        m->xdg_surface = NULL;
    }
    if (m->surf.wl) {
        /* Drop a queued frame callback so it cannot paint a freed menu. */
        surf_cancel(&m->surf);
        wl_surface_destroy(m->surf.wl);
        m->surf.wl = NULL;
    }
    free(m);
}

static void menu_close(Menu *m)
{
    if (!m) return;
    if (m->parent) {
        if (m->parent->items[m->parent_idx].sub == m)
            m->parent->items[m->parent_idx].sub = NULL;
        if (m->parent->sub == m)
            m->parent->sub = NULL;
        m->parent = NULL;
    }
    menu_destroy(m);
}

static void menu_close_root(Out *o)
{
    if (!o || !o->menu) return;
    Menu *root = o->menu;
    o->menu = NULL;
    menu_destroy(root);
}

/* ---- shm buffers and surfaces -------------------------------------------- */

static void buffer_release(void *data, struct wl_buffer *buf)
{
    Buf *b = data;
    b->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static void buf_destroy(App *a, Buf *b)
{
    if (!b) return;
    if (b->buf) { wl_buffer_destroy(b->buf); b->buf = NULL; }
    if (b->data) { munmap(b->data, b->map_size); b->data = NULL; }
    b->map_size = 0;
    b->busy = 0;
    (void)a;
}

static int buf_ensure(App *a, Buf *b, int w, int h)
{
    if (b->buf && b->w == w && b->h == h && !b->busy) return 1;
    if (b->busy) return 0;
    buf_destroy(a, b);
    size_t stride = (size_t)w * 4;
    size_t size = stride * (size_t)h;
    int fd = memfd_create("jtwpi-shm", MFD_CLOEXEC);
    if (fd < 0) return 0;
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return 0; }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return 0; }
    struct wl_shm_pool *pool = wl_shm_create_pool(a->shm, fd, (int32_t)size);
    close(fd);
    if (!pool) { munmap(data, size); return 0; }
    b->buf = wl_shm_pool_create_buffer(pool, 0, w, h, (int32_t)stride,
                                       WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (!b->buf) { munmap(data, size); return 0; }
    wl_buffer_add_listener(b->buf, &buffer_listener, b);
    b->data = data;
    b->map_size = size;
    b->w = w; b->h = h;
    return 1;
}

static int surf_flush(Surf *s);

static void frame_done(void *data, struct wl_callback *cb, uint32_t t)
{
    (void)t;
    Surf *s = data;
    if (s->frame == cb) s->frame = NULL;
    wl_callback_destroy(cb);
    /* Paint here, on the compositor's frame clock, rather than straight from
     * the input handler. Multiple pointer events between frames collapse into
     * one repaint with the latest state. Clear the owner's dirty bit only
     * once those newest coordinates have actually been painted; otherwise a
     * motion arriving while a frame was pending leaves it set and the next
     * frame paint is skipped, which shows up as a doubled full repaint. */
    if (surf_flush(s) && s->dirty_flag) *s->dirty_flag = 0;
}

static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void surf_request_frame(Surf *s)
{
    if (!s->wl || s->frame) return;
    s->frame = wl_surface_frame(s->wl);
    if (s->frame) wl_callback_add_listener(s->frame, &frame_listener, s);
}

static int surf_flush(Surf *s)
{
    if (!s->wl || !s->queued || !s->paint ||
        s->draw_w <= 0 || s->draw_h <= 0) return 0;
    App *a = g_app;
    int w = s->draw_w, h = s->draw_h, scale = s->draw_scale;
    s->queued = 0;

    Buf *b = NULL;
    for (int i = 0; i < NBUFS; i++)
        if (!s->bufs[i].busy && s->bufs[i].buf &&
            s->bufs[i].w == w && s->bufs[i].h == h) { b = &s->bufs[i]; break; }
    if (!b)
        for (int i = 0; i < NBUFS; i++)
            if (!s->bufs[i].busy) { b = &s->bufs[i]; break; }
    if (!b || !buf_ensure(a, b, w, h)) {
        s->queued = 1;
        surf_request_frame(s);
        /* A frame request only takes effect on the next commit. */
        wl_surface_commit(s->wl);
        return 0;
    }

    cairo_surface_t *sf = cairo_image_surface_create_for_data(
        b->data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    if (cairo_surface_status(sf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(sf);
        s->queued = 1;
        surf_request_frame(s);
        wl_surface_commit(s->wl);
        return 0;
    }
    cairo_t *cr = cairo_create(sf);
    cairo_scale(cr, scale, scale);
    s->paint(cr, w / scale, h / scale, s->paint_data);
    cairo_destroy(cr);
    cairo_surface_flush(sf);
    cairo_surface_destroy(sf);

    if (wl_surface_get_version(s->wl) >= 3)
        wl_surface_set_buffer_scale(s->wl, scale);
    wl_surface_attach(s->wl, b->buf, 0, 0);
    wl_surface_damage(s->wl, 0, 0, w, h);
    s->painted = 1;
    wl_surface_commit(s->wl);
    b->busy = 1;
    /* Do not arm another callback here. The next dirty owner will request
     * one in surf_draw(); arming here made redraws wait two frames. */
    return 1;
}

static void surf_cancel(Surf *s)
{
    if (!s) return;
    s->queued = 0;
    s->paint = NULL;
    s->paint_data = NULL;
    s->dirty_flag = NULL;
    if (s->frame) {
        wl_callback_destroy(s->frame);
        s->frame = NULL;
    }
}

static int surf_draw(App *a, Surf *s, int w, int h, int scale,
                     void (*paint)(cairo_t *, int, int, void *), void *data,
                     int *dirty_flag)
{
    (void)a;
    if (!s->wl || w <= 0 || h <= 0) return 0;
    s->draw_w = w;
    s->draw_h = h;
    s->draw_scale = scale;
    s->paint = paint;
    s->paint_data = data;
    s->dirty_flag = dirty_flag;
    s->queued = 1;
    if (!s->painted) {
        /* An unmapped surface (no buffer yet) produces no frame callbacks,
         * so prime the first frame synchronously. Later paints run from the
         * frame callback and stay locked to the compositor's clock. */
        if (surf_flush(s) && s->dirty_flag) *s->dirty_flag = 0;
        return 1;
    }
    /* A callback already pending means the next frame will paint the latest
     * state. Re-arming and committing again here only adds latency and work. */
    if (s->frame) return 1;
    surf_request_frame(s);
    /* wl_surface_frame() is only armed by the following commit. */
    wl_surface_commit(s->wl);
    return 1;
}

/* ---- menu lifecycle ------------------------------------------------------ */

static void out_layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
                                uint32_t serial, uint32_t w, uint32_t h)
{
    Out *o = data;
    o->width = (int)w; o->height = (int)h;
    o->configured = 1;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    if (o->n > 0) out_relayout(o, 1);
    out_mark(o);
}

static void out_layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
    (void)data; (void)layer;
}

static const struct zwlr_layer_surface_v1_listener out_layer_listener = {
    .configure = out_layer_configure, .closed = out_layer_closed,
};

static void menu_popup_configure(void *data, struct xdg_popup *popup,
                                 int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)popup; (void)x; (void)y; (void)w; (void)h;
    Menu *m = data;
    if (!m->configured && m->app && m->app->border_anim_enabled) {
        m->popup_anim_start = now_sec();
        m->popup_anim_active = 1;
    }
    m->configured = 1;
    m->dirty = 1;
}

static void menu_popup_done(void *data, struct xdg_popup *popup)
{
    (void)popup;
    Menu *m = data;
    m->done = 1;
}

static const struct xdg_popup_listener menu_popup_listener = {
    .configure = menu_popup_configure,
    .popup_done = menu_popup_done,
};

static void menu_surface_configure(void *data, struct xdg_surface *xs,
                                   uint32_t serial)
{
    Menu *m = data;
    (void)m;
    xdg_surface_ack_configure(xs, serial);
}

static const struct xdg_surface_listener menu_surface_listener = {
    .configure = menu_surface_configure,
};

static Menu *menu_create(Out *o, int x, int y, Menu *parent, int parent_idx)
{
    App *a = o->app;
    Menu *m = xcalloc(1, sizeof *m);
    m->app = a; m->out = o; m->parent = parent; m->parent_idx = parent_idx;
    m->x = x; m->y = y; m->open = 1; m->hover = -1;

    m->surf.wl = wl_compositor_create_surface(a->compositor);
    m->xdg_surface = xdg_wm_base_get_xdg_surface(a->wm_base, m->surf.wl);
    if (!m->xdg_surface) {
        wl_surface_destroy(m->surf.wl);
        m->surf.wl = NULL;
        free(m);
        return NULL;
    }
    xdg_surface_add_listener(m->xdg_surface, &menu_surface_listener, m);
    /* The xdg_popup role is assigned in menu_show() once the menu has been
     * populated and measured.  A compositor only sends xdg_surface.configure
     * after get_popup(), so no commit happens before that point. */
    return m;
}

static void menu_show(Menu *m)
{
    if (!m) return;
    menu_measure(m->app, m);
    Out *o = m->out;   /* kept for the layer-shell popup association below */
    (void)o;

    struct xdg_positioner *pos = xdg_wm_base_create_positioner(m->app->wm_base);
    if (!pos) return;
    xdg_positioner_set_size(pos, m->w, m->h);
    xdg_positioner_set_anchor_rect(pos, m->x, m->y, 1, 1);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    /* BOTTOM_RIGHT puts the popup's top-left at the anchor so it grows down
     * and right from the pointer, like a Windows 10 context menu. TOP_LEFT
     * gravity grows it up/left and made menus appear above the cursor. */
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(pos,
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);

    if (m->parent) {
        m->popup = xdg_surface_get_popup(m->xdg_surface,
                                         m->parent->xdg_surface, pos);
    } else {
        m->popup = xdg_surface_get_popup(m->xdg_surface, NULL, pos);
        /* Root menu: attach the popup to the layer surface so labwc places
         * it above the desktop layer and sends popup_done on click-away. */
        if (m->popup)
            zwlr_layer_surface_v1_get_popup(o->layer, m->popup);
    }
    xdg_positioner_destroy(pos);
    if (!m->popup) return;
    xdg_popup_add_listener(m->popup, &menu_popup_listener, m);
    xdg_surface_set_window_geometry(m->xdg_surface, 0, 0, m->w, m->h);
    if (m->app->seat)
        xdg_popup_grab(m->popup, m->app->seat, m->app->ptr_serial);
    if (wl_surface_get_version(m->surf.wl) >= 3 && o->scale > 0)
        wl_surface_set_buffer_scale(m->surf.wl, o->scale);
    wl_surface_commit(m->surf.wl);
    m->dirty = 1;
}

/* Open the size submenu for item idx of parent.
 * x,y are coordinates relative to the parent popup's surface. */
static void menu_open_size_sub(Out *o, Menu *parent, int idx, int x, int y)
{
    if (parent->items[idx].sub) return;
    Menu *sub = menu_create(o, x, y, parent, idx);
    if (!sub) return;
    static const int sizes[] = { 32, 48, 64, 96, 128 };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        MenuItem *it = mitem(sub, MI_CHECK, ACT_SIZE, NULL, NULL, NULL, 1);
        snprintf(it->label, sizeof it->label, "%d px", sizes[i]);
        it->id = ACT_SIZE;
        it->checked = sizes[i] == g_app->icon_size;
        /* encode the size in the shortcut field so the action can read it */
        snprintf(it->shortcut, sizeof it->shortcut, "%d", sizes[i]);
    }
    parent->items[idx].sub = sub;
    parent->sub = sub;
    menu_show(sub);
}

static void menu_open(Out *o, int x, int y, int on_icon)
{
    if (o->menu) { menu_close(o->menu); o->menu = NULL; }
    Menu *m = menu_create(o, x, y, NULL, -1);
    if (!m) return;
    App *a = o->app;
    int multi = 0, cnt = 0;
    for (int i = 0; i < o->n; i++) if (o->icons[i].selected) cnt++;
    multi = cnt > 1;
    int can_paste = clipboard_has(a);

    if (on_icon && !multi) {
        MenuItem *it = mitem(m, MI_ACTION, ACT_OPEN, "Open", "Enter", NULL, 1);
        (void)it;
        msep(m);
    }
    mitem(m, MI_ACTION, ACT_CUT, "Cut", "Ctrl+X", NULL, 1);
    mitem(m, MI_ACTION, ACT_COPY, "Copy", "Ctrl+C", NULL, 1);
    mitem(m, MI_ACTION, ACT_PASTE, "Paste", "Ctrl+V", NULL, can_paste);
    msep(m);
    if (on_icon && !multi) {
        mitem(m, MI_ACTION, ACT_RENAME, "Rename…", "F2", NULL, 1);
        mitem(m, MI_ACTION, ACT_TRASH, "Move to Trash", "Delete", NULL, 1);
        mitem(m, MI_ACTION, ACT_DELETE, "Delete Permanently…", "Shift+Del", NULL, 1);
        msep(m);
        mitem(m, MI_ACTION, ACT_PROPERTIES, "Properties", "Alt+Enter", NULL, 1);
        msep(m);
    }
    mitem(m, MI_ACTION, ACT_NEW_FOLDER, "New Folder", "", NULL, 1);
    mitem(m, MI_ACTION, ACT_NEW_LAUNCHER, "New Launcher…", "", NULL, 1);
    msep(m);
    mitem(m, MI_ACTION, ACT_ARRANGE, "Arrange Icons", "", NULL, 1);
    if (!on_icon) {
        MenuItem *snap = mitem(m, MI_CHECK, ACT_SNAP, "Snap to Grid", "", NULL, 1);
        snap->checked = a->snap;
        MenuItem *size = mitem(m, MI_SUB, ACT_SIZE, "Icon Size", "", NULL, 1);
        (void)size;
        MenuItem *show = mitem(m, MI_CHECK, ACT_TOGGLE_ICONS,
                               "Show Desktop Icons", "", NULL, 1);
        show->checked = a->show_icons;
    }
    msep(m);
    mitem(m, MI_ACTION, ACT_REFRESH, "Refresh", "F5", NULL, 1);
    if (!on_icon)
        mitem(m, MI_ACTION, ACT_OPEN_FOLDER, "Open Desktop Folder", "", NULL, 1);

    o->menu = m;
    menu_show(m);
}

/* ---- actions / dialogs --------------------------------------------------- */

static int selected_paths(Out *o, char out[][PATH_MAX], int cap)
{
    int n = 0;
    for (int i = 0; i < o->n && n < cap; i++)
        if (o->icons[i].selected) {
            snprintf(out[n], PATH_MAX, "%s", o->icons[i].path);
            n++;
        }
    return n;
}

/* ---- toplevel windows (dialog + transfer progress) ------------------------ */

/* Secondary windows keep their opening size.  Min/max already pins the edges;
 * also undo any maximize/fullscreen state the compositor tries to apply. */
static void fixed_toplevel_reject_states(struct xdg_toplevel *t,
                                         struct wl_array *states)
{
    if (!t || !states) return;
    int32_t *state;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED)
            xdg_toplevel_unset_maximized(t);
        else if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
            xdg_toplevel_unset_fullscreen(t);
    }
}

static void win_surface_configure(void *data, struct xdg_surface *xs,
                                  uint32_t serial)
{
    Win *w = data;
    xdg_surface_ack_configure(xs, serial);
    w->configured = 1;
    w->dirty = 1;
}

static const struct xdg_surface_listener win_surface_listener = {
    .configure = win_surface_configure,
};

static void win_toplevel_configure(void *data, struct xdg_toplevel *t,
                                   int32_t width, int32_t height,
                                   struct wl_array *states)
{
    /* Dialog and transfer windows are fixed-size: ignore any size proposed by
     * a resize/maximize configure and keep the dimensions we opened with. */
    (void)width; (void)height;
    fixed_toplevel_reject_states(t, states);
    Win *w = data;
    if (w->xdg_surface)
        xdg_surface_set_window_geometry(w->xdg_surface, 0, 0, w->w, w->h);
    w->dirty = 1;
}

static void win_toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)t;
    Win *w = data;
    if (w->on_close && w->app) w->on_close(w->app);
}

static const struct xdg_toplevel_listener win_toplevel_listener = {
    .configure = win_toplevel_configure,
    .close = win_toplevel_close,
};

static void win_close(Win *w);

static void win_open(App *a, Win *w, const char *title, const char *app_id,
                     int w_px, int h_px, int scale, void (*on_close)(App *))
{
    if (!a->compositor || !a->wm_base || w->surface) return;
    w->app = a;
    w->w = w_px;
    w->h = h_px;
    w->scale = scale > 0 ? scale : 1;
    w->on_close = on_close;

    w->surface = wl_compositor_create_surface(a->compositor);
    if (!w->surface) return;
    w->surf.wl = w->surface;          /* the paint engine draws through Surf */
    w->xdg_surface = xdg_wm_base_get_xdg_surface(a->wm_base, w->surface);
    if (!w->xdg_surface) { win_close(w); return; }
    xdg_surface_add_listener(w->xdg_surface, &win_surface_listener, w);
    w->toplevel = xdg_surface_get_toplevel(w->xdg_surface);
    if (!w->toplevel) { win_close(w); return; }
    xdg_toplevel_add_listener(w->toplevel, &win_toplevel_listener, w);
    xdg_toplevel_set_title(w->toplevel, title);
    xdg_toplevel_set_app_id(w->toplevel, app_id);
    xdg_toplevel_set_min_size(w->toplevel, w->w, w->h);
    xdg_toplevel_set_max_size(w->toplevel, w->w, w->h);
    xdg_surface_set_window_geometry(w->xdg_surface, 0, 0, w->w, w->h);

    if (a->deco_manager) {
        w->decoration =
            zxdg_decoration_manager_v1_get_toplevel_decoration(
                a->deco_manager, w->toplevel);
        if (w->decoration)
            zxdg_toplevel_decoration_v1_set_mode(w->decoration,
                ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    if (wl_surface_get_version(w->surface) >= 3)
        wl_surface_set_buffer_scale(w->surface, w->scale);
    wl_surface_commit(w->surface);
    w->dirty = 1;
}

static void win_close(Win *w)
{
    if (!w) return;
    surf_cancel(&w->surf);
    for (int i = 0; i < NBUFS; i++) buf_destroy(w->app, &w->surf.bufs[i]);
    if (w->decoration) {
        zxdg_toplevel_decoration_v1_destroy(w->decoration);
        w->decoration = NULL;
    }
    if (w->toplevel) { xdg_toplevel_destroy(w->toplevel); w->toplevel = NULL; }
    if (w->xdg_surface) {
        xdg_surface_destroy(w->xdg_surface);
        w->xdg_surface = NULL;
    }
    if (w->surface) { wl_surface_destroy(w->surface); w->surface = NULL; }
    w->surf.wl = NULL;
    w->configured = 0;
    w->dirty = 0;
}

/* ---- dialog content and input --------------------------------------------- */

static void paint_dialog(cairo_t *cr, int w, int h, void *data)
{
    (void)w; (void)h;
    App *a = data ? data : g_app;
    Dialog *d = a->dialog;
    if (!d) return;

    double dw = (double)a->dlg_win.w, dh = (double)a->dlg_win.h;
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, dh);
    cairo_pattern_add_color_stop_rgba(g, 0, 0.11, 0.115, 0.125, 1);
    cairo_pattern_add_color_stop_rgba(g, 1, 0.07, 0.072, 0.078, 1);
    cairo_set_source(cr, g);
    cairo_rectangle(cr, 0, 0, dw, dh);
    cairo_fill(cr);
    cairo_pattern_destroy(g);

    jti_text_draw(cr, d->title, 14, 600, a->pal.fg, 1.0,
                  18, 16, JTI_ALIGN_LEFT, 0.0);
    cairo_set_source_rgba(cr, JTI_R(a->pal.fg), JTI_G(a->pal.fg),
                          JTI_B(a->pal.fg), 0.08);
    cairo_move_to(cr, 18, 44.5);
    cairo_line_to(cr, dw - 18, 44.5);
    cairo_stroke(cr);

    if (d->kind == DLG_INFO) {
        char buf[2048], line[256];
        snprintf(buf, sizeof buf, "%s", d->info);
        int y = 62;
        char *save = NULL;
        for (char *s = strtok_r(buf, "\n", &save);
             s && y < dh - 66; s = strtok_r(NULL, "\n", &save)) {
            snprintf(line, sizeof line, "%s", s);
            jti_text_draw(cr, line, 11, 400, a->pal.fg, 0.92,
                          20, y, JTI_ALIGN_LEFT, 0.0);
            y += 19;
        }
    } else if (d->kind == DLG_CONFIRM) {
        jti_text_draw(cr, d->prompt, 12, 400, a->pal.dim, 0.95,
                      20, 72, JTI_ALIGN_LEFT, 0.0);
    } else {
        jti_text_draw(cr, d->prompt, 11, 400, a->pal.dim, 0.95,
                      20, 60, JTI_ALIGN_LEFT, 0.0);
        cairo_set_source_rgba(cr, 0.06, 0.06, 0.065, 1);
        jti_rounded_rect(cr, 20, 74, dw - 40, 34, 5);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, JTI_R(a->pal.accent), JTI_G(a->pal.accent),
                              JTI_B(a->pal.accent), 0.75);
        cairo_set_line_width(cr, 1);
        jti_rounded_rect(cr, 20.5, 74.5, dw - 41, 33, 5);
        cairo_stroke(cr);
        jti_text_draw(cr, d->text, 12, 400, a->pal.fg, 1.0,
                      28, 82, JTI_ALIGN_LEFT, 0.0);
        double tw = jti_text_width(cr, d->text, 12, 400, 0.0);
        cairo_set_source_rgba(cr, JTI_R(a->pal.accent), JTI_G(a->pal.accent),
                              JTI_B(a->pal.accent), 1);
        cairo_rectangle(cr, 28 + tw, 82, 1.5, 18);
        cairo_fill(cr);
    }

    const char *ok = d->kind == DLG_INFO ? "Close" :
                     (d->kind == DLG_CONFIRM ? "Delete" : "OK");
    double bw = 84, by = dh - 48;
    jti_rounded_rect(cr, dw - 22 - bw * 2 - 8, by, bw, 32, 6);
    cairo_set_source_rgba(cr, 0.16, 0.16, 0.17, 1);
    cairo_fill(cr);
    jti_text_draw(cr, "Cancel", 12, 500, a->pal.dim, 1.0,
                  dw - 22 - bw * 2 - 8 + bw / 2.0, by + 8,
                  JTI_ALIGN_CENTER, 0.0);
    jti_rounded_rect(cr, dw - 22 - bw, by, bw, 32, 6);
    if (d->kind == DLG_CONFIRM)
        cairo_set_source_rgba(cr, 0.78, 0.20, 0.20, 1);
    else
        cairo_set_source_rgba(cr, JTI_R(a->pal.accent), JTI_G(a->pal.accent),
                              JTI_B(a->pal.accent), 1);
    cairo_fill(cr);
    jti_text_draw(cr, ok, 12, 600, a->pal.on_accent, 1.0,
                  dw - 22 - bw / 2.0, by + 8, JTI_ALIGN_CENTER, 0.0);
}

static void dialog_click(double x, double y)
{
    App *a = g_app;
    Dialog *d = a->dialog;
    if (!d) return;
    double dw = (double)a->dlg_win.w, dh = (double)a->dlg_win.h;
    double bw = 84, by = dh - 48;
    if (y < by || y > by + 32) return;
    double ok_x = dw - 22 - bw;
    double cancel_x = dw - 22 - bw * 2 - 8;
    if (x >= ok_x && x <= ok_x + bw) {
        char text[512];
        snprintf(text, sizeof text, "%s", d->text);
        int id = d->ok_id;
        void (*cb)(Out *, const char *, int) = d->on_ok;
        Out *out = d->out;
        dialog_close();
        if (cb) cb(out, text, id);
    } else if (x >= cancel_x && x <= cancel_x + bw) {
        dialog_close();
    }
}

static void dialog_key(xkb_keysym_t sym)
{
    App *a = g_app;
    Dialog *d = a->dialog;
    if (!d) return;
    if (sym == XKB_KEY_Escape) { dialog_close(); return; }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        char text[512];
        snprintf(text, sizeof text, "%s", d->text);
        int id = d->ok_id;
        void (*cb)(Out *, const char *, int) = d->on_ok;
        Out *out = d->out;
        dialog_close();
        if (cb) cb(out, text, id);
        return;
    }
    if (d->kind != DLG_TEXT) return;
    size_t len = strlen(d->text);
    if (sym == XKB_KEY_BackSpace) {
        if (d->cursor > 0) {
            memmove(d->text + d->cursor - 1, d->text + d->cursor,
                    len - (size_t)d->cursor + 1);
            d->cursor--;
        }
    } else if (sym == XKB_KEY_Delete) {
        if (d->cursor < (int)len)
            memmove(d->text + d->cursor, d->text + d->cursor + 1,
                    len - (size_t)d->cursor);
    } else if (sym == XKB_KEY_Left) {
        if (d->cursor > 0) d->cursor--;
    } else if (sym == XKB_KEY_Right) {
        if (d->cursor < (int)len) d->cursor++;
    } else if (sym == XKB_KEY_Home) {
        d->cursor = 0;
    } else if (sym == XKB_KEY_End) {
        d->cursor = (int)len;
    } else if (sym >= 0x20 && sym < 0x7f && len < sizeof d->text - 1) {
        memmove(d->text + d->cursor + 1, d->text + d->cursor,
                len - (size_t)d->cursor + 1);
        d->text[d->cursor++] = (char)sym;
    }
    a->dlg_win.dirty = 1;
}

static void dialog_win_close_cb(App *a) { (void)a; dialog_close(); }

static void dialog_close(void)
{
    App *a = g_app;
    if (!a) return;
    if (a->dialog) { free(a->dialog); a->dialog = NULL; }
    win_close(&a->dlg_win);
    memset(&a->dlg_win, 0, sizeof a->dlg_win);
}

static void dialog_open(Out *o, DialogKind kind, const char *title,
                        const char *prompt, const char *initial,
                        const char *info, int ok_id,
                        void (*on_ok)(Out *, const char *, int))
{
    App *a = o->app;
    dialog_close();
    Dialog *d = xcalloc(1, sizeof *d);
    d->kind = kind;
    d->out = o;
    snprintf(d->title, sizeof d->title, "%s", title ? title : "");
    snprintf(d->prompt, sizeof d->prompt, "%s", prompt ? prompt : "");
    snprintf(d->text, sizeof d->text, "%s", initial ? initial : "");
    snprintf(d->info, sizeof d->info, "%s", info ? info : "");
    d->cursor = (int)strlen(d->text);
    d->ok_id = ok_id;
    d->on_ok = on_ok;
    a->dialog = d;
    win_open(a, &a->dlg_win, d->title, "jtwpi-dialog", 460,
             kind == DLG_INFO ? 290 : 176, o->scale, dialog_win_close_cb);
}

static void dialog_text(Out *o, const char *title, const char *prompt,
                        const char *initial, int ok_id,
                        void (*on_ok)(Out *, const char *, int))
{
    dialog_open(o, DLG_TEXT, title, prompt, initial, NULL, ok_id, on_ok);
}

static void dialog_info(Out *o, const char *title, const char *info)
{
    dialog_open(o, DLG_INFO, title, NULL, NULL, info, 0, NULL);
}

static char g_delete_paths[64][PATH_MAX];
static int g_delete_n;

static void dlg_delete_ok(Out *o, const char *text, int id)
{
    (void)text; (void)id;
    if (g_delete_n > 0) fs_op_start(o->app, XOP_DELETE, g_delete_paths, g_delete_n);
    g_delete_n = 0;
}

/* ---- transfer progress window and child polling --------------------------- */

static void status_set(App *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof a->status, fmt, ap);
    va_end(ap);
    a->status_until = time(NULL) + 6;
    all_mark(a);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void fmt_size(char *out, size_t n, double bytes)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int i = 0;
    while (bytes >= 1024.0 && i < 4) { bytes /= 1024.0; i++; }
    if (i == 0) snprintf(out, n, "%.0f %s", bytes, units[i]);
    else snprintf(out, n, "%.1f %s", bytes, units[i]);
}

static const char *xfer_title(int op)
{
    switch (op) {
    case XOP_MOVE:   return "Moving files";
    case XOP_TRASH:  return "Moving to Trash";
    case XOP_DELETE: return "Deleting files";
    default:         return "Copying files";
    }
}

static void xfer_window_close(App *a)
{
    Transfer *x = &a->xfer;
    x->show_pending = 0;
    win_close(&x->win);
    memset(&x->win, 0, sizeof x->win);
}

static void xfer_win_close_cb(App *a)
{
    status_set(a, "Transfer is still running");
}

static void xfer_window_open(App *a)
{
    Transfer *x = &a->xfer;
    if (x->win.surface) return;
    int scale = a->n_outs > 0 ? a->outs[0].scale : 1;
    win_open(a, &x->win, xfer_title(x->op), "jtwpi-transfer",
             520, 220, scale, xfer_win_close_cb);
}

/* Fast copies finish before a progress window could be read.  Delay mapping
 * it briefly and skip it entirely if the worker is already done. */
#define XFER_WINDOW_DELAY 0.25

static void xfer_window_schedule(App *a)
{
    Transfer *x = &a->xfer;
    x->show_pending = 1;
    x->show_at = now_sec() + XFER_WINDOW_DELAY;
}

static void xfer_window_maybe_open(App *a)
{
    Transfer *x = &a->xfer;
    if (!x->active || !x->show_pending) return;
    if (now_sec() < x->show_at) return;
    x->show_pending = 0;
    xfer_window_open(a);
}

static void paint_xfer(cairo_t *cr, int w, int h, void *data)
{
    (void)w; (void)h;
    App *a = data ? data : g_app;
    Transfer *x = &a->xfer;

    double pw = (double)x->win.w, ph = (double)x->win.h;
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, ph);
    cairo_pattern_add_color_stop_rgba(g, 0, 0.11, 0.115, 0.125, 1);
    cairo_pattern_add_color_stop_rgba(g, 1, 0.07, 0.072, 0.078, 1);
    cairo_set_source(cr, g);
    cairo_rectangle(cr, 0, 0, pw, ph);
    cairo_fill(cr);
    cairo_pattern_destroy(g);

    jti_rounded_rect(cr, 18, 18, pw - 36, ph - 36, 12);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.04);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, JTI_R(a->pal.border), JTI_G(a->pal.border),
                          JTI_B(a->pal.border), 0.7);
    cairo_set_line_width(cr, 1);
    jti_rounded_rect(cr, 18.5, 18.5, pw - 37, ph - 37, 12);
    cairo_stroke(cr);

    double left = 38.0, right = pw - 38.0;
    jti_text_draw(cr, xfer_title(x->op), 15, 600, a->pal.fg, 0.98,
                  left, 38, JTI_ALIGN_LEFT, 0.0);

    char detail[NAME_MAX + 64];
    snprintf(detail, sizeof detail, "%s", x->current);
    /* ellipsize by trimming the string until it fits */
    while (detail[0] &&
           jti_text_width(cr, detail, 12, 400, 0.0) > right - left)
        detail[strlen(detail) - 1] = '\0';
    jti_text_draw(cr, detail, 12, 400, a->pal.dim, 0.9,
                  left, 62, JTI_ALIGN_LEFT, 0.0);

    double bar_y = 90.0, bar_w = right - left;
    cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
    jti_rounded_rect(cr, left, bar_y, bar_w, 8, 4);
    cairo_fill(cr);

    double frac = 0.0;
    if (x->total_bytes > 0 && x->bytes_done > 0)
        frac = (double)x->bytes_done / (double)x->total_bytes;
    else if (x->total_items > 0 && x->items_done > 0)
        frac = (double)x->items_done / (double)x->total_items;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    if (frac > 0.0) {
        cairo_pattern_t *fill = cairo_pattern_create_linear(left, bar_y,
                                                            left, bar_y + 8);
        cairo_pattern_add_color_stop_rgba(fill, 0, JTI_R(a->pal.accent_hi),
                                          JTI_G(a->pal.accent_hi),
                                          JTI_B(a->pal.accent_hi), 1);
        cairo_pattern_add_color_stop_rgba(fill, 1, JTI_R(a->pal.accent_lo),
                                          JTI_G(a->pal.accent_lo),
                                          JTI_B(a->pal.accent_lo), 1);
        cairo_set_source(cr, fill);
        jti_rounded_rect(cr, left, bar_y, bar_w * frac, 8, 4);
        cairo_fill(cr);
        cairo_pattern_destroy(fill);
    }

    char counts[160];
    if (x->total_items > 0)
        snprintf(counts, sizeof counts, "%lld of %lld items",
                 x->items_done, x->total_items);
    else
        snprintf(counts, sizeof counts, "Preparing...");
    jti_text_draw(cr, counts, 11, 400, a->pal.dim, 0.85,
                  left, bar_y + 22, JTI_ALIGN_LEFT, 0.0);
    char pct[32];
    snprintf(pct, sizeof pct, "%.0f%%", frac * 100.0);
    jti_text_draw(cr, pct, 11, 600, a->pal.accent, 0.95,
                  right, bar_y + 22, JTI_ALIGN_RIGHT, 0.0);

    if ((x->op == XOP_COPY || x->op == XOP_MOVE) && x->speed > 0.0 &&
        x->total_bytes > 0 && x->bytes_done < x->total_bytes) {
        char spd[64], line[96], eta[64];
        fmt_size(spd, sizeof spd, x->speed);
        snprintf(line, sizeof line, "%s/s", spd);
        jti_text_draw(cr, line, 11, 400, a->pal.dim, 0.85,
                      left, bar_y + 40, JTI_ALIGN_LEFT, 0.0);
        long long s = (long long)((double)(x->total_bytes - x->bytes_done) /
                                  x->speed + 0.5);
        if (s < 60) snprintf(eta, sizeof eta, "%llds left", s);
        else if (s < 3600) snprintf(eta, sizeof eta, "%lldm %llds left", s / 60, s % 60);
        else snprintf(eta, sizeof eta, "%lldh %lldm left", s / 3600, (s % 3600) / 60);
        jti_text_draw(cr, eta, 11, 400, a->pal.dim, 0.85,
                      right, bar_y + 40, JTI_ALIGN_RIGHT, 0.0);
    }
    jti_text_draw(cr, "jtwpi", 10, 600, a->pal.faint, 0.7,
                  right, ph - 34, JTI_ALIGN_RIGHT, 0.0);
}

static void xfer_speed_update(Transfer *x)
{
    double now = now_sec();
    if (x->speed_t0 <= 0.0) {
        x->speed_t0 = now;
        x->speed_bytes0 = (double)x->bytes_done;
        return;
    }
    double dt = now - x->speed_t0;
    if (dt < 0.25) return;
    double delta = (double)x->bytes_done - x->speed_bytes0;
    if (delta < 0.0) delta = 0.0;
    double inst = delta / dt;
    x->speed = x->speed > 0.0 ? x->speed * 0.65 + inst * 0.35 : inst;
    x->speed_t0 = now;
    x->speed_bytes0 = (double)x->bytes_done;
}

static void xfer_parse_line(App *a, char *line)
{
    Transfer *x = &a->xfer;
    int changed = 0;
    if (!strncmp(line, "T ", 2)) {
        long long items = 0, bytes = 0;
        if (sscanf(line + 2, "%lld %lld", &items, &bytes) == 2) {
            changed = x->total_items != items || x->total_bytes != bytes;
            x->total_items = items;
            x->total_bytes = bytes;
        }
    } else if (!strncmp(line, "N ", 2)) {
        changed = strcmp(x->current, line + 2) != 0;
        snprintf(x->current, sizeof x->current, "%s", line + 2);
    } else if (!strncmp(line, "B ", 2)) {
        long long bytes = 0;
        if (sscanf(line + 2, "%lld", &bytes) == 1) {
            changed = x->bytes_done != bytes;
            x->bytes_done = bytes;
        }
    } else if (!strncmp(line, "P ", 2)) {
        long long items = 0, bytes = 0;
        if (sscanf(line + 2, "%lld %lld", &items, &bytes) == 2) {
            changed = x->items_done != items || x->bytes_done != bytes;
            x->items_done = items;
            x->bytes_done = bytes;
        }
    } else if (!strncmp(line, "D ", 2)) {
        int done = 0, failed = 0;
        if (sscanf(line + 2, "%d %d", &done, &failed) == 2) {
            changed = x->done_items != done || x->failed != failed;
            x->done_items = done;
            x->failed = failed;
        }
    } else {
        return;
    }
    if (line[0] == 'B' || line[0] == 'P') xfer_speed_update(x);
    x->win.dirty |= changed;
}

static void xfer_consume(App *a, int final)
{
    Transfer *x = &a->xfer;
    char *start = x->buf;
    size_t remaining = x->len;
    for (;;) {
        char *nl = memchr(start, '\n', remaining);
        if (!nl) break;
        *nl = '\0';
        xfer_parse_line(a, start);
        size_t used = (size_t)(nl - start) + 1;
        start += used;
        remaining -= used;
    }
    if (start != x->buf) {
        memmove(x->buf, start, remaining);
        x->len = remaining;
        x->buf[x->len] = '\0';
    }
    if (final && x->len > 0) {
        x->buf[x->len] = '\0';
        xfer_parse_line(a, x->buf);
        x->len = 0;
        x->buf[0] = '\0';
    }
}

static void xfer_finish(App *a, int code)
{
    Transfer *x = &a->xfer;
    int done = x->done_items > 0 ? x->done_items : (int)x->items_done;
    int failed = x->failed;
    if (code != 0 && failed == 0) failed = 1;
    int cut = x->cut;
    int op = x->op;
    int removes = op == XOP_TRASH || op == XOP_DELETE;
    int n_dst = x->n_dst;
    char dst_names[64][NAME_MAX + 1];
    memcpy(dst_names, x->dst_names, sizeof dst_names);
    PastePlace place = x->place;

    if (x->fd >= 0) { close(x->fd); x->fd = -1; }
    if (x->pid > 0) {
        /* Cancelled from the UI: stop the worker before closing the pipe. */
        if (code != 0) kill(x->pid, SIGTERM);
        int status;
        waitpid(x->pid, &status, WNOHANG);
        x->pid = 0;
    }
    xfer_window_close(a);
    int was_cut = cut && !removes;
    memset(x, 0, sizeof *x);
    x->fd = -1;

    const char *verb = op == XOP_MOVE ? "Moved" :
                       (op == XOP_TRASH ? "Trashed" :
                        (op == XOP_DELETE ? "Deleted" : "Copied"));
    if (failed)
        status_set(a, "%s %d item%s, %d failed", verb, done,
                   done == 1 ? "" : "s", failed);
    else if (op == XOP_TRASH)
        status_set(a, "Moved %d item%s to Trash", done, done == 1 ? "" : "s");
    else if (op == XOP_DELETE)
        status_set(a, "Permanently deleted %d item%s", done, done == 1 ? "" : "s");
    else
        status_set(a, "%s %d item%s", verb, done, done == 1 ? "" : "s");

    if (n_dst > 0 && code == 0)
        post_sel_arm(a, a->desktop_dir, dst_names, n_dst);

    if (was_cut) {
        a->n_clip = 0;
        a->clip_cut = 0;
        a->clip_cache_valid = 0;
        clip_write_cancel_all(a);
        if (a->clip_source) {
            wl_data_source_destroy(a->clip_source);
            a->clip_source = NULL;
            if (a->data_device)
                wl_data_device_set_selection(a->data_device, NULL, a->clip_serial);
            wl_display_flush(a->display);
        }
    }
    for (int i = 0; i < a->n_outs; i++) out_load_icons(&a->outs[i], 1);
    if (place.valid && code == 0)
        place_new_icons(a, place.out, dst_names, n_dst, place.x, place.y);
}

static void xfer_poll(App *a)
{
    Transfer *x = &a->xfer;
    if (!x->active || x->pid <= 0) return;

    if (x->fd >= 0) {
        char buf[256];
        for (int budget = 32; budget > 0; budget--) {
            ssize_t n = read(x->fd, buf, sizeof buf);
            if (n > 0) {
                size_t room = sizeof x->buf - 1 - x->len;
                if ((size_t)n > room) n = (ssize_t)room;
                if (n > 0) {
                    memcpy(x->buf + x->len, buf, (size_t)n);
                    x->len += (size_t)n;
                    x->buf[x->len] = '\0';
                }
                continue;
            }
            if (n == 0) break;
            if (errno == EINTR) continue;
            break;
        }
    }

    xfer_consume(a, 0);

    int status = 0;
    pid_t ret = waitpid(x->pid, &status, WNOHANG);
    if (ret == 0) return;
    if (ret == x->pid && x->fd >= 0) {
        char tail[256];
        ssize_t n;
        while ((n = read(x->fd, tail, sizeof tail)) > 0) {
            size_t room = sizeof x->buf - 1 - x->len;
            if ((size_t)n > room) n = (ssize_t)room;
            if (n <= 0) break;
            memcpy(x->buf + x->len, tail, (size_t)n);
            x->len += (size_t)n;
            x->buf[x->len] = '\0';
        }
    }
    xfer_consume(a, 1);
    x->pid = 0;
    xfer_finish(a, WIFEXITED(status) ? WEXITSTATUS(status) : 1);
}

static int start_child(int *pid_out, int *fd_out, const char *mode,
                       uint32_t code, uint32_t n,
                       char (*paths)[PATH_MAX], int pairs)
{
    int p[2] = { -1, -1 }, q[2] = { -1, -1 };
    if (pipe2(p, O_CLOEXEC) < 0 || pipe2(q, O_CLOEXEC) < 0) {
        if (p[0] >= 0) { close(p[0]); close(p[1]); }
        if (q[0] >= 0) { close(q[0]); close(q[1]); }
        return -1;
    }
    int flags = fcntl(p[0], F_GETFL, 0);
    if (flags >= 0) fcntl(p[0], F_SETFL, flags | O_NONBLOCK);

    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]); close(q[1]);
        if (dup2(q[0], STDIN_FILENO) < 0) _exit(127);
        if (q[0] != STDIN_FILENO) close(q[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0) _exit(127);
        if (p[1] != STDOUT_FILENO) close(p[1]);
        char *av[] = { (char *)"/proc/self/exe", (char *)mode, NULL };
        execv(av[0], av);
        _exit(127);
    }
    close(p[1]); close(q[0]);
    if (pid < 0) {
        close(q[1]); close(p[0]);
        return -1;
    }

    int sent = xfer_write_u32(q[1], code) && xfer_write_u32(q[1], n);
    for (uint32_t i = 0; sent && i < n; i++) {
        uint32_t len = (uint32_t)strlen(paths[i]) + 1;
        sent = xfer_write_u32(q[1], len) &&
               xfer_write_all(q[1], paths[i], len);
        if (sent && pairs && paths[64 + i][0]) {
            uint32_t dlen = (uint32_t)strlen(paths[64 + i]) + 1;
            sent = xfer_write_u32(q[1], dlen) &&
                   xfer_write_all(q[1], paths[64 + i], dlen);
        }
    }
    close(q[1]);
    if (!sent) {
        close(p[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    *pid_out = pid;
    *fd_out = p[0];
    return 0;
}

static void do_paste(App *a)
{
    if (a->n_clip <= 0) { status_set(a, "Clipboard is empty"); return; }
    if (a->xfer.active) {
        status_set(a, "Another transfer is already running");
        return;
    }
    PastePlace place = a->paste_place;
    paste_place_clear(a);
    int n = a->n_clip;
    if (n > 64) n = 64;
    static char paths[128][PATH_MAX];
    for (int i = 0; i < n; i++) {
        snprintf(paths[i], PATH_MAX, "%s", a->clip[i]);
        unique_path(a->desktop_dir, path_base(paths[i]),
                    paths[64 + i], PATH_MAX);
        if (!strcmp(paths[i], paths[64 + i])) {
            status_set(a, "Cannot choose a destination name");
            return;
        }
    }

    int cut = a->clip_cut;
    int pid = -1, fd = -1;
    if (start_child(&pid, &fd, "--xfer-child", (uint32_t)cut, (uint32_t)n,
                    paths, 1) < 0) {
        status_set(a, "Cannot start transfer");
        return;
    }

    Transfer *x = &a->xfer;
    memset(x, 0, sizeof *x);
    x->pid = pid;
    x->fd = fd;
    x->active = 1;
    x->cut = cut;
    x->op = cut ? XOP_MOVE : XOP_COPY;
    x->n_dst = n;
    x->place = place;
    for (int i = 0; i < n; i++)
        snprintf(x->dst_names[i], NAME_MAX + 1, "%s",
                 path_base(paths[64 + i]));
    snprintf(x->current, sizeof x->current, "Scanning...");
    xfer_window_schedule(a);
    status_set(a, "%s %d item%s", cut ? "Moving" : "Copying", n,
               n == 1 ? "" : "s");
}

static void fs_op_start(App *a, int op, char (*paths)[PATH_MAX], int n)
{
    if (n <= 0) return;
    if (a->xfer.active) {
        status_set(a, "Another operation is already running");
        return;
    }
    int pid = -1, fd = -1;
    if (start_child(&pid, &fd, "--fs-child", (uint32_t)op, (uint32_t)n,
                    paths, 0) < 0) {
        status_set(a, "Cannot start operation");
        return;
    }
    Transfer *x = &a->xfer;
    memset(x, 0, sizeof *x);
    x->pid = pid;
    x->fd = fd;
    x->active = 1;
    x->op = op;
    snprintf(x->current, sizeof x->current, "Scanning...");
    xfer_window_schedule(a);
    status_set(a, "%s %d item%s", op == XOP_TRASH ?
               "Moving to Trash" : "Permanently deleting", n,
               n == 1 ? "" : "s");
}

static void dlg_rename_ok(Out *o, const char *text, int id)
{
    (void)id;
    if (!*text || strchr(text, '/')) return;
    int idx = -1;
    for (int i = 0; i < o->n; i++) if (o->icons[i].selected) { idx = i; break; }
    if (idx < 0) return;
    char dst[PATH_MAX];
    path_join(dst, sizeof dst, g_app->desktop_dir, text);
    if (!access(dst, F_OK)) return;
    if (rename(o->icons[idx].path, dst) == 0) {
        char sec[160];
        state_mon_section(o->key, sec, sizeof sec);
        state_set(sec, o->icons[idx].key, "");
        state_save(g_app);
        out_load_icons(o, 1);
    }
}

static void dlg_newfolder_ok(Out *o, const char *text, int id)
{
    (void)id;
    if (!*text || strchr(text, '/')) return;
    char dst[PATH_MAX];
    path_join(dst, sizeof dst, g_app->desktop_dir, text);
    if (mkdir(dst, 0755) == 0) out_load_icons(o, 1);
}

static void dlg_launcher_ok(Out *o, const char *text, int id)
{
    (void)id;
    if (!*text) return;
    char base[128];
    snprintf(base, sizeof base, "launcher-%ld.desktop", (long)time(NULL));
    char dst[PATH_MAX];
    path_join(dst, sizeof dst, g_app->desktop_dir, base);
    FILE *f = fopen(dst, "w");
    if (!f) return;
    fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\n"
               "Icon=application-x-executable\nTerminal=false\n", text, text);
    fclose(f);
    out_load_icons(o, 1);
}

static void launch_exec(DIcon *ic)
{
    if (ic->is_desktop && ic->exec && *ic->exec) {
        char cmd[2048];
        size_t j = 0;
        for (const char *p = ic->exec; *p && j < sizeof cmd - 1; p++) {
            if (*p == '%') {
                if (p[1]) p++;
                continue;
            }
            cmd[j++] = *p;
        }
        cmd[j] = 0;
        const char *av[] = { "/bin/sh", "-c", cmd, NULL };
        spawn_detached(av);
    } else {
        const char *av[] = { "xdg-open", ic->path, NULL };
        spawn_detached(av);
    }
}

static void open_selected(Out *o)
{
    for (int i = 0; i < o->n; i++)
        if (o->icons[i].selected) { launch_exec(&o->icons[i]); break; }
}

static void action_run(Out *o, int id, int arg)
{
    App *a = o->app;
    if (id == ACT_SIZE) {
        int size = arg;
        if (size < MIN_ICON_SIZE) size = MIN_ICON_SIZE;
        if (size > MAX_ICON_SIZE) size = MAX_ICON_SIZE;
        if (size != a->icon_size) {
            a->icon_size = size;
            for (int i = 0; i < a->n_outs; i++) {
                for (int k = 0; k < a->outs[i].n; k++) {
                    DIcon *ic = &a->outs[i].icons[k];
                    if (ic->sf) cairo_surface_destroy(ic->sf);
                    ic->sf = NULL;
                }
                out_relayout(&a->outs[i], 1);
            }
            state_save(a);
        }
        return;
    }
    switch (id) {
    case ACT_OPEN:
        open_selected(o); break;
    case ACT_CUT:
    case ACT_COPY: {
        a->n_clip = selected_paths(o, a->clip, 64);
        a->clip_cut = id == ACT_CUT;
        clipboard_publish(a);
        /* Confirm the staged icons with the same full-lap accent used after
         * a drop/paste.  They are already selected, so start the lap here
         * rather than waiting for a rescan to apply the selection. */
        char names[64][NAME_MAX + 1];
        int n = 0;
        for (int i = 0; i < a->n_clip && n < 64; i++)
            snprintf(names[n++], NAME_MAX + 1, "%s", path_base(a->clip[i]));
        post_sel_arm(a, a->desktop_dir, names, n);
        if (a->border_anim_enabled && a->post_armed)
            a->post_sel_anim_start = now_sec();
        break;
    }
    case ACT_PASTE: {
        int cut = 0;
        /* A just-activated menu supplies its own anchor; every other paste
         * takes a fresh pointer position so a cancelled/stale request cannot
         * hijack the next one. */
        if (!a->paste_place.armed) {
            if (a->ptr_out)
                paste_place_set(a, a->ptr_out, a->ptr_x, a->ptr_y);
            else
                paste_place_set(a, o, o->width / 2.0, o->height / 2.0);
        } else {
            a->paste_place.armed = 0;
        }
        if (clipboard_fetch(a, &cut)) do_paste(a);
        else if (a->clip_read.fd < 0) paste_place_clear(a);
        break;
    }
    case ACT_RENAME: {
        int idx = -1;
        for (int i = 0; i < o->n; i++) if (o->icons[i].selected) { idx = i; break; }
        if (idx >= 0)
            dialog_text(o, "Rename", "Enter the new name:",
                        o->icons[idx].key, ACT_RENAME, dlg_rename_ok);
        break;
    }
    case ACT_TRASH: {
        char paths[64][PATH_MAX];
        int n = selected_paths(o, paths, 64);
        fs_op_start(a, XOP_TRASH, paths, n);
        break;
    }
    case ACT_DELETE: {
        char paths[64][PATH_MAX];
        g_delete_n = selected_paths(o, paths, 64);
        if (g_delete_n <= 0) break;
        memcpy(g_delete_paths, paths, sizeof paths);
        char prompt[256];
        snprintf(prompt, sizeof prompt,
                 "Permanently delete %d item%s? This cannot be undone.",
                 g_delete_n, g_delete_n == 1 ? "" : "s");
        dialog_open(o, DLG_CONFIRM, "Delete Permanently", prompt,
                    NULL, NULL, 0, dlg_delete_ok);
        break;
    }
    case ACT_PROPERTIES: {
        int idx = -1;
        for (int i = 0; i < o->n; i++) if (o->icons[i].selected) { idx = i; break; }
        if (idx >= 0) {
            DIcon *ic = &o->icons[idx];
            char info[2048];
            struct tm tm;
            localtime_r(&ic->mtime, &tm);
            char date[64];
            strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
            snprintf(info, sizeof info,
                     "Name: %s\nType: %s\nSize: %lld bytes\nModified: %s\n\nPath:\n%s",
                     ic->name,
                     ic->is_dir ? "Folder" : (ic->is_desktop ? "Application" : "File"),
                     (long long)ic->size, date, ic->path);
            dialog_info(o, "Properties", info);
        }
        break;
    }
    case ACT_NEW_FOLDER:
        dialog_text(o, "New Folder", "Folder name:", "New Folder",
                    ACT_NEW_FOLDER, dlg_newfolder_ok);
        break;
    case ACT_NEW_LAUNCHER:
        dialog_text(o, "New Launcher", "Command:", "",
                    ACT_NEW_LAUNCHER, dlg_launcher_ok);
        break;
    case ACT_ARRANGE:
        /* Arrange icons on every output, like the old GTK version did. */
        for (int j = 0; j < a->n_outs; j++) {
            Out *oo = &a->outs[j];
            for (int i = 0; i < oo->n; i++) {
                oo->icons[i].x = oo->icons[i].y = -1;
                oo->icons[i].want_x = oo->icons[i].want_y = -1;
                oo->icons[i].auto_placed = 1;
            }
            out_relayout(oo, 1);
        }
        state_save(a);
        break;
    case ACT_SNAP:
        a->snap = !a->snap;
        if (a->snap)
            for (int j = 0; j < a->n_outs; j++)
                out_relayout(&a->outs[j], 1);
        state_save(a);
        all_mark(a);
        break;
    case ACT_TOGGLE_ICONS:
        a->show_icons = !a->show_icons;
        state_save(a);
        all_mark(a);
        break;
    case ACT_REFRESH:
        for (int i = 0; i < a->n_outs; i++) out_load_icons(&a->outs[i], 1);
        break;
    case ACT_OPEN_FOLDER: {
        const char *av[] = { "xdg-open", a->desktop_dir, NULL };
        spawn_detached(av);
        break;
    }
    default: break;
    }
}

/* ---- wayland: outputs ---------------------------------------------------- */

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t sub,
                            const char *make, const char *model, int32_t tr)
{
    (void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph;
    (void)sub; (void)make; (void)model; (void)tr;
}

static void output_mode(void *data, struct wl_output *o, uint32_t flags,
                        int32_t w, int32_t h, int32_t refresh)
{ (void)data; (void)o; (void)flags; (void)w; (void)h; (void)refresh; }

static void output_done(void *data, struct wl_output *o)
{ (void)data; (void)o; }

static void output_scale(void *data, struct wl_output *o, int32_t f)
{
    Out *out = data;
    (void)o;
    if (f < 1) f = 1;
    if (f > 3) f = 3;
    if (out->scale == f) return;
    out->scale = f;
    for (int i = 0; i < out->n; i++) {
        if (out->icons[i].sf) cairo_surface_destroy(out->icons[i].sf);
        out->icons[i].sf = NULL;
    }
    out_mark(out);
    if (out->menu) {
        menu_close(out->menu);
        out->menu = NULL;
    }
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry, .mode = output_mode,
    .done = output_done, .scale = output_scale,
};

static void create_output_surface(App *a, Out *o)
{
    o->surf.wl = wl_compositor_create_surface(a->compositor);
    o->layer = zwlr_layer_shell_v1_get_layer_surface(
        a->layer_shell, o->surf.wl, o->output,
        ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "jtwpi");
    zwlr_layer_surface_v1_add_listener(o->layer, &out_layer_listener, o);
    zwlr_layer_surface_v1_set_anchor(o->layer,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
    if (zwlr_layer_surface_v1_get_version(o->layer) >= 4)
        zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer,
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
    zwlr_layer_surface_v1_set_size(o->layer, 0, 0);
    wl_surface_commit(o->surf.wl);
}

/* ---- wayland: pointer / keyboard ----------------------------------------- */

static void update_cursor(App *a)
{
    if (!a->cursor_manager || !a->pointer) return;
    if (!a->cursor_device) {
        a->cursor_device =
            wp_cursor_shape_manager_v1_get_pointer(a->cursor_manager, a->pointer);
        if (!a->cursor_device) return;
        a->cursor_shape = 0;
    }
    uint32_t shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    if (a->ptr_menu ||
        (a->dialog && a->ptr_surface == a->dlg_win.surf.wl) ||
        (a->xfer.active && a->ptr_surface == a->xfer.win.surf.wl))
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
    else if (a->ptr_out) {
        DIcon *ic = out_hit(a->ptr_out, a->ptr_x, a->ptr_y);
        shape = ic ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER
                   : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    }
    if (shape != a->cursor_shape || a->cursor_serial != a->cursor_applied) {
        wp_cursor_shape_device_v1_set_shape(a->cursor_device, a->cursor_serial,
                                             shape);
        a->cursor_shape = shape;
        a->cursor_applied = a->cursor_serial;
    }
}

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)p;
    App *a = data;
    a->ptr_serial = serial;
    a->cursor_serial = serial;
    a->ptr_surface = surface;
    a->ptr_x = wl_fixed_to_double(sx);
    a->ptr_y = wl_fixed_to_double(sy);
    a->ptr_out = NULL; a->ptr_menu = NULL;
    for (int i = 0; i < a->n_outs; i++) {
        Out *o = &a->outs[i];
        if (o->surf.wl == surface) a->ptr_out = o;
        if (o->menu && o->menu->surf.wl == surface) a->ptr_menu = o->menu;
        if (o->menu && o->menu->sub) {
            Menu *s = o->menu->sub;
            if (s->surf.wl == surface) a->ptr_menu = s;
        }
    }
    if (a->ptr_menu)
        menu_pointer_update(a->ptr_menu, a->ptr_x, a->ptr_y);
    update_cursor(a);
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface)
{
    (void)p; (void)serial; (void)surface;
    App *a = data;
    if (a->ptr_out) {
        Out *o = a->ptr_out;
        /* Do not start DnD from leave: while a button is held the compositor
         * keeps pointer focus on the pressed surface, so leave never arrives
         * when moving over another window. The drag is started from motion
         * while the press serial and origin surface are still valid. */
        if (o->selecting) {
            select_in_rect(o, o->sel_x0, o->sel_y0,
                           o->sel_x1, o->sel_y1, 0);
            o->selecting = 0;
        }
        o->dragging = 0; o->drag_moved = 0;
        o->drag_dx = o->drag_dy = 0;
        out_clear_hover(o);
    }
    a->ptr_surface = NULL; a->ptr_out = NULL; a->ptr_menu = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t t,
                           wl_fixed_t sx, wl_fixed_t sy)
{
    (void)p; (void)t;
    App *a = data;
    a->ptr_x = wl_fixed_to_double(sx);
    a->ptr_y = wl_fixed_to_double(sy);

    if (a->ptr_menu) {
        menu_pointer_update(a->ptr_menu, a->ptr_x, a->ptr_y);
        return;
    }

    Out *o = a->ptr_out;
    if (!o) return;

    if (a->dialog) return;

    if (o->dragging) {
        double dx = a->ptr_x - o->press_x, dy = a->ptr_y - o->press_y;
        if (!o->drag_moved &&
            dx * dx + dy * dy >= DRAG_THRESHOLD * DRAG_THRESHOLD) {
            o->drag_moved = 1;
            /* No desktop icon is hovered once a drag takes over the pointer. */
            out_clear_hover(o);
            /* Start the compositor drag right here, while our surface still
             * owns the implicit pointer grab. labwc validates the press
             * serial and origin surface in start_drag(); doing this on
             * pointer.leave fails because focus never leaves the pressed
             * surface during a held-button drag. Internal drops still
             * reposition icons via dnd_internal_drop(). */
            if (a->dnd_pending && !a->dnd_source) dnd_begin(a, o);
            if (a->dnd_source) return;
        }
        if (o->drag_moved) {
            /* Snap to whole pixels while dragging: subpixel offsets force
             * resampling of every cached icon surface on every frame, and
             * the drop handler already rounds to integers. */
            o->drag_dx = (double)lround(dx);
            o->drag_dy = (double)lround(dy);
            out_mark(o);
        }
        return;
    }
    if (o->selecting) {
        o->sel_x1 = a->ptr_x; o->sel_y1 = a->ptr_y;
        select_in_rect(o, o->sel_x0, o->sel_y0, o->sel_x1, o->sel_y1,
                       xkb_state_mod_name_is_active(a->xkb_state,
                           XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0);
        out_mark(o);
        return;
    }
    DIcon *ic = out_hit(o, a->ptr_x, a->ptr_y);
    int h = ic ? (int)(ic - o->icons) : -1;
    if (h != o->hover) {
        if (o->hover >= 0) o->icons[o->hover].hover = 0;
        o->hover = h;
        if (h >= 0) o->icons[h].hover = 1;
        out_mark(o);
    }
    update_cursor(a);
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
                           uint32_t t, uint32_t button, uint32_t state)
{
    (void)p;
    App *a = data;
    post_sel_disarm(a);
    a->ptr_serial = serial;
    a->clip_serial = serial;
    a->cursor_serial = serial;

    if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (a->ptr_surface == a->dlg_win.surf.wl ||
            a->ptr_surface == a->xfer.win.surf.wl) return;
        Out *o = a->ptr_out;
        if (!o) return;
        if (o->dragging && o->drag_moved) {
            for (int i = 0; i < o->n; i++) {
                DIcon *ic = &o->icons[i];
                if (!ic->selected || ic->x < 0) continue;
                ic->x += (int)lround(o->drag_dx);
                ic->y += (int)lround(o->drag_dy);
                snap_xy(&ic->x, &ic->y);
                ic->want_x = ic->x; ic->want_y = ic->y;
                ic->auto_placed = 0;
                clamp_icon(o, ic);
            }
            resolve_overlaps(o, 1);
            o->dragging = 0; o->drag_moved = 0;
            o->drag_dx = o->drag_dy = 0;
            state_save(a);
            check_dir(a);   /* catch up on any Desktop change deferred mid-drag */
        } else {
            o->dragging = 0; o->drag_moved = 0;
            a->dnd_pending = 0;   /* click/selection, not a drag */
        }
        if (o->selecting) {
            select_in_rect(o, o->sel_x0, o->sel_y0, o->sel_x1, o->sel_y1,
                           xkb_state_mod_name_is_active(a->xkb_state,
                               XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0);
            o->selecting = 0;
        }
        o->drag_dx = o->drag_dy = 0;
        out_mark(o);
        return;
    }
    if (state != WL_POINTER_BUTTON_STATE_PRESSED) return;

    if (a->dialog && a->ptr_surface == a->dlg_win.surf.wl) {
        if (button == BTN_LEFT) dialog_click(a->ptr_x, a->ptr_y);
        return;
    }
    if (a->dialog) return;   /* modal: ignore clicks on the desktop behind it */
    if (a->xfer.active && a->ptr_surface == a->xfer.win.surf.wl) return;

    /* If a context menu is open but this click landed outside it,
     * dismiss the menu (normal desktop behaviour).  pointer_enter()
     * clears ptr_menu when the pointer leaves the menu surface, so we
     * have to look at the outputs directly. */
    if (!a->ptr_menu) {
        int had_menu = 0;
        for (int i = 0; i < a->n_outs; i++) {
            if (a->outs[i].menu) {
                menu_close_root(&a->outs[i]);
                had_menu = 1;
            }
        }
        if (had_menu) {
            /* Right-click outside opens a fresh menu at the new spot. */
            if (button == BTN_RIGHT && a->ptr_out) {
                Out *o = a->ptr_out;
                DIcon *ic = out_hit(o, a->ptr_x, a->ptr_y);
                int h = ic ? (int)(ic - o->icons) : -1;
                if (h >= 0 && !ic->selected) {
                    for (int k = 0; k < o->n; k++) o->icons[k].selected = 0;
                    ic->selected = 1;
                }
                o->hover = h;
                out_mark(o);
                menu_open(o, (int)a->ptr_x, (int)a->ptr_y, h >= 0);
            }
            return;
        }
    }

    if (a->ptr_menu) {
        Menu *m = a->ptr_menu;
        Out *oo = m->out;
        int idx = menu_hit(m, a->ptr_x, a->ptr_y);
        if (button != BTN_LEFT || idx < 0 || !m->items[idx].enabled) {
            if (m->parent && m->parent->items[m->parent_idx].sub == m) {
                Menu *parent = m->parent;
                int pidx = m->parent_idx;
                menu_close(m);
                parent->hover = pidx;
                a->ptr_menu = parent;
            } else {
                menu_close_root(oo);
                a->ptr_menu = NULL;
            }
            return;
        }
        MenuItem *it = &m->items[idx];
        if (it->kind == MI_SUB) {
            menu_open_size_sub(oo, m, idx,
                m->w - MENU_INSET, menu_item_offset_y(m, idx));
            return;
        }
        int id = it->id;
        int arg = it->id == ACT_SIZE ? (int)strtol(it->shortcut, NULL, 10) : 0;
        paste_place_from_menu(a, m, id);
        menu_close_root(oo);
        a->ptr_menu = NULL;
        action_run(oo, id, arg);
        return;
    }

    Out *o = a->ptr_out;
    if (!o) return;

    int ctrl = xkb_state_mod_name_is_active(a->xkb_state, XKB_MOD_NAME_CTRL,
                                             XKB_STATE_MODS_EFFECTIVE) > 0;
    int shift = xkb_state_mod_name_is_active(a->xkb_state, XKB_MOD_NAME_SHIFT,
                                              XKB_STATE_MODS_EFFECTIVE) > 0;
    DIcon *ic = out_hit(o, a->ptr_x, a->ptr_y);
    int h = ic ? (int)(ic - o->icons) : -1;

    /* Double-click detection: same icon, within 400ms and a small radius. */
    if (button == BTN_LEFT && h >= 0 && h == o->last_click_idx &&
        t - o->last_click_time < 400 &&
        fabs(a->ptr_x - o->last_click_x) < 8 &&
        fabs(a->ptr_y - o->last_click_y) < 8) {
        o->last_click_time = 0;
        o->last_click_idx = -1;
        o->dragging = 0; o->drag_moved = 0;
        o->drag_dx = o->drag_dy = 0;
        launch_exec(&o->icons[h]);
        return;
    }
    if (button == BTN_LEFT && h >= 0) {
        o->last_click_time = t;
        o->last_click_idx = h;
        o->last_click_x = a->ptr_x;
        o->last_click_y = a->ptr_y;
    }

    if (button == BTN_RIGHT) {
        if (h >= 0 && !ic->selected) {
            for (int i = 0; i < o->n; i++) o->icons[i].selected = 0;
            ic->selected = 1;
        }
        o->hover = h;
        out_mark(o);
        menu_open(o, (int)a->ptr_x, (int)a->ptr_y, h >= 0);
        return;
    }
    if (button != BTN_LEFT) return;

    if (h >= 0) {
        if (shift) {
            for (int i = 0; i < o->n; i++) o->icons[i].selected = 0;
            for (int i = 0; i <= h; i++) o->icons[i].selected = 1;
        } else if (ctrl) {
            ic->selected = !ic->selected;
        } else {
            if (!ic->selected)
                for (int i = 0; i < o->n; i++) o->icons[i].selected = 0;
            ic->selected = 1;
        }
        o->press = h;
        o->press_x = a->ptr_x; o->press_y = a->ptr_y;
        o->dragging = 1; o->drag_moved = 0;
        o->drag_dx = o->drag_dy = 0;
        a->dnd_press_serial = serial;
        a->dnd_pending = 1;   /* upgrade to a compositor drag past threshold */
    } else {
        if (!ctrl && !shift)
            for (int i = 0; i < o->n; i++) o->icons[i].selected = 0;
        o->selecting = 1;
        o->sel_x0 = o->sel_x1 = a->ptr_x;
        o->sel_y0 = o->sel_y1 = a->ptr_y;
    }
    out_mark(o);
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t t,
                         uint32_t axis, wl_fixed_t v)
{ (void)data; (void)p; (void)t; (void)axis; (void)v; }

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis,
};

static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t fmt,
                            int32_t fd, uint32_t size)
{
    (void)kb;
    App *a = data;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    struct xkb_keymap *km = xkb_keymap_new_from_string(
        a->xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km) return;
    struct xkb_state *st = xkb_state_new(km);
    if (!st) { xkb_keymap_unref(km); return; }
    xkb_state_unref(a->xkb_state);
    xkb_keymap_unref(a->xkb_keymap);
    a->xkb_keymap = km; a->xkb_state = st;
}

static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys)
{
    (void)kb; (void)serial; (void)keys;
    ((App *)data)->kb_surface = surface;
}

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *surface)
{
    (void)kb; (void)serial; (void)surface;
    ((App *)data)->kb_surface = NULL;
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb,
                               uint32_t serial, uint32_t dep, uint32_t lat,
                               uint32_t lock, uint32_t group)
{
    (void)kb; (void)serial;
    App *a = data;
    a->mods_dep = dep; a->mods_lat = lat; a->mods_lock = lock; a->mods_group = group;
    if (a->xkb_state)
        xkb_state_update_mask(a->xkb_state, dep, lat, lock, 0, 0, group);
}

static void menu_key(Out *o, Menu *m, xkb_keysym_t sym)
{
    App *a = m->app;
    if (sym == XKB_KEY_Escape) {
        if (m->parent) {
            Menu *parent = m->parent;
            menu_close(m);
            a->ptr_menu = parent;
            return;
        }
        menu_close_root(o);
        a->ptr_menu = NULL;
        return;
    }
    if (sym == XKB_KEY_Down || sym == XKB_KEY_Up) {
        int dir = sym == XKB_KEY_Down ? 1 : -1;
        int i = m->hover;
        for (int k = 0; k < m->n; k++) {
            i += dir;
            if (i < 0) i = m->n - 1;
            if (i >= m->n) i = 0;
            if (m->items[i].kind != MI_SEP && m->items[i].enabled) break;
        }
        if (m->sub) menu_close(m->sub);
        m->hover = i;
        m->dirty = 1;
        return;
    }
    if (sym == XKB_KEY_Right && m->hover >= 0 &&
        m->items[m->hover].kind == MI_SUB) {
        menu_open_size_sub(o, m, m->hover, m->w - MENU_INSET,
                           menu_item_offset_y(m, m->hover));
        return;
    }
    if (sym == XKB_KEY_Left && m->parent) { menu_close(m); return; }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (m->hover < 0) return;
        MenuItem *it = &m->items[m->hover];
        if (!it->enabled) return;
        if (it->kind == MI_SUB) {
            menu_open_size_sub(o, m, m->hover, m->w - MENU_INSET,
                               menu_item_offset_y(m, m->hover));
            return;
        }
        int id = it->id;
        int arg = id == ACT_SIZE ? (int)strtol(it->shortcut, NULL, 10) : 0;
        paste_place_from_menu(a, m, id);
        menu_close_root(o);
        a->ptr_menu = NULL;
        action_run(o, id, arg);
    }
}

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state)
{
    (void)kb; (void)time;
    App *a = data;
    post_sel_disarm(a);
    a->clip_serial = serial;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (!a->xkb_state) return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(a->xkb_state, key + 8);
    if (a->dialog && a->kb_surface == a->dlg_win.surf.wl) {
        dialog_key(sym);
        return;
    }
    if (a->xfer.active && sym == XKB_KEY_Escape &&
        (!a->xfer.win.surface || a->kb_surface == a->xfer.win.surf.wl)) {
        xfer_finish(a, 1);
        return;
    }
    if (a->xfer.active && a->kb_surface == a->xfer.win.surf.wl) return;
    int ctrl = xkb_state_mod_name_is_active(a->xkb_state, XKB_MOD_NAME_CTRL,
                                             XKB_STATE_MODS_EFFECTIVE) > 0;
    int shift = xkb_state_mod_name_is_active(a->xkb_state, XKB_MOD_NAME_SHIFT,
                                              XKB_STATE_MODS_EFFECTIVE) > 0;

    Out *o = NULL;
    for (int i = 0; i < a->n_outs; i++) {
        Out *cand = &a->outs[i];
        if (cand->surf.wl == a->kb_surface ||
            (cand->menu && cand->menu->surf.wl == a->kb_surface) ||
            (cand->menu && cand->menu->sub &&
             cand->menu->sub->surf.wl == a->kb_surface)) { o = cand; break; }
    }
    if (!o && a->ptr_out) o = a->ptr_out;
    if (!o) return;

    if (a->dialog) return;
    if (o->menu) {
        Menu *m = o->menu;
        if (m->sub && m->sub->surf.wl == a->kb_surface) m = m->sub;
        menu_key(o, m, sym);
        return;
    }

    if (ctrl && sym == XKB_KEY_c) { action_run(o, ACT_COPY, 0); return; }
    if (ctrl && sym == XKB_KEY_x) { action_run(o, ACT_CUT, 0); return; }
    if (ctrl && sym == XKB_KEY_v) { action_run(o, ACT_PASTE, 0); return; }
    if (ctrl && sym == XKB_KEY_a) {
        for (int i = 0; i < o->n; i++) o->icons[i].selected = 1;
        out_mark(o); return;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        open_selected(o); return;
    }
    if (sym == XKB_KEY_F2) { action_run(o, ACT_RENAME, 0); return; }
    if (sym == XKB_KEY_F5) { action_run(o, ACT_REFRESH, 0); return; }
    if (sym == XKB_KEY_Delete) {
        action_run(o, shift ? ACT_DELETE : ACT_TRASH, 0); return;
    }
    if (sym == XKB_KEY_Escape) {
        for (int i = 0; i < o->n; i++) o->icons[i].selected = 0;
        out_mark(o); return;
    }
}

static void keyboard_repeat(void *data, struct wl_keyboard *kb,
                            int32_t rate, int32_t delay)
{
    (void)data; (void)kb; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap, .enter = keyboard_enter,
    .leave = keyboard_leave, .key = keyboard_key,
    .modifiers = keyboard_modifiers, .repeat_info = keyboard_repeat,
};

/* ---- Drag and drop (wl_data_device) ------------------------------------- */

static int dnd_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void dnd_path_to_uri(const char *path, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    if (n < 9) { if (n) out[0] = 0; return; }
    memcpy(out, "file://", 7); j = 7;
    for (const unsigned char *p = (const unsigned char *)path;
         *p && j + 3 < n; p++) {
        if (isalnum(*p) || *p == '/' || *p == '-' || *p == '.' ||
            *p == '_' || *p == '~' || *p == ':') {
            out[j++] = (char)*p;
        } else {
            out[j++] = '%';
            out[j++] = hex[*p >> 4];
            out[j++] = hex[*p & 15];
        }
    }
    out[j] = 0;
}

static void dnd_uri_to_path(const char *uri, char *out, size_t n)
{
    if (!strncmp(uri, "file://", 7)) uri += 7;
    size_t j = 0;
    for (; *uri && *uri != '\r' && *uri != '\n' && j + 1 < n; uri++) {
        if (*uri == '%' && uri[1] && uri[2]) {
            int hi = dnd_hexval(uri[1]), lo = dnd_hexval(uri[2]);
            if (hi >= 0 && lo >= 0) {
                out[j++] = (char)(hi * 16 + lo);
                uri += 2;
                continue;
            }
        }
        out[j++] = *uri;
    }
    out[j] = 0;
}

static void dnd_write_all(int fd, const char *s, size_t len)
{
    while (len) {
        ssize_t n = write(fd, s, len);
        if (n > 0) { s += n; len -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

/* ------------------------------------------------------------------------- */
/*  Clipboard (wl_data_device selection)                                     */
/* ------------------------------------------------------------------------- */

static int clip_is_text_mime(const char *mime)
{
    return !strcmp(mime, "text/plain;charset=utf-8") ||
           !strcmp(mime, "text/plain") ||
           !strcmp(mime, "UTF8_STRING") ||
           !strcmp(mime, "STRING") ||
           !strcmp(mime, "TEXT");
}

static void clip_snapshot_files(App *a, char (*paths)[PATH_MAX], int n, int cut)
{
    memset(&a->clip_snap, 0, sizeof a->clip_snap);
    if (n > 64) n = 64;
    a->clip_snap.n = n > 0 ? n : 0;
    a->clip_snap.cut = cut;
    for (int i = 0; i < a->clip_snap.n; i++)
        snprintf(a->clip_snap.paths[i], PATH_MAX, "%s", paths[i]);
}

/* Queue a nonblocking write for a wl_data_source.send() pipe.  GTK hands
 * these fds to a GOutputStream instead of blocking the Wayland event loop. */
static void clip_write_cancel_all(App *a)
{
    ClipWrite *w = a->clip_writes;
    a->clip_writes = NULL;
    while (w) {
        ClipWrite *next = w->next;
        close(w->fd);
        free(w->buf);
        free(w);
        w = next;
    }
}

static void clip_write_poll(App *a, int fd)
{
    ClipWrite **link = &a->clip_writes;
    while (*link && (*link)->fd != fd) link = &(*link)->next;
    ClipWrite *w = *link;
    if (!w) return;

    while (w->off < w->len) {
        ssize_t n = write(w->fd, w->buf + w->off, w->len - w->off);
        if (n > 0) { w->off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        break;
    }
    *link = w->next;
    close(w->fd);
    free(w->buf);
    free(w);
}

static void clip_write_enqueue(App *a, int fd, char *buf, size_t len)
{
    if (fd < 0 || !buf) {
        if (fd >= 0) close(fd);
        free(buf);
        return;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ClipWrite *w = calloc(1, sizeof *w);
    if (!w) { close(fd); free(buf); return; }
    w->fd = fd;
    w->buf = buf;
    w->len = len;
    w->next = a->clip_writes;
    a->clip_writes = w;
    clip_write_poll(a, fd);
}

/* Freeze the current payload for one mime type.  The gnome target carries the
 * cut verb with LF endings; text/uri-list keeps its required CRLF endings. */
static char *clip_build_payload(App *a, const char *mime, size_t *out_len)
{
    ClipSnapshot *s = &a->clip_snap;
    int gnome = !strcmp(mime, "x-special/gnome-copied-files");
    int uris = !strcmp(mime, "text/uri-list");
    size_t cap = 64 * (PATH_MAX * 3 + 32) + 8192;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t len = 0;

#define CLIP_ADD(str_) do {                                     \
        size_t n_ = strlen(str_);                               \
        if (len + n_ + 1 > cap) { free(buf); return NULL; }     \
        memcpy(buf + len, (str_), n_);                          \
        len += n_;                                              \
        buf[len] = 0;                                           \
    } while (0)

    if (s->has_text && !s->n) {
        if (clip_is_text_mime(mime)) {
            CLIP_ADD(s->text);
        } else if (uris || gnome) {
            char uri[PATH_MAX + 32];
            dnd_path_to_uri(s->text, uri, sizeof uri);
            if (gnome) CLIP_ADD("copy\n");
            CLIP_ADD(uri);
            if (!gnome) CLIP_ADD("\r\n");
        }
        *out_len = len;
        return buf;
    }

    if (!s->n) { *out_len = 0; return buf; }

    if (gnome) {
        CLIP_ADD(s->cut ? "cut\n" : "copy\n");
        /* Nemo splits this target on '\n' without dropping empty lines, so
         * there must be no trailing newline after the final URI. */
        for (int i = 0; i < s->n; i++) {
            char uri[PATH_MAX + 32];
            dnd_path_to_uri(s->paths[i], uri, sizeof uri);
            if (i) CLIP_ADD("\n");
            CLIP_ADD(uri);
        }
    } else {
        for (int i = 0; i < s->n; i++) {
            char line[PATH_MAX * 3 + 32];
            if (clip_is_text_mime(mime)) {
                snprintf(line, sizeof line, "%s\n", s->paths[i]);
            } else {
                char uri[PATH_MAX + 32];
                dnd_path_to_uri(s->paths[i], uri, sizeof uri);
                snprintf(line, sizeof line, "%s\r\n", uri);
            }
            CLIP_ADD(line);
        }
    }
#undef CLIP_ADD

    *out_len = len;
    return buf;
}

static void clip_source_send(void *data, struct wl_data_source *source,
                             const char *mime, int fd)
{
    (void)source;
    App *a = data;
    size_t len = 0;
    char *buf = clip_build_payload(a, mime ? mime : "", &len);
    clip_write_enqueue(a, fd, buf, len);
}

static void clip_source_target(void *data, struct wl_data_source *source,
                               const char *mime)
{ (void)data; (void)source; (void)mime; }

static void clip_source_cancelled(void *data, struct wl_data_source *source)
{
    App *a = data;
    if (a->clip_source != source) return;
    clip_write_cancel_all(a);
    wl_data_source_destroy(source);
    a->clip_source = NULL;
    a->n_clip = 0;
    a->clip_cut = 0;
    a->clip_cache_valid = 0;
    memset(&a->clip_snap, 0, sizeof a->clip_snap);
}

static void clip_source_drop(void *data, struct wl_data_source *source)
{ (void)data; (void)source; }
static void clip_source_finished(void *data, struct wl_data_source *source)
{ (void)data; (void)source; }
static void clip_source_action(void *data, struct wl_data_source *source,
                               uint32_t action)
{ (void)data; (void)source; (void)action; }

static const struct wl_data_source_listener clip_source_listener = {
    .target = clip_source_target, .send = clip_source_send,
    .cancelled = clip_source_cancelled,
    .dnd_drop_performed = clip_source_drop,
    .dnd_finished = clip_source_finished,
    .action = clip_source_action,
};

static void clipboard_publish_snapshot(App *a)
{
    if (!a->data_device || !a->data_device_manager) return;
    uint32_t serial = a->clip_serial ? a->clip_serial : a->ptr_serial;

    clip_read_cancel(a);
    a->clip_cache_valid = 0;
    struct wl_data_source *old = a->clip_source;
    a->clip_source = NULL;

    /* GTK sets the new selection before destroying the old source.  If we
     * destroy first, a clipboard manager can observe an empty selection and
     * restore its own stale payload in the gap. */
    if (!a->clip_snap.n && !a->clip_snap.has_text) {
        clip_write_cancel_all(a);
        wl_data_device_set_selection(a->data_device, NULL, serial);
        if (old) wl_data_source_destroy(old);
        wl_display_flush(a->display);
        return;
    }

    struct wl_data_source *src =
        wl_data_device_manager_create_data_source(a->data_device_manager);
    if (!src) {
        a->clip_source = old;
        return;
    }
    clip_write_cancel_all(a);
    wl_data_source_add_listener(src, &clip_source_listener, a);
    static const char *const mimes[] = {
        "x-special/gnome-copied-files",
        "text/uri-list",
        "text/plain;charset=utf-8",
        "text/plain",
        "UTF8_STRING",
        "STRING",
        "TEXT",
    };
    for (unsigned i = 0; i < sizeof mimes / sizeof mimes[0]; i++)
        wl_data_source_offer(src, mimes[i]);
    a->clip_source = src;
    wl_data_device_set_selection(a->data_device, src, serial);
    if (old) wl_data_source_destroy(old);
    wl_display_flush(a->display);
}

/* Cut/Copy freezes the selected paths here.  Later selection changes cannot
 * make a pending paste send the wrong payload. */
static void clipboard_publish(App *a)
{
    clip_snapshot_files(a, a->clip, a->n_clip, a->clip_cut);
    clipboard_publish_snapshot(a);
}

static int clipboard_has(App *a)
{
    return a->n_clip > 0 || a->clip_source || a->clip_snap.has_text ||
           (a->clip_ctx && (a->clip_ctx->uris || a->clip_ctx->gnome ||
                            a->clip_ctx->text));
}

static void clip_read_cancel(App *a)
{
    if (a->clip_read.fd >= 0) {
        close(a->clip_read.fd);
        a->clip_read.fd = -1;
    }
    a->clip_read.len = 0;
    a->clip_read.overflow = 0;
}

static int clipboard_store_payload(App *a, const char *buf, int gnome,
                                   int *cut)
{
    *cut = 0;
    if (!buf || !*buf) return 0;

    const char *lines = buf;
    if (gnome) {
        const char *nl = strchr(buf, '\n');
        if (!nl) return 0;
        size_t vlen = (size_t)(nl - buf);
        if (vlen == 3 && !strncmp(buf, "cut", 3)) *cut = 1;
        lines = nl + 1;
    }

    int count = 0;
    char *save = NULL;
    char *copy = strdup(lines);
    if (!copy) return 0;
    for (char *line = strtok_r(copy, "\r\n", &save);
         line && count < 64; line = strtok_r(NULL, "\r\n", &save)) {
        if (!*line || *line == '#') continue;
        char path[PATH_MAX];
        if (!strncmp(line, "file://", 7))
            dnd_uri_to_path(line, path, sizeof path);
        else
            snprintf(path, sizeof path, "%s", line);
        if (!*path || access(path, F_OK) != 0) continue;
        snprintf(a->clip[count], PATH_MAX, "%s", path);
        count++;
    }
    free(copy);
    return count;
}

/* Nonblocking replacement for the old blocking clipboard_read_offer(). */
static int clipboard_start_async(App *a)
{
    DataOffer *ctx = a->clip_ctx;
    if (!ctx || !(ctx->uris || ctx->gnome || ctx->text)) return 0;

    a->clip_cache_valid = 0;
    const char *mime;
    if (ctx->gnome) mime = "x-special/gnome-copied-files";
    else if (ctx->uris) mime = "text/uri-list";
    else if (ctx->text_mime[0]) mime = ctx->text_mime;
    else mime = "text/plain;charset=utf-8";

    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return 0;
    wl_data_offer_receive(ctx->offer, mime, p[1]);
    close(p[1]);
    int flags = fcntl(p[0], F_GETFL, 0);
    if (flags >= 0) fcntl(p[0], F_SETFL, flags | O_NONBLOCK);
    wl_display_flush(a->display);

    clip_read_cancel(a);
    ClipRead *r = &a->clip_read;
    r->fd = p[0];
    r->len = 0;
    r->overflow = 0;
    r->attempt++;
    r->gnome = ctx->gnome ? 1 : 0;
    r->deadline = now_sec() + 5.0;
    return 1;
}

/* Called from the main poll loop whenever the transfer fd is readable or
 * its deadline has passed.  The compositor keeps the UI loop alive while
 * the source process writes, so we never block inside a key handler. */
static void clip_read_drain(App *a)
{
    ClipRead *r = &a->clip_read;
    if (r->fd < 0) return;

    for (;;) {
        if (r->len + 1 >= sizeof r->buf) {
            r->overflow = 1;
            break;
        }
        ssize_t n = read(r->fd, r->buf + r->len,
                         sizeof r->buf - 1 - r->len);
        if (n > 0) { r->len += (size_t)n; continue; }
        if (n == 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (now_sec() >= r->deadline) {
                status_set(a, "Clipboard source timed out");
                clip_read_cancel(a);
                paste_place_clear(a);
            }
            return;
        }
        break;
    }

    r->buf[r->len] = 0;
    int gnome = r->gnome;
    int empty = (r->len == 0);
    int overflow = r->overflow;
    close(r->fd);
    r->fd = -1;
    r->len = 0;

    if (overflow) {
        status_set(a, "Clipboard payload is too large");
        paste_place_clear(a);
        return;
    }

    /* A layer-shell focus/menu change can make the offer inert just before
     * we send receive(), which shows up as an immediate zero-byte EOF.  Let
     * the compositor finish that churn and, if it replaced the selection
     * offer, retry against the live one.  Cap the retries so a broken source
     * cannot keep us in a loop. */
    if (empty && r->attempt < 3) {
        DataOffer *old = a->clip_ctx;
        if (wl_display_roundtrip(a->display) == 0 &&
            a->clip_ctx && a->clip_ctx != old &&
            (a->clip_ctx->uris || a->clip_ctx->gnome || a->clip_ctx->text) &&
            clipboard_start_async(a))
            return;
    }

    int cut = 0;
    int count = clipboard_store_payload(a, r->buf, gnome, &cut);
    if (!count) {
        status_set(a, "Clipboard has no file paths");
        paste_place_clear(a);
        return;
    }
    a->n_clip = count;
    a->clip_cut = cut;
    a->clip_cache_valid = 1;   /* GTK-style per-offer buffer cache */
    do_paste(a);
}

/* Pull a remote selection into a pending async read, or use local state.
 * A zero return with an active offer means paste will finish later. */
static int clipboard_fetch(App *a, int *cut)
{
    *cut = 0;
    /* If we own the selection, our staged payload wins even before the
     * compositor echoes the selection event back to us. */
    if (a->clip_source) {
        if (a->n_clip > 0) {
            *cut = a->clip_cut;
            return a->n_clip;
        }
        if (a->clip_snap.has_text && !a->clip_snap.n) {
            int c = 0;
            int n = clipboard_store_payload(a, a->clip_snap.text, 0, &c);
            if (n) return n;
        }
    }
    /* A completed read is cached until the selection changes, just as GTK
     * caches each SelectionBuffer on the wl_data_offer. */
    if (a->clip_ctx && a->clip_cache_valid && a->n_clip > 0) {
        *cut = a->clip_cut;
        return a->n_clip;
    }
    if (a->clip_ctx && (a->clip_ctx->uris || a->clip_ctx->gnome ||
                        a->clip_ctx->text)) {
        /* Closing a menu or changing keyboard focus can replace the offer
         * between selection() and this paste callback.  Settle pending
         * Wayland traffic first so receive() lands on a live offer. */
        if (wl_display_roundtrip(a->display) < 0) return 0;
        if (!a->clip_ctx) {
            status_set(a, "Clipboard is no longer available");
            return 0;
        }
        a->clip_read.attempt = 0;   /* new paste, not an empty-offer retry */
        if (clipboard_start_async(a)) return 0;
    }
    if (a->n_clip > 0) {
        *cut = a->clip_cut;
        return a->n_clip;
    }
    if (a->clip_snap.has_text && !a->clip_snap.n) {
        int c = 0;
        int n = clipboard_store_payload(a, a->clip_snap.text, 0, &c);
        if (n) return n;
    }
    return 0;
}

/* Build the list of selected file URIs for the outgoing drag.  GNOME's
 * copied-files target is LF-only; text/uri-list requires CRLF. */
static size_t dnd_selected_uris(Out *o, char *buf, size_t cap,
                                const char *eol)
{
    size_t len = 0;
    for (int i = 0; i < o->n && len + 300 < cap; i++) {
        DIcon *ic = &o->icons[i];
        if (!ic->selected || ic->x < 0) continue;
        char uri[PATH_MAX + 32];
        dnd_path_to_uri(ic->path, uri, sizeof uri);
        int n = snprintf(buf + len, cap - len, "%s%s", uri, eol);
        if (n <= 0 || (size_t)n >= cap - len) break;
        len += (size_t)n;
    }
    return len;
}

static void dnd_source_send(void *data, struct wl_data_source *source,
                            const char *mime, int fd)
{
    (void)source;
    App *a = data;
    Out *o = a->dnd_src_out;
    if (!o) { close(fd); return; }

    if (!strcmp(mime, "text/uri-list") ||
        !strcmp(mime, "text/plain;charset=utf-8") ||
        !strcmp(mime, "text/plain")) {
        char buf[16384];
        size_t len = dnd_selected_uris(o, buf, sizeof buf, "\r\n");
        dnd_write_all(fd, buf, len);
    } else if (!strcmp(mime, "x-special/gnome-copied-files")) {
        char buf[16384];
        size_t len = 0;
        const char *verb = a->dnd_action ==
            WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE ? "cut\n" : "copy\n";
        size_t vn = strlen(verb);
        memcpy(buf, verb, vn); len = vn;
        len += dnd_selected_uris(o, buf + len, sizeof buf - len, "\n");
        dnd_write_all(fd, buf, len);
    } else {
        close(fd);
        return;
    }
    close(fd);
}

static void dnd_source_target(void *data, struct wl_data_source *source,
                              const char *mime)
{ (void)data; (void)source; (void)mime; }

static void dnd_source_cancelled(void *data, struct wl_data_source *source)
{
    App *a = data;
    if (a->dnd_source == source) {
        wl_data_source_destroy(source);
        a->dnd_source = NULL;
    }
    dnd_icon_destroy(a);
    a->dnd_src_out = NULL;
    a->dnd_pending = 0;
}

static void dnd_source_drop_performed(void *data, struct wl_data_source *source)
{ (void)data; (void)source; }

static void dnd_source_finished(void *data, struct wl_data_source *source)
{
    App *a = data;
    if (a->dnd_source == source) {
        wl_data_source_destroy(source);
        a->dnd_source = NULL;
    }
    dnd_icon_destroy(a);
    a->dnd_src_out = NULL;
    a->dnd_pending = 0;
}

static void dnd_source_action(void *data, struct wl_data_source *source,
                              uint32_t action)
{
    (void)source;
    App *a = data;
    a->dnd_action = action;
}

static const struct wl_data_source_listener dnd_source_listener = {
    .target = dnd_source_target, .send = dnd_source_send,
    .cancelled = dnd_source_cancelled,
    .dnd_drop_performed = dnd_source_drop_performed,
    .dnd_finished = dnd_source_finished, .action = dnd_source_action,
};

/* Create a compositor-managed drag icon showing the first selected item. */
static struct wl_surface *dnd_icon_create(App *a, Out *o)
{
    DIcon *sel = NULL;
    for (int i = 0; i < o->n; i++)
        if (o->icons[i].selected && o->icons[i].x >= 0) {
            sel = &o->icons[i]; break;
        }
    if (!sel) return NULL;

    int sz = a->icon_size;
    if (!buf_ensure(a, &a->dnd_icon_buf, sz, sz)) return NULL;
    Buf *b = &a->dnd_icon_buf;

    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        b->data, CAIRO_FORMAT_ARGB32, sz, sz, sz * 4);
    if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(cs);
        return NULL;
    }
    cairo_t *cr = cairo_create(cs);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    jti_icon_draw(cr, sel->icon, (double)sz, 0, 0, &a->pal, 1, sel->name);
    cairo_destroy(cr);
    cairo_surface_flush(cs);
    cairo_surface_destroy(cs);

    if (!a->dnd_icon_surface)
        a->dnd_icon_surface = wl_compositor_create_surface(a->compositor);
    if (!a->dnd_icon_surface) return NULL;
    wl_surface_attach(a->dnd_icon_surface, b->buf, 0, 0);
    wl_surface_damage(a->dnd_icon_surface, 0, 0, sz, sz);
    if (wl_surface_get_version(a->dnd_icon_surface) >= 3)
        wl_surface_set_buffer_scale(a->dnd_icon_surface, 1);
    wl_surface_commit(a->dnd_icon_surface);
    b->busy = 1;
    return a->dnd_icon_surface;
}

static void dnd_icon_destroy(App *a)
{
    if (a->dnd_icon_surface) {
        wl_surface_destroy(a->dnd_icon_surface);
        a->dnd_icon_surface = NULL;
    }
    buf_destroy(a, &a->dnd_icon_buf);
}

/* Start a real Wayland drag after the drag threshold is exceeded. */
static void dnd_begin(App *a, Out *o)
{
    if (!a->dnd_pending || a->dnd_source || !a->data_device_manager ||
        !a->data_device) return;
    int nsel = 0;
    for (int i = 0; i < o->n; i++)
        if (o->icons[i].selected && o->icons[i].x >= 0) nsel++;
    if (!nsel) { a->dnd_pending = 0; return; }

    struct wl_data_source *src =
        wl_data_device_manager_create_data_source(a->data_device_manager);
    if (!src) return;
    wl_data_source_add_listener(src, &dnd_source_listener, a);
    wl_data_source_offer(src, "text/uri-list");
    wl_data_source_offer(src, "x-special/gnome-copied-files");
    wl_data_source_offer(src, "text/plain;charset=utf-8");
    if (wl_data_source_get_version(src) >= WL_DATA_SOURCE_SET_ACTIONS_SINCE_VERSION)
        wl_data_source_set_actions(src,
            WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
            WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    a->dnd_source = src;
    a->dnd_pending = 0;
    a->dnd_action = 0;

    struct wl_surface *icon = dnd_icon_create(a, o);
    a->dnd_src_out = o;
    a->dnd_drop_out = NULL;
    wl_data_device_start_drag(a->data_device, src, o->surf.wl, icon,
                              a->dnd_press_serial);
    wl_display_flush(a->display);

    /* Compositor takes over pointer now; stop our own visual drag. */
    o->dragging = 0;
    o->drag_moved = 0;
    o->drag_dx = o->drag_dy = 0;
    out_mark(o);
}

/* ---- incoming drag: copy files to the desktop ---- */

static int dnd_receive_uris(App *a, struct wl_data_offer *offer)
{
    /* Snapshot the drop target before the blocking pipe read; the data
     * device keep the offer alive until drop returns, but being explicit
     * avoids depending on that detail. */
    Out *dst = a->dnd_drop_out;
    int have_drop = dst && dst->width > 0 && dst->height > 0;
    double drop_x = a->dnd_drop_x, drop_y = a->dnd_drop_y;

    const char *mime = a->dnd_have_gnome ? "x-special/gnome-copied-files"
                                         : "text/uri-list";
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return 0;
    wl_data_offer_receive(offer, mime, p[1]);
    close(p[1]);
    wl_display_flush(a->display);

    char buf[16384];
    size_t len = 0;
    struct pollfd pf = { .fd = p[0], .events = POLLIN };
    while (len < sizeof buf - 1) {
        if (poll(&pf, 1, 2000) <= 0) break;
        ssize_t n = read(p[0], buf + len, sizeof buf - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(p[0]);
    buf[len] = 0;

    /* "cut" in the gnome target means move.  We do the move ourselves, so
     * the source app never needs to delete the originals after the drop. */
    int cut = 0;
    char *lines = buf;
    if (a->dnd_have_gnome) {
        char *nl = strchr(buf, '\n');
        if (nl) {
            *nl = 0;
            cut = !strcmp(buf, "cut") || !strncmp(buf, "cut\r", 4);
            lines = nl + 1;
        }
    }

    char copied[64][NAME_MAX + 1];
    int ncopied = 0;
    char *save = NULL;
    for (char *line = strtok_r(lines, "\r\n", &save);
         line && ncopied < 64; line = strtok_r(NULL, "\r\n", &save)) {
        if (!*line || *line == '#') continue;
        char path[PATH_MAX];
        dnd_uri_to_path(line, path, sizeof path);
        if (!*path || access(path, F_OK) != 0) continue;
        char target[PATH_MAX];
        unique_path(a->desktop_dir, path_base(path), target, sizeof target);
        if (!strcmp(path, target)) continue;
        if (cut) {
            if (rename(path, target) != 0) {
                copy_recursive(path, target);
                if (access(target, F_OK) == 0) remove_tree(path);
            }
        } else {
            copy_recursive(path, target);
        }
        snprintf(copied[ncopied++], sizeof copied[0], "%s", path_base(target));
    }
    if (!ncopied) return 0;

    post_sel_arm(a, a->desktop_dir, copied, ncopied);

    /* Every output shows the same desktop directory, so refresh them all;
     * only the output the files were dropped on gets the explicit position. */
    for (int i = 0; i < a->n_outs; i++) out_load_icons(&a->outs[i], 1);

    if (have_drop) {
        place_new_icons(a, dst, copied, ncopied, drop_x, drop_y);
    } else {
        state_save(a);
        for (int i = 0; i < a->n_outs; i++) out_mark(&a->outs[i]);
    }
    return 1;
}

/* Icon under the drop point, ignoring everything that is being dragged.
 * Selection is per-output, so match the source's dragged paths as well to
 * avoid treating one of them as a target on another monitor. */
static DIcon *dnd_drop_target(App *a, Out *src, Out *dst)
{
    if (!dst || !g_app->show_icons) return NULL;
    int cw = cell_w(), ch = cell_h();
    for (int i = dst->n - 1; i >= 0; i--) {
        DIcon *ic = &dst->icons[i];
        if (ic->x < 0 || ic->y < 0 || ic->selected) continue;
        int dragged = 0;
        for (int j = 0; src && j < src->n; j++) {
            DIcon *s = &src->icons[j];
            if (!s->selected || s->x < 0) continue;
            if (!strcmp(s->path, ic->path) || !strcmp(s->key, ic->key)) {
                dragged = 1;
                break;
            }
        }
        if (dragged) continue;
        if (a->dnd_drop_x >= ic->x && a->dnd_drop_x < ic->x + cw &&
            a->dnd_drop_y >= ic->y && a->dnd_drop_y < ic->y + ch)
            return ic;
    }
    return NULL;
}

static int dnd_path_within(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    return !strncmp(path, dir, n) && (path[n] == '\0' || path[n] == '/');
}

/* Move the dragged desktop icons into the folder they were dropped on. */
static int dnd_move_into_folder(App *a, Out *src, DIcon *folder,
                                char *first_path, size_t first_n)
{
    char folder_path[PATH_MAX];
    snprintf(folder_path, sizeof folder_path, "%s", folder->path);
    char moved_keys[64][256];
    char moved_paths[64][PATH_MAX];
    int n = 0;

    for (int i = 0; i < src->n && n < 64; i++) {
        DIcon *ic = &src->icons[i];
        if (!ic->selected || ic->x < 0) continue;
        if (dnd_path_within(folder_path, ic->path)) continue;

        char dst[PATH_MAX];
        unique_path(folder_path, path_base(ic->path), dst, sizeof dst);
        if (!strcmp(ic->path, dst)) continue;

        int ok = 0;
        if (rename(ic->path, dst) == 0) {
            ok = 1;
        } else if (errno == EXDEV) {
            copy_recursive(ic->path, dst);
            if (access(dst, F_OK) == 0) {
                remove_tree(ic->path);
                ok = 1;
            }
        }
        if (!ok) continue;

        snprintf(moved_keys[n], sizeof moved_keys[0], "%s", ic->key);
        snprintf(moved_paths[n], sizeof moved_paths[0], "%s", dst);
        n++;
    }
    if (!n) return 0;
    snprintf(first_path, first_n, "%s", moved_paths[0]);

    /* Forget the old desktop cells before reloading, otherwise a later
     * file with the same name could inherit this icon's saved position. */
    char sec[160];
    for (int k = 0; k < n; k++)
        for (int i = 0; i < a->n_outs; i++) {
            if (!a->outs[i].key[0]) continue;
            state_mon_section(a->outs[i].key, sec, sizeof sec);
            state_set(sec, moved_keys[k], "");
        }

    for (int i = 0; i < a->n_outs; i++) out_load_icons(&a->outs[i], 1);
    state_save(a);
    for (int i = 0; i < a->n_outs; i++) out_mark(&a->outs[i]);
    return n;
}

/* Reposition the dragged desktop icons to the internal drop cell. */
static void dnd_internal_drop(App *a)
{
    Out *src = a->dnd_src_out;    /* where the icons came from */
    /* Normally enter() sets this to our surface. If the compositor did not
     * send an enter for the origin surface, treat it as a drop back on the
     * desktop we started from instead of losing the movement. */
    Out *dst = a->dnd_drop_out ? a->dnd_drop_out : a->dnd_src_out;
    a->dnd_drop_out = NULL;
    if (!src || !dst) return;
    out_clear_hover(src);
    if (dst != src) out_clear_hover(dst);

    DIcon *target = dnd_drop_target(a, src, dst);
    if (target && target->is_dir) {
        char selected[PATH_MAX];
        if (dnd_move_into_folder(a, src, target, selected, sizeof selected)) {
            /* jtfiles is the default folder opener and now understands
             * --select, so open the destination with the moved file lit. */
            const char *av[] = { "jtfiles", "--select", selected, NULL };
            spawn_detached(av);
        }
        return;
    }
    if (target) {
        /* Dropped on a non-folder icon.  The compositor drag never changed
         * the desktop cells, so leaving them alone makes the file snap back. */
        out_mark(src);
        if (src != dst) out_mark(dst);
        return;
    }
    /* Keep the pointer's offset inside the first dragged icon so the group
     * lands where the user dropped it rather than jumping to the cell origin. */
    DIcon *anchor = NULL;
    for (int i = 0; i < src->n; i++) {
        if (src->icons[i].selected && src->icons[i].x >= 0) {
            anchor = &src->icons[i]; break;
        }
    }
    int off_x = 0, off_y = 0;
    if (anchor) {
        off_x = (int)lround(a->dnd_drop_x) - anchor->x;
        off_y = (int)lround(a->dnd_drop_y) - anchor->y;
    }
    for (int i = 0; i < src->n; i++) {
        DIcon *ic = &src->icons[i];
        if (!ic->selected || ic->x < 0) continue;
        ic->x += off_x; ic->y += off_y;
        snap_xy(&ic->x, &ic->y);
        ic->want_x = ic->x; ic->want_y = ic->y;
        ic->auto_placed = 0;
        clamp_icon(dst, ic);
    }
    resolve_overlaps(dst, 1);
    state_save(a);
    out_mark(dst);
    if (src != dst) out_mark(src);
}

/* MIME events arrive before enter()/selection(), so record them on the
 * offer context rather than in a shared "new offer" slot. */
static void dnd_offer_offer(void *data, struct wl_data_offer *offer,
                            const char *mime)
{
    (void)offer;
    DataOffer *ctx = data;
    if (!strcmp(mime, "text/uri-list")) ctx->uris = 1;
    if (!strcmp(mime, "x-special/gnome-copied-files")) ctx->gnome = 1;
    if (clip_is_text_mime(mime)) {
        ctx->text = 1;
        if (!ctx->text_mime[0])
            snprintf(ctx->text_mime, sizeof ctx->text_mime, "%s", mime);
    }
}

static void dnd_offer_source_actions(void *data, struct wl_data_offer *offer,
                                     uint32_t actions)
{ (void)data; (void)offer; (void)actions; }

static void dnd_offer_action(void *data, struct wl_data_offer *offer,
                             uint32_t action)
{
    (void)offer;
    DataOffer *ctx = data;
    if (ctx->app->dnd_ctx == ctx)
        ctx->app->dnd_offer_action = action;
}

static const struct wl_data_offer_listener dnd_offer_listener = {
    .offer = dnd_offer_offer, .source_actions = dnd_offer_source_actions,
    .action = dnd_offer_action,
};

static DataOffer *data_offer_create(App *a, struct wl_data_offer *offer)
{
    DataOffer *ctx = calloc(1, sizeof *ctx);
    if (!ctx) { wl_data_offer_destroy(offer); return NULL; }
    ctx->app = a;
    ctx->offer = offer;
    ctx->next = a->offers;
    a->offers = ctx;
    wl_data_offer_add_listener(offer, &dnd_offer_listener, ctx);
    return ctx;
}

static DataOffer *data_offer_find(App *a, struct wl_data_offer *offer)
{
    for (DataOffer *c = a->offers; c; c = c->next)
        if (c->offer == offer) return c;
    return NULL;
}

static void data_offer_destroy(App *a, DataOffer *ctx)
{
    if (!ctx) return;
    DataOffer **link = &a->offers;
    while (*link && *link != ctx) link = &(*link)->next;
    if (*link) *link = ctx->next;
    if (a->clip_ctx == ctx) {
        a->clip_ctx = NULL;
        a->clip_offer = NULL;
        a->clip_cache_valid = 0;
        clip_read_cancel(a);
    }
    if (a->dnd_ctx == ctx) {
        a->dnd_ctx = NULL;
        a->dnd_offer = NULL;
    }
    wl_data_offer_destroy(ctx->offer);
    free(ctx);
}

/* Offers that never became active (focus changes, cancelled drags) would
 * otherwise live until exit.  Keep the active contexts and the new one. */
static void data_offer_prune(App *a, DataOffer *keep)
{
    DataOffer *c = a->offers;
    while (c) {
        DataOffer *next = c->next;
        if (c != keep && c != a->clip_ctx && c != a->dnd_ctx)
            data_offer_destroy(a, c);
        c = next;
    }
}

static void data_offer_destroy_all(App *a)
{
    clip_read_cancel(a);
    a->clip_cache_valid = 0;
    while (a->offers) data_offer_destroy(a, a->offers);
    a->clip_ctx = a->dnd_ctx = NULL;
    a->clip_offer = a->dnd_offer = NULL;
}

static void dnd_data_device_enter(void *data, struct wl_data_device *device,
                                  uint32_t serial, struct wl_surface *surface,
                                  wl_fixed_t x, wl_fixed_t y,
                                  struct wl_data_offer *offer)
{
    (void)device;
    App *a = data;
    if (a->dnd_ctx) data_offer_destroy(a, a->dnd_ctx);
    DataOffer *ctx = data_offer_find(a, offer);
    if (!ctx) ctx = data_offer_create(a, offer);
    a->dnd_ctx = ctx;
    a->dnd_offer = ctx ? offer : NULL;
    a->dnd_enter_serial = serial;
    a->dnd_offer_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    a->dnd_drop_out = NULL;
    if (!ctx) return;
    /* Mime events were delivered before enter(); use what we recorded. */
    a->dnd_accept = ctx->uris || ctx->gnome;
    a->dnd_have_gnome = ctx->gnome;
    a->dnd_accepted = 0;
    if (a->dnd_accept) {
        wl_data_offer_accept(offer, serial, a->dnd_have_gnome
                             ? "x-special/gnome-copied-files" : "text/uri-list");
        a->dnd_accepted = 1;
        if (wl_data_offer_get_version(offer) >= WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION)
            wl_data_offer_set_actions(offer,
                WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE,
                WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    }
    data_offer_prune(a, ctx);
    /* Is this drag entering one of our own desktop surfaces? */
    for (int i = 0; i < a->n_outs; i++) {
        if (a->outs[i].surf.wl == surface) {
            a->dnd_drop_out = &a->outs[i];
            a->dnd_drop_x = wl_fixed_to_double(x);
            a->dnd_drop_y = wl_fixed_to_double(y);
            break;
        }
    }
}

static void dnd_data_device_motion(void *data, struct wl_data_device *device,
                                   uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)device; (void)time;
    App *a = data;
    a->dnd_drop_x = wl_fixed_to_double(x);
    a->dnd_drop_y = wl_fixed_to_double(y);
}

/* v3 offers must be finished, otherwise wlroots turns a successful drop into
 * wl_data_source.cancelled on the source side instead of dnd_finished. */
static void dnd_offer_release(App *a, int success)
{
    DataOffer *ctx = a->dnd_ctx;
    if (ctx) {
        struct wl_data_offer *offer = ctx->offer;
        if (success && a->dnd_accepted &&
            wl_data_offer_get_version(offer) >= WL_DATA_OFFER_FINISH_SINCE_VERSION &&
            a->dnd_offer_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE &&
            a->dnd_offer_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
            wl_data_offer_finish(offer);
        data_offer_destroy(a, ctx);
    }
    a->dnd_accept = a->dnd_accepted = 0;
    a->dnd_offer_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    a->dnd_drop_out = NULL;
    data_offer_prune(a, NULL);
}

static void dnd_data_device_leave(void *data, struct wl_data_device *device)
{
    (void)device;
    App *a = data;
    if (a->dnd_ctx) data_offer_destroy(a, a->dnd_ctx);
    a->dnd_accept = a->dnd_accepted = 0;
    a->dnd_offer_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    a->dnd_drop_out = NULL;
    data_offer_prune(a, NULL);
}

static void dnd_data_device_drop(void *data, struct wl_data_device *device)
{
    (void)device;
    App *a = data;
    /* If this is our own drag coming back to the desktop, reposition icons.
     * An incoming drag from another app must copy files even though it
     * entered our surface (dnd_drop_out is set for any drag on our surface). */
    int ok = 0;
    if (a->dnd_source) {
        dnd_internal_drop(a);
        ok = 1;
    } else if (a->dnd_offer && a->dnd_accept) {
        ok = dnd_receive_uris(a, a->dnd_offer);
    }
    dnd_offer_release(a, ok);
}

static void dnd_data_device_selection(void *data, struct wl_data_device *device,
                                      struct wl_data_offer *offer)
{
    (void)device;
    App *a = data;
    clip_read_cancel(a);
    a->clip_cache_valid = 0;
    if (a->clip_ctx) data_offer_destroy(a, a->clip_ctx);

    /* The compositor echoes our own selection back to us.  Keep the staged
     * clipboard payload: clearing it here would make the next local paste
     * fail after copy/cut. */
    if (a->clip_source) {
        if (offer) {
            DataOffer *ctx = data_offer_find(a, offer);
            if (ctx) data_offer_destroy(a, ctx);
            else data_offer_create(a, offer);
        }
        data_offer_prune(a, NULL);
        return;
    }

    a->n_clip = 0;
    a->clip_cut = 0;
    if (offer) {
        DataOffer *ctx = data_offer_find(a, offer);
        if (!ctx) ctx = data_offer_create(a, offer);
        if (ctx && (ctx->uris || ctx->gnome || ctx->text)) {
            a->clip_ctx = ctx;
            a->clip_offer = offer;
        } else if (ctx) {
            data_offer_destroy(a, ctx);
        }
    }
    data_offer_prune(a, NULL);
}

static void dnd_data_device_data_offer(void *data,
                                       struct wl_data_device *device,
                                       struct wl_data_offer *offer)
{
    (void)device;
    App *a = data;
    /* Create a context now; MIME events that follow are recorded on it. */
    data_offer_create(a, offer);
}

static const struct wl_data_device_listener dnd_data_device_listener = {
    .data_offer = dnd_data_device_data_offer,
    .enter = dnd_data_device_enter, .leave = dnd_data_device_leave,
    .motion = dnd_data_device_motion, .drop = dnd_data_device_drop,
    .selection = dnd_data_device_selection,
};

/* ---- registry / seat ----------------------------------------------------- */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    App *a = data;
    if (a->data_device_manager && !a->data_device) {
        a->data_device = wl_data_device_manager_get_data_device(
            a->data_device_manager, seat);
        if (a->data_device)
            wl_data_device_add_listener(a->data_device,
                                        &dnd_data_device_listener, a);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->pointer) {
        a->pointer = wl_seat_get_pointer(seat);
        if (a->pointer)
            wl_pointer_add_listener(a->pointer, &pointer_listener, a);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && a->pointer) {
        wl_pointer_destroy(a->pointer); a->pointer = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !a->keyboard) {
        a->keyboard = wl_seat_get_keyboard(seat);
        if (a->keyboard)
            wl_keyboard_add_listener(a->keyboard, &keyboard_listener, a);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && a->keyboard) {
        wl_keyboard_destroy(a->keyboard); a->keyboard = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{ (void)data; (void)seat; (void)name; }

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities, .name = seat_name,
};

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void registry_global(void *data, struct wl_registry *reg, uint32_t id,
                            const char *iface, uint32_t ver)
{
    App *a = data;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        if (!a->compositor)
            a->compositor = wl_registry_bind(reg, id, &wl_compositor_interface,
                                             ver < 5 ? ver : 5);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        if (!a->shm) a->shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        if (!a->layer_shell)
            a->layer_shell = wl_registry_bind(reg, id,
                &zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        if (!a->wm_base) {
            a->wm_base = wl_registry_bind(reg, id, &xdg_wm_base_interface,
                                          ver < 3 ? ver : 3);
            if (a->wm_base)
                xdg_wm_base_add_listener(a->wm_base, &wm_listener, a);
        }
    } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
        if (!a->deco_manager)
            a->deco_manager = wl_registry_bind(reg, id,
                &zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        if (!a->seat) {
            a->seat = wl_registry_bind(reg, id, &wl_seat_interface,
                                       ver < 4 ? ver : 4);
            wl_seat_add_listener(a->seat, &seat_listener, a);
        }
    } else if (!strcmp(iface, wl_output_interface.name)) {
        if (a->n_outs < MAX_OUTPUTS) {
            Out *o = &a->outs[a->n_outs++];
            memset(o, 0, sizeof *o);
            o->app = a; o->id = (int)id; o->scale = 1;
            o->hover = o->press = -1;
            o->output = wl_registry_bind(reg, id, &wl_output_interface,
                                         ver < 3 ? ver : 3);
            wl_output_add_listener(o->output, &output_listener, o);
            snprintf(o->key, sizeof o->key, "output%d", a->n_outs - 1);
        }
    } else if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name)) {
        if (!a->cursor_manager)
            a->cursor_manager = wl_registry_bind(reg, id,
                &wp_cursor_shape_manager_v1_interface, 1);
    } else if (!strcmp(iface, wl_data_device_manager_interface.name)) {
        if (!a->data_device_manager)
            a->data_device_manager = wl_registry_bind(reg, id,
                &wl_data_device_manager_interface,
                ver < 3 ? ver : 3);
    }
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t id)
{ (void)data; (void)reg; (void)id; }

static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_remove,
};

/* ---- control socket ------------------------------------------------------ */

static void ctl_path(App *a)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) rt = "/tmp";
    snprintf(a->ctl_path, sizeof a->ctl_path, "%s/jtwpi-ctl.sock", rt);
}

static void ctl_handle(App *a, const char *line, int *quit)
{
    char buf[128];
    snprintf(buf, sizeof buf, "%s", line);
    trim(buf);
    if (!*buf) return;
    if (!strcmp(buf, "quit")) { *quit = 1; return; }
    if (!strcmp(buf, "reload")) { appearance_config_load(a); return; }
    const char *c = buf;
    if (!strncmp(c, "accent", 6)) {
        c += 6;
        while (*c == ' ' || *c == '\t') c++;
    }
    unsigned rgb;
    if (accent_parse(c, &rgb)) accent_set(a, rgb);
}

static void ctl_accept(App *a)
{
    int fd = accept4(a->ctl_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) return;
    char buf[512];
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    if (n > 0) {
        buf[n] = 0;
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save))
            ctl_handle(a, line, &a->quit);
    }
    close(fd);
}

static int ctl_init(App *a)
{
    ctl_path(a);
    /* Probe first: never steal the socket of a running instance. */
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe >= 0) {
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        snprintf(addr.sun_path, sizeof addr.sun_path, "%s", a->ctl_path);
        if (connect(probe, (struct sockaddr *)&addr, sizeof addr) == 0) {
            close(probe);
            return 0; /* another jtwpi owns the socket */
        }
        close(probe);
        struct stat st;
        if (lstat(a->ctl_path, &st) == 0 && S_ISSOCK(st.st_mode))
            unlink(a->ctl_path); /* stale socket */
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", a->ctl_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    a->ctl_fd = fd;
    a->ctl_created = 1;
    return 0;
}

static void ctl_cleanup(App *a)
{
    if (a->ctl_fd >= 0) { close(a->ctl_fd); a->ctl_fd = -1; }
    if (a->ctl_created) {
        struct stat st, lst;
        if (lstat(a->ctl_path, &lst) == 0 && S_ISSOCK(lst.st_mode) &&
            stat(a->ctl_path, &st) == 0)
            unlink(a->ctl_path);
        a->ctl_created = 0;
    }
}

/* ---- main loop ----------------------------------------------------------- */

static void check_dir(App *a)
{
    /* A Desktop change mid-drag would free every icon (and its cached
     * surfaces) and relayout them, which shows up as a bad hitch. Defer it
     * until the button is released. */
    for (int i = 0; i < a->n_outs; i++)
        if (a->outs[i].dragging || a->outs[i].selecting) return;

    struct stat st;
    if (stat(a->desktop_dir, &st) != 0) return;
    if (a->dir_mtime && st.st_mtime == a->dir_mtime) return;
    a->dir_mtime = st.st_mtime;
    for (int i = 0; i < a->n_outs; i++) out_load_icons(&a->outs[i], 1);
}

static void usage(const char *p)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS] [ACCENT]\n"
        "  --desktop PATH       Desktop directory (default: XDG desktop dir)\n"
        "  --icon-size N        Icon size, 24-128 (default %d)\n"
        "  --accent COLOR       Initial accent #RRGGBB\n"
        "  --hidden             Start with desktop icons hidden\n"
        "  --version, --help\n"
        "Live accent/quit: $XDG_RUNTIME_DIR/jtwpi-ctl.sock (see jtwpictl)\n",
        p, DEFAULT_ICON_SIZE);
}

int main(int argc, char **argv)
{
    /* Re-exec'd progress workers: no Wayland, just a framed request on stdin
     * and framed progress on stdout. */
    if (argc >= 2 && !strcmp(argv[1], "--xfer-child")) {
        signal(SIGPIPE, SIG_IGN);
        uint32_t cut32 = 0, n32 = 0;
        if (fread(&cut32, sizeof cut32, 1, stdin) != 1 ||
            fread(&n32, sizeof n32, 1, stdin) != 1 ||
            n32 == 0 || n32 > 64)
            return 1;
        static char srcs[64][PATH_MAX], dsts[64][PATH_MAX];
        for (uint32_t i = 0; i < n32; i++)
            if (!xfer_read_path(srcs[i], sizeof srcs[i]) ||
                !xfer_read_path(dsts[i], sizeof dsts[i]))
                return 1;
        xfer_child(STDOUT_FILENO, (int)cut32, srcs, dsts, (int)n32);
        return 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "--fs-child")) {
        signal(SIGPIPE, SIG_IGN);
        uint32_t op32 = 0, n32 = 0;
        if (fread(&op32, sizeof op32, 1, stdin) != 1 ||
            fread(&n32, sizeof n32, 1, stdin) != 1 ||
            n32 == 0 || n32 > 64 ||
            (op32 != XOP_TRASH && op32 != XOP_DELETE))
            return 1;
        static char paths[64][PATH_MAX];
        for (uint32_t i = 0; i < n32; i++)
            if (!xfer_read_path(paths[i], sizeof paths[i]))
                return 1;
        fs_child(STDOUT_FILENO, (int)op32, paths, (int)n32);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    App *a = xcalloc(1, sizeof *a);
    g_app = a;
    a->icon_size = DEFAULT_ICON_SIZE;
    a->snap = 1; a->show_icons = 1;
    a->ctl_fd = -1;
    a->xfer.fd = -1;
    a->clip_read.fd = -1;
    a->border_anim_enabled = 0;
    a->accent = DEFAULT_ACCENT;
    jti_palette_set(&a->pal, a->accent);

    appearance_config_load(a);
    /* Accent: config file first, CLI overrides. */
    accent_load_default(a);
    for (int i = 1; i < argc; i++) {
        const char *val = NULL;
        int consumed = 0;
        if (!strcmp(argv[i], "--accent")) {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            val = argv[++i]; consumed = 1;
        } else if (!strncmp(argv[i], "--accent=", 9)) {
            val = argv[i] + 9;
        } else if (argv[i][0] == '#') {
            val = argv[i];
        } else if (i == 1) {
            unsigned rgb;
            if (accent_parse(argv[i], &rgb)) val = argv[i];
        }
        if (!val) continue;
        unsigned rgb;
        if (!accent_parse(val, &rgb)) {
            fprintf(stderr, "%s: invalid accent color '%s'\n", argv[0], val);
            return 1;
        }
        accent_set(a, rgb);
        for (int j = i; j + consumed < argc; j++) argv[j] = argv[j + consumed];
        argc -= consumed; i--;
    }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) {
            printf("%s %s\n", APP_NAME, APP_VERSION); return 0;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]); return 0;
        } else if (!strcmp(argv[i], "--hidden")) {
            a->opt_hidden = 1;
        } else if (!strcmp(argv[i], "--desktop") && i + 1 < argc) {
            a->opt_desktop = xstrdup(argv[++i]);
        } else if (!strncmp(argv[i], "--desktop=", 10)) {
            a->opt_desktop = xstrdup(argv[i] + 10);
        } else if (!strcmp(argv[i], "--icon-size") && i + 1 < argc) {
            a->opt_size = (int)strtol(argv[++i], NULL, 10);
        } else if (!strncmp(argv[i], "--icon-size=", 12)) {
            a->opt_size = (int)strtol(argv[i] + 12, NULL, 10);
        }
    }

    a->desktop_dir = desktop_directory(a);
    mkdir(a->desktop_dir, 0755);
    state_load(a);
    a->show_icons = state_bool("general", "show_icons", 1);
    a->snap = state_bool("general", "snap", 1);
    a->icon_size = state_int("general", "icon_size", DEFAULT_ICON_SIZE);
    if (a->opt_size > 0) a->icon_size = a->opt_size;
    if (a->icon_size < MIN_ICON_SIZE) a->icon_size = MIN_ICON_SIZE;
    if (a->icon_size > MAX_ICON_SIZE) a->icon_size = MAX_ICON_SIZE;
    if (a->opt_hidden) a->show_icons = 0;
    jti_palette_set(&a->pal, a->accent);

    a->display = wl_display_connect(NULL);
    if (!a->display) {
        fprintf(stderr, "jtwpi: cannot connect to Wayland (WAYLAND_DISPLAY)\n");
        return 1;
    }
    a->registry = wl_display_get_registry(a->display);
    wl_registry_add_listener(a->registry, &registry_listener, a);
    wl_display_roundtrip(a->display); /* populate globals */

    if (!a->compositor || !a->shm || !a->layer_shell) {
        fprintf(stderr, "jtwpi: compositor missing wl_compositor/wl_shm/zwlr_layer_shell_v1\n");
        return 1;
    }

    a->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    for (int i = 0; i < a->n_outs; i++) {
        Out *o = &a->outs[i];
        create_output_surface(a, o);
    }
    wl_display_roundtrip(a->display); /* layer configure events */
    for (int i = 0; i < a->n_outs; i++) {
        Out *o = &a->outs[i];
        out_load_icons(o, 1);
        if (o->configured && o->width > 0)
            (void)surf_draw(a, &o->surf, o->width * o->scale, o->height * o->scale,
                            o->scale, paint_out, o, &o->dirty);
    }

    ctl_init(a);
    struct stat st;
    if (stat(a->desktop_dir, &st) == 0) a->dir_mtime = st.st_mtime;

    int wlfd = wl_display_get_fd(a->display);
    while (!a->quit) {
        wl_display_flush(a->display);
        struct pollfd fds[20];
        int write_idx[16], n_write_fds = 0;
        int nfds = 0;
        fds[nfds].fd = wlfd; fds[nfds].events = POLLIN; nfds++;
        int ctl_idx = -1, xfer_idx = -1, clip_idx = -1;
        if (a->ctl_fd >= 0) {
            ctl_idx = nfds;
            fds[nfds].fd = a->ctl_fd; fds[nfds].events = POLLIN; nfds++;
        }
        if (a->xfer.active && a->xfer.fd >= 0) {
            xfer_idx = nfds;
            fds[nfds].fd = a->xfer.fd; fds[nfds].events = POLLIN; nfds++;
        }
        if (a->clip_read.fd >= 0) {
            clip_idx = nfds;
            fds[nfds].fd = a->clip_read.fd; fds[nfds].events = POLLIN; nfds++;
        }
        for (ClipWrite *w = a->clip_writes; w && n_write_fds < 16; w = w->next) {
            write_idx[n_write_fds++] = nfds;
            fds[nfds].fd = w->fd; fds[nfds].events = POLLOUT; nfds++;
        }
        int timeout_ms = 1000;
        if (clip_idx >= 0) {
            double left = a->clip_read.deadline - now_sec();
            int ms = left <= 0.0 ? 0 : (int)(left * 1000.0);
            if (ms < timeout_ms) timeout_ms = ms;
        }
        if (a->xfer.active && a->xfer.show_pending) {
            double left = a->xfer.show_at - now_sec();
            int ms = left <= 0.0 ? 0 : (int)(left * 1000.0);
            if (ms < timeout_ms) timeout_ms = ms;
        }
        if (a->border_anim_enabled && a->post_sel_anim_start > 0.0 &&
            SEL_LAP_TICK_MS < timeout_ms)
            timeout_ms = SEL_LAP_TICK_MS;
        for (int i = 0; i < a->n_outs; i++)
            if (menu_tree_anim_active(a->outs[i].menu) &&
                SEL_LAP_TICK_MS < timeout_ms)
                timeout_ms = SEL_LAP_TICK_MS;
        int r = poll(fds, (nfds_t)nfds, timeout_ms);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_dispatch(a->display) < 0) break;
        } else if (fds[0].revents & POLLERR) {
            break;
        } else {
            if (wl_display_dispatch_pending(a->display) < 0) break;
        }
        if (ctl_idx >= 0 && (fds[ctl_idx].revents & POLLIN)) ctl_accept(a);
        if (xfer_idx >= 0 && (fds[xfer_idx].revents &
                              (POLLIN | POLLHUP | POLLERR))) xfer_poll(a);
        xfer_window_maybe_open(a);
        if (clip_idx >= 0) clip_read_drain(a);
        for (int i = 0; i < n_write_fds; i++) {
            short re = fds[write_idx[i]].revents;
            if (re & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))
                clip_write_poll(a, fds[write_idx[i]].fd);
        }

        /* The compositor ends the popup grab (and sends popup_done) when a
         * button is pressed outside any of our menu surfaces. */
        for (int i = 0; i < a->n_outs; i++) {
            Out *o = &a->outs[i];
            Menu *m = o->menu;
            if (!m) continue;
            int done = m->done || (m->sub && m->sub->done);
            if (done) {
                menu_close_root(o);
                a->ptr_menu = NULL;
            }
        }

        check_dir(a);

        /* Drive the post-drop lap off the presentation clock.  Every tick
         * marks the outputs so surf_draw() reuses the pending frame callback
         * and paints the segment at its current perimeter position. */
        if (a->border_anim_enabled && a->post_sel_anim_start > 0.0) {
            if (post_sel_elapsed_ms(a) > SEL_LAP_MS)
                a->post_sel_anim_start = -1; /* lap played for this batch */
            all_mark(a);
        }

        for (int i = 0; i < a->n_outs; i++) menu_tree_tick(a->outs[i].menu, now_sec());

        /* Coalesce to one paint per surface per frame. surf_draw() queues the
         * newest state and reuses any pending frame callback; the owner's
         * dirty bit is cleared by frame_done() only after the paint happens,
         * so motion arriving mid-frame is never lost or painted twice. */
        for (int i = 0; i < a->n_outs; i++) {
            Out *o = &a->outs[i];
            if (o->dirty && o->configured && o->width > 0)
                (void)surf_draw(a, &o->surf, o->width * o->scale,
                                o->height * o->scale, o->scale, paint_out, o,
                                &o->dirty);
            for (Menu *m = o->menu; m; m = m->sub) {
                if (m->configured && m->dirty)
                    (void)surf_draw(a, &m->surf, m->w * o->scale,
                                    m->h * o->scale, o->scale, paint_menu, m,
                                    &m->dirty);
            }
        }
        if (a->dialog && a->dlg_win.configured && a->dlg_win.dirty)
            (void)surf_draw(a, &a->dlg_win.surf,
                            a->dlg_win.w * a->dlg_win.scale,
                            a->dlg_win.h * a->dlg_win.scale,
                            a->dlg_win.scale, paint_dialog, a,
                            &a->dlg_win.dirty);
        if (a->xfer.active && a->xfer.win.configured && a->xfer.win.dirty)
            (void)surf_draw(a, &a->xfer.win.surf,
                            a->xfer.win.w * a->xfer.win.scale,
                            a->xfer.win.h * a->xfer.win.scale,
                            a->xfer.win.scale, paint_xfer, a,
                            &a->xfer.win.dirty);
    }

    /* Tear down the extra windows and any still-running child. */
    dialog_close();
    if (a->xfer.active) {
        if (a->xfer.pid > 0) kill(a->xfer.pid, SIGTERM);
        if (a->xfer.fd >= 0) { close(a->xfer.fd); a->xfer.fd = -1; }
        xfer_window_close(a);
    }
    state_save(a);
    for (int i = 0; i < a->n_outs; i++) {
        Out *o = &a->outs[i];
        if (o->menu) menu_destroy(o->menu);
        for (int k = 0; k < o->n; k++) icon_free(&o->icons[k]);
    }
    ctl_cleanup(a);
    free(a->desktop_dir);
    free(a->state_path);
    free(a->opt_desktop);
    if (a->xkb_state) xkb_state_unref(a->xkb_state);
    if (a->xkb_keymap) xkb_keymap_unref(a->xkb_keymap);
    if (a->xkb_ctx) xkb_context_unref(a->xkb_ctx);
    clip_write_cancel_all(a);
    if (a->dnd_source) wl_data_source_destroy(a->dnd_source);
    if (a->clip_source) wl_data_source_destroy(a->clip_source);
    data_offer_destroy_all(a);
    dnd_icon_destroy(a);
    if (a->data_device) wl_data_device_destroy(a->data_device);
    if (a->data_device_manager) wl_data_device_manager_destroy(a->data_device_manager);
    if (a->cursor_device) wp_cursor_shape_device_v1_destroy(a->cursor_device);
    if (a->cursor_manager) wp_cursor_shape_manager_v1_destroy(a->cursor_manager);
    if (a->keyboard) wl_keyboard_destroy(a->keyboard);
    if (a->pointer) wl_pointer_destroy(a->pointer);
    if (a->seat) wl_seat_destroy(a->seat);
    if (a->deco_manager) zxdg_decoration_manager_v1_destroy(a->deco_manager);
    if (a->registry) wl_registry_destroy(a->registry);
    if (a->display) wl_display_disconnect(a->display);
    free(g_state);
    free(a);
    g_app = NULL;
    return 0;
}
