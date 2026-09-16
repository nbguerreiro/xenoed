# xenoed

A tiny, modal (vi-like) text editor for X11, built on Xlib + Cairo + Pango.
Proportional (and variable) fonts render correctly because text is laid out
and shaped by Pango/HarfBuzz rather than blitted from a fixed-width glyph
grid.

This is a standalone editor — no Neovim involved. It implements just two
modes (**Normal** and **Insert**) with the smallest useful command set.

## Build

Dependencies (Debian/Ubuntu package names):

```
sudo apt-get install build-essential pkg-config libx11-dev libcairo2-dev libpango1.0-dev
```

Then:

```
make
./xenoed [file] [line-number]
```

Examples:

```
./xenoed notes.txt
./xenoed notes.txt 42        # opens with the cursor on line 42
```

If no filename is given, xenoed opens an empty untitled buffer (save it
with `:w <path>`). An out-of-range line number is clamped to the last line
rather than treated as an error; an invalid (non-numeric) one is reported
to stderr and ignored, opening at line 1.

The font is deliberately compile-time only, not a runtime argument — no
config file to parse, consistent with the rest of the project. It defaults
to `"Sans 12"`; override it at build time instead of editing source:

```
make EXTRA_CFLAGS='-DXENOED_FONT="Georgia 14"'
make EXTRA_CFLAGS='-DXENOED_FONT="Inter Variable @wght=650,opsz=18"'   # variable font axes
```

## Keybindings

**Normal mode**
| Key | Action |
|---|---|
| `h j k l` / arrow keys | move left/down/up/right, wrapping to the previous/next line at line boundaries |
| `0` / `$` | start / end of line |
| `i` | insert before cursor |
| `a` | insert after cursor |
| `I` / `A` | insert at start / end of line |
| `o` / `O` | open new line below / above, enter insert |
| `x` | delete character under cursor |
| `dd` | delete current line |
| `yy` | yank (copy) current line to CLIPBOARD |
| `p` | paste CLIPBOARD after the current line (or inline, if what's on the clipboard isn't a whole line) |
| `u` | undo |
| `Ctrl+R` | redo |
| `v` | enter visual mode (characterwise) |
| `V` | enter visual mode (linewise) |
| `!` | filter the whole buffer through a shell command (dmenu prompt) |
| `/` | search forward (see below) |
| `n` / `N` | repeat the last search, forward / backward |
| `:` | open the command picker (see below) |
| `SPACE` then a key | run a user-defined external command (see below) |
| `Esc` | (no-op in normal mode) |

**Visual mode** (entered with `v` or `V`)

`v` is characterwise; `V` is linewise (whole lines from the smaller of
the anchor/cursor line through the larger, regardless of column). You can
switch between the two with `v`/`V` while already in visual mode.
Movement (`h j k l`, arrows, `0`, `$`) extends the selection from wherever
the cursor was when visual mode began. Unlike insert-mode selection, the
cursor here rests *on* a character (matching normal mode), so characterwise
selections are inclusive of both ends -- pressing `v` alone already
selects the one character under the cursor, and `V` alone selects the
whole current line.

| Key | Action |
|---|---|
| `y` | yank the selection to CLIPBOARD, cursor moves to its start, back to normal mode |
| `d` / `x` | cut (yank, then delete) the selection, back to normal mode |
| `p` | paste CLIPBOARD over the selection, back to normal mode |
| `!` | filter the selection through a shell command (dmenu prompt) |
| `v` / `V` | switch to characterwise / linewise while staying in visual mode |
| `SPACE` then a key | run a user-defined external command on the selection (see below) |
| `Esc` | cancel -- back to normal mode, buffer unchanged |

Linewise yanks end in a newline (same as `yy`), so a later `p` pastes them
as whole lines; linewise delete removes the lines entirely (same as `dd`).

