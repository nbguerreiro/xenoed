#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <cairo/cairo-xlib.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "buffer.h"
#include "cmdhist.h"
#include "cmdline.h"
#include "config.h"
#include "editor.h"
#include "render.h"

typedef struct {
    Display *dpy;
    Window win;
    Visual *visual;
    int depth;
    GC gc;
    Pixmap pixmap;
    cairo_surface_t *surface;
    int width, height;
} Backbuffer;

static void backbuffer_create(Backbuffer *bb, int width, int height) {
    bb->pixmap = XCreatePixmap(bb->dpy, bb->win, (unsigned)width, (unsigned)height, (unsigned)bb->depth);
    bb->surface = cairo_xlib_surface_create(bb->dpy, bb->pixmap, bb->visual, width, height);
    XSetWindowBackgroundPixmap(bb->dpy, bb->win, bb->pixmap);
    bb->width = width;
    bb->height = height;
}

static void backbuffer_destroy(Backbuffer *bb) {
    cairo_surface_destroy(bb->surface);
    XFreePixmap(bb->dpy, bb->pixmap);
}

static void backbuffer_resize(Backbuffer *bb, int width, int height) {
    if (width == bb->width && height == bb->height) return;
    cairo_surface_destroy(bb->surface);
    Pixmap old_pixmap = bb->pixmap;
    backbuffer_create(bb, width, height);
    XFreePixmap(bb->dpy, old_pixmap);
}

static void backbuffer_present(Backbuffer *bb) {
    XCopyArea(bb->dpy, bb->pixmap, bb->win, bb->gc, 0, 0,
              (unsigned)bb->width, (unsigned)bb->height, 0, 0);
    XFlush(bb->dpy);
}

static void redraw(RenderState *rs, Backbuffer *bb, Editor *ed, int width, int height, int focused) {
    render_frame(rs, bb->surface, ed, width, height, focused);
    cairo_surface_flush(bb->surface);
    backbuffer_present(bb);
}

typedef struct {
    Atom clipboard;
    Atom utf8_string;
    Atom targets;
    Atom paste_prop;
    int primary_owned;
} X11Selections;

static void sync_primary_ownership(Display *dpy, Window win, const Editor *ed, X11Selections *sel) {
    int has_sel = editor_has_selection(ed);
    if (has_sel && !sel->primary_owned) {
        XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
        sel->primary_owned = 1;
    } else if (!has_sel && sel->primary_owned) {
        XSetSelectionOwner(dpy, XA_PRIMARY, None, CurrentTime);
        sel->primary_owned = 0;
    }
}

static void handle_selection_request(Display *dpy, Editor *ed, X11Selections *sel,
                                      XSelectionRequestEvent *req) {
    XSelectionEvent resp;
    resp.type = SelectionNotify;
    resp.display = req->display;
    resp.requestor = req->requestor;
    resp.selection = req->selection;
    resp.target = req->target;
    resp.time = req->time;
    resp.property = None;

    Atom prop = (req->property != None) ? req->property : req->target;

    if (req->target == sel->targets) {
        Atom offered[3] = { sel->targets, sel->utf8_string, XA_STRING };
        XChangeProperty(dpy, req->requestor, prop, XA_ATOM, 32, PropModeReplace,
                         (unsigned char *)offered, 3);
        resp.property = prop;
    } else if (req->target == sel->utf8_string || req->target == XA_STRING) {
        char *data = NULL;
        size_t dlen = 0;
        int have = 0;
        int must_free = 0;

        if (req->selection == XA_PRIMARY) {
            have = editor_get_selection_text(ed, &data, &dlen);
            must_free = have;
        } else if (req->selection == sel->clipboard) {
            if (ed->yank_text) {
                data = ed->yank_text;
                dlen = ed->yank_len;
                have = 1;
            }
        }

        if (have) {
            XChangeProperty(dpy, req->requestor, prop, req->target, 8, PropModeReplace,
                             (unsigned char *)data, (int)dlen);
            resp.property = prop;
        }
        if (must_free) free(data);
    }

    XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&resp);
}

static int fetch_selection_sync(Display *dpy, Window win, Editor *ed, Atom selection,
                                 X11Selections *sel, char **out_data, size_t *out_len) {
    XConvertSelection(dpy, selection, sel->utf8_string, sel->paste_prop, win, CurrentTime);
    XFlush(dpy);

    XEvent ev;
    for (int i = 0; i < 250; i++) {
        while (XCheckTypedWindowEvent(dpy, win, SelectionRequest, &ev)) {
            handle_selection_request(dpy, ed, sel, &ev.xselectionrequest);
        }
        XFlush(dpy);

        if (XCheckTypedWindowEvent(dpy, win, SelectionNotify, &ev)) {
            if (ev.xselection.property == None) return 0;

            Atom type;
            int format;
            unsigned long nitems, bytes_after;
            unsigned char *data = NULL;
            XGetWindowProperty(dpy, win, sel->paste_prop, 0, LONG_MAX / 4, False,
                                AnyPropertyType, &type, &format, &nitems, &bytes_after, &data);
            if (!data) return 0;

            *out_data = malloc(nitems + 1);
            if (!*out_data) {
                XFree(data);
                XDeleteProperty(dpy, win, sel->paste_prop);
                return 0;
            }
            memcpy(*out_data, data, nitems);
            (*out_data)[nitems] = '\0';
            *out_len = nitems;

            XFree(data);
            XDeleteProperty(dpy, win, sel->paste_prop);
            return 1;
        }
        struct timespec poll_delay = {0, 2 * 1000 * 1000};
        nanosleep(&poll_delay, NULL);
    }
    return 0;
}

