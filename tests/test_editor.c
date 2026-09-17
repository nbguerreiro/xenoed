#include "../src/buffer.h"
#include "../src/config.h"
#include "../src/editor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static void feed(Editor *ed, const char *keys) {
    /* Very small DSL: <Esc> <BS> <CR> tokens, everything else is literal
     * UTF-8 text fed byte-by-byte as if each byte were its own keypress
     * (good enough to also test multi-byte UTF-8 insertion since we just
     * feed the whole multi-byte sequence as one "keypress" chunk when it
     * appears between {} braces). */
    const char *p = keys;
    while (*p) {
        if (strncmp(p, "<Esc>", 5) == 0) { editor_handle_key(ed, EKEY_ESCAPE, NULL, 0); p += 5; continue; }
        if (strncmp(p, "<BS>", 4) == 0) { editor_handle_key(ed, EKEY_BACKSPACE, NULL, 0); p += 4; continue; }
        if (strncmp(p, "<CR>", 4) == 0) { editor_handle_key(ed, EKEY_RETURN, NULL, 0); p += 4; continue; }
        if (*p == '{') {
            const char *end = strchr(p, '}');
            assert(end);
            editor_handle_key(ed, EKEY_NONE, p + 1, (int)(end - p - 1));
            p = end + 1;
            continue;
        }
        editor_handle_key(ed, EKEY_NONE, p, 1);
        p++;
    }
}

static void dump(Buffer *b) {
    for (size_t i = 0; i < b->count; i++) {
        Line *l = buffer_line(b, i);
        printf("  [%zu] \"%.*s\"\n", i, (int)l->len, l->data);
    }
}

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } } while (0)

