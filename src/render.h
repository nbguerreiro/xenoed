#ifndef XENOED_RENDER_H
#define XENOED_RENDER_H

#include <cairo.h>
#include <pango/pango.h>
#include "editor.h"

typedef struct {
    PangoFontDescription *font_desc;
    PangoFontDescription *bar_font_desc;
    int row_height;   /* fixed vertical rhythm, in pixels (from font metrics) */
    int ascent;       /* baseline offset within a row, in pixels */
    int padding;      /* left text padding / gutter, in pixels */
} RenderState;

/* Initializes metrics (row_height/ascent) by measuring `font_desc` against
 * a throwaway Cairo surface. Font descriptor ownership is NOT taken; caller
 * must keep both descriptors alive and free them. */
void render_init(RenderState *rs, PangoFontDescription *font_desc,
                 PangoFontDescription *bar_font_desc);

/* Draws the full editor UI (text buffer, cursor, status/command line) onto
 * `surface`, which must already be sized to width x height pixels. Also
 * calls editor_ensure_visible() for scrolling based on the computed number
 * of visible rows. */
void render_frame(RenderState *rs, cairo_surface_t *surface, Editor *ed, int width, int height);

/* Number of text rows that fit in a window of the given pixel height,
 * reserving one row at the bottom for the status/command line. */
int render_visible_rows(const RenderState *rs, int height);

/* Hit-tests a pixel position (as from an X ButtonPress/MotionNotify event)
 * against the currently-visible text and reports which line/byte-offset it
 * corresponds to. `surface` supplies the font rendering context (pass the
 * same backing surface used for drawing, e.g. the backbuffer) but is not
 * modified. Returns 0 (and leaves *out_line and *out_col untouched) if the
 * point falls in the status/command bar rather than the text area. */
int render_xy_to_pos(RenderState *rs, cairo_surface_t *surface, Editor *ed,
                      int x, int y, int width, int height,
                      size_t *out_line, size_t *out_col);

#endif