This is intentionally minimal: no case-changing or block-visual variants --
just enough for `y`/`d` to act on an arbitrary span or set of lines instead
of only whole lines via `yy`/`dd`. Note vim's own visual-mode `u` means
something different (lowercase the selection); rather than risk that
confusion, `u` is simply unbound in visual mode here.

**Insert mode**

Typed text is inserted at the cursor (full UTF-8 passthrough via X's input
method, so dead keys / compose sequences work normally). `Backspace` and
`Enter` behave as expected, including joining lines and splitting lines.
`Esc` returns to normal mode.

Text selection via Shift+arrow or mouse drag (see visual mode above for the
normal-mode equivalent):
- **Keyboard**: hold `Shift` with `← → ↑ ↓` to extend a selection from
  wherever the cursor was when you started holding Shift. A plain arrow
  (no Shift) collapses the selection and moves normally.
- **Mouse**: click and drag to select. A plain click without dragging just
  moves the cursor. The mouse wheel scrolls the viewport by
  `XENOED_SCROLL_LINES` lines (default 3; override at compile time), in
  any mode; the cursor stays inside the visible area so the next redraw
  doesn't snap the view back.
- With an active selection, typing replaces it; `Backspace` deletes it
  (without also deleting an extra character); `Enter` replaces it with a
  line break. Selections spanning multiple lines are supported.
- `Esc` clears any active selection along with returning to normal mode.

## Copy / paste

Two separate X11 selections are used, deliberately not one:

- **PRIMARY** tracks whatever is currently selected -- Shift+arrow, mouse
  drag, or visual mode, all the same, since it's driven by
  `editor_has_selection()` rather than checking which mode made the
  selection -- automatically, the traditional X convention, so it's always
  available for middle-click paste into any other X app with no explicit
  copy action needed.
- **CLIPBOARD** is only touched by an explicit yank/paste (`yy`/`p` in
  normal mode, `y`/`d`/`x`/`p` in visual mode), matching the more familiar
  "you have to copy on purpose" convention most apps use.

Normal-mode `y`/`p` only work on whole lines (`yy`); visual mode (`v` /
`V`) is how to yank or cut an arbitrary span or set of lines instead. `p`
always asks the real CLIPBOARD owner for its content -- so it works for
pasting text copied in *any* other X application, not just xenoed's own
yanks -- and decides whether to paste it as whole new line(s) or inline at
the cursor based on whether the copied text ends in a newline.

## Right-click context menu

