#include "editor.h"
#include "config.h"
#include "utf8.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined further down, alongside the rest of the undo/redo machinery;
 * forward-declared so editor_deinit() (which comes first, to stay next to
 * editor_init()) can use it for cleanup without duplicating its logic. */
static void undo_stack_clear(UndoSnapshot **stack, size_t *count, size_t *cap);

void editor_init(Editor *ed, Buffer *buf) {
    memset(ed, 0, sizeof(*ed));
    ed->buf = buf;
    ed->mode = MODE_NORMAL;

    /* One-time sanity check on the user's own config.h table: a silently
     * shadowed duplicate key would be a far more confusing failure mode
     * than a startup warning. Doesn't fail -- just reports it, same spirit
     * as an unrecognized ':' command reporting rather than refusing to
     * run. */
    for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
        if (XENOED_COMMANDS[i].key == 0) continue;
        for (int j = i + 1; XENOED_COMMANDS[j].script != NULL; j++) {
            if (XENOED_COMMANDS[j].key == XENOED_COMMANDS[i].key) {
                fprintf(stderr,
                        "xenoed: warning: commands \"%s\" and \"%s\" in config.h both bind key '%c'\n",
                        XENOED_COMMANDS[i].name ? XENOED_COMMANDS[i].name : "(unnamed)",
                        XENOED_COMMANDS[j].name ? XENOED_COMMANDS[j].name : "(unnamed)",
                        XENOED_COMMANDS[i].key);
            }
        }
    }
}

void editor_deinit(Editor *ed) {
    free(ed->yank_text);
    ed->yank_text = NULL;
    undo_stack_clear(&ed->undo_stack, &ed->undo_count, &ed->undo_cap);
    undo_stack_clear(&ed->redo_stack, &ed->redo_count, &ed->redo_cap);
}

/* Normal and Visual mode both keep the cursor resting ON a character
 * (never past the last one); Insert and Command mode allow it to sit right
 * after the last character too (where you'd continue typing). Shared by
 * editor_clamp_cursor() and move_right()'s line-end check below. */
static int cursor_rests_on_char(const Editor *ed) {
    return ed->mode == MODE_NORMAL || ed->mode == MODE_VISUAL;
}

void editor_clamp_cursor(Editor *ed) {
    Buffer *b = ed->buf;
    if (b->count == 0) {
        ed->cur_line = 0;
        ed->cur_col = 0;
        return;
    }
    if (ed->cur_line >= b->count) ed->cur_line = b->count - 1;

    Line *l = buffer_line(b, ed->cur_line);
    size_t len = l->len;
    size_t max_col;
    if (cursor_rests_on_char(ed)) {
        max_col = (len == 0) ? 0 : utf8_prev_boundary(l->data, len);
    } else {
        max_col = len;
    }
    if (ed->cur_col > max_col) ed->cur_col = max_col;
    /* Defensive: never leave the cursor mid-codepoint. */
    while (ed->cur_col > 0 && utf8_is_cont((unsigned char)l->data[ed->cur_col])) {
        ed->cur_col--;
    }
}

void editor_ensure_visible(Editor *ed, size_t visible_rows) {
    if (visible_rows == 0) return;
    if (ed->cur_line < ed->top_line) {
        ed->top_line = ed->cur_line;
    } else if (ed->cur_line >= ed->top_line + visible_rows) {
        ed->top_line = ed->cur_line - visible_rows + 1;
    }
}

void editor_selection_start(Editor *ed) {
    ed->sel_active = 1;
    ed->sel_anchor_line = ed->cur_line;
    ed->sel_anchor_col = ed->cur_col;
}

void editor_selection_clear(Editor *ed) {
    ed->sel_active = 0;
    ed->sel_inclusive = 0;
}

int editor_has_selection(const Editor *ed) {
    if (!ed->sel_active) return 0;
    /* Visual mode always has at least the one character under the cursor
     * selected, even with zero movement since 'v' -- unlike insert-mode
     * selection, where anchor == cursor genuinely means a zero-width,
     * nothing-selected gap. */
    if (ed->sel_inclusive) return 1;
    return !(ed->sel_anchor_line == ed->cur_line && ed->sel_anchor_col == ed->cur_col);
}

void editor_selection_range(const Editor *ed, size_t *from_line, size_t *from_col,
                             size_t *to_line, size_t *to_col) {
    size_t al = ed->sel_anchor_line, ac = ed->sel_anchor_col;
    size_t cl = ed->cur_line, cc = ed->cur_col;
    size_t fl, fc, tl, tc;
    if (al < cl || (al == cl && ac <= cc)) {
        fl = al; fc = ac; tl = cl; tc = cc;
    } else {
        fl = cl; fc = cc; tl = al; tc = ac;
    }
    if (ed->sel_inclusive) {
        /* The upper bound (whichever of anchor/cursor it turned out to be)
         * names a character to rest ON, not a boundary to stop BEFORE --
         * so advance past it to make the usual half-open [from, to)
         * convention actually include it. */
        Line *l = buffer_line(ed->buf, tl);
        tc = utf8_next_boundary(l->data, l->len, tc);
    }
    *from_line = fl; *from_col = fc; *to_line = tl; *to_col = tc;
}

