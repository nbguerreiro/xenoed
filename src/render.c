#include "render.h"
#include "utf8.h"
#include "config.h"
#include <pango/pangocairo.h>
#include <string.h>
#include <stdio.h>

/* Light "sepia paper" theme. Values are 0..1 for cairo_set_source_rgb(a). */

/* background #dfbfbf */
#define BG_R 0.874
#define BG_G 0.749
#define BG_B 0.749

/* black */
#define FG_R 0.0
#define FG_G 0.0
#define FG_B 0.0

/*cursor*/
/* warm amber accent, #8a5a24 */
#define CURSOR_R 0.541
#define CURSOR_G 0.353
#define CURSOR_B 0.141

/*status bar*/
/* #e7d9b8, slightly darker sepia */
#define STATUSBG_R 0.906
#define STATUSBG_G 0.851
#define STATUSBG_B 0.722

/*status bar text*/
/* #494133-ish dark brown */
#define STATUSFG_R 0.286
#define STATUSFG_G 0.255
#define STATUSFG_B 0.200

/* soft goldenrod highlight, sits well on sepia */
#define SELECTION_R 0.71
#define SELECTION_G 0.58
#define SELECTION_B 0.20
#define SELECTION_A 0.35

void render_init(RenderState *rs, PangoFontDescription *font_desc) {
    rs->font_desc = font_desc;
    rs->padding = 6;

    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(tmp);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, font_desc);

    PangoContext *ctx = pango_layout_get_context(layout);
    PangoFontMetrics *metrics = pango_context_get_metrics(ctx, font_desc, NULL);

    int ascent = pango_font_metrics_get_ascent(metrics) / PANGO_SCALE;
    int descent = pango_font_metrics_get_descent(metrics) / PANGO_SCALE;

    pango_font_metrics_unref(metrics);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(tmp);

    rs->ascent = ascent;
    rs->row_height = (int)((ascent + descent) * XENOED_LINE_SPACING);
    if (rs->row_height < ascent + descent) rs->row_height = ascent + descent;
}

int render_visible_rows(const RenderState *rs, int height) {
    int usable = height - rs->row_height;
    if (usable < 0) usable = 0;
    int rows = usable / rs->row_height;
    return rows < 1 ? 1 : rows;
}

static void draw_cursor(cairo_t *cr, RenderState *rs, PangoLayout *layout,
                         size_t byte_index, int row_y, int fallback_w, int block_mode) {
    PangoRectangle rect;
    pango_layout_index_to_pos(layout, (int)byte_index, &rect);

    int cx = rs->padding + rect.x / PANGO_SCALE;
    int cw = rect.width / PANGO_SCALE;
    if (cw <= 0) cw = fallback_w;

    if (block_mode) {
        cairo_set_source_rgba(cr, CURSOR_R, CURSOR_G, CURSOR_B, 0.45);
        cairo_rectangle(cr, cx, row_y, cw, rs->row_height);
        cairo_fill(cr);
    } else {
        cairo_set_source_rgb(cr, CURSOR_R, CURSOR_G, CURSOR_B);
        cairo_rectangle(cr, cx, row_y, 2, rs->row_height);
        cairo_fill(cr);
    }
}

static int layout_x_at(const RenderState *rs, PangoLayout *layout, size_t byte_index) {
    PangoRectangle rect;
    pango_layout_index_to_pos(layout, (int)byte_index, &rect);
    return rs->padding + rect.x / PANGO_SCALE;
}

static void draw_selection_row(cairo_t *cr, RenderState *rs, PangoLayout *layout,
                                int row_y, int width, size_t from_col, size_t to_col,
                                int is_first, int is_last) {
    int x_from = is_first ? layout_x_at(rs, layout, from_col) : rs->padding;
    int x_to   = is_last  ? layout_x_at(rs, layout, to_col)   : width;
    if (x_to <= x_from) x_to = x_from + 2;

    cairo_set_source_rgba(cr, SELECTION_R, SELECTION_G, SELECTION_B, SELECTION_A);
    cairo_rectangle(cr, x_from, row_y, x_to - x_from, rs->row_height);
    cairo_fill(cr);
}

