#include "render.h"
#include "utf8.h"
#include "config.h"
#include <pango/pangocairo.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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
    rs->top_margin = XENOED_TOP_MARGIN;

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
    int usable = height - rs->row_height - rs->top_margin;
    if (usable < 0) usable = 0;
    int rows = usable / rs->row_height;
    return rows < 1 ? 1 : rows;
}

/* x pixel position (layout coordinates, before any horizontal scroll
 * shift) of `byte_index` within a Pango layout. No rs->padding gutter
 * offset -- that's added by the callers, so this stays usable for the
 * scroll math directly. */
static int layout_byte_x(PangoLayout *layout, size_t byte_index) {
    PangoRectangle rect;
    pango_layout_index_to_pos(layout, (int)byte_index, &rect);
    return rect.x / PANGO_SCALE;
}

/* ed->left_col clamped to the given line: never past its end, and always
 * on a UTF-8 boundary -- a scroll offset landing mid-character would slice
 * a multi-byte glyph at its continuation byte and confuse the align math. */
static size_t line_left_col(const Line *l, size_t left_col) {
    if (left_col > l->len) left_col = l->len;
    while (left_col > 0 && left_col < l->len &&
           utf8_is_cont((unsigned char)l->data[left_col]))
        left_col--;
    return left_col;
}

/* The longest line in the buffer -- the horizontal ruler that owns
 * ed->left_col's scroll space. Scrolling advances left_col along this
 * line, NOT along the cursor's, so shift+wheel keeps working while the
 * cursor rests on a short or empty line (the headless tests had only ever
 * exercised cursor-on-long-line). Shorter rows get clamped by their own
 * line_left_col() at draw time: a row whose length is past left_col is
 * simply drawn scrolled out past the left edge. */
static size_t buffer_max_line_len(Buffer *b) {
    size_t maxlen = 0;
    for (size_t i = 0; i < b->count; i++) {
        size_t len = buffer_line(b, i)->len;
        if (len > maxlen) maxlen = len;
    }
    return maxlen;
}

/* Horizontal scrolling (todo #35). Called once per frame from render_frame
 * with the frame's Cairo context -- the only place x positions can be
 * measured -- to nudge ed->left_col the minimum distance needed to keep
 * the cursor inside the horizontal viewport, mirroring what
 * editor_ensure_visible() does for the vertical axis. It never "recenters"
 * on its own; it only reacts to a cursor that navigation moved out of view.
 * `text_width` is the usable pixel width of the text area. */
static void editor_ensure_hscroll(cairo_t *cr, RenderState *rs, Editor *ed, int text_width) {
    if (text_width <= 0) return;
    const Line *l = buffer_line(ed->buf, ed->cur_line);

    /* The cursor's own line is shorter than the scroll offset: it is
     * scrolled entirely out of view. Leave the user's scroll alone instead
     * of snapping back to the cursor -- the long rows being read keep
     * their offsets via per-row line_left_col() at draw time. (Without
     * this, a cursor sitting on a short line instantly undid every
     * shift+wheel move.) Navigation back into the line re-snaps on its
     * next redraw. */
    if (ed->left_col >= l->len) return;

    size_t lc = line_left_col(l, ed->left_col);

    if (ed->cur_col < lc) {
        /* Cursor went left of the first visible byte: snap the view back so
         * the cursor sits at the left edge of the text area. */
        lc = ed->cur_col;
    } else {
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, rs->font_desc);
        pango_layout_set_text(layout, l->data, (int)l->len);

        int cx = layout_byte_x(layout, ed->cur_col);
        int lx = layout_byte_x(layout, lc);
        if (cx - lx > text_width) {
            /* Cursor past the right edge: scroll right just far enough to
             * bring it back -- the smallest char boundary whose x position
             * still clears the left edge. Binary search: x is monotone
             * (advances are never negative); the result is retro-aligned
             * to a UTF-8 boundary by line_left_col(). */
            int target = cx - text_width;
            size_t lo = 0, hi = ed->cur_col;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (layout_byte_x(layout, mid) >= target) hi = mid;
                else lo = mid + 1;
            }
            lc = line_left_col(l, lo);
        }
        g_object_unref(layout);
    }
    ed->left_col = lc;
}