/* Deletes the currently selected text (if any), leaves the cursor at the
 * start of what was selected, and clears the selection. */
static void editor_delete_selection(Editor *ed) {
    if (!editor_has_selection(ed)) return;

    size_t fl, fc, tl, tc;
    editor_selection_range(ed, &fl, &fc, &tl, &tc);
    Buffer *b = ed->buf;

    if (fl == tl) {
        Line *l = buffer_line(b, fl);
        line_delete_bytes(l, fc, tc - fc);
    } else {
        Line *first = buffer_line(b, fl);
        Line *last = buffer_line(b, tl);
        size_t tail_len = last->len - tc;
        /* Truncate the first line at fc, then splice on the surviving tail
         * of the last line; then drop every whole line in between. */
        line_delete_bytes(first, fc, first->len - fc);
        line_insert_bytes(first, first->len, last->data + tc, tail_len);
        for (size_t i = tl; i > fl; i--) buffer_remove_line(b, i);
    }

    ed->cur_line = fl;
    ed->cur_col = fc;
    editor_selection_clear(ed);
    b->dirty = 1;
}

static void move_left(Editor *ed) {
    if (ed->cur_col > 0) {
        Line *l = buffer_line(ed->buf, ed->cur_line);
        ed->cur_col = utf8_prev_boundary(l->data, ed->cur_col);
    } else if (ed->cur_line > 0) {
        /* At the start of a line: wrap to the end of the previous one.
         * Land past the last byte here (prev->len); editor_clamp_cursor
         * below pulls that back onto the last character in normal mode,
         * while in insert mode "past the last byte" is exactly the right
         * spot to keep typing from. */
        ed->cur_line--;
        Line *prev = buffer_line(ed->buf, ed->cur_line);
        ed->cur_col = prev->len;
    }
    editor_clamp_cursor(ed);
}

static void move_right(Editor *ed) {
    Buffer *b = ed->buf;
    Line *l = buffer_line(b, ed->cur_line);
    size_t next = utf8_next_boundary(l->data, l->len, ed->cur_col);

    /* "Nothing left to move onto on this line" differs slightly by mode:
     * normal mode's cursor must always rest ON a character, so it's out of
     * room as soon as the next boundary would be the end of the line;
     * insert mode's cursor can rest right after the last character, so
     * it's only out of room once it's already there. */
    int at_line_end = cursor_rests_on_char(ed) ? (next >= l->len) : (ed->cur_col >= l->len);

    if (at_line_end) {
        if (ed->cur_line + 1 < b->count) {
            ed->cur_line++;
            ed->cur_col = 0;
        }
        /* else: already at the very end of the buffer -- nowhere to go */
    } else {
        ed->cur_col = next;
    }
    editor_clamp_cursor(ed);
}

static void move_vert(Editor *ed, int delta) {
    Line *l = buffer_line(ed->buf, ed->cur_line);
    size_t cp = utf8_count(l->data, ed->cur_col);

    if (delta < 0) {
        if (ed->cur_line == 0) return;
        ed->cur_line--;
    } else {
        if (ed->cur_line + 1 >= ed->buf->count) return;
        ed->cur_line++;
    }

    Line *nl = buffer_line(ed->buf, ed->cur_line);
    ed->cur_col = utf8_offset_for_count(nl->data, nl->len, cp);
    editor_clamp_cursor(ed);
}

static void set_status(Editor *ed, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ed->status, sizeof(ed->status), fmt, ap);
    va_end(ap);
}

/* --- Undo/redo -------------------------------------------------------
 *
 * Whole-buffer snapshots, not per-edit diffs. For a "lightweight editor
 * for actual files" (not a giant-document IDE), a full deep copy per undo
 * step is cheap enough to not matter, and it sidesteps a whole class of
 * bugs that a hand-rolled diff/patch representation would risk getting
 * subtly wrong -- simplicity over cleverness, same trade-off this project
 * has made everywhere else.
 *
 * Granularity matches vim: each discrete normal-mode command (x, dd, p)
 * is its own undo step, taken immediately before that command mutates the
 * buffer. An entire insert-mode session -- everything typed between
 * i/a/A/I/o/O and Esc, including any mid-session selection-replace -- is
 * ONE undo step, because the checkpoint is taken once, when insert mode
 * is entered, and nothing inside handle_insert() ever checkpoints again.
 */

#define UNDO_MAX_DEPTH 500

static void snapshot_free(UndoSnapshot *s) {
    for (size_t i = 0; i < s->line_count; i++) free(s->line_data[i]);
    free(s->line_data);
    free(s->line_len);
}

