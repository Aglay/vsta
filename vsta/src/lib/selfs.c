/*
 * selfs.c
 *	Supporting routines for a server to support select()
 */
#include <sys/fs.h>
#include <stdio.h>
#include <std.h>
#define _SELFS_INTERNAL
#include <select.h>
#include <selfs.h>

static port_t selfs_port = -1;	/* Share a port across all clients */

/*
 * sc_wstat()
 *	Handle FS_WSTAT of "select" and "unselect" messages
 *
 * Returns 0 if it's handled here, 1 if not.
 */
int
sc_wstat(struct msg *m, struct selclient *scp, char *field, char *val)
{
	if (!strcmp(field, "select")) {
		extern port_t path_open();

		if (scp->sc_mask) {
			msg_err(m->m_sender, EBUSY);
			return(0);
		}
		if ((sscanf(val, "%u,%ld,%d,%lu,%*s",
				&scp->sc_mask, &scp->sc_clid,
				&scp->sc_fd, &scp->sc_key) != 4) ||
				 (!scp->sc_mask)) {
			msg_err(m->m_sender, EINVAL);
			return(0);
		}

		/*
		 * TBD: use hostname for clustered case
		 */
		if (selfs_port < 0) {
			selfs_port = path_open("//fs/select", 0);
		}
		if (selfs_port < 0) {
			scp->sc_mask = 0;
			msg_err(m->m_sender, strerror());
			return(0);
		}
		scp->sc_needsel = 1;
		scp->sc_iocount = 0;

	/*
	 * Client no longer uses select() on this connection
	 */
	} else if (!strcmp(field, "unselect")) {
		sc_done(scp);
	} else {
		/*
		 * Not a message *we* handle...
		 */
		return(1);
	}

	/*
	 * Success
	 */
	m->m_arg = m->m_arg1 = m->m_buflen = m->m_nseg = 0;
	msg_reply(m->m_sender, m);
	return(0);
}

/*
 * sc_done()
 *	All done with this client
 */
void
sc_done(struct selclient *scp)
{
	scp->sc_mask = 0;
	scp->sc_needsel = 0;
}

/*
 * sc_event()
 *	Send a select event to the select server
 */
void
sc_event(struct selclient *scp, uint event)
{
	struct select_event se;
	struct msg m;

	/*
	 * If we're seeing new data since the client last did
	 * I/O, send a select event.
	 */
	if (!scp->sc_needsel) {
		return;
	}

	/*
	 * Fill in the event
	 */
	se.se_clid = scp->sc_clid;
	se.se_key = scp->sc_key;
	se.se_index = scp->sc_fd;
	se.se_mask = event;
	se.se_iocount = scp->sc_iocount;

	/*
	 * Send it to the server
	 */
	m.m_op = FS_WRITE;
	m.m_buf = &se;
	m.m_arg = m.m_buflen = sizeof(se);
	m.m_nseg = 1;
	m.m_arg1 = 0;
	(void)msg_send(selfs_port, &m);

	/*
	 * Ok, don't need to bother the client until
	 * they do an I/O for this data.
	 */
	scp->sc_needsel = 0;
}

/*
 * sc_init()
 *	Initialize fields
 *
 * Setting them all to zeroes would appear to suffice, for now
 */
void
sc_init(struct selclient *scp)
{
	bzero(scp, sizeof(*scp));
}
