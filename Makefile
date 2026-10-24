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

clean:
	rm -f ced ced-asan

.PHONY: clean
