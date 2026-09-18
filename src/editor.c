#include "editor.h"
#include "config.h"
#include "utf8.h"
#include <ctype.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* One editor instance exists in xenoed. This transient flag distinguishes
 * normal-mode P (paste before) from p (paste after) without making the X11
 * side-effect API carry vim-specific paste direction state. */
static int paste_before_requested;

static void undo_stack_clear(UndoSnapshot **stack, size_t *count, size_t *cap);

void editor_init(Editor *ed, Buffer *buf) {
    memset(ed, 0, sizeof(*ed));
    ed->buf = buf;
    ed->mode = MODE_NORMAL;
    for (int i = 0; i < 26; i++) ed->marks_line[i] = (size_t)-1;
    for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
        if (XENOED_COMMANDS[i].key.mod == XENOED_MOD_NONE) continue;
        for (int j = i + 1; XENOED_COMMANDS[j].script != NULL; j++) {
            if (XENOED_COMMANDS[j].key.mod == XENOED_MOD_NONE) continue;
            if (XENOED_COMMANDS[j].key.mod == XENOED_COMMANDS[i].key.mod &&
                XENOED_COMMANDS[j].key.key == XENOED_COMMANDS[i].key.key) {
                const char *modname[] = { "(picker only)", "leader", "Ctrl", "plain" };
                fprintf(stderr,
                        "xenoed: warning: commands \"%s\" and \"%s\" in config.h "
                        "both bind %s '%c'\n",
                        XENOED_COMMANDS[i].name ? XENOED_COMMANDS[i].name : "(unnamed)",
                        XENOED_COMMANDS[j].name ? XENOED_COMMANDS[j].name : "(unnamed)",
                        modname[XENOED_COMMANDS[i].key.mod],
                        XENOED_COMMANDS[i].key.key);
            }
        }
    }

    /* Plain bindings share the same single-key namespace as xenoed's own
     * normal/visual-mode commands, so a binding that shadows a built-in is
     * a footgun the author of a config deserves to be told about. Ctrl
     * bindings are delivered as ASCII control characters, which insert
     * mode already interprets for Ctrl+X/C/V; a table entry copying one of
     * those never fires from insert mode. ' ' is the leader key: binding it
     * plain would swallow the leader namespace entirely, so that's listed
     * as a built-in too. */
    const char normal_builtins[] = "hjl k0$GgiaAIoOrDxdypPuvV  :!/nN'm";
    const char visual_builtins[] = "hjl k0$GgydxpvV:!";
    const char insert_ctrl[] = "xcv";
    for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
        const XenoedKey *k = &XENOED_COMMANDS[i].key;
        const char *where = NULL;
        if (k->mod == XENOED_MOD_PLAIN) {
            if (strchr(normal_builtins, k->key) || strchr(visual_builtins, k->key))
                where = "built-in command";
        } else if (k->mod == XENOED_MOD_CTRL) {
            if (k->key == 'r' || strchr(insert_ctrl, k->key))
                where = "existing Ctrl+ shortcut";
        }
        if (where) {
            fprintf(stderr,
                    "xenoed: warning: command \"%s\" binds a %s using key '%c', "
                    "which shadows a %s\n",
                    XENOED_COMMANDS[i].name ? XENOED_COMMANDS[i].name : "(unnamed)",
                    k->mod == XENOED_MOD_PLAIN ? "plain" : "Ctrl",
                    k->key, where);
        }
    }
}

void editor_deinit(Editor *ed) {
    free(ed->yank_text);
    ed->yank_text = NULL;
    undo_stack_clear(&ed->undo_stack, &ed->undo_count, &ed->undo_cap);
    undo_stack_clear(&ed->redo_stack, &ed->redo_count, &ed->redo_cap);
}

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

    const Line *l = buffer_line(b, ed->cur_line);
    size_t len = l->len;
    size_t max_col;
    if (cursor_rests_on_char(ed)) {
        max_col = (len == 0) ? 0 : utf8_prev_boundary(l->data, len);
    } else {
        max_col = len;
    }
    if (ed->cur_col > max_col) ed->cur_col = max_col;
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

void editor_scroll_by(Editor *ed, int delta, size_t visible_rows) {
    if (visible_rows == 0 || delta == 0) return;

    size_t count = ed->buf->count;
    size_t max_top = (count > visible_rows) ? count - visible_rows : 0;

    if (delta < 0) {
        size_t up = (size_t)(-delta);
        ed->top_line = (ed->top_line > up) ? ed->top_line - up : 0;
    } else {
        size_t down = (size_t)delta;
        if (ed->top_line >= max_top) ed->top_line = max_top;
        else if (ed->top_line + down > max_top) ed->top_line = max_top;
        else ed->top_line += down;
    }

    /* Keep the cursor inside the viewport; otherwise the next redraw's
     * editor_ensure_visible() would snap top_line back to the cursor. */
    if (ed->cur_line < ed->top_line) {
        ed->cur_line = ed->top_line;
    } else if (ed->cur_line >= ed->top_line + visible_rows) {
        ed->cur_line = ed->top_line + visible_rows - 1;
        if (count > 0 && ed->cur_line >= count) ed->cur_line = count - 1;
    }
    editor_clamp_cursor(ed);
}

void editor_selection_start(Editor *ed) {
    ed->sel_active = 1;
    ed->sel_anchor_line = ed->cur_line;
    ed->sel_anchor_col = ed->cur_col;
}

void editor_selection_clear(Editor *ed) {
    ed->sel_active = 0;
    ed->sel_inclusive = 0;
    ed->sel_linewise = 0;
}

int editor_has_selection(const Editor *ed) {
    if (!ed->sel_active) return 0;
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
    if (ed->sel_linewise) {
        fc = 0;
        tc = buffer_line(ed->buf, tl)->len;
    } else if (ed->sel_inclusive) {
        const Line *l = buffer_line(ed->buf, tl);
        tc = utf8_next_boundary(l->data, l->len, tc);
    }
    *from_line = fl; *from_col = fc; *to_line = tl; *to_col = tc;
}

