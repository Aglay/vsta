/*
 * tty.c
 *	Routines for doing I/O to TTY-like devices
 *
 * In VSTa, the TTY driver knows nothing about line editing and
 * so forth--the user code handles it instead.  These are the default
 * routines, which do most of what you'd expect.  They use a POSIX
 * TTY interface and do editing, modes, and so forth right here.
 *
 * This module assumes there's just a single TTY.  It gets harder
 * to associate state back to various file descriptors.  We could
 * stat the device and hash on device/inode, but that would have to
 * be done per fillbuf, which seems wasteful.  This should suffice
 * for most applications; I'll think of something for the real
 * solution.
 */
#include <sys/fs.h>
#include <termios.h>
#include <fdl.h>
#include <std.h>
#include <stdio.h>

/*
 * Current tty state, initialized for line-by-line
 */
static struct termios tty_state = {
	ISTRIP|ICRNL,		/* c_iflag */
	OPOST|ONLCR,		/* c_oflag */
	CS8|CREAD|CLOCAL,	/* c_cflag */
	ECHOE|ECHOK|ECHO|
		ISIG|ICANON,	/* c_lflag */
				/* c_cc[] */
{'\4', '\n', '\10', '\27', '\30', '\3', '\37', '\21', '\23', '\1', 0},
	B9600,			/* c_ispeed */
	B9600			/* c_ospeed */
};

/*
 * Where we stash unconsumed TTY data
 */
typedef struct {
	uint f_cnt;
	char *f_pos;
	uint f_bufsz;
	char f_buf[BUFSIZ];
} TTYBUF;

/*
 * addc()
 *	Stuff another character into the buffer
 */
static void
addc(TTYBUF *fp, char c)
{
	if (fp->f_cnt < fp->f_bufsz) {
		fp->f_buf[fp->f_cnt] = c;
		fp->f_cnt += 1;
	}
}

/*
 * delc()
 *	Remove last character from buffer
 */
static void
delc(TTYBUF *fp)
{
	fp->f_pos -= 1;
	fp->f_cnt -= 1;
}

/*
 * echo()
 *	Dump bytes directly to TTY
 */
static void
echo(char *p)
{
	write(1, p, strlen(p));
}

/*
 * init_port()
 *	Create TTYBUF for per-TTY state
 */
static
init_port(struct port *port)
{
	TTYBUF *t;

	t = port->p_data = malloc(sizeof(TTYBUF));
	if (!t) {
		return(1);
	}
	t->f_cnt = 0;
	t->f_bufsz = BUFSIZ;
	t->f_pos = t->f_buf;
	return(0);
}

/*
 * Macro to do one-time setup of a file descriptor for a TTY
 */
#define SETUP(port) \
	if (port->p_data == 0) { \
		if (init_port(port)) { \
			return(-1); \
		} \
	}

/*
 * __tty_read()
 *	Fill buffer from TTY-type device
 */
__tty_read(struct port *port, void *buf, uint nbyte)
{
	char c, c2;
	struct termios *t = &tty_state;
	char echobuf[2];
	TTYBUF *fp;
	uint cnt = 0;
	struct msg m;
	int error = 0;

	/*
	 * Do one-time setup if needed, get pointer to state info
	 */
	SETUP(port);
	fp = port->p_data;

	/*
	 * Load next buffer if needed
	 */
	if (fp->f_cnt == 0) {
		/*
		 * Get ready to read from scratch
		 */
		echobuf[1] = 0;
		fp->f_cnt = 0;
		fp->f_pos = fp->f_buf;

		/*
		 * Loop getting characters
		 */
		for (;;) {
			/*
			 * Build request
			 */
			m.m_op = FS_READ|M_READ;
			m.m_buf = &c2;
			m.m_arg = m.m_buflen = sizeof(c2);
			m.m_nseg = 1;
			m.m_arg1 = 0;

			/*
			 * Errors are handled below.  Otherwise we
			 * move to a local variable so the optimizer
			 * can leave it in a register if it likes.
			 */
			if (msg_send(port->p_port, &m) != sizeof(c2)) {
				error = -1;
				break;
			}
			c = c2;

			/*
			 * Null char--always store.  Null in c_cc[] means
			 * an * inactive entry, so null characters would
			 * cause some confusion.
			 */
			if (!c) {
				addc(fp, c);
				continue;
			}

			/*
			 * ICRNL--map CR to NL
			 */
			if ((c == '\r') && (t->c_iflag & ICRNL)) {
				c = '\n';
			}

			/*
			 * Erase?
			 */
			if (c == t->c_cc[VERASE]) {
				if (fp->f_cnt < 1) {
					continue;
				}
				delc(fp);
				echo("\b \b");	/* Not right for tab */
				continue;
			}

			/*
			 * Kill?
			 */
			if (c == t->c_cc[VKILL]) {
				echo("\\\r\n");	/* Should be optional */
				fp->f_cnt = 0;
				fp->f_pos = fp->f_buf;
				continue;
			}

			/*
			 * Add the character
			 */
			addc(fp, c);

			/*
			 * Echo?
			 */
			if (t->c_lflag & ECHO) {
				echobuf[0] = c;
				echo(echobuf);
			}

			/*
			 * End of line?
			 */
			if (c == t->c_cc[VEOL]) {
				break;
			}
		}
	}

	/*
	 * I/O errors get caught here
	 */
	if (error && (fp->f_cnt == 0)) {
		return(-1);
	}

	/*
	 * Now that we have a buffer with the needed bytes, copy
	 * out as many as will fit and are available.
	 */
	cnt = MIN(fp->f_cnt, nbyte);
	bcopy(fp->f_pos, buf, cnt);
	fp->f_pos += cnt;
	fp->f_cnt -= cnt;

	return(cnt);
}

/*
 * __tty_write()
 *	Flush buffers to a TTY-type device
 *
 * XXX add the needed CRNL conversion and such.
 */
__tty_write(struct port *port, void *buf, uint nbyte)
{
	struct msg m;

	m.m_op = FS_WRITE;
	m.m_buf = buf;
	m.m_arg = m.m_buflen = nbyte;
	m.m_nseg = 1;
	m.m_arg1 = 0;
	return(msg_send(port->p_port, &m));
}

/*
 * __tty_close()
 *	Free our typing buffer
 */
__tty_close(struct port *port)
{
	free(port->p_data);
	return(0);
}
