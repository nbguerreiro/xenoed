#define _POSIX_C_SOURCE 200809L /* nanosleep, mkstemp, fork/exec family */
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
#include "config.h"
#include "editor.h"
#include "render.h"

/* Off-screen backing store: we render each frame into an X Pixmap (via a
 * Cairo Xlib surface wrapping it) and then XCopyArea it onto the window in
 * a single call. Drawing straight onto the window's own surface -- clear,
 * then draw text as two separate server-visible operations -- is what was
 * causing the flicker; XCopyArea from a fully-drawn pixmap is atomic from
 * the server's point of view, so nothing is ever visible half-drawn. */
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
    bb->width = width;
    bb->height = height;
}

static void backbuffer_destroy(Backbuffer *bb) {
    cairo_surface_destroy(bb->surface);
    XFreePixmap(bb->dpy, bb->pixmap);
}

static void backbuffer_resize(Backbuffer *bb, int width, int height) {
    if (width == bb->width && height == bb->height) return;
    backbuffer_destroy(bb);
    backbuffer_create(bb, width, height);
}

static void backbuffer_present(Backbuffer *bb) {
    XCopyArea(bb->dpy, bb->pixmap, bb->win, bb->gc, 0, 0,
              (unsigned)bb->width, (unsigned)bb->height, 0, 0);
    XFlush(bb->dpy);
}

static void redraw(RenderState *rs, Backbuffer *bb, Editor *ed, int width, int height) {
    render_frame(rs, bb->surface, ed, width, height);
    cairo_surface_flush(bb->surface);
    backbuffer_present(bb);
}

/* X11 selection handling: PRIMARY tracks the live editor selection (set by
 * Shift+arrow / mouse drag), CLIPBOARD holds whatever was last explicitly
 * yanked with 'y'. Both are served on demand from SelectionRequest -- we
 * never push content anywhere proactively, we just answer "what do you
 * currently own" whenever asked, which is the whole point of the ICCCM
 * selection model. */
typedef struct {
    Atom clipboard;
    Atom utf8_string;
    Atom targets;
    Atom paste_prop; /* our own property name, used to receive paste replies */
    int primary_owned;
} X11Selections;