static void editor_delete_selection(Editor *ed) {
    if (!editor_has_selection(ed)) return;

    size_t fl, fc, tl, tc;
    editor_selection_range(ed, &fl, &fc, &tl, &tc);
    Buffer *b = ed->buf;

    if (ed->sel_linewise) {
        /* Remove whole lines, same idea as dd -- leave an empty buffer
         * rather than a zero-line one, and land the cursor on the line
         * that slid into fl (or the last remaining line if we deleted
         * through EOF). */
        for (size_t i = tl; i > fl; i--) buffer_remove_line(b, i);
        buffer_remove_line(b, fl);
        if (b->count == 0) buffer_insert_line(b, 0, "", 0);
        if (fl >= b->count) fl = b->count - 1;
        ed->cur_line = fl;
        ed->cur_col = 0;
    } else if (fl == tl) {
        Line *l = buffer_line(b, fl);
        line_delete_bytes(l, fc, tc - fc);
        ed->cur_line = fl;
        ed->cur_col = fc;
    } else {
        Line *first = buffer_line(b, fl);
        const Line *last = buffer_line(b, tl);
        size_t tail_len = last->len - tc;
        line_delete_bytes(first, fc, first->len - fc);
        line_insert_bytes(first, first->len, last->data + tc, tail_len);
        for (size_t i = tl; i > fl; i--) buffer_remove_line(b, i);
        ed->cur_line = fl;
        ed->cur_col = fc;
    }

    editor_selection_clear(ed);
    b->dirty = 1;
}

static void move_left(Editor *ed) {
    if (ed->cur_col > 0) {
        const Line *l = buffer_line(ed->buf, ed->cur_line);
        ed->cur_col = utf8_prev_boundary(l->data, ed->cur_col);
    } else if (ed->cur_line > 0) {
        ed->cur_line--;
        const Line *prev = buffer_line(ed->buf, ed->cur_line);
        ed->cur_col = prev->len;
    }
    editor_clamp_cursor(ed);
}

static void move_right(Editor *ed) {
    Buffer *b = ed->buf;
    const Line *l = buffer_line(b, ed->cur_line);
    size_t next = utf8_next_boundary(l->data, l->len, ed->cur_col);
    int at_line_end = cursor_rests_on_char(ed) ? (next >= l->len) : (ed->cur_col >= l->len);

    if (at_line_end) {
        if (ed->cur_line + 1 < b->count) {
            ed->cur_line++;
            ed->cur_col = 0;
        }
    } else {
        ed->cur_col = next;
    }
    editor_clamp_cursor(ed);
}

static void move_vert(Editor *ed, int delta) {
    const Line *l = buffer_line(ed->buf, ed->cur_line);
    size_t cp = utf8_count(l->data, ed->cur_col);

    if (delta < 0) {
        if (ed->cur_line == 0) return;
        ed->cur_line--;
    } else {
        if (ed->cur_line + 1 >= ed->buf->count) return;
        ed->cur_line++;
    }

    const Line *nl = buffer_line(ed->buf, ed->cur_line);
    ed->cur_col = utf8_offset_for_count(nl->data, nl->len, cp);
    editor_clamp_cursor(ed);
}

static void set_status(Editor *ed, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ed->status, sizeof(ed->status), fmt, ap);
    va_end(ap);
}

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
        UndoSnapshot *new_stack = realloc(*stack, newcap * sizeof(UndoSnapshot));
        if (!new_stack) _exit(1);
        *stack = new_stack;
        *cap = newcap;
    }
    (*stack)[*count] = snap;
    (*count)++;
}

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
        const Line *l = buffer_line(b, i);
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
        Line *new_lines = realloc(b->lines, snap->line_count * sizeof(Line));
        if (!new_lines) return;
        b->lines = new_lines;
        b->cap = snap->line_count;
    }
    for (size_t i = 0; i < snap->line_count; i++) {
        b->lines[i] = (Line){0};
        line_set(&b->lines[i], snap->line_data[i], snap->line_len[i]);
    }
    b->count = snap->line_count;
    b->dirty = 1;

    ed->cur_line = snap->cur_line;
    ed->cur_col = snap->cur_col;
    editor_selection_clear(ed);
    editor_clamp_cursor(ed);
}