Right-click offers Copy/Cut (only when there's an active selection --
insert-mode Shift+arrow/mouse-drag or visual mode, doesn't matter which)
and Paste (always). Available in any mode, unlike left-click.

Genuinely outsourced, same spirit as search: this shells out to your own
`$DMENU`, not a hand-built popup widget. Set `$DMENU` (e.g. in `.profile`)
to your preferred dmenu invocation and its exact flags, colors, and font
carry over unchanged:

```
DMENU="dmenu -fn sans-12 -nf #ffffff -nb #000000 -sb #ffffff -sf #000000 -b -i -p > -l 15"
```

Falls back to plain `dmenu` if `$DMENU` isn't set. xenoed always adds `-w`
to embed dmenu directly into its own window -- this needs a dmenu build
with the embed patch (`-w windowid`); without one, add it or drop `-w` from
how xenoed invokes it. Embedding means no separate X11 window for i3 to
tile awkwardly, at the cost of dmenu appearing wherever *your* `-b`/`-l`
flags place it within the xenoed window, not necessarily right at the
click point -- a deliberate trade-off, not an oversight: positioning
precisely at the cursor would need dmenu's own geometry patches (`-x`/`-y`)
layered on top, which is a reasonable follow-up if it turns out to matter
in practice, but wasn't built speculatively here.

`$DMENU` is expanded by a real shell at runtime, inherited from xenoed's
own environment, specifically so any shell-meaningful characters already
in it (this example's own unquoted `-p >`) stay literal dmenu arguments
exactly like they already do in your own scripts, rather than risk being
re-parsed as actual shell syntax.

## Command picker

`:` no longer opens an inline typing bar at the bottom of the window --
it shells out to the same `$DMENU` as the right-click menu, seeded with
the known commands:

| Command | Action |
|---|---|
| `w` | save |
| `w path` | save as `path` |
| `q` | quit (refuses if there are unsaved changes) |
| `q!` | quit, discarding changes |
| `wq` | save and quit |
| `x` | save and quit |
| `s/pattern/repl/[g]` | search-and-replace (see below) |

Picking one directly works as expected; so does ignoring the list and just
typing, e.g. `w notes.txt` -- dmenu returns whatever's currently typed on
Enter even when nothing's highlighted, so commands taking an argument work
exactly as they did with the old bar, just entered through dmenu instead.

### Search and replace (`:s`)

`:s/pattern/repl/` finds `pattern` (POSIX extended regex, case-insensitive)
and replaces each line's *first* match with `repl`; a trailing `g` replaces
*every* match per line. In normal mode the scope is the whole buffer; in
visual mode it's just the selection (one undo step covers a selection
replacement; whole-buffer replaces are also one undo step). Prefix the
command with `%` (`:%s/.../.../`) to force whole-buffer scope even from
visual mode. An empty pattern reuses the last `/` search pattern; an empty
replacement deletes the matches.

In `repl`, `&` is the whole match, `\0`-`\9` are the capture groups
(`\0` the whole match, `\1`.. the parenthesized groups), `\X` a literal `X`
(e.g. `\\` a single backslash), and `\/` a literal `/`. A pattern that
matches nothing leaves the buffer unchanged and reports
`E: pattern not found`.

Every non-empty choice is remembered in `~/.xenoed/colon_hist`. Next time
you open `:`, most-used commands float to the top of the dmenu list
(builtins and named `XENOED_COMMANDS` entries that aren't already in the
history still appear below).

## Filter (`!`)

Vim-style filter through an arbitrary shell command, prompted with the
same embedded dmenu as `:` (prompt `!`, empty item list -- type freely):

| Context | Input | On exit 0 |
|---|---|---|
| Normal mode `!` | whole buffer | replaces the entire buffer |
| Visual mode `!` | current selection | replaces just the selection |

The command runs via `/bin/sh -c`, so shell syntax works (`sort -u`,
`tr 'a-z' 'A-Z'`, `grep -v '^#'`, pipes, etc.). Cancel (Esc in dmenu), an
empty command, or a non-zero exit leave the buffer unchanged and keep any
visual selection intact. Successful empty stdout (e.g. `true`, or `grep`
with no matches) deletes the filtered span. One undo step covers the
whole replace.

Successful filters are remembered in `~/.xenoed/bang_hist` and float to
the top of the next `!` dmenu prompt (most-used first). Failed or
cancelled commands are not recorded.

## External commands

A compile-time table in `src/config.h` (`XENOED_COMMANDS`) for calling
your own scripts -- same philosophy as the font and the command list
above: data the compiler checks, not a runtime config file with its own
parser. Each entry:

```c
{ "indent", "indent.sh", XENOED_KEY_LEADER('i'), CMD_INPUT_BUFFER },
```

is a name (shown in the `:` picker; `NULL` to leave it out), a script
(passed to `execvp` -- a bare name searches `$PATH`, same as `grep`/`dmenu`
are already invoked), a keybinding, and an input kind. A keybinding is
one of four spellings:

| Spelling | Runs when |
|---|---|
| `XENOED_KEY_LEADER('i')` | you press `SPACE` then `i` |
| `XENOED_KEY_CTRL('t')` | you press `Ctrl+t` (letter matched case-insensitively) |
| `XENOED_KEY_PLAIN('e')` | you press plain `e` alone |
| `XENOED_KEY_NONE` | only from the `:` picker |

`SPACE` (the leader key, `XENOED_LEADER`) is a separate two-keystroke
namespace that can't collide with xenoed's own single keys by
construction. Ctrl bindings reach the editor as ASCII control characters
`Ctrl+A`=`0x01` ... `Ctrl+Z`=`0x1A`, the same channel the built-in
`Ctrl+X`/`Ctrl+C`/`Ctrl+V` insert-mode shortcuts already use. Plain
bindings live in the same single-key namespace as xenoed's own commands,
so they *can* shadow one (at your discretion -- xenoed warns about it on
startup), which is why `Ctrl`/leader exist as deliberate alternatives.

| Input kind | What happens |
|---|---|
| `CMD_INPUT_NONE` | No stdin. xenoed doesn't wait -- the script is fully detached and forgotten. For side effects: launchers, notifications, anything with nothing to hand back. |
| `CMD_INPUT_BUFFER` | The whole buffer's current content (not necessarily what's on disk) goes to stdin. On exit 0, stdout replaces the entire buffer, as one undo step. A non-zero exit leaves the buffer untouched. |
| `CMD_INPUT_SELECTION` | The current selection's text goes to stdin; on exit 0, stdout replaces just that selection. Only reachable from **visual mode** -- normal mode never has an active selection, so triggering one via `:` always reports "needs a selection". This is what makes a direct-keybinding (leader/Ctrl/plain) command with this input kind read as an operator, the same way visual-mode `y`/`d`/`x` already do. |