static void undo_stack_clear(UndoSnapshot **stack, size_t *count, size_t *cap) {
    for (size_t i = 0; i < *count; i++) snapshot_free(&(*stack)[i]);
    free(*stack);
    *stack = NULL;
    *count = 0;
    *cap = 0;
}

static void undo_stack_push(UndoSnapshot **stack, size_t *count, size_t *cap, UndoSnapshot snap) {
    if (*count + 1 > *cap) {
        size_t newcap = *cap ? *cap * 2 : 8;
        *stack = realloc(*stack, newcap * sizeof(UndoSnapshot));
        *cap = newcap;
    }
    (*stack)[*count] = snap;
    (*count)++;
}

/* Bounds memory use over a long editing session: once the undo stack is
 * deeper than UNDO_MAX_DEPTH, the oldest steps are dropped. O(depth) per
 * call, but that only happens once we're already at the cap, and the cap
 * is small enough (500) for that to be unmeasurable. */
static void undo_stack_trim(UndoSnapshot **stack, size_t *count, size_t max_depth) {
    if (*count <= max_depth) return;
    size_t drop = *count - max_depth;
    for (size_t i = 0; i < drop; i++) snapshot_free(&(*stack)[i]);
    memmove(*stack, *stack + drop, (*count - drop) * sizeof(UndoSnapshot));
    *count -= drop;
}

static void snapshot_capture(const Editor *ed, UndoSnapshot *out) {
    Buffer *b = ed->buf;
    out->line_count = b->count;
    out->line_data = malloc(b->count * sizeof(char *));
    out->line_len = malloc(b->count * sizeof(size_t));
    for (size_t i = 0; i < b->count; i++) {
        Line *l = buffer_line(b, i);
        out->line_data[i] = malloc(l->len + 1);
        memcpy(out->line_data[i], l->data, l->len);
        out->line_data[i][l->len] = '\0';
        out->line_len[i] = l->len;
    }
    out->cur_line = ed->cur_line;
    out->cur_col = ed->cur_col;
}

static void snapshot_restore(Editor *ed, const UndoSnapshot *snap) {
    Buffer *b = ed->buf;
    for (size_t i = 0; i < b->count; i++) line_free(&b->lines[i]);

    if (b->cap < snap->line_count) {
        b->lines = realloc(b->lines, snap->line_count * sizeof(Line));
        b->cap = snap->line_count;
    }
    for (size_t i = 0; i < snap->line_count; i++) {
        b->lines[i] = (Line){0};
        line_set(&b->lines[i], snap->line_data[i], snap->line_len[i]);
    }
    b->count = snap->line_count;
    /* We don't track whether this snapshot happens to exactly match the
     * on-disk saved state, so conservatively mark dirty either way -- see
     * the README's known-limitations note on this. */
    b->dirty = 1;

    ed->cur_line = snap->cur_line;
    ed->cur_col = snap->cur_col;
    editor_selection_clear(ed); /* can't have a real one anyway: undo/redo are normal-mode only */
    editor_clamp_cursor(ed);
}

/* Call immediately BEFORE a normal-mode command (or an insert-mode
 * session) mutates the buffer, to record what to go back to. */
static void editor_checkpoint(Editor *ed) {
    UndoSnapshot snap;
    snapshot_capture(ed, &snap);
    undo_stack_push(&ed->undo_stack, &ed->undo_count, &ed->undo_cap, snap);
    undo_stack_trim(&ed->undo_stack, &ed->undo_count, UNDO_MAX_DEPTH);
    /* Any new edit invalidates whatever could have been redone. */
    undo_stack_clear(&ed->redo_stack, &ed->redo_count, &ed->redo_cap);
}

static void editor_undo(Editor *ed) {
    if (ed->undo_count == 0) {
        set_status(ed, "Already at oldest change");
        return;
    }
    UndoSnapshot current;
    snapshot_capture(ed, &current);
    undo_stack_push(&ed->redo_stack, &ed->redo_count, &ed->redo_cap, current);

    UndoSnapshot *top = &ed->undo_stack[ed->undo_count - 1];
    snapshot_restore(ed, top);
    snapshot_free(top);
    ed->undo_count--;
    set_status(ed, "undo");
}

static void editor_redo(Editor *ed) {
    if (ed->redo_count == 0) {
        set_status(ed, "Already at newest change");
        return;
    }
    UndoSnapshot current;
    snapshot_capture(ed, &current);
    undo_stack_push(&ed->undo_stack, &ed->undo_count, &ed->undo_cap, current);

    UndoSnapshot *top = &ed->redo_stack[ed->redo_count - 1];
    snapshot_restore(ed, top);
    snapshot_free(top);
    ed->redo_count--;
    set_status(ed, "redo");
}

