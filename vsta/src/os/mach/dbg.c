/*
 * dbg.c
 *	A simple/simplistic debug interface
 *
 * Hard-wired to talk out either console or display.  If KDB isn't
 * configured, only the parts needed to display kernel printf()'s
 * are compiled in.
 */

extern void cons_putc(int);
static int col = 0;		/* When to wrap */

#ifdef KDB
extern int cons_getc(void);
static char buf[80];		/* Typing buffer */
#endif

/*
 * putchar()
 *	Write a character to the debugger port
 *
 * Brain damage as my serial terminal doesn't wrap columns.
 */
void
putchar(int c)
{
	if (c == '\n') {
		col = 0;
		cons_putc('\r');
	} else {
		if (++col >= 78) {
			cons_putc('\r'); cons_putc('\n');
			col = 1;
		}
	}
	cons_putc(c);
}

#ifdef KDB
/*
 * getchar()
 *	Get a character from the debugger port
 */
getchar(void)
{
	char c;

	c = cons_getc() & 0x7F;
	if (c == '\r')
		c = '\n';
	return(c);
}

/*
 * gets()
 *	A spin-oriented "get line" routine
 */
void
gets(char *p)
{
	char c;
	char *start = p;

	putchar('>');
	for (;;) {
		c = getchar();
		if (c == '\b') {
			if (p > start) {
				printf("\b \b");
				p -= 1;
			}
		} else if (c == '') {
			p = start;
			printf("\\\n");
		} else {
			putchar(c);
			if (c == '\n')
				break;
			*p++ = c;
		}
	}
	*p = '\0';
}

/*
 * dbg_enter()
 *	Basic interface for debugging
 */
void
dbg_enter(void)
{
	extern void dbg_main(void);

	printf("[Kernel debugger]\n");
	dbg_main();
}
#endif /* KDB */
