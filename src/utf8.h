#ifndef XENOED_UTF8_H
#define XENOED_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Minimal UTF-8 helpers. We only need boundary stepping and codepoint
 * counting; we never need to decode to full codepoints for editing logic
 * since Pango handles shaping/rendering from raw UTF-8 bytes directly. */

static inline int utf8_is_cont(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

/* Number of bytes in the UTF-8 sequence starting with lead byte c. */
static inline int utf8_seq_len(unsigned char c) {
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1; /* invalid lead byte: treat as single byte to avoid getting stuck */
}

/* Given byte offset i into s (length len), return the byte offset of the
 * previous codepoint boundary. If i == 0, returns 0. */
static inline size_t utf8_prev_boundary(const char *s, size_t i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && utf8_is_cont((unsigned char)s[i])) i--;
    return i;
}

/* Given byte offset i into s (length len), return the byte offset of the
 * next codepoint boundary. If i >= len, returns len. */
static inline size_t utf8_next_boundary(const char *s, size_t len, size_t i) {
    if (i >= len) return len;
    int n = utf8_seq_len((unsigned char)s[i]);
    size_t j = i + (size_t)n;
    if (j > len) j = len;
    return j;
}

/* Count codepoints (UTF-8 lead bytes) in s[0..len). */
static inline size_t utf8_count(const char *s, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (!utf8_is_cont((unsigned char)s[i])) n++;
    }
    return n;
}

/* Return the byte offset of the boundary that is `count` codepoints into
 * s[0..len), clamped to len. */
static inline size_t utf8_offset_for_count(const char *s, size_t len, size_t count) {
    size_t i = 0, n = 0;
    while (i < len && n < count) {
        i = utf8_next_boundary(s, len, i);
        n++;
    }
    return i;
}

#endif
