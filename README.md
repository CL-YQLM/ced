# ced

Terminal text editor written from scratch in C. No ncurses: raw `termios`
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

## Tests

The suites drive ced on a real pty and read the frames back off it, so they
need python3 as well.

```sh
make test        # behaviour, then highlighting
make test-asan   # the memory paths, under ASan and UBSan
```

Run test-asan from a native filesystem. LeakSanitizer cannot symbolize a
binary on a mounted Windows drive, and it hangs printing DEADLYSIGNAL
rather than saying so.

## Keys

Starts in normal mode, like vim.

```
normal mode
  h j k l            move
  w b                word forward / back
  0 $                line start / end
  gg G               file start / end
  i a I A            insert at cursor / after it / line start / line end
  o O                open a line below / above
  x dd               delete a character / a line
  / n N              search, next match, previous match
  :w :q :q! :wq      save, quit, quit anyway, save and quit
  esc                leave insert mode

either mode
  arrows             move
  home end           line start / end
  pgup pgdn          move a screenful
  enter              split the line        (insert mode)
  backspace delete   delete                (insert mode)
  ctrl-s             save
  ctrl-f             search
  ctrl-q             quit (four presses if there are unsaved edits)
```

Saving a buffer that has no filename prompts for one.

## Implementation notes

Every line is kept twice: `chars` is the bytes on disk, `render` is the same
line with tabs expanded to 8-column stops. That's why the cursor needs two
column numbers: `cx` indexes `chars`, `rx` indexes `render`, and the two
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

Search, `:` commands and the save-as prompt all run through one function that
reads a line on the message bar. It takes an optional callback that fires on
every keystroke, which is the whole trick behind search jumping as you type:
the callback moves the cursor, and ESC puts it back where it started.

Highlighting stores one colour code per byte of `render`, not of `chars`,
because that's what gets drawn. Block comments are the awkward part: whether
a line starts inside one depends on where the line above finished, so each
row remembers whether it ended with a comment still open, and a row whose
answer changes pushes a rescan down the file. Type `/*` at the top of a file
and everything below it goes cyan until something closes it, and because C
comments don't nest, the first `*/` wins.

## Not done yet

- SIGWINCH: resizing the terminal mid-session doesn't re-measure
- C is the only filetype in the table
- search only finds the first match on a line, so `n` steps line by line
- normal mode lets the cursor sit one past the last character; vim clamps it
- scrolled right on a long line, stepping onto a short one scrolls the window
  to the cursor and leaves that line off to the left