static void sync_primary_ownership(Display *dpy, Window win, Editor *ed, X11Selections *sel) {
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
    resp.property = None; /* default: refuse */

    /* ICCCM: pre-2.0 clients may set property to None, meaning "reply using
     * the target atom itself as the property name." */
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

/* Blocking round trip: ask `selection`'s current owner for UTF8_STRING
 * content and wait for the reply. Uses XCheckTypedWindowEvent in a short
 * poll loop rather than a plain XNextEvent -- that function only removes a
 * matching event from the queue, leaving any other pending events
 * (keypresses, etc.) right where they are for the main loop to handle
 * normally right after this returns. Real-world round trips here are
 * effectively instant (same machine, no network); the timeout just bounds
 * how long we'd wait on a misbehaving or nonexistent owner.
 *
 * One case needs special care: if THIS process is both the owner and the
 * requestor (yank, then paste, without ever leaving this window), the X
 * server sends the resulting SelectionRequest back to our own window --
 * and nothing will ever answer it if we only sit here waiting for
 * SelectionNotify. So we also drain and answer any SelectionRequest that
 * shows up during the wait, exactly like the main loop's own case does.
 * Without this, a same-window yank-then-paste deadlocks until timeout. */
static int fetch_selection_sync(Display *dpy, Window win, Editor *ed, Atom selection,
                                 X11Selections *sel, char **out_data, size_t *out_len) {
    XConvertSelection(dpy, selection, sel->utf8_string, sel->paste_prop, win, CurrentTime);
    XFlush(dpy);

    XEvent ev;
    for (int i = 0; i < 250; i++) { /* ~500ms max */
        while (XCheckTypedWindowEvent(dpy, win, SelectionRequest, &ev)) {
            handle_selection_request(dpy, ed, sel, &ev.xselectionrequest);
        }
        XFlush(dpy); /* make sure any reply we just queued actually reaches the server */

        if (XCheckTypedWindowEvent(dpy, win, SelectionNotify, &ev)) {
            if (ev.xselection.property == None) return 0; /* no owner, or it declined */

            Atom type;
            int format;
            unsigned long nitems, bytes_after;
            unsigned char *data = NULL;
            XGetWindowProperty(dpy, win, sel->paste_prop, 0, LONG_MAX / 4, False,
                                AnyPropertyType, &type, &format, &nitems, &bytes_after, &data);
            if (!data) return 0;

            *out_data = malloc(nitems + 1);
            memcpy(*out_data, data, nitems);
            (*out_data)[nitems] = '\0';
            *out_len = nitems;

            XFree(data);
            XDeleteProperty(dpy, win, sel->paste_prop);
            return 1;
        }
        struct timespec poll_delay = {0, 2 * 1000 * 1000}; /* 2ms */
        nanosleep(&poll_delay, NULL);
    }
    return 0; /* timed out: no owner responded */
}

/* One match, translated from grep's "byte offset into the file we handed
 * it" back into our own (line, col) coordinates. */
typedef struct {
    size_t line;
    size_t col;
    size_t global; /* grep's own byte offset for this match, unchanged --
                     * reused directly for the forward/backward comparisons
                     * below instead of re-deriving it from line/col. */
} SearchMatch;

/* Runs `ed->search_pattern` (an ERE, per grep -E) against the buffer's
 * CURRENT content -- not necessarily what's on disk -- and moves the
 * cursor to the next match in the requested direction, wrapping around
 * with a vim-style status message if needed. Writes an error to ed->status
 * instead if the pattern doesn't compile or nothing matches.
 *
 * The buffer is handed to grep via a temp file, not stdin: piping a large
 * buffer into a subprocess while also trying to read its output back
 * requires either interleaved non-blocking I/O or risks both ends filling
 * their pipe buffers and deadlocking. A temp file sidesteps that whole
 * class of problem for a fraction of the code -- write it, close it, let
 * grep open it itself, done -- and /tmp (or $TMPDIR) is typically tmpfs
 * anyway, so this isn't really touching a physical disk in practice. */
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

    /* Remember where each of our own lines starts within the file we're
     * about to write, in the exact same byte terms grep will report
     * matches in, so a match's byte offset can be mapped straight back to
     * one of our lines with no re-parsing of anything grep printed. */
    size_t *line_start = malloc(b->count * sizeof(size_t));
    if(line_start == NULL){ _exit(-1); };

    size_t total = 0;
    FILE *tmp_f = fdopen(tmp_fd, "w");
    if(tmp_f == NULL){ _exit(-1); };

    for (size_t i = 0; i < b->count; i++) {
        Line *l = buffer_line(b, i);
        line_start[i] = total;
        fwrite(l->data, 1, l->len, tmp_f);
        fputc('\n', tmp_f);
        total += l->len + 1;
    }
    fclose(tmp_f); /* also closes tmp_fd */

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
        /* Child: stdout -> the pipe; stderr -> /dev/null, since we report
         * our own error messages rather than surfacing grep's raw text.
         * -h suppresses the filename prefix grep would otherwise always
         * add for a single named-file argument like this one; -- ensures
         * a pattern that happens to start with '-' can't be misread as a
         * flag; the pattern and path are passed as separate argv entries
         * (never through a shell), so nothing in the pattern -- quotes,
         * semicolons, backticks -- can do anything but be literal regex. */
        dup2(outpipe[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(outpipe[0]);
        close(outpipe[1]);
        execlp("grep", "grep", "-n", "-o", "-b", "-h", "-E", "--", ed->search_pattern, tmp_path,
               (char *)NULL);
        _exit(127); /* only reached if exec itself failed */
    }

    close(outpipe[1]);
    char *out = NULL;
    size_t outlen = 0, outcap = 0;
    char chunk[4096];
    ssize_t n;
    while ((n = read(outpipe[0], chunk, sizeof(chunk))) > 0) {
        if (outlen + (size_t)n > outcap) {
            outcap = (outlen + (size_t)n) * 2 + 64;
            out = realloc(out, outcap);
            if(out == NULL){ _exit(-1); };
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

    /* Parse "LINE:BYTEOFFSET:MATCHEDTEXT\n" per match. We only trust our
     * own line_start[] for locating matches (not grep's LINE field) since
     * we already know exactly how the file was laid out; li advances
     * monotonically across matches rather than resetting per-match, since
     * grep reports them in increasing file order. */
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
            char *c2 = memchr(c1 + 1, ':', (size_t)((p + linelen) - (c1 + 1)));
            if (c2) {
                long byte_off = strtol(c1 + 1, NULL, 10);
                if (byte_off >= 0) {
                    while (li + 1 < b->count && line_start[li + 1] <= (size_t)byte_off) li++;
                    if (match_count + 1 > match_cap) {
                        match_cap = match_cap ? match_cap * 2 : 16;
                        matches = realloc(matches, match_cap * sizeof(SearchMatch));
                        if(matches == NULL){ _exit(-1); };
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
        /* %.60s, not %s: search_pattern is a fixed 256-byte buffer, so
         * GCC's -Wformat-truncation correctly points out the two literal
         * prefixes/suffixes plus a worst-case-length pattern could exceed
         * status's own 256 bytes. snprintf would truncate safely regardless
         * (it never overflows), but capping the pattern explicitly both
         * satisfies that check and keeps the message legible -- the status
         * bar is one line and can't show 255 characters on screen anyway. */
        snprintf(ed->status, sizeof(ed->status), "E: pattern not found: %.60s", ed->search_pattern);
        free(matches);
        free(line_start);
        return;
    }

    /* line_start[] gives this in O(1); grep's own byte_off (stored as
     * matches[i].global below) already IS each match's global offset, so
     * no further per-match recomputation is needed for the comparisons
     * that follow. */
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

/* Spawns `script` fully detached: double-forked so the grandchild gets
 * reparented to init and never becomes xenoed's responsibility to reap
 * (not even as a zombie), with stdin/stdout/stderr redirected to
 * /dev/null. For CMD_INPUT_NONE commands -- xenoed does not wait for or
 * care about this process at all once it's launched. */
static void run_detached(const char *script, const char *filename) {
    pid_t pid = fork();
    if (pid < 0) return; /* best-effort; nothing sensible to report from here */

    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 == 0) {
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
            }
            execlp(script, script, filename, (char *)NULL);
            _exit(127); /* only reached if exec itself failed */
        }
        _exit(0); /* first child exits immediately; the grandchild is now init's problem */
    }
    waitpid(pid, NULL, 0); /* reap the near-instantly-exiting first child only */
}

/* Runs `script` as an ordinary Unix filter: `input` (input_len bytes) goes
 * to its stdin, its stdout is captured and returned (malloc'd, caller
 * frees; *out_len set to its length). Returns NULL if the script couldn't
 * be spawned, wasn't found, or exited non-zero -- output is only ever
 * returned on a CONFIRMED exit-0 success, so a failing or misbehaving
 * script can never clobber the buffer with a half-finished or error-
 * message result.
 *
 * Feeding `input` into the pipe is done by a disposable THIRD process, not
 * xenoed itself, specifically to avoid the classic bidirectional-pipe
 * deadlock: if xenoed tried to interleave writing input with reading
 * output itself, both ends' pipe buffers could fill up simultaneously --
 * xenoed blocked writing because the script hasn't drained enough yet,
 * the script blocked writing its own output because xenoed hasn't started
 * reading yet -- with neither side able to make progress. perform_search()
 * sidesteps this differently (a temp file, since grep is happy to take a
 * path instead of stdin), which doesn't fit here: the whole point is
 * scripts that behave like ordinary Unix filters, reading stdin and
 * writing stdout, the way indent.sh or any real filter script would. */
static char *run_filter(const char *script, const char *filename,
                         const char *input, size_t input_len, size_t *out_len) {
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
        execlp(script, script, filename, (char *)NULL);
        _exit(127);
    }

    /* Disposable writer: its only job is feeding `input` into inpipe, so
     * the actual parent never has to interleave writing input with
     * reading output -- see the deadlock explanation above. */
    pid_t writer_pid = fork();
    if (writer_pid == 0) {
        close(inpipe[0]);
        close(outpipe[0]); close(outpipe[1]);
        size_t written = 0;
        while (written < input_len) {
            ssize_t n = write(inpipe[1], input + written, input_len - written);
            if (n <= 0) break; /* best-effort; if this fails, the script just sees a short read */
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
            out = realloc(out, outcap);
            if(out == NULL){ _exit(-1); };
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
    if (!out) out = malloc(1); /* empty-but-successful output is legitimate (e.g. a script that intentionally clears the buffer) */
    return out;
}

/* Dispatches XENOED_COMMANDS[ed->user_command_index] (set by editor.c's
 * leader-key or ':' picker dispatch, see editor_request_user_command())
 * to run_detached() or run_filter() per its `input` kind, and applies the
 * result -- editor_replace_buffer_text() for CMD_INPUT_BUFFER,
 * editor_paste_text() for CMD_INPUT_SELECTION, both of which already do
 * exactly the right thing and handle their own undo checkpointing.
 * `filename` is argv[1] from main(), unconditionally passed to the script
 * regardless of input kind, per how config.h documents it. */
static void run_user_command(Editor *ed, const char *filename) {
    int idx = ed->user_command_index;
    if (idx < 0) return; /* defensive; editor.c only ever sets a valid index */
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
        size_t off = 0;
        for (size_t i = 0; i < b->count; i++) {
            Line *l = buffer_line(b, i);
            memcpy(input_text + off, l->data, l->len);
            off += l->len;
            input_text[off++] = '\n';
        }
        input_len = total;
    } else { /* CMD_INPUT_SELECTION */
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
    } else {
        editor_paste_text(ed, output, output_len);
    }
    free(output);

    snprintf(ed->status, sizeof(ed->status), "%s: done", label);
}

/* Claims CLIPBOARD ownership after a yank, and fetches+applies a paste --
 * the two "editor.c requested some OS-level X11 work" flags that need
 * handling after ANY input event that could set them, not just KeyPress
 * (the right-click context menu's Copy/Cut/Paste can set them too). */
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

/* Runs the user's own $DMENU (falling back to plain `dmenu` if unset),
 * embedded into our own window via -w so it needs no separate i3 window
 * rule and can't get tiled somewhere odd, offering `items` (one per line)
 * on its stdin. Returns whatever line was chosen (malloc'd, caller frees),
 * or NULL if nothing was (Esc in dmenu, dmenu missing, or any other
 * failure along the way).
 *
 * $DMENU is expanded by a REAL shell at runtime (inherited from our own
 * environment), via ${DMENU:-dmenu} inside a script string passed to
 * `sh -c` -- deliberately NOT read via getenv() in C and re-embedded into
 * a freshly-built command string. Those look equivalent but aren't: this
 * project's own example config has an unquoted `-p >` in it, which is
 * completely safe under normal (word-splitting-only) variable expansion --
 * exactly how it already behaves in the user's own shell scripts -- but
 * would get re-parsed as an actual shell redirection operator if the
 * fully-expanded string were fed to a fresh `sh -c "..."` as new syntax
 * (verified this failure mode directly before writing it this way: it
 * silently ate the "-l" that should have been dmenu's line-count flag,
 * redirecting output to a file literally named "-l" instead). The window
 * ID and prompt are passed as real positional parameters ($1/$2), not
 * string-interpolated, so they can't have this problem regardless of
 * content either -- though they're fixed/trusted values here anyway.
 *
 * Reclaims `win` (xenoed's own window) as input focus once dmenu exits,
 * rather than leaving that to whatever implicit revert-to state resulted
 * from dmenu taking focus for itself while embedded. This matters more
 * than it might look: an earlier attempt at this fixed a reported "focus
 * left dangling after quitting" bug by forcing focus to PointerRoot at
 * shutdown, but that traded it for a DIFFERENT bug -- PointerRoot puts the
 * X server into a raw "focus follows whatever's under the pointer" state
 * that i3 has no idea about, since i3 manages its own notion of focus
 * independently of what any client directly pokes into the server, so its
 * bookkeeping went stale even though the server itself was routing keys
 * correctly again. RevertToParent instead: for a reparenting WM like i3,
 * a client window's "parent" is the WM's own frame around it, so if THIS
 * window later becomes unviewable (including via XDestroyWindow at
 * xenoed's own shutdown), focus reverts to that frame -- which i3 already
 * owns and already knows what to do with, rather than a raw pointer-
 * position rule it was never told about. Doing this right after every
 * dmenu close, not just at shutdown, keeps i3's bookkeeping continuously
 * correct through the whole session, so by the time xenoed does exit,
 * there's nothing unusual left for i3's own destroy-triggered refocus
 * logic to untangle -- it just does what it always does. */
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
        if (write(inpipe[1], items[i], strlen(items[i])) < 0) break; /* best-effort */
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
            out = realloc(out, outcap);
            if(out == NULL){ _exit(-1); };
        }
        memcpy(out + outlen, chunk, (size_t)n);
        outlen += (size_t)n;
    }
    close(outpipe[0]);

    waitpid(pid, NULL, 0); /* exit status doesn't matter here, unlike perform_search()'s grep call */
    XSetInputFocus(dpy, win, RevertToParent, CurrentTime);

    if (!out || outlen == 0) {
        free(out);
        return NULL; /* cancelled, or dmenu isn't available */
    }
    if (out[outlen - 1] == '\n') outlen--; /* dmenu always terminates its choice with one */
    out = realloc(out, outlen + 1);
    out[outlen] = '\0';
    return out;
}

