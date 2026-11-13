"""Check the C syntax highlighting by reading the colour escapes ced emits.

The other suite strips escape sequences to assert on text. This one does the
opposite: it parses "\x1b[<n>m" out of a drawn row and rebuilds the row as a
list of (colour, text) runs, so an assertion can say "int is green and 42 is
red" rather than just checking the characters arrived.

Colours come from editor_syntax_to_color:
  36 comment   33 keyword   32 type   35 string   31 number   34 search match
A run with colour None is whatever the terminal default is.
"""
import fcntl
import os
import pty
import re
import select
import struct
import sys
import termios
import time

BIN = os.environ.get("CED_BIN", "./ced")
ROWS, COLS = 24, 80
TEXTROWS = ROWS - 2
HIDE, SHOW = b"\x1b[?25l", b"\x1b[?25h"
COLOR_RE = re.compile(rb"\x1b\[([0-9;?]*)m")
ESC_RE = re.compile(rb"\x1b\[[0-9;?]*[a-zA-Z]")

COMMENT, KEYWORD, TYPE, STRING, NUMBER, MATCH = 36, 33, 32, 35, 31, 34

failures = []


def check(label, got, want):
    ok = got == want
    print("  %-42s %-30s %s" % (label, repr(got), "ok" if ok else "WANT " + repr(want)))
    if not ok:
        failures.append("%s: got %r want %r" % (label, got, want))
    return ok


def runs_of(line):
    """Split one drawn line into [(colour, text), ...]."""
    out = []
    color = None
    pos = 0
    for m in COLOR_RE.finditer(line):
        chunk = line[pos:m.start()]
        if chunk:
            out.append((color, ESC_RE.sub(b"", chunk).decode("utf-8", "replace")))
        arg = m.group(1).decode()
        if arg in ("", "0", "39"):
            color = None
        else:
            try:
                color = int(arg)
            except ValueError:
                color = None
        pos = m.end()
    chunk = line[pos:]
    if chunk:
        out.append((color, ESC_RE.sub(b"", chunk).decode("utf-8", "replace")))
    return [(c, t) for c, t in out if t != ""]


def colored(line, color):
    """Every piece of this line drawn in `color`, concatenated."""
    return "".join(t for c, t in runs_of(line) if c == color)


def plain(line):
    """The parts of the line left in the default colour."""
    return "".join(t for c, t in runs_of(line) if c is None)


class Ced:
    def __init__(self, args):
        self.master, self.slave = pty.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", ROWS, COLS, 0, 0))
        self.pid = os.fork()
        if self.pid == 0:
            os.close(self.master)
            os.setsid()
            fcntl.ioctl(self.slave, termios.TIOCSCTTY, 0)
            os.dup2(self.slave, 0)
            os.dup2(self.slave, 1)
            os.dup2(self.slave, 2)
            os.execvpe(BIN, [BIN] + args,
                       dict(os.environ, TERM="xterm-256color"))
            os._exit(127)
        self.wait_status = None
        time.sleep(0.4)
        self.raw = self.drain()

    def drain(self, timeout=0.35):
        buf = b""
        while True:
            r, _, _ = select.select([self.master], [], [], timeout)
            if not r:
                break
            try:
                chunk = os.read(self.master, 1 << 20)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            timeout = 0.12
        return buf

    def press(self, keys, settle=0.3):
        os.write(self.master, keys)
        time.sleep(settle)
        self.raw = self.drain()
        return self.raw

    def type(self, text):
        buf = b""
        for ch in text.encode("utf-8"):
            os.write(self.master, bytes([ch]))
            time.sleep(0.03)
            buf += self.drain(0.0)
        time.sleep(0.4)
        buf += self.drain()
        self.raw = buf
        return self.raw

    def frame(self):
        done = [HIDE + p for p in self.raw.split(HIDE)[1:] if SHOW in p]
        return done[-1] if done else b""

    def lines(self):
        return self.frame().split(b"\r\n")

    def row(self, n):
        ls = self.lines()
        return ls[n] if n < len(ls) else b""

    def text(self, n):
        return ESC_RE.sub(b"", self.row(n)).decode("utf-8", "replace")

    def anywhere(self, color):
        """Text drawn in `color` anywhere on screen.

        Accepting a search jumps the view so the match sits on the top row,
        so there is no fixed row to look at.
        """
        return "".join(colored(line, color) for line in self.lines())

    def statusbar(self):
        return self.text(TEXTROWS)

    def msgbar(self):
        return self.text(TEXTROWS + 1)

    def quit(self):
        for _ in range(5):
            done, st = os.waitpid(self.pid, os.WNOHANG)
            if done != 0:
                self.wait_status = st
                break
            os.write(self.master, b"\x11")
            time.sleep(0.3)
            self.drain(0.2)
        if self.wait_status is None:
            _, self.wait_status = os.waitpid(self.pid, 0)
        os.close(self.master)
        os.close(self.slave)
        return (os.WEXITSTATUS(self.wait_status)
                if os.WIFEXITED(self.wait_status) else -1)


