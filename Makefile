CC      ?= cc

# -std=c99 hides the BSD-ish bits of glibc (ioctl, TIOCGWINSZ), so ask for
# the default feature set back explicitly.
CFLAGS  = -std=c99 -Wall -Wextra -pedantic -g -D_DEFAULT_SOURCE

ced: ced.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f ced

.PHONY: clean
