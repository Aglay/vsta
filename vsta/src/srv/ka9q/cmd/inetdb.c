/*
 * inetdb.c
 *	Test harness for interacting with /inet filesystem
 */
#include <sys/fs.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#include <std.h>
#include <select.h>
#include <termios.h>

extern port_t path_open(), __fd_port();

typedef void (*cmdfun)(char *);

static int fd;		/* fd and port for file we're accessing */
static port_t port;
static FILE *tmpf;

static void my_read(char *), my_stat(char *), my_write(char *),
	my_wstat(char *), my_exit(char *);
static void dump_s(char *, uint), my_sleep(char *), my_dot(char *),
	my_term(char *), my_open(char *);

/*
 * Table of commands
 */
static struct {
	char *c_name;	/* Name of command */
	cmdfun c_fn;	/* Function to process the command */
} cmdtab[] = {
	{".", my_dot},
	{"exit", my_exit},
	{"open", my_open},
	{"quit", my_exit},
	{"read", my_read},
	{"sleep", my_sleep},
	{"stat", my_stat},
	{"term", my_term},
	{"write", my_write},
	{"wstat", my_wstat},
	{0, 0},
};

/*
 * my_open()
 *	Generate a proxyd-type FS_OPEN request
 */
static void
my_open(char *path)
{
	struct msg m;
	int x;

	x = wstat(port, "proxy=1\n");
	if (x < 0) {
		printf("Can't set proxy mode\n");
		return;
	}
	m.m_op = FS_OPEN;
	m.m_arg = ACC_READ | ACC_WRITE;
	m.m_buf = path;
	m.m_buflen = strlen(path)+1;
	m.m_nseg = 1;
	m.m_arg1 = 0;
	x = msg_send(port, &m);
	printf("FS_OPEN returns %d\n", x);
}

/*
 * my_term()
 *	Go into read/write terminal mode
 */
static void
my_term(char *dummy)
{
	int x;
	fd_set rfd;
	char buf[256];
	struct termios told, tnew;

	printf("Terminal mode.  Enter '.' to end.\n");
	tcgetattr(0, &told);
	tnew = told;
	tnew.c_lflag &= ~ICANON;
	tnew.c_cc[VMIN] = 1;
	tnew.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &tnew);
	for (;;) {
		FD_ZERO(&rfd);
		FD_SET(fd, &rfd);
		FD_SET(0, &rfd);
		if (select(fd+1, &rfd, NULL, NULL, NULL) < 0) {
			perror("select");
			exit(1);
		}
		if (FD_ISSET(0, &rfd)) {
			x = read(0, buf, 1);
			if (buf[0] == '.') {
				tcsetattr(0, TCSANOW, &told);
				break;
			}
			write(fd, buf, 1);
		}
		if (FD_ISSET(fd, &rfd)) {
			x = read(fd, buf, sizeof(buf));
			write(1, buf, x);
		}
	}
	printf("Done with terminal mode\n");
}

/*
 * my_sleep()
 *	Sleep for <arg> seconds, default to 1
 */
static void
my_sleep(char *p)
{
	int sec;

	if (p && p[0]) {
		sec = atoi(p);
	} else {
		sec = 1;
	}
	sleep(sec);
}

/*
 * my_exit()
 *	Quit
 */
static void
my_exit(char *p)
{
	if (p && *p && isdigit(*p)) {
		exit(atoi(p));
	}
	exit(0);
}

/*
 * my_read()
 *	Read a buffer of data
 */
static void
my_read(char *p)
{
	char buf[256];
	int cnt, x;
	struct msg m;

	/*
	 * Optional argument is count
	 */
	if (p && *p && isdigit(*p)) {
		cnt = atoi(p);
		if (cnt > sizeof(buf)) {
			printf("(rounded down to %d)\n", cnt);
			cnt = sizeof(buf);
		}
	} else {
		cnt = sizeof(buf);
	}

	/*
	 * Generate read
	 */
	m.m_op = FS_READ | M_READ;
	m.m_nseg = 1;
	m.m_buf = buf;
	m.m_arg = m.m_buflen = cnt;
	m.m_arg1 = 0;
	x = msg_send(port, &m);

	/*
	 * Print results
	 */
	if (x < 0) {
		perror("read");
	}
	printf("Return value %d\n", x);
	if (x >= 0) {
		dump_s(buf, m.m_arg);
	}
}

/*
 * my_write()
 *	Write a string out to the port
 */