typedef struct {
    size_t line;
    size_t col;
    size_t global;
} SearchMatch;

static void perform_search(Editor *ed) {
    if (ed->search_pattern[0] == '\0') return;
    Buffer *b = ed->buf;

    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s/xenoed-search-XXXXXX", tmpdir);

    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        snprintf(ed->status, sizeof(ed->status), "E: search: %s", strerror(errno));
        return;
    }

    size_t *line_start = malloc(b->count * sizeof(size_t));
    if(line_start == NULL){ _exit(-1); };

    size_t total = 0;
    FILE *tmp_f = fdopen(tmp_fd, "w");
    if(tmp_f == NULL){ _exit(-1); };

    for (size_t i = 0; i < b->count; i++) {
        const Line *l = buffer_line(b, i);
        line_start[i] = total;
        fwrite(l->data, 1, l->len, tmp_f);
        fputc('\n', tmp_f);
        total += l->len + 1;
    }
    fclose(tmp_f);

    int outpipe[2];
    if (pipe(outpipe) != 0) {
        snprintf(ed->status, sizeof(ed->status), "E: search: %s", strerror(errno));
        unlink(tmp_path);
        free(line_start);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(ed->status, sizeof(ed->status), "E: search: %s", strerror(errno));
        close(outpipe[0]);
        close(outpipe[1]);
        unlink(tmp_path);
        free(line_start);
        return;
    }

    if (pid == 0) {
        dup2(outpipe[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(outpipe[0]);
        close(outpipe[1]);
        execlp("grep", "grep", "-n", "-o", "-b", "-h", "-E", "-i", "--", ed->search_pattern, tmp_path,
               (char *)NULL);
        _exit(127);
    }

    close(outpipe[1]);
    char *out = NULL;
    size_t outlen = 0, outcap = 0;
    char chunk[4096];
    ssize_t n;
    while ((n = read(outpipe[0], chunk, sizeof(chunk))) > 0) {
        if (outlen + (size_t)n > outcap) {
            outcap = (outlen + (size_t)n) * 2 + 64;
            char *new_out = realloc(out, outcap);
            if (!new_out) {
                free(out);
                close(outpipe[0]);
                waitpid(pid, NULL, 0);
                unlink(tmp_path);
                free(line_start);
                return;
            }
            out = new_out;
        }
        memcpy(out + outlen, chunk, (size_t)n);
        outlen += (size_t)n;
    }
    close(outpipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    unlink(tmp_path);

    if (!WIFEXITED(status) || WEXITSTATUS(status) == 127) {
        snprintf(ed->status, sizeof(ed->status), "E: grep not found in $PATH");
        free(out);
        free(line_start);
        return;
    }
    if (WEXITSTATUS(status) >= 2) {
        snprintf(ed->status, sizeof(ed->status), "E: invalid search pattern");
        free(out);
        free(line_start);
        return;
    }

    SearchMatch *matches = malloc(sizeof(SearchMatch));
    if(matches == NULL){ _exit(-1); };

    size_t match_count = 0, match_cap = 0;
    size_t li = 0;
    char *p = out, *end = out + outlen;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char *c1 = memchr(p, ':', linelen);
        if (c1) {
            const char *c2 = memchr(c1 + 1, ':', (size_t)((p + linelen) - (c1 + 1)));
            if (c2) {
                long byte_off = strtol(c1 + 1, NULL, 10);
                if (byte_off >= 0) {
                    while (li + 1 < b->count && line_start[li + 1] <= (size_t)byte_off) li++;
                    if (match_count + 1 > match_cap) {
                        match_cap = match_cap ? match_cap * 2 : 16;
                        SearchMatch *new_matches = realloc(matches, match_cap * sizeof(SearchMatch));
                        if (!new_matches) {
                            free(matches);
                            free(out);
                            free(line_start);
                            return;
                        }
                        matches = new_matches;
                    }
                    matches[match_count].line = li;
                    matches[match_count].col = (size_t)byte_off - line_start[li];
                    matches[match_count].global = (size_t)byte_off;
                    match_count++;
                }
            }
        }
        p = nl ? nl + 1 : end;
    }
    free(out);

    if (match_count == 0) {
        snprintf(ed->status, sizeof(ed->status), "E: pattern not found: %.60s", ed->search_pattern);
        free(matches);
        free(line_start);
        return;
    }

    size_t cursor_global = line_start[ed->cur_line] + ed->cur_col;
    free(line_start);

    long best = -1;
    int wrapped = 0;
    if (!ed->search_backward) {
        for (size_t i = 0; i < match_count; i++) {
            if (matches[i].global > cursor_global) { best = (long)i; break; }
        }
        if (best < 0) { best = 0; wrapped = 1; }
    } else {
        for (long i = (long)match_count - 1; i >= 0; i--) {
            if (matches[i].global < cursor_global) { best = i; break; }
        }
        if (best < 0) { best = (long)match_count - 1; wrapped = 1; }
    }

    ed->cur_line = matches[best].line;
    ed->cur_col = matches[best].col;
    editor_clamp_cursor(ed);

    if (wrapped && match_count > 1) {
        snprintf(ed->status, sizeof(ed->status),
                 ed->search_backward ? "search hit TOP, continuing at BOTTOM (%zu matches)"
                                      : "search hit BOTTOM, continuing at TOP (%zu matches)",
                 match_count);
    } else {
        snprintf(ed->status, sizeof(ed->status), "match %ld/%zu", best + 1, match_count);
    }

    free(matches);
}

/* Split `script` into an execvp-ready argv (NULL-terminated). A bare program
 * name (a single word) gets `filename` injected at argv[1], preserving the
 * old `execlp(script, script, filename)` contract for whole scripts. A
 * multi-word one-liner like "perl -pe '$_ = lc'" is executed exactly as
 * written -- the filename is NOT appended -- so stdin filters keep reading
 * the piped buffer/selection instead of accidentally opening the file.
 * Returns the argv on success (caller frees with xenoed_free_argv), or NULL
 * on a malformed command line or allocation failure. */
static char **build_exec_argv(const char *script, const char *filename) {
    char **argv;
    int argc = xenoed_split_cmdline(script, &argv);
    if (argc <= 0) return NULL;

    if (argc == 1) {
        char **na = realloc(argv, 3 * sizeof(char *));
        if (!na) { xenoed_free_argv(argv); return NULL; }
        argv = na;
        argv[2] = NULL;
        argv[1] = strdup(filename ? filename : "");
        if (!argv[1]) { xenoed_free_argv(argv); return NULL; }
    }
    return argv;
}

static void run_detached(const char *script, const char *filename) {
    char **argv = build_exec_argv(script, filename);
    if (!argv) {
        fprintf(stderr, "xenoed: bad external command: %s\n", script);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) { xenoed_free_argv(argv); return; }

    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 == 0) {
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
            }
            execvp(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    xenoed_free_argv(argv);
}

static char *run_filter(const char *script, const char *filename,
                         const char *input, size_t input_len, size_t *out_len) {
    char **argv = build_exec_argv(script, filename);
    if (!argv) return NULL;

    int inpipe[2], outpipe[2];
    if (pipe(inpipe) != 0) { xenoed_free_argv(argv); return NULL; }
    if (pipe(outpipe) != 0) {
        close(inpipe[0]); close(inpipe[1]);
        xenoed_free_argv(argv);
        return NULL;
    }

    pid_t script_pid = fork();
    if (script_pid < 0) {
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        xenoed_free_argv(argv);
        return NULL;
    }
    if (script_pid == 0) {
        dup2(inpipe[0], STDIN_FILENO);
        dup2(outpipe[1], STDOUT_FILENO);
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    pid_t writer_pid = fork();
    if (writer_pid == 0) {
        close(inpipe[0]);
        close(outpipe[0]); close(outpipe[1]);
        size_t written = 0;
        while (written < input_len) {
            ssize_t n = write(inpipe[1], input + written, input_len - written);
            if (n <= 0) break;
            written += (size_t)n;
        }
        close(inpipe[1]);
        _exit(0);
    }

    close(inpipe[0]);
    close(inpipe[1]);
    close(outpipe[1]);

    char *out = NULL;
    size_t outlen = 0, outcap = 0;
    char chunk[4096];
    ssize_t n;
    while ((n = read(outpipe[0], chunk, sizeof(chunk))) > 0) {
        if (outlen + (size_t)n > outcap) {
            outcap = (outlen + (size_t)n) * 2 + 64;
            char *new_out = realloc(out, outcap);
            if (!new_out) {
                free(out);
                close(outpipe[0]);
                waitpid(script_pid, NULL, 0);
                if (writer_pid > 0) waitpid(writer_pid, NULL, 0);
                xenoed_free_argv(argv);
                return NULL;
            }
            out = new_out;
        }
        memcpy(out + outlen, chunk, (size_t)n);
        outlen += (size_t)n;
    }
    close(outpipe[0]);

    int status = 0;
    waitpid(script_pid, &status, 0);
    if (writer_pid > 0) waitpid(writer_pid, NULL, 0);
    xenoed_free_argv(argv);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(out);
        return NULL;
    }

    *out_len = outlen;
    if (!out) out = malloc(1);
    return out;
}

/* Like run_filter, but runs `command` via /bin/sh -c so dmenu-entered
 * filters can use shell syntax (pipes, quotes, flags). Success with empty
 * stdout returns a non-NULL buffer with *out_len == 0; failure returns NULL. */
static char *run_shell_filter(const char *command,
                              const char *input, size_t input_len,
                              size_t *out_len) {
    int inpipe[2], outpipe[2];
    if (pipe(inpipe) != 0) return NULL;
    if (pipe(outpipe) != 0) { close(inpipe[0]); close(inpipe[1]); return NULL; }

    pid_t script_pid = fork();
    if (script_pid < 0) {
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        return NULL;
    }
    if (script_pid == 0) {
        dup2(inpipe[0], STDIN_FILENO);
        dup2(outpipe[1], STDOUT_FILENO);
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    pid_t writer_pid = fork();
    if (writer_pid == 0) {
        close(inpipe[0]);
        close(outpipe[0]); close(outpipe[1]);
        size_t written = 0;
        while (written < input_len) {
            ssize_t n = write(inpipe[1], input + written, input_len - written);
            if (n <= 0) break;
            written += (size_t)n;
        }
        close(inpipe[1]);
        _exit(0);
    }

    close(inpipe[0]);
    close(inpipe[1]);
    close(outpipe[1]);

    char *out = NULL;
    size_t outlen = 0, outcap = 0;
    char chunk[4096];
    ssize_t n;
    while ((n = read(outpipe[0], chunk, sizeof(chunk))) > 0) {
        if (outlen + (size_t)n > outcap) {
            outcap = (outlen + (size_t)n) * 2 + 64;
            char *new_out = realloc(out, outcap);
            if (!new_out) {
                free(out);
                close(outpipe[0]);
                waitpid(script_pid, NULL, 0);
                if (writer_pid > 0) waitpid(writer_pid, NULL, 0);
                return NULL;
            }
            out = new_out;
        }
        memcpy(out + outlen, chunk, (size_t)n);
        outlen += (size_t)n;
    }
    close(outpipe[0]);

    int status = 0;
    waitpid(script_pid, &status, 0);
    if (writer_pid > 0) waitpid(writer_pid, NULL, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(out);
        return NULL;
    }

    *out_len = outlen;
    if (!out) out = malloc(1);
    return out;
}

static void run_user_command(Editor *ed, const char *filename) {
    int idx = ed->user_command_index;
    if (idx < 0) return;
    const XenoedCommand *cmd = &XENOED_COMMANDS[idx];
    const char *fname = filename ? filename : "";
    const char *label = cmd->name ? cmd->name : cmd->script;

    if (cmd->input == CMD_INPUT_NONE) {
        run_detached(cmd->script, fname);
        snprintf(ed->status, sizeof(ed->status), "%s: launched", label);
        return;
    }

    char *input_text = NULL;
    size_t input_len = 0;

    if (cmd->input == CMD_INPUT_BUFFER) {
        Buffer *b = ed->buf;
        size_t total = 0;
        for (size_t i = 0; i < b->count; i++) total += buffer_line(b, i)->len + 1;
        input_text = malloc(total ? total : 1);
        if (!input_text) {
            snprintf(ed->status, sizeof(ed->status), "E: %s failed", label);
            return;
        }
        size_t off = 0;
        for (size_t i = 0; i < b->count; i++) {
            const Line *l = buffer_line(b, i);
            memcpy(input_text + off, l->data, l->len);
            off += l->len;
            input_text[off++] = '\n';
        }
        input_len = total;
    } else if (cmd->input == CMD_INPUT_WORD) {
        if (!editor_get_word_text(ed, &input_text, &input_len)) {
            snprintf(ed->status, sizeof(ed->status), "E: no word under cursor");
            return;
        }
    } else if (cmd->input == CMD_INPUT_INSERT) {
        /* No stdin: the script is a producer (date, a snippet generator),
         * not a filter -- its empty stdin is closed and it just writes
         * stdout, which editor_insert_at_cursor() drops at the cursor. */
        input_text = NULL;
        input_len = 0;
    } else {
        if (!editor_get_selection_text(ed, &input_text, &input_len)) {
            snprintf(ed->status, sizeof(ed->status), "E: %s needs a selection", label);
            return;
        }
    }

    size_t output_len = 0;
    char *output = run_filter(cmd->script, fname, input_text, input_len, &output_len);
    free(input_text);

    if (!output) {
        snprintf(ed->status, sizeof(ed->status), "E: %s failed", label);
        return;
    }

    if (cmd->input == CMD_INPUT_BUFFER) {
        editor_replace_buffer_text(ed, output, output_len);
    } else if (cmd->input == CMD_INPUT_WORD) {
        editor_replace_word_text(ed, output, output_len);
    } else if (cmd->input == CMD_INPUT_INSERT) {
        /* trailing newline(s) are the shell's doing (`date`, `echo`,
         * `cat` all emit one); drop them so the output sits inline at the
         * cursor and the command doesn't drag a dangling blank line in */
        while (output_len > 0 &&
               (output[output_len - 1] == '\n' || output[output_len - 1] == '\r'))
            output_len--;
        editor_insert_at_cursor(ed, output, output_len);
    } else {
        editor_paste_text(ed, output, output_len);
    }
    free(output);

    snprintf(ed->status, sizeof(ed->status), "%s: done", label);
}

static void process_editor_side_effects(Display *dpy, Window win, Editor *ed, X11Selections *sel) {
    if (ed->yank_dirty) {
        XSetSelectionOwner(dpy, sel->clipboard, win, CurrentTime);
        ed->yank_dirty = 0;
    }
    if (ed->paste_requested) {
        ed->paste_requested = 0;
        char *data = NULL;
        size_t dlen = 0;
        if (fetch_selection_sync(dpy, win, ed, sel->clipboard, sel, &data, &dlen)) {
            editor_paste_text(ed, data, dlen);
            free(data);
        } else {
            snprintf(ed->status, sizeof(ed->status), "(nothing to paste)");
        }
    }
}

static char *run_dmenu(Display *dpy, Window win, const char *const *items, int n_items,
                        const char *prompt) {
    char win_str[32];
    snprintf(win_str, sizeof(win_str), "%lu", (unsigned long)win);

    int inpipe[2], outpipe[2];
    if (pipe(inpipe) != 0) return NULL;
    if (pipe(outpipe) != 0) { close(inpipe[0]); close(inpipe[1]); return NULL; }

    pid_t pid = fork();
    if (pid < 0) {
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        return NULL;
    }

    if (pid == 0) {
        dup2(inpipe[0], STDIN_FILENO);
        dup2(outpipe[1], STDOUT_FILENO);
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        execl("/bin/sh", "sh", "-c", "${DMENU:-dmenu} -w \"$1\" -p \"$2\"",
              "sh", win_str, prompt, (char *)NULL);
        _exit(127);
    }

    close(inpipe[0]);
    close(outpipe[1]);
    for (int i = 0; i < n_items; i++) {
        if (write(inpipe[1], items[i], strlen(items[i])) < 0) break;
        if (write(inpipe[1], "\n", 1) < 0) break;
    }
    close(inpipe[1]);

    char *out = NULL;
    size_t outlen = 0, outcap = 0;
    char chunk[256];
    ssize_t n;
    while ((n = read(outpipe[0], chunk, sizeof(chunk))) > 0) {
        if (outlen + (size_t)n > outcap) {
            outcap = (outlen + (size_t)n) * 2 + 64;
            char *new_out = realloc(out, outcap);
            if (!new_out) {
                free(out);
                close(outpipe[0]);
                waitpid(pid, NULL, 0);
                XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
                return NULL;
            }
            out = new_out;
        }
        memcpy(out + outlen, chunk, (size_t)n);
        outlen += (size_t)n;
    }
    close(outpipe[0]);

    waitpid(pid, NULL, 0);
    XSetInputFocus(dpy, win, RevertToParent, CurrentTime);

    if (!out || outlen == 0) {
        free(out);
        return NULL;
    }
    if (out[outlen - 1] == '\n') outlen--;
    char *shrunk = realloc(out, outlen + 1);
    if (!shrunk) {
        free(out);
        return NULL;
    }
    out = shrunk;
    out[outlen] = '\0';
    return out;
}

static void show_context_menu(Display *dpy, Window win, Editor *ed) {
    int has_sel = editor_has_selection(ed);

    const char *items[3];
    int n_items = 0;
    if (has_sel) {
        items[n_items++] = "Copy";
        items[n_items++] = "Cut";
    }
    items[n_items++] = "Paste";

    char *choice = run_dmenu(dpy, win, items, n_items, "action:");
    if (!choice) return;

    if (strcmp(choice, "Copy") == 0) {
        editor_yank_selection(ed);
    } else if (strcmp(choice, "Cut") == 0) {
        editor_cut_selection(ed);
    } else if (strcmp(choice, "Paste") == 0) {
        ed->paste_requested = 1;
    }
    free(choice);
}

static void show_command_menu(Display *dpy, Window win, Editor *ed) {
    CmdHist hist;
    cmdhist_init(&hist);
    char *hist_path = cmdhist_path("colon_hist");
    if (hist_path) cmdhist_load(&hist, hist_path);

    int catalog_n = 0;
    const char *const *catalog = editor_command_names(&catalog_n);

    /* Most-used remembered commands float to the top; catalog entries
     * that aren't already in the history follow in their usual order. */
    int cap = hist.n + catalog_n;
    const char **items = NULL;
    int n = 0;
    if (cap > 0) {
        items = malloc((size_t)cap * sizeof(*items));
        if (items) {
            for (int i = 0; i < hist.n; i++) items[n++] = hist.entries[i].cmd;
            for (int i = 0; i < catalog_n; i++) {
                int found = 0;
                for (int j = 0; j < hist.n; j++) {
                    if (strcmp(catalog[i], hist.entries[j].cmd) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found) items[n++] = catalog[i];
            }
        }
    }

    char *choice = run_dmenu(dpy, win, items, n, ":");
    free(items);
    if (choice) {
        editor_run_command(ed, choice);
        cmdhist_bump(&hist, choice);
        if (hist_path) cmdhist_save(&hist, hist_path);
        free(choice);
    }
    free(hist_path);
    cmdhist_free(&hist);
}

static void run_external_filter(Display *dpy, Window win, Editor *ed) {
    int whole_buffer = ed->external_filter_whole_buffer;

    if (!whole_buffer && !editor_has_selection(ed)) {
        snprintf(ed->status, sizeof(ed->status), "E: ! needs a selection");
        return;
    }

    CmdHist hist;
    cmdhist_init(&hist);
    char *hist_path = cmdhist_path("bang_hist");
    if (hist_path) cmdhist_load(&hist, hist_path);

    const char **items = NULL;
    int n = 0;
    if (hist.n > 0) {
        items = malloc((size_t)hist.n * sizeof(*items));
        if (items) {
            for (int i = 0; i < hist.n; i++) items[n++] = hist.entries[i].cmd;
        }
    }

    char *command = run_dmenu(dpy, win, items, n, "!");
    free(items);
    if (!command) {
        snprintf(ed->status, sizeof(ed->status), "Filter cancelled");
        free(hist_path);
        cmdhist_free(&hist);
        return;
    }
    if (command[0] == '\0') {
        snprintf(ed->status, sizeof(ed->status), "E: no external command entered");
        free(command);
        free(hist_path);
        cmdhist_free(&hist);
        return;
    }

    char *input_text = NULL;
    size_t input_len = 0;

    if (whole_buffer) {
        Buffer *b = ed->buf;
        size_t total = 0;
        for (size_t i = 0; i < b->count; i++) total += buffer_line(b, i)->len + 1;
        input_text = malloc(total ? total : 1);
        if (!input_text) {
            snprintf(ed->status, sizeof(ed->status), "E: filter failed");
            free(command);
            free(hist_path);
            cmdhist_free(&hist);
            return;
        }
        size_t off = 0;
        for (size_t i = 0; i < b->count; i++) {
            const Line *l = buffer_line(b, i);
            memcpy(input_text + off, l->data, l->len);
            off += l->len;
            input_text[off++] = '\n';
        }
        input_len = total;
    } else {
        if (!editor_get_selection_text(ed, &input_text, &input_len)) {
            snprintf(ed->status, sizeof(ed->status), "E: unable to read selection");
            free(command);
            free(hist_path);
            cmdhist_free(&hist);
            return;
        }
    }

    size_t output_len = 0;
    char *output = run_shell_filter(command, input_text, input_len, &output_len);
    free(input_text);

    if (!output) {
        snprintf(ed->status, sizeof(ed->status), "E: filter failed");
        free(command);
        free(hist_path);
        cmdhist_free(&hist);
        return;
    }

    if (whole_buffer) {
        editor_replace_buffer_text(ed, output, output_len);
    } else {
        editor_replace_selection_text(ed, output, output_len);
    }
    free(output);

    /* Only successful filters are remembered, so failed experiments
     * don't pollute the bang menu. */
    cmdhist_bump(&hist, command);
    if (hist_path) cmdhist_save(&hist, hist_path);
    free(command);
    free(hist_path);
    cmdhist_free(&hist);
    snprintf(ed->status, sizeof(ed->status), "Filter applied");
}

static EditorSpecialKey classify_keysym(KeySym ks, unsigned int state) {
    int shift = (state & ShiftMask) != 0;
    int ctrl = (state & ControlMask) != 0;
    switch (ks) {
        case XK_Escape:    return EKEY_ESCAPE;
        case XK_BackSpace: return EKEY_BACKSPACE;
        case XK_Delete:    return EKEY_DELETE;
        case XK_Return:
        case XK_KP_Enter:  return EKEY_RETURN;
        case XK_Left:      return shift ? EKEY_SHIFT_LEFT  : EKEY_LEFT;
        case XK_Right:     return shift ? EKEY_SHIFT_RIGHT : EKEY_RIGHT;
        case XK_Up:        return shift ? EKEY_SHIFT_UP    : EKEY_UP;
        case XK_Down:      return shift ? EKEY_SHIFT_DOWN  : EKEY_DOWN;
        case XK_Next:      return EKEY_NEXT;   /* Page Down */
        case XK_Prior:     return EKEY_PRIOR;  /* Page Up */
        case XK_r:
        case XK_R:         return ctrl ? EKEY_REDO : EKEY_NONE;
        default:           return EKEY_NONE;
    }
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : NULL;

    long requested_line = 0;
    if (argc > 2) {
        char *end = NULL;
        long v = strtol(argv[2], &end, 10);
        if (end != argv[2] && *end == '\0' && v > 0) {
            requested_line = v;
        } else {
            fprintf(stderr, "xenoed: ignoring invalid line number '%s'\n", argv[2]);
        }
    }

    setlocale(LC_ALL, "");
    if (!XSupportsLocale()) {
        fprintf(stderr, "xenoed: X locale support missing; UTF-8 input may misbehave.\n");
    }
    XSetLocaleModifiers("");

    Buffer *buf = buffer_new();
    if (buffer_load(buf, path) != 0) {
        fprintf(stderr, "xenoed: failed to load '%s'\n", path ? path : "(none)");
        return 1;
    }

    Editor ed;
    editor_init(&ed, buf);

    if (requested_line > 0) {
        size_t target = (size_t)(requested_line - 1);
        if (target >= buf->count) target = buf->count - 1;
        ed.cur_line = target;
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "xenoed: cannot open X display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Visual *visual = DefaultVisual(dpy, screen);
    int depth = DefaultDepth(dpy, screen);

    int width = 900, height = 600;
    Window win = XCreateSimpleWindow(dpy, root, 0, 0, (unsigned)width, (unsigned)height, 0,
                                      BlackPixel(dpy, screen), BlackPixel(dpy, screen));

    /* External commands (config.h's XENOED_COMMANDS) and the '!' filter
     * inherit this, so a script can find the editor's own X window the
     * same way most X tooling does -- e.g. to embed a dmenu with
     * `dmenu -w "$WINDOWID"` in the editor window. Set once; every fork/
     * exec below inherits it. */
    {
        char win_str[32];
        snprintf(win_str, sizeof(win_str), "%lu", (unsigned long)win);
        setenv("WINDOWID", win_str, 1);
    }

    XStoreName(dpy, win, path ? path : "xenoed");

    XClassHint *class_hint = XAllocClassHint();
    if (class_hint) {
        class_hint->res_name = (char *)"xenoed";
        class_hint->res_class = (char *)"Xenoed";
        XSetClassHint(dpy, win, class_hint);
        XFree(class_hint);
    }

    XSelectInput(dpy, win,
                 KeyPressMask | ExposureMask | StructureNotifyMask |
                 ButtonPressMask | ButtonReleaseMask | Button1MotionMask |
                 FocusChangeMask);

    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    XIM xim = XOpenIM(dpy, NULL, NULL, NULL);
    XIC xic = NULL;
    if (xim) {
        xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                         XNClientWindow, win, XNFocusWindow, win, NULL);
    }
    if (!xic) {
        fprintf(stderr, "xenoed: warning: no input context; falling back to plain XLookupString.\n");
    }

    X11Selections sel = {0};
    sel.clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    sel.utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    sel.targets = XInternAtom(dpy, "TARGETS", False);
    sel.paste_prop = XInternAtom(dpy, "XENOED_PASTE_TARGET", False);

    Backbuffer bb = {0};
    bb.dpy = dpy;
    bb.win = win;
    bb.visual = visual;
    bb.depth = depth;
    bb.gc = XCreateGC(dpy, win, 0, NULL);
    backbuffer_create(&bb, width, height);

    PangoFontDescription *font_desc = pango_font_description_from_string(XENOED_FONT);
    RenderState rs;
    render_init(&rs, font_desc);

    int focused = 1;

    redraw(&rs, &bb, &ed, width, height, focused);
    XMapWindow(dpy, win);

    int running = 1;
    int mouse_dragging = 0;
    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        if (xic && XFilterEvent(&ev, None)) continue;

        switch (ev.type) {
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_delete) running = 0;
                break;

            case ConfigureNotify:
                if (ev.xconfigure.width != width || ev.xconfigure.height != height) {
                    width = ev.xconfigure.width;
                    height = ev.xconfigure.height;
                    backbuffer_resize(&bb, width, height);
                    redraw(&rs, &bb, &ed, width, height, focused);
                }
                break;

            case Expose:
                if (ev.xexpose.count == 0) backbuffer_present(&bb);
                break;

            case ButtonPress:
                if (ev.xbutton.button == Button1 &&
                    (ed.mode == MODE_INSERT || ed.mode == MODE_NORMAL)) {
                    size_t l, c;
                    if (render_xy_to_pos(&rs, bb.surface, &ed, ev.xbutton.x, ev.xbutton.y,
                                          width, height, &l, &c)) {
                        ed.cur_line = l;
                        ed.cur_col = c;
                        editor_clamp_cursor(&ed);
                    }
                    ed.no_recenter = 1; /* cursor went where the pointer is; don't center on it */
                    editor_selection_clear(&ed);
                    mouse_dragging = 1;
                    sync_primary_ownership(dpy, win, &ed, &sel);
                    redraw(&rs, &bb, &ed, width, height, focused);
                } else if (ev.xbutton.button == Button3) {
                    show_context_menu(dpy, win, &ed);
                    process_editor_side_effects(dpy, win, &ed, &sel);
                    sync_primary_ownership(dpy, win, &ed, &sel);
                    redraw(&rs, &bb, &ed, width, height, focused);
                } else if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5) {
                    /* Classic X11 mouse wheel: Button4 = up, Button5 = down.
                     * Works in any mode. cursor is kept in the viewport and
                     * editor_scroll_by syncs follow_line so that the next
                     * redraw's editor_ensure_visible() won't recenter on it. */
                    int visible_rows = render_visible_rows(&rs, height);
                    int delta = (ev.xbutton.button == Button4)
                        ? -XENOED_SCROLL_LINES : XENOED_SCROLL_LINES;
                    editor_scroll_by(&ed, delta, (size_t)visible_rows);
                    sync_primary_ownership(dpy, win, &ed, &sel);
                    redraw(&rs, &bb, &ed, width, height, focused);
                }
                break;

            case MotionNotify:
                if (mouse_dragging && (ed.mode == MODE_INSERT || ed.mode == MODE_NORMAL ||
                                       ed.mode == MODE_VISUAL)) {
                    size_t l, c;
                    if (render_xy_to_pos(&rs, bb.surface, &ed, ev.xmotion.x, ev.xmotion.y,
                                          width, height, &l, &c)) {
                        if (!ed.sel_active) {
                            editor_selection_start(&ed);
                            if (ed.mode == MODE_NORMAL) {
                                /* Dragging in normal mode enters visual mode
                                 * with an inclusive selection, so the char
                                 * under the final cursor is included and the
                                 * usual visual keys (y/d/x/p, '!', ':') act on
                                 * what was selected with the mouse. */
                                ed.sel_inclusive = 1;
                                ed.sel_linewise = 0;
                                ed.mode = MODE_VISUAL;
                            }
                        }
                        ed.cur_line = l;
                        ed.cur_col = c;
                        editor_clamp_cursor(&ed);
                        ed.no_recenter = 1; /* drag follows the pointer, not a recentering */
                        sync_primary_ownership(dpy, win, &ed, &sel);
                        redraw(&rs, &bb, &ed, width, height, focused);
                    }
                }
                break;

            case ButtonRelease:
                if (ev.xbutton.button == Button1) mouse_dragging = 0;
                break;

            case SelectionRequest:
                handle_selection_request(dpy, &ed, &sel, &ev.xselectionrequest);
                break;

            case SelectionClear:
                if (ev.xselectionclear.selection == XA_PRIMARY) sel.primary_owned = 0;
                break;

            case KeyPress: {
                char text[64];
                KeySym keysym = NoSymbol;
                Status status;
                int len;

                if (xic) {
                    len = Xutf8LookupString(xic, &ev.xkey, text, (int)sizeof(text) - 1,
                                             &keysym, &status);
                } else {
                    len = XLookupString(&ev.xkey, text, (int)sizeof(text) - 1, &keysym, NULL);
                }
                if (len < 0) len = 0;
                text[len] = '\0';

                EditorSpecialKey special = classify_keysym(keysym, ev.xkey.state);
                if (special != EKEY_NONE) {
                    editor_handle_key(&ed, special, NULL, 0);
                } else {
                    /* Ctrl+key reaches the editor as its ASCII control
                     * character (Ctrl+A = 0x01 ... Ctrl+Z = 0x1A,
                     * Ctrl+[ = 0x1B, Ctrl+\ = 0x1C, Ctrl+] = 0x1D, Ctrl+^
                     * = 0x1E, Ctrl+_ = 0x1F), so editor.c can dispatch
                     * XENOED_COMMANDS' <c>X bindings in a toolkit-agnostic
                     * way -- the same representation the existing Ctrl+X/C/V
                     * shortcuts rely on, but synthesized straight from the
                     * keysym so it works even when the input method produces
                     * no text bytes for the combination (as it typically
                     * does for Ctrl+]). The bracket keysyms fall in the
                     * ASCII 0x5B..0x5F range, so the same 0x1F mask yields
                     * the right control byte. Ctrl+R was already consumed
                     * above as EKEY_REDO. */
                    unsigned char ctrl_byte = 0;
                    if ((ev.xkey.state & ControlMask) &&
                        ((keysym >= XK_a && keysym <= XK_z) ||
                         (keysym >= XK_A && keysym <= XK_Z) ||
                         (keysym >= XK_bracketleft && keysym <= XK_underscore)))
                        ctrl_byte = (unsigned char)(keysym & 0x1F);
                    if (ctrl_byte) {
                        editor_handle_key(&ed, EKEY_NONE, (const char *)&ctrl_byte, 1);
                    } else if (len > 0) {
                        editor_handle_key(&ed, EKEY_NONE, text, len);
                    }
                }

                if (ed.want_quit) { running = 0; break; }

                process_editor_side_effects(dpy, win, &ed, &sel);
                if (ed.search_requested) {
                    ed.search_requested = 0;
                    perform_search(&ed);
                }
                if (ed.command_menu_requested) {
                    ed.command_menu_requested = 0;
                    show_command_menu(dpy, win, &ed);
                    if (ed.want_quit) { running = 0; break; }
                }
                if (ed.goto_line_requested) {
                    ed.goto_line_requested = 0;
                    /* Bare prompt, no items: dmenu returns whatever line
                     * number was typed (or NULL on Escape). Feeding it back
                     * to editor_run_command() keeps this path in lockstep
                     * with the `:42` spelling, including clamp + status
                     * on a bad number. */
                    char *line_str = run_dmenu(dpy, win, NULL, 0, "goto line:");
                    if (line_str) {
                        editor_run_command(&ed, line_str);
                        free(line_str);
                    }
                }
                if (ed.external_filter_requested) {
                    ed.external_filter_requested = 0;
                    run_external_filter(dpy, win, &ed);
                }
                if (ed.user_command_requested) {
                    ed.user_command_requested = 0;
                    run_user_command(&ed, path);
                }

                sync_primary_ownership(dpy, win, &ed, &sel);
                redraw(&rs, &bb, &ed, width, height, focused);
                break;
            }

            case FocusIn:
                focused = 1;
                redraw(&rs, &bb, &ed, width, height, focused);
                break;

            case FocusOut:
                focused = 0;
                redraw(&rs, &bb, &ed, width, height, focused);
                break;

            default:
                break;
        }
    }

    if (xic) XDestroyIC(xic);
    if (xim) XCloseIM(xim);

    pango_font_description_free(font_desc);
    backbuffer_destroy(&bb);
    XFreeGC(dpy, bb.gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    editor_deinit(&ed);
    buffer_free(buf);

    return 0;
}
