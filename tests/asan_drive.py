"""ASan/UBSan pass over the memory-handling paths, on a C file.

The behaviour suites run on the normal build; this one is only about memory,
so it stays short - ASan makes every redraw slow enough that volume buys
nothing. The fixture is .c on purpose, otherwise E.syntax stays NULL and the
highlighter never runs at all.

Paths this is here to touch:
  editor_update_syntax      malloc per row, the memsets for comments,
                            keywords and strings, and the recursive cascade
                            when a line's block-comment state flips
  editor_select_syntax_highlight   rescan of every row on open
  editor_search_from        HL_MATCH memset, with cx converted to a render
                            offset first
  editor_update_row         tab expansion rebuilding render
  editor_draw_rows          the horizontal slice plus per-byte colour runs
  editor_insert_row         mid-array memmove and the idx renumbering
  editor_row_insert_char    memmove carrying the terminator
  editor_row_del_char       x and backspace
  editor_rows_to_string     save

Two things that previously made this look hung, both my fault rather than
ced's: pump() must wait rather than poll with a zero timeout, or the writes
outrun an ASan-slowed child and both ends block on full pty buffers; and a
dirty buffer deliberately refuses the first three Ctrl-Q presses, so it has
to be saved or pressed CED_QUIT_TIMES + 1 times.
"""
import fcntl
import os
import pty
import select
import struct
import sys
import termios
import time

BIN = os.environ.get("CED_BIN", "./ced-asan")

# Built here rather than shared with the other suites: /tmp does not survive
# the WSL VM shutting down between runs.
FIX = "/tmp/ced_asan_fixture.c"
with open(FIX, "w") as fh:
    fh.write("/* header */\n")
    fh.write("int main(void)\n")
    fh.write("{\n")
    fh.write("\tint n = 42;\n")
    fh.write("\tchar *s = \"hi\\n\";\n")
    fh.write("\t// note\n")
    fh.write("\tlong wide = " + "1234567890" * 30 + ";\n")
    fh.write("\treturn 0;\n")
    fh.write("}\n")
    fh.write("/* multi\n")
    fh.write("   line */\n")
    fh.write("long tail = 7;\n")

ESC = b"\x1b"
RIGHT, END, HOME = b"\x1b[C", b"\x1b[F", b"\x1b[H"
BKSP, ENTER = b"\x7f", b"\r"


def text(s):
    return [bytes([b]) for b in s.encode("utf-8")]


def typed(cmd, s):
    return [cmd] + text(s) + [ESC]


SCRIPT = (
    # the very wide line: horizontal slice and a big render rebuild
    [b"j"] * 6 + [END, HOME] +

    # open a block comment at the top, which cascades the whole file
    text("gg") + typed(b"I", "/*") +

    # take it straight back out, cascading everything back to code
    [b"x", b"x"] +

    # an unterminated string, to walk the string scanner off the end
    [b"j"] * 4 + typed(b"A", " \"loose") +

    # tab-indented line, so render and hl disagree on offsets
    text("gg") + [b"j"] * 3 + [END, BKSP] +

    # search: HL_MATCH memset after converting cx to a render column
    [b"/"] + text("tail") + [ENTER] +
    [b"n", b"N"] +

    # save, which serialises every row
    [b":"] + text("w") + [ENTER]
)

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

drawn = 0


def pump(timeout):
    global drawn
    while True:
        r, _, _ = select.select([master], [], [], timeout)
        if not r:
            return
        try:
            chunk = os.read(master, 1 << 20)
        except OSError:
            return
        if not chunk:
            return
        drawn += len(chunk)
        timeout = 0.05


pump(3.0)
print("startup bytes :", drawn, flush=True)

for keys in SCRIPT:
    os.write(master, keys)
    pump(0.08)
print("after keys    :", drawn, flush=True)

status = None


def reaped():
    """Wait status once the child is gone, else None.

    waitpid hands a status over exactly once, so it is cached; polling with
    WNOHANG and then waiting again loses the exit code and raises ECHILD.
    """
    global status
    if status is None:
        done, st = os.waitpid(pid, os.WNOHANG)
        if done != 0:
            status = st
    return status


for key in (b":", b"q", b"\r"):
    os.write(master, key)
    pump(0.3)

for _ in range(4):
    if reaped() is not None:
        break
    os.write(master, b"\x11")
    pump(0.4)

deadline = time.time() + 20
while reaped() is None and time.time() < deadline:
    pump(0.2)

if status is None:
    print("VERDICT       : still running, killing", flush=True)
    os.kill(pid, 9)
    os.waitpid(pid, 0)
    sys.exit(2)

code = os.WEXITSTATUS(status) if os.WIFEXITED(status) else -os.WTERMSIG(status)
print("keys sent     :", len(SCRIPT), flush=True)
print("exit code     :", code, flush=True)
print("VERDICT       : clean" if code == 0 else "VERDICT       : bad exit",
      flush=True)
sys.exit(0 if code == 0 else 1)
