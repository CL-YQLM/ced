/* ced - a terminal text editor written from scratch.
 *
 * There is no curses layer here. Keyboard input comes straight off the
 * terminal in raw mode (termios), and the screen is drawn by printing VT100
 * escape sequences.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CED_VERSION "0.3.0"
#define CED_TAB_STOP 8

/* How many extra times Ctrl-Q has to be pressed to throw away unsaved work. */
#define CED_QUIT_TIMES 3

/* A terminal strips the upper three bits of a byte while Ctrl is held, so
   Ctrl-Q shows up as 0x11. Masking with 0x1f does the same thing. */
#define CTRL_KEY(k) ((k) & 0x1f)

/* Keys that arrive as multi-byte escape sequences are mapped to values no
   single byte can hold, so they can never be mistaken for text. */
enum editor_key {
	BACKSPACE = 127,
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
 * rx indexes render, and on any line containing a tab the two disagree.
 * Editing always goes through chars; render is rebuilt from it afterwards. */
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
	int screenrows;		/* text rows, excluding the message bar */
	int screencols;
	int numrows;
	erow *row;
	int dirty;		/* edits made since the last save */
	char *filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
};

static struct editor_config E;

/* Lives down in the output section, but the save path needs to report with
   it. */
static void editor_set_status_message(const char *fmt, ...);

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

static void editor_insert_row(int at, const char *s, size_t len)
{
	erow *grown;

	if (at < 0 || at > E.numrows)
		return;

	grown = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	if (grown == NULL)
		die("realloc");
	E.row = grown;

	/* Open a gap at `at` by sliding everything below it down one slot. */
	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

	E.row[at].size = (int)len;
	E.row[at].chars = malloc(len + 1);
	if (E.row[at].chars == NULL)
		die("malloc");
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editor_update_row(&E.row[at]);

	E.numrows++;
	E.dirty++;
}

static void editor_free_row(erow *row)
{
	free(row->render);
	free(row->chars);
}

static void editor_del_row(int at)
{
	if (at < 0 || at >= E.numrows)
		return;

	editor_free_row(&E.row[at]);
	memmove(&E.row[at], &E.row[at + 1],
		sizeof(erow) * (E.numrows - at - 1));
	E.numrows--;
	E.dirty++;
}

static void editor_row_insert_char(erow *row, int at, int c)
{
	char *grown;

	if (at < 0 || at > row->size)
		at = row->size;

	/* One byte for the character, one for the terminator. */
	grown = realloc(row->chars, row->size + 2);
	if (grown == NULL)
		die("realloc");
	row->chars = grown;

	/* +1 on the length carries the terminator along with the tail. */
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = (char)c;

	editor_update_row(row);
	E.dirty++;
}

static void editor_row_del_char(erow *row, int at)
{
	if (at < 0 || at >= row->size)
		return;

	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
	row->size--;

	editor_update_row(row);
	E.dirty++;
}

static void editor_row_append_string(erow *row, const char *s, size_t len)
{
	char *grown = realloc(row->chars, row->size + len + 1);

	if (grown == NULL)
		die("realloc");
	row->chars = grown;

	memcpy(&row->chars[row->size], s, len);
	row->size += (int)len;
	row->chars[row->size] = '\0';

	editor_update_row(row);
	E.dirty++;
}

/* ---------- editor operations ---------- */

static void editor_insert_char(int c)
{
	/* Typing on the virtual line past the end of the file brings it into
	   existence first. */
	if (E.cy == E.numrows)
		editor_insert_row(E.numrows, "", 0);

	editor_row_insert_char(&E.row[E.cy], E.cx, c);
	E.cx++;
}

static void editor_insert_newline(void)
{
	if (E.cx == 0) {
		/* At the start of a line, push a blank one above instead of
		   splitting anything. */
		editor_insert_row(E.cy, "", 0);
	} else {
		erow *row = &E.row[E.cy];

		/* Everything right of the cursor moves down to a new line. */
		editor_insert_row(E.cy + 1, &row->chars[E.cx],
				  (size_t)(row->size - E.cx));

		/* That insert reallocs E.row, so the old pointer may now be
		   dangling. Look the row up again before truncating it. */
		row = &E.row[E.cy];
		row->size = E.cx;
		row->chars[row->size] = '\0';
		editor_update_row(row);
	}

	E.cy++;
	E.cx = 0;
}

static void editor_del_char(void)
{
	erow *row;

	if (E.cy == E.numrows)
		return;
	if (E.cx == 0 && E.cy == 0)
		return;

	row = &E.row[E.cy];
	if (E.cx > 0) {
		editor_row_del_char(row, E.cx - 1);
		E.cx--;
	} else {
		/* Backspace in column 0 joins this line onto the end of the
		   one above, so the cursor lands where the join happened. */
		E.cx = E.row[E.cy - 1].size;
		editor_row_append_string(&E.row[E.cy - 1], row->chars,
					 (size_t)row->size);
		editor_del_row(E.cy);
		E.cy--;
	}
}

/* ---------- file i/o ---------- */

