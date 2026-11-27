/*
 * jticons.h - the jtfiles built-in Cairo icon set, made standalone.
 *
 * This is a direct port of jtfiles' icon renderer (folder / drive / disc /
 * file-type glyphs and the "big" dark-plate layout) with the accent colours
 * passed in explicitly instead of read from jtfiles' globals.  No GTK, no
 * icon theme, no external icon assets: every glyph is drawn with Cairo and
 * Pango, and every accent-dependent colour is derived from the caller's
 * accent.  Used by jtwpi for its large desktop icons and menu icons.
 */
#ifndef JTICONS_H
#define JTICONS_H

#include <cairo.h>
#include <pango/pangocairo.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PI
#define PI 3.14159265358979323846
#endif

enum {
    JTI_ALIGN_LEFT, JTI_ALIGN_CENTER, JTI_ALIGN_RIGHT
};

/* Palette: the fixed UI colours plus the accent-derived ones. */
typedef struct {
    unsigned accent, accent_hi, accent_lo, on_accent;
    unsigned fg, dim, faint, border, card, card_top, green;
} JtiPalette;

static int jti_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int jti_parse_color(const char *s, unsigned *out)
{
    if (!s) return 0;
    if (*s == '#') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (strlen(s) != 6) return 0;
    unsigned v = 0;
    for (int i = 0; i < 6; i++) {
        int n = jti_hex_nibble(s[i]);
        if (n < 0) return 0;
        v = (v << 4) | (unsigned)n;
    }
    *out = v;
    return 1;
}

static double jti_linear_channel(unsigned c)
{
    double v = (double)c / 255.0;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double jti_luminance(unsigned rgb)
{
    return 0.2126 * jti_linear_channel((rgb >> 16) & 0xFF) +
           0.7152 * jti_linear_channel((rgb >> 8) & 0xFF) +
           0.0722 * jti_linear_channel(rgb & 0xFF);
}

static double jti_contrast(double a, double b)
{
    if (a < b) { double t = a; a = b; b = t; }
    return (a + 0.05) / (b + 0.05);
}

static unsigned jti_ink(unsigned rgb)
{
    const unsigned dark = 0x14100A, light = 0xF7F5F0;
    double bg = jti_luminance(rgb);
    return jti_contrast(bg, jti_luminance(light)) >
           jti_contrast(bg, jti_luminance(dark)) ? light : dark;
}

static unsigned jti_mix(unsigned rgb, unsigned toward, double amount)
{
    int r1 = (int)((rgb >> 16) & 0xFF), g1 = (int)((rgb >> 8) & 0xFF);
    int b1 = (int)(rgb & 0xFF);
    int r2 = (int)((toward >> 16) & 0xFF), g2 = (int)((toward >> 8) & 0xFF);
    int b2 = (int)(toward & 0xFF);
    int r = (int)lround(r1 + (r2 - r1) * amount);
    int g = (int)lround(g1 + (g2 - g1) * amount);
    int b = (int)lround(b1 + (b2 - b1) * amount);
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

static void jti_palette_set(JtiPalette *p, unsigned rgb)
{
    p->accent = rgb & 0xFFFFFF;
    p->accent_hi = jti_mix(p->accent, 0xFFFFFF, 0.16);
    p->accent_lo = jti_mix(p->accent, 0x000000, 0.14);
    p->on_accent = jti_ink(p->accent);
    p->fg     = 0xFFFFFF;
    p->dim    = 0x8C8C8C;
    p->faint  = 0x5C5C5C;
    p->border = 0x404040;
    p->card     = 0x0F1011;
    p->card_top = 0x131416;
    p->green    = 0x66B366;
}

#define JTI_R(h) ((((h) >> 16) & 0xFF) / 255.0)
#define JTI_G(h) ((((h) >> 8) & 0xFF) / 255.0)
#define JTI_B(h) (((h) & 0xFF) / 255.0)

static void jti_rgba(cairo_t *cr, unsigned hex, double alpha)
{
    cairo_set_source_rgba(cr, JTI_R(hex), JTI_G(hex), JTI_B(hex), alpha);
}

static void jti_rounded_rect(cairo_t *cr, double x, double y,
                             double w, double h, double r)
{
    if (w < 2 * r) r = w / 2.0;
    if (h < 2 * r) r = h / 2.0;
    if (r < 0) r = 0;
    cairo_new_path(cr);
    cairo_move_to(cr, x + r, y);
    cairo_arc(cr, x + w - r, y + r, r, -PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, PI / 2, PI);
    cairo_arc(cr, x + r, y + r, r, PI, 3 * PI / 2);
    cairo_close_path(cr);
}

/* Small Pango font cache shared by all icon glyphs. */
#define JTI_FONT_CACHE 16
typedef struct { double size; int weight; PangoFontDescription *fd; } JtiFontEnt;
static JtiFontEnt jti_fonts[JTI_FONT_CACHE];
static int jti_font_count;

static PangoFontDescription *jti_font(double size, int weight)
{
    for (int i = 0; i < jti_font_count; i++)
        if (jti_fonts[i].size == size && jti_fonts[i].weight == weight)
            return jti_fonts[i].fd;
    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, "Sans");
    pango_font_description_set_style(fd, PANGO_STYLE_NORMAL);
    pango_font_description_set_variant(fd, PANGO_VARIANT_NORMAL);
    pango_font_description_set_stretch(fd, PANGO_STRETCH_NORMAL);
    pango_font_description_set_absolute_size(fd, (int)size * PANGO_SCALE);
    pango_font_description_set_weight(fd, (PangoWeight)weight);
    if (jti_font_count < JTI_FONT_CACHE) {
        jti_fonts[jti_font_count].size = size;
        jti_fonts[jti_font_count].weight = weight;
        jti_fonts[jti_font_count].fd = fd;
        jti_font_count++;
    }
    return fd;
}

static PangoLayout *jti_layout(cairo_t *cr, const char *text,
                               double size, int weight, double spacing)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(l, jti_font(size, weight));
    pango_layout_set_text(l, text ? text : "", -1);
    if (spacing > 0.0) {
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs,
            pango_attr_letter_spacing_new((int)lround(spacing * PANGO_SCALE)));
        pango_layout_set_attributes(l, attrs);
        pango_attr_list_unref(attrs);
    }
    return l;
}