/* Appends `n` bytes to a growable buffer, reallocating as needed. */
static void append_bytes(char **buf, size_t *len, size_t *cap, const char *src, size_t n) {
    if (*len + n > *cap) {
        size_t newcap = *cap ? *cap * 2 : 64;
        while (newcap < *len + n) newcap *= 2;
        *buf = realloc(*buf, newcap);
        *cap = newcap;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
}

int editor_get_selection_text(const Editor *ed, char **out_text, size_t *out_len) {
    if (!editor_has_selection(ed)) return 0;

    size_t fl, fc, tl, tc;
    editor_selection_range(ed, &fl, &fc, &tl, &tc);
    Buffer *b = ed->buf;

    char *out = NULL;
    size_t len = 0, cap = 0;

    if (fl == tl) {
        Line *l = buffer_line(b, fl);
        append_bytes(&out, &len, &cap, l->data + fc, tc - fc);
    } else {
        Line *first = buffer_line(b, fl);
        append_bytes(&out, &len, &cap, first->data + fc, first->len - fc);
        for (size_t i = fl + 1; i < tl; i++) {
            append_bytes(&out, &len, &cap, "\n", 1);
            Line *mid = buffer_line(b, i);
            append_bytes(&out, &len, &cap, mid->data, mid->len);
        }
        append_bytes(&out, &len, &cap, "\n", 1);
        Line *last = buffer_line(b, tl);
        append_bytes(&out, &len, &cap, last->data, tc);
    }

    if (!out) out = malloc(1); /* empty selection: still return a valid pointer */
    *out_text = out;
    *out_len = len;
    return 1;
}

/* "yy": copies the current line (plus a trailing '\n', which is what marks
 * it as linewise for editor_paste_text's heuristic) into the yank
 * register. Doesn't touch X11 itself -- see the yank_dirty comment in
 * editor.h for why that's main.c's job. */
/* Takes ownership of `text` (must be malloc'd), replacing whatever was
 * previously yanked, and flags main.c to claim CLIPBOARD ownership. Shared
 * by every yank/cut path (normal-mode 'yy', visual-mode y/d/x) so there's
 * one place that can't forget to set yank_dirty. */
static void editor_set_yank(Editor *ed, char *text, size_t len) {
    free(ed->yank_text);
    ed->yank_text = text;
    ed->yank_len = len;
    ed->yank_dirty = 1;
}

static void editor_yank_line(Editor *ed) {
    Line *l = buffer_line(ed->buf, ed->cur_line);
    char *copy = malloc(l->len + 1);
    memcpy(copy, l->data, l->len);
    copy[l->len] = '\n';
    editor_set_yank(ed, copy, l->len + 1);
    set_status(ed, "1 line yanked");
}

void editor_paste_text(Editor *ed, const char *text, size_t len) {
    if (len == 0) return;
    Buffer *b = ed->buf;

    editor_checkpoint(ed); /* one undo step for the whole paste, selection-replace included */

    if (editor_has_selection(ed)) editor_delete_selection(ed);

    int linewise = (text[len - 1] == '\n');

    if (linewise) {
        /* Insert each clipboard line as a whole new line below the cursor,
         * vim-'p'-style. The final '\n' just terminates the last one -- it
         * doesn't introduce a trailing empty line. */
        size_t line_start = 0;
        size_t insert_at = ed->cur_line + 1;
        size_t first_inserted = insert_at;
        for (size_t i = 0; i < len; i++) {
            if (text[i] == '\n') {
                buffer_insert_line(b, insert_at, text + line_start, i - line_start);
                insert_at++;
                line_start = i + 1;
            }
        }
        ed->cur_line = first_inserted;
        ed->cur_col = 0;
    } else {
        /* Insert inline at the cursor, splitting the current line at each
         * embedded newline (there's no trailing one here to treat as a
         * whole-line marker). */
        size_t seg_start = 0;
        for (size_t i = 0; i <= len; i++) {
            if (i < len && text[i] != '\n') continue;

            Line *l = buffer_line(b, ed->cur_line);
            line_insert_bytes(l, ed->cur_col + 1, text + seg_start, i - seg_start);
            ed->cur_col += (i - seg_start);

            if (i < len) { /* text[i] == '\n': split the line right here */
                Line *cur = buffer_line(b, ed->cur_line);
                size_t rest_len = cur->len - ed->cur_col;
                buffer_insert_line(b, ed->cur_line + 1, cur->data + ed->cur_col, rest_len);
                cur = buffer_line(b, ed->cur_line);
                line_delete_bytes(cur, ed->cur_col, rest_len);
                ed->cur_line++;
                ed->cur_col = 0;
            }
            seg_start = i + 1;
        }
    }

    b->dirty = 1;
    editor_clamp_cursor(ed);
}

void editor_replace_buffer_text(Editor *ed, const char *text, size_t len) {
    editor_checkpoint(ed);
    buffer_set_from_text(ed->buf, text, len); /* also sets b->dirty */
    ed->cur_line = 0;
    ed->cur_col = 0;
    editor_selection_clear(ed); /* whatever byte offsets it had are meaningless in new content */
    if (ed->mode == MODE_VISUAL) ed->mode = MODE_NORMAL;
    editor_clamp_cursor(ed);
}

void editor_yank_selection(Editor *ed) {
    if (!editor_has_selection(ed)) return;
    char *text_out = NULL;
    size_t tlen = 0;
    if (editor_get_selection_text(ed, &text_out, &tlen)) {
        editor_set_yank(ed, text_out, tlen);
        set_status(ed, "%zu bytes yanked", tlen);
    }
    /* Deliberately nothing else: no cursor move, no mode change, no
     * clearing the selection -- see the comment in editor.h on why this
     * differs from visual-mode 'y'. */
}

void editor_cut_selection(Editor *ed) {
    if (!editor_has_selection(ed)) return;
    char *text_out = NULL;
    size_t tlen = 0;
    if (editor_get_selection_text(ed, &text_out, &tlen)) {
        editor_set_yank(ed, text_out, tlen);
    }
    editor_checkpoint(ed);
    editor_delete_selection(ed); /* repositions the cursor, clears the selection */
    if (ed->mode == MODE_VISUAL) ed->mode = MODE_NORMAL; /* can't stay in visual with nothing selected */
    set_status(ed, "%zu bytes cut", tlen);
}

/* Shared by editor_run_command()'s user-command fallback (matched by
 * name) and editor_dispatch_leader_key() (matched by key) below -- one
 * place for the "is this actually runnable right now" check and the
 * mode-forcing, rather than duplicating it per trigger mechanism. */
static void editor_request_user_command(Editor *ed, int index) {
    const XenoedCommand *cmd = &XENOED_COMMANDS[index];

    if (cmd->input == CMD_INPUT_SELECTION && !editor_has_selection(ed)) {
        set_status(ed, "E: %s needs a selection", cmd->name ? cmd->name : cmd->script);
        return;
    }
    /* Consistent with visual-mode 'p': force normal mode BEFORE the
     * request is even set, so by the time main.c applies whatever the
     * script hands back, there's no stale visual selection left implying
     * byte offsets that may no longer mean anything. */
    if (ed->mode == MODE_VISUAL) ed->mode = MODE_NORMAL;

    ed->user_command_index = index;
    ed->user_command_requested = 1;
}

void editor_run_command(Editor *ed, const char *raw_cmd) {
    Buffer *b = ed->buf;
    const char *cmd = raw_cmd;

    while (*cmd == ' ') cmd++;

    if (cmd[0] == '\0') {
        return;
    } else if (strcmp(cmd, "q") == 0) {
        if (b->dirty) set_status(ed, "E: unsaved changes (:q! to discard)");
        else ed->want_quit = 1;
    } else if (strcmp(cmd, "q!") == 0) {
        ed->want_quit = 1;
    } else if (strcmp(cmd, "w") == 0) {
        if (!b->filename) set_status(ed, "E: no file name (use :w <path>)");
        else if (buffer_save(b, NULL) == 0) set_status(ed, "\"%s\" written", b->filename);
        else set_status(ed, "E: could not write %s", b->filename);
    } else if (strncmp(cmd, "w ", 2) == 0) {
        const char *path = cmd + 2;
        while (*path == ' ') path++;
        if (buffer_save(b, path) == 0) set_status(ed, "\"%s\" written", path);
        else set_status(ed, "E: could not write %s", path);
    } else if (strcmp(cmd, "wq") == 0 || strcmp(cmd, "x") == 0) {
        if (!b->filename) { set_status(ed, "E: no file name (use :w <path> first)"); }
        else if (buffer_save(b, NULL) == 0) ed->want_quit = 1;
        else set_status(ed, "E: could not write %s", b->filename);
    } else {
        int matched = -1;
        for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
            if (XENOED_COMMANDS[i].name && strcmp(cmd, XENOED_COMMANDS[i].name) == 0) {
                matched = i;
                break;
            }
        }
        if (matched >= 0) editor_request_user_command(ed, matched);
        else set_status(ed, "E: unknown command: %s", cmd);
    }
}

