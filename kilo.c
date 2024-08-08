#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/* my little addition */
#define die(s) __die((s), (__func__))

struct termios default_termios;

void __die(const char *s, const char *f)
{
	printf("%s: ", f);
	fflush(stdout);
	perror(s);
	exit(1);
}

/* restore original mode */
void disable_raw_mode()
{
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH , &default_termios) == -1)
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
	if (tcgetattr(STDIN_FILENO, &default_termios) == -1)
		die("tcsetattr");
	atexit(disable_raw_mode);
	struct termios raw = default_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH , &raw) == -1)
		die("tcsetattr");
}

int main()
{
	enable_raw_mode();

	while (1) {
		char c = '\0';
		if (read(STDIN_FILENO, &c, 1) == -1)
			die("read");
		if (iscntrl(c))
			printf("%d\r\n", c);
		else
			printf("%d ('%c')\r\n", c, c);
		if (c == 3)
			printf("use 'q' to quit c;\n");
		if (c == 'q')
			break;

	}

	return 0;
}