int main(void) {
    /* Test 1: basic insert */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        printf("Test 1 (insert 'hello'):\n"); dump(b);
        CHECK(b->count == 1);
        CHECK(strcmp(buffer_line(b, 0)->data, "hello") == 0);
        CHECK(ed.mode == MODE_NORMAL);
        CHECK(ed.cur_col == 4); /* Esc steps back one from col 5 */
        buffer_free(b);
    }

    /* Test 2: o opens a new line below and enters insert */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ifirst<Esc>osecond<Esc>");
        printf("Test 2 (o for new line):\n"); dump(b);
        CHECK(b->count == 2);
        CHECK(strcmp(buffer_line(b, 0)->data, "first") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "second") == 0);
        buffer_free(b);
    }

    /* Test 3: dd deletes a line */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ione<Esc>otwo<Esc>othree<Esc>");
        CHECK(b->count == 3);
        ed.cur_line = 1; /* move to "two" */
        feed(&ed, "dd");
        printf("Test 3 (dd on middle line):\n"); dump(b);
        CHECK(b->count == 2);
        CHECK(strcmp(buffer_line(b, 0)->data, "one") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "three") == 0);
        buffer_free(b);
    }

    /* Test 4: x deletes char under cursor, including multi-byte UTF-8 */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        /* café: é is 2 bytes (U+00E9 = 0xC3 0xA9) */
        feed(&ed, "i{caf\xc3\xa9}<Esc>");
        printf("Test 4a (insert 'caf\xc3\xa9'):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "caf\xc3\xa9") == 0);
        CHECK(ed.cur_col == 3); /* cursor sits on the 'e-acute' start byte after Esc */
        feed(&ed, "x");
        printf("Test 4b (x deletes the 'e-acute'):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "caf") == 0);
        buffer_free(b);
    }

    /* Test 5: backspace across a line join */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ifoo<CR>bar<Esc>");
        CHECK(b->count == 2);
        ed.mode = MODE_INSERT; ed.cur_line = 1; ed.cur_col = 0; /* start of "bar" */
        feed(&ed, "<BS>");
        printf("Test 5 (backspace joins lines):\n"); dump(b);
        CHECK(b->count == 1);
        CHECK(strcmp(buffer_line(b, 0)->data, "foobar") == 0);
        CHECK(ed.cur_line == 0 && ed.cur_col == 3);
        buffer_free(b);
    }

    /* Test 6: h/l/j/k movement bounds */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihi<Esc>");
        CHECK(ed.cur_col == 1); /* on 'i' of "hi", last char index */
        feed(&ed, "l"); /* should NOT move past last char in normal mode */
        CHECK(ed.cur_col == 1);
        feed(&ed, "hh"); /* move left twice, clamps at 0 */
        CHECK(ed.cur_col == 0);
        printf("Test 6 (movement clamping): cur_col=%zu OK\n", ed.cur_col);
        buffer_free(b);
    }

    /* Test 7: save/load round-trip */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ione<CR>two<CR>three<Esc>");
        int rc = buffer_save(b, "/tmp/xenoed_test.txt");
        CHECK(rc == 0);
        buffer_free(b);

        Buffer *b2 = buffer_new();
        int lrc = buffer_load(b2, "/tmp/xenoed_test.txt");
        printf("Test 7 (save/load round-trip):\n"); dump(b2);
        CHECK(lrc == 0);
        CHECK(b2->count == 3);
        CHECK(strcmp(buffer_line(b2, 0)->data, "one") == 0);
        CHECK(strcmp(buffer_line(b2, 1)->data, "two") == 0);
        CHECK(strcmp(buffer_line(b2, 2)->data, "three") == 0);
        buffer_free(b2);
    }

    /* Test 8: shift-left selection then backspace deletes the selection */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.mode = MODE_INSERT; ed.cur_line = 0; ed.cur_col = 11; /* end of "hello world" */
        editor_handle_key(&ed, EKEY_SHIFT_LEFT, NULL, 0); /* select 'd' */
        editor_handle_key(&ed, EKEY_SHIFT_LEFT, NULL, 0); /* select 'ld' */
        editor_handle_key(&ed, EKEY_SHIFT_LEFT, NULL, 0); /* select 'rld' */
        CHECK(ed.sel_active);
        CHECK(editor_has_selection(&ed));
        editor_handle_key(&ed, EKEY_BACKSPACE, NULL, 0);
        printf("Test 8 (shift-left select + backspace deletes selection):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "hello wo") == 0);
        CHECK(!editor_has_selection(&ed));
        buffer_free(b);
    }

    /* Test 9: typing over a selection replaces it */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.mode = MODE_INSERT; ed.cur_line = 0; ed.cur_col = 0;
        for (int i = 0; i < 5; i++) editor_handle_key(&ed, EKEY_SHIFT_RIGHT, NULL, 0); /* select "hello" */
        CHECK(editor_has_selection(&ed));
        feed(&ed, "goodbye");
        printf("Test 9 (typing replaces selection):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "goodbye world") == 0);
        CHECK(!editor_has_selection(&ed));
        buffer_free(b);
    }

    /* Test 10: multi-line selection deletion via shift-down + shift-right */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ione<CR>two<CR>three<Esc>");
        CHECK(b->count == 3);
        ed.mode = MODE_INSERT; ed.cur_line = 0; ed.cur_col = 1; /* after 'o' in "one" */
        editor_handle_key(&ed, EKEY_SHIFT_DOWN, NULL, 0);  /* extend to line 1, col 1 (after 't' in "two") */
        editor_handle_key(&ed, EKEY_SHIFT_RIGHT, NULL, 0); /* -> col 2 ("tw") */
        CHECK(editor_has_selection(&ed));
        editor_handle_key(&ed, EKEY_BACKSPACE, NULL, 0);
        printf("Test 10 (multi-line selection deletion):\n"); dump(b);
        CHECK(b->count == 2);
        CHECK(strcmp(buffer_line(b, 0)->data, "oo") == 0); /* "o" + tail of "two" after col 2 = "o" */
        CHECK(strcmp(buffer_line(b, 1)->data, "three") == 0);
        buffer_free(b);
    }

    /* Test 11: plain arrow (no shift) collapses an active selection */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        ed.mode = MODE_INSERT; ed.cur_line = 0; ed.cur_col = 0;
        editor_handle_key(&ed, EKEY_SHIFT_RIGHT, NULL, 0);
        editor_handle_key(&ed, EKEY_SHIFT_RIGHT, NULL, 0);
        CHECK(editor_has_selection(&ed));
        editor_handle_key(&ed, EKEY_RIGHT, NULL, 0);
        printf("Test 11 (plain arrow collapses selection): sel_active=%d\n", ed.sel_active);
        CHECK(!ed.sel_active);
        buffer_free(b);
    }

    /* Test 12: normal mode 'l' at end of line wraps to the start of the next */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "iab<CR>cd<Esc>");
        ed.cur_line = 0; ed.cur_col = 1; /* on the 'b' */
        feed(&ed, "l");
        printf("Test 12 (normal 'l' wraps to next line): line=%zu col=%zu\n", ed.cur_line, ed.cur_col);
        CHECK(ed.cur_line == 1 && ed.cur_col == 0);
        buffer_free(b);
    }

    /* Test 13: normal mode 'h' at start of line wraps to the end of the previous */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "iab<CR>cd<Esc>");
        ed.cur_line = 1; ed.cur_col = 0; /* on the 'c' */
        feed(&ed, "h");
        printf("Test 13 (normal 'h' wraps to prev line): line=%zu col=%zu\n", ed.cur_line, ed.cur_col);
        CHECK(ed.cur_line == 0 && ed.cur_col == 1); /* lands ON 'b', the last char */
        buffer_free(b);
    }

    /* Test 14: insert mode Right at absolute end of line wraps to col 0 of next */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "iab<CR>cd<Esc>");
        ed.mode = MODE_INSERT; ed.cur_line = 0; ed.cur_col = 2; /* past the 'b' */
        editor_handle_key(&ed, EKEY_RIGHT, NULL, 0);
        printf("Test 14 (insert Right wraps to next line): line=%zu col=%zu\n", ed.cur_line, ed.cur_col);
        CHECK(ed.cur_line == 1 && ed.cur_col == 0);
        buffer_free(b);
    }

    /* Test 15: insert mode Left at col 0 wraps to the true end of the previous line */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "iab<CR>cd<Esc>");
        ed.mode = MODE_INSERT; ed.cur_line = 1; ed.cur_col = 0;
        editor_handle_key(&ed, EKEY_LEFT, NULL, 0);
        printf("Test 15 (insert Left wraps to prev line end): line=%zu col=%zu\n", ed.cur_line, ed.cur_col);
        CHECK(ed.cur_line == 0 && ed.cur_col == 2); /* full end, past the 'b', ready to keep typing */
        buffer_free(b);
    }

    /* Test 16: wrapping doesn't fall off the buffer's edges */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "iab<CR>cd<Esc>");
        ed.cur_line = 0; ed.cur_col = 0; /* very first position */
        feed(&ed, "h"); /* nothing above/before -- should be a no-op */
        CHECK(ed.cur_line == 0 && ed.cur_col == 0);
        ed.cur_line = 1; ed.cur_col = 1; /* on the 'd', last char of last line */
        feed(&ed, "l"); /* nothing below/after -- should be a no-op */
        printf("Test 16 (wrap doesn't overrun buffer edges): line=%zu col=%zu\n", ed.cur_line, ed.cur_col);
        CHECK(ed.cur_line == 1 && ed.cur_col == 1);
        buffer_free(b);
    }

    /* Test 17: 'yy' yanks the current line into the yank register (linewise) */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        feed(&ed, "yy");
        printf("Test 17 ('yy' yanks current line): yank_text=\"%.*s\" yank_dirty=%d\n",
               (int)ed.yank_len, ed.yank_text ? ed.yank_text : "(null)", ed.yank_dirty);
        CHECK(ed.yank_text != NULL);
        CHECK(ed.yank_len == 6 && memcmp(ed.yank_text, "hello\n", 6) == 0);
        CHECK(ed.yank_dirty);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 18: 'p' only sets a request flag -- main.c does the actual X11
     * round trip and calls editor_paste_text(); the buffer shouldn't
     * change just from pressing 'p' in isolation. */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        feed(&ed, "p");
        printf("Test 18 ('p' sets paste_requested): paste_requested=%d\n", ed.paste_requested);
        CHECK(ed.paste_requested);
        CHECK(strcmp(buffer_line(b, 0)->data, "hello") == 0); /* buffer untouched so far */
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 19: editor_paste_text with linewise content (trailing '\n')
     * inserts whole new line(s) below the cursor */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ione<CR>three<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        editor_paste_text(&ed, "two\n", 4);
        printf("Test 19 (linewise paste inserts new line below):\n"); dump(b);
        CHECK(b->count == 3);
        CHECK(strcmp(buffer_line(b, 0)->data, "one") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "two") == 0);
        CHECK(strcmp(buffer_line(b, 2)->data, "three") == 0);
        CHECK(ed.cur_line == 1 && ed.cur_col == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 20: editor_paste_text with characterwise content (no trailing
     * '\n') inserts inline at the cursor */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.mode = MODE_INSERT; ed.cur_line = 0; ed.cur_col = 6; /* right before "world" */
        editor_paste_text(&ed, "brave new ", 10);
        printf("Test 20 (characterwise paste inserts inline):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "hello brave new world") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 21: characterwise paste with an embedded (but not trailing)
     * newline splits the current line */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "iac<Esc>");
        ed.mode = MODE_INSERT; ed.cur_line = 0; ed.cur_col = 1; /* between 'a' and 'c' */
        editor_paste_text(&ed, "X\nY", 3); /* no trailing newline -> characterwise */
        printf("Test 21 (characterwise paste splits on embedded newline):\n"); dump(b);
        CHECK(b->count == 2);
        CHECK(strcmp(buffer_line(b, 0)->data, "aX") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "Yc") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 22: pasting over an active selection replaces it first */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.mode = MODE_INSERT; ed.cur_line = 0; ed.cur_col = 0;
        for (int i = 0; i < 5; i++) editor_handle_key(&ed, EKEY_SHIFT_RIGHT, NULL, 0); /* select "hello" */
        CHECK(editor_has_selection(&ed));
        editor_paste_text(&ed, "goodbye", 7);
        printf("Test 22 (paste replaces active selection):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "goodbye world") == 0);
        CHECK(!editor_has_selection(&ed));
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 23: editor_get_selection_text for a single-line selection */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.mode = MODE_INSERT; ed.cur_line = 0; ed.cur_col = 0;
        for (int i = 0; i < 5; i++) editor_handle_key(&ed, EKEY_SHIFT_RIGHT, NULL, 0); /* select "hello" */
        char *text = NULL; size_t len = 0;
        int ok = editor_get_selection_text(&ed, &text, &len);
        printf("Test 23 (get_selection_text single-line): \"%.*s\"\n", (int)len, text ? text : "");
        CHECK(ok);
        CHECK(len == 5 && memcmp(text, "hello", 5) == 0);
        free(text);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 24: editor_get_selection_text for a multi-line selection joins
     * lines with '\n' */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ione<CR>two<CR>three<Esc>");
        ed.mode = MODE_INSERT; ed.cur_line = 0; ed.cur_col = 1; /* after 'o' in "one" */
        editor_handle_key(&ed, EKEY_SHIFT_DOWN, NULL, 0);
        editor_handle_key(&ed, EKEY_SHIFT_DOWN, NULL, 0); /* extend down to "three" */
        editor_handle_key(&ed, EKEY_SHIFT_RIGHT, NULL, 0);
        editor_handle_key(&ed, EKEY_SHIFT_RIGHT, NULL, 0); /* -> col 3 of "three" ("thr") */
        char *text = NULL; size_t len = 0;
        int ok = editor_get_selection_text(&ed, &text, &len);
        printf("Test 24 (get_selection_text multi-line): \"%.*s\"\n", (int)len, text ? text : "");
        CHECK(ok);
        CHECK(len == 10 && memcmp(text, "ne\ntwo\nthr", 10) == 0);
        free(text);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 25: 'y' followed by anything other than 'y' cancels and is a
     * no-op, same as 'd' followed by a non-'d' key */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "yh"); /* 'y' then 'h' -- not a repeat, should cancel without yanking or moving */
        printf("Test 25 ('yh' cancels without yanking): yank_text=%s cur_col=%zu\n",
               ed.yank_text ? "SET" : "(null)", ed.cur_col);
        CHECK(ed.yank_text == NULL);
        CHECK(ed.cur_col == 0); /* the 'h' was swallowed, not acted on */
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 26: 'x' can be undone with 'u' */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "x"); /* delete 'h' */
        CHECK(strcmp(buffer_line(b, 0)->data, "ello") == 0);
        feed(&ed, "u");
        printf("Test 26 ('x' then 'u' restores the char):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "hello") == 0);
        CHECK(ed.cur_col == 0); /* cursor restored to where it was pre-'x' too */
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 27: an entire insert-mode session undoes as ONE step, not one
     * step per character typed */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>"); /* one 'i'...Esc session */
        CHECK(strcmp(buffer_line(b, 0)->data, "hello world") == 0);
        feed(&ed, "u");
        printf("Test 27 (whole insert session undoes in one step):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "") == 0); /* back to the empty starting line */
        CHECK(ed.undo_count == 0); /* nothing further to undo -- it really was one step */
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 28: 'dd' undo restores the deleted line in place */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ione<CR>two<CR>three<Esc>");
        ed.cur_line = 1; ed.cur_col = 0;
        feed(&ed, "dd"); /* delete "two" */
        CHECK(b->count == 2);
        feed(&ed, "u");
        printf("Test 28 ('dd' then 'u' restores the line):\n"); dump(b);
        CHECK(b->count == 3);
        CHECK(strcmp(buffer_line(b, 0)->data, "one") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "two") == 0);
        CHECK(strcmp(buffer_line(b, 2)->data, "three") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 29: Ctrl+R (EKEY_REDO) redoes an undone change */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "x");
        CHECK(strcmp(buffer_line(b, 0)->data, "ello") == 0);
        feed(&ed, "u");
        CHECK(strcmp(buffer_line(b, 0)->data, "hello") == 0);
        editor_handle_key(&ed, EKEY_REDO, NULL, 0);
        printf("Test 29 (undo then redo):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "ello") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 30: a new edit after an undo clears the redo stack */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "x");        /* delete 'h' -> "ello" */
        feed(&ed, "u");        /* undo -> "hello", redo stack now has 1 entry */
        CHECK(ed.redo_count == 1);
        feed(&ed, "x");        /* a fresh edit: should invalidate that redo */
        printf("Test 30 (new edit clears redo history): redo_count=%zu\n", ed.redo_count);
        CHECK(ed.redo_count == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 31: undo/redo with nothing to undo/redo are safe no-ops -- must
     * use a totally untouched editor, since even entering insert mode
     * ('i') itself checkpoints, so anything that typed text would already
     * have something on the undo stack. */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "u"); /* nothing has ever been checkpointed */
        printf("Test 31 (undo/redo no-ops when stacks are empty):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "") == 0);
        CHECK(ed.undo_count == 0);
        editor_handle_key(&ed, EKEY_REDO, NULL, 0); /* nothing to redo either */
        CHECK(strcmp(buffer_line(b, 0)->data, "") == 0);
        CHECK(ed.redo_count == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 32: paste is undoable as one step, selection-replace included */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.mode = MODE_INSERT; ed.cur_line = 0; ed.cur_col = 0;
        for (int i = 0; i < 5; i++) editor_handle_key(&ed, EKEY_SHIFT_RIGHT, NULL, 0); /* select "hello" */
        editor_paste_text(&ed, "goodbye", 7); /* replaces "hello" -> "goodbye world" */
        CHECK(strcmp(buffer_line(b, 0)->data, "goodbye world") == 0);
        ed.mode = MODE_NORMAL;
        feed(&ed, "u");
        printf("Test 32 (paste-over-selection undoes in one step):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "hello world") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 33: 'v' + movement + 'y' yanks an arbitrary characterwise span
     * from normal mode -- not just a whole line -- and lands the cursor at
     * the start of what was yanked, vim-style */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "v");
        CHECK(ed.mode == MODE_VISUAL);
        CHECK(ed.sel_active);
        feed(&ed, "llll"); /* extend selection to cover "hello" (0..4 inclusive) */
        feed(&ed, "y");
        printf("Test 33 (visual v+llll+y yanks 'hello'): yank=\"%.*s\"\n",
               (int)ed.yank_len, ed.yank_text ? ed.yank_text : "");
        CHECK(ed.mode == MODE_NORMAL);
        CHECK(!ed.sel_active);
        CHECK(ed.yank_text != NULL);
        CHECK(ed.yank_len == 5 && memcmp(ed.yank_text, "hello", 5) == 0);
        CHECK(ed.cur_line == 0 && ed.cur_col == 0); /* cursor at start of what was yanked */
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 34: visual 'd' cuts (deletes AND yanks) the selected span, and
     * is undoable as one step */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.cur_line = 0; ed.cur_col = 6; /* on 'w' of "world" */
        feed(&ed, "v");
        feed(&ed, "llll"); /* extend to cover "world" */
        feed(&ed, "d");
        printf("Test 34 (visual d cuts 'world'):\n"); dump(b);
        CHECK(ed.mode == MODE_NORMAL);
        CHECK(strcmp(buffer_line(b, 0)->data, "hello ") == 0);
        CHECK(ed.yank_text != NULL && ed.yank_len == 5 && memcmp(ed.yank_text, "world", 5) == 0);
        feed(&ed, "u");
        printf("Test 34b (undo restores it):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "hello world") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 35: visual mode spanning multiple lines, deleted with 'x' (the
     * vim-equivalent alias for visual-mode delete) */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ione<CR>two<CR>three<Esc>");
        ed.cur_line = 0; ed.cur_col = 1; /* on the 'n' in "one" (cursor rests ON chars here) */
        feed(&ed, "v");
        feed(&ed, "jj"); /* extend down to line 2 ("three"), preserving column */
        feed(&ed, "x");
        printf("Test 35 (visual multi-line x):\n"); dump(b);
        CHECK(b->count == 1);
        CHECK(strcmp(buffer_line(b, 0)->data, "oree") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 36: Esc cancels visual mode without changing the buffer */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "v");
        feed(&ed, "lll");
        CHECK(ed.sel_active);
        feed(&ed, "<Esc>");
        printf("Test 36 (Esc cancels visual mode, no change):\n"); dump(b);
        CHECK(ed.mode == MODE_NORMAL);
        CHECK(!ed.sel_active);
        CHECK(strcmp(buffer_line(b, 0)->data, "hello") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 37: pasting over a visual selection replaces it (editor_paste_text
     * doesn't care which mode made the selection, only that one is active) */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "v");
        feed(&ed, "llll"); /* select "hello" */
        feed(&ed, "p");    /* sets mode back to NORMAL + paste_requested */
        CHECK(ed.mode == MODE_NORMAL);
        CHECK(ed.paste_requested);
        CHECK(ed.sel_active); /* still active -- editor_paste_text() is what consumes it */
        editor_paste_text(&ed, "goodbye", 7);
        printf("Test 37 (visual p replaces the selection):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "goodbye world") == 0);
        CHECK(!ed.sel_active);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 38: '/' + typing + Enter commits search_pattern and requests a
     * forward search; editor.c itself never touches the buffer for this
     * (main.c is what actually runs grep and moves the cursor) */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        feed(&ed, "/needle");
        CHECK(ed.mode == MODE_SEARCH);
        CHECK(ed.cmdlen == 6 && memcmp(ed.cmdline, "needle", 6) == 0);
        editor_handle_key(&ed, EKEY_RETURN, NULL, 0);
        printf("Test 38 ('/'+pattern+Enter commits search): pattern=\"%s\" requested=%d backward=%d\n",
               ed.search_pattern, ed.search_requested, ed.search_backward);
        CHECK(ed.mode == MODE_NORMAL);
        CHECK(strcmp(ed.search_pattern, "needle") == 0);
        CHECK(ed.search_requested);
        CHECK(!ed.search_backward);
        CHECK(strcmp(buffer_line(b, 0)->data, "hello") == 0); /* unchanged -- editor.c doesn't run grep */
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 39: Esc while typing a search pattern cancels it -- no request,
     * no pattern committed */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "/abc");
        feed(&ed, "<Esc>");
        printf("Test 39 (Esc cancels search): mode=%d requested=%d pattern=\"%s\"\n",
               ed.mode, ed.search_requested, ed.search_pattern);
        CHECK(ed.mode == MODE_NORMAL);
        CHECK(!ed.search_requested);
        CHECK(ed.search_pattern[0] == '\0');
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 40: 'n'/'N' request a repeat search in the right direction, but
     * only once a pattern has actually been committed */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "n"); /* no previous search yet */
        CHECK(!ed.search_requested);
        CHECK(strcmp(ed.status, "E: no previous search") == 0);

        feed(&ed, "/x");
        editor_handle_key(&ed, EKEY_RETURN, NULL, 0);
        ed.search_requested = 0; /* pretend main.c already handled the initial search */

        feed(&ed, "n");
        CHECK(ed.search_requested && !ed.search_backward);
        ed.search_requested = 0;

        feed(&ed, "N");
        printf("Test 40 ('n'/'N' request search in the right direction)\n");
        CHECK(ed.search_requested && ed.search_backward);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 41: editor_yank_selection() is non-destructive -- unlike
     * visual-mode 'y', it leaves the selection, cursor, and mode alone */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "v");
        feed(&ed, "llll"); /* select "hello", still in visual mode */
        editor_yank_selection(&ed);
        printf("Test 41 (editor_yank_selection is non-destructive): mode=%d sel_active=%d\n",
               ed.mode, ed.sel_active);
        CHECK(ed.yank_text != NULL && ed.yank_len == 5 && memcmp(ed.yank_text, "hello", 5) == 0);
        CHECK(ed.mode == MODE_VISUAL);   /* unchanged */
        CHECK(ed.sel_active);            /* unchanged */
        CHECK(ed.cur_line == 0 && ed.cur_col == 4); /* unchanged */
        CHECK(strcmp(buffer_line(b, 0)->data, "hello world") == 0); /* buffer unchanged */
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 42: editor_yank_selection() with no active selection is a no-op */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        editor_yank_selection(&ed);
        printf("Test 42 (editor_yank_selection no-op without a selection): yank_text=%s\n",
               ed.yank_text ? "SET" : "(null)");
        CHECK(ed.yank_text == NULL);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 43: editor_cut_selection() from insert mode (a mouse-drag-style
     * selection, not visual mode) deletes and yanks, but does NOT force
     * normal mode -- insert-mode selections are a normal transient state,
     * unlike visual mode which structurally requires one */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.mode = MODE_INSERT; ed.cur_line = 0; ed.cur_col = 0;
        for (int i = 0; i < 5; i++) editor_handle_key(&ed, EKEY_SHIFT_RIGHT, NULL, 0); /* select "hello" */
        editor_cut_selection(&ed);
        printf("Test 43 (editor_cut_selection from insert mode):\n"); dump(b);
        CHECK(ed.mode == MODE_INSERT); /* stayed in insert mode */
        CHECK(!ed.sel_active);
        CHECK(strcmp(buffer_line(b, 0)->data, " world") == 0);
        CHECK(ed.yank_text != NULL && ed.yank_len == 5 && memcmp(ed.yank_text, "hello", 5) == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 44: editor_cut_selection() from visual mode drops back to
     * normal mode (can't stay in visual with nothing selected), and is
     * undoable as one step */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "v");
        feed(&ed, "llll");
        editor_cut_selection(&ed);
        printf("Test 44 (editor_cut_selection from visual mode):\n"); dump(b);
        CHECK(ed.mode == MODE_NORMAL);
        CHECK(strcmp(buffer_line(b, 0)->data, " world") == 0);
        feed(&ed, "u");
        CHECK(strcmp(buffer_line(b, 0)->data, "hello world") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 45: editor_run_command() replaces the old character-by-character
     * ':' bar -- same parsing, now takes the whole string as an argument
     * (as a dmenu picker would hand it over) instead of reading it from an
     * internally-typed cmdline buffer */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ione<CR>two<CR>three<Esc>");
        editor_run_command(&ed, "w /tmp/xenoed_test2.txt");
        printf("Test 45 (editor_run_command \"w <path>\"): status=\"%s\"\n", ed.status);
        CHECK(strstr(ed.status, "written") != NULL);
        CHECK(b->filename != NULL && strcmp(b->filename, "/tmp/xenoed_test2.txt") == 0);
        CHECK(!b->dirty);

        Buffer *b2 = buffer_new();
        int lrc = buffer_load(b2, "/tmp/xenoed_test2.txt");
        CHECK(lrc == 0 && b2->count == 3);
        CHECK(strcmp(buffer_line(b2, 0)->data, "one") == 0);
        buffer_free(b2);

        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 46: ":q" refuses with unsaved changes; ":q!" quits regardless */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "idirty<Esc>");
        CHECK(b->dirty);

        editor_run_command(&ed, "q");
        printf("Test 46a (\"q\" refuses with unsaved changes): status=\"%s\" want_quit=%d\n",
               ed.status, ed.want_quit);
        CHECK(!ed.want_quit);
        CHECK(strstr(ed.status, "unsaved") != NULL);

        editor_run_command(&ed, "q!");
        printf("Test 46b (\"q!\" quits regardless): want_quit=%d\n", ed.want_quit);
        CHECK(ed.want_quit);

        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 47: an empty command is a silent no-op; an unrecognized one
     * reports an error, matching the old bar's behavior exactly */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);

        editor_run_command(&ed, "");
        CHECK(ed.status[0] == '\0');
        CHECK(!ed.want_quit);

        editor_run_command(&ed, "bogus");
        printf("Test 47 (unknown command reports an error): status=\"%s\"\n", ed.status);
        CHECK(strstr(ed.status, "unknown command") != NULL);

        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 48: editor_command_names() exposes the built-in commands, PLUS
     * whatever config.h's XENOED_COMMANDS table adds (named entries), for a
     * dmenu-style picker to offer */
    {
        int count = 0;
        const char *const *names = editor_command_names(&count);
        int user_named = 0;
        for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
            if (XENOED_COMMANDS[i].name) user_named++;
        }
        printf("Test 48 (editor_command_names): count=%d\n", count);
        CHECK(count == 6 + user_named);
        CHECK(strcmp(names[0], "w") == 0);
        CHECK(strcmp(names[1], "q") == 0);
        CHECK(strcmp(names[2], "q!") == 0);
        CHECK(strcmp(names[3], "wq") == 0);
        CHECK(strcmp(names[4], "x") == 0);
        CHECK(strcmp(names[5], "s/pat/repl/[g]") == 0);
        for (int i = 0, n = 6; XENOED_COMMANDS[i].script != NULL; i++) {
            if (!XENOED_COMMANDS[i].name) continue;
            CHECK(strcmp(names[n], XENOED_COMMANDS[i].name) == 0);
            n++;
        }
    }

    /* Test 49: leader key (SPACE) + a bound key requests the matching
     * user-defined command from config.h's XENOED_COMMANDS */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        /* Find the first command that has a leader keybinding. */
        int idx = -1;
        char key = 0;
        for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
            if (XENOED_COMMANDS[i].key.mod == XENOED_MOD_LEADER) {
                idx = i;
                key = XENOED_COMMANDS[i].key.key;
                break;
            }
        }
        CHECK(idx >= 0);
        char keys[3] = { ' ', key, '\0' };
        feed(&ed, keys);
        printf("Test 49 (leader+key requests a user command): requested=%d index=%d\n",
               ed.user_command_requested, ed.user_command_index);
        CHECK(ed.user_command_requested);
        CHECK(ed.user_command_index == idx);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 50: leader key + an unbound key is a silent no-op, same as any
     * other unrecognized key */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, " z"); /* nothing in config.h binds 'z' */
        printf("Test 50 (leader+unbound key is a no-op): requested=%d\n", ed.user_command_requested);
        CHECK(!ed.user_command_requested);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 51: a CMD_INPUT_SELECTION command triggered from normal mode
     * (where there's never an active selection) reports needing one,
     * rather than silently requesting a doomed subprocess call */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        int idx = -1;
        for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
            if (XENOED_COMMANDS[i].input == CMD_INPUT_SELECTION && XENOED_COMMANDS[i].name) {
                idx = i;
                break;
            }
        }
        CHECK(idx >= 0);
        editor_run_command(&ed, XENOED_COMMANDS[idx].name);
        printf("Test 51 (selection-input command from normal mode refuses): status=\"%s\" requested=%d\n",
               ed.status, ed.user_command_requested);
        CHECK(!ed.user_command_requested);
        CHECK(strstr(ed.status, "needs a selection") != NULL);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 52: the SAME command, triggered from visual mode WITH an active
     * selection, requests it correctly -- this is what makes it read as
     * an operator, like visual-mode y/d/x -- and drops back to normal
     * mode, consistent with how y/d/x already leave visual mode too */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        int idx = -1;
        for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
            if (XENOED_COMMANDS[i].input == CMD_INPUT_SELECTION && XENOED_COMMANDS[i].name) {
                idx = i;
                break;
            }
        }
        CHECK(idx >= 0);
        feed(&ed, "ihello<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "v");
        feed(&ed, "lll"); /* select "hell" */
        editor_run_command(&ed, XENOED_COMMANDS[idx].name);
        printf("Test 52 (selection-input command from visual mode): requested=%d mode=%d\n",
               ed.user_command_requested, ed.mode);
        CHECK(ed.user_command_requested);
        CHECK(ed.user_command_index == idx);
        CHECK(ed.mode == MODE_NORMAL);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 53: editor_run_command() falls back to the user command table
     * by NAME (the ':' picker path), after the built-ins don't match */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        int idx = -1;
        for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
            if (XENOED_COMMANDS[i].name && XENOED_COMMANDS[i].input != CMD_INPUT_SELECTION) {
                idx = i;
                break;
            }
        }
        CHECK(idx >= 0);
        editor_run_command(&ed, XENOED_COMMANDS[idx].name);
        printf("Test 53 (':' picker dispatches a user command by name): requested=%d\n",
               ed.user_command_requested);
        CHECK(ed.user_command_requested);
        CHECK(ed.user_command_index == idx);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 54: a name matching neither a built-in nor a user command still
     * reports "unknown command", same as before this feature existed */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        editor_run_command(&ed, "totally-bogus-name");
        printf("Test 54 (still-unknown command reports an error): status=\"%s\"\n", ed.status);
        CHECK(!ed.user_command_requested);
        CHECK(strstr(ed.status, "unknown command") != NULL);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 55: buffer_set_from_text() replaces the buffer's entire
     * contents, splitting on '\n' the same way buffer_load() does */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        buffer_set_from_text(b, "alpha\nbeta\ngamma\n", 17);
        printf("Test 55 (buffer_set_from_text splits into lines):\n"); dump(b);
        CHECK(b->count == 3);
        CHECK(strcmp(buffer_line(b, 0)->data, "alpha") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "beta") == 0);
        CHECK(strcmp(buffer_line(b, 2)->data, "gamma") == 0);
        CHECK(b->dirty);
        buffer_free(b);
    }

    /* Test 56: editor_replace_buffer_text() -- the whole-buffer counterpart
     * to editor_paste_text() -- replaces everything as one undo step,
     * resets the cursor, and drops out of visual mode (any selection's
     * byte offsets are meaningless in entirely different content) */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ione<CR>two<CR>three<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "v");
        feed(&ed, "ll"); /* an active selection, to confirm it gets cleared */
        editor_replace_buffer_text(&ed, "replaced\ncontent\n", 17);
        printf("Test 56 (editor_replace_buffer_text):\n"); dump(b);
        CHECK(b->count == 2);
        CHECK(strcmp(buffer_line(b, 0)->data, "replaced") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "content") == 0);
        CHECK(ed.cur_line == 0 && ed.cur_col == 0);
        CHECK(ed.mode == MODE_NORMAL);
        CHECK(!ed.sel_active);
        feed(&ed, "u");
        printf("Test 56b (undo restores the original):\n"); dump(b);
        CHECK(b->count == 3);
        CHECK(strcmp(buffer_line(b, 0)->data, "one") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 57: 'V' alone selects the whole current line; yank is linewise
     * (trailing '\n'), matching yy */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.cur_line = 0; ed.cur_col = 3; /* mid-line -- column must not matter */
        feed(&ed, "V");
        CHECK(ed.mode == MODE_VISUAL);
        CHECK(ed.sel_active);
        CHECK(ed.sel_linewise);
        {
            size_t fl, fc, tl, tc;
            editor_selection_range(&ed, &fl, &fc, &tl, &tc);
            CHECK(fl == 0 && fc == 0 && tl == 0 && tc == 11);
        }
        feed(&ed, "y");
        printf("Test 57 (visual V+y yanks whole line with trailing newline): yank=\"%.*s\"\n",
               (int)ed.yank_len, ed.yank_text ? ed.yank_text : "");
        CHECK(ed.mode == MODE_NORMAL);
        CHECK(!ed.sel_active);
        CHECK(ed.yank_text != NULL);
        CHECK(ed.yank_len == 12 && memcmp(ed.yank_text, "hello world\n", 12) == 0);
        CHECK(ed.cur_line == 0 && ed.cur_col == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 58: 'V' + movement + 'd' deletes whole lines (not a mid-line
     * splice), and the yank is linewise */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ione<CR>two<CR>three<Esc>");
        ed.cur_line = 0; ed.cur_col = 1;
        feed(&ed, "Vj"); /* lines 0 and 1 */
        feed(&ed, "d");
        printf("Test 58 (visual Vj+d deletes two whole lines):\n"); dump(b);
        CHECK(ed.mode == MODE_NORMAL);
        CHECK(b->count == 1);
        CHECK(strcmp(buffer_line(b, 0)->data, "three") == 0);
        CHECK(ed.yank_text != NULL && ed.yank_len == 8 &&
              memcmp(ed.yank_text, "one\ntwo\n", 8) == 0);
        CHECK(ed.cur_line == 0 && ed.cur_col == 0);
        feed(&ed, "u");
        printf("Test 58b (undo restores both lines):\n"); dump(b);
        CHECK(b->count == 3);
        CHECK(strcmp(buffer_line(b, 0)->data, "one") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "two") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 59: pasting a linewise yank over a linewise visual selection
     * replaces those lines in-place (not below the line that slid up) */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ione<CR>two<CR>three<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "V");
        feed(&ed, "p");
        CHECK(ed.paste_requested);
        CHECK(ed.sel_active && ed.sel_linewise);
        editor_paste_text(&ed, "NEW\n", 4);
        printf("Test 59 (visual V+p replaces the line in place):\n"); dump(b);
        CHECK(b->count == 3);
        CHECK(strcmp(buffer_line(b, 0)->data, "NEW") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "two") == 0);
        CHECK(strcmp(buffer_line(b, 2)->data, "three") == 0);
        CHECK(!ed.sel_active);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 59b: linewise replace of the last line keeps earlier lines;
     * replacing every line leaves no empty-buffer guard behind */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ione<CR>two<Esc>");
        ed.cur_line = 1; ed.cur_col = 0;
        feed(&ed, "V");
        editor_paste_text(&ed, "LAST\n", 5);
        printf("Test 59b (V+p on last line):\n"); dump(b);
        CHECK(b->count == 2);
        CHECK(strcmp(buffer_line(b, 0)->data, "one") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "LAST") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ione<CR>two<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "Vj");
        editor_paste_text(&ed, "ONLY\n", 5);
        printf("Test 59c (V+p replacing every line):\n"); dump(b);
        CHECK(b->count == 1);
        CHECK(strcmp(buffer_line(b, 0)->data, "ONLY") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 60: switching v <-> V inside visual mode changes whether the
     * selection is characterwise or linewise */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "iabcdef<Esc>");
        ed.cur_line = 0; ed.cur_col = 1; /* on 'b' */
        feed(&ed, "vll"); /* characterwise over "bcd" */
        CHECK(ed.sel_linewise == 0);
        {
            size_t fl, fc, tl, tc;
            editor_selection_range(&ed, &fl, &fc, &tl, &tc);
            CHECK(fl == 0 && fc == 1 && tl == 0 && tc == 4);
        }
        feed(&ed, "V"); /* switch to linewise */
        CHECK(ed.sel_linewise == 1);
        {
            size_t fl, fc, tl, tc;
            editor_selection_range(&ed, &fl, &fc, &tl, &tc);
            CHECK(fl == 0 && fc == 0 && tl == 0 && tc == 6);
        }
        feed(&ed, "v"); /* back to characterwise */
        CHECK(ed.sel_linewise == 0);
        {
            size_t fl, fc, tl, tc;
            editor_selection_range(&ed, &fl, &fc, &tl, &tc);
            CHECK(fl == 0 && fc == 1 && tl == 0 && tc == 4);
        }
        printf("Test 60 (v/V toggle characterwise <-> linewise): ok\n");
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 61: editor_scroll_by moves top_line and keeps the cursor inside
     * the new viewport (so ensure_visible won't undo a wheel scroll) */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        for (int i = 0; i < 20; i++) {
            char line[16];
            int n = snprintf(line, sizeof(line), "L%d", i);
            if (i == 0) line_set(buffer_line(b, 0), line, (size_t)n);
            else buffer_insert_line(b, (size_t)i, line, (size_t)n);
        }
        ed.cur_line = 0;
        ed.cur_col = 0;
        ed.top_line = 0;
        editor_scroll_by(&ed, 5, 10); /* scroll down 5, viewport 10 rows */
        printf("Test 61 (scroll_by down): top=%zu cur=%zu\n", ed.top_line, ed.cur_line);
        CHECK(ed.top_line == 5);
        CHECK(ed.cur_line == 5); /* was above the new viewport; clamped in */
        editor_scroll_by(&ed, -3, 10);
        printf("Test 61b (scroll_by up): top=%zu cur=%zu\n", ed.top_line, ed.cur_line);
        CHECK(ed.top_line == 2);
        CHECK(ed.cur_line == 5); /* still inside [2, 12); unchanged */
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 62: scroll_by clamps at the ends -- can't go above 0, and
     * can't leave an empty region past the last full page */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        for (int i = 0; i < 15; i++) {
            char line[16];
            int n = snprintf(line, sizeof(line), "L%d", i);
            if (i == 0) line_set(buffer_line(b, 0), line, (size_t)n);
            else buffer_insert_line(b, (size_t)i, line, (size_t)n);
        }
        ed.cur_line = 0;
        ed.top_line = 0;
        editor_scroll_by(&ed, -10, 10);
        CHECK(ed.top_line == 0);
        CHECK(ed.cur_line == 0);
        editor_scroll_by(&ed, 100, 10); /* max_top = 15-10 = 5 */
        printf("Test 62 (scroll_by clamps at EOF): top=%zu cur=%zu\n", ed.top_line, ed.cur_line);
        CHECK(ed.top_line == 5);
        CHECK(ed.cur_line == 5);
        /* Short buffer: everything fits, scroll is a no-op on top_line */
        editor_scroll_by(&ed, 3, 20);
        CHECK(ed.top_line == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 63: normal-mode '!' requests a whole-buffer external filter */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "!");
        printf("Test 63 (normal ! requests buffer filter): requested=%d whole=%d\n",
               ed.external_filter_requested, ed.external_filter_whole_buffer);
        CHECK(ed.external_filter_requested);
        CHECK(ed.external_filter_whole_buffer);
        CHECK(ed.mode == MODE_NORMAL);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 64: visual-mode '!' requests a selection filter and keeps the
     * selection active until main.c applies (or cancel/fail) */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "vll");
        feed(&ed, "!");
        printf("Test 64 (visual ! requests selection filter): requested=%d whole=%d mode=%d sel=%d\n",
               ed.external_filter_requested, ed.external_filter_whole_buffer,
               ed.mode, ed.sel_active);
        CHECK(ed.external_filter_requested);
        CHECK(!ed.external_filter_whole_buffer);
        CHECK(ed.mode == MODE_VISUAL);
        CHECK(ed.sel_active);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 65: editor_replace_selection_text replaces the span as one
     * undo step, and empty output deletes the selection */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ipear<CR>apple<CR>orange<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "Vjj"); /* all three lines, linewise */
        CHECK(editor_replace_selection_text(&ed, "apple\norange\npear\n", 18));
        printf("Test 65 (replace_selection_text sorts lines):\n"); dump(b);
        CHECK(ed.mode == MODE_NORMAL);
        CHECK(!ed.sel_active);
        CHECK(b->count == 3);
        CHECK(strcmp(buffer_line(b, 0)->data, "apple") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "orange") == 0);
        CHECK(strcmp(buffer_line(b, 2)->data, "pear") == 0);
        feed(&ed, "u");
        printf("Test 65b (undo restores pre-filter text):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "pear") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "apple") == 0);
        CHECK(strcmp(buffer_line(b, 2)->data, "orange") == 0);

        ed.cur_line = 1; ed.cur_col = 0;
        feed(&ed, "V");
        CHECK(editor_replace_selection_text(&ed, "", 0));
        printf("Test 65c (empty stdout deletes the selection):\n"); dump(b);
        CHECK(b->count == 2);
        CHECK(strcmp(buffer_line(b, 0)->data, "pear") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "orange") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 66: replace_selection_text with no selection is a no-op */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        CHECK(!editor_replace_selection_text(&ed, "x", 1));
        CHECK(strcmp(buffer_line(b, 0)->data, "hello") == 0);
        printf("Test 66 (replace_selection_text no-op without selection): ok\n");
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 67: :s/pat/repl/ replaces the FIRST match on every line */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ifoo<CR>foobar<CR>foo<Esc>");
        editor_run_command(&ed, "s/foo/F/");
        printf("Test 67 (whole-buffer :s first-match-per-line): status=\"%s\"\n", ed.status);
        CHECK(strcmp(ed.status, "3 replacements") == 0);
        CHECK(strcmp(buffer_line(b, 0)->data, "F") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "Fbar") == 0);
        CHECK(strcmp(buffer_line(b, 2)->data, "F") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 68: trailing g replaces EVERY match; one undo step restores */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ifoo foo<CR>foo<Esc>");
        editor_run_command(&ed, "s/foo/bar/g");
        printf("Test 68 (:s ... /g):\n"); dump(b);
        CHECK(strcmp(ed.status, "3 replacements") == 0);
        CHECK(strcmp(buffer_line(b, 0)->data, "bar bar") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "bar") == 0);
        feed(&ed, "u");
        CHECK(strcmp(buffer_line(b, 0)->data, "foo foo") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "foo") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 69: replacement backrefs: '&' whole match, '\N' groups, '\\' */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        editor_run_command(&ed, "s/(h)/[&]/g");
        CHECK(strcmp(ed.status, "1 replacements") == 0);
        CHECK(strcmp(buffer_line(b, 0)->data, "[h]ello") == 0);

        feed(&ed, "u");
        editor_run_command(&ed, "s/(l)(l)/\\1\\2\\2/g");
        CHECK(strcmp(buffer_line(b, 0)->data, "helllo") == 0);

        feed(&ed, "u");
        editor_run_command(&ed, "s/e/\\\\/g");
        CHECK(strcmp(ed.status, "1 replacements") == 0);
        printf("Test 69 (replacement & / \\N / \\\\ escapes): \"%s\"\n", buffer_line(b, 0)->data);
        CHECK(strcmp(buffer_line(b, 0)->data, "h\\llo") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 70: no match reports an error and changes nothing (nor undo) */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        size_t undo_before = ed.undo_count;
        editor_run_command(&ed, "s/xyz/ABC/g");
        printf("Test 70 (no match): status=\"%s\" undo=%zu\n", ed.status, ed.undo_count);
        CHECK(strstr(ed.status, "pattern not found") != NULL);
        CHECK(strcmp(buffer_line(b, 0)->data, "hello") == 0);
        CHECK(ed.undo_count == undo_before);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 71: empty pattern reuses the last /-search pattern */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihex<CR>null<Esc>");
        strncpy(ed.search_pattern, "l", sizeof(ed.search_pattern) - 1);
        editor_run_command(&ed, "s//X/g");
        printf("Test 71 (empty pattern reuses last search): status=\"%s\"\n", ed.status);
        CHECK(strcmp(buffer_line(b, 0)->data, "hex") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "nuXX") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 72: invalid regex and malformed substitution are reported */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        editor_run_command(&ed, "s/(/x/");
        printf("Test 72a (invalid regex): status=\"%s\"\n", ed.status);
        CHECK(strstr(ed.status, "invalid") != NULL);
        CHECK(strcmp(buffer_line(b, 0)->data, "hello") == 0);

        editor_run_command(&ed, "s/foo/bar/bogus");
        printf("Test 72b (bad substitution syntax): status=\"%s\"\n", ed.status);
        CHECK(strstr(ed.status, "substitution") != NULL);

        editor_run_command(&ed, "s/hello//");
        CHECK(strcmp(buffer_line(b, 0)->data, "") == 0);
        printf("Test 72c (empty replacement deletes matches): ok\n");
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 73: visual-mode :s only touches the selection, in one undo step */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello big world<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "vllll"); /* selects "hello" -- h e l l o */
        CHECK(ed.mode == MODE_VISUAL);
        editor_run_command(&ed, "s/l/L/g");
        printf("Test 73 (visual :s on selection):\n"); dump(b);
        CHECK(strcmp(ed.status, "2 replacements") == 0);
        CHECK(strcmp(buffer_line(b, 0)->data, "heLLo big world") == 0);
        CHECK(ed.mode == MODE_NORMAL);
        feed(&ed, "u");
        CHECK(strcmp(buffer_line(b, 0)->data, "hello big world") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 74: '%' prefix forces whole-buffer scope even in visual mode */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ifoo bar<CR>baz foo<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "vll"); /* selects "foo" */
        editor_run_command(&ed, "%s/foo/F/g");
        printf("Test 74 (%%s forces whole buffer from visual):\n"); dump(b);
        CHECK(strcmp(ed.status, "2 replacements") == 0);
        CHECK(strcmp(buffer_line(b, 0)->data, "F bar") == 0);
        CHECK(strcmp(buffer_line(b, 1)->data, "baz F") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 75: ':' opens the command menu from visual mode too */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "v");
        feed(&ed, ":");
        printf("Test 75 (visual : requests command menu): req=%d mode=%d\n",
               ed.command_menu_requested, ed.mode);
        CHECK(ed.command_menu_requested);
        CHECK(ed.mode == MODE_VISUAL);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 76: a plain-letter binding (XENOED_KEY_PLAIN) fires from normal
     * mode with no leader key involved */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        int idx = -1;
        char key = 0;
        for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
            if (XENOED_COMMANDS[i].key.mod == XENOED_MOD_PLAIN) {
                idx = i;
                key = XENOED_COMMANDS[i].key.key;
                break;
            }
        }
        CHECK(idx >= 0);
        char keys[2] = { key, '\0' };
        feed(&ed, keys);
        printf("Test 76 (plain key requests a user command): requested=%d index=%d\n",
               ed.user_command_requested, ed.user_command_index);
        CHECK(ed.user_command_requested);
        CHECK(ed.user_command_index == idx);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 77: a Ctrl binding (XENOED_KEY_CTRL) fires when the matching
     * ASCII control character (Ctrl+T = 0x14) arrives -- the representation
     * main.c synthesizes for Ctrl+letter -- and, since the shipped ctrl
     * example is CMD_INPUT_SELECTION, entering it from visual mode with a
     * live selection requests it and drops back to normal mode */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        int idx = -1;
        char key = 0;
        for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
            if (XENOED_COMMANDS[i].key.mod == XENOED_MOD_CTRL) {
                idx = i;
                key = XENOED_COMMANDS[i].key.key;
                break;
            }
        }
        CHECK(idx >= 0);
        feed(&ed, "ihello<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "vll"); /* select "hel" */
        CHECK(ed.mode == MODE_VISUAL);
        char ctrlkeys[2] = { (char)(key - 'a' + 1), '\0' };
        feed(&ed, ctrlkeys);
        printf("Test 77 (Ctrl+key requests a user command): requested=%d index=%d mode=%d\n",
               ed.user_command_requested, ed.user_command_index, ed.mode);
        CHECK(ed.user_command_requested);
        CHECK(ed.user_command_index == idx);
        CHECK(ed.mode == MODE_NORMAL);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 78: leader-pending consumes the next key BEFORE plain binding
     * dispatch, so "leader + a plain-bound key" is a no-op (SPACE+'e'
     * isn't bound; only bare 'e' is) rather than firing the plain command */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        int plain_idx = -1;
        for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
            if (XENOED_COMMANDS[i].key.mod == XENOED_MOD_PLAIN) {
                plain_idx = i;
                break;
            }
        }
        CHECK(plain_idx >= 0);
        int leader_idx = -1;
        for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
            if (XENOED_COMMANDS[i].key.mod == XENOED_MOD_LEADER) {
                leader_idx = i;
                break;
            }
        }
        CHECK(leader_idx >= 0);
        char seq[3] = { ' ', XENOED_COMMANDS[plain_idx].key.key, '\0' };
        feed(&ed, seq);
        printf("Test 78 (leader+plain-bound key is a no-op): requested=%d\n",
               ed.user_command_requested);
        CHECK(!ed.user_command_requested);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 79: an unbound Ctrl combination is a silent no-op, same as any
     * other unrecognized key (Ctrl+U = 0x15 has no XENOED_KEY_CTRL('u')) */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        char ctrlkeys[2] = { 0x15, '\0' };
        feed(&ed, ctrlkeys);
        printf("Test 79 (unbound Ctrl key is a no-op): requested=%d\n",
               ed.user_command_requested);
        CHECK(!ed.user_command_requested);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 80: editor_get_word_text returns the maximal run of
     * non-whitespace around the cursor -- the word under it */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.cur_line = 0; ed.cur_col = 6; /* on the 'w' of "world" */
        char *w = NULL; size_t wlen = 0;
        CHECK(editor_get_word_text(&ed, &w, &wlen));
        printf("Test 80 (word under cursor on 'w'): \"%.*s\"\n", (int)wlen, w ? w : "");
        CHECK(wlen == 5 && memcmp(w, "world", 5) == 0);
        free(w);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 81: cursor on whitespace spans to the nearest word -- left
     * word when spaces separate it, right word when leading whitespace
     * (or an empty left side) is all there is, trailing word at EOL */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "i  alpha beta <Esc>"); /* "  alpha beta " (len 13) */
        char *w = NULL; size_t wlen = 0;

        ed.cur_line = 0; ed.cur_col = 4; /* in "alpha" */
        CHECK(editor_get_word_text(&ed, &w, &wlen));
        CHECK(wlen == 5 && memcmp(w, "alpha", 5) == 0);
        free(w); w = NULL;

        ed.cur_col = 7; /* the space right after "alpha" */
        CHECK(editor_get_word_text(&ed, &w, &wlen));
        CHECK(wlen == 5 && memcmp(w, "alpha", 5) == 0); /* prefers left */
        free(w); w = NULL;

        ed.cur_col = 1; /* leading whitespace, nothing to the left */
        CHECK(editor_get_word_text(&ed, &w, &wlen));
        CHECK(wlen == 5 && memcmp(w, "alpha", 5) == 0); /* falls to the right */
        free(w); w = NULL;

        ed.cur_col = 13; /* trailing space after "beta" (== len) */
        CHECK(editor_get_word_text(&ed, &w, &wlen));
        CHECK(wlen == 4 && memcmp(w, "beta", 4) == 0); /* left word */
        free(w); w = NULL;

        printf("Test 81 (whitespace/edge word span): ok\n");
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 82: no word on an empty or all-whitespace line */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        char *w = NULL; size_t wlen = 0;
        CHECK(!editor_get_word_text(&ed, &w, &wlen)); /* empty buffer line */
        feed(&ed, "i   <Esc>");                       /* whitespace only */
        CHECK(!editor_get_word_text(&ed, &w, &wlen));
        CHECK(w == NULL); /* untouched on failure */
        printf("Test 82 (no word on empty/blank line): ok\n");
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 83: editor_replace_word_text swaps the word in place, as one
     * undo step, and an empty replacement deletes it */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello world<Esc>");
        ed.cur_line = 0; ed.cur_col = 6; /* on 'w' of "world" */
        CHECK(editor_replace_word_text(&ed, "there", 5));
        printf("Test 83 (replace_word_text):\n"); dump(b);
        CHECK(strcmp(buffer_line(b, 0)->data, "hello there") == 0);
        CHECK(ed.cur_col == 6); /* cursor on the replacement's first char */
        feed(&ed, "u");
        CHECK(strcmp(buffer_line(b, 0)->data, "hello world") == 0);

        ed.cur_col = 6;
        CHECK(editor_replace_word_text(&ed, "", 0)); /* empty -> deletes */
        CHECK(strcmp(buffer_line(b, 0)->data, "hello ") == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 84: replace_word_text with no word is a no-op */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        CHECK(!editor_replace_word_text(&ed, "x", 1));
        printf("Test 84 (replace_word_text no-op without a word): ok\n");
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 85: 'SPACE' then the leader-bound CMD_INPUT_WORD key requests it
     * from normal mode -- the direct-keybinding dispatch the feature is
     * built around */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        int idx = -1;
        char key = 0;
        for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
            if (XENOED_COMMANDS[i].input == CMD_INPUT_WORD &&
                XENOED_COMMANDS[i].key.mod == XENOED_MOD_LEADER) {
                idx = i;
                key = XENOED_COMMANDS[i].key.key;
                break;
            }
        }
        CHECK(idx >= 0);
        feed(&ed, "ihello<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        char keys[3] = { ' ', key, '\0' };
        feed(&ed, keys);
        printf("Test 85 (leader+word-command key requests it): requested=%d index=%d\n",
               ed.user_command_requested, ed.user_command_index);
        CHECK(ed.user_command_requested);
        CHECK(ed.user_command_index == idx);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 86: a CMD_INPUT_WORD command from visual mode (where the cursor
     * extends a selection rather than a word) is refused, and from a blank
     * line it reports "no word"; from normal mode with a word it requests. */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        int idx = -1;
        for (int i = 0; XENOED_COMMANDS[i].script != NULL; i++) {
            if (XENOED_COMMANDS[i].input == CMD_INPUT_WORD && XENOED_COMMANDS[i].name) {
                idx = i;
                break;
            }
        }
        CHECK(idx >= 0);
        const char *name = XENOED_COMMANDS[idx].name;

        feed(&ed, "ihello<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "vll"); /* visual mode with a live selection */
        editor_run_command(&ed, name);
        printf("Test 86a (word command refused in visual mode): status=\"%s\" requested=%d mode=%d\n",
               ed.status, ed.user_command_requested, ed.mode);
        CHECK(!ed.user_command_requested);
        CHECK(ed.mode == MODE_VISUAL); /* selection kept, like the :s guard */
        CHECK(strstr(ed.status, "normal-mode") != NULL);

        Buffer *b2 = buffer_new();
        buffer_load(b2, NULL);
        Editor ed2; editor_init(&ed2, b2);
        editor_run_command(&ed2, name); /* blank line, no word */
        printf("Test 86b (word command refused on blank line): status=\"%s\" requested=%d\n",
               ed2.status, ed2.user_command_requested);
        CHECK(!ed2.user_command_requested);
        CHECK(strstr(ed2.status, "no word") != NULL);

        feed(&ed2, "iword<Esc>");
        ed2.cur_line = 0; ed2.cur_col = 0;
        editor_run_command(&ed2, name);
        printf("Test 86c (word command from normal mode requests): requested=%d index=%d\n",
               ed2.user_command_requested, ed2.user_command_index);
        CHECK(ed2.user_command_requested);
        CHECK(ed2.user_command_index == idx);
        editor_deinit(&ed2);
        buffer_free(b2);

        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 87: 'm'+letter sets a mark at the cursor; '''+letter jumps back
     * to that line at column 0, vim-style */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ialpha<CR>beta<CR>gamma<Esc>");
        ed.cur_line = 2; ed.cur_col = 0; /* on "gamma" */
        feed(&ed, "ma");                 /* mark 'a' at line 2 */
        CHECK(ed.marks_line['a' - 'a'] == 2);
        CHECK(ed.marks_col['a' - 'a'] == 0);
        ed.cur_line = 0; ed.cur_col = 3; /* wander back up */
        feed(&ed, "'a");
        printf("Test 87 (mark set + jump): line=%zu col=%zu\n", ed.cur_line, ed.cur_col);
        CHECK(ed.cur_line == 2);
        CHECK(ed.cur_col == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 88: a mark stores the column where it was set, but ''' still
     * lands on column 0 of the marked line (vim behavior) */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ihello<Esc>");
        ed.cur_line = 0; ed.cur_col = 4; /* on the 'o' */
        feed(&ed, "ma");
        CHECK(ed.marks_line[0] == 0 && ed.marks_col[0] == 4);
        ed.cur_col = 1;
        feed(&ed, "'a");
        printf("Test 88 (mark col stored, jump goes to col 0): col=%zu\n", ed.cur_col);
        CHECK(ed.cur_line == 0 && ed.cur_col == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    /* Test 89: jumping to an unset mark reports an error and doesn't move */
    {
        Buffer *b = buffer_new();
        buffer_load(b, NULL);
        Editor ed; editor_init(&ed, b);
        feed(&ed, "ione<CR>two<Esc>");
        ed.cur_line = 0; ed.cur_col = 0;
        feed(&ed, "'z");
        printf("Test 89 (jump to unset mark): status=\"%s\"\n", ed.status);
        CHECK(ed.marks_line['z' - 'a'] == (size_t)-1);
        CHECK(strstr(ed.status, "not set") != NULL);
        CHECK(ed.cur_line == 0 && ed.cur_col == 0);
        editor_deinit(&ed);
        buffer_free(b);
    }

    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        printf("\n%d check(s) FAILED.\n", failures);
        return 1;
    }
}