static double jti_text_width(cairo_t *cr, const char *text, double size,
                             int weight, double spacing)
{
    if (!text || !*text) return 0.0;
    PangoLayout *l = jti_layout(cr, text, size, weight, spacing);
    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);
    double w = logical.width;
    g_object_unref(l);
    return w;
}

static double jti_text_draw(cairo_t *cr, const char *text, double size,
                            int weight, unsigned hex, double alpha,
                            double x, double y, int align, double spacing)
{
    if (!text || !*text) return 0.0;
    PangoLayout *l = jti_layout(cr, text, size, weight, spacing);
    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);
    double w = logical.width;
    if (align == JTI_ALIGN_CENTER) x -= w / 2.0;
    else if (align == JTI_ALIGN_RIGHT) x -= w;
    cairo_move_to(cr, x, y);
    jti_rgba(cr, hex, alpha);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    return w;
}

/* ---------------------------------------------------------------- */
/*  Icon kinds                                                       */
/* ---------------------------------------------------------------- */

enum {
    JTI_FOLDER, JTI_FOLDER_LINK, JTI_HOME, JTI_DESKTOP, JTI_DOWNLOADS,
    JTI_FILE, JTI_IMAGE, JTI_VIDEO, JTI_AUDIO, JTI_ARCHIVE,
    JTI_PDF, JTI_CODE, JTI_EXEC, JTI_DISC, JTI_DRIVE,
    JTI_BACK, JTI_FORWARD, JTI_UP, JTI_REFRESH, JTI_SEARCH,
    JTI_LIST, JTI_GRID, JTI_APP
};

static int jti_icon_kind(const char *n)
{
    if (!n || !*n) return JTI_FILE;
    if (!strcmp(n, "user-home") || !strcmp(n, "user-home-symbolic") ||
        !strcmp(n, "drive-harddisk-root")) return JTI_HOME;
    if (!strcmp(n, "user-desktop")) return JTI_DESKTOP;
    if (!strcmp(n, "download") || !strcmp(n, "downloads") ||
        strstr(n, "download")) return JTI_DOWNLOADS;
    if (!strcmp(n, "folder-symlink") || strstr(n, "symlink"))
        return JTI_FOLDER_LINK;
    if (!strcmp(n, "folder") || !strncmp(n, "folder-", 7)) return JTI_FOLDER;
    if (strstr(n, "go-previous") || strstr(n, "back")) return JTI_BACK;
    if (strstr(n, "go-next") || strstr(n, "forward")) return JTI_FORWARD;
    if (strstr(n, "go-up")) return JTI_UP;
    if (strstr(n, "refresh") || strstr(n, "reload")) return JTI_REFRESH;
    if (strstr(n, "search")) return JTI_SEARCH;
    if (strstr(n, "view-list") || strstr(n, "list-view")) return JTI_LIST;
    if (strstr(n, "view-grid") || strstr(n, "grid-view") ||
        strstr(n, "view-icons")) return JTI_GRID;
    if (strstr(n, "drive-") || strstr(n, "harddisk")) return JTI_DRIVE;
    if (strstr(n, "cd-image") || strstr(n, "optical") || strstr(n, "disc"))
        return JTI_DISC;
    if (strstr(n, "image")) return JTI_IMAGE;
    if (strstr(n, "video")) return JTI_VIDEO;
    if (strstr(n, "audio") || strstr(n, "sound")) return JTI_AUDIO;
    if (strstr(n, "archive") || strstr(n, "package")) return JTI_ARCHIVE;
    if (strstr(n, "pdf")) return JTI_PDF;
    if (strstr(n, "executable") || strstr(n, "application-x-desktop"))
        return JTI_EXEC;
    if (strstr(n, "script") || strstr(n, "src") || strstr(n, "chdr") ||
        strstr(n, "html")) return JTI_CODE;
    /* .desktop Icon= names that do not map to a file type become app tiles. */
    if (strstr(n, "firefox") || strstr(n, "chrome") || strstr(n, "browser") ||
        strstr(n, "web") || strstr(n, "network")) return JTI_APP;
    if (strstr(n, "terminal") || strstr(n, "console") || strstr(n, "shell"))
        return JTI_APP;
    if (strstr(n, "text-editor") || strstr(n, "editor") || strstr(n, "code"))
        return JTI_APP;
    return JTI_FILE;
}

