#ifndef _STDIO_H
#define _STDIO_H
/*
 * stdio.h
 *	My poor little stdio emulation
 */
#include <sys/types.h>

/*
 * A open, buffered file
 */
typedef struct __file {
	ushort f_flags;			/* See below */
	ushort f_fd;			/* File descriptor we use */
	char *f_buf, *f_pos;		/* Buffer */
	int f_bufsz, f_cnt;		/*  ...size, bytes queued */
	struct __file			/* List of all open files */
		*f_next, *f_prev;
} FILE;

/*
 * Bits in f_flags
 */
#define _F_WRITE (1)		/* Can write */
#define _F_READ (2)		/*  ...read */
#define _F_DIRTY (4)		/* Buffer holds new data */
#define _F_EOF (8)		/* Sticky EOF */
#define _F_ERR (16)		/*  ...error */
#define _F_UNBUF (32)		/* Flush after each put */
#define _F_LINE (64)		/*  ...after each newline */
#define _F_SETUP (128)		/* Buffers, etc. set up now */
#define _F_UBUF (256)		/* User-provided buffer */

/*
 * The following in-line functions replace the more classic UNIX
 * approach of gnarly ?: constructs.  I think that inline C in
 * a .h is disgusting, but a step forward from unreadably
 * complex C expressions.
 */

/*
 * These need to be seen before getc()/putc() are defined
 */
extern int fgetc(FILE *), fputc(int, FILE *);

/*
 * getc()
 *	Get a char from a stream
 */
static inline int
getc(FILE *fp)
{
	if (((fp->f_flags & (_F_READ|_F_DIRTY)) != _F_READ) ||
			(fp->f_cnt == 0)) {
		return(fgetc(fp));
	}
	fp->f_cnt -= 1;
	fp->f_pos += 1;
	return(fp->f_pos[-1]);
}

/*
 * putc()
 *	Put a char to a stream
 */
static inline int
putc(char c, FILE *fp)
{
	if (((fp->f_flags & (_F_WRITE|_F_SETUP|_F_DIRTY)) !=
			(_F_WRITE|_F_SETUP)) ||
			(fp->f_cnt >= fp->f_bufsz) ||
			((fp->f_flags & _F_LINE) && (c == '\n'))) {
		return(fputc(c, fp));
	}
	*(fp->f_pos) = c;
	fp->f_pos += 1;
	fp->f_cnt += 1;
	fp->f_flags |= _F_DIRTY;
	return(0);
}

/*
 * Smoke and mirrors
 */
#define getchar() getc(stdin)
#define putchar(c) putc(c, stdout)

/*
 * Pre-allocated stdio structs
 */
extern FILE __iob[3];
#define stdin (&__iob[0])
#define stdout (&__iob[1])
#define stderr (&__iob[2])

/*
 * stdio routines
 */
extern FILE *fopen(char *fname, char *mode),
	*freopen(char *, char *, FILE *),
	*fdopen(int, char *);
extern int fclose(FILE *),
	fread(void *, int, int, FILE *),
	fwrite(void *, int, int, FILE *),
	feof(FILE *), ferror(FILE *),
	fileno(FILE *), ungetc(int, FILE *);
extern off_t fseek(FILE *, off_t, int), ftell(FILE *);
extern char *gets(char *), *fgets(char *, int, FILE *);
extern int puts(char *), fputs(char *, FILE *);
extern void clearerr(FILE *), setbuf(FILE *, char *),
	setbuffer(FILE *, char *, uint);
#ifndef __PRINTF_INTERNAL
/*
 * These prototypes are guarded by an #ifdef so that our actual
 * implementation can add an extra parameter, so that it can take
 * the address of the first arg from the stack.
 *
 * We *should* use varargs/stdargs, but these interfaces are
 * unpleasant.
 */
extern int printf(const char *, ...),
	fprintf(FILE *, const char *, ...),
	sprintf(char *, const char *, ...);
extern int scanf(char *, ...),
	fscanf(FILE *, char *, ...),
	sscanf(char *, char *, ...);
#endif

/*
 * Miscellany
 */
#if !defined(TRUE) && !defined(FALSE)
#define TRUE (1)
#define FALSE (0)
#endif
#define EOF (-1)
#if !defined(MIN) && !defined(MAX)
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif
#define BUFSIZ (4096)
#ifndef NULL
#define NULL (0)
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) (((a) <= (b)) ? (a) : (b))
#endif

#endif /* _STDIO_H */
