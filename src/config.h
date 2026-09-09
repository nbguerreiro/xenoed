#ifndef XENOED_CONFIG_H
#define XENOED_CONFIG_H

#include <stddef.h> /* NULL, used in XENOED_COMMANDS' sentinel entry below */

/* The font is deliberately compile-time only -- no runtime config file to
 * parse, consistent with the rest of the project's minimalism. Override it
 * without touching this file via:
 *   make EXTRA_CFLAGS='-DXENOED_FONT="Georgia 14"'
 * or a variable-font axis string, since Pango passes these straight to
 * fontconfig:
 *   make EXTRA_CFLAGS='-DXENOED_FONT="Inter Variable @wght=650,opsz=18"'
 */
#ifndef XENOED_FONT
#define XENOED_FONT "Sans 14"
#endif

/* --- External commands -------------------------------------------------
 *
 * A compile-time table of external scripts, each optionally reachable from
 * the ':' picker (by name), a direct keypress (leader key + one letter),
 * or both. Same philosophy as the font above: this is data the compiler
 * checks, not a runtime config file with its own parser and its own ways
 * to get malformed.
 *
 * The leader key exists specifically so user-defined keybindings can never
 * collide with xenoed's own (h j k l 0 $ i a A I o O x d y p u v / n N :
 * in normal mode, Ctrl+R, y d x p in visual mode): SPACE-then-key is a
 * separate two-keystroke namespace that none of those single keys or
 * two-key sequences (dd, yy) touch, by construction -- not something that
 * has to be checked by hand against the existing list every time a new
 * command is added.
 */
#define XENOED_LEADER ' '

typedef enum {
    /* No stdin. xenoed does NOT wait for the script -- it's spawned fully
     * detached (double-forked so it can't become a zombie) and forgotten.
     * For side effects: launchers, notifications, anything that doesn't
     * hand something back to be inserted into the buffer. */
    CMD_INPUT_NONE,

    /* The whole buffer's current content (not necessarily what's on disk)
     * goes to the script's stdin. If the script exits 0, its stdout
     * replaces the ENTIRE buffer. A non-zero exit leaves the buffer
     * untouched and reports a failure status instead -- so a formatter
     * that errors out on invalid input can't clobber your buffer with a
     * half-finished or error-message output. */
    CMD_INPUT_BUFFER,

    /* The current selection's text goes to the script's stdin, and (on
     * exit 0) its stdout replaces just that selection. Only reachable via
     * the leader key from VISUAL mode -- normal mode never has an active
     * selection to feed it, so triggering one from the ':' picker (which
     * is normal-mode only) will always just report "needs a selection".
     * This is what makes a leader-key command with this input kind read
     * as an operator, the same way visual-mode y/d/x already do. */
    CMD_INPUT_SELECTION
} XenoedCommandInput;

typedef struct {
    const char *name;   /* shown in the ':' picker; NULL = don't list it there */
    const char *script; /* passed to execvp -- a bare name searches $PATH,
                          * anything containing '/' is used as-is, exactly
                          * like grep/dmenu are already invoked elsewhere */
    char key;           /* pressed right after the leader key to run this
                          * directly, e.g. 'i' for SPACE then i; 0 = no
                          * direct keybinding, ':' picker only */
    XenoedCommandInput input;
} XenoedCommand;

/* The script always receives the current file's path as argv[1] (an empty
 * string if there isn't one yet) -- unconditionally, regardless of
 * `input`, so there's no separate placeholder for it. A script that wants
 * to know its own language from the file extension, or just wants to
 * operate on a named file directly (CMD_INPUT_NONE) rather than through
 * stdin/stdout, always has it available.
 *
 * NULL-terminated, not sized by sizeof/sizeof: an array with a compile-
 * time-known length of zero isn't valid standard C (verified this
 * directly -- GCC tolerates it as an extension under this project's
 * actual build flags, but not under stricter ones, and it'd break outright
 * the moment someone deletes the example line to have zero commands of
 * their own). A sentinel avoids that regardless of how many real entries
 * exist. KEEP THE SENTINEL as the last entry.
 *
 * Real (uncommented), but harmless by construction: "your-script-here"
 * doesn't exist on any real system, so these entries can never
 * accidentally do something unexpected to your buffer before you've
 * replaced them with real scripts of your own -- output only ever gets
 * applied on a CONFIRMED exit-0 success (see main.c's run_filter()), so a
 * missing script just fails closed with a status message, regardless of
 * input kind. Kept uncommented specifically so the dispatch mechanism
 * itself -- the leader key, the ':' picker, the "needs a selection" check
 * -- is exercised and testable even before you've written a single script.
 * Replace the script paths (and add your own entries) freely; SPACE then
 * 't'/'b'/'s', or ":" then the name, will show each one is wired up
 * correctly and safely before anything real is behind it.
 */
static const XenoedCommand XENOED_COMMANDS[] = {
    { "notify", "notify-send", 'n', CMD_INPUT_NONE },
    { "indent", "indent.sh", 'i', CMD_INPUT_BUFFER },

    { NULL, NULL, 0, CMD_INPUT_NONE } /* sentinel -- must stay last */
};

#endif