void render_hscroll_by(RenderState *rs, cairo_surface_t *surface, Editor *ed,
                       int delta_cols, int width) {
    if (delta_cols == 0 || ed->buf->count == 0) return;
    Buffer *b = ed->buf;
    const Line *l = buffer_line(b, ed->cur_line); /* cursor line, for below */

    /* Step left_col along the buffer's longest line (the horizontal ruler),
     * not the cursor's line: byte boundaries exist there to walk even when
     * the cursor sits on a short/empty row, which is the very situation
     * shift+wheel is used in (reading a long line above a short tail). */
    size_t maxlen = buffer_max_line_len(b);
    if (maxlen == 0) return;

    /* The line that owns maxlen -- byte boundaries walked below belong to
     * it (they may not be boundaries on the cursor's shorter line). */
    Line *ruler = NULL;
    for (size_t i = 0; i < b->count; i++) {
        Line *l = buffer_line(b, i);
        if (!ruler || l->len > ruler->len) ruler = l;
    }

    size_t lc = ed->left_col;
    if (lc >= maxlen) lc = maxlen;
    while (lc > 0 && lc < maxlen && utf8_is_cont((unsigned char)ruler->data[lc])) lc--;

    /* Move the left edge left (negative) or right by `delta_cols` chars. */
    if (delta_cols < 0) {
        size_t n = (size_t)(-delta_cols);
        while (n > 0 && lc > 0) { lc = utf8_prev_boundary(ruler->data, lc); n--; }
    } else {
        size_t n = (size_t)delta_cols;
        while (n > 0 && lc < maxlen) { lc = utf8_next_boundary(ruler->data, maxlen, lc); n--; }
    }
    ed->left_col = lc;

    /* Keep the cursor inside the horizontal viewport, mirroring how
     * editor_scroll_by keeps it inside the vertical one: a cursor the
     * scroll left stranded off-screen gets pulled back to the viewport
     * edge, never left invisible. */
    int text_width = width - 2 * rs->padding;
    if (text_width < 1) text_width = 1;

    if (ed->cur_col < lc) {
        ed->cur_col = lc;
    } else {
        cairo_t *cr = cairo_create(surface);
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, rs->font_desc);
        pango_layout_set_text(layout, l->data, (int)l->len);

        int cx = layout_byte_x(layout, ed->cur_col);
        int lx = layout_byte_x(layout, lc);
        if (cx - lx > text_width) {
            /* Scroll moved the cursor off the right edge: pull it back to
             * the last char that still fits (largest boundary with
             * x <= limit; binary search). */
            int limit = lx + text_width;
            size_t lo = 0, hi = ed->cur_col;
            while (lo < hi) {
                size_t mid = lo + (hi - lo + 1) / 2;
                if (layout_byte_x(layout, mid) <= limit) lo = mid;
                else hi = mid - 1;
            }
            size_t col = line_left_col(l, lo);
            size_t nb = utf8_next_boundary(l->data, l->len, col);
            if (nb <= ed->cur_col && layout_byte_x(layout, nb) <= limit) col = nb;
            ed->cur_col = col;
        }
        g_object_unref(layout);
        cairo_destroy(cr);
    }
    editor_clamp_cursor(ed);
}

static void draw_cursor(cairo_t *cr, RenderState *rs, PangoLayout *layout,
                         size_t byte_index, int row_y, int fallback_w,
                         int block_mode, int focused, int shift) {
    PangoRectangle rect;
    pango_layout_index_to_pos(layout, (int)byte_index, &rect);

    int cx = shift + rect.x / PANGO_SCALE;
    int cw = rect.width / PANGO_SCALE;
    if (cw <= 0) cw = fallback_w;

    if (block_mode) {
        if (focused) {
            cairo_set_source_rgba(cr, CURSOR_R, CURSOR_G, CURSOR_B, 0.45);
            cairo_rectangle(cr, cx, row_y, cw, rs->row_height);
            cairo_fill(cr);
        } else {
            cairo_set_source_rgba(cr, CURSOR_R, CURSOR_G, CURSOR_B, 0.6);
            cairo_set_line_width(cr, 2);
            cairo_rectangle(cr, cx + 1, row_y + 1, cw - 2, rs->row_height - 2);
            cairo_stroke(cr);
        }
    } else {
        cairo_set_source_rgb(cr, CURSOR_R, CURSOR_G, CURSOR_B);
        cairo_rectangle(cr, cx, row_y, 2, rs->row_height);
        cairo_fill(cr);
    }
}

