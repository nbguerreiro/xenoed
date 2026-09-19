# TODO

- [x] 1. Insert-mode clipboard shortcuts
- [x] 2. Vim commands
- [x] 3. x command should also copy
- [x] 4. Increase line spacing to about 120% of its current value, with a configurable variable in `config.h`
- [x] 5. Refine the status bar: use `XENOED_FONT_BAR`, right-align the text, show filename plus line/total-lines and column, and omit mode/Ln/Col labels
- [x] 6. Add a `.` command to repeat the last used command
- [x] 7. Accept an optional line-number argument at startup and jump immediately to that line
- [x] 8. Replace the editor's search prompt with dmenu for a more consistent command/search interface
- [x] 9. Make search case-insensitive
- [x] 10. 'V' for selecting lines
- [x] 11. mouse scroll
- [x] 12. ! command
- [x] 13. remember commands
- [x] 14. : should work on selection (visual mode)
- [x] 15. Search and replace

- [x] 16. XENOED_COMMANDS should accept commands starting with 'leader', 'control', or just plain letters.
  { "com1", "external_com", '<leader>e', CMD_INPUT_BUFFER },
  { "com2", "external_com", '<c>e', CMD_INPUT_BUFFER },
  { "com3", "external_com", 'e', CMD_INPUT_BUFFER },
  { "com4", "external_com", 0, CMD_INPUT_BUFFER },

- [x] 17. XENOED_COMMANDS should work with 'oneliners'
    for example:
    { "lowercase",   "perl -pe '$_ = lc'", XENOED_KEY_NONE,   CMD_INPUT_SELECTION },
    { "format_table",   "column -t -s '|' -o '|'", XENOED_KEY_NONE,   CMD_INPUT_SELECTION },

- [x] 18. send word under cursor to external command
- [x] 19. block cursor should be a hollow rectangle when window is not in focus
- [x] 20. marks
- [x] 22. XENOED_KEY_CTRL(']') should work
- [x] 23. window id should also be sent to external commands
- [x] 24. commands: r, D
- [x] 25. on visual mode, while selecting text, I should be able to use gg and G.
- [x] 26. should be able to select with mouse while on normal mode
- [x] 27. goto line
- [x] 28. small margin at top
- [ ] 29. the status bar should indicate if file was modified in-editor, and if it was modified on disk. 
- [ ] 30. Command to reload from disk.
- [ ] 31. in the status bar, filenames like "/home/fx/..." can be shortened to "~/..."