The script's file path is always passed as `argv[1]` (an empty string if
there isn't one), regardless of input kind -- there's no separate
placeholder for it, unlike the input kind above. One thing worth knowing
if you write a `CMD_INPUT_BUFFER`/`CMD_INPUT_SELECTION` script that also
wants this: most standard Unix filters (`cat`, `sort`, `tr`, `sed`
without `-i`, etc.) treat a trailing filename argument as "read from this
file instead of stdin" -- the opposite of what you want, since the whole
point is that stdin carries the buffer/selection content, not whatever's
currently on disk. A script meant for those two input kinds should ignore
`$1` as a data source (use it only for context, e.g. picking a formatter
by file extension) and read stdin unconditionally.

Four example entries ship uncommented, using an obviously-nonexistent
script name (`your-script-here`) or a deliberately-harmless one rather
than anything real: harmless by construction, since output only ever gets
applied on a *confirmed* exit-0 success, so a missing script just fails
closed with a status message regardless of input kind -- it can never
surprise you by touching the buffer. They're there so the dispatch
mechanism itself (the leader key, the Ctrl key, the plain key, the `:`
picker, the "needs a selection" check) is something you can see working
-- `SPACE` then `i`, `Ctrl+t`, plain `e`, or `:` then the name -- before
you've written a single script of your own. Replace them, or add
alongside. If one of your plain bindings collides with one of xenoed's
own keys (say `j` or `:`), xenoed prints a one-line startup warning --
that's the head's-up that the binding will shadow the built-in, which is
exactly what a plain binding does by design.

**Duplicate keybindings across your own entries** (two commands bound to
the same modifier+key, e.g. two `XENOED_KEY_CTRL` entries on the same
letter) are reported as a warning on startup, not silently resolved one
way or the other -- check xenoed's stderr if a keybinding doesn't seem to
do what you expected.

**Fire-and-forget (`CMD_INPUT_NONE`) processes are double-forked** so the
detached process is reparented to init and reaped automatically when it
eventually exits, whatever it's doing -- it never becomes a zombie, and
xenoed never has to track it.

**Feeding a script's stdin is done by a disposable third process**, not
xenoed itself, to avoid the classic bidirectional-pipe deadlock: writing
input to a subprocess while also trying to read its output back can leave
both ends' pipe buffers full with neither side able to make progress if
they're interleaved in one process. A separate writer process sidesteps
that entirely, at the cost of one extra `fork()` per invocation -- cheap,
and simpler than restructuring around non-blocking I/O for what's meant
to be an occasional, user-triggered action rather than something in a
hot path.