/* ---------------------------------------------------------------- */
/*  Shared helpers (jtfiles' glyph geometry)                        */
/* ---------------------------------------------------------------- */

#define JTI_BODY_TOP    12.0
#define JTI_BODY_BOTTOM 42.0
#define JTI_BODY_H      (JTI_BODY_BOTTOM - JTI_BODY_TOP)

static void jti_accent_fill(cairo_t *cr, double x, double y,
                            double w, double h, const JtiPalette *p)
{
    (void)w;
    cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgba(g, 0.0, JTI_R(p->accent_hi),
                                      JTI_G(p->accent_hi), JTI_B(p->accent_hi), 1);
    cairo_pattern_add_color_stop_rgba(g, 0.55, JTI_R(p->accent),
                                      JTI_G(p->accent), JTI_B(p->accent), 1);
    cairo_pattern_add_color_stop_rgba(g, 1.0, JTI_R(p->accent_lo),
                                      JTI_G(p->accent_lo), JTI_B(p->accent_lo), 1);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
}

static void jti_file_path(cairo_t *cr)
{
    cairo_new_path(cr);
    cairo_move_to(cr, 13.0, JTI_BODY_TOP);
    cairo_line_to(cr, 31.0, JTI_BODY_TOP);
    cairo_line_to(cr, 39.0, 20.0);
    cairo_line_to(cr, 39.0, JTI_BODY_BOTTOM);
    cairo_line_to(cr, 13.0, JTI_BODY_BOTTOM);
    cairo_close_path(cr);
}

static void jti_file_body(cairo_t *cr, const char *label, const JtiPalette *p)
{
    jti_file_path(cr);
    jti_rgba(cr, p->card, 1.0);
    cairo_fill_preserve(cr);
    jti_rgba(cr, p->border, 0.85);
    cairo_set_line_width(cr, 1.4);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_move_to(cr, 31.0, JTI_BODY_TOP);
    cairo_line_to(cr, 31.0, 20.0);
    cairo_line_to(cr, 39.0, 20.0);
    cairo_close_path(cr);
    jti_rgba(cr, p->accent_lo, 0.95);
    cairo_fill(cr);

    jti_rounded_rect(cr, 16.5, JTI_BODY_TOP, 3.0, JTI_BODY_H, 1.5);
    jti_rgba(cr, p->accent, 0.90);
    cairo_fill(cr);

    if (label && *label)
        jti_text_draw(cr, label, 9, 700, p->accent, 0.95,
                      27.0, JTI_BODY_BOTTOM - 9.0, JTI_ALIGN_CENTER, 0.6);
}

static void jti_file_label(const char *name, char *out, size_t cap)
{
    *out = '\0';
    if (strstr(name, "pdf")) snprintf(out, cap, "PDF");
    else if (strstr(name, "archive") || strstr(name, "package"))
        snprintf(out, cap, "ZIP");
    else if (strstr(name, "executable") || strstr(name, "desktop"))
        snprintf(out, cap, "RUN");
    else if (strstr(name, "script") || strstr(name, "code") ||
             strstr(name, "src") || strstr(name, "chdr"))
        snprintf(out, cap, "</>");
    else if (strstr(name, "html")) snprintf(out, cap, "HTM");
    else if (strstr(name, "text")) snprintf(out, cap, "TXT");
    else snprintf(out, cap, "FILE");
}

static void jti_folder_body(cairo_t *cr, const JtiPalette *p)
{
    jti_rounded_rect(cr, 5.0, 12.0, 18.0, 8.0, 2.5);
    jti_rgba(cr, p->accent_hi, 0.95);
    cairo_fill(cr);
    jti_rounded_rect(cr, 5.0, 17.0, 38.0, 25.0, 4.0);
    jti_accent_fill(cr, 5.0, 17.0, 38.0, 25.0, p);
    jti_rounded_rect(cr, 5.6, 17.6, 36.8, 23.8, 3.6);
    jti_rgba(cr, p->accent_lo, 0.9);
    cairo_set_line_width(cr, 1.1);
    cairo_stroke(cr);
}

