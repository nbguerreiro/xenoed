#ifndef XENOED_EDITOR_H
#define XENOED_EDITOR_H

#include "buffer.h"
#include <stddef.h>

typedef enum {
    MODE_NORMAL,
    MODE_INSERT,
    MODE_VISUAL,
    MODE_SEARCH
} EditorMode;

/* Special keys the renderer/input layer must recognize regardless of mode.
 * Everything else is delivered as plain UTF-8 text via `text`/`text_len`.
 * Kept free of any X11 dependency so this file/editor.c stay toolkit-agnostic. */
typedef enum {
    EKEY_NONE = 0,
    EKEY_ESCAPE,
    EKEY_BACKSPACE,
    EKEY_DELETE,
    EKEY_RETURN,
    EKEY_LEFT,
    EKEY_RIGHT,
    EKEY_UP,
    EKEY_DOWN,
    EKEY_SHIFT_LEFT,
    EKEY_SHIFT_RIGHT,
    EKEY_SHIFT_UP,
    EKEY_SHIFT_DOWN,
    EKEY_REDO,
    EKEY_INSERT_CUT,
    EKEY_INSERT_COPY,
    EKEY_INSERT_PASTE
} EditorSpecialKey;

typedef struct {
    char **line_data;
    size_t *line_len;
    size_t line_count;
    size_t cur_line;
    size_t cur_col;
} UndoSnapshot;

typedef struct {
    Buffer *buf;
    EditorMode mode;
    size_t cur_line;
    size_t cur_col;
    size_t top_line;
    char pending_op;
    int leader_pending;
    char cmdline[256];
    size_t cmdlen;
    char status[256];
    char search_pattern[256];
    int search_requested;
    int search_backward;
    int command_menu_requested;
    int user_command_requested;
    int user_command_index;
    int sel_active;
    size_t sel_anchor_line;
    size_t sel_anchor_col;
    int sel_inclusive;
    char *yank_text;
    size_t yank_len;
    int yank_dirty;
    int paste_requested;
    UndoSnapshot *undo_stack;
    size_t undo_count, undo_cap;
    UndoSnapshot *redo_stack;
    size_t redo_count, redo_cap;
    int want_quit;
} Editor;

void editor_init(Editor *ed, Buffer *buf);
void editor_deinit(Editor *ed);
void editor_handle_key(Editor *ed, EditorSpecialKey special, const char *text, int text_len);
void editor_clamp_cursor(Editor *ed);
void editor_ensure_visible(Editor *ed, size_t visible_rows);
void editor_selection_start(Editor *ed);
void editor_selection_clear(Editor *ed);
int editor_has_selection(const Editor *ed);
void editor_selection_range(const Editor *ed, size_t *from_line, size_t *from_col,
                             size_t *to_line, size_t *to_col);
int editor_get_selection_text(const Editor *ed, char **out_text, size_t *out_len);
void editor_paste_text(Editor *ed, const char *text, size_t len);
void editor_yank_selection(Editor *ed);
void editor_cut_selection(Editor *ed);
void editor_run_command(Editor *ed, const char *cmd);
const char *const *editor_command_names(int *out_count);
void editor_replace_buffer_text(Editor *ed, const char *text, size_t len);

#endif