const char *const *editor_command_names(int *out_count) {
    static const char *const builtin[] = { "w", "q", "q!", "wq", "x" };
    static const char *names[5 + 64]; /* 64 is a generous, arbitrary cap on
                                        * user command count -- config.h's
                                        * table is small by nature (hand-
                                        * written, hand-bound to keys), so
                                        * this just needs to not be the
                                        * thing that runs out first. */
    int n = 0;
    for (size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]) && n < 5 + 64; i++) {
        names[n++] = builtin[i];
    }
    for (int i = 0; XENOED_COMMANDS[i].script != NULL && n < 5 + 64; i++) {
        if (XENOED_COMMANDS[i].name) names[n++] = XENOED_COMMANDS[i].name;
    }
    *out_count = n;
    return names;
}

/* Called from handle_normal()/handle_visual() once XENOED_LEADER has been
 * seen and this is the following keypress. Silently ignores a key nothing
 * is bound to -- same as any other unrecognized key in either mode. */
static void editor_dispatch_leader_key(Editor *ed, char key) {
    for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
        if (XENOED_COMMANDS[i].key != 0 && XENOED_COMMANDS[i].key == key) {
            editor_request_user_command(ed, i);
            return;
        }
    }
}

static void handle_normal(Editor *ed, EditorSpecialKey special, const char *text, int len) {
    if (special == EKEY_REDO)  { editor_redo(ed); return; }
    if (special == EKEY_UP)    { move_vert(ed, -1); return; }
    if (special == EKEY_DOWN)  { move_vert(ed, +1); return; }
    if (special == EKEY_LEFT)  { move_left(ed); return; }
    if (special == EKEY_RIGHT) { move_right(ed); return; }
    if (special != EKEY_NONE) { ed->pending_op = 0; ed->leader_pending = 0; return; }
    if (len < 1) return;

    char c = text[0];

    if (ed->leader_pending) {
        ed->leader_pending = 0;
        editor_dispatch_leader_key(ed, c);
        return;
    }

    if (ed->pending_op) {
        char op = ed->pending_op;
        ed->pending_op = 0;
        if (c == op) {
            if (op == 'd') {
                editor_checkpoint(ed);
                Buffer *b = ed->buf;
                buffer_remove_line(b, ed->cur_line);
                if (b->count == 0) buffer_insert_line(b, 0, "", 0);
                b->dirty = 1;
                ed->cur_col = 0;
                editor_clamp_cursor(ed);
            } else if (op == 'y') {
                editor_yank_line(ed);
            }
        }
        return;
    }

    Buffer *b = ed->buf;
    Line *l = buffer_line(b, ed->cur_line);

    switch (c) {
        case 'h': move_left(ed); break;
        case 'l': move_right(ed); break;
        case 'j': move_vert(ed, +1); break;
        case 'k': move_vert(ed, -1); break;
        case '0': ed->cur_col = 0; break;
        case '$': ed->cur_col = (l->len == 0) ? 0 : utf8_prev_boundary(l->data, l->len); break;

        case 'i': editor_checkpoint(ed); ed->mode = MODE_INSERT; break;
        case 'a':
            editor_checkpoint(ed);
            ed->mode = MODE_INSERT;
            if (l->len > 0) ed->cur_col = utf8_next_boundary(l->data, l->len, ed->cur_col);
            editor_clamp_cursor(ed);
            break;
        case 'A':
            editor_checkpoint(ed);
            ed->mode = MODE_INSERT;
            ed->cur_col = l->len;
            break;
        case 'I':
            editor_checkpoint(ed);
            ed->mode = MODE_INSERT;
            ed->cur_col = 0;
            break;
        case 'o':
            editor_checkpoint(ed);
            buffer_insert_line(b, ed->cur_line + 1, "", 0);
            ed->cur_line++;
            ed->cur_col = 0;
            ed->mode = MODE_INSERT;
            b->dirty = 1;
            break;
        case 'O':
            editor_checkpoint(ed);
            buffer_insert_line(b, ed->cur_line, "", 0);
            ed->cur_col = 0;
            ed->mode = MODE_INSERT;
            b->dirty = 1;
            break;
        case 'x':
            if (l->len > 0) {
                editor_checkpoint(ed);
                size_t next = utf8_next_boundary(l->data, l->len, ed->cur_col);
                line_delete_bytes(l, ed->cur_col, next - ed->cur_col);
                b->dirty = 1;
                editor_clamp_cursor(ed);
            }
            break;
        case 'd':
            ed->pending_op = 'd';
            break;
        case 'y':
            ed->pending_op = 'y';
            break;
        case 'p':
            /* Actual paste happens once main.c fetches CLIPBOARD content
             * and calls editor_paste_text(), which checkpoints itself. */
            ed->paste_requested = 1;
            break;
        case 'u':
            editor_undo(ed);
            break;
        case 'v':
            editor_selection_start(ed); /* anchor = the character currently under the cursor */
            ed->sel_inclusive = 1;
            ed->mode = MODE_VISUAL;
            break;
        case XENOED_LEADER:
            ed->leader_pending = 1;
            break;
        case ':':
            /* No inline bar anymore -- main.c shows a dmenu picker and
             * feeds the result to editor_run_command(). Stays in normal
             * mode the whole time, same shape as 'p' -> paste_requested. */
            ed->command_menu_requested = 1;
            break;
        case '/':
            ed->mode = MODE_SEARCH;
            ed->cmdlen = 0;
            ed->cmdline[0] = '\0';
            break;
        case 'n':
            if (ed->search_pattern[0] != '\0') {
                ed->search_requested = 1;
                ed->search_backward = 0;
            } else {
                set_status(ed, "E: no previous search");
            }
            break;
        case 'N':
            if (ed->search_pattern[0] != '\0') {
                ed->search_requested = 1;
                ed->search_backward = 1;
            } else {
                set_status(ed, "E: no previous search");
            }
            break;
        default:
            break;
    }
}

