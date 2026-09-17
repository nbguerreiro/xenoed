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
    CMD_INPUT_SELECTION,

    /* The word under the cursor -- a maximal run of non-whitespace bytes
     * on the current line; a cursor resting on whitespace spans to the
     * nearest word (the one to the left, then the one to the right) --
     * goes to the script's stdin, and (on exit 0) its stdout replaces
     * just that word, one undo step, cursor landing on the replacement's
     * first character. The normal-mode counterpart to CMD_INPUT_SELECTION:
     * it needs no selection because the cursor itself supplies the span,
     * so it's the natural way to bind an operator that acts on a single
     * word (uppercase it, look it up, run a linter on the identifier you
     * just typed). Visual mode refuses it -- the cursor there extends a
     * selection, not a word. */
    CMD_INPUT_WORD
} XenoedCommandInput;

typedef struct {
    const char *name;   /* shown in the ':' picker; NULL = don't list it there */
    const char *script; /* a command line, tokenized like a shell (single and
                          * double quotes, backslash escapes). A bare program
                          * name searches $PATH; anything containing '/' is
                          * used as-is. A multi-word one-liner (e.g.
                          * "perl -pe '$_ = lc'") runs exactly as written. */
    XenoedKey key;      /* how to run this directly: XENOED_KEY_LEADER('i')
                          * for SPACE then i, XENOED_KEY_CTRL('i') for Ctrl+i,
                          * XENOED_KEY_PLAIN('i') for i alone, or
                          * XENOED_KEY_NONE for ':' picker only */
    XenoedCommandInput input;
} XenoedCommand;

/* A bare program name (a single word, no spaces) receives the current file's
 * path as argv[1] (an empty string if there isn't one yet) -- the historical
 * contract that lets a script know its own language from the file extension,
 * or operate on a named file directly (CMD_INPUT_NONE) rather than through
 * stdin/stdout. A multi-word one-liner, by contrast, is executed exactly as
 * written: like the '!' filter, it reads the buffer/selection on stdin and is
 * NOT given the filename as an argument, so "perl -pe '$_ = lc'" keeps
 * filtering stdin instead of being told to open the file. To read the
 * filename itself in a one-liner, keep it as a separate field of your own.
 *
 * NULL-terminated, not sized by sizeof/sizeof: an array with a compile-
 * time-known length of zero isn't valid standard C (verified this
 * directly -- GCC tolerates it as an extension under this project's
 * actual build flags, but not under stricter ones, and it'd break outright
 * the moment someone deletes the example line to have zero commands of
 * their own). A sentinel avoids that regardless of how many real entries
 * exist. KEEP THE SENTINEL as the last entry.
 *
 * The "lowercase" and "format_table" entries are real, functional
 * one-liners (perl/column are present on any system with a decent base
 * install): selecting some text and running them via the ':' picker
 * actually transforms the selection, which is exactly the point of this
 * table. They're read-only examples of the CMD_INPUT_SELECTION pattern --
 * both only rewrite what you select, and only on CONFIRMED exit-0 success
 * (see main.c's run_filter()), so nothing happens unless you invoke them.
 * "upper_word" is the same idea one level smaller: "SPACE" then 'u'
 * uppercases the word under the cursor in place (CMD_INPUT_WORD), the
 * normal-mode singleton cut of the same perl one-liner, and
 * "lowercase_word" is its mirror image on the plain key 'w' (which
 * shadows no built-in), demonstrating the plain-letter binding spelling.
 * The "script" entry shows a Ctrl binding spelled as a punctuation key:
 * Ctrl+] is XENOED_KEY_CTRL(']').
 * "indent.sh" and the commented examples exist as dispatch-mechanism
 * placeholders that fail closed with a status message. Replace and add
 * entries freely; the leader key, the Ctrl key, the plain key and the ':'
 * picker all dispatch through the same path.
 */
static const XenoedCommand XENOED_COMMANDS[] = {
    { "indent",         "indent.sh",               XENOED_KEY_LEADER('i'), CMD_INPUT_BUFFER },
    { "lowercase",      "perl -pe '$_ = lc'",      XENOED_KEY_NONE,   CMD_INPUT_SELECTION },
    { "uppercase",      "perl -pe '$_ = uc'",      XENOED_KEY_NONE,   CMD_INPUT_SELECTION },
    { "upper_word",     "perl -pe '$_ = uc'",      XENOED_KEY_NONE,   CMD_INPUT_WORD },
    { "lowercase_word", "perl -pe '$_ = lc'",      XENOED_KEY_NONE,   CMD_INPUT_WORD },
    { "format_table",   "column -t -s '|' -o '|'", XENOED_KEY_NONE,   CMD_INPUT_SELECTION },

    { "tag_goto",   "/home/fx/src/x/xenoed/script.sh", XENOED_KEY_CTRL(']'),   CMD_INPUT_WORD },

    /*{ "example_picker", "your-script-here",           XENOED_KEY_NONE,        CMD_INPUT_BUFFER },*/

    { NULL, NULL, XENOED_KEY_NONE, CMD_INPUT_NONE } /* sentinel -- must stay last */
};

#endif
