#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8

/* my little addition */
#define die(s) __die((s), (__func__), (__LINE__))
#define CTRL_KEY(k) ((k) & 0x1f)
#define ABUF_INIT {NULL, 0}

enum editor_mode {
	MODE_NORMAL,
	MODE_INSERT
};

enum editor_key {
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN,
};

struct erow {
	int size;
	int rsize;
	char *chars;
	char *render;
};

struct editor_state {
	struct termios default_termios;
	char *filename;
	char statusmsg[80];
	time_t statusmsg_time;
	int mode;
	int scr_rows, scr_cols;
	int cx, cy;
	int rx;
	int numrows;
	int rowoff;
	int coloff;
	struct erow *row;
};

struct abuf {
	char *b;
	int len;
};

struct editor_state estate;

void abuf_append(struct abuf *ab, const char *s, int len)
{
	char *new = realloc(ab->b, ab->len + len);
	if (!new)
		return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abuf_free(struct abuf *ab)
{
	free(ab->b);
}

void clear_screen()
{
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
}

void __die(const char *s, const char *f, const int l)
{
	clear_screen();
	printf("%s:%d - ", f, l);
	fflush(stdout);
	perror(s);
	exit(1);
}

/* see https://vt100.net/docs/vt100-ug/chapter3.html */
/* restore original mode */
void disable_raw_mode()
{
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH , &estate.default_termios) == -1)
		die("tcsetattr");
}

/* prepare terminal to work with our thingy, disables echo and canon mode and
 * some other niceties 
 * in order:
 * 	BRKINT	a "break condition" whatever that means will not $ kill -2 
 * 	ICRNL	dont change \r to \n on input
 * 	INPCK	diable parity checking (this is very baud lain navi)
 * 	ISTRIP	8th bit is being turned off by default. just so you dont start
 * 		thinking that interpreting only 7 bytes (ascii) in an 8 byte 
 * 		value (char on every computer on earth) was a good idea
 * 		(we turn this off)
 * 	IXON	disable ctrl+s (suspend input) and ctrl+q (resume input)
 *
 * 	OPOST 	dont change \n to \r\n on output
 *
 * 	ECHO	dont echo, duh
 * 	ICANON	dont wait for newline
 *	IEXTEN	disable ctrl-v
 *	ISIG	disable ctrl-c and ctrl-z
 *
 *	c_cc[VMIN]	minimum number of bytes to ret for read()
 *	c_cc[VTIME]	read() timeout
 */
void enable_raw_mode()
{
	if (tcgetattr(STDIN_FILENO, &estate.default_termios) == -1)
		die("tcsetattr");
	atexit(disable_raw_mode);
	struct termios raw = estate.default_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH , &raw) == -1)
		die("tcsetattr");
}

int editor_read_key()
{
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN)
			die("read");
	}

	if (c == '\x1b') {
		char seq[3];
		if (read(STDIN_FILENO, &seq[0], 1) != 1)
			return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1)
			return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1)
					return '\x1b';
				if (seq[2] == '~')
					switch (seq[1]) {
					case '1': return HOME_KEY;
					case '3': return DEL_KEY;
					case '4': return END_KEY;
					case '5': return PAGE_UP;
					case '6': return PAGE_DOWN;
					case '7': return HOME_KEY;
					case '8': return END_KEY;
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
			switch (seq[1]) {
			case 'H': return HOME_KEY;
			case 'F': return END_KEY;
			}
		}
		return '\x1b';
	}

	return c;
}

int get_cursor_pos(int *rows, int *cols)
{
	char buf[32];
	unsigned int i = 0;
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
		return -1;

	while (i < sizeof(buf)/sizeof(char) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1)
			break;
		if (buf[i] == 'R')
			break;
		i++;
	}
	buf[i] = '\0';
	if (buf[0] != '\x1b' || buf[1] != '[')
		return -1;

	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
		return -1;

	editor_read_key();
	return 0;
}

int get_window_size(int *rows, int *cols)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		/* fallback for no ioctl above */
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
			return -1;
		return get_cursor_pos(rows, cols);
	}

	*cols = ws.ws_col;
	*rows = ws.ws_row;
	return 0;
}

