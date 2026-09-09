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
    EKEY_RETURN,
    EKEY_LEFT,
    EKEY_RIGHT,
    EKEY_UP,
    EKEY_DOWN,
    EKEY_SHIFT_LEFT,
    EKEY_SHIFT_RIGHT,
    EKEY_SHIFT_UP,
    EKEY_SHIFT_DOWN,
    EKEY_REDO /* Ctrl+R; 'u' itself needs no entry since it's plain text */
} EditorSpecialKey;

/* A full deep copy of the buffer's lines plus where the cursor was, used as
 * one undo/redo step. See editor_checkpoint()'s comment in editor.c for why
 * whole-buffer snapshots (rather than per-edit diffs) were chosen, and for
 * the granularity rules for when one gets taken. Exposed here (rather than
 * kept opaque in editor.c) only because Editor needs to hold an array of
 * them -- nothing outside editor.c actually touches its fields. */
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

    size_t cur_line;   /* 0-based line index */
    size_t cur_col;    /* byte offset into current line, always on a UTF-8 boundary */

    size_t top_line;   /* first visible line, for vertical scrolling */

    char pending_op;   /* 0, or 'd'/'y' after a lone 'd'/'y' in normal mode, awaiting the repeat */
    int leader_pending; /* true right after XENOED_LEADER, awaiting the command key */

    char cmdline[256]; /* text typed after '/' (search mode) */
    size_t cmdlen;

    char status[256];  /* transient status/error message shown in the status bar */

    /* Search. As with yank_dirty/paste_requested, editor.c can't itself
     * spawn grep -- no OS access from this toolkit-agnostic file -- so it
     * just records what's wanted; main.c does the actual subprocess work
     * (see perform_search() there) and writes the result straight into
     * cur_line/cur_col like any other cursor move. */
    char search_pattern[256];   /* last committed pattern; '\0' if none yet */
    int search_requested;       /* main.c should run a search now */
    int search_backward;        /* direction for search_requested: 0 = 'n'-style forward, 1 = 'N'-style backward */

    /* ':' no longer opens an inline typing bar -- pressing it just sets
     * this flag, and main.c shows a dmenu picker (seeded from
     * editor_command_names() below) and feeds whatever comes back into
     * editor_run_command(). Same request-flag shape as search/paste. */
    int command_menu_requested;

    /* User-defined external command (config.h's XENOED_COMMANDS), matched
     * by either the leader key or the ':' picker. main.c reads these,
     * spawns the subprocess per the matched entry's `input` kind, and
     * calls back with the result via editor_replace_buffer_text() (for
     * CMD_INPUT_BUFFER) or editor_paste_text() (for CMD_INPUT_SELECTION,
     * which already does exactly the right thing for "replace the
     * selection with this text" -- no new logic needed there at all).
     * user_command_index indexes XENOED_COMMANDS directly; main.c already
     * has that array visible via config.h, same as editor.c does. */
    int user_command_requested;
    int user_command_index;

    /* Selection state (insert mode only, for now). The anchor is fixed
     * where the selection began; the moving endpoint is always the current
     * cursor position, so we only need to track the anchor separately. */
    int sel_active;
    size_t sel_anchor_line;
    size_t sel_anchor_col;
    /* True for a selection made in Visual mode (cursor rests ON a
     * character, so the selection is inclusive of both endpoints);
     * false for one made via insert-mode Shift+arrow/mouse-drag (cursor
     * rests BETWEEN characters, so it's exclusive at the cursor end).
     * Deliberately tracked separately from ed->mode rather than checked
     * via `ed->mode == MODE_VISUAL` at read-time: visual-mode 'p' flips
     * mode back to MODE_NORMAL before the actual paste (which needs to
     * see the correct inclusive range) happens, once main.c fetches
     * CLIPBOARD content. See editor_selection_range() in editor.c. */
    int sel_inclusive;

    /* Yank/paste ("registers", vim-speak). Copy is fully local -- editor.c
     * can produce yank_text by itself from the buffer -- but paste needs
     * actual X11 CLIPBOARD content, which this toolkit-agnostic file has no
     * way to fetch. So the split is: editor.c decides *when* a yank/paste
     * happens and does the buffer-side work; main.c owns the X11 round
     * trips and watches these two flags to know when to act.
     *   yank_dirty: main.c should claim CLIPBOARD ownership; yank_text now
     *               holds what to hand back when asked.
     *   paste_requested: main.c should fetch CLIPBOARD and call
     *                     editor_paste_text() with the result. */
    char *yank_text;
    size_t yank_len;
    int yank_dirty;
    int paste_requested;

    /* Undo/redo stacks of whole-buffer snapshots. New edits (via
     * editor_checkpoint()) push onto undo_stack and clear redo_stack; 'u'
     * and Ctrl+R move a snapshot between the two without touching the
     * other one, in the usual undo/redo way. */
    UndoSnapshot *undo_stack;
    size_t undo_count, undo_cap;
    UndoSnapshot *redo_stack;
    size_t redo_count, redo_cap;

    int want_quit;
} Editor;