static void editor_checkpoint(Editor *ed) {
    UndoSnapshot snap;
    snapshot_capture(ed, &snap);
    undo_stack_push(&ed->undo_stack, &ed->undo_count, &ed->undo_cap, snap);
    undo_stack_trim(&ed->undo_stack, &ed->undo_count, UNDO_MAX_DEPTH);
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

static void append_bytes(char **buf, size_t *len, size_t *cap, const char *src, size_t n) {
    if (*len + n > *cap) {
        size_t newcap = *cap ? *cap * 2 : 64;
        while (newcap < *len + n) newcap *= 2;
        char *new_buf = realloc(*buf, newcap);
        if (!new_buf) _exit(1);
        *buf = new_buf;
        *cap = newcap;
    }
    if (n > 0) {
        if (!src) _exit(1);
        memcpy(*buf + *len, src, n);
        *len += n;
    }
}

int editor_get_selection_text(const Editor *ed, char **out_text, size_t *out_len) {
    if (!editor_has_selection(ed)) return 0;

    size_t fl, fc, tl, tc;
    editor_selection_range(ed, &fl, &fc, &tl, &tc);
    Buffer *b = ed->buf;

    char *out = NULL;
    size_t len = 0, cap = 0;

    if (fl == tl) {
        const Line *l = buffer_line(b, fl);
        append_bytes(&out, &len, &cap, l->data + fc, tc - fc);
    } else {
        const Line *first = buffer_line(b, fl);
        append_bytes(&out, &len, &cap, first->data + fc, first->len - fc);
        for (size_t i = fl + 1; i < tl; i++) {
            append_bytes(&out, &len, &cap, "\n", 1);
            const Line *mid = buffer_line(b, i);
            append_bytes(&out, &len, &cap, mid->data, mid->len);
        }
        append_bytes(&out, &len, &cap, "\n", 1);
        const Line *last = buffer_line(b, tl);
        append_bytes(&out, &len, &cap, last->data, tc);
    }

    /* Linewise yanks end in '\n' so paste treats them as whole lines,
     * matching yy / dd. Characterwise spans deliberately omit it. */
    if (ed->sel_linewise) append_bytes(&out, &len, &cap, "\n", 1);

    if (!out) out = malloc(1);
    *out_text = out;
    *out_len = len;
    return 1;
}

/* A "word" is a maximal run of non-whitespace bytes; whitespace here means
 * ASCII ' \t\r\n\v\f', none of which is ever a UTF-8 continuation byte, so
 * scanning the bytes directly stays correct for multi-byte text. The cursor
 * rests on a character in normal mode, on a boundary in insert mode; either
 * way, span outward from it: first the run containing the cursor, then the
 * word that ends just left of a whitespace run the cursor sits in, then the
 * first word to the right. Returns 0 only when the line is empty or all
 * whitespace. */
static int word_bounds(const Editor *ed, size_t *wstart, size_t *wend) {
    const Line *l = buffer_line(ed->buf, ed->cur_line);
    size_t len = l->len;
    if (len == 0) return 0;

    size_t col = ed->cur_col;
    if (col > len) col = len;

    size_t s = col, e = col;
    while (s > 0 && !isspace((unsigned char)l->data[s - 1])) s--;
    while (e < len && !isspace((unsigned char)l->data[e])) e++;
    if (s < e) { *wstart = s; *wend = e; return 1; }

    /* Cursor on whitespace: prefer the word ending just left of it. */
    if (s > 0) {
        size_t ls = s;
        while (ls > 0 && isspace((unsigned char)l->data[ls - 1])) ls--;
        if (ls > 0) { /* a real word must precede the whitespace run */
            size_t ws = ls;
            while (ws > 0 && !isspace((unsigned char)l->data[ws - 1])) ws--;
            if (ws < ls) {
                *wstart = ws;
                *wend = ls; /* exclusive end of the word */
                return 1;
            }
        }
    }

    /* ...otherwise the first word to the right. */
    size_t rs = e;
    while (rs < len && isspace((unsigned char)l->data[rs])) rs++;
    if (rs < len) {
        size_t re = rs;
        while (re < len && !isspace((unsigned char)l->data[re])) re++;
        *wstart = rs;
        *wend = re;
        return 1;
    }
    return 0;
}

int editor_get_word_text(const Editor *ed, char **out_text, size_t *out_len) {
    size_t ws, we;
    if (!word_bounds(ed, &ws, &we)) return 0;

    const Line *l = buffer_line(ed->buf, ed->cur_line);
    char *out = malloc(we - ws);
    if (!out) _exit(1);
    memcpy(out, l->data + ws, we - ws);
    *out_text = out;
    *out_len = we - ws;
    return 1;
}

static void editor_set_yank(Editor *ed, char *text, size_t len) {
    free(ed->yank_text);
    ed->yank_text = text;
    ed->yank_len = len;
    ed->yank_dirty = 1;
}

static void editor_yank_line(Editor *ed) {
    const Line *l = buffer_line(ed->buf, ed->cur_line);
    char *copy = malloc(l->len + 1);
    if(copy == NULL){ _exit(-1); };
    memcpy(copy, l->data, l->len);
    copy[l->len] = '\n';
    editor_set_yank(ed, copy, l->len + 1);
    set_status(ed, "1 line yanked");
}

/* Insert text at the cursor without an undo checkpoint. `insert_at_cursor`
 * forces characterwise paste at cur_col (selection replace / insert mode /
 * P); otherwise normal-mode p pastes after the character under the cursor.
 * `replacing_linewise` + `linewise_replace_at` place linewise text exactly
 * where a just-deleted linewise selection was. */
static void editor_insert_text(Editor *ed, const char *text, size_t len,
                               int insert_at_cursor,
                               int replacing_linewise,
                               size_t linewise_replace_at) {
    if (len == 0) return;
    Buffer *b = ed->buf;
    int insert_mode = (ed->mode == MODE_INSERT);
    int linewise = (text[len - 1] == '\n');

    if (linewise) {
        size_t line_start = 0;
        size_t insert_at;
        if (replacing_linewise) {
            insert_at = linewise_replace_at;
            /* Deleting every line leaves a single empty guard (buffers
             * aren't allowed to sit at count 0). Drop it so a full-buffer
             * replace doesn't leave a trailing blank. */
            if (b->count == 1 && buffer_line(b, 0)->len == 0 && insert_at == 0) {
                buffer_remove_line(b, 0);
            }
            if (insert_at > b->count) insert_at = b->count;
        } else {
            insert_at = paste_before_requested ? ed->cur_line : ed->cur_line + 1;
        }
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
        size_t seg_start = 0;
        const Line *l = buffer_line(b, ed->cur_line);
        size_t paste_col = insert_at_cursor
                       ? ed->cur_col
                       : utf8_next_boundary(l->data, l->len, ed->cur_col);
        for (size_t i = 0; i <= len; i++) {
            if (i < len && text[i] != '\n') continue;

            Line *paste_line = buffer_line(b, ed->cur_line);
            line_insert_bytes(paste_line, paste_col, text + seg_start, i - seg_start);
            paste_col += (i - seg_start);
            ed->cur_col = insert_mode ? paste_col : paste_col - 1;

            if (i < len) {
                Line *cur = buffer_line(b, ed->cur_line);
                size_t rest_len = cur->len - paste_col;
                buffer_insert_line(b, ed->cur_line + 1, cur->data + paste_col, rest_len);
                cur = buffer_line(b, ed->cur_line);
                line_delete_bytes(cur, paste_col, rest_len);
                ed->cur_line++;
                paste_col = 0;
                ed->cur_col = 0;
            }
            seg_start = i + 1;
        }
    }

    b->dirty = 1;
    editor_clamp_cursor(ed);
}

void editor_paste_text(Editor *ed, const char *text, size_t len) {
    if (len == 0) return;
    int insert_mode = (ed->mode == MODE_INSERT);

    editor_checkpoint(ed);

    /* Replacing a linewise selection should put new linewise text exactly
     * where the deleted lines were (index fl), not after the line that
     * slid into that slot -- which is what ordinary paste-after would do. */
    int replacing_linewise = 0;
    int replaced_selection = 0;
    size_t linewise_replace_at = 0;
    if (editor_has_selection(ed)) {
        if (ed->sel_linewise) {
            size_t fl, fc, tl, tc;
            editor_selection_range(ed, &fl, &fc, &tl, &tc);
            (void)fc; (void)tl; (void)tc;
            replacing_linewise = 1;
            linewise_replace_at = fl;
        }
        editor_delete_selection(ed);
        replaced_selection = 1;
    }

    editor_insert_text(ed, text, len,
                       insert_mode || paste_before_requested || replaced_selection,
                       replacing_linewise, linewise_replace_at);
    paste_before_requested = 0;
}

int editor_replace_selection_text(Editor *ed, const char *text, size_t len) {
    if (!editor_has_selection(ed)) return 0;

    int replacing_linewise = ed->sel_linewise;
    size_t linewise_replace_at = 0;
    if (replacing_linewise) {
        size_t fl, fc, tl, tc;
        editor_selection_range(ed, &fl, &fc, &tl, &tc);
        (void)fc; (void)tl; (void)tc;
        linewise_replace_at = fl;
    }

    editor_checkpoint(ed);
    editor_delete_selection(ed);
    if (ed->mode == MODE_VISUAL) ed->mode = MODE_NORMAL;

    if (len > 0) {
        editor_insert_text(ed, text, len, 1, replacing_linewise, linewise_replace_at);
    } else {
        ed->buf->dirty = 1;
        editor_clamp_cursor(ed);
    }
    return 1;
}

int editor_replace_word_text(Editor *ed, const char *text, size_t len) {
    size_t ws, we;
    if (!word_bounds(ed, &ws, &we)) return 0;

    editor_checkpoint(ed);
    Line *l = buffer_line(ed->buf, ed->cur_line);
    line_delete_bytes(l, ws, we - ws);
    if (len > 0) line_insert_bytes(l, ws, text, len);
    ed->cur_col = ws;
    ed->buf->dirty = 1;
    editor_clamp_cursor(ed);
    return 1;
}

void editor_replace_buffer_text(Editor *ed, const char *text, size_t len) {
    editor_checkpoint(ed);
    buffer_set_from_text(ed->buf, text, len);
    ed->cur_line = 0;
    ed->cur_col = 0;
    editor_selection_clear(ed);
    if (ed->mode == MODE_VISUAL) ed->mode = MODE_NORMAL;
    editor_clamp_cursor(ed);
}

/* --- :s search-and-replace -----------------------------------------------
 *
 * `:s/pattern/replacement/[g]` (leading '%' optional: forces whole-buffer
 * scope). Replaces regex matches of `pattern` with `replacement`; without
 * the trailing `g` only the first match on each line is touched, with `g`
 * every match. Normal mode `:s` works on the whole buffer; visual mode
 * `:s` works on the current selection (the leading `%` forces the whole
 * buffer even then). An empty `pattern` reuses the last `/`-search pattern.
 *
 * The pattern is grep's extended regex with the same case-insensitivity as
 * `/`-search (-E -i), applied per line via POSIX regexec -- in-process, no
 * shell, no sed -- so `replacement`, which may contain anything, can never
 * be re-interpreted by a shell. `replacement` is literal text where:
 *     '&'          the whole match
 *     '\0'..'\9'   a captured group (empty if that group didn't match)
 *     '\X'         a literal 'X' (e.g. '\\' is a single backslash)
 */

static void expand_replacement(const char *repl, size_t repl_len,
                               const char *text, const regmatch_t *rm,
                               char **out, size_t *out_len, size_t *out_cap) {
    for (size_t i = 0; i < repl_len; i++) {
        char c = repl[i];
        if (c == '\\') {
            if (i + 1 >= repl_len) break;
            char e = repl[i + 1];
            if (e >= '0' && e <= '9') {
                int g = e - '0';
                if (rm[g].rm_so >= 0)
                    append_bytes(out, out_len, out_cap, text + rm[g].rm_so,
                                 (size_t)(rm[g].rm_eo - rm[g].rm_so));
            } else {
                append_bytes(out, out_len, out_cap, &e, 1);
            }
            i++;
        } else if (c == '&') {
            append_bytes(out, out_len, out_cap, text + rm[0].rm_so,
                         (size_t)(rm[0].rm_eo - rm[0].rm_so));
        } else {
            append_bytes(out, out_len, out_cap, &c, 1);
        }
    }
}

/* Rewrite one line's matches into *out. Returns the number of replacements
 * (0 if the pattern didn't match). `line` only needs a NUL after `len`,
 * which buffer_line guarantees. */
static long sub_line(const char *line, size_t len, regex_t *re,
                     const char *repl, size_t repl_len, int global,
                     char **out, size_t *out_len, size_t *out_cap) {
    regmatch_t rm[10];
    size_t pos = 0;
    long count = 0;
    for (;;) {
        if (regexec(re, line + pos, 10, rm, 0) == REG_NOMATCH) {
            append_bytes(out, out_len, out_cap, line + pos, len - pos);
            break;
        }
        size_t start = pos + (size_t)rm[0].rm_so;
        size_t end = pos + (size_t)rm[0].rm_eo;
        append_bytes(out, out_len, out_cap, line + pos, start - pos);
        expand_replacement(repl, repl_len, line, rm, out, out_len, out_cap);
        count++;
        if (!global) {
            append_bytes(out, out_len, out_cap, line + end, len - end);
            break;
        }
        if (end == start) {
            /* Zero-width match: emit the matched-at byte and step past it so
             * the scan makes progress; a bare `$`/`.*` at end-of-line stops. */
            if (end < len) {
                append_bytes(out, out_len, out_cap, line + end, 1);
                pos = end + 1;
            } else {
                break;
            }
        } else {
            pos = end;
        }
        if (pos > len) break;
    }
    return count;
}

static long sub_whole_buffer(Editor *ed, regex_t *re,
                             const char *repl, size_t repl_len, int global) {
    Buffer *b = ed->buf;
    long total = 0;
    for (size_t i = 0; i < b->count; i++) {
        const Line *l = buffer_line(b, i);
        char *out = NULL;
        size_t olen = 0, ocap = 0;
        long n = sub_line(l->data, l->len, re, repl, repl_len, global, &out, &olen, &ocap);
        if (n > 0) {
            if (olen != l->len || (out && memcmp(out, l->data, olen) != 0)) {
                line_set(buffer_line(b, i), out ? out : "", olen);
            }
            total += n;
        }
        free(out);
    }
    return total;
}

static long sub_selection(Editor *ed, regex_t *re,
                          const char *repl, size_t repl_len, int global) {
    if (!editor_has_selection(ed)) { set_status(ed, "E: no selection"); return -1; }
    char *sel = NULL;
    size_t slen = 0;
    if (!editor_get_selection_text(ed, &sel, &slen)) {
        set_status(ed, "E: unable to read selection");
        return -1;
    }

    /* Apply per line and re-join, so `^`/`$` and the non-g "first match per
     * line" rule behave exactly as they do for the whole-buffer scope. */
    char *res = NULL;
    size_t rlen = 0, rcap = 0;
    long total = 0;
    size_t i = 0;
    while (i <= slen) {
        size_t end = i;
        while (end < slen && sel[end] != '\n') end++;
        int had_nl = (end < slen);
        char *out = NULL;
        size_t olen = 0, ocap = 0;
        total += sub_line(sel + i, end - i, re, repl, repl_len, global, &out, &olen, &ocap);
        append_bytes(&res, &rlen, &rcap, out ? out : "", olen);
        free(out);
        if (had_nl) {
            append_bytes(&res, &rlen, &rcap, "\n", 1);
            i = end + 1;
        } else {
            break;
        }
    }
    free(sel);

    if (total <= 0) { free(res); return 0; }

    /* One undo checkpoint covers the whole substitution; replaces the
     * span and drops back to normal mode. */
    editor_replace_selection_text(ed, res ? res : "", rlen);
    free(res);
    return total;
}

typedef struct {
    char *pattern;   /* malloc'd; NULL when empty (then use_last is set) */
    size_t pattern_len;
    char *repl;      /* malloc'd; may be zero-length */
    size_t repl_len;
    int global;
    int whole;       /* leading '%' seen */
} Subst;

/* Returns 1 when cmd is a substitution, 0 when the string isn't one (caller
 * may still match it against XENOED_COMMANDS), -1 after reporting a syntax
 * error. */
/* Copy one escaped unit from *pp into out. Rules (delim = substitution
 * delimiter):
 *   \DELIM  -> literal DELIM           (so / can appear in pat/repl)
 *   \\      -> both backslashes kept   (expand_replacement/regcomp unescape)
 *   earlier: regcomp sees the escape; expand_replacement handles \0-\9 etc)
 *   \X      -> both chars kept
 *   \0      -> the backslash itself (match nothing) */
static void sub_copy_escaped(char **out, size_t *out_len, size_t *out_cap,
                             const char **pp, char delim) {
    const char *p = *pp;
    if (*p == '\\' && p[1] != '\0' && p[1] == delim) {
        append_bytes(out, out_len, out_cap, &delim, 1);
        *pp = p + 2;
    } else if (*p == '\\' && p[1] == '\\') {
        append_bytes(out, out_len, out_cap, p, 2);
        *pp = p + 2;
    } else if (*p == '\\' && p[1] != '\0') {
        append_bytes(out, out_len, out_cap, p, 2);
        *pp = p + 2;
    } else {
        append_bytes(out, out_len, out_cap, p, 1);
        *pp = p + 1;
    }
}

static int sub_parse(Editor *ed, const char *cmd, Subst *st) {
    memset(st, 0, sizeof(*st));
    const char *p = cmd;
    if (*p == '%') { st->whole = 1; p++; }
    if (*p != 's') return 0;
    p++;

    char delim = *p;
    if (delim == '\0' || isspace((unsigned char)delim) || isalnum((unsigned char)delim))
        return 0;
    p++;

    char *pat = NULL;
    size_t pl = 0, pc = 0;
    for (; *p != '\0' && *p != delim; ) {
        sub_copy_escaped(&pat, &pl, &pc, &p, delim);
    }
    if (*p == '\0') {
        free(pat);
        set_status(ed, "E: bad substitution");
        return -1;
    }
    p++;

    /* regcomp needs a NUL-terminated pattern; append_bytes never wrote one. */
    {
        char *tp = realloc(pat, pl + 1);
        if (!tp) _exit(1);
        pat = tp;
        pat[pl] = '\0';
    }

    char *rep = NULL;
    size_t rl = 0, rc = 0;
    for (; *p != '\0' && *p != delim; ) {
        sub_copy_escaped(&rep, &rl, &rc, &p, delim);
    }
    if (*p == delim) p++;
    if (*p == 'g') { st->global = 1; p++; }
    if (*p != '\0') {
        free(pat);
        free(rep);
        set_status(ed, "E: bad substitution");
        return -1;
    }

    st->pattern = pat;
    st->pattern_len = pl;
    st->repl = rep;
    st->repl_len = rl;
    return 1;
}

static int run_substitute(Editor *ed, const char *cmd) {
    Subst st;
    int r = sub_parse(ed, cmd, &st);
    if (r <= 0) return r == 0 ? 0 : 1;

    if (st.pattern_len == 0) {
        if (ed->search_pattern[0] == '\0') {
            set_status(ed, "E: no previous search pattern");
            free(st.pattern);
            free(st.repl);
            return 1;
        }
        /* Reuse the last /-search pattern; st.pattern stays NULL. */
    }

    const char *pat = st.pattern_len == 0 ? ed->search_pattern : st.pattern;
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED | REG_ICASE) != 0) {
        set_status(ed, "E: invalid search pattern");
        free(st.pattern);
        free(st.repl);
        return 1;
    }

    long n;
    if (st.whole || !(ed->mode == MODE_VISUAL && editor_has_selection(ed))) {
        /* Whole buffer. Snapshot first, commit into undo only if something
         * actually changed, so a no-match :s leaves undo history intact. */
        UndoSnapshot before;
        snapshot_capture(ed, &before);
        n = sub_whole_buffer(ed, &re, st.repl, st.repl_len, st.global);
        if (n > 0) {
            undo_stack_push(&ed->undo_stack, &ed->undo_count, &ed->undo_cap, before);
            undo_stack_trim(&ed->undo_stack, &ed->undo_count, UNDO_MAX_DEPTH);
            undo_stack_clear(&ed->redo_stack, &ed->redo_count, &ed->redo_cap);
            ed->buf->dirty = 1;
            /* A '%'-prefixed :s from visual mode leaves the selection's
             * byte offsets meaningless; drop out of visual mode like any
             * whole-buffer op does. */
            if (ed->mode == MODE_VISUAL) {
                ed->mode = MODE_NORMAL;
                editor_selection_clear(ed);
            }
            editor_clamp_cursor(ed);
        } else {
            snapshot_free(&before);
        }
    } else {
        n = sub_selection(ed, &re, st.repl, st.repl_len, st.global);
    }

    regfree(&re);
    free(st.pattern);
    free(st.repl);

    if (n < 0) { /* sub_selection already reported the problem */
        return 1;
    } else if (n == 0) {
        set_status(ed, "E: pattern not found");
    } else {
        set_status(ed, "%ld replacements", n);
    }
    return 1;
}

