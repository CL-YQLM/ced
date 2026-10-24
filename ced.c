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

#define CED_VERSION "0.5.0"
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

enum editor_mode {
	MODE_NORMAL,
	MODE_INSERT
};

enum editor_highlight {
	HL_NORMAL = 0,
	HL_COMMENT,
	HL_MLCOMMENT,
	HL_KEYWORD1,
	HL_KEYWORD2,
	HL_STRING,
	HL_NUMBER,
	HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

struct editor_syntax {
	const char *filetype;
	const char **filematch;
	const char **keywords;
	const char *singleline_comment_start;
	const char *multiline_comment_start;
	const char *multiline_comment_end;
	int flags;
};

/* One line of the file.
 *
 * `chars` is exactly what sits on disk. `render` is the same line with tabs
 * blown out into spaces, which is what actually goes on screen. Keeping both
 * is what forces the cursor to carry two column numbers: cx indexes chars,
 * rx indexes render, and on any line containing a tab the two disagree.
 * Editing always goes through chars; render is rebuilt from it afterwards.
 *
 * `hl` is one highlight code per byte of render, not of chars, since it is
 * consumed while drawing. `idx` is the row's own position, which the
 * highlighter needs to reach the line above and below. */
typedef struct erow {
	int idx;
	int size;
	int rsize;
	char *chars;
	char *render;
	unsigned char *hl;
	int hl_open_comment;	/* line ends with a block comment still open */
} erow;

struct editor_config {
	int cx, cy;		/* cursor position within the file */
	int rx;			/* cursor column once tabs are expanded */
	int rowoff;		/* first file row visible on screen */
	int coloff;		/* first render column visible on screen */
	int screenrows;		/* text rows, excluding status and message bars */
	int screencols;
	int numrows;
	erow *row;
	int dirty;		/* edits made since the last save */
	int mode;
	int pending;		/* first key of a two-key command like dd */
	char *filename;
	char *last_query;	/* what n and N search for */
	int match_row;		/* row currently wearing the search highlight */
	struct editor_syntax *syntax;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
};

static struct editor_config E;

/* All defined further down, but needed earlier: saving prompts for a file
   name when there isn't one, and the prompt has to redraw and read keys
   while it runs. */
static void editor_set_status_message(const char *fmt, ...);
static void editor_refresh_screen(void);
static int editor_read_key(void);
static char *editor_prompt(const char *prompt, void (*callback)(char *, int));
static void editor_move_cursor(int key);

/* ---------- filetypes ---------- */

static const char *C_HL_extensions[] = {
	".c", ".h", ".cpp", ".cc", ".hpp", NULL
};

/* Keywords with a trailing bar get the second colour; they are the type
   names, as opposed to control flow. */
static const char *C_HL_keywords[] = {
	"switch", "if", "while", "for", "break", "continue", "return", "else",
	"struct", "union", "typedef", "static", "enum", "case", "default",
	"do", "goto", "sizeof", "extern", "const", "volatile", "register",
	"inline", "restrict",

	"int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
	"void|", "short|", "auto|", "size_t|", "ssize_t|", "time_t|", NULL
};

static struct editor_syntax HLDB[] = {
	{
		"c",
		C_HL_extensions,
		C_HL_keywords,
		"//", "/*", "*/",
		HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
	}
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/* ---------- terminal ---------- */

static void editor_exit(void)
{
	(void)!write(STDOUT_FILENO, "\x1b[2J", 4);
	(void)!write(STDOUT_FILENO, "\x1b[H", 3);
	exit(0);
}

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

/* ---------- syntax highlighting ---------- */

static int is_separator(int c)
{
	return isspace(c) || c == '\0' ||
	       strchr(",.()+-/*=~%<>[];{}:&|!?", c) != NULL;
}

/* Work out the highlight code for every byte of this line's render.
 *
 * Block comments are what make this more than a per-line job: whether this
 * line starts inside a comment depends on where the line above finished, so
 * the state is carried in hl_open_comment and a line whose state flips has to
 * push a rescan down the file. */
static void editor_update_syntax(erow *row)
{
	const char **keywords;
	const char *scs, *mcs, *mce;
	int scs_len, mcs_len, mce_len;
	int prev_sep = 1;
	int in_string = 0;
	int in_comment;
	int i = 0;
	int changed;

	free(row->hl);
	row->hl = NULL;
	if (row->rsize > 0) {
		row->hl = malloc((size_t)row->rsize);
		if (row->hl == NULL)
			die("malloc");
		memset(row->hl, HL_NORMAL, (size_t)row->rsize);
	}

	if (E.syntax == NULL)
		return;

	keywords = E.syntax->keywords;
	scs = E.syntax->singleline_comment_start;
	mcs = E.syntax->multiline_comment_start;
	mce = E.syntax->multiline_comment_end;
	scs_len = scs != NULL ? (int)strlen(scs) : 0;
	mcs_len = mcs != NULL ? (int)strlen(mcs) : 0;
	mce_len = mce != NULL ? (int)strlen(mce) : 0;

	in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

	while (i < row->rsize) {
		char c = row->render[i];
		unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

		/* A line comment swallows the rest of the line, but not if it
		   is really inside a string or a block comment. */
		if (scs_len > 0 && in_string == 0 && !in_comment) {
			if (strncmp(&row->render[i], scs, (size_t)scs_len) == 0) {
				memset(&row->hl[i], HL_COMMENT,
				       (size_t)(row->rsize - i));
				break;
			}
		}

		if (mcs_len > 0 && mce_len > 0 && in_string == 0) {
			if (in_comment) {
				row->hl[i] = HL_MLCOMMENT;
				if (strncmp(&row->render[i], mce,
					    (size_t)mce_len) == 0) {
					memset(&row->hl[i], HL_MLCOMMENT,
					       (size_t)mce_len);
					i += mce_len;
					in_comment = 0;
					prev_sep = 1;
					continue;
				}
				i++;
				continue;
			} else if (strncmp(&row->render[i], mcs,
					   (size_t)mcs_len) == 0) {
				memset(&row->hl[i], HL_MLCOMMENT,
				       (size_t)mcs_len);
				i += mcs_len;
				in_comment = 1;
				continue;
			}
		}

		if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
			if (in_string != 0) {
				row->hl[i] = HL_STRING;
				/* A backslash escapes the next byte, so a quote
				   straight after one closes nothing. */
				if (c == '\\' && i + 1 < row->rsize) {
					row->hl[i + 1] = HL_STRING;
					i += 2;
					continue;
				}
				if (c == in_string)
					in_string = 0;
				i++;
				prev_sep = 1;
				continue;
			} else if (c == '"' || c == '\'') {
				in_string = c;
				row->hl[i] = HL_STRING;
				i++;
				continue;
			}
		}

		if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
			/* Only count a digit that starts a word, or carries on
			   one already being highlighted, so the 2 in "x2" stays
			   plain. The dot keeps decimals in one piece. */
			if ((isdigit((unsigned char)c) &&
			     (prev_sep || prev_hl == HL_NUMBER)) ||
			    (c == '.' && prev_hl == HL_NUMBER)) {
				row->hl[i] = HL_NUMBER;
				i++;
				prev_sep = 0;
				continue;
			}
		}