/* Same folder with its tab lifted into an open lid.  open=0 matches
 * jti_folder_body() exactly, so the transition starts from the normal icon. */
static void jti_folder_body_open(cairo_t *cr, const JtiPalette *p, double open)
{
    if (open <= 0.0) {
        jti_folder_body(cr, p);
        return;
    }
    if (open > 1.0) open = 1.0;

    jti_rounded_rect(cr, 5.0, 17.0, 38.0, 25.0, 4.0);
    jti_accent_fill(cr, 5.0, 17.0, 38.0, 25.0, p);
    jti_rounded_rect(cr, 5.6, 17.6, 36.8, 23.8, 3.6);
    jti_rgba(cr, p->accent_lo, 0.9);
    cairo_set_line_width(cr, 1.1);
    cairo_stroke(cr);

    jti_rounded_rect(cr, 7.0, 18.4, 34.0, 4.4, 2.2);
    jti_rgba(cr, p->accent_lo, 0.78);
    cairo_fill(cr);

    cairo_save(cr);
    cairo_translate(cr, 5.0, 17.0);
    cairo_rotate(cr, -0.62 * open);
    jti_rounded_rect(cr, 0.0, -5.0, 18.0, 8.0, 2.5);
    jti_rgba(cr, p->accent_hi, 0.98);
    cairo_fill(cr);
    jti_rounded_rect(cr, 0.4, -4.6, 17.2, 7.2, 2.2);
    jti_rgba(cr, p->accent_lo, 0.80);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void jti_home_glyph(cairo_t *cr, const JtiPalette *p)
{
    jti_rgba(cr, p->on_accent, 0.88);
    cairo_set_line_width(cr, 2.4);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, 16.0, 31.0);
    cairo_line_to(cr, 24.0, 23.0);
    cairo_line_to(cr, 32.0, 31.0);
    cairo_stroke(cr);
    jti_rounded_rect(cr, 19.0, 30.0, 10.0, 9.0, 1.5);
    cairo_fill(cr);
}

static void jti_desktop_glyph(cairo_t *cr, const JtiPalette *p)
{
    jti_rgba(cr, p->on_accent, 0.88);
    jti_rounded_rect(cr, 17.0, 24.0, 16.0, 11.0, 2.0);
    cairo_fill(cr);
    jti_rgba(cr, p->accent_lo, 1.0);
    jti_rounded_rect(cr, 18.5, 25.5, 13.0, 8.0, 1.0);
    cairo_fill(cr);
    jti_rgba(cr, p->on_accent, 0.88);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, 22.0, 39.0);
    cairo_line_to(cr, 28.0, 39.0);
    cairo_move_to(cr, 25.0, 35.5);
    cairo_line_to(cr, 25.0, 39.0);
    cairo_stroke(cr);
}

static void jti_downloads_glyph(cairo_t *cr, const JtiPalette *p)
{
    jti_rgba(cr, p->on_accent, 0.90);
    cairo_set_line_width(cr, 2.4);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, 24.0, 25.5);
    cairo_line_to(cr, 24.0, 33.0);
    cairo_move_to(cr, 20.0, 29.5);
    cairo_line_to(cr, 24.0, 33.5);
    cairo_line_to(cr, 28.0, 29.5);
    cairo_stroke(cr);
    jti_rounded_rect(cr, 17.5, 36.0, 13.0, 3.0, 1.5);
    cairo_fill(cr);
}

