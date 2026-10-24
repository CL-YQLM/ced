# ced

Terminal text editor written from scratch in C. No ncurses — raw `termios`
for keyboard input, VT100 escape sequences for drawing.

Honors project. I live in vim and nano and wanted to know what's actually
going on underneath.

## Build

POSIX only, since `termios` doesn't exist on Windows. WSL is fine.

```sh
make
./ced file.c
```

A filename that doesn't exist yet opens an empty buffer and gets created on
the first save.

## Keys

```
arrows, home/end, pgup/pgdn   move around
enter, backspace, delete      edit
ctrl-s                        save
ctrl-q                        quit — four presses if there are unsaved edits
```

## Implementation notes

Every line is kept twice: `chars` is the bytes on disk, `render` is the same
line with tabs expanded to 8-column stops. That's why the cursor needs two
column numbers — `cx` indexes `chars`, `rx` indexes `render`, and the two
only agree on lines without tabs. Horizontal scrolling has to work off `rx`
or indented lines jump by the wrong amount.

Inserting a row goes through `realloc`, so anything still holding an
`erow *` across one is holding a dangling pointer. Splitting a line walks
straight into this: insert the new row first, then look the old one up again
before truncating it.

Save truncates in place instead of writing a temp file and renaming, so the
inode survives and permissions, hard links and symlinks come through
unchanged. The cost is a crash window between `ftruncate` and `write`.

Every line is written with a trailing newline, and CRs are stripped on load,
so a CRLF file saves back as LF.

## Not done yet

- status bar, incremental search, vim normal/insert modes
- syntax highlighting for C
- SIGWINCH — resizing the terminal mid-session doesn't re-measure