		if (prev_sep) {
			int j;

			for (j = 0; keywords[j] != NULL; j++) {
				int klen = (int)strlen(keywords[j]);
				int kw2 = keywords[j][klen - 1] == '|';

				if (kw2)
					klen--;

				/* The character after has to be a separator too,
				   or "int" would light up inside "integer".
				   render is NUL-terminated, so reading one past
				   the last byte is safe and counts as one. */
				if (strncmp(&row->render[i], keywords[j],
					    (size_t)klen) == 0 &&
				    is_separator(row->render[i + klen])) {
					memset(&row->hl[i],
					       kw2 ? HL_KEYWORD2 : HL_KEYWORD1,
					       (size_t)klen);
					i += klen;
					break;
				}
			}
			if (keywords[j] != NULL) {
				prev_sep = 0;
				continue;
			}
		}

		prev_sep = is_separator(c);
		i++;
	}

	changed = (row->hl_open_comment != in_comment);
	row->hl_open_comment = in_comment;

	/* The comment state at the end of this line changed, so every line
	   below was highlighted on a stale assumption. Recursion stops as soon
	   as a line's state comes out the same as before. */
	if (changed && row->idx + 1 < E.numrows)
		editor_update_syntax(&E.row[row->idx + 1]);
}

static int editor_syntax_to_color(int hl)
{
	switch (hl) {
	case HL_COMMENT:
	case HL_MLCOMMENT:	return 36;	/* cyan */
	case HL_KEYWORD1:	return 33;	/* yellow */
	case HL_KEYWORD2:	return 32;	/* green */
	case HL_STRING:		return 35;	/* magenta */
	case HL_NUMBER:		return 31;	/* red */
	case HL_MATCH:		return 34;	/* blue */
	default:		return 37;	/* white */
	}
}