void editor_yank_selection(Editor *ed) {
    if (!editor_has_selection(ed)) return;
    char *text_out = NULL;
    size_t tlen = 0;
    if (editor_get_selection_text(ed, &text_out, &tlen)) {
        editor_set_yank(ed, text_out, tlen);
        set_status(ed, "%zu bytes yanked", tlen);
    }
}

void editor_cut_selection(Editor *ed) {
    if (!editor_has_selection(ed)) return;
    char *text_out = NULL;
    size_t tlen = 0;
    if (editor_get_selection_text(ed, &text_out, &tlen)) {
        editor_set_yank(ed, text_out, tlen);
    }
    editor_checkpoint(ed);
    editor_delete_selection(ed);
    if (ed->mode == MODE_VISUAL) ed->mode = MODE_NORMAL;
    set_status(ed, "%zu bytes cut", tlen);
}

static void editor_request_user_command(Editor *ed, int index) {
    const XenoedCommand *cmd = &XENOED_COMMANDS[index];
    const char *label = cmd->name ? cmd->name : cmd->script;

    if (cmd->input == CMD_INPUT_SELECTION && !editor_has_selection(ed)) {
        set_status(ed, "E: %s needs a selection", label);
        return;
    }
    if (cmd->input == CMD_INPUT_WORD) {
        if (ed->mode == MODE_VISUAL) {
            set_status(ed, "E: %s is a normal-mode command", label);
            return;
        }
        size_t ws, we;
        if (!word_bounds(ed, &ws, &we)) {
            set_status(ed, "E: no word under cursor");
            return;
        }
    }

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
        if (buffer_save(b, path) == 0) set_status(ed, "\"%s\" written", b->filename);
        else set_status(ed, "E: could not write %s", b->filename);
    } else if (strcmp(cmd, "wq") == 0 || strcmp(cmd, "x") == 0) {
        if (!b->filename) { set_status(ed, "E: no file name (use :w <path> first)"); }
        else if (buffer_save(b, NULL) == 0) ed->want_quit = 1;
        else set_status(ed, "E: could not write %s", b->filename);
    } else if (cmd[0] == 's' || (cmd[0] == '%' && cmd[1] == 's')) {
        /* `s/pat/repl/[g]` search-and-replace (run_substitute returns 0 only
         * if the string isn't actually one -- then a config.h command named
         * just 's' below still gets a chance). */
        if (run_substitute(ed, cmd) > 0) return;
    } else {
        /* A bare number is a goto-line command, vim-style (`:42` jumps to
         * line 42, 1-based; 0 is an error since lines are 1-based; leading
         * zeros like `:007` are fine). Leading whitespace was already
         * skipped; trailing whitespace is tolerated like everywhere else. */
        const char *p = cmd;
        while (*p >= '0' && *p <= '9') p++;
        while (*p == ' ') p++;   /* tolerate trailing whitespace */
        if (p != cmd && *p == '\0') {
            long target = strtol(cmd, NULL, 10);
            if (target >= 1) {
                ed->cur_line = (size_t)(target - 1);
                if (ed->cur_line >= b->count) ed->cur_line = b->count - 1;
                ed->cur_col = 0;
                editor_clamp_cursor(ed);
                return;
            }
            set_status(ed, "E: unknown command: %s", cmd);
            return;
        }
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
    static const char *const builtin[] = { "w", "q", "q!", "wq", "x", "s/pat/repl/[g]" };
    static const char *names[6 + 64];
    int n = 0;
    for (size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]) && n < 6 + 64; i++) {
        names[n++] = builtin[i];
    }
    for (int i = 0; XENOED_COMMANDS[i].script != NULL && n < 6 + 64; i++) {
        if (XENOED_COMMANDS[i].name) names[n++] = XENOED_COMMANDS[i].name;
    }
    *out_count = n;
    return names;
}

