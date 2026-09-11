#include "editor.h"
#include <stdlib.h>
#include <string.h>

/* Linker wrapper around editor_handle_key keeps the repeat implementation
 * independent of the editor's command dispatcher. Only one Editor exists
 * in xenoed, and wrapping editor_init gives tests a clean repeat register
 * whenever they create a fresh editor. */

typedef struct {
    EditorSpecialKey special;
    char *text;
    int len;
} RepeatEvent;

static RepeatEvent *events;
static size_t event_count;
static size_t event_cap;
static Editor *event_editor;
static int recording;
static int recording_changed;
static int replaying;

extern void __real_editor_handle_key(Editor *, EditorSpecialKey, const char *, int);
extern void __real_editor_init(Editor *, Buffer *);

static void clear_events(void) {
    for (size_t i = 0; i < event_count; i++) free(events[i].text);
    free(events);
    events = NULL;
    event_count = 0;
    event_cap = 0;
}

static void begin_recording(Editor *ed) {
    clear_events();
    event_editor = ed;
    recording = 1;
    recording_changed = 0;
}

static void record_event(EditorSpecialKey special, const char *text, int len) {
    if (!recording) return;
    if (event_count == event_cap) {
        size_t new_cap = event_cap ? event_cap * 2 : 16;
        RepeatEvent *new_events = realloc(events, new_cap * sizeof(*new_events));
        if (!new_events) abort();
        events = new_events;
        event_cap = new_cap;
    }

    RepeatEvent *ev = &events[event_count++];
    ev->special = special;
    ev->len = len;
    ev->text = NULL;
    if (len > 0) {
        ev->text = malloc((size_t)len);
        if (!ev->text) abort();
        memcpy(ev->text, text, (size_t)len);
    }
}

static int is_insert_edit(EditorSpecialKey special, const char *text, int len) {
    if (special == EKEY_RETURN || special == EKEY_BACKSPACE || special == EKEY_DELETE)
        return 1;
    if (special != EKEY_NONE || len < 1 || !text) return 0;

    unsigned char c0 = (unsigned char)text[0];
    return c0 == 0x18 || (c0 >= 0x20) || c0 == '\t';
}

static void finish_recording(void) {
    if (!recording_changed || event_count == 0) {
        clear_events();
        event_editor = NULL;
    }
    recording = 0;
    recording_changed = 0;
}

static void replay(Editor *ed) {
    if (event_count == 0 || event_editor != ed || replaying) return;

    replaying = 1;
    size_t count = event_count;
    for (size_t i = 0; i < count; i++) {
        __real_editor_handle_key(ed, events[i].special, events[i].text, events[i].len);
    }
    replaying = 0;
}

void __wrap_editor_init(Editor *ed, Buffer *buf) {
    clear_events();
    event_editor = NULL;
    recording = 0;
    recording_changed = 0;
    replaying = 0;
    __real_editor_init(ed, buf);
}

void __wrap_editor_handle_key(Editor *ed, EditorSpecialKey special,
                               const char *text, int text_len) {
    if (replaying) {
        __real_editor_handle_key(ed, special, text, text_len);
        return;
    }

    if (special == EKEY_ESCAPE && ed->mode == MODE_INSERT) {
        record_event(special, text, text_len);
        finish_recording();
        __real_editor_handle_key(ed, special, text, text_len);
        return;
    }

    if (ed->mode == MODE_INSERT && recording && event_editor == ed) {
        record_event(special, text, text_len);
        if (is_insert_edit(special, text, text_len)) recording_changed = 1;
        __real_editor_handle_key(ed, special, text, text_len);
        return;
    }

    if (ed->mode == MODE_NORMAL && special == EKEY_NONE && text_len == 1 && text) {
        char c = text[0];

        if (c == '.' && ed->pending_op == 0 && !ed->leader_pending) {
            replay(ed);
            return;
        }

        if (c == 'i' || c == 'a' || c == 'A' || c == 'I' || c == 'o' || c == 'O') {
            begin_recording(ed);
            record_event(EKEY_NONE, text, text_len);
            __real_editor_handle_key(ed, special, text, text_len);
            return;
        }

        if (c == 'x') {
            __real_editor_handle_key(ed, special, text, text_len);
            clear_events();
            event_editor = ed;
            recording = 1;
            record_event(EKEY_NONE, text, text_len);
            recording = 0;
            recording_changed = 0;
            return;
        }

        if (c == 'd' && ed->pending_op == 'd') {
            __real_editor_handle_key(ed, special, text, text_len);
            clear_events();
            event_editor = ed;
            recording = 1;
            record_event(EKEY_NONE, "d", 1);
            record_event(EKEY_NONE, "d", 1);
            recording = 0;
            recording_changed = 0;
            return;
        }
    }

    __real_editor_handle_key(ed, special, text, text_len);
}