static void editor_select_syntax_highlight(void)
{
	unsigned int entry;
	char *ext;

	E.syntax = NULL;
	if (E.filename == NULL)
		return;

	ext = strrchr(E.filename, '.');

	for (entry = 0; entry < HLDB_ENTRIES; entry++) {
		struct editor_syntax *s = &HLDB[entry];
		int i = 0;

		while (s->filematch[i] != NULL) {
			int is_ext = (s->filematch[i][0] == '.');

			if ((is_ext && ext != NULL &&
			     strcmp(ext, s->filematch[i]) == 0) ||
			    (!is_ext &&
			     strstr(E.filename, s->filematch[i]) != NULL)) {
				int row;

				E.syntax = s;
				/* Rows read before the filetype was known were
				   highlighted as plain text. */
				for (row = 0; row < E.numrows; row++)
					editor_update_syntax(&E.row[row]);
				return;
			}
			i++;
		}
	}
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

	editor_update_syntax(row);
}

static void editor_insert_row(int at, const char *s, size_t len)
{
	erow *grown;
	int j;

	if (at < 0 || at > E.numrows)
		return;

	grown = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	if (grown == NULL)
		die("realloc");
	E.row = grown;

	/* Open a gap at `at` by sliding everything below it down one slot. */
	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
	/* Those rows all moved, and the highlighter navigates by idx. */
	for (j = at + 1; j <= E.numrows; j++)
		E.row[j].idx = j;

	E.row[at].idx = at;
	E.row[at].size = (int)len;
	E.row[at].chars = malloc(len + 1);
	if (E.row[at].chars == NULL)
		die("malloc");
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	E.row[at].hl = NULL;
	E.row[at].hl_open_comment = 0;

	/* numrows has to be right before highlighting runs, because a block
	   comment cascade walks forward through the rows. */
	E.numrows++;
	editor_update_row(&E.row[at]);

	E.dirty++;
}

static void editor_free_row(erow *row)
{
	free(row->render);
	free(row->chars);
	free(row->hl);
}

