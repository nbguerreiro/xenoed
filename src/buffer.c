#define _POSIX_C_SOURCE 200809L
#include "buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void line_ensure_cap(Line *l, size_t need) {
    if (need + 1 <= l->cap) return; /* +1 for NUL */
    size_t newcap = l->cap ? l->cap * 2 : 32;
    while (newcap < need + 1) newcap *= 2;
    l->data = realloc(l->data, newcap);
    l->cap = newcap;
}

void line_set(Line *l, const char *bytes, size_t n) {
    line_ensure_cap(l, n);
    memcpy(l->data, bytes, n);
    l->data[n] = '\0';
    l->len = n;
}

void line_free(Line *l) {
    free(l->data);
    l->data = NULL;
    l->len = l->cap = 0;
}

void line_insert_bytes(Line *l, size_t byte_off, const char *bytes, size_t n) {
    if (byte_off > l->len) byte_off = l->len;
    line_ensure_cap(l, l->len + n);
    memmove(l->data + byte_off + n, l->data + byte_off, l->len - byte_off);
    memcpy(l->data + byte_off, bytes, n);
    l->len += n;
    l->data[l->len] = '\0';
}

void line_delete_bytes(Line *l, size_t byte_off, size_t n) {
    if (byte_off >= l->len) return;
    if (byte_off + n > l->len) n = l->len - byte_off;
    memmove(l->data + byte_off, l->data + byte_off + n, l->len - byte_off - n);
    l->len -= n;
    l->data[l->len] = '\0';
}

static void buffer_ensure_cap(Buffer *b, size_t need) {
    if (need <= b->cap) return;
    size_t newcap = b->cap ? b->cap * 2 : 16;
    while (newcap < need) newcap *= 2;
    b->lines = realloc(b->lines, newcap * sizeof(Line));
    b->cap = newcap;
}

Buffer *buffer_new(void) {
    Buffer *b = calloc(1, sizeof(Buffer));
    buffer_ensure_cap(b, 1);
    b->lines[0] = (Line){0};
    line_set(&b->lines[0], "", 0);
    b->count = 1;
    return b;
}

void buffer_free(Buffer *b) {
    if (!b) return;
    for (size_t i = 0; i < b->count; i++) line_free(&b->lines[i]);
    free(b->lines);
    free(b->filename);
    free(b);
}

Line *buffer_line(Buffer *b, size_t idx) {
    if (idx >= b->count) return NULL;
    return &b->lines[idx];
}

void buffer_insert_line(Buffer *b, size_t idx, const char *text, size_t len) {
    if (idx > b->count) idx = b->count;
    buffer_ensure_cap(b, b->count + 1);
    memmove(&b->lines[idx + 1], &b->lines[idx], (b->count - idx) * sizeof(Line));
    b->lines[idx] = (Line){0};
    line_set(&b->lines[idx], text, len);
    b->count++;
}

void buffer_remove_line(Buffer *b, size_t idx) {
    if (idx >= b->count) return;
    line_free(&b->lines[idx]);
    memmove(&b->lines[idx], &b->lines[idx + 1], (b->count - idx - 1) * sizeof(Line));
    b->count--;
}

/* Splits raw bytes into lines (on '\n', stripping a trailing '\r' for CRLF
 * input) and replaces b's entire contents with them. Shared by buffer_load
 * (reading a file) and buffer_set_from_text (replacing the buffer with an
 * external command's stdout) -- same splitting rules either way, so this
 * stays the one place that logic lives rather than two copies of it. */
static void buffer_split_into_lines(Buffer *b, const char *text, size_t len) {
    for (size_t i = 0; i < b->count; i++) line_free(&b->lines[i]);
    b->count = 0;

    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            size_t line_len = i - start;
            if (line_len > 0 && text[start + line_len - 1] == '\r') line_len--;
            buffer_insert_line(b, b->count, text + start, line_len);
            start = i + 1;
        }
    }
    if (start < len || len == 0) {
        buffer_insert_line(b, b->count, text + start, len - start);
    }
    if (b->count == 0) buffer_insert_line(b, 0, "", 0);
}

void buffer_set_from_text(Buffer *b, const char *text, size_t len) {
    buffer_split_into_lines(b, text, len);
    b->dirty = 1;
}

int buffer_load(Buffer *b, const char *path) {
    free(b->filename);
    b->filename = path ? strdup(path) : NULL;
    b->dirty = 0;

    if (!path) {
        buffer_split_into_lines(b, "", 0);
        return 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        /* New file: start with one empty line. */
        buffer_split_into_lines(b, "", 0);
        return 0;
    }

    char chunk[4096];
    char *acc = NULL;
    size_t acc_len = 0, acc_cap = 0;
    size_t nread;
    while ((nread = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (acc_len + nread > acc_cap) {
            acc_cap = (acc_len + nread) * 2 + 64;
            acc = realloc(acc, acc_cap);
        }
        memcpy(acc + acc_len, chunk, nread);
        acc_len += nread;
    }
    fclose(f);

    buffer_split_into_lines(b, acc, acc_len);

    free(acc);
    return 0;
}

int buffer_save(Buffer *b, const char *path) {
    const char *target = path ? path : b->filename;
    if (!target) return -1;

    FILE *f = fopen(target, "wb");
    if (!f) return -1;

    for (size_t i = 0; i < b->count; i++) {
        fwrite(b->lines[i].data, 1, b->lines[i].len, f);
        fputc('\n', f);
    }
    fclose(f);

    if (path && path != b->filename) {
        free(b->filename);
        b->filename = strdup(path);
    }
    b->dirty = 0;
    return 0;
}
