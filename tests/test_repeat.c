#include "../src/buffer.h"
#include "../src/editor.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static void key(Editor *ed, char c) {
    editor_handle_key(ed, EKEY_NONE, &c, 1);
}

static void text(Editor *ed, const char *s) {
    editor_handle_key(ed, EKEY_NONE, s, (int)strlen(s));
}

static void esc(Editor *ed) {
    editor_handle_key(ed, EKEY_ESCAPE, NULL, 0);
}

static void test_insert_repeat(void) {
    Buffer *b = buffer_new();
    buffer_load(b, NULL);
    Editor ed;
    editor_init(&ed, b);

    key(&ed, 'i');
    text(&ed, "hello");
    esc(&ed);
    CHECK(strcmp(buffer_line(b, 0)->data, "hello") == 0);

    ed.cur_col = 0;
    key(&ed, '.');
    CHECK(strcmp(buffer_line(b, 0)->data, "hellohello") == 0);
    CHECK(ed.mode == MODE_NORMAL);

    editor_deinit(&ed);
    buffer_free(b);
}

static void test_dd_repeat(void) {
    Buffer *b = buffer_new();
    buffer_load(b, NULL);
    Editor ed;
    editor_init(&ed, b);

    key(&ed, 'i'); text(&ed, "one"); esc(&ed);
    key(&ed, 'o'); text(&ed, "two"); esc(&ed);
    key(&ed, 'o'); text(&ed, "three"); esc(&ed);

    ed.cur_line = 1;
    ed.cur_col = 0;
    key(&ed, 'd');
    key(&ed, 'd');
    CHECK(b->count == 2);
    CHECK(strcmp(buffer_line(b, 0)->data, "one") == 0);
    CHECK(strcmp(buffer_line(b, 1)->data, "three") == 0);

    key(&ed, '.');
    CHECK(b->count == 1);
    CHECK(strcmp(buffer_line(b, 0)->data, "one") == 0);

    editor_deinit(&ed);
    buffer_free(b);
}

static void test_no_repeat_initially(void) {
    Buffer *b = buffer_new();
    buffer_load(b, NULL);
    Editor ed;
    editor_init(&ed, b);

    key(&ed, '.');
    CHECK(b->count == 1);
    CHECK(strcmp(buffer_line(b, 0)->data, "") == 0);
    CHECK(ed.mode == MODE_NORMAL);

    editor_deinit(&ed);
    buffer_free(b);
}

static void test_repeat_survives_movement(void) {
    Buffer *b = buffer_new();
    buffer_load(b, NULL);
    Editor ed;
    editor_init(&ed, b);

    key(&ed, 'i'); text(&ed, "X"); esc(&ed);
    key(&ed, 'o'); text(&ed, "abc"); esc(&ed);
    ed.cur_line = 0;
    ed.cur_col = 0;
    key(&ed, 'j');
    key(&ed, '.');

    CHECK(b->count == 3);
    CHECK(strcmp(buffer_line(b, 0)->data, "X") == 0);
    CHECK(strcmp(buffer_line(b, 1)->data, "abc") == 0);
    CHECK(strcmp(buffer_line(b, 2)->data, "abc") == 0);
    CHECK(ed.mode == MODE_NORMAL);

    editor_deinit(&ed);
    buffer_free(b);
}

int main(void) {
    test_insert_repeat();
    test_dd_repeat();
    test_no_repeat_initially();
    test_repeat_survives_movement();

    if (failures == 0) {
        printf("repeat-command tests: all passed\n");
    }
    return failures ? 1 : 0;
}