static void editor_del_row(int at)
{
	int j;

	if (at < 0 || at >= E.numrows)
		return;

	editor_free_row(&E.row[at]);
	memmove(&E.row[at], &E.row[at + 1],
		sizeof(erow) * (E.numrows - at - 1));
	E.numrows--;
	for (j = at; j < E.numrows; j++)
		E.row[j].idx = j;

	/* The line that closed a block comment may have just gone. */
	if (at < E.numrows)
		editor_update_syntax(&E.row[at]);

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

/* Delete the character sitting under the cursor - vim's x. */
static void editor_del_char_forward(void)
{
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

	if (row == NULL || E.cx >= row->size)
		return;

	editor_row_del_char(row, E.cx);
	if (E.cx > row->size)
		E.cx = row->size;
}

/* Delete the whole line - vim's dd. */
static void editor_del_current_row(void)
{
	if (E.cy >= E.numrows)
		return;

	editor_del_row(E.cy);
	if (E.cy >= E.numrows && E.cy > 0)
		E.cy--;
	E.cx = 0;
}

static int is_word_char(int c)
{
	return isalnum(c) || c == '_';
}

static void editor_move_word_forward(void)
{
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

	if (row == NULL)
		return;

	/* Step off whatever word we are in, then across the gap after it. */
	while (E.cx < row->size &&
	       is_word_char((unsigned char)row->chars[E.cx]))
		E.cx++;
	while (E.cx < row->size &&
	       !is_word_char((unsigned char)row->chars[E.cx]))
		E.cx++;

	/* Ran off the end, so carry on from the start of the next line. */
	if (E.cx >= row->size && E.cy < E.numrows - 1) {
		E.cy++;
		E.cx = 0;
	}
}

static void editor_move_word_back(void)
{
	erow *row;

	if (E.cx == 0) {
		if (E.cy == 0)
			return;
		E.cy--;
		E.cx = E.row[E.cy].size;
	}

	row = &E.row[E.cy];
	while (E.cx > 0 && !is_word_char((unsigned char)row->chars[E.cx - 1]))
		E.cx--;
	while (E.cx > 0 && is_word_char((unsigned char)row->chars[E.cx - 1]))
		E.cx--;
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

	/* Pick the filetype before reading, so rows are highlighted as they
	   come in rather than needing a second pass. */
	editor_select_syntax_highlight();

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
		E.filename = editor_prompt("Save as: %s (ESC cancels)", NULL);
		if (E.filename == NULL) {
			editor_set_status_message("Save cancelled");
			return;
		}
		/* The new name may well have told us the filetype. */
		editor_select_syntax_highlight();
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

/* ---------- search ---------- */

/* Drop the search highlight by simply re-running the highlighter over the
   row that is wearing it. */
static void editor_clear_match_highlight(void)
{
	if (E.match_row >= 0 && E.match_row < E.numrows)
		editor_update_syntax(&E.row[E.match_row]);
	E.match_row = -1;
}

/* Jump to the next line matching `query`, starting one line off from where
   the cursor is. Only the first hit on a line is found, which is enough for
   stepping through a file. */
static int editor_search_from(const char *query, int from, int direction)
{
	int current = from;
	int i;

	if (E.numrows == 0)
		return 0;

	for (i = 0; i < E.numrows; i++) {
		char *match;
		erow *row;

		current += direction;
		if (current == -1)
			current = E.numrows - 1;
		else if (current == E.numrows)
			current = 0;

		row = &E.row[current];
		match = strstr(row->chars, query);
		if (match == NULL)
			continue;

		editor_clear_match_highlight();

		E.cy = current;
		E.cx = (int)(match - row->chars);
		/* Shove rowoff past the end so the next scroll() pass brings
		   the match onto the screen. */
		E.rowoff = E.numrows;

		/* hl is indexed by render, so the chars offset has to be
		   converted first. A query containing a tab would measure the
		   wrong width here, which is not worth handling. */
		if (row->hl != NULL) {
			int rx = editor_row_cx_to_rx(row, E.cx);
			int n = (int)strlen(query);

			if (rx + n > row->rsize)
				n = row->rsize - rx;
			if (n > 0)
				memset(&row->hl[rx], HL_MATCH, (size_t)n);
			E.match_row = current;
		}

		return 1;
	}

	return 0;
}

static void editor_find_callback(char *query, int key)
{
	static int last_match = -1;
	static int direction = 1;

	if (key == '\r' || key == '\x1b') {
		last_match = -1;
		direction = 1;
		return;
	} else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
		direction = 1;
	} else if (key == ARROW_LEFT || key == ARROW_UP) {
		direction = -1;
	} else {
		/* The query changed, so start over from the top. */
		last_match = -1;
		direction = 1;
	}

	if (last_match == -1)
		direction = 1;

	if (query[0] == '\0')
		return;

	if (editor_search_from(query, last_match, direction))
		last_match = E.cy;
}

static void editor_find(void)
{
	int saved_cx = E.cx;
	int saved_cy = E.cy;
	int saved_coloff = E.coloff;
	int saved_rowoff = E.rowoff;
	char *query;

	query = editor_prompt("Search: %s (arrows to step, ESC cancels)",
			      editor_find_callback);

	if (query != NULL) {
		free(E.last_query);
		E.last_query = query;		/* kept for n and N */
	} else {
		/* Cancelled, so put the view back exactly as it was. */
		editor_clear_match_highlight();
		E.cx = saved_cx;
		E.cy = saved_cy;
		E.coloff = saved_coloff;
		E.rowoff = saved_rowoff;
	}
}

static void editor_find_next(int direction)
{
	if (E.last_query == NULL) {
		editor_set_status_message("No previous search");
		return;
	}

	if (!editor_search_from(E.last_query, E.cy, direction))
		editor_set_status_message("Not found: %s", E.last_query);
}

/* ---------- ex commands ---------- */

static void editor_run_command(void)
{
	char *cmd = editor_prompt(":%s", NULL);

	if (cmd == NULL)
		return;

	if (strcmp(cmd, "w") == 0) {
		editor_save();
	} else if (strcmp(cmd, "q") == 0) {
		if (E.dirty > 0)
			editor_set_status_message(
				"Unsaved changes - :q! discards them");
		else
			editor_exit();
	} else if (strcmp(cmd, "q!") == 0) {
		editor_exit();
	} else if (strcmp(cmd, "wq") == 0 || strcmp(cmd, "x") == 0) {
		editor_save();
		if (E.dirty == 0)
			editor_exit();
	} else {
		editor_set_status_message("Not a command: :%s", cmd);
	}

	free(cmd);
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

			if (len > 0) {
				const char *c =
					&E.row[filerow].render[E.coloff];
				const unsigned char *hl =
					&E.row[filerow].hl[E.coloff];
				int current_color = -1;
				int j;

				for (j = 0; j < len; j++) {
					if (hl[j] == HL_NORMAL) {
						if (current_color != -1) {
							ab_append(ab,
								  "\x1b[39m",
								  5);
							current_color = -1;
						}
					} else {
						int color =
						    editor_syntax_to_color(hl[j]);

						if (color != current_color) {
							char seq[16];
							int slen;

							slen = snprintf(
								seq,
								sizeof(seq),
								"\x1b[%dm",
								color);
							current_color = color;
							ab_append(ab, seq, slen);
						}
					}
					ab_append(ab, &c[j], 1);
				}
				/* Leave the line in the default colour so the
				   status bar below is not tinted. */
				ab_append(ab, "\x1b[39m", 5);
			}
		}

		/* Erase whatever used to be on the rest of this line. Cheaper
		   and flicker-free compared to clearing the whole screen. */
		ab_append(ab, "\x1b[K", 3);
		ab_append(ab, "\r\n", 2);
	}
}

