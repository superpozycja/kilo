#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define KILO_VERSION "0.0.1"
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

struct editor_state {
	struct termios default_termios;
	int mode;
	int scr_rows, scr_cols;
	int cx, cy;
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
	switch (key) {
	case ARROW_LEFT:
	case 'h':
		estate.cx = estate.cx > 0 ? estate.cx - 1 : 0;
		break;
	case ARROW_DOWN:
	case 'j':
		estate.cy = estate.cy < estate.scr_rows - 2 ? estate.cy + 1 : estate.scr_rows - 2;
		break;
	case ARROW_UP:
	case 'k':
		estate.cy = estate.cy > 0 ? estate.cy - 1 : 0;
		break;
	case ARROW_RIGHT:
	case 'l':
		estate.cx = estate.cx < estate.scr_cols - 1? estate.cx + 1 : estate.scr_cols - 1;
		break;
	}
}

void editor_process_keypress()
{
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
		estate.cx = estate.scr_cols - 1;
		break;
	}
}

void editor_draw_rows(struct abuf *ab)
{
	int y;
	for (y = 0; y < estate.scr_rows; y++) {
		if (y == estate.scr_rows / 5) {
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
		/* clr line */
		abuf_append(ab, "\x1b[K", 3);
		/* if (y < estate.scr_rows - 1) */
		abuf_append(ab, "\r\n", 2);
	}
	char modestr[80];
	switch (estate.mode) {
	case MODE_NORMAL:
		strcpy(modestr, "");
		break;
	case MODE_INSERT:
		strcpy(modestr, "-- INSERT --");
		break;
	}
	abuf_append(ab, "\x1b[K", 3);
	abuf_append(ab, modestr, strlen(modestr));
}

/* see https://vt100.net/docs/vt100-ug/chapter3.html */
void editor_refresh_screen()
{
	struct abuf ab = ABUF_INIT;
	/* disable cursor */
	abuf_append(&ab, "\x1b[?25l", 6);
	/* reset cursor */
	abuf_append(&ab, "\x1b[H", 3);
	editor_draw_rows(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf)/sizeof(char), "\x1b[%d;%dH", estate.cy + 1, estate.cx + 1);
	abuf_append(&ab, buf, strlen(buf));
	/* enable cursor */
	abuf_append(&ab, "\x1b[?25h", 6);
	write(STDOUT_FILENO, ab.b, ab.len);
	abuf_free(&ab);
}


void init_editor()
{
	estate.cx = 0;
	estate.cy = 0;
	estate.mode = MODE_NORMAL;
	if (get_window_size(&estate.scr_rows, &estate.scr_cols) == -1)
		die("get_window_size");
}

int main()
{
	enable_raw_mode();
	init_editor();

	while (1) {
		editor_refresh_screen();
		editor_process_keypress();
	}

	return 0;
}
