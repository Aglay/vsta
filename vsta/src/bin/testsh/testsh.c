/*
 * main.c
 *	Main routines for test shell
 */
#include <sys/types.h>
#include <sys/ports.h>
#include <sys/fs.h>
#include <stdio.h>
#include <ctype.h>
#include <std.h>
#include <fcntl.h>
#include <ctype.h>

extern char *__cwd;	/* Current working dir */
static void cd(), md(), quit(), ls(), pwd(), mount(), cat(), sleep();

/*
 * Table of commands
 */
struct {
	char *c_name;	/* Name of command */
	voidfun c_fn;	/* Function to process the command */
} cmdtab[] = {
	"cat", cat,
	"cd", cd,
	"chdir", cd,
	"exit", quit,
	"ls", ls,
	"md", md,
	"mkdir", md,
	"mount", mount,
	"pwd", pwd,
	"quit", quit,
	"sleep", sleep,
	0, 0
};

/*
 * sleep()
 *	Pause the requested amount of time
 */
static void
sleep(char *p)
{
	struct time tm;

	if (!p || !p[0]) {
		printf("Usage: sleep <secs>\n");
		return;
	}
	time_get(&tm);
	printf("Time now: %d sec / %d usec\n", tm.t_sec, tm.t_usec);
	tm.t_sec += atoi(p);
	time_sleep(&tm);
	printf("Back from sleep\n");
}

/*
 * md()
 *	Make a directory
 */
static void
md(char *p)
{
	if (!p || !p[0]) {
		printf("Usage is: mkdir <path>\n");
		return;
	}
	while (isspace(*p)) {
		++p;
	}
	if (mkdir(p) < 0) {
		perror(p);
	}
}

/*
 * cat()
 *	File I/O
 */
static void
cat(char *p)
{
	char *name, *val;
	int fd, x, output = 0;
	char buf[128];

	if (!p || !p[0]) {
		printf("Usage: cat [>] <name>\n");
		return;
	}
	if (*p == '>') {
		output = 1;
		++p;
		while (isspace(*p)) {
			++p;
		}
	}
	if (!p[0]) {
		printf("Missing filename\n");
		return;
	}
	name = p;
	fd = open(name, output ? (O_WRITE|O_CREAT) : O_READ, 0);
	if (fd < 0) {
		perror(name);
		return;
	}
	if (output) {
		while ((x = read(0, buf, sizeof(buf))) > 0) {
			if (buf[0] == '\n') {
				break;
			}
			write(fd, buf, x);
		}
	} else {
		while ((x = read(fd, buf, sizeof(buf))) > 0) {
			write(1, buf, x);
		}
	}
	close(fd);
}

/*
 * mount()
 *	Mount the given port number in a slot
 */
static void
mount(char *p)
{
	int port, x;

	while (isspace(*p)) {
		++p;
	}
	port = atoi(p);
	port = msg_connect(port, ACC_READ);
	if (port < 0) {
		printf("Bad port.\n");
		return;
	}
	p = strchr(p, ' ');
	if (!p) {
		printf("Usage: mount <port> <path>\n");
		return;
	}
	while (isspace(*p)) {
		++p;
	}
	x = mountport(p, port);
	if (x < 0) {
		perror(p);
	}
}

/*
 * quit()
 *	Bye bye
 */
static void
quit(void)
{
	exit(0);
}

/*
 * cd()
 *	Change directory
 */
static void
cd(char *p)
{
	while (isspace(*p)) {
		++p;
	}
	if (chdir(p) < 0) {
		perror(p);
	} else {
		printf("New dir: %s\n", __cwd);
	}
}

/*
 * pwd()
 *	Print current directory
 */
static void
pwd(void)
{
	printf("%s\n", __cwd);
}

/*
 * ls_l()
 *	Do long listing of an entry
 */
static void
ls_l(char *p)
{
	int fd, first;
	char buf[MAXSTAT];
	struct msg m;
	char *cp;

	fd = open(p, O_READ);
	if (fd < 0) {
		perror(p);
		return;
	}
	m.m_op = FS_STAT|M_READ;
	m.m_buf = buf;
	m.m_arg = m.m_buflen = MAXSTAT;
	m.m_nseg = 1;
	m.m_arg1 = 0;
	if (msg_send(__fd_port(fd), &m) <= 0) {
		printf("%s: stat failed\n", p);
		close(fd);
		return;
	}
	close(fd);
	printf("%s: ", p);
	cp = buf;
	first = 1;
	while (p = strchr(cp, '\n')) {
		*p++ = '\0';
		if (first) {
			first = 0;
		} else {
			printf(", ");
		}
		printf("%s", cp);
		cp = p;
	}
	printf("\n");
}

/*
 * ls()
 *	Print contents of current directory
 */
static void
ls(char *p)
{
	int fd, x, l = 0;
	char buf[256];

	/*
	 * Only -l supported
	 */
	if (p && p[0]) {
		if (strcmp(p, "-l")) {
			printf("Usage: ls [-l]\n");
			return;
		}
		l = 1;
	}

	/*
	 * Open current dir
	 */
	fd = open(__cwd, O_READ);
	if (fd < 0) {
		perror(__cwd);
		return;
	}
	while ((x = read(fd, buf, sizeof(buf)-1)) > 0) {
		char *cp;

		buf[x] = '\0';
		if (!l) {
			printf("%s", buf);
			continue;
		}
		cp = buf;
		while (p = strchr(cp, '\n')) {
			*p++ = '\0';
			ls_l(cp);
			cp = p;
		}
	}
	close(fd);
}

/*
 * do_cmd()
 *	Given command string, look up and invoke handler
 */
static void
do_cmd(str)
	char *str;
{
	int x, len, matches = 0, match;
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
 * main()
 *	Get started
 */
main(void)
{
	char buf[128];
	int scrn, kbd;

	kbd = msg_connect(PORT_KBD, ACC_READ);
	(void)__fd_alloc(kbd);
	scrn = msg_connect(PORT_CONS, ACC_WRITE);
	(void)__fd_alloc(scrn);
	(void)__fd_alloc(scrn);

	for (;;) {
		printf("%% "); fflush(stdout);
		gets(buf);
		do_cmd(buf);
	}
}