/* Match an XENOED_COMMANDS entry whose keybinding is modifier `mod` +
 * key `key`, and request it. Returns 1 if matched (the key is consumed),
 * 0 if no command claims it. `key` for ctrl bindings arrives as the plain
 * lowercase letter -- callers convert the ASCII control byte first. */
static int editor_dispatch_command(Editor *ed, XenoedKeyModifier mod, char key) {
    for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
        if (XENOED_COMMANDS[i].key.mod == mod && XENOED_COMMANDS[i].key.key == key) {
            editor_request_user_command(ed, i);
            return 1;
        }
    }
    return 0;
}

/* Ctrl+key reaches the editor as its ASCII control character (Ctrl+A is
 * 0x01, Ctrl+T is 0x14, Ctrl+] is 0x1D, ...), the same representation
 * main.c's X11 layer already uses for Ctrl+X/C/V in insert mode. Return
 * the key the control byte stands for -- the lowercase letter for
 * 0x01..0x1A, a punctuation character for 0x1B..0x1F ('[', '\', ']', '^',
 * '_'; 0x1B is Ctrl+[) -- for a dispatch lookup, or 0 if `text` isn't
 * such a keypress. */
static char ctrl_character_to_key(const char *text, int len) {
    if (len == 1) {
        unsigned char c0 = (unsigned char)text[0];
        if (c0 >= 1 && c0 <= 26) return (char)('a' + c0 - 1);
        if (c0 >= 0x1B && c0 <= 0x1F) return "[\\]^_"[c0 - 0x1B];
    }
    return 0;
}

