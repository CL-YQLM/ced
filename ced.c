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

#define CED_VERSION "0.2.0"
#define CED_TAB_STOP 8

/* A terminal strips the upper three bits of a byte while Ctrl is held, so
   Ctrl-Q shows up as 0x11. Masking with 0x1f does the same thing. */
#define CTRL_KEY(k) ((k) & 0x1f)

/* Keys that arrive as multi-byte escape sequences are mapped to values no
   single byte can hold, so they can never be mistaken for text. */
enum editor_key {
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

/* One line of the file.
 *
 * `chars` is exactly what sits on disk. `render` is the same line with tabs
 * blown out into spaces, which is what actually goes on screen. Keeping both
 * is what forces the cursor to carry two column numbers: cx indexes chars,
 * rx indexes render, and on any line containing a tab the two disagree. */
typedef struct erow {
	int size;
	int rsize;
	char *chars;
	char *render;
} erow;

struct editor_config {
	int cx, cy;		/* cursor position within the file */
	int rx;			/* cursor column once tabs are expanded */
	int rowoff;		/* first file row visible on screen */
	int coloff;		/* first render column visible on screen */
	int screenrows;
	int screencols;
	int numrows;
	erow *row;
	char *filename;
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

/* ---------- rows ---------- */

/* Which screen column does character `cx` of this line land on? */
static int editor_row_cx_to_rx(const erow *row, int cx)
{
	int rx = 0;
	int i;

	for (i = 0; i < cx; i++) {
		if (row->chars[i] == '\t') {
			/* A tab runs to the next multiple of CED_TAB_STOP, so
			   how wide it is depends on where it starts. */
			rx += (CED_TAB_STOP - 1) - (rx % CED_TAB_STOP);
		}
		rx++;
	}

	return rx;
}

static void editor_update_row(erow *row)
{
	int tabs = 0;
	int i, idx = 0;

	for (i = 0; i < row->size; i++) {
		if (row->chars[i] == '\t')
			tabs++;
	}

	free(row->render);
	/* Worst case each tab opens up a whole tab stop, and it already takes
	   a byte of its own in chars. */
	row->render = malloc(row->size + tabs * (CED_TAB_STOP - 1) + 1);
	if (row->render == NULL)
		die("malloc");

	for (i = 0; i < row->size; i++) {
		if (row->chars[i] == '\t') {
			row->render[idx++] = ' ';
			while (idx % CED_TAB_STOP != 0)
				row->render[idx++] = ' ';
		} else {
			row->render[idx++] = row->chars[i];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

static void editor_append_row(const char *s, size_t len)
{
	erow *grown = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	erow *row;

	if (grown == NULL)
		die("realloc");
	E.row = grown;

	row = &E.row[E.numrows];
	row->size = (int)len;
	row->chars = malloc(len + 1);
	if (row->chars == NULL)
		die("malloc");
	memcpy(row->chars, s, len);
	row->chars[len] = '\0';

	row->rsize = 0;
	row->render = NULL;
	editor_update_row(row);

	E.numrows++;
}

/* ---------- file i/o ---------- */

static void editor_open(const char *filename)
{
	FILE *fp;
	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	free(E.filename);
	E.filename = strdup(filename);
	if (E.filename == NULL)
		die("strdup");

	fp = fopen(filename, "r");
	if (fp == NULL)
		die("fopen");

	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		/* getline hands back the newline as well. Strip it, and a
		   trailing carriage return too, so a file written on Windows
		   doesn't show a stray ^M at the end of every line. */
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
				       line[linelen - 1] == '\r'))
			linelen--;
		editor_append_row(line, (size_t)linelen);
	}

	free(line);
	fclose(fp);
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

/* Slide the visible window until the cursor is inside it again. */
static void editor_scroll(void)
{
	E.rx = 0;
	if (E.cy < E.numrows)
		E.rx = editor_row_cx_to_rx(&E.row[E.cy], E.cx);

	if (E.cy < E.rowoff)
		E.rowoff = E.cy;
	if (E.cy >= E.rowoff + E.screenrows)
		E.rowoff = E.cy - E.screenrows + 1;

	/* Horizontal scrolling works off rx, not cx, or the window would jump
	   by the wrong amount on lines that are indented with tabs. */
	if (E.rx < E.coloff)
		E.coloff = E.rx;
	if (E.rx >= E.coloff + E.screencols)
		E.coloff = E.rx - E.screencols + 1;
}

static void editor_draw_rows(struct abuf *ab)
{
	int y;

	for (y = 0; y < E.screenrows; y++) {
		int filerow = y + E.rowoff;

		if (filerow >= E.numrows) {
			/* The greeting would just be in the way of a real file,
			   so it only appears when nothing was opened. */
			if (E.numrows == 0 && y == E.screenrows / 3) {
				char welcome[80];
				int len = snprintf(welcome, sizeof(welcome),
						   "ced -- version %s",
						   CED_VERSION);
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
		} else {
			/* Scrolling sideways is just drawing a slice of the
			   rendered line. Once coloff is past the end of a short
			   line there is nothing left to show. */
			int len = E.row[filerow].rsize - E.coloff;

			if (len < 0)
				len = 0;
			if (len > E.screencols)
				len = E.screencols;
			if (len > 0)
				ab_append(ab, &E.row[filerow].render[E.coloff],
					  len);
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
	char buf[32];

	editor_scroll();

	ab_append(&ab, "\x1b[?25l", 6);		/* hide the cursor */
	ab_append(&ab, "\x1b[H", 3);		/* move to row 1, col 1 */

	editor_draw_rows(&ab);

	/* Escape sequences count from 1, and the cursor is positioned in
	   screen space, so the scroll offsets have to come back off. */
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
		 (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
	ab_append(&ab, buf, (int)strlen(buf));

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
	char seq[3];

	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		/* VTIME expiring gives nread == 0, which is not a failure. */
		if (nread == -1 && errno != EAGAIN)
			die("read");
	}

	if (c != '\x1b')
		return (unsigned char)c;

	/* An escape byte on its own is the Escape key; anything else is the
	   start of a sequence like "\x1b[A". If the rest never turns up inside
	   the VTIME window, fall back to a plain Escape. */
	if (read(STDIN_FILENO, &seq[0], 1) != 1)
		return '\x1b';
	if (read(STDIN_FILENO, &seq[1], 1) != 1)
		return '\x1b';

	if (seq[0] == '[') {
		if (seq[1] >= '0' && seq[1] <= '9') {
			/* The numbered forms carry a trailing '~'. */
			if (read(STDIN_FILENO, &seq[2], 1) != 1)
				return '\x1b';
			if (seq[2] == '~') {
				switch (seq[1]) {
				case '1': return HOME_KEY;
				case '3': return DEL_KEY;
				case '4': return END_KEY;
				case '5': return PAGE_UP;
				case '6': return PAGE_DOWN;
				case '7': return HOME_KEY;
				case '8': return END_KEY;
				}
			}
		} else {
			switch (seq[1]) {
			case 'A': return ARROW_UP;
			case 'B': return ARROW_DOWN;
			case 'C': return ARROW_RIGHT;
			case 'D': return ARROW_LEFT;
			case 'H': return HOME_KEY;
			case 'F': return END_KEY;
			}
		}
	} else if (seq[0] == 'O') {
		/* Home and End look like this when the terminal has
		   application keypad mode switched on. */
		switch (seq[1]) {
		case 'H': return HOME_KEY;
		case 'F': return END_KEY;
		}
	}

	return '\x1b';
}

static void editor_move_cursor(int key)
{
	/* Sitting one line past the end of the file is legal, and there is no
	   erow there to ask about. */
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	int rowlen;

	switch (key) {
	case ARROW_LEFT:
		if (E.cx != 0) {
			E.cx--;
		} else if (E.cy > 0) {
			/* Off the left edge lands at the end of the line above,
			   the way every real editor behaves. */
			E.cy--;
			E.cx = E.row[E.cy].size;
		}
		break;
	case ARROW_RIGHT:
		if (row != NULL && E.cx < row->size) {
			E.cx++;
		} else if (row != NULL && E.cx == row->size) {
			E.cy++;
			E.cx = 0;
		}
		break;
	case ARROW_UP:
		if (E.cy != 0)
			E.cy--;
		break;
	case ARROW_DOWN:
		if (E.cy < E.numrows)
			E.cy++;
		break;
	}

	/* The line just moved onto may be shorter than the one left behind, so
	   pull the cursor back to the end of it. */
	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	rowlen = (row != NULL) ? row->size : 0;
	if (E.cx > rowlen)
		E.cx = rowlen;
}

static void editor_process_keypress(void)
{
	int c = editor_read_key();

	switch (c) {
	case CTRL_KEY('q'):
		(void)!write(STDOUT_FILENO, "\x1b[2J", 4);
		(void)!write(STDOUT_FILENO, "\x1b[H", 3);
		exit(0);

	case HOME_KEY:
		E.cx = 0;
		break;

	case END_KEY:
		if (E.cy < E.numrows)
			E.cx = E.row[E.cy].size;
		break;

	case PAGE_UP:
	case PAGE_DOWN: {
		int times = E.screenrows;

		/* Park the cursor at the top or bottom of the current view
		   first, then walk a whole screen from there. Going through the
		   arrow logic keeps the end-of-line clamping consistent. */
		if (c == PAGE_UP) {
			E.cy = E.rowoff;
		} else {
			E.cy = E.rowoff + E.screenrows - 1;
			if (E.cy > E.numrows)
				E.cy = E.numrows;
		}

		while (times-- > 0)
			editor_move_cursor(c == PAGE_UP ? ARROW_UP
							: ARROW_DOWN);
		break;
	}

	case ARROW_UP:
	case ARROW_DOWN:
	case ARROW_LEFT:
	case ARROW_RIGHT:
		editor_move_cursor(c);
		break;
	}
}

/* ---------- init ---------- */

static void init_editor(void)
{
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.row = NULL;
	E.filename = NULL;

	if (get_window_size(&E.screenrows, &E.screencols) == -1)
		die("get_window_size");
}

int main(int argc, char *argv[])
{
	enable_raw_mode();
	init_editor();
	if (argc >= 2)
		editor_open(argv[1]);

	for (;;) {
		editor_refresh_screen();
		editor_process_keypress();
	}

	return 0;
}