void editor_init(Editor *ed, Buffer *buf);
void editor_deinit(Editor *ed);

/* Feed one input event to the editor. `special` is EKEY_NONE for ordinary
 * text input, in which case `text`/`text_len` holds the UTF-8 bytes produced
 * by the input method for this key press (may be empty for pure modifier
 * presses -- callers should simply not call this in that case). */
void editor_handle_key(Editor *ed, EditorSpecialKey special, const char *text, int text_len);

/* Clamp cur_col to a valid position for the current mode (normal mode may
 * not rest past the last character of a non-empty line; insert mode may
 * rest one past it). Call after any edit that could invalidate cur_col. */
void editor_clamp_cursor(Editor *ed);

/* Adjust top_line so cur_line stays within a viewport of `visible_rows`
 * lines. Call once per frame before rendering, from the render layer. */
void editor_ensure_visible(Editor *ed, size_t visible_rows);

/* Selection (insert mode only). Exposed publicly because the mouse-drag
 * handler in main.c manipulates it directly, outside editor_handle_key. */
void editor_selection_start(Editor *ed);   /* anchor = current cursor position */
void editor_selection_clear(Editor *ed);
int editor_has_selection(const Editor *ed); /* false if inactive or zero-length */
/* Normalized (from <= to) selection endpoints. Only meaningful if
 * editor_has_selection() is true. */
void editor_selection_range(const Editor *ed, size_t *from_line, size_t *from_col,
                             size_t *to_line, size_t *to_col);

/* Mallocs *out_text (caller frees) with the text of the current selection,
 * multi-line spans joined with '\n'. Returns 0 (leaving *out_text/out_len
 * untouched) if there's no active selection. Used both to serve X11
 * PRIMARY requests and could serve any other "give me the selection" need. */
int editor_get_selection_text(const Editor *ed, char **out_text, size_t *out_len);

/* Inserts `text` (as fetched from CLIPBOARD, or from anywhere else) at the
 * cursor. If an active selection exists, it's replaced first, same as
 * typing does. Follows a vim-like linewise/characterwise heuristic: text
 * ending in '\n' is inserted as whole new line(s) below the cursor line;
 * text without a trailing '\n' is inserted inline at the cursor, splitting
 * the current line across any embedded newlines. */
void editor_paste_text(Editor *ed, const char *text, size_t len);

/* Copy/cut for mouse- or menu-driven callers (the right-click context
 * menu), deliberately NOT sharing code with visual-mode 'y' despite doing
 * almost the same thing: keyboard 'y' follows vim's convention of exiting
 * visual mode and repositioning the cursor to the selection start, which
 * is correct for a transient vim mode but wrong for a GUI "Copy" action,
 * which should touch nothing else. No-ops if there's no active selection.
 *
 * editor_cut_selection() DOES share code with visual-mode 'd'/'x', since
 * "delete a selection" has one correct resulting state (cursor at the
 * deletion point, selection gone) regardless of how it was triggered --
 * unlike copy, there's no non-destructive version to diverge into. */
void editor_yank_selection(Editor *ed);
void editor_cut_selection(Editor *ed);

/* Parses and runs one ':' command (e.g. "w", "w path", "q", "q!", "wq",
 * "x") -- what used to be typed character-by-character into the inline
 * ':' bar now arrives here as a single string, typically from a dmenu
 * picker. Unknown commands and an empty string are both handled
 * gracefully (status message / silent no-op respectively), same as
 * before. */
void editor_run_command(Editor *ed, const char *cmd);

/* The known ':' commands, for anything (a dmenu-based picker, currently)
 * that wants to offer them as a menu. This list and editor_run_command()'s
 * own dispatch are maintained by hand, not generated from one another --
 * editor_run_command() needs per-command argument handling ("w" takes an
 * optional path, others don't) that a simple name list can't express, so
 * unifying them isn't worth the complexity at this scale. Adding a new
 * command means updating both, by design, not by oversight. */
const char *const *editor_command_names(int *out_count);

/* Replaces the ENTIRE buffer with `text` (an external command's stdout,
 * for a CMD_INPUT_BUFFER command) as one undo step. Resets the cursor to
 * (0,0), since the old position may not correspond to anything sensible
 * in entirely different content, and drops out of visual mode if that's
 * where this was triggered from -- any active selection's byte offsets
 * are meaningless after a full replace, same reasoning as
 * editor_cut_selection() dropping out of visual mode for the same reason. */
void editor_replace_buffer_text(Editor *ed, const char *text, size_t len);

#endif