static void editor_draw_status_bar(struct abuf *ab)
{
	char left[128];
	char right[48];
	int llen, rlen;

	ab_append(ab, "\x1b[7m", 4);		/* swap foreground and back */

	llen = snprintf(left, sizeof(left), "%.40s%s  %s",
			E.filename != NULL ? E.filename : "[no name]",
			E.dirty > 0 ? " [+]" : "",
			E.mode == MODE_INSERT ? "-- INSERT --" : "-- NORMAL --");
	rlen = snprintf(right, sizeof(right), "%s | %d/%d",
			E.syntax != NULL ? E.syntax->filetype : "no ft",
			E.cy + 1, E.numrows);

	if (llen < 0)
		llen = 0;
	if (rlen < 0)
		rlen = 0;
	if (llen > E.screencols)
		llen = E.screencols;
	ab_append(ab, left, llen);

	while (llen < E.screencols) {
		/* Pad with spaces, dropping the position in once it lines up
		   with the right-hand edge. */
		if (E.screencols - llen == rlen) {
			ab_append(ab, right, rlen);
			break;
		}
		ab_append(ab, " ", 1);
		llen++;
	}

	ab_append(ab, "\x1b[m", 3);		/* back to normal video */
	ab_append(ab, "\r\n", 2);
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
	editor_draw_status_bar(&ab);
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

/* Read a line from the user on the message bar.
 *
 * `prompt` is a format string with one %s in it, which is where the text so
 * far gets shown. If `callback` is given it runs after every keystroke, which
 * is what makes search update as you type. Returns NULL if cancelled; the
 * caller owns the string otherwise. */
static char *editor_prompt(const char *prompt, void (*callback)(char *, int))
{
	size_t bufsize = 128;
	size_t buflen = 0;
	char *buf = malloc(bufsize);

	if (buf == NULL)
		die("malloc");
	buf[0] = '\0';

	for (;;) {
		int c;

		editor_set_status_message(prompt, buf);
		editor_refresh_screen();

		c = editor_read_key();

		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
			if (buflen != 0)
				buf[--buflen] = '\0';
		} else if (c == '\x1b') {
			editor_set_status_message("");
			if (callback != NULL)
				callback(buf, c);
			free(buf);
			return NULL;
		} else if (c == '\r') {
			if (buflen != 0) {
				editor_set_status_message("");
				if (callback != NULL)
					callback(buf, c);
				return buf;
			}
		} else if (!iscntrl(c) && c < 1000) {
			if (buflen == bufsize - 1) {
				char *grown;

				bufsize *= 2;
				grown = realloc(buf, bufsize);
				if (grown == NULL)
					die("realloc");
				buf = grown;
			}
			buf[buflen++] = (char)c;
			buf[buflen] = '\0';
		}

		if (callback != NULL)
			callback(buf, c);
	}
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

static void editor_page(int key)
{
	int times = E.screenrows;

	/* Park the cursor at the top or bottom of the current view first, then
	   walk a whole screen from there. Going through the arrow logic keeps
	   the end-of-line clamping consistent. */
	if (key == PAGE_UP) {
		E.cy = E.rowoff;
	} else {
		E.cy = E.rowoff + E.screenrows - 1;
		if (E.cy > E.numrows)
			E.cy = E.numrows;
	}

	while (times-- > 0)
		editor_move_cursor(key == PAGE_UP ? ARROW_UP : ARROW_DOWN);
}

/* Keys that mean the same thing whichever mode we are in. Returns 1 if the
   key was dealt with here. */
static int editor_handle_common_key(int c)
{
	switch (c) {
	case CTRL_KEY('s'):
		editor_save();
		return 1;

	case CTRL_KEY('f'):
		editor_find();
		return 1;

	case ARROW_UP:
	case ARROW_DOWN:
	case ARROW_LEFT:
	case ARROW_RIGHT:
		editor_move_cursor(c);
		return 1;

	case HOME_KEY:
		E.cx = 0;
		return 1;

	case END_KEY:
		if (E.cy < E.numrows)
			E.cx = E.row[E.cy].size;
		return 1;

	case PAGE_UP:
	case PAGE_DOWN:
		editor_page(c);
		return 1;

	case CTRL_KEY('l'):	/* redraw: the main loop already does one */
		return 1;
	}

	return 0;
}

static void editor_process_insert_key(int c)
{
	switch (c) {
	case '\x1b':
		E.mode = MODE_NORMAL;
		break;

	case '\r':
		editor_insert_newline();
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

	default:
		/* Control codes we have not claimed would show up as garbage,
		   so only printable bytes get inserted. Bytes above 127 are
		   kept so UTF-8 text at least survives a round trip, even
		   though the column maths still counts them one per byte. */
		if (!iscntrl(c))
			editor_insert_char(c);
		break;
	}
}

static void editor_process_normal_key(int c)
{
	/* g and d both need a second keystroke before they mean anything. */
	if (E.pending != 0) {
		int first = E.pending;

		E.pending = 0;
		if (first == 'g' && c == 'g') {
			E.cy = 0;
			E.cx = 0;
		} else if (first == 'd' && c == 'd') {
			editor_del_current_row();
		}
		return;
	}

	switch (c) {
	case 'h': editor_move_cursor(ARROW_LEFT); break;
	case 'j': editor_move_cursor(ARROW_DOWN); break;
	case 'k': editor_move_cursor(ARROW_UP); break;
	case 'l': editor_move_cursor(ARROW_RIGHT); break;

	case '0':
		E.cx = 0;
		break;
	case '$':
		if (E.cy < E.numrows)
			E.cx = E.row[E.cy].size;
		break;

	case 'g':
	case 'd':
		E.pending = c;
		break;

	case 'G':
		E.cy = E.numrows > 0 ? E.numrows - 1 : 0;
		E.cx = 0;
		break;

	case 'w': editor_move_word_forward(); break;
	case 'b': editor_move_word_back(); break;

	case 'x': editor_del_char_forward(); break;

	case 'i':
		E.mode = MODE_INSERT;
		break;
	case 'a':
		if (E.cy < E.numrows && E.cx < E.row[E.cy].size)
			E.cx++;
		E.mode = MODE_INSERT;
		break;
	case 'I':
		E.cx = 0;
		E.mode = MODE_INSERT;
		break;
	case 'A':
		if (E.cy < E.numrows)
			E.cx = E.row[E.cy].size;
		E.mode = MODE_INSERT;
		break;
	case 'o':
		/* Splitting at the end of a line leaves a blank one below it
		   with the cursor already there. */
		if (E.cy < E.numrows)
			E.cx = E.row[E.cy].size;
		editor_insert_newline();
		E.mode = MODE_INSERT;
		break;
	case 'O':
		/* Splitting at column 0 pushes a blank line above, and leaves
		   the cursor on the original line, so step back up onto it. */
		E.cx = 0;
		editor_insert_newline();
		if (E.cy > 0)
			E.cy--;
		E.mode = MODE_INSERT;
		break;

	case '/': editor_find(); break;
	case 'n': editor_find_next(1); break;
	case 'N': editor_find_next(-1); break;

	case ':': editor_run_command(); break;

	case BACKSPACE:
		editor_move_cursor(ARROW_LEFT);
		break;
	}
}

static void editor_process_keypress(void)
{
	static int quit_times = CED_QUIT_TIMES;
	int c = editor_read_key();

	/* Ctrl-Q is the only key that cares about its own repeat count, so it
	   returns early and skips the reset at the bottom. */
	if (c == CTRL_KEY('q')) {
		if (E.dirty > 0 && quit_times > 0) {
			editor_set_status_message(
				"%d unsaved change%s. Press Ctrl-Q %d more "
				"time%s to quit anyway.",
				E.dirty, E.dirty == 1 ? "" : "s",
				quit_times, quit_times == 1 ? "" : "s");
			quit_times--;
			return;
		}
		editor_exit();
	}

	if (editor_handle_common_key(c)) {
		/* A shared key in the middle of dd or gg abandons it. */
		E.pending = 0;
	} else if (E.mode == MODE_INSERT) {
		editor_process_insert_key(c);
	} else {
		editor_process_normal_key(c);
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
	E.mode = MODE_NORMAL;
	E.pending = 0;
	E.filename = NULL;
	E.last_query = NULL;
	E.match_row = -1;
	E.syntax = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;

	if (get_window_size(&E.screenrows, &E.screencols) == -1)
		die("get_window_size");

	/* The bottom two lines are the status bar and the message bar. */
	E.screenrows -= 2;
}

int main(int argc, char *argv[])
{
	enable_raw_mode();
	init_editor();
	if (argc >= 2)
		editor_open(argv[1]);

	editor_set_status_message(
		"i = insert | ESC = normal | / = search | :w :q | Ctrl-S Ctrl-Q");

	for (;;) {
		editor_refresh_screen();
		editor_process_keypress();
	}

	return 0;
}