## Undo / redo

Whole-buffer snapshots, not per-edit diffs -- for the size of file this
editor targets, a full deep copy per undo step is cheap enough not to
matter, and it avoids an entire category of bugs a hand-rolled diff/patch
format could get subtly wrong. Granularity matches vim: `x`, `dd`, and `p`
are each their own undo step; an entire insert-mode session -- everything
typed between `i`/`a`/`o`/etc. and `Esc`, including any mid-session
selection-replace -- undoes as a single step, not one step per keystroke.
History is capped at 500 steps (oldest dropped first) so a very long
editing session can't grow memory use without bound; a normal edit
session will never come close to that.

One simplification worth knowing: undo doesn't track whether a given
snapshot happens to exactly match what's on disk, so the modified (`[+]`)
indicator conservatively stays set after an undo even if you've undone
your way back to the last-saved state. It'll only clear on an explicit
`:w`.

## Search

Genuinely outsourced to `grep -E`, not a hand-rolled regex engine: `/`
prompts for an extended-regex pattern, `Enter` jumps to the first match at
or after the cursor, and `n`/`N` repeat it forward/backward, wrapping
around (with a "search hit BOTTOM/TOP" status message, like vim) when
there's nowhere further to go. `Esc` while typing a pattern cancels it.

Search runs against the buffer's *current* content, not what's on disk --
consistent with copy/paste already working the same way. The pattern is
handed to grep as a literal argv entry (never through a shell), so nothing
in it -- quotes, backticks, semicolons -- can do anything but be regex; an
invalid pattern is reported as `E: invalid search pattern` rather than
crashing or silently matching nothing.

## Design notes