static void handle_normal(Editor *ed, EditorSpecialKey special, const char *text, int len) {
    if (special == EKEY_REDO)  { editor_redo(ed); return; }
    if (special == EKEY_UP)    { move_vert(ed, -1); return; }
    if (special == EKEY_DOWN)  { move_vert(ed, +1); return; }
    if (special == EKEY_LEFT)  { move_left(ed); return; }
    if (special == EKEY_RIGHT) { move_right(ed); return; }
    if (special != EKEY_NONE) { ed->pending_op = 0; ed->leader_pending = 0; return; }
    if (len < 1) return;

    if (len == 2 &&
        (unsigned char)text[0] == 0xC3 &&
        (unsigned char)text[1] == 0xA7) {
        ed->command_menu_requested = 1;
        return;
    }

    char c = text[0];

    if (ed->leader_pending) {
        ed->leader_pending = 0;
        editor_dispatch_command(ed, XENOED_MOD_LEADER, c);
        return;
    }

    char ctrl_key = ctrl_character_to_key(text, len);
    if (ctrl_key) {
        ed->pending_op = 0;
        if (!editor_dispatch_command(ed, XENOED_MOD_CTRL, ctrl_key)) {
            /* Ctrl+G with no XENOED_COMMANDS binding = goto line: main.c
             * opens a bare dmenu for a line number. */
            if (ctrl_key == 'g') ed->goto_line_requested = 1;
        }
        return;
    }

    if (ed->pending_op) {
        char op = ed->pending_op;
        ed->pending_op = 0;
        if (op == 'm' && c >= 'a' && c <= 'z') {
            ed->marks_line[c - 'a'] = ed->cur_line;
            ed->marks_col[c - 'a'] = ed->cur_col;
            return;
        } else if (op == '\'' && c >= 'a' && c <= 'z') {
            if (ed->marks_line[c - 'a'] != (size_t)-1) {
                ed->cur_line = ed->marks_line[c - 'a'];
                ed->cur_col = 0;
                editor_clamp_cursor(ed);
            } else {
                set_status(ed, "E: mark '%c' not set", c);
            }
            return;
        } else if (op == 'r') {
            /* 'r'+char: replace the character under the cursor with the
             * just-typed text (which may be a multi-byte UTF-8 character),
             * one undo step, cursor staying on the replacement's first
             * byte. Refuses an empty line or a cursor resting past the
             * last character. */
            Line *rl = buffer_line(ed->buf, ed->cur_line);
            if (ed->cur_col >= rl->len) {
                set_status(ed, "E: nothing to replace");
                return;
            }
            size_t rnext = utf8_next_boundary(rl->data, rl->len, ed->cur_col);
            if (rnext <= ed->cur_col) {
                set_status(ed, "E: nothing to replace");
                return;
            }
            editor_checkpoint(ed);
            line_delete_bytes(rl, ed->cur_col, rnext - ed->cur_col);
            if (len > 0) line_insert_bytes(rl, ed->cur_col, text, (size_t)len);
            ed->buf->dirty = 1;
            editor_clamp_cursor(ed);
            set_status(ed, "replaced");
            return;
        } else if (c == op) {
            if (op == 'd') {
                editor_yank_line(ed);
                editor_checkpoint(ed);
                Buffer *b = ed->buf;
                buffer_remove_line(b, ed->cur_line);
                if (b->count == 0) buffer_insert_line(b, 0, "", 0);
                b->dirty = 1;
                ed->cur_col = 0;
                editor_clamp_cursor(ed);
            } else if (op == 'y') {
                editor_yank_line(ed);
            } else if (op == 'g') {
                ed->cur_line = 0;
                ed->cur_col = 0;
                editor_clamp_cursor(ed);
            }
        }
        return;
    }

    /* Plain-letter external-command bindings are checked before the
     * built-in switch, so they can shadow xenoed's own keys at the
     * config author's discretion (warned about at startup). */
    if (editor_dispatch_command(ed, XENOED_MOD_PLAIN, c)) return;

    Buffer *b = ed->buf;
    Line *l = buffer_line(b, ed->cur_line);

    switch (c) {
        case 'h': move_left(ed); break;
        case 'l': move_right(ed); break;
        case 'j': move_vert(ed, +1); break;
        case 'k': move_vert(ed, -1); break;
        case '0': ed->cur_col = 0; break;
        case '$': ed->cur_col = (l->len == 0) ? 0 : utf8_prev_boundary(l->data, l->len); break;
        case 'G':
            ed->cur_line = (b->count > 0) ? b->count - 1 : 0;
            editor_clamp_cursor(ed);
            break;
        case 'g': ed->pending_op = 'g'; break;
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
                char *copy = malloc(next - ed->cur_col);
                if(copy == NULL){ _exit(-1); };
                memcpy(copy, l->data + ed->cur_col, next - ed->cur_col);
                editor_set_yank(ed, copy, next - ed->cur_col);
                line_delete_bytes(l, ed->cur_col, next - ed->cur_col);
                b->dirty = 1;
                editor_clamp_cursor(ed);
            }

            break;
        case 'd': ed->pending_op = 'd'; break;
        case 'y': ed->pending_op = 'y'; break;
        case 'r': ed->pending_op = 'r'; break;
        case 'D':
            if (l->len > ed->cur_col) {
                size_t dlen = l->len - ed->cur_col;
                char *copy = malloc(dlen);
                if(copy == NULL){ _exit(-1); };
                memcpy(copy, l->data + ed->cur_col, dlen);
                editor_set_yank(ed, copy, dlen);
                editor_checkpoint(ed);
                line_delete_bytes(l, ed->cur_col, dlen);
                b->dirty = 1;
                editor_clamp_cursor(ed);
                set_status(ed, "%zu bytes cut", dlen);
            }
            break;
        case 'm': ed->pending_op = 'm'; break;
        case '\'': ed->pending_op = '\''; break;
        case 'p': paste_before_requested = 0; ed->paste_requested = 1; break;
        case 'P': paste_before_requested = 1; ed->paste_requested = 1; break;
        case 'u': editor_undo(ed); break;
        case 'v':
            editor_selection_start(ed);
            ed->sel_inclusive = 1;
            ed->sel_linewise = 0;
            ed->mode = MODE_VISUAL;
            break;
        case 'V':
            editor_selection_start(ed);
            ed->sel_inclusive = 1;
            ed->sel_linewise = 1;
            ed->mode = MODE_VISUAL;
            break;
        case XENOED_LEADER: ed->leader_pending = 1; break;
        case ':': ed->command_menu_requested = 1; break;
        case '!':
            ed->external_filter_requested = 1;
            ed->external_filter_whole_buffer = 1;
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
            } else set_status(ed, "E: no previous search");
            break;
        case 'N':
            if (ed->search_pattern[0] != '\0') {
                ed->search_requested = 1;
                ed->search_backward = 1;
            } else set_status(ed, "E: no previous search");
            break;
        default: break;
    }
}

