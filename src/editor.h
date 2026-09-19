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
    EKEY_NEXT,  /* Page Down: jump +XENOED_PAGE_JUMP_LINES lines */
    EKEY_PRIOR, /* Page Up:   jump -XENOED_PAGE_JUMP_LINES lines */
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
    size_t left_col;   /* byte offset of the first visible byte of each line,
                        * for horizontal scrolling (todo #35). Char-aligned;
                        * always 0 when nothing is scrolled. Kept in the
                        * toolkit-agnostic Editor only so the view layer
                        * (render.c, the only place that can measure pixel
                        * widths) has one home for it -- editor.c itself
                        * never reads it. */

    /* Viewport-follow state. editor_ensure_visible() centers the cursor
     * vertically whenever cur_line moves, but must NOT recenter when the
     * viewport itself moved under a stationary cursor (mouse wheel) or
     * when the mouse placed the cursor where it pointed. follow_line
     * remembers the cur_line the view last followed: a mismatch means the
     * cursor moved by navigation (recenter); a match means only top_line
     * moved (leave it alone). no_recenter is a one-shot flag set by the
     * mouse handlers so that a click/drag position isn't mistaken for a
     * navigation move on the next redraw. */
    size_t follow_line;
    int no_recenter;

    char pending_op;   /* 0, or 'd'/'y'/'m'/'\''/'r' after a lone
                        * 'd'/'y'/'m'/'\''/'r' in normal mode, awaiting the
                        * repeat, the mark letter, or the replacement char */
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

    /* Ctrl+G: main.c runs a bare "goto line:" dmenu (no items), parses the
     * typed number, and jumps the cursor to that line -- the same jump
     * `:42` performs via editor_run_command(), shared through that path so
     * the two entry points can't drift apart. Left to main.c for the same
     * reason as search: this file has no OS access to spawn dmenu. */
    int goto_line_requested;

    /* '!' filter: main.c shows an embedded dmenu for a shell command, runs
     * it with either the whole buffer or the current selection on stdin,
     * and replaces that span with stdout on exit 0. Selection is kept
     * intact until a successful replace (cancel/failure leave it alone).
     * external_filter_whole_buffer distinguishes normal-mode ! (buffer)
     * from visual-mode ! (selection). */
    int external_filter_requested;
    int external_filter_whole_buffer;

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

    /* Disk-change conflict on save (':w', ':w <path>', ':wq', ':x'). When
     * buffer_disk_changed() reports the file moved on disk, editor.c
     * refuses to clobber it silently: instead of saving, it sets this
     * flag and stashes exactly what the pending save would have been in
     * save_conflict_path (first byte '\0' == "use b->filename") and
     * save_conflict_quit (the command was ':wq'/':x', so a successful
     * save must quit), so main.c can pop its "overwrite or reload from
     * disk?" warning -- the dialog needs a subprocess, which this
     * toolkit-agnostic file never spawns. main.c resolves it with
     * editor_save_force() (overwrite) or editor_run_command(ed, "e!")
     * (reload); a cancel just clears the flag and the buffer stays
     * unsaved, the status bar's [!] still showing. */
    int save_conflict_requested;
    int save_conflict_quit;
    char save_conflict_path[256];

    /* Selection state (insert-mode Shift+arrow/mouse-drag, or Visual mode).
     * The anchor is fixed where the selection began; the moving endpoint is
     * always the current cursor position, so we only need to track the
     * anchor separately. */
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
    /* True for linewise visual mode ('V'): the selection covers whole
     * lines from the smaller of (anchor, cursor) line through the larger,
     * regardless of column. Yanked text then ends in '\n' (same heuristic
     * as yy), and delete removes the lines entirely rather than splicing
     * mid-line. Kept alongside sel_inclusive for the same reason -- visual
     * 'p' leaves MODE_VISUAL before paste runs. */
    int sel_linewise;

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

    /* Marks (a-z): named positions the user can jump back to. Each mark
     * records the line and column where 'm'+letter was pressed; SIZE_MAX
     * in marks_line means "unset". ' goes to the marked line's column 0,
     * as in vim. */
    size_t marks_line[26];
    size_t marks_col[26];

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

/* Adjust top_line so cur_line is centered vertically in a viewport of
 * `visible_rows` lines (clamped at both ends when the buffer is shorter
 * or the cursor is near the top or bottom edge).  A navigation move
 * (arrows, page up/down, goto line, search) always triggers centering;
 * a wheel scroll or mouse placement is left undisturbed.  Call once per
 * frame before rendering, from the render layer. */
void editor_ensure_visible(Editor *ed, size_t visible_rows);

/* Scroll the viewport by `delta` lines (negative = up, positive = down),
 * clamped so top_line never past the last full page. The cursor is kept
 * inside the new viewport so a following editor_ensure_visible() won't
 * undo the scroll. Used by main.c for mouse-wheel Button4/Button5. */
void editor_scroll_by(Editor *ed, int delta, size_t visible_rows);

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

