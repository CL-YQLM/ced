# ced

Terminal text editor written from scratch in C. No ncurses — raw `termios`
for keyboard input, VT100 escape sequences for drawing.

Honors project. I live in vim and nano and wanted to know what's actually
going on underneath.

## Build

POSIX only, since `termios` doesn't exist on Windows. WSL is fine.

```sh
make
./ced
```

## Keys

```
ctrl-q    quit
```

## Not done yet

Opening files, moving around, editing, saving. Right now it draws a screen
and exits cleanly.
