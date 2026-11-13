CC      ?= cc

# -std=c99 hides the BSD-ish bits of glibc (ioctl, TIOCGWINSZ), so ask for
# the default feature set back explicitly.
CFLAGS  = -std=c99 -Wall -Wextra -pedantic -g -D_DEFAULT_SOURCE

ced: ced.c
	$(CC) $(CFLAGS) -o $@ $<

# Same thing with the sanitizers on. Worth a run after touching the row
# buffers, where a bad render size would corrupt memory quietly.
ced-asan: ced.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $<

# The suites drive ced on a real pty and look for ./ced, so they run from
# here rather than from inside tests/.
test: ced
	python3 tests/test_editor.py
	python3 tests/test_highlight.py

# leak_canary leaks on purpose. A clean leak report from ced proves nothing
# unless leak checking is known to work in the first place, and that is what
# the canary is for; its non-zero exit is expected and ignored.
#
# It gets built under /tmp deliberately. LeakSanitizer cannot symbolize a
# binary sitting on a mounted Windows filesystem, and instead of failing it
# spins forever printing DEADLYSIGNAL, which is no fun to debug twice.
test-asan: ced-asan
	$(CC) -g -fsanitize=address -o /tmp/ced_leak_canary tests/leak_canary.c
	-ASAN_OPTIONS=detect_leaks=1 /tmp/ced_leak_canary
	CED_BIN=./ced-asan ASAN_OPTIONS=detect_leaks=1 python3 tests/asan_drive.py

clean:
	rm -f ced ced-asan /tmp/ced_leak_canary

.PHONY: clean test test-asan