static void jti_link_badge(cairo_t *cr, const JtiPalette *p)
{
    cairo_save(cr);
    cairo_translate(cr, 37.0, 35.0);
    cairo_rotate(cr, -0.55);
    jti_rounded_rect(cr, -8.0, -2.5, 16.0, 5.0, 2.5);
    jti_rgba(cr, p->card, 1.0);
    cairo_fill(cr);
    jti_rounded_rect(cr, -8.0, -2.5, 16.0, 5.0, 2.5);
    jti_rgba(cr, p->accent_hi, 0.95);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void jti_chevron(cairo_t *cr, int kind, const JtiPalette *p)
{
    jti_rgba(cr, p->accent, 0.95);
    cairo_set_line_width(cr, 3.2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (kind == JTI_BACK) {
        cairo_move_to(cr, 29.0, 14.0);
        cairo_line_to(cr, 19.0, 24.0);
        cairo_line_to(cr, 29.0, 34.0);
    } else if (kind == JTI_FORWARD) {
        cairo_move_to(cr, 19.0, 14.0);
        cairo_line_to(cr, 29.0, 24.0);
        cairo_line_to(cr, 19.0, 34.0);
    } else {
        cairo_move_to(cr, 14.0, 29.0);
        cairo_line_to(cr, 24.0, 19.0);
        cairo_line_to(cr, 34.0, 29.0);
    }
    cairo_stroke(cr);
}

static void jti_refresh(cairo_t *cr, const JtiPalette *p)
{
    jti_rgba(cr, p->accent, 0.95);
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, 24.0, 24.0, 11.0, 0.55, 5.35);
    cairo_stroke(cr);
    cairo_move_to(cr, 31.5, 11.5);
    cairo_line_to(cr, 36.5, 10.0);
    cairo_line_to(cr, 35.0, 15.0);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void jti_search(cairo_t *cr, const JtiPalette *p)
{
    jti_rgba(cr, p->accent, 0.95);
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, 21.0, 21.0, 10.0, 0.0, 2.0 * PI);
    cairo_stroke(cr);
    cairo_move_to(cr, 28.5, 28.5);
    cairo_line_to(cr, 38.0, 38.0);
    cairo_stroke(cr);
}

static void jti_view(cairo_t *cr, int grid, const JtiPalette *p)
{
    jti_rgba(cr, p->accent, 0.95);
    cairo_set_line_width(cr, 2.2);
    if (grid) {
        for (int row = 0; row < 2; row++)
            for (int col = 0; col < 2; col++) {
                jti_rounded_rect(cr, 11.0 + col * 14.0, 12.0 + row * 13.0,
                                 10.0, 9.0, 2.0);
                cairo_fill(cr);
            }
    } else {
        for (int i = 0; i < 3; i++) {
            double y = 14.0 + i * 9.0;
            jti_rounded_rect(cr, 11.0, y, 4.0, 4.0, 1.0);
            cairo_fill(cr);
            cairo_move_to(cr, 20.0, y + 2.0);
            cairo_line_to(cr, 37.0, y + 2.0);
            cairo_stroke(cr);
        }
    }
}

static void jti_drive(cairo_t *cr, const JtiPalette *p)
{
    cairo_save(cr);
    cairo_translate(cr, 0.0, JTI_BODY_TOP);
    cairo_scale(cr, 1.0, JTI_BODY_H / (37.0 - 12.5));
    cairo_translate(cr, 0.0, -12.5);

    jti_rounded_rect(cr, 7.0, 16.0, 34.0, 21.0, 5.0);
    jti_rgba(cr, p->card, 1.0);
    cairo_fill_preserve(cr);
    jti_rgba(cr, p->border, 0.9);
    cairo_set_line_width(cr, 1.3);
    cairo_stroke(cr);

    jti_rounded_rect(cr, 9.5, 12.5, 29.0, 9.0, 4.5);
    jti_accent_fill(cr, 9.5, 12.5, 29.0, 9.0, p);

    jti_rgba(cr, p->border, 0.7);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 12.0, 31.0);
    cairo_line_to(cr, 24.0, 31.0);
    cairo_stroke(cr);
    jti_rgba(cr, p->green, 0.95);
    cairo_arc(cr, 35.0, 32.0, 1.7, 0.0, 2.0 * PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void jti_disc(cairo_t *cr, const JtiPalette *p)
{
    cairo_arc(cr, 24.0, 27.0, 15.0, 0.0, 2.0 * PI);
    jti_accent_fill(cr, 9.0, 12.0, 30.0, 30.0, p);
    cairo_arc(cr, 24.0, 27.0, 5.0, 0.0, 2.0 * PI);
    jti_rgba(cr, p->card, 1.0);
    cairo_fill(cr);
    cairo_arc(cr, 24.0, 27.0, 15.0, 0.0, 2.0 * PI);
    jti_rgba(cr, p->accent_lo, 0.9);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
}

/* Generic application tile used for .desktop entries with no file-type icon. */
static void jti_app_tile(cairo_t *cr, const char *name, const JtiPalette *p,
                         int big)
{
    double top = big ? 12.0 : 10.0;
    double size = big ? 36.0 : 38.0;
    cairo_pattern_t *g = cairo_pattern_create_linear(
        6.0, top, 6.0, top + size);
    cairo_pattern_add_color_stop_rgba(g, 0, JTI_R(p->accent_hi),
                                      JTI_G(p->accent_hi), JTI_B(p->accent_hi), 0.98);
    cairo_pattern_add_color_stop_rgba(g, 1, JTI_R(p->accent),
                                      JTI_G(p->accent), JTI_B(p->accent), 0.98);
    jti_rounded_rect(cr, 6.0, top, size, size, 8.0);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    jti_rgba(cr, p->accent_lo, 0.95);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);

    char letter[2] = { 0, 0 };
    if (name && *name) letter[0] = (char)toupper((unsigned char)*name);
    else letter[0] = 'A';
    jti_text_draw(cr, letter, big ? 19 : 18, 700, p->on_accent, 0.95,
                  6.0 + size / 2.0, top + 6.0, JTI_ALIGN_CENTER, 0.0);
}

static void jti_set_crisp(cairo_t *cr, int target)
{
    if (target > 48) return;
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    cairo_font_options_t *fo = cairo_font_options_create();
    cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_NONE);
    cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_ON);
    cairo_set_font_options(cr, fo);
    cairo_font_options_destroy(fo);
}

