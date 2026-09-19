#define _POSIX_C_SOURCE 200809L
#include <cairo.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>

#include "buffer.h"
#include "editor.h"
#include "render.h"

/* Headless tests for the horizontal scrolling (todo #35) and the mouse
 * hit-testing in src/render.c. render.c has no X11 dependency -- it draws
 * onto whatever cairo surface it's given, so an in-memory image surface is
 * enough to exercise render_frame(), render_hscroll_by() and
 * render_xy_to_pos() without a display. */

static int failures = 0;

#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { \
        failures++; \
        printf("FAIL: " fmt "\n", ##__VA_ARGS__); \
    } \
} while (0)

static int px_x(RenderState *rs, cairo_surface_t *surface,
                const char *text, size_t len, size_t byte) {
    cairo_t *cr = cairo_create(surface);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, rs->font_desc);
    pango_layout_set_text(layout, text, (int)len);
    PangoRectangle r;
    pango_layout_index_to_pos(layout, (int)byte, &r);
    int x = r.x / PANGO_SCALE;
    g_object_unref(layout);
    cairo_destroy(cr);
    return x;
}

int main(void) {
    /* A single long line: long enough that text_width (default window less
     * the two padding gutters) can never show it all, so scrolling has
     * somewhere to go. */
    enum { N = 400 };
    char line[N];
    memset(line, 'x', sizeof(line));

    Buffer *buf = buffer_new();
    buffer_set_from_text(buf, line, sizeof(line));

    Editor ed;
    editor_init(&ed, buf);

    int width = 400, height = 200;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                          width, height);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 12");
    RenderState rs;
    render_init(&rs, fd);

    int text_width = width - 2 * rs.padding;
    int row0_y = rs.top_margin;

    /* --- Test 1: no scroll by default ------------------------------- */
    render_frame(&rs, surface, &ed, width, height, 1);
    CHECK(ed.left_col == 0, "no scroll: left_col stays 0 (got %zu)", ed.left_col);

    /* Pixel-click at column 10 maps back to ~10 (offset tolerance for
     * Pango's trailing-edge conversion). */
    size_t l, c;
    int col10_px = rs.padding + px_x(&rs, surface, line, sizeof(line), 10);
    render_xy_to_pos(&rs, surface, &ed, col10_px, row0_y, width, height, &l, &c);
    CHECK(c >= 9 && c <= 11, "unscrolled click at col 10 -> %zu", c);

    /* --- Test 2: cursor-follow scroll ------------------------------- */
    ed.cur_col = 200;
    render_frame(&rs, surface, &ed, width, height, 1);
    CHECK(ed.left_col > 0, "cursor at col 200 scrolled the view (left_col=%zu)", ed.left_col);
    /* The cursor must actually be inside the text area. */
    int left_x = px_x(&rs, surface, line, sizeof(line), ed.left_col);
    int cur_x = px_x(&rs, surface, line, sizeof(line), 200);
    CHECK(cur_x - left_x <= text_width, "cursor visible (cur_x - left_x = %d <= %d)",
          cur_x - left_x, text_width);
    /* Left edge of the text area maps to a byte at/after left_col. */
    render_xy_to_pos(&rs, surface, &ed, rs.padding, row0_y, width, height, &l, &c);
    CHECK(c >= ed.left_col, "left edge maps at/after left_col (c=%zu, left_col=%zu)",
          c, ed.left_col);
    CHECK(c < N, "left-edge col within line (got %zu)", c);

    /* Round-trip: pixel position of a visible col maps back to ~that col. */
    size_t V = ed.left_col + 20;
    int vpx = rs.padding + px_x(&rs, surface, line, sizeof(line), V)
                              - left_x;
    render_xy_to_pos(&rs, surface, &ed, vpx, row0_y, width, height, &l, &c);
    CHECK(c >= V && c < V + 3, "round-trip click at visible col %zu -> %zu", V, c);

    /* --- Test 3: explicit shift+wheel scroll ------------------------ */
    ed.cur_col = 5;
    ed.left_col = 0;
    render_hscroll_by(&rs, surface, &ed, 10, width);
    CHECK(ed.left_col > 0, "hscroll right moved left_col (got %zu)", ed.left_col);
    CHECK(ed.cur_col >= ed.left_col, "cursor kept in view (cur=%zu, left=%zu)",
          ed.cur_col, ed.left_col);

    size_t before = ed.left_col;
    render_hscroll_by(&rs, surface, &ed, -10, width);
    CHECK(ed.left_col < before, "hscroll left moved back (before=%zu, after=%zu)",
          before, ed.left_col);
    CHECK(ed.left_col <= ed.cur_col, "cursor still in view after scrolling left");

    /* Scrolling can't push left_col past the end of the line. */
    ed.cur_col = 5;
    ed.left_col = 0;
    render_hscroll_by(&rs, surface, &ed, 100000, width);
    CHECK(ed.left_col >= N - 10, "hscroll clamps at line end (left_col=%zu)", ed.left_col);

    /* --- Test 4: cursor-follow scrolls back left -------------------- */
    ed.cur_col = 400;
    ed.left_col = 0;
    render_frame(&rs, surface, &ed, width, height, 1);
    CHECK(ed.left_col > 0, "scrolled right (left_col=%zu)", ed.left_col);
    ed.cur_col = 2;
    render_frame(&rs, surface, &ed, width, height, 1);
    CHECK(ed.left_col <= 2, "cursor off the left snapped the view back (left_col=%zu)",
          ed.left_col);

    /* --- Test 5: frame doesn't break with a scrolled viewport -------- */
    ed.cur_col = 300;
    render_frame(&rs, surface, &ed, width, height, 1);
    CHECK(ed.left_col > 0, "still scrolled after redraw (left_col=%zu)", ed.left_col);

    /* --- Test 6: multi-byte lines stay boundary-aligned --------------- */
    {
        /* 200 repetitions of 2-byte U+00E9 ('é'): every byte offset 0..399,
         * boundaries on the evens. left_col must never sit on a
         * continuation byte. */
        enum { MLEN = 400 };
        char mb[MLEN];
        for (int i = 0; i < MLEN / 2; i++) { mb[2 * i] = (char)0xC3; mb[2 * i + 1] = (char)0xA9; }
        Buffer *b6 = buffer_new();
        buffer_set_from_text(b6, mb, sizeof(mb));
        Editor e6;
        editor_init(&e6, b6);
        render_hscroll_by(&rs, surface, &e6, 100, width);
        CHECK(e6.left_col % 2 == 0, "hscroll left_col stays aligned (got %zu)", e6.left_col);
        e6.cur_col = 300;
        render_frame(&rs, surface, &e6, width, height, 1);
        CHECK(e6.left_col % 2 == 0, "follow-scroll left_col stays aligned (got %zu)", e6.left_col);
        CHECK(e6.left_col < 300, "follow-scroll didn't overshoot (got %zu)", e6.left_col);
        /* hit-test on a scrolled multi-byte line maps back to a boundary */
        size_t l6, c6;
        int mb_left = px_x(&rs, surface, mb, sizeof(mb), e6.left_col);
        int mb_px = rs.padding + px_x(&rs, surface, mb, sizeof(mb), 250) - mb_left;
        render_xy_to_pos(&rs, surface, &e6, mb_px, row0_y, width, height, &l6, &c6);
        CHECK(c6 % 2 == 0, "hit-test col aligned (got %zu)", c6);
        editor_deinit(&e6);
        buffer_free(b6);
    }

    pango_font_description_free(fd);
    cairo_surface_destroy(surface);
    editor_deinit(&ed);
    buffer_free(buf);

    if (failures == 0) {
        printf("render/hscroll tests: all passed\n");
        return 0;
    }
    printf("%d render/hscroll test(s) FAILED\n", failures);
    return 1;
}