/* Visual mode: movement extends the selection (the anchor set by 'v' stays
 * put; the cursor doubles as the selection's moving endpoint, exactly like
 * insert-mode Shift+arrow/mouse-drag selection already works), and y/d/x/p
 * act on the resulting span instead of a whole line or single character.
 * Deliberately minimal: no vim visual-mode extras like case-changing (real
 * vim's visual 'u' means something entirely different -- lowercase the
 * selection -- so 'u' is simply unbound here rather than risk that
 * confusion) or block/linewise visual variants, just enough to make y/d
 * work on an arbitrary span from normal mode. */
static void handle_visual(Editor *ed, EditorSpecialKey special, const char *text, int len) {
    if (special == EKEY_UP)    { move_vert(ed, -1); return; }
    if (special == EKEY_DOWN)  { move_vert(ed, +1); return; }
    if (special == EKEY_LEFT)  { move_left(ed); return; }
    if (special == EKEY_RIGHT) { move_right(ed); return; }
    if (special != EKEY_NONE) { ed->leader_pending = 0; return; } /* nothing else is bound in visual mode */
    if (len < 1) return;

    char c = text[0];

    if (ed->leader_pending) {
        ed->leader_pending = 0;
        editor_dispatch_leader_key(ed, c); /* may drop back to normal mode -- see editor_request_user_command() */
        return;
    }

    switch (c) {
        case 'h': move_left(ed); break;
        case 'l': move_right(ed); break;
        case 'j': move_vert(ed, +1); break;
        case 'k': move_vert(ed, -1); break;
        case '0': ed->cur_col = 0; break;
        case '$': {
            Line *l = buffer_line(ed->buf, ed->cur_line);
            ed->cur_col = (l->len == 0) ? 0 : utf8_prev_boundary(l->data, l->len);
            break;
        }

        case 'y': {
            char *text_out = NULL;
            size_t tlen = 0;
            if (editor_get_selection_text(ed, &text_out, &tlen)) {
                editor_set_yank(ed, text_out, tlen);
                set_status(ed, "%zu bytes yanked", tlen);
            }
            /* vim: cursor lands at the start of what was selected. */
            size_t fl, fc, tl, tc;
            editor_selection_range(ed, &fl, &fc, &tl, &tc);
            ed->cur_line = fl;
            ed->cur_col = fc;
            editor_selection_clear(ed);
            ed->mode = MODE_NORMAL;
            editor_clamp_cursor(ed);
            break;
        }
        case 'd':
        case 'x':
            editor_cut_selection(ed); /* also repositions cursor, clears selection, drops to normal mode */
            break;
        case 'p':
            /* Leave the selection active -- editor_paste_text() (called
             * once main.c fetches CLIPBOARD content) replaces whatever's
             * currently selected before inserting, same as it already does
             * for insert-mode selections. Its own editor_checkpoint() call
             * covers this as one undo step. */
            ed->mode = MODE_NORMAL;
            ed->paste_requested = 1;
            break;
        case XENOED_LEADER:
            ed->leader_pending = 1;
            break;

        default:
            break;
    }
}