/* ---------------------------------------------------------------- */
/*  Big ("large icon layout") glyphs                                 */
/* ---------------------------------------------------------------- */

static void jti_big_plate(cairo_t *cr, double x, double y,
                          double w, double h, const JtiPalette *p)
{
    cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgba(g, 0.0, JTI_R(p->card_top),
                                      JTI_G(p->card_top), JTI_B(p->card_top), 1);
    cairo_pattern_add_color_stop_rgba(g, 1.0, JTI_R(p->card),
                                      JTI_G(p->card), JTI_B(p->card), 1);
    jti_rounded_rect(cr, x, y, w, h, 4.0);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    jti_rgba(cr, p->accent, 0.80);
    cairo_set_line_width(cr, 1.6);
    cairo_stroke(cr);
}

static void jti_big_folder(cairo_t *cr, const JtiPalette *p)
{
    jti_rounded_rect(cr, 5.0, 11.0, 19.0, 8.5, 2.5);
    jti_rgba(cr, p->accent, 0.95);
    cairo_fill(cr);
    jti_big_plate(cr, 5.0, 16.0, 38.0, 26.0, p);
    cairo_move_to(cr, 9.0, 19.5);
    cairo_line_to(cr, 39.0, 19.5);
    jti_rgba(cr, p->accent_hi, 0.45);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
}

static void jti_big_folder_open(cairo_t *cr, const JtiPalette *p, double open)
{
    if (open <= 0.0) {
        jti_big_folder(cr, p);
        return;
    }
    if (open > 1.0) open = 1.0;

    jti_big_plate(cr, 5.0, 16.0, 38.0, 26.0, p);
    jti_rounded_rect(cr, 7.0, 18.2, 34.0, 4.8, 2.4);
    jti_rgba(cr, p->accent_lo, 0.70);
    cairo_fill(cr);

    cairo_save(cr);
    cairo_translate(cr, 5.0, 16.0);
    cairo_rotate(cr, -0.62 * open);
    jti_rounded_rect(cr, 0.0, -5.0, 19.0, 8.5, 2.5);
    jti_rgba(cr, p->accent, 0.95);
    cairo_fill(cr);
    jti_rounded_rect(cr, 0.4, -4.6, 18.2, 7.7, 2.2);
    jti_rgba(cr, p->accent_hi, 0.70);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_restore(cr);

    cairo_move_to(cr, 9.0, 21.5);
    cairo_line_to(cr, 39.0, 21.5);
    jti_rgba(cr, p->accent_hi, 0.40);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
}