/* Mallocs *out_text (caller frees) with the word under the cursor: the
 * maximal run of non-whitespace bytes on the current line touching the
 * cursor; a cursor on whitespace spans to the nearest word (left first,
 * then right). No trailing newline -- the word is an inline span, unlike a
 * linewise selection. Returns 0 if the line holds no word at all. This is
 * what feeds CMD_INPUT_WORD external commands; see editor_replace_word_text
 * for the "replace it with the script's stdout" half. */
int editor_get_word_text(const Editor *ed, char **out_text, size_t *out_len);

/* Inserts `text` (as fetched from CLIPBOARD, or from anywhere else) at the
 * cursor. If an active selection exists, it's replaced first, same as
 * typing does. Follows a vim-like linewise/characterwise heuristic: text
 * ending in '\n' is inserted as whole new line(s) below the cursor line;
 * text without a trailing '\n' is inserted inline at the cursor, splitting
 * the current line across any embedded newlines. */
void editor_paste_text(Editor *ed, const char *text, size_t len);

/* Replaces the active selection with `text` as one undo step. Unlike
 * editor_paste_text, a successful empty `text` (len == 0) deletes the
 * selection rather than no-op'ing -- needed for filter commands that
 * produce no stdout (e.g. `true`, `grep` with no matches). Returns 0 if
 * there is no active selection. Leaves MODE_NORMAL on success. */
int editor_replace_selection_text(Editor *ed, const char *text, size_t len);

/* Replaces the word under the cursor (per editor_get_word_text) with
 * `text` as one undo step -- the CMD_INPUT_WORD counterpart to
 * editor_replace_selection_text: an external command's exit-0 stdout lands
 * exactly where its stdin's word came from. An empty `text` deletes the
 * word. Cursor moves to the replacement's first character. Returns 0 if
 * there's no word under the cursor. Only meaningful from normal mode
 * (editor_request_user_command already refuses visual-mode trigger). */
int editor_replace_word_text(Editor *ed, const char *text, size_t len);

/* Replaces the word under the cursor with `text` as one undo step,
 * exactly like editor_replace_word_text, but lands the cursor AFTER the
 * replacement instead of on its first character -- the
 * CMD_INPUT_INSERT_WORD counterpart, for completion: main.c feeds the
 * partial word under the cursor to the script on stdin, then calls this
 * with the completed candidate, leaving insert mode's cursor past it so
 * typing continues. Same "no word under cursor => returns 0" and
 * empty-text-deletes-the-word behavior. */
int editor_replace_word_text_at_end(Editor *ed, const char *text, size_t len);

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

/* Parses and runs one ':' command (e.g. "w", "w path", "e", "e!",
 * "q", "q!", "wq", "x") -- what used to be typed character-by-character
 * into the inline ':' bar now arrives here as a single string, typically
 * from a dmenu picker. Unknown commands and an empty string are both
 * handled gracefully (status message / silent no-op respectively), same as
 * before. */
void editor_run_command(Editor *ed, const char *cmd);

/* The "overwrite" half of the disk-changed save conflict: performs the
 * save the conflicted :w/:wq/:x deferred (same target path, same
 * save-and-quit intent), skipping the buffer_disk_changed() guard main.c
 * has already shown the user. The "reload from disk" half is plain
 * `:e!` via editor_run_command(). Only meaningful after
 * editor_run_command() set save_conflict_requested; clears it. */
void editor_save_force(Editor *ed);

/* The known ':' commands, for anything (a dmenu-based picker, currently)
 * that wants to offer them as a menu. This list and editor_run_command()'s
 * own dispatch are maintained by hand, not generated from one another --
 * editor_run_command() needs per-command argument handling ("w" takes an
 * optional path, others don't) that a simple name list can't express, so
 * unifying them isn't worth the complexity at this scale. Adding a new
 * command means updating both, by design, not by oversight. */
const char *const *editor_command_names(int *out_count);

/* Inserts `text` at the cursor exactly as if it had been typed there, as
 * one undo step: embedded '\n' breaks the line (a trailing '\n' included,
 * pushing the rest of the line down and leaving the cursor on the new
 * line), an active selection is replaced first, and the editor stays in
 * its current mode with the cursor after the inserted text. This is the
 * insertion half of a CMD_INPUT_INSERT external command -- main.c strips
 * trailing newlines from the command's stdout before calling it, so most
 * commands land inline -- but it's a plain editor primitive, usable
 * anywhere. Unlike editor_paste_text there is no linewise/characterwise
 * heuristic: the bytes are bytes, the cursor is a boundary. */
void editor_insert_at_cursor(Editor *ed, const char *text, size_t len);

/* Replaces the ENTIRE buffer with `text` (an external command's stdout,
 * for a CMD_INPUT_BUFFER command) as one undo step. Resets the cursor to
 * (0,0), since the old position may not correspond to anything sensible
 * in entirely different content, and drops out of visual mode if that's
 * where this was triggered from -- any active selection's byte offsets
 * are meaningless after a full replace, same reasoning as
 * editor_cut_selection() dropping out of visual mode for the same reason. */
void editor_replace_buffer_text(Editor *ed, const char *text, size_t len);

#endif