static void handle_insert(Editor *ed, EditorSpecialKey special, const char *text, int len) {
    Buffer *b = ed->buf;
    Line *l = buffer_line(b, ed->cur_line);

    /* Shift+arrow extends (or starts) the selection; the anchor stays put
     * and the cursor -- which doubles as the selection's moving endpoint --
     * just moves normally. Plain arrows collapse any active selection. */
    switch (special) {
        case EKEY_SHIFT_LEFT:
            if (!ed->sel_active) editor_selection_start(ed);
            move_left(ed);
            return;
        case EKEY_SHIFT_RIGHT:
            if (!ed->sel_active) editor_selection_start(ed);
            move_right(ed);
            return;
        case EKEY_SHIFT_UP:
            if (!ed->sel_active) editor_selection_start(ed);
            move_vert(ed, -1);
            return;
        case EKEY_SHIFT_DOWN:
            if (!ed->sel_active) editor_selection_start(ed);
            move_vert(ed, +1);
            return;
        case EKEY_LEFT:
            editor_selection_clear(ed);
            move_left(ed);
            return;
        case EKEY_RIGHT:
            editor_selection_clear(ed);
            move_right(ed);
            return;
        case EKEY_UP:
            editor_selection_clear(ed);
            move_vert(ed, -1);
            return;
        case EKEY_DOWN:
            editor_selection_clear(ed);
            move_vert(ed, +1);
            return;
        default:
            break;
    }

    /* Any actual edit (Enter, Backspace, or typing) replaces an active
     * selection first, same as a typical GUI text editor. */
    int is_edit = (special == EKEY_RETURN || special == EKEY_BACKSPACE ||
                   special == EKEY_DELETE || (special == EKEY_NONE && len >= 1));
    if (is_edit && editor_has_selection(ed)) {
        editor_delete_selection(ed);
        l = buffer_line(b, ed->cur_line); /* buffer may have been mutated */
        if (special == EKEY_DELETE || special == EKEY_BACKSPACE) return; /* Backspace-on-selection just deletes it */
    }

    if (special == EKEY_RETURN) {
        size_t rest_len = l->len - ed->cur_col;
        buffer_insert_line(b, ed->cur_line + 1, l->data + ed->cur_col, rest_len);
        /* buffer_insert_line may have reallocated b->lines; re-fetch l */
        l = buffer_line(b, ed->cur_line);
        line_delete_bytes(l, ed->cur_col, rest_len);
        ed->cur_line++;
        ed->cur_col = 0;
        b->dirty = 1;
        return;
    }

    if (special == EKEY_BACKSPACE) {
        if (ed->cur_col > 0) {
            size_t prev = utf8_prev_boundary(l->data, ed->cur_col);
            line_delete_bytes(l, prev, ed->cur_col - prev);
            ed->cur_col = prev;
            b->dirty = 1;
        } else if (ed->cur_line > 0) {
            Line *prevline = buffer_line(b, ed->cur_line - 1);
            size_t join_col = prevline->len;
            line_insert_bytes(prevline, prevline->len, l->data, l->len);
            buffer_remove_line(b, ed->cur_line);
            ed->cur_line--;
            ed->cur_col = join_col;
            b->dirty = 1;
        }
        return;
    }

    if (special != EKEY_NONE) return;
    if (len < 1) return;

    unsigned char c0 = (unsigned char)text[0];
    if (c0 < 0x20 && c0 != '\t') return; /* swallow stray control chars */
    if (c0 == 0x7F) return;

    line_insert_bytes(l, ed->cur_col, text, len);
    ed->cur_col += (size_t)len;
    b->dirty = 1;
}

