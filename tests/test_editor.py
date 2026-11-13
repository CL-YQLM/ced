"""Drive ./ced inside a real pty and assert what it drew and what it saved.

Raw mode only means anything on a tty, so the editor runs on a pty slave and
every frame it writes is captured on the master side. The last
"\x1b[<row>;<col>H" of a frame is ced's own idea of where the cursor is, which
makes it the ground truth for every cursor assertion below.

Frames have to be split apart before anything is read out of them. Every frame
opens with the hide-cursor sequence, so that is the delimiter; splitting a
multi-frame capture on "\r\n" alone would glue one frame's bottom line onto
the next frame's top line.

Screen geometry: on a 24-row terminal ced keeps 22 rows for text and gives the
last two to the status bar and the message bar, so a frame splits into 22 text
rows, then the status bar at index 22 and the message bar at index 23.

ced starts in NORMAL mode, so anything that types text has to send `i` first.

Assertions marked "known:" pin down rough edges I know about and decided to
leave in, so a change in that behaviour shows up here as a failure rather
than going unnoticed.
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
STATUSBAR, MSGBAR = TEXTROWS, TEXTROWS + 1
HIDE, SHOW = b"\x1b[?25l", b"\x1b[?25h"
CURSOR_RE = re.compile(rb"\x1b\[(\d+);(\d+)H")
ESC_RE = re.compile(rb"\x1b\[[0-9;?]*[a-zA-Z]")
CED_QUIT_TIMES = 3

ARROW_UP = b"\x1b[A"
ARROW_DOWN = b"\x1b[B"
ARROW_RIGHT = b"\x1b[C"
ARROW_LEFT = b"\x1b[D"
HOME = b"\x1b[H"
END = b"\x1b[F"
PAGE_UP = b"\x1b[5~"
PAGE_DOWN = b"\x1b[6~"
DEL = b"\x1b[3~"
ENTER = b"\r"
BACKSPACE = b"\x7f"
ESC = b"\x1b"
CTRL_Q = b"\x11"
CTRL_S = b"\x13"
CTRL_F = b"\x06"

failures = []


def check(label, got, want):
    ok = got == want
    print("  %-40s %-26s %s" % (label, repr(got), "ok" if ok else "WANT " + repr(want)))
    if not ok:
        failures.append("%s: got %r want %r" % (label, got, want))
    return ok


class Ced:
    def __init__(self, args, rows=ROWS, cols=COLS):
        self.master, self.slave = pty.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))
        self.termios_before = termios.tcgetattr(self.slave)
        self.pid = os.fork()
        if self.pid == 0:
            os.close(self.master)
            os.setsid()
            fcntl.ioctl(self.slave, termios.TIOCSCTTY, 0)
            os.dup2(self.slave, 0)
            os.dup2(self.slave, 1)
            os.dup2(self.slave, 2)
            if self.slave > 2:
                os.close(self.slave)
            os.execvpe(BIN, [BIN] + args,
                       dict(os.environ, TERM="xterm-256color"))
            os._exit(127)
        self.wait_status = None
        time.sleep(0.35)
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

    def press(self, keys, settle=0.25):
        os.write(self.master, keys)
        time.sleep(settle)
        self.raw = self.drain()
        return self.raw

    def repeat(self, keys, n):
        """Send n keypresses, keeping every byte that comes back.

        Draining inside the loop is what stops the pty filling up and
        wedging ced inside write(), but the bytes have to be accumulated
        rather than thrown away or the last frame is gone before frame()
        ever sees it.
        """
        buf = b""
        for _ in range(n):
            os.write(self.master, keys)
            time.sleep(0.03)
            buf += self.drain(0.0)
        time.sleep(0.45)
        buf += self.drain()
        self.raw = buf
        return self.raw

    def type(self, text):
        buf = b""
        for ch in text.encode("utf-8"):
            os.write(self.master, bytes([ch]))
            time.sleep(0.025)
            buf += self.drain(0.0)
        time.sleep(0.4)
        buf += self.drain()
        self.raw = buf
        return self.raw

    def insert(self, text):
        """Enter insert mode and type, since ced boots into normal mode."""
        self.press(b"i")
        return self.type(text)

    def frame(self):
        done = [HIDE + p for p in self.raw.split(HIDE)[1:] if SHOW in p]
        return done[-1] if done else b""

    def cursor(self):
        hits = CURSOR_RE.findall(self.frame())
        return (int(hits[-1][0]), int(hits[-1][1])) if hits else None

    def rows_text(self):
        return [ESC_RE.sub(b"", line).decode("utf-8", "replace")
                for line in self.frame().split(b"\r\n")]

    def status(self):
        rows = self.rows_text()
        return rows[STATUSBAR] if len(rows) > STATUSBAR else None

    def msgbar(self):
        rows = self.rows_text()
        return rows[MSGBAR] if len(rows) > MSGBAR else None

    def raw_mode_on(self):
        attrs = termios.tcgetattr(self.slave)
        return not (attrs[3] & termios.ICANON) and not (attrs[3] & termios.ECHO)

    def _reap(self, block):
        """True while the child is still running.

        waitpid reaps an exited child and its status can only be collected
        once, so it is stashed here; polling with WNOHANG and then waiting
        again would fail with ECHILD.
        """
        if self.wait_status is not None:
            return False
        pid, status = os.waitpid(self.pid, 0 if block else os.WNOHANG)
        if pid == 0:
            return True
        self.wait_status = status
        return False

    def alive(self):
        return self._reap(block=False)

    def exit_code(self):
        self._reap(block=True)
        if os.WIFEXITED(self.wait_status):
            return os.WEXITSTATUS(self.wait_status)
        return -os.WTERMSIG(self.wait_status)

    def quit(self, force=True):
        os.write(self.master, CTRL_Q)
        time.sleep(0.3)
        tail = self.drain(0.2)
        if force:
            for _ in range(CED_QUIT_TIMES):
                if not self.alive():
                    break
                os.write(self.master, CTRL_Q)
                time.sleep(0.25)
                tail += self.drain(0.2)
        code = self.exit_code()
        restored = termios.tcgetattr(self.slave)[:4] == self.termios_before[:4]
        os.close(self.master)
        os.close(self.slave)
        return code, restored, tail


def write_file(path, data):
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(path, mode) as fh:
        fh.write(data)
    return path


def read_file(path):
    with open(path, "rb") as fh:
        return fh.read()


# ---------- fixtures ----------
#
# 60 lines, so row index i holds "line%02d" % (i + 1). Index 2 is indented
# with a real tab (size 9, rsize 16) and index 4 is 300 characters wide.

FIX = "/tmp/ced_fixture.txt"
with open(FIX, "w") as f:
    for i in range(1, 61):
        if i == 3:
            f.write("\tindented\n")
        elif i == 5:
            f.write("x" * 300 + "\n")
        else:
            f.write("line%02d\n" % i)

EMPTY = write_file("/tmp/ced_empty.txt", "")


# ---------- terminal ----------

print("[1] no argument -> greeting, bars, clean exit")
c = Ced([])
rows = c.rows_text()
check("text rows + 2 bars", len(rows), ROWS)
check("greeting present", any("ced -- version" in r for r in rows[:TEXTROWS]), True)
check("status bar names no file", "[no name]" in (c.status() or ""), True)
check("message bar shows help", "ESC = normal" in (c.msgbar() or ""), True)
check("cursor at top left", c.cursor(), (1, 1))
check("raw mode live", c.raw_mode_on(), True)
code, restored, tail = c.quit()
check("exit code", code, 0)
check("termios restored", restored, True)
check("cleared on quit", b"\x1b[2J" in tail, True)

print("\n[1b] Ctrl-C and Ctrl-Z must not kill it")
c = Ced([FIX])
alive = []
for key in (b"\x03", b"\x1a"):
    os.write(c.master, key)
    time.sleep(0.25)
    c.drain(0.15)
    alive.append(c.alive())
check("survives both", alive, [True, True])
code, _, _ = c.quit()
check("Ctrl-Q exits 0", code, 0)


# ---------- buffer, cursor and scrolling ----------

print("\n[2] open 60-line file -> 22 text rows, no greeting")
c = Ced([FIX])
rows = c.rows_text()
check("top line", rows[0], "line01")
check("greeting suppressed",
      any("ced -- version" in r for r in rows[:TEXTROWS]), False)
check("tab expanded to 8 columns", rows[2], " " * 8 + "indented")
check("long line clipped to width", len(rows[4]), COLS)
check("bottom text row", rows[TEXTROWS - 1], "line22")
check("cursor at top left", c.cursor(), (1, 1))

print("\n[3] 30x down -> cy=30, rowoff=9, rows 10..31 visible")
c.repeat(ARROW_DOWN, 30)
rows = c.rows_text()
check("top visible line", rows[0], "line10")
check("bottom visible line", rows[TEXTROWS - 1], "line31")
check("cursor pinned to last text row", c.cursor(), (TEXTROWS, 1))

print("\n[3b] page up -> back to the top")
c.press(PAGE_UP)
check("top visible line", c.rows_text()[0], "line01")
check("cursor", c.cursor(), (1, 1))

print("\n[4] tab line: End lands on a render column, not a char index")
c = Ced([FIX])
c.repeat(ARROW_DOWN, 2)
check("line start", c.cursor(), (3, 1))
c.press(ARROW_RIGHT)
check("one char past the tab", c.cursor(), (3, 9))
c.press(END)
check("End on tab line", c.cursor(), (3, 17))

print("\n[5] 300-char line: End scrolls sideways")
c = Ced([FIX])
c.repeat(ARROW_DOWN, 4)
c.press(END)
rows = c.rows_text()
check("cursor clamped to last column", c.cursor(), (5, 80))
check("visible slice length", len(rows[4]), 79)
check("short rows scrolled off", rows[0], "")
c.press(HOME)
check("Home scrolls back", c.cursor(), (5, 1))

print("\n[6] long line -> short line clamps cx")
c = Ced([FIX])
c.repeat(ARROW_DOWN, 4)
c.press(END)
c.press(ARROW_DOWN)
check("known: window snaps to rx", c.cursor(), (6, 1))
check("known: short line renders empty", c.rows_text()[5], "")

print("\n[7] cursor may sit one row past EOF, never further")
c = Ced([FIX])
c.repeat(ARROW_DOWN, 80)
rows = c.rows_text()
check("last text row", rows[TEXTROWS - 2], "line60")
check("virtual row past EOF", rows[TEXTROWS - 1], "~")
check("cursor on the virtual row", c.cursor(), (TEXTROWS, 1))
code, _, _ = c.quit()
check("exit code", code, 0)


# ---------- editing and saving ----------

print("\n[8] insert characters, then save and byte-compare")
path = write_file("/tmp/ced_e1.txt", "abc\ndef\n")
c = Ced([path])
c.insert("XY")
check("screen shows the insert", c.rows_text()[0], "XYabc")
check("cursor advanced", c.cursor(), (1, 3))
c.press(CTRL_S)
check("save reported bytes", c.msgbar(), "10 bytes written to disk")
check("file on disk", read_file(path), b"XYabc\ndef\n")
code, _, _ = c.quit(force=False)
check("clean quit after save", code, 0)

print("\n[9] Enter splits a line at the cursor")
path = write_file("/tmp/ced_e2.txt", "hello\n")
c = Ced([path])
c.press(b"i")
c.repeat(ARROW_RIGHT, 2)
c.press(ENTER)
rows = c.rows_text()
check("first half", rows[0], "he")
check("second half", rows[1], "llo")
check("cursor at start of new line", c.cursor(), (2, 1))
c.press(CTRL_S)
check("file on disk", read_file(path), b"he\nllo\n")
c.quit(force=False)

print("\n[10] Backspace at column 0 joins onto the line above")
path = write_file("/tmp/ced_e3.txt", "ab\ncd\n")
c = Ced([path])
c.press(b"i")
c.press(ARROW_DOWN)
c.press(HOME)
c.press(BACKSPACE)
check("lines joined", c.rows_text()[0], "abcd")
check("cursor at the join", c.cursor(), (1, 3))
c.press(CTRL_S)
check("file on disk", read_file(path), b"abcd\n")
c.quit(force=False)

print("\n[11] Delete removes the character to the right")
path = write_file("/tmp/ced_e5.txt", "abcd\n")
c = Ced([path])
c.press(b"i")
c.press(DEL)
check("screen", c.rows_text()[0], "bcd")
c.press(CTRL_S)
check("file on disk", read_file(path), b"bcd\n")
c.quit(force=False)

print("\n[12] unsaved changes block Ctrl-Q until pressed 4 times")
path = write_file("/tmp/ced_e7.txt", "x\n")
c = Ced([path])
c.insert("y")
survived = []
for _ in range(CED_QUIT_TIMES):
    c.press(CTRL_Q)
    survived.append(c.alive())
check("alive through 3 warnings", survived, [True, True, True])
check("warning names the key", "Ctrl-Q" in (c.msgbar() or ""), True)
os.write(c.master, CTRL_Q)
time.sleep(0.4)
c.drain(0.2)
check("4th press quits", c.alive(), False)
check("exit code", c.exit_code(), 0)
check("file left untouched", read_file(path), b"x\n")

print("\n[13] a name that is not on disk starts an empty buffer")
path = "/tmp/ced_brand_new.txt"
if os.path.exists(path):
    os.unlink(path)
c = Ced([path])
check("did not die on missing file", c.alive(), True)
check("empty buffer, tildes", c.rows_text()[0], "~")
c.insert("hi")
c.press(CTRL_S)
check("save created the file", os.path.exists(path), True)
check("file contents", read_file(path), b"hi\n")
c.quit(force=False)

print("\n[14] line endings on save")
path = write_file("/tmp/ced_e10.txt", b"x\r\ny\r\n")
c = Ced([path])
check("CR stripped on load", c.rows_text()[0], "x")
c.press(CTRL_S)
check("CRLF normalised to LF", read_file(path), b"x\ny\n")
c.quit(force=False)


# ---------- modes, status bar, search and ex commands ----------

print("\n[15] status bar contents")
c = Ced([FIX])
st = c.status()
check("shows the filename", "ced_fixture.txt" in st, True)
check("starts in normal mode", "-- NORMAL --" in st, True)
check("no dirty marker yet", "[+]" in st, False)
check("position right-aligned", st.rstrip().endswith("1/60"), True)
check("status bar spans the width", len(st), COLS)
c.press(ARROW_DOWN)
check("position tracks the cursor", c.status().rstrip().endswith("2/60"), True)

print("\n[15b] mode and dirty marker update")
c.press(b"i")
check("insert mode shown", "-- INSERT --" in c.status(), True)
c.type("z")
check("dirty marker appears", "[+]" in c.status(), True)
c.press(ESC)
check("back to normal mode", "-- NORMAL --" in c.status(), True)
check("dirty marker stays", "[+]" in c.status(), True)
c.quit()

print("\n[16] normal mode does not insert text")
path = write_file("/tmp/ced_m1.txt", "abc\n")
c = Ced([path])
c.type("zzz")
check("text unchanged", c.rows_text()[0], "abc")
check("buffer still clean", "[+]" in c.status(), False)
c.quit(force=False)

print("\n[17] hjkl motions")
c = Ced([FIX])
c.repeat(b"j", 3)
check("j moves down", c.cursor()[0], 4)
c.press(b"k")
check("k moves up", c.cursor()[0], 3)
c.press(END)
c.press(b"h")
check("h moves left", c.cursor(), (3, 16))
c.press(b"l")
check("l moves right", c.cursor(), (3, 17))

print("\n[17b] 0 and $ and gg and G")
c.press(b"0")
check("0 goes to line start", c.cursor(), (3, 1))
c.press(b"$")
check("$ goes to line end", c.cursor(), (3, 17))
c.press(b"G")
check("G goes to the last line", c.cursor()[0], TEXTROWS)
check("G lands on line 60", c.status().rstrip().endswith("60/60"), True)
c.type("gg")
check("gg goes back to the top", c.cursor(), (1, 1))
check("status says line 1", c.status().rstrip().endswith("1/60"), True)
c.quit()

print("\n[18] w and b word motion")
path = write_file("/tmp/ced_w1.txt", "alpha beta gamma\n")
c = Ced([path])
c.press(b"w")
check("w to second word", c.cursor(), (1, 7))
c.press(b"w")
check("w to third word", c.cursor(), (1, 12))
c.press(b"b")
check("b back to second", c.cursor(), (1, 7))
c.press(b"b")
check("b back to first", c.cursor(), (1, 1))
c.quit(force=False)

print("\n[19] x deletes a character, dd deletes a line")
path = write_file("/tmp/ced_x1.txt", "abcd\nsecond\nthird\n")
c = Ced([path])
c.press(b"x")
check("x removed the first char", c.rows_text()[0], "bcd")
c.type("dd")
check("dd removed the line", c.rows_text()[0], "second")
check("next line moved up", c.rows_text()[1], "third")
c.press(CTRL_S)
check("file on disk", read_file(path), b"second\nthird\n")
c.quit(force=False)

print("\n[20] i a I A o O insert positions")
path = write_file("/tmp/ced_i1.txt", "abc\n")
c = Ced([path])
c.press(b"a")
c.type("X")
check("a inserts after the cursor", c.rows_text()[0], "aXbc")
c.press(ESC)
c.press(b"A")
c.type("Z")
check("A appends at end of line", c.rows_text()[0], "aXbcZ")
c.press(ESC)
c.press(b"I")
c.type("S")
check("I inserts at line start", c.rows_text()[0], "SaXbcZ")
c.press(ESC)
c.quit()

print("\n[20b] o opens below, O opens above")
path = write_file("/tmp/ced_i2.txt", "one\ntwo\n")
c = Ced([path])
c.press(b"o")
c.type("below")
rows = c.rows_text()
check("o opened a line below", rows[1], "below")
check("original line pushed down", rows[2], "two")
c.press(ESC)
c.press(CTRL_S)
check("file on disk", read_file(path), b"one\nbelow\ntwo\n")
c.quit(force=False)

path = write_file("/tmp/ced_i3.txt", "one\ntwo\n")
c = Ced([path])
c.press(ARROW_DOWN)
c.press(b"O")
c.type("above")
rows = c.rows_text()
check("O opened a line above", rows[1], "above")
check("original line pushed down", rows[2], "two")
c.press(ESC)
c.press(CTRL_S)
check("file on disk", read_file(path), b"one\nabove\ntwo\n")
c.quit(force=False)

print("\n[21] incremental search with /")
c = Ced([FIX])
c.press(b"/")
check("prompt is showing", "Search:" in (c.msgbar() or ""), True)
c.type("line45")
check("jumped to the match", c.status().rstrip().endswith("45/60"), True)
c.press(ENTER)
check("prompt cleared", "Search:" in (c.msgbar() or ""), False)
check("stayed on the match", c.status().rstrip().endswith("45/60"), True)

print("\n[21b] ESC during search restores the original position")
c = Ced([FIX])
start = c.status().rstrip()
c.press(b"/")
c.type("line50")
check("moved while typing", c.status().rstrip().endswith("50/60"), True)
c.press(ESC)
check("position restored", c.status().rstrip(), start)
c.quit()

print("\n[21c] n and N step through matches")
c = Ced([FIX])
c.press(b"/")
c.type("indented")
c.press(ENTER)
check("found the tab line", c.status().rstrip().endswith("3/60"), True)
c.press(b"n")
check("n wrapped back to the only match", c.status().rstrip().endswith("3/60"), True)
c.quit()

path = write_file("/tmp/ced_s1.txt", "aaa\nbbb\naaa\nccc\n")
c = Ced([path])
c.press(b"/")
c.type("aaa")
c.press(ENTER)
first = c.status().rstrip()
check("first match on line 1", first.endswith("1/4"), True)
c.press(b"n")
check("n to the next match", c.status().rstrip().endswith("3/4"), True)
c.press(b"N")
check("N back to the previous", c.status().rstrip().endswith("1/4"), True)
c.quit(force=False)

print("\n[21d] Ctrl-F also opens search")
c = Ced([FIX])
c.press(CTRL_F)
check("prompt is showing", "Search:" in (c.msgbar() or ""), True)
c.press(ESC)
c.quit()

print("\n[22] ex commands")
path = write_file("/tmp/ced_ex1.txt", "data\n")
c = Ced([path])
c.insert("new ")
c.press(ESC)
c.type(":w")
c.press(ENTER)
check(":w saved", read_file(path), b"new data\n")
check("dirty marker cleared", "[+]" in c.status(), False)
c.type(":q")
c.press(ENTER)
check(":q exited a clean buffer", c.alive(), False)
check("exit code", c.exit_code(), 0)

print("\n[22b] :q refuses to drop unsaved work, :q! forces it")
path = write_file("/tmp/ced_ex2.txt", "keep\n")
c = Ced([path])
c.insert("dirty ")
c.press(ESC)
c.type(":q")
c.press(ENTER)
check("still alive", c.alive(), True)
check("told to use :q!", "q!" in (c.msgbar() or ""), True)
c.type(":q!")
c.press(ENTER)
check(":q! quit", c.alive(), False)
check("exit code", c.exit_code(), 0)
check("file untouched", read_file(path), b"keep\n")

print("\n[22c] :wq saves and exits")
path = write_file("/tmp/ced_ex3.txt", "old\n")
c = Ced([path])
c.press(b"A")
c.type(" tail")
c.press(ESC)
c.type(":wq")
c.press(ENTER)
check(":wq exited", c.alive(), False)
check("exit code", c.exit_code(), 0)
check("file written", read_file(path), b"old tail\n")

print("\n[22d] an unknown command is reported, not run")
c = Ced([FIX])
c.type(":nope")
c.press(ENTER)
check("still alive", c.alive(), True)
check("reported back", "nope" in (c.msgbar() or ""), True)
c.quit()

print("\n[23] saving a buffer with no filename prompts for one")
path = "/tmp/ced_saveas.txt"
if os.path.exists(path):
    os.unlink(path)
c = Ced([])
c.insert("fresh buffer")
c.press(ESC)
c.press(CTRL_S)
check("prompted for a name", "Save as:" in (c.msgbar() or ""), True)
c.type(path)
c.press(ENTER)
check("file created", os.path.exists(path), True)
check("file contents", read_file(path), b"fresh buffer\n")
check("status bar picked up the name", path.split("/")[-1] in c.status(), True)
c.quit(force=False)

print("\n[23b] ESC at the save-as prompt cancels")
c = Ced([])
c.insert("not saving this")
c.press(ESC)
c.press(CTRL_S)
c.press(ESC)
check("cancel reported", "cancel" in (c.msgbar() or "").lower(), True)
check("still no name", "[no name]" in c.status(), True)
c.quit()


# ---------- verdict ----------

print()
if failures:
    print("FAILURES (%d):" % len(failures))
    for f in failures:
        print("  -", f)
    print("\nRESULT: FAIL")
    sys.exit(1)
print("RESULT: PASS")
