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

#ifndef XENOED_FONT_BAR
#define XENOED_FONT_BAR "Sans 11"
#endif

/* Multiplier applied to the font's ascent+descent to determine the editor
 * row height. The previous value was 1.25; 1.50 is therefore about 120% of
 * that spacing. Override this at compile time if desired, for example:
 *   make EXTRA_CFLAGS='-DXENOED_LINE_SPACING=1.35'
 */
#ifndef XENOED_LINE_SPACING
#define XENOED_LINE_SPACING 1.50
#endif

/* Lines moved per mouse-wheel click (X11 Button4/Button5). Override at
 * compile time if desired, for example:
 *   make EXTRA_CFLAGS='-DXENOED_SCROLL_LINES=5'
 */
#ifndef XENOED_SCROLL_LINES
#define XENOED_SCROLL_LINES 3
#endif

/* --- External commands -------------------------------------------------
 *
 * A compile-time table of external scripts, each optionally reachable from
 * the ':' picker (by name), a direct keypress, or both. Same philosophy as
 * the font above: this is data the compiler checks, not a runtime config
 * file with its own parser and its own ways to get malformed.
 *
 * A binding is a modifier plus a key. Three bindings exist:
 *   - leader: XENOED_LEADER then the key (a separate two-keystroke
 *     namespace that can't collide with xenoed's own single keys by
 *     construction);
 *   - control: Ctrl + key (the key is matched case-insensitively);
 *   - plain: the bare key -- these CAN shadow xenoed's built-ins, at
 *     your discretion; xenoed warns at startup if you do.
 * Or no binding at all -- reachable only via the ':' picker.
 */
#define XENOED_LEADER ' '

typedef enum {
    XENOED_MOD_NONE,   /* no direct keybinding: the ':' picker only */
    XENOED_MOD_LEADER, /* XENOED_LEADER then the key */
    XENOED_MOD_CTRL,   /* Ctrl + key */
    XENOED_MOD_PLAIN   /* the plain key alone */
} XenoedKeyModifier;

typedef struct {
    XenoedKeyModifier mod;
    char key;          /* the key's character; meaningful unless mod == XENOED_MOD_NONE */
} XenoedKey;

/* Config-table convenience spellings:
 *   XENOED_KEY_LEADER('i')  -- SPACE then i
 *   XENOED_KEY_CTRL('t')    -- Ctrl+t
 *   XENOED_KEY_PLAIN('e')   -- e alone
 *   XENOED_KEY_NONE         -- ':' picker only
 */
#define XENOED_KEY_NONE      { XENOED_MOD_NONE, 0 }
#define XENOED_KEY_LEADER(k) { XENOED_MOD_LEADER, (k) }
#define XENOED_KEY_CTRL(k)   { XENOED_MOD_CTRL, (k) }
#define XENOED_KEY_PLAIN(k)  { XENOED_MOD_PLAIN, (k) }

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
    XenoedKey key;      /* how to run this directly: XENOED_KEY_LEADER('i')
                          * for SPACE then i, XENOED_KEY_CTRL('i') for Ctrl+i,
                          * XENOED_KEY_PLAIN('i') for i alone, or
                          * XENOED_KEY_NONE for ':' picker only */
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
 * Real (uncommented), but harmless by construction: "indent.sh" and
 * "your-script-here" don't exist on any real system, so these entries can
 * never accidentally do something unexpected to your buffer before you've
 * replaced them with real scripts of your own -- output only ever gets
 * applied on a CONFIRMED exit-0 success (see main.c's run_filter()), so a
 * missing script just fails closed with a status message, regardless of
 * input kind. Kept uncommented specifically so the dispatch mechanism
 * itself -- the leader key, the Ctrl key, the plain key, the ':' picker,
 * the "needs a selection" check -- is exercised and testable even before
 * you've written a single script. Replace the script paths (and add your
 * own entries) freely; SPACE then 'i', Ctrl+t, plain 'e', or ":" then the
 * name, will each show its binding is wired up correctly and safely before
 * anything real is behind it. 'e' happens not to collide with any of
 * xenoed's own keys; a plain binding that DOES (say 'j' or ':' in
 * normal mode) prints a startup warning to stderr as a heads-up, then
 * shadows the built-in -- the plain namespace is deliberately unchecked.
 */
static const XenoedCommand XENOED_COMMANDS[] = {
    { "indent",         "indent.sh",                  XENOED_KEY_LEADER('i'), CMD_INPUT_BUFFER },

    { "lowercase",   "perl -pe '$_ = lc'", XENOED_KEY_NONE,   CMD_INPUT_SELECTION },
    { "format_table",   "column -t -s '|' -o '|'", XENOED_KEY_NONE,   CMD_INPUT_SELECTION },

    /*{ "script",   "/home/fx/src/x/xenoed/script.sh", XENOED_KEY_CTRL('t'),   CMD_INPUT_SELECTION },*/

    /*{ "example_plain",  "your-script-here",           XENOED_KEY_PLAIN('e'),  CMD_INPUT_NONE },*/
    /*{ "example_picker", "your-script-here",           XENOED_KEY_NONE,        CMD_INPUT_BUFFER },*/

    { NULL, NULL, XENOED_KEY_NONE, CMD_INPUT_NONE } /* sentinel -- must stay last */
};

#endif
