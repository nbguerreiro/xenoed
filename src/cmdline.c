#include "cmdline.h"
#include <stdlib.h>
#include <string.h>

#define MAX_CMDLINE_ARGS 64

/* Growable word buffer. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Word;

static int word_append(Word *w, char c) {
    if (w->len + 2 > w->cap) { /* +2: room for the byte and the NUL */
        size_t ncap = w->cap ? w->cap * 2 : 64;
        char *nd = realloc(w->data, ncap);
        if (!nd) return -1;
        w->data = nd;
        w->cap = ncap;
    }
    w->data[w->len++] = c;
    return 0;
}

/* Appends `word` (ownership transfers on success) to *argv, growing the
 * array as needed and keeping it NULL-terminated. Returns 0 on success, -1
 * on allocation failure or when the token limit MAX_CMDLINE_ARGS is hit. */
static int argv_push(char ***argv, int *argc, int *cap, char *word) {
    if (*argc >= MAX_CMDLINE_ARGS) return -1;
    if (*argc + 2 > *cap) {
        int ncap = *cap ? *cap * 2 : 8;
        char **na = realloc(*argv, sizeof(char *) * (size_t)ncap);
        if (!na) return -1;
        *argv = na;
        *cap = ncap;
    }
    (*argv)[(*argc)++] = word;
    (*argv)[*argc] = NULL;
    return 0;
}

int xenoed_split_cmdline(const char *cmdline, char ***out_argv) {
    char **argv = NULL;
    int argc = 0, cap = 0;
    Word w = { NULL, 0, 0 };
    const char *p = cmdline;
    int quote = 0;
    int result = -1;

    *out_argv = NULL;

    while (*p != '\0') {
        if (quote != 0) {
            char c = *p;
            if (c == quote) {
                quote = 0;
                p++;
                continue;
            }
            if (c == '\\' && quote == '"') {
                p++;
                if (*p == '\0') goto done;
                c = *p++;
                if (word_append(&w, c) != 0) goto done;
                continue;
            }
            if (word_append(&w, c) != 0) goto done;
            p++;
            continue;
        }

        if (*p == '\'' || *p == '"') {
            quote = *p++;
            continue;
        }
        if (*p == '\\') {
            p++;
            if (*p == '\0') goto done;
            if (word_append(&w, *p++) != 0) goto done;
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            if (w.data) {
                w.data[w.len] = '\0';
                if (argv_push(&argv, &argc, &cap, w.data) != 0) goto done;
                w.data = NULL;
                w.len = 0;
                w.cap = 0;
            }
            p++;
            continue;
        }
        if (word_append(&w, *p++) != 0) goto done;
    }

    if (quote != 0) goto done;

    if (w.data) {
        w.data[w.len] = '\0';
        if (argv_push(&argv, &argc, &cap, w.data) != 0) goto done;
        w.data = NULL;
    }

    result = argc;
    *out_argv = argv;

done:
    if (result < 0) {
        xenoed_free_argv(argv);
        free(w.data);
    } else {
        free(w.data);
    }
    return result;
}

void xenoed_free_argv(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}