/* Right-click context menu: Copy/Cut (only offered with an active
 * selection) and Paste (always offered). Available regardless of mode --
 * unlike left-click, which only starts a selection in insert mode -- since
 * a selection made via visual mode or insert-mode Shift+arrow is just as
 * valid a thing to right-click on. */
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

/* Replaces the old inline ':' typing bar: offers the known commands (see
 * editor_command_names()) via dmenu, and hands whatever comes back --
 * either one picked directly, or freely typed text like "w notes.txt"
 * that dmenu returns verbatim on Enter even with nothing highlighted --
 * to editor_run_command(), which does the actual parsing exactly as it
 * did when fed from the character-by-character bar. */
static void show_command_menu(Display *dpy, Window win, Editor *ed) {
    int count = 0;
    const char *const *names = editor_command_names(&count);

    char *choice = run_dmenu(dpy, win, names, count, ":");
    if (!choice) return; /* Esc in dmenu, or dmenu unavailable */

    editor_run_command(ed, choice);
    free(choice);
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
        case XK_r:
        case XK_R:         return ctrl ? EKEY_REDO : EKEY_NONE;
        default:           return EKEY_NONE;
    }
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : NULL;

    /* Optional 1-based line number, e.g. `xenoed notes.txt 12`. An invalid
     * (non-numeric) value is reported and ignored rather than treated as a
     * fatal error; an out-of-range one is silently clamped to the last
     * line once the file is loaded and we know how many there are. */
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

    XStoreName(dpy, win, path ? path : "xenoed");

    /* WM_CLASS: lets external tools (e.g. an i3-IPC script hunting for "the
     * editor's windows") identify xenoed windows without xenoed itself ever
     * needing to know i3 exists. res_name is the per-instance name (lower
     * case, by convention); res_class is the general class (capitalized). */
    XClassHint *class_hint = XAllocClassHint();
    if (class_hint) {
        class_hint->res_name = (char *)"xenoed";
        class_hint->res_class = (char *)"Xenoed";
        XSetClassHint(dpy, win, class_hint);
        XFree(class_hint);
    }

    /* We do our own full-frame double buffering, so the server doesn't need
     * to keep a backing store for us -- ConfigureNotify + Expose handling
     * below is enough to always have a correct frame ready to present.
     * Button1MotionMask (rather than PointerMotionMask) means we only get
     * MotionNotify events while button 1 is actually held, which is all
     * drag-to-select needs. */
    XSelectInput(dpy, win,
                 KeyPressMask | ExposureMask | StructureNotifyMask |
                 ButtonPressMask | ButtonReleaseMask | Button1MotionMask);

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

    XMapWindow(dpy, win);

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

    /* Paint a real first frame now: without this, the Expose event from
     * XMapWindow would XCopyArea an as-yet-unrendered (garbage) backbuffer
     * before anything ever calls render_frame(). */
    redraw(&rs, &bb, &ed, width, height);

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
                    redraw(&rs, &bb, &ed, width, height);
                }
                break;

            case Expose:
                /* The backbuffer already holds a fully rendered frame, so a
                 * plain copy is enough -- no need to re-run layout/render. */
                if (ev.xexpose.count == 0) backbuffer_present(&bb);
                break;

            case ButtonPress:
                if (ev.xbutton.button == Button1 && ed.mode == MODE_INSERT) {
                    size_t l, c;
                    if (render_xy_to_pos(&rs, bb.surface, &ed, ev.xbutton.x, ev.xbutton.y,
                                          width, height, &l, &c)) {
                        ed.cur_line = l;
                        ed.cur_col = c;
                        editor_clamp_cursor(&ed);
                    }
                    editor_selection_clear(&ed); /* a plain click starts fresh */
                    mouse_dragging = 1;
                    sync_primary_ownership(dpy, win, &ed, &sel);
                    redraw(&rs, &bb, &ed, width, height);
                } else if (ev.xbutton.button == Button3) {
                    /* Blocks here until the menu is dismissed -- correctly
                     * so: the whole point of a context menu is to pause
                     * editing until something's chosen, same as dmenu
                     * already blocks perform_search()'s caller. */
                    show_context_menu(dpy, win, &ed);
                    process_editor_side_effects(dpy, win, &ed, &sel);
                    sync_primary_ownership(dpy, win, &ed, &sel);
                    redraw(&rs, &bb, &ed, width, height);
                }
                break;

            case MotionNotify:
                if (mouse_dragging && ed.mode == MODE_INSERT) {
                    size_t l, c;
                    if (render_xy_to_pos(&rs, bb.surface, &ed, ev.xmotion.x, ev.xmotion.y,
                                          width, height, &l, &c)) {
                        /* editor_selection_start() anchors at the CURRENT
                         * cursor position -- which, since we haven't moved
                         * it yet this event, is still the original press
                         * point. Only do this once, the first time the drag
                         * actually moves off that point. */
                        if (!ed.sel_active) editor_selection_start(&ed);
                        ed.cur_line = l;
                        ed.cur_col = c;
                        editor_clamp_cursor(&ed);
                        sync_primary_ownership(dpy, win, &ed, &sel);
                        redraw(&rs, &bb, &ed, width, height);
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
                } else if (len > 0) {
                    editor_handle_key(&ed, EKEY_NONE, text, len);
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
                    if (ed.want_quit) { running = 0; break; } /* e.g. a ":q"/"wq" picked from the menu */
                }
                if (ed.user_command_requested) {
                    /* Checked AFTER command_menu_requested, not instead of
                     * it: a user command picked via ':' (editor_run_command()'s
                     * fallback) sets this same flag, so a leader-key press
                     * and a ':'-picker choice both end up handled right
                     * here either way. */
                    ed.user_command_requested = 0;
                    run_user_command(&ed, path);
                }

                sync_primary_ownership(dpy, win, &ed, &sel);
                redraw(&rs, &bb, &ed, width, height);
                break;
            }

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
