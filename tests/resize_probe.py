"""Probe what actually happens when the terminal is resized mid-session.

ced reads the window size once, in init_editor, and never installs a SIGWINCH
handler. This measures the real consequence rather than assuming one: the pty
is grown from 24x80 to 40x100 and the editor is asked to redraw.
"""
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time

BIN = os.environ.get("CED_BIN", "./ced")
HIDE, SHOW = b"\x1b[?25l", b"\x1b[?25h"
ESC_RE = re.compile(rb"\x1b\[[0-9;?]*[a-zA-Z]")

FIX = "/tmp/ced_resize_fixture.txt"
with open(FIX, "w") as fh:
    for n in range(1, 61):
        fh.write("line%02d\n" % n)

master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))

pid = os.fork()
if pid == 0:
    os.close(master)
    os.setsid()
    fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
    os.dup2(slave, 0)
    os.dup2(slave, 1)
    os.dup2(slave, 2)
    os.execvpe(BIN, [BIN, FIX], dict(os.environ, TERM="xterm-256color"))
    os._exit(127)


def drain(timeout=0.4):
    buf = b""
    while True:
        r, _, _ = select.select([master], [], [], timeout)
        if not r:
            break
        try:
            chunk = os.read(master, 1 << 20)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        timeout = 0.12
    return buf


def rows_of(raw):
    done = [HIDE + p for p in raw.split(HIDE)[1:] if SHOW in p]
    if not done:
        return []
    return [ESC_RE.sub(b"", l).decode("utf-8", "replace")
            for l in done[-1].split(b"\r\n")]


time.sleep(0.5)
before = rows_of(drain())
print("before resize : %d rows drawn, last row %r" % (len(before), before[-1]))

NEW_ROWS = int(os.environ.get("NEW_ROWS", "40"))
NEW_COLS = int(os.environ.get("NEW_COLS", "100"))

fcntl.ioctl(slave, termios.TIOCSWINSZ,
            struct.pack("HHHH", NEW_ROWS, NEW_COLS, 0, 0))
os.kill(pid, signal.SIGWINCH)
time.sleep(0.3)
drain(0.3)

os.write(master, b"\x1b[B")
time.sleep(0.4)
after = rows_of(drain())
print("after  resize : %d rows drawn, last row %r (terminal is now %dx%d)"
      % (len(after), after[-1], NEW_ROWS, NEW_COLS))

alive = os.waitpid(pid, os.WNOHANG) == (0, 0)
print("survived SIGWINCH :", alive)
print("adapted to %-2d rows: %s" % (NEW_ROWS, len(after) == NEW_ROWS))
if len(after) > NEW_ROWS:
    print("OVERDRAW          : writes %d rows into a %d-row terminal -> scrolls"
          % (len(after), NEW_ROWS))
else:
    print("rows left stale   :", NEW_ROWS - len(after))

if alive:
    os.write(master, b"\x11")
    time.sleep(0.4)
    drain(0.25)
    _, status = os.waitpid(pid, 0)
    code = os.WEXITSTATUS(status) if os.WIFEXITED(status) else -os.WTERMSIG(status)
    print("exit code         :", code)
sys.exit(0)