static char *editor_rows_to_string(int *buflen)
{
	int totlen = 0;
	int j;
	char *buf;
	char *p;

	for (j = 0; j < E.numrows; j++)
		totlen += E.row[j].size + 1;	/* room for the newline */
	*buflen = totlen;

	/* malloc(0) is allowed to hand back NULL, which would look like an
	   allocation failure, so never ask for nothing. */
	buf = malloc(totlen == 0 ? 1 : (size_t)totlen);
	if (buf == NULL)
		die("malloc");

	p = buf;
	for (j = 0; j < E.numrows; j++) {
		memcpy(p, E.row[j].chars, E.row[j].size);
		p += E.row[j].size;
		*p++ = '\n';
	}

	return buf;
}

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
	if (fp == NULL) {
		/* A name that is not on disk yet just means a new file: start
		   from an empty buffer and let the first save create it. */
		if (errno == ENOENT)
			return;
		die("fopen");
	}

	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		/* getline hands back the newline as well. Strip it, and a
		   trailing carriage return too, so a file written on Windows
		   doesn't show a stray ^M at the end of every line. */
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
				       line[linelen - 1] == '\r'))
			linelen--;
		editor_insert_row(E.numrows, line, (size_t)linelen);
	}

	free(line);
	fclose(fp);

	/* Loading a file is not an edit. */
	E.dirty = 0;
}

static void editor_save(void)
{
	int len;
	int fd;
	char *buf;
	const char *problem = NULL;

	if (E.filename == NULL) {
		editor_set_status_message("No file name - start ced with one");
		return;
	}

	buf = editor_rows_to_string(&len);

	/* Truncating in place keeps the same inode, so permissions, hard links
	   and symlinks all survive the save. The trade-off is a window where
	   the file on disk is short: a crash between ftruncate and write loses
	   data. Writing a temp file and renaming it closes that window, but
	   replaces a symlink with a regular file and needs the same
	   filesystem, so this keeps the simpler behaviour. */
	fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	if (fd == -1) {
		problem = strerror(errno);
	} else {
		if (ftruncate(fd, len) == -1)
			problem = strerror(errno);
		else if (write(fd, buf, (size_t)len) != len)
			problem = strerror(errno);
		close(fd);
	}

	free(buf);

	if (problem != NULL) {
		editor_set_status_message("Can't save: %s", problem);
		return;
	}

	E.dirty = 0;
	editor_set_status_message("%d bytes written to disk", len);
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
			/* The greeting is only for a session that was handed
			   nothing at all. Naming a file means there is a file,
			   even an empty or brand new one, and a version banner
			   sitting in the text area would read as content. */
			if (E.numrows == 0 && E.filename == NULL &&
			    y == E.screenrows / 3) {
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
		ab_append(ab, "\r\n", 2);
	}
}

static void editor_draw_message_bar(struct abuf *ab)
{
	int len = (int)strlen(E.statusmsg);

	ab_append(ab, "\x1b[K", 3);

	if (len > E.screencols)
		len = E.screencols;
	/* Messages time out so the bar stops advertising something that
	   happened a long time ago. */
	if (len > 0 && time(NULL) - E.statusmsg_time < 5)
		ab_append(ab, E.statusmsg, len);
}

static void editor_refresh_screen(void)
{
	struct abuf ab = ABUF_INIT;
	char buf[32];

	editor_scroll();

	ab_append(&ab, "\x1b[?25l", 6);		/* hide the cursor */
	ab_append(&ab, "\x1b[H", 3);		/* move to row 1, col 1 */

	editor_draw_rows(&ab);
	editor_draw_message_bar(&ab);

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

static void editor_set_status_message(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);

	E.statusmsg_time = time(NULL);
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
	static int quit_times = CED_QUIT_TIMES;
	int c = editor_read_key();

	switch (c) {
	case '\r':
		editor_insert_newline();
		break;

	case CTRL_KEY('q'):
		if (E.dirty > 0 && quit_times > 0) {
			editor_set_status_message(
				"%d unsaved change%s. Press Ctrl-Q %d more "
				"time%s to quit anyway.",
				E.dirty, E.dirty == 1 ? "" : "s",
				quit_times, quit_times == 1 ? "" : "s");
			quit_times--;
			return;
		}
		(void)!write(STDOUT_FILENO, "\x1b[2J", 4);
		(void)!write(STDOUT_FILENO, "\x1b[H", 3);
		exit(0);

	case CTRL_KEY('s'):
		editor_save();
		break;

	case HOME_KEY:
		E.cx = 0;
		break;

	case END_KEY:
		if (E.cy < E.numrows)
			E.cx = E.row[E.cy].size;
		break;

	case BACKSPACE:
	case CTRL_KEY('h'):	/* what Backspace sent before 127 won out */
		editor_del_char();
		break;

	case DEL_KEY:
		/* Forward delete is a backspace from one place to the right. */
		editor_move_cursor(ARROW_RIGHT);
		editor_del_char();
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

	case CTRL_KEY('l'):	/* redraw: every loop already does one */
	case '\x1b':
		break;

	default:
		/* Control codes we have not claimed would show up as garbage,
		   so only printable bytes get inserted. Bytes above 127 are
		   kept so UTF-8 text at least survives a round trip, even
		   though the column maths still counts them one per byte. */
		if (!iscntrl(c))
			editor_insert_char(c);
		break;
	}

	quit_times = CED_QUIT_TIMES;
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
	E.dirty = 0;
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;

	if (get_window_size(&E.screenrows, &E.screencols) == -1)
		die("get_window_size");

	/* The bottom line belongs to the message bar. */
	E.screenrows -= 1;
}

int main(int argc, char *argv[])
{
	enable_raw_mode();
	init_editor();
	if (argc >= 2)
		editor_open(argv[1]);

	editor_set_status_message("Ctrl-S = save | Ctrl-Q = quit");

	for (;;) {
		editor_refresh_screen();
		editor_process_keypress();
	}

	return 0;
}
