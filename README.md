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

## Keys

```
arrows, home/end, pgup/pgdn   move around
ctrl-q                        quit
```

## Implementation notes

Every line is kept twice: `chars` is the bytes on disk, `render` is the same
line with tabs expanded to 8-column stops. That's why the cursor needs two
column numbers — `cx` indexes `chars`, `rx` indexes `render`, and the two
only agree on lines without tabs. Horizontal scrolling has to work off `rx`
or indented lines jump by the wrong amount.

## Not done yet

Editing and saving. Right now it's read-only.