static void handle_visual(Editor *ed, EditorSpecialKey special, const char *text, int len) {
    if (special == EKEY_UP)    { move_vert(ed, -1); return; }
    if (special == EKEY_DOWN)  { move_vert(ed, +1); return; }
    if (special == EKEY_LEFT)  { move_left(ed); return; }
    if (special == EKEY_RIGHT) { move_right(ed); return; }
    if (special != EKEY_NONE) { ed->leader_pending = 0; return; }
    if (len < 1) return;

    char c = text[0];

    if (ed->leader_pending) {
        ed->leader_pending = 0;
        editor_dispatch_command(ed, XENOED_MOD_LEADER, c);
        return;
    }

    char ctrl_key = ctrl_character_to_key(text, len);
    if (ctrl_key) {
        ed->pending_op = 0;
        editor_dispatch_command(ed, XENOED_MOD_CTRL, ctrl_key);
        return;
    }

    if (ed->pending_op) {
        char op = ed->pending_op;
        ed->pending_op = 0;
        if (c == op && op == 'g') {
            ed->cur_line = 0;
            ed->cur_col = 0;
            editor_clamp_cursor(ed);
        }
        return;
    }

    if (editor_dispatch_command(ed, XENOED_MOD_PLAIN, c)) return;

    switch (c) {
        case 'h': move_left(ed); break;
        case 'l': move_right(ed); break;
        case 'j': move_vert(ed, +1); break;
        case 'k': move_vert(ed, -1); break;
        case '0': ed->cur_col = 0; break;
        case '$': {
            const Line *l = buffer_line(ed->buf, ed->cur_line);
            ed->cur_col = (l->len == 0) ? 0 : utf8_prev_boundary(l->data, l->len);
            break;
        }
        case 'G':
            ed->cur_line = (ed->buf->count > 0) ? ed->buf->count - 1 : 0;
            editor_clamp_cursor(ed);
            break;
        case 'g': ed->pending_op = 'g'; break;
        case 'y': {
            char *text_out = NULL;
            size_t tlen = 0;
            if (editor_get_selection_text(ed, &text_out, &tlen)) {
                editor_set_yank(ed, text_out, tlen);
                set_status(ed, "%zu bytes yanked", tlen);
            }
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
            editor_cut_selection(ed);
            break;
        case 'p':
            ed->mode = MODE_NORMAL;
            ed->paste_requested = 1;
            break;
        case 'v':
            ed->sel_linewise = 0;
            break;
        case 'V':
            ed->sel_linewise = 1;
            break;
        case '!':
            /* Keep visual mode + selection until main.c applies (or the
             * user cancels / the filter fails). */
            ed->external_filter_requested = 1;
            ed->external_filter_whole_buffer = 0;
            break;
        case ':':
            /* command menu from visual mode, so :s/.../... operates on the
             * selection; selection is kept until the command runs */
            ed->command_menu_requested = 1;
            break;
        case XENOED_LEADER: ed->leader_pending = 1; break;
        default: break;
    }
}

static void handle_insert(Editor *ed, EditorSpecialKey special, const char *text, int len) {
    Buffer *b = ed->buf;
    Line *l = buffer_line(b, ed->cur_line);

    size_t prev = 0;
    size_t next = 0;

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
            prev = utf8_prev_boundary(l->data, ed->cur_col);
            next = utf8_next_boundary(l->data, l->len, ed->cur_col);
            (void)prev;
            (void)next;
            editor_selection_clear(ed);
            move_left(ed);
            return;
        case EKEY_RIGHT:
            prev = utf8_prev_boundary(l->data, ed->cur_col);
            next = utf8_next_boundary(l->data, l->len, ed->cur_col);
            (void)prev;
            (void)next;
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

    if (special == EKEY_NONE && len >= 1) {
        unsigned char c0 = (unsigned char)text[0];
        if (c0 == 0x18) {
            editor_cut_selection(ed);
            return;
        }
        if (c0 == 0x03) {
            editor_yank_selection(ed);
            return;
        }
    }

    int is_edit = (special == EKEY_RETURN || special == EKEY_BACKSPACE ||
                   special == EKEY_DELETE || (special == EKEY_NONE && len >= 1));
    if (is_edit && editor_has_selection(ed)) {
        editor_delete_selection(ed);
        l = buffer_line(b, ed->cur_line);
        if (special == EKEY_DELETE || special == EKEY_BACKSPACE) return;
    }

    if (special == EKEY_RETURN) {
        size_t rest_len = l->len - ed->cur_col;
        buffer_insert_line(b, ed->cur_line + 1, l->data + ed->cur_col, rest_len);
        l = buffer_line(b, ed->cur_line);
        line_delete_bytes(l, ed->cur_col, rest_len);
        ed->cur_line++;
        ed->cur_col = 0;
        b->dirty = 1;
        return;
    }

    if (special == EKEY_BACKSPACE) {
        if (ed->cur_col > 0) {
            size_t prev_col = utf8_prev_boundary(l->data, ed->cur_col);
            line_delete_bytes(l, prev_col, ed->cur_col - prev_col);
            ed->cur_col = prev_col;
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

    if (special == EKEY_DELETE) {
        if (l->len > 0) {
            editor_checkpoint(ed);
            size_t next_col = utf8_next_boundary(l->data, l->len, ed->cur_col);
            line_delete_bytes(l, ed->cur_col, next_col - ed->cur_col);
            b->dirty = 1;
            editor_clamp_cursor(ed);
        }
        return;
    }

    if (special != EKEY_NONE) return;
    if (len < 1) return;

    unsigned char c0 = (unsigned char)text[0];
    if (c0 == 0x16) {
        ed->paste_requested = 1;
        return;
    }
    if (c0 < 0x20 && c0 != '\t') return;
    if (c0 == 0x7F) return;

    line_insert_bytes(l, ed->cur_col, text, len);
    ed->cur_col += (size_t)len;
    b->dirty = 1;
}

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
    if (!ed->paste_requested) paste_before_requested = 0;
    ed->status[0] = '\0';

    if (special == EKEY_ESCAPE) {
        if (ed->mode == MODE_INSERT) {
            ed->mode = MODE_NORMAL;
            /* Leaving insert mode steps the cursor one UTF-8 boundary left
             * -- the insert cursor rests one position past the character,
             * so normal mode should sit ON the last typed character -- but
             * unlike a plain 'h'/Left motion, Esc must NOT wrap to the end
             * of the previous line when the cursor is at a line start. */
            if (ed->cur_col > 0) {
                const Line *l = buffer_line(ed->buf, ed->cur_line);
                ed->cur_col = utf8_prev_boundary(l->data, ed->cur_col);
            }
            editor_clamp_cursor(ed);
        } else if (ed->mode == MODE_SEARCH) {
            ed->mode = MODE_NORMAL;
            ed->cmdlen = 0;
        } else if (ed->mode == MODE_VISUAL) {
            ed->mode = MODE_NORMAL;
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
