/* ced - a terminal text editor written from scratch.
 *
 * There is no curses layer here. Keyboard input comes straight off the
 * terminal in raw mode (termios), and the screen is drawn by printing VT100
 * escape sequences.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CED_VERSION "0.1.0"

/* A terminal strips the upper three bits of a byte while Ctrl is held, so
   Ctrl-Q shows up as 0x11. Masking with 0x1f does the same thing. */
#define CTRL_KEY(k) ((k) & 0x1f)

struct editor_config {
	int screenrows;
	int screencols;
	struct termios orig_termios;
};

static struct editor_config E;

/* ---------- terminal ---------- */

static void die(const char *msg)
{
	/* The screen is covered in our own output, so clear it first or the
	   error message ends up buried among the tildes. */
	(void)!write(STDOUT_FILENO, "\x1b[2J", 4);
	(void)!write(STDOUT_FILENO, "\x1b[H", 3);

	perror(msg);
	exit(1);
}

static void disable_raw_mode(void)
{
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
}

static void enable_raw_mode(void)
{
	struct termios raw;

	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
		die("tcgetattr");

	/* Whatever happens from here on, the terminal gets handed back in the
	   state we found it in. */
	atexit(disable_raw_mode);

	raw = E.orig_termios;

	/* IXON kills Ctrl-S/Ctrl-Q software flow control, ICRNL stops the
	   driver rewriting carriage returns as newlines (so Ctrl-M really is
	   13). The other three are the traditional raw recipe and hardly
	   matter on anything modern. */
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

	/* No more "\n" -> "\r\n" on the way out, which means every line we
	   draw has to carry its own carriage return. */
	raw.c_oflag &= ~(OPOST);

	raw.c_cflag |= (CS8);

	/* ECHO stops keys being printed back at us. ICANON is the important
	   one: without it the kernel stops buffering a line at a time and
	   read() returns the moment a key is pressed. ISIG turns Ctrl-C and
	   Ctrl-Z into plain bytes, IEXTEN does the same for Ctrl-V. */
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

	/* Return as soon as there is at least one byte, and give up after
	   100ms if nothing arrives. */
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die("tcsetattr");
}

static int get_cursor_position(int *rows, int *cols)
{
	char buf[32];
	unsigned int i = 0;

	/* "\x1b[6n" is a Device Status Report asking for the cursor position.
	   The reply arrives on stdin as "\x1b[<row>;<col>R". */
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
		return -1;

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1)
			return -1;
		if (buf[i] == 'R')
			break;
		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[')
		return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
		return -1;

	return 0;
}

static int get_window_size(int *rows, int *cols)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		/* Not every terminal answers that ioctl. Plan B: drive the
		   cursor far past the bottom right corner (C and B clamp at the
		   edge rather than scrolling) and ask where it ended up. */
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
			return -1;
		return get_cursor_position(rows, cols);
	}

	*cols = ws.ws_col;
	*rows = ws.ws_row;
	return 0;
}

/* ---------- append buffer ----------
 *
 * One write() per piece of the screen makes the cursor flicker badly, so a
 * frame is assembled in memory and flushed with a single syscall.
 */

struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT { NULL, 0 }

static void ab_append(struct abuf *ab, const char *s, int len)
{
	char *grown = realloc(ab->b, ab->len + len);

	if (grown == NULL)
		return;

	memcpy(&grown[ab->len], s, len);
	ab->b = grown;
	ab->len += len;
}

static void ab_free(struct abuf *ab)
{
	free(ab->b);
}

/* ---------- output ---------- */

static void editor_draw_rows(struct abuf *ab)
{
	int y;

	for (y = 0; y < E.screenrows; y++) {
		if (y == E.screenrows / 3) {
			char welcome[80];
			int len = snprintf(welcome, sizeof(welcome),
					   "ced -- version %s", CED_VERSION);
			int padding;

			if (len > E.screencols)
				len = E.screencols;

			padding = (E.screencols - len) / 2;
			if (padding > 0) {
				ab_append(ab, "~", 1);
				padding--;
			}
			while (padding-- > 0)
				ab_append(ab, " ", 1);

			ab_append(ab, welcome, len);
		} else {
			ab_append(ab, "~", 1);
		}

		/* Erase whatever used to be on the rest of this line. Cheaper
		   and flicker-free compared to clearing the whole screen. */
		ab_append(ab, "\x1b[K", 3);

		/* No newline after the last row, or the terminal scrolls and
		   the top line disappears. */
		if (y < E.screenrows - 1)
			ab_append(ab, "\r\n", 2);
	}
}

static void editor_refresh_screen(void)
{
	struct abuf ab = ABUF_INIT;

	ab_append(&ab, "\x1b[?25l", 6);		/* hide the cursor */
	ab_append(&ab, "\x1b[H", 3);		/* move to row 1, col 1 */

	editor_draw_rows(&ab);

	ab_append(&ab, "\x1b[H", 3);
	ab_append(&ab, "\x1b[?25h", 6);		/* show it again */

	if (write(STDOUT_FILENO, ab.b, ab.len) == -1)
		die("write");
	ab_free(&ab);
}

/* ---------- input ---------- */

static int editor_read_key(void)
{
	int nread;
	char c;

	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		/* VTIME expiring gives nread == 0, which is not a failure. */
		if (nread == -1 && errno != EAGAIN)
			die("read");
	}

	return c;
}

static void editor_process_keypress(void)
{
	int c = editor_read_key();

	switch (c) {
	case CTRL_KEY('q'):
		(void)!write(STDOUT_FILENO, "\x1b[2J", 4);
		(void)!write(STDOUT_FILENO, "\x1b[H", 3);
		exit(0);
	}
}

/* ---------- init ---------- */

static void init_editor(void)
{
	if (get_window_size(&E.screenrows, &E.screencols) == -1)
		die("get_window_size");
}

int main(void)
{
	enable_raw_mode();
	init_editor();

	for (;;) {
		editor_refresh_screen();
		editor_process_keypress();
	}

	return 0;
}