static void jti_big_glyph(cairo_t *cr, int kind, const JtiPalette *p)
{
    jti_rgba(cr, p->accent_hi, 0.96);
    cairo_set_line_width(cr, 2.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (kind == JTI_HOME) {
        cairo_move_to(cr, 16.0, 31.0);
        cairo_line_to(cr, 24.0, 23.0);
        cairo_line_to(cr, 32.0, 31.0);
        cairo_stroke(cr);
        jti_rounded_rect(cr, 19.0, 30.0, 10.0, 9.0, 1.5);
        cairo_fill(cr);
    } else if (kind == JTI_DESKTOP) {
        jti_rounded_rect(cr, 16.5, 23.5, 15.0, 10.0, 2.0);
        cairo_stroke(cr);
        cairo_move_to(cr, 22.0, 38.5);
        cairo_line_to(cr, 27.0, 38.5);
        cairo_stroke(cr);
    } else if (kind == JTI_DOWNLOADS) {
        cairo_move_to(cr, 24.0, 22.5);
        cairo_line_to(cr, 24.0, 32.0);
        cairo_move_to(cr, 19.5, 27.5);
        cairo_line_to(cr, 24.0, 32.5);
        cairo_line_to(cr, 28.5, 27.5);
        cairo_stroke(cr);
        cairo_move_to(cr, 17.5, 36.5);
        cairo_line_to(cr, 30.5, 36.5);
        cairo_stroke(cr);
    } else if (kind == JTI_FOLDER_LINK) {
        jti_link_badge(cr, p);
    }
}

static void jti_big_file(cairo_t *cr, const char *label, const JtiPalette *p)
{
    jti_file_path(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(
        13.0, JTI_BODY_TOP, 13.0, JTI_BODY_BOTTOM);
    cairo_pattern_add_color_stop_rgba(g, 0.0, JTI_R(p->card_top),
                                      JTI_G(p->card_top), JTI_B(p->card_top), 1);
    cairo_pattern_add_color_stop_rgba(g, 1.0, JTI_R(p->card),
                                      JTI_G(p->card), JTI_B(p->card), 1);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    jti_rgba(cr, p->accent, 0.80);
    cairo_set_line_width(cr, 1.6);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_move_to(cr, 31.0, JTI_BODY_TOP);
    cairo_line_to(cr, 31.0, 20.0);
    cairo_line_to(cr, 39.0, 20.0);
    cairo_close_path(cr);
    jti_rgba(cr, p->accent, 0.92);
    cairo_fill(cr);

    jti_rounded_rect(cr, 16.5, JTI_BODY_TOP, 3.0, JTI_BODY_H, 1.5);
    jti_rgba(cr, p->accent_hi, 0.92);
    cairo_fill(cr);

    if (label && *label)
        jti_text_draw(cr, label, 10, 700, p->accent_hi, 1.0,
                      27.0, JTI_BODY_BOTTOM - 8.0, JTI_ALIGN_CENTER, 0.8);
}

static void jti_big_file_label(const char *name, char *out, size_t cap)
{
    *out = '\0';
    if (strstr(name, "pdf")) snprintf(out, cap, "PDF");
    else if (strstr(name, "image")) snprintf(out, cap, "IMG");
    else if (strstr(name, "video")) snprintf(out, cap, "VID");
    else if (strstr(name, "audio") || strstr(name, "sound"))
        snprintf(out, cap, "AUD");
    else if (strstr(name, "archive") || strstr(name, "package"))
        snprintf(out, cap, "ZIP");
    else if (strstr(name, "executable") || strstr(name, "desktop"))
        snprintf(out, cap, "RUN");
    else if (strstr(name, "script") || strstr(name, "code") ||
             strstr(name, "src") || strstr(name, "chdr"))
        snprintf(out, cap, "</>");
    else if (strstr(name, "html")) snprintf(out, cap, "HTM");
    else if (strstr(name, "text")) snprintf(out, cap, "TXT");
    else snprintf(out, cap, "FILE");
}

/* Draw one icon into the current cr. cr is a square `px` by `px` surface. */
static void jti_icon_render(cairo_t *cr, const char *name, int px,
                            const JtiPalette *p, int big, const char *display)
{
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, (double)px / 48.0, (double)px / 48.0);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    jti_set_crisp(cr, px);

    int kind = jti_icon_kind(name);
    char label[16];
    if (big) {
        switch (kind) {
        case JTI_FOLDER:
        case JTI_FOLDER_LINK:
        case JTI_HOME:
        case JTI_DESKTOP:
        case JTI_DOWNLOADS:
            jti_big_folder(cr, p);
            jti_big_glyph(cr, kind, p);
            break;
        case JTI_DRIVE: jti_drive(cr, p); break;
        case JTI_DISC:  jti_disc(cr, p); break;
        case JTI_BACK:
        case JTI_FORWARD:
        case JTI_UP:
            jti_chevron(cr, kind, p); break;
        case JTI_REFRESH: jti_refresh(cr, p); break;
        case JTI_SEARCH:  jti_search(cr, p); break;
        case JTI_LIST:    jti_view(cr, 0, p); break;
        case JTI_GRID:    jti_view(cr, 1, p); break;
        case JTI_APP:
            jti_app_tile(cr, display ? display : name, p, 1); break;
        default:
            jti_big_file_label(name, label, sizeof label);
            jti_big_file(cr, label, p);
            break;
        }
    } else {
        switch (kind) {
        case JTI_FOLDER:
        case JTI_FOLDER_LINK:
        case JTI_HOME:
        case JTI_DESKTOP:
        case JTI_DOWNLOADS:
            jti_folder_body(cr, p);
            if (kind == JTI_HOME) jti_home_glyph(cr, p);
            else if (kind == JTI_DESKTOP) jti_desktop_glyph(cr, p);
            else if (kind == JTI_DOWNLOADS) jti_downloads_glyph(cr, p);
            else if (kind == JTI_FOLDER_LINK) jti_link_badge(cr, p);
            break;
        case JTI_IMAGE:
            jti_file_body(cr, NULL, p);
            jti_rounded_rect(cr, 18.0, 23.0, 17.0, 12.0, 2.0);
            jti_rgba(cr, p->accent, 0.30);
            cairo_fill(cr);
            cairo_new_path(cr);
            cairo_move_to(cr, 19.0, 33.0);
            cairo_line_to(cr, 25.0, 27.0);
            cairo_line_to(cr, 30.0, 31.0);
            cairo_line_to(cr, 34.0, 28.0);
            cairo_line_to(cr, 35.0, 34.0);
            cairo_close_path(cr);
            jti_rgba(cr, p->accent, 0.95);
            cairo_fill(cr);
            cairo_arc(cr, 31.0, 25.0, 1.8, 0.0, 2.0 * PI);
            jti_rgba(cr, p->fg, 0.85);
            cairo_fill(cr);
            break;
        case JTI_VIDEO:
            jti_file_body(cr, NULL, p);
            jti_rounded_rect(cr, 17.0, 23.0, 19.0, 12.0, 3.0);
            jti_accent_fill(cr, 17.0, 23.0, 19.0, 12.0, p);
            cairo_new_path(cr);
            cairo_move_to(cr, 24.0, 26.0);
            cairo_line_to(cr, 31.0, 29.0);
            cairo_line_to(cr, 24.0, 32.0);
            cairo_close_path(cr);
            jti_rgba(cr, p->on_accent, 0.92);
            cairo_fill(cr);
            break;
        case JTI_AUDIO:
            jti_file_body(cr, NULL, p);
            jti_text_draw(cr, "♪", 18, 700, p->accent, 0.95,
                          26.0, 21.0, JTI_ALIGN_CENTER, 0.0);
            break;
        case JTI_DRIVE: jti_drive(cr, p); break;
        case JTI_DISC:  jti_disc(cr, p); break;
        case JTI_BACK:
        case JTI_FORWARD:
        case JTI_UP:
            jti_chevron(cr, kind, p); break;
        case JTI_REFRESH: jti_refresh(cr, p); break;
        case JTI_SEARCH:  jti_search(cr, p); break;
        case JTI_LIST:    jti_view(cr, 0, p); break;
        case JTI_GRID:    jti_view(cr, 1, p); break;
        case JTI_APP:
            jti_app_tile(cr, display ? display : name, p, 0); break;
        default:
            jti_file_label(name, label, sizeof label);
            jti_file_body(cr, label, p);
            break;
        }
    }
}

/* Folder variant used while a drag hovers over it.  Non-folder icons are
 * rendered normally; this only changes the folder body/lid geometry. */
static void jti_icon_render_open(cairo_t *cr, const char *name, int px,
                                 const JtiPalette *p, int big,
                                 const char *display, double open)
{
    int kind = jti_icon_kind(name);
    int folder = kind == JTI_FOLDER || kind == JTI_FOLDER_LINK ||
                 kind == JTI_HOME || kind == JTI_DESKTOP ||
                 kind == JTI_DOWNLOADS;
    if (!folder || open <= 0.0) {
        jti_icon_render(cr, name, px, p, big, display);
        return;
    }

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, (double)px / 48.0, (double)px / 48.0);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    jti_set_crisp(cr, px);

    if (big) {
        jti_big_folder_open(cr, p, open);
        jti_big_glyph(cr, kind, p);
    } else {
        jti_folder_body_open(cr, p, open);
        if (kind == JTI_HOME) jti_home_glyph(cr, p);
        else if (kind == JTI_DESKTOP) jti_desktop_glyph(cr, p);
        else if (kind == JTI_DOWNLOADS) jti_downloads_glyph(cr, p);
        else if (kind == JTI_FOLDER_LINK) jti_link_badge(cr, p);
    }
}