SRC = "/tmp/ced_hl.c"
with open(SRC, "w") as f:
    f.write("/* block comment */\n")          # row 0
    f.write("int main(void)\n")               # row 1
    f.write("{\n")                            # row 2
    f.write("\tint integer = 42;\n")          # row 3
    f.write("\tchar *s = \"hello\";\n")       # row 4
    f.write("\t// line comment\n")            # row 5
    f.write("\treturn 0;\n")                  # row 6
    f.write("}\n")                            # row 7
    f.write("/* multi\n")                     # row 8
    f.write("   line\n")                      # row 9
    f.write("   comment */\n")                # row 10
    f.write("int after = 7;\n")               # row 11

PLAIN = "/tmp/ced_hl_plain.txt"
with open(PLAIN, "w") as f:
    f.write("int main(void) 42 \"str\"\n")


print("[1] filetype detection")
c = Ced([SRC])
check("status bar reports c", "c | " in c.statusbar(), True)
check("position still on the right",
      c.statusbar().rstrip().endswith("1/12"), True)

print("\n[2] single-line block comment")
check("whole line is comment coloured", colored(c.row(0), COMMENT),
      "/* block comment */")

print("\n[3] types and keywords are different colours")
check("int is a type", colored(c.row(1), TYPE), "intvoid")
check("nothing on that line is keyword coloured",
      colored(c.row(1), KEYWORD), "")
check("return is a keyword", colored(c.row(6), KEYWORD), "return")

print("\n[4] keyword boundaries")
check("int highlighted once, not inside integer",
      colored(c.row(3), TYPE), "int")
check("integer sits in the uncoloured text",
      "integer" in plain(c.row(3)), True)

print("\n[5] numbers")
check("42 is a number", colored(c.row(3), NUMBER), "42")
check("0 is a number", colored(c.row(6), NUMBER), "0")

print("\n[6] strings")
check("quoted text is a string", colored(c.row(4), STRING), '"hello"')
check("char is still a type", colored(c.row(4), TYPE), "char")

print("\n[7] line comment")
check("// to end of line", colored(c.row(5), COMMENT), "// line comment")

print("\n[8] block comment spanning three rows")
check("opening row", colored(c.row(8), COMMENT), "/* multi")
check("middle row entirely comment", colored(c.row(9), COMMENT), "   line")
check("closing row", colored(c.row(10), COMMENT), "   comment */")
check("line after the comment is live code again",
      colored(c.row(11), TYPE), "int")
check("and its number is highlighted", colored(c.row(11), NUMBER), "7")
c.quit()

print("\n[9] a .txt file gets no highlighting at all")
c = Ced([PLAIN])
check("status bar says no filetype", "no ft | " in c.statusbar(), True)
check("no type colour", colored(c.row(0), TYPE), "")
check("no number colour", colored(c.row(0), NUMBER), "")
check("no string colour", colored(c.row(0), STRING), "")
check("text is still correct", c.text(0), 'int main(void) 42 "str"')
c.quit()

print("\n[10] search match is highlighted")
c = Ced([SRC])
c.press(b"/")
c.type("hello")
check("match coloured while typing", c.anywhere(MATCH), "hello")
c.press(b"\r")
check("still highlighted after accepting", c.anywhere(MATCH), "hello")
c.quit()

print("\n[10b] ESC clears the match highlight")
c = Ced([SRC])
c.press(b"/")
c.type("hello")
check("highlighted while searching", c.anywhere(MATCH), "hello")
c.press(b"\x1b")
check("cleared after ESC", c.anywhere(MATCH), "")
check("string colour is back on its row", colored(c.row(4), STRING), '"hello"')
c.quit()

print("\n[11] typing a comment opener cascades down the file")
c = Ced([SRC])
check("row 2 starts as plain code", colored(c.row(2), COMMENT), "")
c.press(b"G")           # last line
c.press(b"o")           # open a line below, into insert mode
c.type("/* opened")
check("the new line is a comment", COMMENT in [x for x, _ in runs_of(c.row(12))],
      True)
c.press(b"\x1b")
c.quit()

print("\n[12] block comments do not nest: the first */ closes one")
c = Ced([SRC])
c.type("gg")
c.press(b"O")           # blank line above row 0, lands in insert mode
c.type("/*")
check("row 1 is swallowed whole", colored(c.row(1), COMMENT),
      "/* block comment */")
check("its own */ closes the comment", colored(c.row(2), COMMENT), "")
check("so row 2 is live code", colored(c.row(2), TYPE), "intvoid")
c.press(b"\x1b")
c.quit()

print("\n[13] opening a comment cascades until something closes it")
c = Ced([SRC])
c.press(b"j")
c.press(b"j")           # row 2, the opening brace
c.press(b"I")
c.type("/*")
check("row 3 turned into comment", colored(c.row(3), COMMENT) != "", True)
check("row 4 turned into comment", colored(c.row(4), COMMENT) != "", True)
check("row 6 turned into comment", colored(c.row(6), COMMENT) != "", True)
check("row 4 lost its string colour", colored(c.row(4), STRING), "")
check("row 11 is code again, past the closing */",
      colored(c.row(11), TYPE), "int")
check("and its number is back", colored(c.row(11), NUMBER), "7")
c.press(b"\x1b")
c.quit()


print()
if failures:
    print("FAILURES (%d):" % len(failures))
    for f in failures:
        print("  -", f)
    print("\nRESULT: FAIL")
    sys.exit(1)
print("RESULT: PASS")
