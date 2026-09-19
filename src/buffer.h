#ifndef XENOED_BUFFER_H
#define XENOED_BUFFER_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

typedef struct {
    char *data;   /* UTF-8 bytes, NOT NUL-terminated guaranteed but we keep
                     a NUL after len for convenience/safety with C string
                     APIs (Pango wants a length anyway). */
    size_t len;
    size_t cap;
} Line;

typedef struct {
    Line *lines;
    size_t count;
    size_t cap;
    char *filename; /* may be NULL */
    int dirty;
    /* On-disk fingerprint of filename, captured by buffer_load() and
     * refreshed by buffer_save(), so buffer_disk_changed() can detect an
     * external program editing (or deleting, or creating) the file between
     * our own reads/writes. disk_known is 0 until the first successful
     * stat(): a load that found no such file keeps it 0, so a file that
     * appears later still counts as a change. */
    int disk_known;
    time_t disk_mtime;
    off_t disk_size;
} Buffer;

Buffer *buffer_new(void);
void buffer_free(Buffer *b);

/* Load a file into the buffer. Returns 0 on success. If the file does not
 * exist, initializes an empty single-line buffer and remembers the path
 * for later saving (returns 0). Returns -1 only on a real I/O error. */
int buffer_load(Buffer *b, const char *path);

/* Replaces the buffer's entire contents with `text` (split into lines on
 * '\n', same CRLF-stripping rule buffer_load uses for files), leaving
 * b->filename untouched. Used for applying an external command's stdout
 * to the whole buffer -- unlike buffer_load, this never touches disk. */
void buffer_set_from_text(Buffer *b, const char *text, size_t len);
int buffer_save(Buffer *b, const char *path); /* path may be NULL -> use b->filename */

/* True when the file on disk no longer matches the state buffer_load() or
 * buffer_save() last recorded: someone else modified it (mtime or size
 * moved), deleted it, or -- for a path that didn't exist at load -- created
 * it. Always false when there is no filename. This is what lets the status
 * bar show a "changed on disk" hint next to the [+]. */
int buffer_disk_changed(const Buffer *b);

Line *buffer_line(Buffer *b, size_t idx);

/* Insert a new line at idx with the given bytes (may be len 0). */
void buffer_insert_line(Buffer *b, size_t idx, const char *text, size_t len);
void buffer_remove_line(Buffer *b, size_t idx);

void line_insert_bytes(Line *l, size_t byte_off, const char *bytes, size_t n);
void line_delete_bytes(Line *l, size_t byte_off, size_t n);
void line_set(Line *l, const char *bytes, size_t n);
void line_free(Line *l);

#endif