static void draw_selection_row(cairo_t *cr, RenderState *rs, PangoLayout *layout,
                                int row_y, int width, size_t from_col, size_t to_col,
                                int is_first, int is_last, int shift) {
    int x_from = is_first ? shift + layout_byte_x(layout, from_col) : rs->padding;
    int x_to   = is_last  ? shift + layout_byte_x(layout, to_col)   : width;
    if (x_to <= x_from) x_to = x_from + 2;

    cairo_set_source_rgba(cr, SELECTION_R, SELECTION_G, SELECTION_B, SELECTION_A);
    cairo_rectangle(cr, x_from, row_y, x_to - x_from, rs->row_height);
    cairo_fill(cr);
}

void render_frame(RenderState *rs, cairo_surface_t *surface, Editor *ed, int width, int height, int focused) {
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, BG_R, BG_G, BG_B);
    cairo_paint(cr);

    int visible_rows = render_visible_rows(rs, height);
    editor_ensure_visible(ed, (size_t)visible_rows);

    /* Horizontal scroll (todo #35): the text area is the window minus both
     * padding gutters. editor_ensure_hscroll() keeps the cursor inside it;
     * each row's text is then drawn shifted left by its left_col's x so the
     * scrolled-out part hangs off-screen, and clipped to the text area so
     * it stays out of the gutters (and the status bar, reset before that). */
    int text_left = rs->padding;
    int text_width = width - 2 * rs->padding;
    if (text_width < 1) text_width = 1;
    editor_ensure_hscroll(cr, rs, ed, text_width);

    cairo_rectangle(cr, text_left, rs->top_margin, text_width,
                    visible_rows * rs->row_height);
    cairo_clip(cr);

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
        int row_y = rs->top_margin + row * rs->row_height;

        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, rs->font_desc);
        pango_layout_set_text(layout, l->data, (int)l->len);

        /* Horizontal scroll offset for this row: left_col as a pixel shift,
         * clamped to the row's own line length (shorter lines are simply
         * scrolled out past the left edge). */
        int shift = rs->padding - layout_byte_x(layout, line_left_col(l, ed->left_col));

        if (has_sel && line_idx >= sel_fl && line_idx <= sel_tl) {
            draw_selection_row(cr, rs, layout, row_y, width, sel_fc, sel_tc,
                                line_idx == sel_fl, line_idx == sel_tl, shift);
        }

        if (line_idx == ed->cur_line && ed->mode != MODE_INSERT) {
            draw_cursor(cr, rs, layout, ed->cur_col, row_y, fallback_cursor_w, 1, focused, shift);
        }

        cairo_set_source_rgb(cr, FG_R, FG_G, FG_B);
        cairo_move_to(cr, shift, row_y);
        pango_cairo_show_layout(cr, layout);

        if (line_idx == ed->cur_line && ed->mode == MODE_INSERT) {
            draw_cursor(cr, rs, layout, ed->cur_col, row_y, fallback_cursor_w, 0, focused, shift);
        }

        g_object_unref(layout);
    }

    cairo_reset_clip(cr);
    int status_y = rs->top_margin + visible_rows * rs->row_height;

    PangoFontDescription *bar_font_desc = pango_font_description_from_string(XENOED_FONT_BAR);

    /* Keep each status-bar section at a fixed screen position. */
    const int col_width = 56;
    const int line_width = 100;
    const int section_gap = 12;
    const int col_x = width - rs->padding - col_width;
    const int line_x = col_x - section_gap - line_width;
    const int name_x = rs->padding;
    const int name_width = line_x - section_gap - name_x;

    /* todo #32: an absolute filename under the user's home directory reads
     * as ~/... in the status bar instead of /home/user/..., the way shells
     * spell it. Returns buf when shortened, otherwise the original path.
     * No shortening when HOME is unset, is "/" (walking every path down
     * to a bare prefix is useless), or when the path isn't actually inside
     * the home tree (plain names like "notes.txt" and foreign trees are
     * left alone). */
    char short_name[512];
    const char *fname;
    if (buf->filename) {
        const char *home = getenv("HOME");
        if (home && *home && home[1] != '\0') {
            size_t hl = strlen(home);
            if (strncmp(buf->filename, home, hl) == 0 &&
                (buf->filename[hl] == '\0' || buf->filename[hl] == '/')) {
                snprintf(short_name, sizeof(short_name), "~%s", buf->filename + hl);
                fname = short_name;
            } else {
                fname = buf->filename;
            }
        } else {
            fname = buf->filename;
        }
    } else {
        fname = "[No Name]";
    }
    char line_text[64];
    char col_text[32];
    snprintf(line_text, sizeof(line_text), "%zu/%zu", ed->cur_line + 1, buf->count);
    Line *current_line = buffer_line(buf, ed->cur_line);
    size_t col = 1 + (size_t)g_utf8_strlen(current_line->data,
                                            (glong)ed->cur_col);
    snprintf(col_text, sizeof(col_text), "%zu", col);

    /* Modified-in-editor shows vim-style as [+]; a file edited (or created/
     * deleted) behind our back since the last load/save shows as [!].
     * Sized for worst case (512-byte shortened name + both suffixes). */
    char file_label[1024];
    snprintf(file_label, sizeof(file_label), "%s%s%s",
             fname,
             buf->dirty ? " [+]" : "",
             buffer_disk_changed(buf) ? " [!]" : "");

    PangoLayout *name_layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(name_layout, bar_font_desc);
    pango_layout_set_width(name_layout, name_width * PANGO_SCALE);
    pango_layout_set_ellipsize(name_layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_text(name_layout, file_label, -1);

    PangoLayout *line_layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(line_layout, bar_font_desc);
    pango_layout_set_width(line_layout, line_width * PANGO_SCALE);
    pango_layout_set_alignment(line_layout, PANGO_ALIGN_RIGHT);
    pango_layout_set_text(line_layout, line_text, -1);

    PangoLayout *col_layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(col_layout, bar_font_desc);
    pango_layout_set_width(col_layout, col_width * PANGO_SCALE);
    pango_layout_set_alignment(col_layout, PANGO_ALIGN_RIGHT);
    pango_layout_set_text(col_layout, col_text, -1);

    cairo_set_source_rgb(cr, STATUSFG_R, STATUSFG_G, STATUSFG_B);
    cairo_move_to(cr, name_x, status_y);
    pango_cairo_show_layout(cr, name_layout);
    cairo_move_to(cr, line_x, status_y);
    pango_cairo_show_layout(cr, line_layout);
    cairo_move_to(cr, col_x, status_y);
    pango_cairo_show_layout(cr, col_layout);

    g_object_unref(name_layout);
    g_object_unref(line_layout);
    g_object_unref(col_layout);
    pango_font_description_free(bar_font_desc);
    cairo_destroy(cr);
}

int render_xy_to_pos(RenderState *rs, cairo_surface_t *surface, Editor *ed,
                      int x, int y, int width, int height,
                      size_t *out_line, size_t *out_col) {
    (void)width;
    int visible_rows = render_visible_rows(rs, height);
    int status_y = rs->top_margin + visible_rows * rs->row_height;
    if (y >= status_y) return 0;

    int row = (y - rs->top_margin) / rs->row_height;
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

    /* Horizontal scroll (todo #35): add back the pixel width of everything
     * scrolled off the left edge so the pointer maps to the same byte index
     * it would if the line were un-scrolled. */
    int left_x = layout_byte_x(layout, line_left_col(l, ed->left_col));
    int x_rel = x - rs->padding + left_x;
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