void editor_move_cursor(int key)
{
	struct erow *row = (estate.cy >= estate.numrows) ? NULL : estate.row + estate.cy;

	switch (key) {
	case ARROW_LEFT:
	case 'h':
		estate.cx = estate.cx > 0 ? estate.cx - 1 : 0;
		break;
	case ARROW_DOWN:
	case 'j':
		estate.cy = estate.cy < estate.numrows - 1 ? estate.cy + 1 : estate.scr_rows - 1;
		break;
	case ARROW_UP:
	case 'k':
		estate.cy = estate.cy > 0 ? estate.cy - 1 : 0;
		break;
	case ARROW_RIGHT:
	case 'l':
		if (row)
			estate.cx += estate.cx < row->size;
		break;
	}

	row = (estate.cy >= estate.numrows) ? NULL : estate.row + estate.cy;
	int rowlen = row ? row->size : 0;
	if (estate.cx > rowlen)
		estate.cx = rowlen;
}

void editor_process_keypress()
{
	struct erow *row = (estate.cy >= estate.numrows) ? NULL : estate.row + estate.cy;

	int c = editor_read_key();
	switch (c) {
	case CTRL_KEY('c'):
		break;
	case CTRL_KEY('q'):
		clear_screen();
		exit(0);
		break;
	case 27:
		estate.mode = MODE_NORMAL;
		break;
	case 'i':
		if (estate.mode == MODE_NORMAL)
			estate.mode = MODE_INSERT;
		break;
		
	case PAGE_UP:
	case PAGE_DOWN: {
		int times = estate.scr_rows;
		while (times--)
			editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);

	}
		break;
	case 'h':
	case 'j':
	case 'k':
	case 'l':
		if (estate.mode == MODE_INSERT)
			break;
		__attribute__ ((fallthrough));
	case ARROW_LEFT:
	case ARROW_DOWN:
	case ARROW_UP:
	case ARROW_RIGHT:
		editor_move_cursor(c);
		break;
	case '^':
		if (estate.mode == MODE_INSERT)
			break;
		__attribute__ ((fallthrough));
	case HOME_KEY:
		estate.cx = 0;
		break;
	case '$':
		if (estate.mode == MODE_INSERT)
			break;
		__attribute__ ((fallthrough));
	case END_KEY:
		estate.cx = row->size;
		break;
	}
}

void editor_draw_status_bar(struct abuf *ab)
{
	char status[80], rstatus[80];
	int totallen = 0;
	int len;

	abuf_append(ab, "\x1b[7m", 4);
	char modestr[80];
	switch (estate.mode) {
	case MODE_NORMAL:
		strcpy(modestr, "             ");
		break;
	case MODE_INSERT:
		strcpy(modestr, "-- INSERT -- ");
		break;
	}
	len = strlen(modestr);
	totallen += len;
	if (totallen > estate.scr_cols)
		len = totallen - estate.scr_cols;

	abuf_append(ab, modestr, len);

	len = snprintf(status, sizeof(status), "%.20s - %d lines",
			estate.filename ? estate.filename : "[no name]", estate.numrows);
	totallen += len;
	if (totallen > estate.scr_cols)
		len = totallen - estate.scr_cols;
	abuf_append(ab, status, len);

	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", estate.cy + 1, estate.numrows);

	while (totallen < estate.scr_cols - 1) {
		if (estate.scr_cols - totallen == rlen) {
			abuf_append(ab, rstatus, rlen);
			break;
		}
		abuf_append(ab, " ", 1);
		totallen++;
	}
	abuf_append(ab, "\x1b[m", 3);
	abuf_append(ab, "\r\n", 2);
}

void editor_draw_statusmsg(struct abuf *ab)
{
	abuf_append(ab, "\x1b[K", 3);
	int msglen = strlen(estate.statusmsg);
	if (msglen > estate.scr_cols)
		msglen = estate.scr_cols;
	if (msglen && time(NULL) - estate.statusmsg_time < 5)
		abuf_append(ab, estate.statusmsg, msglen);
}

void editor_draw_rows(struct abuf *ab)
{
	int y;
	for (y = 0; y < estate.scr_rows; y++) {
		int frow = y + estate.rowoff;
		if (y >= estate.numrows) {
			if (estate.numrows == 0&& y == estate.scr_rows / 5) {
				char msg[80];
				int msglen = snprintf(msg,
						      sizeof(msg)/sizeof(char),
						      "vikilo editor %s", KILO_VERSION);
				int pad = (estate.scr_cols - msglen) / 2;
				if (msglen + pad > estate.scr_cols)
					msglen = estate.scr_cols - pad;
				if (pad) {
					abuf_append(ab, "~", 1);
					pad--;
				}
				while (pad--)
					abuf_append(ab, " ", 1);
				abuf_append(ab, msg, msglen);
			} else {
				abuf_append(ab, "~", 1);
			} 
		} else {
			int len = estate.row[frow].rsize - estate.coloff;
			if (len < 0)
				len = 0;
			if (len > estate.scr_cols)
				len = estate.scr_cols;
			abuf_append(ab, estate.row[frow].render + estate.coloff, len);
		}

		/* clr line */
		abuf_append(ab, "\x1b[K", 3);
		abuf_append(ab, "\r\n", 2);
	}
}

