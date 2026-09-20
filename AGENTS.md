# xenoed

Single-binary modal (vi-like) X11 editor in C: Xlib + Cairo + Pango, one
file/buffer per process, no line wrapping, no config files. Debian
deps: `build-essential pkg-config libx11-dev libcairo2-dev libpango1.0-dev`.

## Build & verify

- `make` builds `./xenoed [file] [line]`; variants `make debug`, `make sanitize`,
  `make fanalyzer` all build the binary (one `cc` invocation, output teed to
  `out.log`). `dpkg-buildflags` is not installed here, so `make` prints
  "Permission denied" for it and just skips those hardening flags — harmless.
- `make lint` = `cppcheck --enable=warning,style,performance,portability
  --error-exitcode=1` over `src/*.c`. It currently exits 1 on a pre-existing
  finding at `src/buffer.c:58` (calloc-deref style check in `buffer_new`);
  don't treat that as a regression you introduced.
- Tests are headless (no X display needed):
  - `make test-cmdhist` works.
  - `make test-cmdline` (the XENOED_COMMANDS one-liner tokenizer) works.
  - `make test-render` (todo #35 horizontal scroll + `render_xy_to_pos`
    hit-testing, on an in-memory cairo surface) works. It also passes under
    ASan/UBSan (`cc -fsanitize=address,undefined tests/test_render.c
    src/render.c src/editor.c src/buffer.c $(pkg-config --cflags --libs
    cairo pangocairo) -lX11`).
  - `make test` (repeat tests) works as far as linking goes (the `-lX11`
    fix described two lines down has been applied), but it **still fails at
    runtime**: `tests/test_repeat.c:95` asserts `.` replays the FIRST insert
    (`i`+`X`) while `begin_recording()` in src/repeat.c clears prior events,
    so the LAST edit (`o`+`abc`) wins — the test expectation contradicts the
    vim-like "repeat last change" semantics. Unknown whether the test or the
    code is "right" (todo #33 candidate). The README's manual `cc` editor
    suite (`tests/test_editor.c`) links but has the pre-existing config.h
    sample-drift crash at test 98 (see below).
  - The README's manual `cc -std=c11 -D_POSIX_C_SOURCE=200809L
    tests/test_editor.c src/buffer.c src/editor.c -o /tmp/test_editor &&
    /tmp/test_editor` works and is the broadest editor/buffer suite.
- CI (`.github/workflows/c-cpp.yml`) builds/lints/sanitizes only — it never runs
  `make test`.

## Architecture — read before editing

- Layering (keep it clean): `src/buffer.c` X11-free storage; `src/editor.c`
  mode state machine, X11-free too; `src/main.c` owns the X11 display, event
  loop, and **all** subprocesses (grep search, `${DMENU:-dmenu} -w ...`
  pickers, filter pipes). `src/render.c` Cairo/Pango drawing; `src/utf8.h`
  header-only boundary helpers; `src/cmdhist.c` most-used-command history in
  `$XDG_STATE_HOME/xenoed/` (fallback `~/.local/state/xenoed/`);
  `src/cmdline.c` is a small X11-free tokenizer that splits a shell-style
  command line (single/double quotes, backslash escapes, no expansion) into
  an argv -- what lets `XENOED_COMMANDS` entries be one-liners.
  Font zoom (todo #34) follows the same "view-only ops live in main.c"
  rule as wheel-scroll/window-resize: `Ctrl+=`/`Ctrl+-`/`Ctrl+wheel` are
  intercepted at the X11 layer (never reaching editor.c/repeat.c), step the
  `PangoFontDescription` size via `zoom_font()` and re-run `render_init()`.
  Horizontal scroll (todo #35) is the same idea but one layer down: it
  lives in render.c, because advance widths need Pango. `ed->left_col` (a
  byte offset per line) is the horizontal `top_line`, kept on the Editor
  struct but touched only by render.c; `render_frame`'s
  `editor_ensure_hscroll()` nudges it to keep the cursor in view and every
  row is drawn clipped/`shift`ed to the text area; `render_hscroll_by()`
  is the Shift+wheel analogue of `editor_scroll_by()`; `render_xy_to_pos`
  adds back the scrolled-px so mouse hit-testing stays byte-accurate.
  `tests/test_render.c` covers all of it headlessly (image surface, no X).
  Focus restoration after dmenu (todo #37): `run_dmenu`'s single
  `XSetInputFocus` after `waitpid` raced the WM processing dmenu's unmap
  asynchronously, so focus was *sometimes* lost ("hollow cursor, keystrokes
  go elsewhere"). `restore_focus()` in main.c (used by `run_dmenu`) and its
  self-contained twin in repeat.c (used by the `/` search picker, which
  previously never restored focus at all) mirror dmenu's own `grabfocus()`:
  send `_NET_ACTIVE_WINDOW` to root for EWMH WMs, then loop
  `XGetInputFocus`/`XSetInputFocus` (10ms steps, bounded ~0.5s) until the
  server confirms the editor window has focus. Deliberately NOT applied to
  the CMD_INPUT_NONE/detached spawn paths — the user may have launched a
  program and we must not steal focus back from it. Exit is the mirror case:
  `main.c` grabs the focused window (`prev_focus`) right after XOpenDisplay,
  before the editor window maps, and `restore_previous_focus()` hands focus
  back to it (the launching terminal) just before `XDestroyWindow` — without
  it, a click-to-focus WM with no focus history leaves nothing focused after
  xenoed quits. It validates the target still exists and is mapped and
  swallows BadWindow/BadMatch via a temporary `XSetErrorHandler`.
- **Biggest gotcha**: the `.` repeat command and the `/` search prompt are NOT
  in `editor.c` — they live in `src/repeat.c` as ELF linker wrappers. Both the
  binary and tests link with `-Wl,--wrap=editor_init -Wl,--wrap=editor_handle_key`;
  repeat.c defines `__wrap_editor_init`/`__wrap_editor_handle_key` and forwards to
  the `__real_*` symbols. The wrapper intercepts, before editor.c sees them:
  `/` (opens an embedded dmenu search), `i a A I o O` then `x`/`dd` (records for
  replay), `.` (replays), `Esc` from insert (stops recording). Editor.c's own
  inline `MODE_SEARCH` `/` bar still exists but is only reached in builds without
  the wrap — which is exactly why `tests/test_editor.c`'s search tests pass while
  the real binary uses dmenu.
- Consequence: a hand-rolled compile of `editor.c` + `main.c` without
  `repeat.c` and the two `--wrap` flags silently loses `.` and dmenu search.
  Any new test binary must reproduce the wrap flags and link repeat.c's X11 deps.
- New normal-mode keybindings belong in `editor.c` `handle_normal()`; remember
  repeat.c shadows the chars listed above. Add `:` commands to both
  `editor_run_command()` and `editor_command_names()` (by design, not generated).
- `CMD_INPUT_WORD` (todo #18) is the third "input supplied by the editor"
  command kind alongside `CMD_INPUT_BUFFER`/`CMD_INPUT_SELECTION`: the word
  under the cursor -- a maximal run of non-whitespace bytes, whitespace
  cursor falling back left-then-right -- is piped to the script's stdin and
  stdout (on exit 0) replaces just that word. The word boundary helpers live
  in `editor.c` (`word_bounds`, `editor_get_word_text`,
  `editor_replace_word_text`); `main.c` `run_user_command()` uses them the
  same way it uses `editor_get_selection_text`/`editor_paste_text`.
  `editor_request_user_command()` refuses WORD commands from visual mode and
  from lines with no word, mirroring SELECTION's "needs a selection" guard.
  `CMD_INPUT_INSERT` (the insert-mode family member, sample: the `insert_date`
  Ctrl+d entry) is the sibling pair to `CMD_INPUT_NONE`'s "no input": it also
  passes no stdin to the script, but on exit 0 its stdout is inserted at the
  cursor as one undo step by `editor_insert_at_cursor()` (main.c strips the
  trailing newline(s) first so `date`/`echo` land inline).
  `CMD_INPUT_INSERT_WORD` (sample: the `complete_word` Ctrl+n entry) is the
  completion sibling of `CMD_INPUT_WORD` in the insert family: it pipes the
  same word span to stdin but main.c lands the replacement via
  `editor_replace_word_text_at_end()` -- the replace-word twin that leaves
  the cursor AFTER the replacement -- so the editor stays in insert mode and
  typing continues (trailing newline(s) stripped like INSERT). Inside the editor,
  insert-mode dispatch is `editor_dispatch_insert_command()`, the sibling of
  `editor_dispatch_command()`: it only fires Ctrl-bound commands whose input
  kind is INSERT, INSERT_WORD or NONE (plain keys are text, the leader key is the spacebar,
  so neither is dispatchable mid-typing; `'\t'`/Ctrl+I is excluded to keep Tab
  as text). `Ctrl+s` is reserved before that dispatch (todo #36): it saves
  via `editor_run_command(ed, "w")` -- the `:w` path, so `b->dirty` clears
  (the status bar's [+] disappears on the next redraw), a file changed on
  disk defers to the same overwrite-or-reload conflict main.c resolves, and
  insert mode is left untouched; 's' is in the `insert_ctrl` warnings list
  so a config binding to Ctrl+s is flagged. `editor_request_user_command()` matches WORD's mode guard by
  refusing everything except INSERT/INSERT_WORD/NONE from MODE_INSERT. `editor.c`'s
  `editor_insert_text()` takes a `force_charwise` flag (set by
  `editor_insert_at_cursor`) that disables the trailing-'\n' linewise heuristic
  so inserted command output splits lines exactly as typing would.
- Every external command (and the `!` filter) inherits the editor's X window
  id as `$WINDOWID` — `main.c` `setenv`s it once after `XCreateSimpleWindow`,
  before the event loop, so all `fork`/`exec` children get it regardless of
  argv spelling (todo #23). Single-word scripts still get only the filename
  as argv[1]; the window id is deliberately env-only, not an extra argv slot.
- `XENOED_COMMANDS` keybindings are `XenoedKey { XenoedKeyModifier mod; char key; }`
  with four spellings in `config.h`: `XENOED_KEY_LEADER(k)` (SPACE then k),
  `XENOED_KEY_CTRL(k)` (Ctrl+k), `XENOED_KEY_PLAIN(k)`, and `XENOED_KEY_NONE`
  (`:` picker only). Ctrl+letter arrives at editor.c as its ASCII control byte
  (`Ctrl+A`=`0x01`..`0x1A`) — `main.c` synthesizes it from the keysym in the
  `KeyPress` handler, editor.c converts it back in `ctrl_character_to_letter()`.
  Plain bindings are checked *before* `handle_normal`/`handle_visual`'s built-in
  switch (so they can shadow builtins; `editor_init` warns about that and about
  duplicates); leader-pending consumes the next key before plain dispatch.
- `:s/pattern/repl/[g]` is an in-process POSIX regex substitution (no grep/sed
  subprocess). Implemented in `editor.c` as `sub_parse()` + `sub_line()` +
  `sub_whole_buffer()` / `sub_selection()`. Whole-buffer in normal mode;
  selection in visual mode (`:` bound in `handle_visual`). The `%s` prefix
  forces whole-buffer scope even in visual mode.
- `src/config.h` is the compile-time config: font, line spacing, scroll
  lines, font-zoom step/range, and the `XENOED_COMMANDS` external-command
  table. The README's
  `make EXTRA_CFLAGS=...` override is **stale** — the Makefile never references
  `EXTRA_CFLAGS`; change defaults in config.h (or carefully via `make CFLAGS=...`,
  which drops the pkg-config include/lib flags). `XENOED_COMMANDS` script
  entries are tokenized by src/cmdline.c and `execvp`'d: a bare single-word
  program name (like "indent.sh") receives the filename as argv[1], but a
  multi-word one-liner ("perl -pe '$_ = lc'", "column -t -s '|' -o '|'") runs
  exactly as written and reads stdin — the filename is NOT appended, so
  filters keep reading the piped buffer/selection.
  README's claims that main.c is the only X11/multi-OS file and `Sans 12` font
  default are both stale — trust config.h / repeat.c.

## Git gotchas

- `.gitignore` is a whitelist (`*` then `!` rules): effectively only `Makefile`,
  `README.md`, `src/*.{c,h}`, `tests/*.c`, the CI workflow, `TODO.md`, and
  `.gitignore` are tracked. `plan.md`, `tags`, `out.log`, `compile_commands.json`
  are untracked, and any new top-level file is silently ignored — add a `!`
  rule to `.gitignore` or put it under `src/`/`tests/`.
- Commit messages are date-stamps, e.g. `2026-09-16_11:48`.