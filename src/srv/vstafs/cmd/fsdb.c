/*
 * fsdb.c
 *	A basic filesystem debugger
 */
#include "../vstafs.h"
#include <std.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>

extern char *perm_print();

static int fsfd;	/* Open file descriptor to filesystem */
static char *secbuf;	/* Utility sector buffer */
static char *prog;	/* Name fsdb runs under */

/*
 * rdsec()
 *	Fillin secbuf with the numbered sector
 *
 * Return non-zero on error, 0 on success.
 */
static int
rdsec(daddr_t sec)
{
	off_t o, o2;
	int x;

	o = sec*SECSZ;
	o2 = lseek(fsfd, o, 0);
	if (o2 != o) {
		printf("rdsec: seek failed.  Position %lx -> %lx\n",
			o, o2);
		return(1);
	}
	x = read(fsfd, secbuf, SECSZ);
	if (x != SECSZ) {
		printf("rdsec: read failed.  Got %d\n", x);
		return(1);
	}
	return(0);
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
	int x, col = 0, lines = 0;

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
			if (++lines >= 8) {
				char buf[20];

				lines = 0;
				if (gets(buf) == NULL) {
					exit(0);
				}
				if (buf[0] == 'q') {
					return;
				}
			}
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

/*
 * dump_fs()
 *	Dump initial sector
 */
static void
dump_fs(int argc, char **argv)
{
	struct fs *fs;
	struct alloc *a;
	uint x;

	if (rdsec(BASE_SEC)) {
		return;
	}
	fs = (struct fs *)secbuf;
	printf("fs_magic 0x%lx size %ld extsize %ld free @ %ld\n",
		fs->fs_magic, fs->fs_size, fs->fs_extsize,
		fs->fs_free);
	a = fs->fs_freesecs;
	if (a->a_start) {
		x = 0;
		printf("Pending:");
		while ((x < BASE_FREESECS) && a->a_start) {
			printf(" %lx..%lx",
				a->a_start,
				a->a_start + a->a_len - 1);
			x += 1;
			a += 1;
		}
		printf("\n");
	}
}

/*
 * dump_free()
 *	Dump free extents described by a sector
 */
static void
dump_free(int argc, char **argv)
{
	struct free *f;
	daddr_t sec;
	uint x;
	struct alloc *a;

	if (argc < 1) {
		printf("Usage: free <sec>\n");
		return;
	}
	if (sscanf(argv[0], "%lx", &sec) != 1) {
		printf("Bad sector: %s\n", argv[0]);
		return;
	}
	if (rdsec(sec)) {
		return;
	}
	f = (struct free *)secbuf;
	printf("next @ %lx, nfree %d\n", f->f_next, f->f_nfree);
	a = f->f_free;
	for (x = 0; x < f->f_nfree; ++x,++a) {
		printf(" %lx..%lx", a->a_start, a->a_start + a->a_len - 1);
	}
	printf("\n");
}

/*
 * dump_dir()
 *	Dump a directory entry
 */
static void
dump_dir(int argc, char **argv)
{
	daddr_t sec;
	struct fs_dirent *d;
	uint idx;

	if (argc < 1) {
		printf("Usage: dir <sec>\n");
		return;
	} else if (argc < 2) {
		idx = 0;
	} else {
		if (sscanf(argv[1], "%d", &idx) != 1) {
			printf("Bad index: %s\n", argv[1]);
			return;
		}
	}
	if (sscanf(argv[0], "%lx", &sec) != 1) {
		printf("Bad sector: %s\n", argv[0]);
		return;
	}
	if (rdsec(sec)) {
		return;
	}
	d = (struct fs_dirent *)secbuf;
	while (idx > 0) {
		++d;
		--idx;
	}
	idx = 10;
	while ((char *)d < (secbuf + SECSZ)) {
		printf("Name: '%s' @ 0x%lx\n", d->fs_name, d->fs_clstart);
		if (--idx == 0) {
			break;
		}
		d += 1;
	}
}

/*
 * dump_file()
 *	Dump a file header
 */
static void
dump_file(int argc, char **argv)
{
	daddr_t sec;
	struct fs_file *fs;
	uint x;
	ulong len;

	if (argc < 1) {
		printf("Usage: file <sector>\n");
		return;
	}
	if (sscanf(argv[0], "%lx", &sec) != 1) {
		printf("Bad sector: %s\n", argv[0]);
		return;
	}
	if (rdsec(sec)) {
		return;
	}
	fs = (struct fs_file *)secbuf;
	printf("prev @ 0x%lx, revision %lu, len %lu, type %s\n",
		fs->fs_prev, fs->fs_rev, fs->fs_len,
		(fs->fs_type == FT_DIR) ? "dir" : "file");
	printf("nlink %d prot %s\n",
		fs->fs_nlink, perm_print(&fs->fs_prot));
	len = 0;
	for (x = 0; x < MAXEXT; ++x) {
		struct alloc *a;

		a = &fs->fs_blks[x];
		len += stob(a->a_len);
		printf(" %lx..%lx", a->a_start,
			a->a_start + a->a_len - 1);
		if (len >= fs->fs_len) {
			break;
		}
	}
	printf("\n");
}

/*
 * dump_sec()
 *	Dump a sector's worth of data
 */
static void
dump_sec(int argc, char **argv)
{
	daddr_t sec;

	if (argc < 1) {
		printf("Usage: sec <sector>\n");
		return;
	}
	if (sscanf(argv[0], "%lx", &sec) != 1) {
		printf("Bad sector: %s\n", argv[0]);
		return;
	}
	if (rdsec(sec)) {
		return;
	}
	dump_s(secbuf, SECSZ);
}

/*
 * save_sectors()
 *	Extract the data from sector(s), dump to file
 *
 * Sort of a cheap-o way of lost+found, in fsdb
 */
static void
save_sectors(int argc, char **argv)
{
	daddr_t x, low, high;
	FILE *fp;

	if (argc != 3) {
		printf("Usage is: save <low> <high> <file>\n");
		return;
	}
	(void)sscanf(argv[0], "%lx", &low);
	(void)sscanf(argv[1], "%lx", &high);
	fp = fopen(argv[2], "w");
	if (fp == NULL) {
		perror(argv[2]);
		return;
	}
	for (x = low; x <= high; ++x) {
		if (rdsec(x)) {
			perror(argv[x]);
			fclose(fp);
			unlink(argv[2]);
			return;
		}
		(void)fwrite(secbuf, SECSZ, sizeof(char), fp);
	}
	fclose(fp);
}

/*
 * run()
 *	Do various fsdb actions
 */
static void
run(int argc, char **argv)
{
	char *p;

	p = argv[0];		/* Record cmd, advance args */
	argc -= 1;
	argv += 1;

	if (!strcmp(p, "sec")) {
		dump_sec(argc, argv);
		return;
	}
	if (!strcmp(p, "dir")) {
		dump_dir(argc, argv);
		return;
	}
	if (!strcmp(p, "free")) {
		dump_free(argc, argv);
		return;
	}
	if (!strcmp(p, "fs")) {
		dump_fs(argc, argv);
		return;
	}
	if (!strcmp(p, "file")) {
		dump_file(argc, argv);
		return;
	}
	if (!strcmp(p, "quit") || !strcmp(p, "exit")) {
		exit(0);
	}
	if (!strcmp(p, "save")) {
		save_sectors(argc, argv);
		return;
	}
	printf("Unknown command: '%s'\n", p);
}

/*
 * getline()
 *	Get a line of arbitrary length
 *
 * Returns a malloc()'ed buffer which must be freed by the caller
 * when he's done looking at it.
 */
static char *
getline(void)
{
	char *buf = 0;
	int c, len;

	len = 0;
	while ((c = getchar()) != EOF) {
		char *p;

		if ((c == '\n') || (c == '\r')) {
			break;
		}
		len += 1;
		p = realloc(buf, len+1);
		if (p == 0) {
			free(buf);
			return(0);
		}
		buf = p;
		buf[len-1] = c;
	}
	if (c == EOF) {
		exit(0);
	}
	if (buf) {
		buf[len] = '\0';
	}
	return(buf);
}

/*
 * explode()
 *	Return vectors to each word in given buffer
 *
 * Modifies buffer to null-terminate each "word"
 */
static char **
explode(char *buf)
{
	char **argv = 0, **arg2;
	int len = 0;

	for (;;) {
		/*
		 * Skip forward to next word
		 */
		while (isspace(*buf)) {
			++buf;
		}

		/*
		 * End of string?
		 */
		if (*buf == '\0') {
			break;
		}

		/*
		 * New word--add to vector
		 */
		len += 1;
		arg2 = realloc(argv, (len+1) * sizeof(char *));
		if (arg2 == 0) {
			free(argv);
			return(0);
		}
		argv = arg2;
		argv[len-1] = buf;

		/*
		 * Quoted string--assemble until closing quote
		 */
		if (*buf == '"') {
			argv[len-1] += 1;
			for (++buf; *buf && (*buf != '"'); ++buf) {
				/*
				 * Allow backslash-quoting within
				 * string (to embed quotes, usually)
				 */
				if (*buf == '\\') {
					if (buf[1]) {
						++buf;
					}
				}
			}
		} else {
			/*
			 * Walk to end of word
			 */
			while (*buf && !isspace(*buf)) {
				++buf;
			}
		}

		/*
		 * And null-terminate
		 */
		if (*buf) {
			*buf++ = '\0';
		}
	}

	/*
	 * Add null pointer to end of vectors
	 */
	if (argv) {
		argv[len] = 0;
	}
	return(argv);
}

/*
 * cmd()
 *	Main command loop
 */
static void
cmd(void)
{
	char *line, **argv;
	int argc;

	for (;;) {
		printf("fsdb> "); fflush(stdout);
		line = getline();
		if (line == 0) {
			clearerr(stdin);
			continue;
		}
		argv = explode(line);
		for (argc = 0; argv[argc]; ++argc)
			;
		run(argc, argv);
		free(argv);
		free(line);
	}
}

static void
usage(void)
{
	printf("Usage is: %s [-p] <device>\n", prog);
	exit(1);
}

int
main(int argc, char **argv)
{
	prog = argv[0];
	if ((argc < 2) || (argc > 3)) {
		usage();
	}
	secbuf = malloc(2*SECSZ);
	if (secbuf == 0) {
		perror("fsdb");
		exit(1);
	}
	secbuf = (char *)(((ulong)secbuf + (SECSZ-1)) & ~(SECSZ-1));
	if (argc == 2) {
		if ((fsfd = open(argv[1], 0)) < 0) {
			perror(argv[1]);
			exit(1);
		}
	} else {
		usage();
	}
	cmd();
	return(0);
}