/* Same small "type into cmdline, Enter commits, Backspace/Esc cancel"
 * shape used to be shared with handle_command() for the ':' bar, before
 * that got replaced by the dmenu-based command picker -- this is the only
 * one left using it now, for '/' search patterns. */
static void handle_search(Editor *ed, EditorSpecialKey special, const char *text, int len) {
    if (special == EKEY_RETURN) {
        if (ed->cmdlen > 0) {
            memcpy(ed->search_pattern, ed->cmdline, ed->cmdlen);
            ed->search_pattern[ed->cmdlen] = '\0';
            ed->search_requested = 1;
            ed->search_backward = 0;
        }
        ed->mode = MODE_NORMAL;
        ed->cmdlen = 0;
        return;
    }
    if (special == EKEY_BACKSPACE) {
        if (ed->cmdlen > 0) {
            size_t newlen = utf8_prev_boundary(ed->cmdline, ed->cmdlen);
            ed->cmdlen = newlen;
            ed->cmdline[newlen] = '\0';
        } else {
            ed->mode = MODE_NORMAL;
        }
        return;
    }
    if (special != EKEY_NONE) return;
    if (len < 1) return;

    if (ed->cmdlen + (size_t)len < sizeof(ed->cmdline) - 1) {
        memcpy(ed->cmdline + ed->cmdlen, text, (size_t)len);
        ed->cmdlen += (size_t)len;
        ed->cmdline[ed->cmdlen] = '\0';
    }
}

void editor_handle_key(Editor *ed, EditorSpecialKey special, const char *text, int text_len) {
    ed->status[0] = '\0';

    if (special == EKEY_ESCAPE) {
        if (ed->mode == MODE_INSERT) {
            ed->mode = MODE_NORMAL;
            move_left(ed); /* vim: leaving insert steps cursor back one */
            editor_clamp_cursor(ed);
        } else if (ed->mode == MODE_SEARCH) {
            ed->mode = MODE_NORMAL;
            ed->cmdlen = 0;
        } else if (ed->mode == MODE_VISUAL) {
            ed->mode = MODE_NORMAL; /* cancels the selection, changes nothing */
        }
        editor_selection_clear(ed);
        ed->pending_op = 0;
        return;
    }

    switch (ed->mode) {
        case MODE_NORMAL:  handle_normal(ed, special, text, text_len); break;
        case MODE_INSERT:  handle_insert(ed, special, text, text_len); break;
        case MODE_VISUAL:  handle_visual(ed, special, text, text_len); break;
        case MODE_SEARCH:  handle_search(ed, special, text, text_len); break;
    }
}