static void
my_write(char *p)
{
	struct msg m;
	int x;

	if (!p) {
		return;
	}

	/*
	 * Concatenate a newline unless it's "-n"
	 */
	if (!strncmp(p, "-n ", 3)) {
		p += 3;
	} else {
		strcat(p, "\n");
	}

	/*
	 * Generate write
	 */
	m.m_op = FS_WRITE;
	m.m_nseg = 1;
	m.m_buf = p;
	m.m_arg = m.m_buflen = strlen(p);
	m.m_arg1 = 0;
	x = msg_send(port, &m);

	/*
	 * Result
	 */
	if (x < 0) {
		perror("write");
	}
	printf("Return value is %d\n", x);
}

/*
 * my_stat()
 *	Pull stat strings
 */
static void
my_stat(char *p)
{
	char *field, *ret;

	/*
	 * Pick out particular field?
	 */
	if (!p || !*p) {
		field = 0;
	} else {
		field = p;
	}

	/*
	 * Get stat return
	 */
	ret = rstat(port, field);

	/*
	 * Print result
	 */
	if (ret) {
		printf("%s\n", ret);
	} else {
		printf("failed--errstr is %s\n", strerror());
	}
}

/*
 * my_wstat()
 *	Send out a wstat
 */
static void
my_wstat(char *p)
{
	int x;

	if (!p || !*p) {
		return;
	}
	strcat(p, "\n");
	x = wstat(port, p);
	if (x < 0) {
		perror("wstat");
	}
	printf("Result is %d\n", x);
}

/*
 * my_cmd()
 *	Given command string, look up and invoke handler
 */
static void
my_cmd(char *str)
{
	int x, len, matches = 0, match = 0;
	char *p;

	p = strchr(str, ' ');
	if (p) {
		len = p-str;
	} else {
		len = strlen(str);
	}

	for (x = 0; cmdtab[x].c_name; ++x) {
		if (!strncmp(cmdtab[x].c_name, str, len)) {
			++matches;
			match = x;
		}
	}
	if (matches == 0) {
		printf("No such command\n");
		return;
	}
	if (matches > 1) {
		printf("Ambiguous\n");
		return;
	}
	(*cmdtab[match].c_fn)(p ? p+1 : p);
}

/*
 * my_dot()
 *	Source a file
 */
static void
my_dot(char *p)
{
	if (!p || !*p) {
		return;
	}
	if (tmpf) {
		printf("Can't nest\n");
		return;
	}
	tmpf = fopen(p, "r");
}

int
main(int argc, char **argv)
{
	char *path, buf[128];

	/*
	 * Access the named file
	 */
	if (argc < 2) {
		path = "//net/inet:tcp/clone";
	} else {
		path = argv[1];
	}
	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror(path);
		exit(1);
	}
	port = __fd_port(fd);

	/*
	 * Command loop
	 */
	for (;;) {
		if (tmpf) {
			if (fgets(buf, sizeof(buf), tmpf) == 0) {
				fclose(tmpf);
				tmpf = 0;
				continue;
			}
			buf[strlen(buf)-1] = '\0';
		} else {
			printf("->");
			fflush(stdout);
			if (gets(buf) == 0) {
				exit(0);
			}
		}
		if (buf[0] == '\0') {
			continue;
		}
		my_cmd(buf);
	}
}

/*
 * prpad()
 *	Print hex number with leading 0 padding
 */
static void
prpad(unsigned long n, int len)
{
	char buf[16], *p;
	int x;

	p = buf+16;
	*--p = '\0';
	while (len-- > 0) {
		x = n & 0xF;
		n >>= 4;
		if (x < 10) {
			*--p = x + '0';
		} else {
			*--p = (x-10) + 'a';
		}
	}
	*--p = ' ';
	printf(p);
}

/*
 * dump_s()
 *	Dump strings
 */
static void
dump_s(char *buf, uint count)
{
	int x, col = 0;

	for (x = 0; x < count; ++x) {
		prpad(buf[x] & 0xFF, 2);
		if (++col >= 16) {
			int y;
			char c;

			printf(" ");
			for (y = 0; y < 16; ++y) {
				c = buf[x-15+y];
				if ((c < ' ') || (c >= 0x7F)) {
					c = '.';
				}
				putchar(c);
			}
			printf("\n");
			col = 0;
		}
	}
	if (col) {		/* Partial line */
		int y;

		for (y = col; y < 16; ++y) {
			printf("   ");
		}
		for (y = 0; y < col; ++y) {
			char c;

			c = buf[count-col+y];
			if ((c < ' ') || (c >= 0x7F)) {
				c = '.';
			}
			putchar(c);
		}
		printf("\n");
	}
}