**Window management**: xenoed doesn't know or care about i3, tabs, splits,
or multiple buffers — it opens exactly one file per process and expects an
external window manager (i3, in practice) to own everything about how that
window is placed, resized, and switched between. The one concession to
that: `WM_CLASS` is set to `xenoed`/`Xenoed`, purely so external tooling
(e.g. a script querying i3's IPC tree) can identify xenoed's own windows
among everyone else's — xenoed itself never touches i3's IPC socket.

**Why fixed row height but proportional glyph widths?** Pango/HarfBuzz
handle horizontal shaping per line (so glyph advances are real, proportional
widths — no grid-snapping). Vertically, each line still occupies a fixed
`row_height` slot computed once from the font's ascent+descent, which keeps
scrolling and cursor math simple and gives an even line rhythm. This is the
same tradeoff most proportional-font editors make; true variable-height
lines would need to account for line-wrapping and per-line metrics, which
this MVP intentionally skips.

**Cursor**: a translucent block in normal mode (so the character underneath
stays legible), a thin bar in insert mode — its x position and width come
from `pango_layout_index_to_pos()`, so it's always exactly under the
right glyph regardless of that glyph's proportional width.

**Selection**: tracked as a single anchor point (`sel_anchor_line/col`) in
`Editor`; the moving endpoint is just whatever the cursor currently is, so
there's no separate "selection end" to keep in sync. `editor.c` stays free
of any notion of pixels or mouse coordinates — it only knows "move to this
line/col" and "is there a selection right now"; `main.c` does the pixel ↔
buffer-position translation for mouse events via `render_xy_to_pos()`
(built on `pango_layout_xy_to_index()`), and drives the same
`editor_selection_start()`/`cur_line`/`cur_col` primitives that Shift+arrow
uses internally.

**Text input**: uses `XOpenIM`/`XCreateIC` + `Xutf8LookupString` rather than
raw `XLookupString`, so multi-byte UTF-8 and IME/compose input are captured
correctly, not just Latin-1.

**Files**
- `src/buffer.{h,c}` — line-based text storage, file load/save, and
  replacing the whole buffer from external text (an external command's
  stdout). No X11/Pango dependency; unit-testable on its own.
- `src/editor.{h,c}` — modal state machine (Normal/Insert/Visual/Search)
  and all editing commands, including dispatching leader/Ctrl/plain
  keybindings and the `:` picker to `config.h`'s external-command table.
  Still has no OS/X11 dependency -- it takes an abstract
  `EditorSpecialKey` enum plus raw UTF-8 text (Ctrl+letters arrive as
  their ASCII control bytes `0x01`..`0x1A`), and reads `config.h` as
  compile-time data, not by calling out to anything -- so it doesn't know
  or care that X11 (or a subprocess, or dmenu) exists.
- `src/render.{h,c}` — Cairo + Pango drawing of the buffer, cursor, and
  status/command bar onto any Cairo surface.
- `src/main.c` — the only file that touches Xlib or spawns subprocesses:
  window/IC setup and the event loop, translating X events into calls on
  the two modules above, plus every `fork`/`exec` in the project (grep for
  search, dmenu for the right-click menu and `:` picker, and the actual
  scripts behind external commands).
- `src/utf8.h` — small header-only UTF-8 boundary-stepping helpers used by
  `editor.c` for correct cursor movement/editing on multi-byte characters.
- `src/config.h` — compile-time settings: the font, and the
  `XENOED_COMMANDS` external-command table.

## Tests

`tests/test_editor.c` exercises `buffer.c`/`editor.c` directly (insert,
`o`/`dd`/`x`, backspace-joins-lines, UTF-8 cursor movement over multi-byte
characters, save/load round-trip) without needing an X display:

```
make -C . 2>/dev/null; cc -std=c11 -D_POSIX_C_SOURCE=200809L \
  tests/test_editor.c src/buffer.c src/editor.c -o /tmp/test_editor \
  && /tmp/test_editor
```

## Known limitations (intentional, for a v1)

- No line wrapping (long lines just run off the right edge).
- No horizontal scrolling.
- `dd` is the only multi-key normal-mode command; no `dw`, counts, or
  registers (there's only ever one yank register, not vim's `"a`-`"z`).
- Visual mode is intentionally minimal: no case-changing or block-visual
  variants, and `v`/`V` don't extend to `d`/`y` composing with motions the
  way vim's operator-pending mode does (e.g. no `dw`) -- just enough for
  `y`/`d`/`x`/`p` to act on a characterwise or linewise span selected by
  hand.
- Search is find-and-jump only: no incremental "highlight as you type" and
  no case-insensitive toggle. The `:s` substitution is implemented in the
  editor itself with POSIX regexes -- it doesn't lean on `sed`/`perl` the
  way the "outsource to external tools" idea (the original motivating
  example for `CMD_INPUT_BUFFER`) once promised; for that, `!` plus a
  `sed`/`perl` one-liner still works.
- Mouse drag only starts a selection in insert mode; it doesn't enter
  visual mode from normal mode, so there's no click-and-drag equivalent of
  pressing `v` first. Also doesn't auto-scroll or grab the pointer, so
  dragging outside the window stops extending the selection until the
  pointer re-enters it.
- Both the right-click menu and the `:` command picker need a dmenu build
  with the embed patch to show at all, since xenoed always adds `-w` to
  the invocation; and neither positions itself at the click point (for the
  menu) or the cursor (for `:`) -- just wherever your dmenu's own flags
  (`-b`, `-l`, etc.) place it within the window. The right-click menu
  itself only has Copy/Cut/Paste so far.
- External commands (`config.h`) take a bare script path only, not a full
  command line -- `"tr a-z A-Z"` as a `script` value won't work, since
  that whole string is passed to `execvp` as a single program name to
  search for, not a program plus arguments; a script wanting extra flags
  needs to be an actual (even one-line) wrapper script. There's also no
  progress indicator for a slow `CMD_INPUT_BUFFER`/`CMD_INPUT_SELECTION`
  script -- xenoed blocks synchronously until it exits, so a slow one will
  make the editor appear to hang with no feedback that anything's running.
- No syntax highlighting.