int editor_row_cx_to_rx(struct erow *row, int cx)
{
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++) {
		if (row->chars[j] == '\t')
			rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
		rx++;
	}
	return rx;
}

void editor_update_row(struct erow *row)
{
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++)
		tabs += row->chars[j] == '\t';

	free(row->render);
	row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

	int idx = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % KILO_TAB_STOP != 0)
				row->render[idx++] = ' ';
		} else {
			row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editor_append_row(char *s, size_t len)
{
	estate.row = realloc(estate.row, sizeof(struct erow) * (estate.numrows + 1));

	int at = estate.numrows;
	estate.row[at].size = len;
	estate.row[at].chars = malloc(len + 1);
	memcpy(estate.row[at].chars, s, len);
	estate.row[at].chars[len] = '\0';

	estate.row[at].rsize = 0;
	estate.row[at].render = NULL;
	editor_update_row(&estate.row[at]);

	estate.numrows++;

}

void editor_scroll()
{
	estate.rx = estate.cx;
	if (estate.cy < estate.numrows)
		estate.rx = editor_row_cx_to_rx(&estate.row[estate.cy], estate.cx);

	estate.rowoff = estate.cy < estate.rowoff ? estate.cy : estate.rowoff;
	if (estate.cy >= estate.rowoff + estate.scr_rows)
		estate.rowoff = estate.cy - estate.scr_rows + 1;

	estate.coloff = estate.cx < estate.coloff ? estate.cx : estate.coloff;
	if (estate.rx >= estate.coloff + estate.scr_cols)
		estate.coloff = estate.rx - estate.scr_cols + 1;

}

/* see https://vt100.net/docs/vt100-ug/chapter3.html */
void editor_refresh_screen()
{
	editor_scroll();

	struct abuf ab = ABUF_INIT;
	/* disable cursor */
	abuf_append(&ab, "\x1b[?25l", 6);
	/* reset cursor */
	abuf_append(&ab, "\x1b[H", 3);

	editor_draw_rows(&ab);
	editor_draw_status_bar(&ab);
	editor_draw_statusmsg(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf)/sizeof(char), "\x1b[%d;%dH", (estate.cy - estate.rowoff) + 1, (estate.rx - estate.coloff) + 1);
	abuf_append(&ab, buf, strlen(buf));
	/* enable cursor */
	abuf_append(&ab, "\x1b[?25h", 6);
	write(STDOUT_FILENO, ab.b, ab.len);
	abuf_free(&ab);
}

void editor_open(char *filename) {
	free(estate.filename);
	estate.filename = strdup(filename);

	FILE *fp = fopen(filename, "r");
	if (!fp)
		die("fopen");
	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
				       line[linelen - 1] == '\r'))
			linelen--;
		editor_append_row(line, linelen);

	}
	free(line);
	fclose(fp);
}

void editor_set_statusmsg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(estate.statusmsg, sizeof(estate.statusmsg), fmt, ap);
	va_end(ap);
	estate.statusmsg_time = time(NULL);
}

void init_editor()
{
	estate.cx = 0;
	estate.cy = 0;
	estate.rx = 0;
	estate.filename = NULL;
	estate.statusmsg[0] = '\0';
	estate.statusmsg_time = 0;
	estate.mode = MODE_NORMAL;
	estate.numrows = 0;
	estate.rowoff = 0;
	estate.coloff = 0;
	estate.row = NULL;
	if (get_window_size(&estate.scr_rows, &estate.scr_cols) == -1)
		die("get_window_size");
	estate.scr_rows -= 2;
}

int main(int argc, char *argv[])
{
	enable_raw_mode();
	init_editor();
	if (argc >= 2)
		editor_open(argv[1]);

	editor_set_statusmsg("help: ctrl+q to quit");

	while (1) {
		editor_refresh_screen();
		editor_process_keypress();
	}

	return 0;
}
