#include "editor.h"
#include "config.h"
#include "utf8.h"
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

    if (fl == tl) {
        Line *l = buffer_line(b, fl);
        line_delete_bytes(l, fc, tc - fc);
    } else {
        Line *first = buffer_line(b, fl);
        const Line *last = buffer_line(b, tl);
        size_t tail_len = last->len - tc;
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

    if (!out) out = malloc(1);
    *out_text = out;
    *out_len = len;
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
    memcpy(copy, l->data, l->len);
    copy[l->len] = '\n';
    editor_set_yank(ed, copy, l->len + 1);
    set_status(ed, "1 line yanked");
}

void editor_paste_text(Editor *ed, const char *text, size_t len) {
    if (len == 0) return;
    Buffer *b = ed->buf;
    int insert_mode = (ed->mode == MODE_INSERT);

    editor_checkpoint(ed);

    if (editor_has_selection(ed)) editor_delete_selection(ed);

    int linewise = (text[len - 1] == '\n');

    if (linewise) {
        size_t line_start = 0;
        size_t insert_at = paste_before_requested ? ed->cur_line : ed->cur_line + 1;
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
        size_t paste_col = (insert_mode || paste_before_requested) ? ed->cur_col : ed->cur_col + 1;
        for (size_t i = 0; i <= len; i++) {
            if (i < len && text[i] != '\n') continue;

            Line *l = buffer_line(b, ed->cur_line);
            line_insert_bytes(l, paste_col, text + seg_start, i - seg_start);
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
    paste_before_requested = 0;
    editor_clamp_cursor(ed);
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

    if (cmd->input == CMD_INPUT_SELECTION && !editor_has_selection(ed)) {
        set_status(ed, "E: %s needs a selection", cmd->name ? cmd->name : cmd->script);
        return;
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
    static const char *names[5 + 64];
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
                line_delete_bytes(l, ed->cur_col, next - ed->cur_col);
                b->dirty = 1;
                editor_clamp_cursor(ed);
            }
            break;
        case 'd': ed->pending_op = 'd'; break;
        case 'y': ed->pending_op = 'y'; break;
        case 'p': paste_before_requested = 0; ed->paste_requested = 1; break;
        case 'P': paste_before_requested = 1; ed->paste_requested = 1; break;
        case 'u': editor_undo(ed); break;
        case 'v':
            editor_selection_start(ed);
            ed->sel_inclusive = 1;
            ed->mode = MODE_VISUAL;
            break;
        case XENOED_LEADER: ed->leader_pending = 1; break;
        case ':': ed->command_menu_requested = 1; break;
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
        editor_dispatch_leader_key(ed, c);
        return;
    }

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
            move_left(ed);
            editor_clamp_cursor(ed);
        } else if (ed->mode == MODE_SEARCH) {
            ed->mode = MODE_NORMAL;
            ed->cmdlen = 0;
        } else if (ed->mode == MODE_VISUAL) {
            ed->mode = MODE_NORMAL;
        }
        ed->selection_clear(ed); 
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