void render_frame(RenderState *rs, cairo_surface_t *surface, Editor *ed, int width, int height) {
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, BG_R, BG_G, BG_B);
    cairo_paint(cr);

    int visible_rows = render_visible_rows(rs, height);
    editor_ensure_visible(ed, (size_t)visible_rows);

    Buffer *buf = ed->buf;
    int fallback_cursor_w = rs->row_height / 2;
    if (fallback_cursor_w < 4) fallback_cursor_w = 4;

    int has_sel = editor_has_selection(ed);
    size_t sel_fl = 0, sel_fc = 0, sel_tl = 0, sel_tc = 0;
    if (has_sel) editor_selection_range(ed, &sel_fl, &sel_fc, &sel_tl, &sel_tc);

    for (int row = 0; row < visible_rows; row++) {
        size_t line_idx = ed->top_line + (size_t)row;
        if (line_idx >= buf->count) break;

        Line *l = buffer_line(buf, line_idx);
        int row_y = row * rs->row_height;

        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, rs->font_desc);
        pango_layout_set_text(layout, l->data, (int)l->len);

        if (has_sel && line_idx >= sel_fl && line_idx <= sel_tl) {
            draw_selection_row(cr, rs, layout, row_y, width, sel_fc, sel_tc,
                                line_idx == sel_fl, line_idx == sel_tl);
        }

        if (line_idx == ed->cur_line && ed->mode != MODE_INSERT) {
            draw_cursor(cr, rs, layout, ed->cur_col, row_y, fallback_cursor_w, 1);
        }

        cairo_set_source_rgb(cr, FG_R, FG_G, FG_B);
        cairo_move_to(cr, rs->padding, row_y);
        pango_cairo_show_layout(cr, layout);

        if (line_idx == ed->cur_line && ed->mode == MODE_INSERT) {
            draw_cursor(cr, rs, layout, ed->cur_col, row_y, fallback_cursor_w, 0);
        }

        g_object_unref(layout);
    }

    int status_y = visible_rows * rs->row_height;
    cairo_set_source_rgb(cr, STATUSBG_R, STATUSBG_G, STATUSBG_B);
    cairo_rectangle(cr, 0, status_y, width, rs->row_height);
    cairo_fill(cr);

    char line_text[512];
    if (ed->mode == MODE_SEARCH) {
        snprintf(line_text, sizeof(line_text), "/%s", ed->cmdline);
    } else {
        const char *mode_name = ed->mode == MODE_INSERT ? "INSERT"
                               : ed->mode == MODE_VISUAL ? "VISUAL"
                               : "NORMAL";
        const char *fname = buf->filename ? buf->filename : "[No Name]";
        const char *dirty = buf->dirty ? " [+]" : "";
        if (ed->status[0]) {
            snprintf(line_text, sizeof(line_text), "-- %s -- %s%s  %s",
                      mode_name, fname, dirty, ed->status);
        } else {
            snprintf(line_text, sizeof(line_text), "-- %s -- %s%s  Ln %zu, Col %zu",
                      mode_name, fname, dirty, ed->cur_line + 1,
                      1 + (size_t)g_utf8_strlen(buffer_line(buf, ed->cur_line)->data,
                                                 (glong)ed->cur_col));
        }
    }

    PangoLayout *status_layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(status_layout, rs->font_desc);
    pango_layout_set_text(status_layout, line_text, -1);
    cairo_set_source_rgb(cr, STATUSFG_R, STATUSFG_G, STATUSFG_B);
    cairo_move_to(cr, rs->padding, status_y);
    pango_cairo_show_layout(cr, status_layout);

    if (ed->mode == MODE_SEARCH) {
        draw_cursor(cr, rs, status_layout, ed->cmdlen + 1, status_y, fallback_cursor_w, 0);
    }

    g_object_unref(status_layout);
    cairo_destroy(cr);
}

int render_xy_to_pos(RenderState *rs, cairo_surface_t *surface, Editor *ed,
                      int x, int y, int width, int height,
                      size_t *out_line, size_t *out_col) {
    (void)width;
    int visible_rows = render_visible_rows(rs, height);
    int status_y = visible_rows * rs->row_height;
    if (y >= status_y) return 0;

    int row = y / rs->row_height;
    if (row < 0) row = 0;
    if (row >= visible_rows) row = visible_rows - 1;

    Buffer *buf = ed->buf;
    size_t line_idx = ed->top_line + (size_t)row;
    if (line_idx >= buf->count) {
        line_idx = buf->count - 1;
        *out_line = line_idx;
        *out_col = buffer_line(buf, line_idx)->len;
        return 1;
    }

    Line *l = buffer_line(buf, line_idx);
    cairo_t *cr = cairo_create(surface);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, rs->font_desc);
    pango_layout_set_text(layout, l->data, (int)l->len);

    int x_rel = x - rs->padding;
    if (x_rel < 0) x_rel = 0;

    int index = 0, trailing = 0;
    pango_layout_xy_to_index(layout, x_rel * PANGO_SCALE, 0, &index, &trailing);
    size_t col = (size_t)index;
    if (trailing > 0) col = utf8_next_boundary(l->data, l->len, col);
    if (col > l->len) col = l->len;

    g_object_unref(layout);
    cairo_destroy(cr);

    *out_line = line_idx;
    *out_col = col;
    return 1;
}