static cairo_surface_t *jti_icon_surface_open(const char *name, int px,
                                              const JtiPalette *p, int big,
                                              const char *display, double open)
{
    if (!name || !*name || px < 1) return NULL;
    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    if (cairo_surface_status(sf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(sf);
        return NULL;
    }
    cairo_t *cr = cairo_create(sf);
    jti_icon_render_open(cr, name, px, p, big, display, open);
    cairo_destroy(cr);
    cairo_surface_flush(sf);
    return sf;
}

static cairo_surface_t *jti_icon_surface(const char *name, int px,
                                         const JtiPalette *p, int big,
                                         const char *display)
{
    if (!name || !*name || px < 1) return NULL;
    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    if (cairo_surface_status(sf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(sf);
        return NULL;
    }
    cairo_t *cr = cairo_create(sf);
    jti_icon_render(cr, name, px, p, big, display);
    cairo_destroy(cr);
    cairo_surface_flush(sf);
    return sf;
}

/* Draw an icon at an arbitrary position/size in an already-scaled cr. */
static void jti_icon_draw(cairo_t *cr, const char *name, double size,
                          double x, double y, const JtiPalette *p, int big,
                          const char *display)
{
    if (!name || !*name || size < 1) return;
    cairo_surface_t *sf = jti_icon_surface(name, (int)lround(size), p, big,
                                           display);
    if (!sf) return;
    cairo_save(cr);
    cairo_set_source_surface(cr, sf, x, y);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(sf);
}

#endif /* JTICONS_H */
