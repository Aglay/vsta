/*
 * vsta_srv.c
 *	Offer a VSTa filesystem interface to the KA9Q TCP/IP engine
 *
 * There's more to this than meets the eye.  For outgoing connections
 * and non-clone incoming connections,
 * the client can simply open a port and complete their
 * single point-to-point connection.  Destination IP address and port
 * are specified using FS_WSTAT.
 *
 * However, for servers, a single receiving CLONE port can have many
 * attached clients.  To handle this, an open connection can have
 * a list of connections, with a particular one being the current;
 * the seek position indicates which one.  The position is not
 * changed due to I/O, but can be set using FS_ABS{READ,WRITE}
 * to allow switch-and-I/O in a single message operation.
 *
 * m_arg1 is used to provide
 * standard I/O compatibility if 0, and an efficient read interface
 * for scalable servers if non-zero:
 *
 * A read with m_arg1 == 0 causes the reader to block until data
 * is available.
 *
 * A read with m_arg1 != 0 causes the next available data from any
 * connection to be returned, and m_arg1 is filled in with the connection
 * number.
 *
 * For writes, the current position determines which connection
 * receives the data.
 *
 * Connections are numbered 0..max.  Note that there can be
 * "holes" in the numbering if a client disconnects while other clients
 * exist above.
 */
#include <sys/fs.h>
#include <sys/perm.h>
#include <sys/assert.h>
#include <sys/param.h>
#include <hash.h>
#include <llist.h>
#include <std.h>
#include <alloc.h>
#include <selfs.h>
#include "global.h"
#include "mbuf.h"
#include "netuser.h"
#include "timer.h"
#include "tcp.h"
#include "vsta.h"

#define HASH_SIZE (16)		/* Guess, # clients */
#define MAXIO (64*1024)		/* Max I/O size */

extern int32 ip_addr;		/* Our node's IP address */

static struct hash *clients;	/* /inet filesystem clients */
static struct hash *ports;	/* Port # -> tcp_port mapping */
static port_t serv_port;	/* Our port */
static port_name serv_name;	/*  ...its namer name */
static uint nport;		/* # ports being served */
static uint port_idx;		/* Thread index for port daemon */
static struct llist tbdq;	/* Queue of proxy work pending */

/*
 * Protection of privileged TCP port numbers
 */
static struct prot port_priv = {
	2,
	0,
	{1,	1},
	{0,	ACC_READ|ACC_WRITE|ACC_CHMOD}
};

/*
 * Per-connection TCP state
 */
struct tcp_conn {
	struct tcb *t_tcb;	/* KA9Q's TCB struct */
	struct tcp_port		/* Back-pointer to our tcp_port */
		*t_port;
	uint t_conn;		/*  ...and index in tcp_port's t_conns[] */
	struct llist		/* Reader/writer queues */
		t_readers,
		t_writers,
		t_selectors;	/* List of select() clients */
	struct mbuf *t_readq;	/* Queue of mbufs of data to be consumed */
};

/*
 * Per-port TCP state.  Note that there can be numerous distinct
 * client connections, enumerated via t_conns/t_maxconn
 */
struct tcp_port {
	uint t_refs;		/* # clients attached */
	struct prot t_prot;	/* Protection on this TCP port */
	struct socket
		t_lsock,	/* Local TCP socket */
		t_fsock;	/*  ...proposed remote, for active connect */
	struct tcp_conn
		**t_conns;	/* Connected remote clients */
	ushort t_maxconn,	/* # of slots in t_conns */
		t_proxy;	/* Proxy on remote side? */
	struct llist t_any;	/* Readers for "any" connection */
};

/*
 * Per-client state.  Each client has a TCP port open; multiple
 * clients can have concurrent access to a particular port.  Each
 * client also has a particular connection opened; for the case of
 * a single remote connection, this is always 1.
 */
struct client {
	struct tcp_port
		*c_port;	/* Current port open */
	struct msg		/* Pending I/O, if any */
		c_msg;
	struct llist		/* Queue waiting within */
		*c_entry;
	enum {DIR_ROOT = 0, DIR_TCP, DIR_PORT}
		c_dir;		/* Current dir position */
	uint c_pos;		/* Position, for dir reads */
				/* Connection #, for TCP mode */
	struct perm		/* Client abilities */
		c_perms[PROCPERMS];
	uint c_nperms;
	uint c_perm;		/*  ...as applied to current node */
	uint c_msg_bytes,	/* Assembly of proxy reply message */
		c_body_bytes,
		c_wait_reply;	/*  ...wait for reply? */
	struct selclient
		c_selfs;	/* State for select() applications */
	struct llist		/* Our llist entry as a select client */
		*c_sentry;
};

/*
 * Pending work on client in proxy mode
 */
struct tbd {
	voidfun tbd_fun;		/* Function to call */
	struct msg tbd_msg;		/* Message to process */
	struct client *tbd_cl;		/* Client */
	struct llist *tbd_entry;	/* Queue entry */
};

/*
 * Forward references
 */
static void port_daemon(void),
	inetfs_rcv(struct tcb *, int16),
	inetfs_xmt(struct tcb *, int16),
	inetfs_state(struct tcb *, char, char),
	fill_sendq(struct tcp_conn *c),
	clear_tbd(void);

/*
 * msg_to_mbuf()
 *	Convert VSTa message to mbuf chain
 *
 * Payload is bcopy'd so we can complete the writer async to
 * our outgoing I/O.
 */
static struct mbuf *
msg_to_mbuf(struct msg *m)
{
	uint x;
	struct mbuf *mb, *mtmp;

	mb = NULLBUF;
	for (x = 0; x < m->m_nseg; ++x) {
		seg_t *s;

		s = &m->m_seg[x];
		mtmp = alloc_mbuf(s->s_buflen);
		if (mtmp == NULLBUF) {
			(void)free_p(mb);
			return(0);
		}
		bcopy(s->s_buf, mtmp->data, s->s_buflen);
		mtmp->cnt = mtmp->size = s->s_buflen;
		append(&mb, mtmp);
	}
	return(mb);
}

/*
 * mbuf_to_msg()
 *	Convert mbufs into a VSTa message
 */
static uint
mbuf_to_msg(struct mbuf *mb, struct msg *m, uint size)
{
	uint count, nseg, step, mb_pullup;

	count = nseg = mb_pullup = 0;
	while (size && mb && (nseg < MSGSEGS)) {
		m->m_seg[nseg].s_buf = mb->data;
		if (mb->cnt > size) {
			mb_pullup = step = size;
		} else {
			step = mb->cnt;
		}
		m->m_seg[nseg].s_buflen = step;
		size -= step;
		count += step;
		nseg += 1;
		mb = mb->next;
	}
	m->m_arg = count;
	m->m_nseg = nseg;
	return(mb_pullup);
}

/*
 * inetfs_seek()
 *	Set file position
 */
static void
inetfs_seek(struct msg *m, struct client *cl)
{
	if (m->m_arg < 0) {
		msg_err(m->m_sender, EINVAL);
		return;
	}
	cl->c_pos = m->m_arg;
	m->m_buflen = m->m_arg = m->m_arg1 = m->m_nseg = 0;
	msg_reply(m->m_sender, m);
	cl->c_selfs.sc_iocount += 1;
}

/*
 * new_client()
 *	Create new per-connect structure
 */
static void
new_client(struct msg *m)
{
	struct client *cl;
	struct perm *perms;
	int nperms;

	/*
	 * Get data structure
	 */
	if ((cl = malloc(sizeof(struct client))) == 0) {
		msg_err(m->m_sender, strerror());
		return;
	}

	/*
	 * Initialize fields
	 */
	bzero(cl, sizeof(struct client));
	sc_init(&cl->c_selfs);

	/*
	 * Hash under the sender's handle
	 */
        if (hash_insert(clients, m->m_sender, cl)) {
		free(cl);
		msg_err(m->m_sender, ENOMEM);
		return;
	}

	/*
	 * Record abilities
	 */
	perms = (struct perm *)m->m_buf;
	nperms = (m->m_buflen) / sizeof(struct perm);
	bcopy(perms, &cl->c_perms, nperms * sizeof(struct perm));
	cl->c_nperms = nperms;

	/*
	 * Return acceptance
	 */
	msg_accept(m->m_sender);
}

/*
 * dup_client()
 *	Duplicate current file access onto new session
 */
static void
dup_client(struct msg *m, struct client *cold)
{
	struct client *c;

	/*
	 * Get data structure
	 */
	if ((c = malloc(sizeof(struct client))) == 0) {
		msg_err(m->m_sender, strerror());
		return;
	}

	/*
	 * Fill in fields
	 */
	bcopy(cold, c, sizeof(struct client));
	sc_init(&c->c_selfs);
	c->c_sentry = 0;

	/*
	 * Hash under the sender's handle
	 */
        if (hash_insert(clients, m->m_arg, c)) {
		free(c);
		msg_err(m->m_sender, ENOMEM);
		return;
	}

	/*
	 * Add ref
	 */
	if (c->c_port) {
		c->c_port->t_refs += 1;
	}

	/*
	 * Return acceptance
	 */
	m->m_arg = m->m_arg1 = m->m_buflen = m->m_nseg = 0;
	msg_reply(m->m_sender, m);
}

/*
 * deref_tcp()
 *	Deref a tcp_port, clean up on last ref
 */
static void
deref_tcp(struct tcp_port *t)
{
	t->t_refs -= 1;
	if (t->t_refs == 0) {
		uint x;

		/*
		 * Clean up each connection under the port
		 */
		for (x = 0; x < t->t_maxconn; ++x) {
			struct tcp_conn *c;
			struct tcb *tcb;

			/*
			 * Continue if this slot isn't in use now
			 */
			c = t->t_conns[x];
			if (c == 0) {
				continue;
			}

			/*
			 * Sanity
			 */
			ASSERT_DEBUG(LL_EMPTY(&c->t_readers),
				"deref_tcp: readers");
			ASSERT_DEBUG(LL_EMPTY(&c->t_writers),
				"deref_tcp: writers");

			/*
			 * Clear TCB; note that we aren't being nice
			 * any more, it's simply deleted.
			 */
			tcb = c->t_tcb;
			if (tcb) {
				close_tcp(tcb); c->t_tcb = 0;
				tcb->user = 0;
			}

			/*
			 * Free up connection state
			 */
			free(c); t->t_conns[x] = 0;
		}

		/*
		 * Clear out port itself
		 */
		ASSERT_DEBUG(LL_EMPTY(&t->t_any), "deref_tcp: any");
		(void)hash_delete(ports, t->t_lsock.port);
		if (t->t_conns) {
			free(t->t_conns); t->t_conns = 0;
		}
		free(t);
	}
}

/*
 * abort_op()
 *	Abort a pending I/O operation
 */
static void
abort_op(struct client *c)
{
	if (c->c_entry) {
		/* XXX what if the buffer's active? */
		ll_delete(c->c_entry); c->c_entry = 0;
	}
}

/*
 * shut_client()
 *	Close a client
 */
static int
shut_client(long client, struct client *c, int arg)
{
	struct tcp_port *t;

	/*
	 * Remove from queued I/O list
	 */
	abort_op(c);

	/*
	 * Deref TCP port
	 */
	if (t = c->c_port) {
		deref_tcp(t); c->c_port = 0;
	}
	return(0);
}

/*
 * dead_client()
 *	Someone has gone away.  Free their info.
 */
static void
dead_client(struct msg *m, struct client *cl)
{
	shut_client(m->m_sender, cl, 0);
	(void)hash_delete(clients, m->m_sender);
	if (cl->c_sentry) {
		ll_delete(cl->c_sentry);
	}
	free(cl);
}

/*
 * cleanup()
 *	Clean up whatever resources are allocated
 */
static void
cleanup(void)
{
	if (serv_port > 0) {
		msg_disconnect(serv_port);
		serv_port = 0;
	}
	if (clients) {
		hash_foreach(clients, shut_client, 0);
		hash_dealloc(clients);
		clients = 0;
	}
	if (ports) {
		hash_dealloc(ports);
		ports = 0;
	}
	clear_tbd();
	vsta_daemon_done(port_idx);
}

/*
 * find_port()
 *	Get existing tcp_port
 */
static struct tcp_port *
find_port(uint portnum)
{
	return(hash_lookup(ports, portnum));
}

/*
 * uncreate_conn()
 *	Clean up a connection under a tcp_port
 */
static void
uncreate_conn(struct tcp_port *t, struct tcp_conn *c)
{
	uint idx;

	for (idx = 0; idx < t->t_maxconn; ++idx) {
		if (t->t_conns[idx] == c) {
			t->t_conns[idx] = 0;
			free(c);
			return;
		}
	}
	ASSERT(0, "uncreate_conn: not found");
}

/*
 * create_conn()
 *	Create a new connection under a tcp_port
 */
static struct tcp_conn *
create_conn(struct tcp_port *t)
{
	struct tcp_conn *c, **cp;
	uint idx;

	/*
	 * Make room for another connection
	 */
	for (idx = 0; idx < t->t_maxconn; ++idx) {
		if (t->t_conns[idx] == 0) {
			break;
		}
	}
	if (idx == t->t_maxconn) {
		cp = realloc(t->t_conns,
			(t->t_maxconn + 1) * sizeof(struct tcp_conn *));
		if (cp == 0) {
			return(0);
		}
		t->t_conns = cp;
		t->t_maxconn += 1;
	}

	/*
	 * Get connection data structure
	 */
	t->t_conns[idx] = c = malloc(sizeof(struct tcp_conn));
	if (c == 0) {
		/*
		 * Yes, this leaves t_conns resized, but that's harmless.
		 * The NULL return also cleared the slot out correctly.
		 */
		return(0);
	}

	/*
	 * Initialize
	 */
	bzero(c, sizeof(struct tcp_conn));
	ll_init(&c->t_readers);
	ll_init(&c->t_writers);
	ll_init(&c->t_selectors);
	c->t_port = t;
	c->t_conn = idx;

	/*
	 * Here's another port...
	 */
	nport += 1;

	return(c);
}

/*
 * create_port()
 *	Create new tcp_port
 */
static struct tcp_port *
create_port(int16 portnum)
{
	struct tcp_port *t;

	t = malloc(sizeof(struct tcp_port));
	if (t == 0) {
		return(0);
	}
	if (hash_insert(ports, portnum, t)) {
		free(t);
		return(0);
	}
	bzero(t, sizeof(struct tcp_port));
	t->t_lsock.address = ip_addr;
	t->t_lsock.port = portnum;
	ll_init(&t->t_any);

	return(t);
}

/*
 * default_prot()
 *	Set default protection for a port based on client's ID
 */
static void
default_prot(struct tcp_port *t, struct client *cl)
{
	struct prot *p;

	/*
	 * Assign default protection
	 */
	p = &t->t_prot;
	bzero(p, sizeof(*p));
	p->prot_len = PERM_LEN(&cl->c_perms[0]);
	bcopy(cl->c_perms[0].perm_id, p->prot_id, PERMLEN);
	cl->c_perm = p->prot_bits[p->prot_len-1] =
		ACC_READ|ACC_WRITE|ACC_CHMOD;
}

/*
 * inetfs_open()
 */
static void
inetfs_open(struct msg *m, struct client *cl)
{
	uint portnum;
	struct socket lsocket;
	struct tcp_port *t;

	/*
	 * Handle open request based on current dir level
	 */
	switch (cl->c_dir) {
	case DIR_ROOT:
		if (strcmp(m->m_buf, "tcp")) {
			msg_err(m->m_sender, ESRCH);
			return;
		}
		cl->c_dir = DIR_TCP;
		cl->c_perm = ACC_READ | ACC_WRITE | ACC_CHMOD;
		break;
	case DIR_TCP:
		/*
		 * Numeric chooses a particular TCP port #
		 */
		if (sscanf(m->m_buf, "%d", &portnum) != 1) {
			extern int16 lport;

			/*
			 * Only other entry is "clone"
			 */
			if (strcmp(m->m_buf, "clone")) {
				msg_err(m->m_sender, ESRCH);
				return;
			}

			/*
			 * Clone open; pick an unused port number
			 */
			portnum = lport++;
		}

		/*
		 * Have a port #, connect to existing or create new
		 */
		t = find_port(portnum);
		if (t == 0) {
			/*
			 * Create a new one; initialize its protection
			 */
			t = create_port(portnum);
			if (t == 0) {
				msg_err(m->m_sender, strerror());
				return;
			}
			default_prot(t, cl);
		} else {
			uint x, want;

			/*
			 * See if we're allowed to access this port
			 */
			x = perm_calc(cl->c_perms, cl->c_nperms,
				&t->t_prot);
			want = m->m_arg & (ACC_READ|ACC_WRITE|ACC_CHMOD);
			if ((want & x) != want) {
				msg_err(m->m_sender, EPERM);
				return;
			}
			cl->c_perm = x;
		}

		/*
		 * Add client to this port, move to this node
		 */
		cl->c_port = t;
		cl->c_dir = DIR_PORT;
		t->t_refs += 1;
		break;

	default:
		msg_err(m->m_sender, EINVAL);
		return;
	}

	/*
	 * Success
	 */
	m->m_arg = m->m_arg1 = m->m_nseg = 0;
	cl->c_pos = 0;
	msg_reply(m->m_sender, m);
}

/*
 * inetfs_stat()
 *	Handle status query operations
 */
static void
inetfs_stat(struct msg *m, struct client *cl)
{
	msg_err(m->m_sender, EINVAL);
}

/*
 * cleario()
 *	Clean up pending I/O on the given llist queue
 */
static void
cleario(struct llist *l, char *err, struct tcp_conn *c)
{
	struct client *cl;

	while (!LL_EMPTY(l)) {
		struct msg *m;

		cl = LL_NEXT(l)->l_data;
		ll_delete(cl->c_entry); cl->c_entry = 0;
		m = &cl->c_msg;
		if (err) {
			msg_err(m->m_sender, err);
		} else {
			m->m_nseg = m->m_arg = m->m_arg1 = 0;
			msg_reply(m->m_sender, m);
			if (c) {
				cl->c_pos = c->t_conn;
			}
		}
	}
}

/*
 * inetfs_state()
 *	Callback for connection state change
 */
static void
inetfs_state(struct tcb *tcb, char old, char new)
{
	struct tcp_conn *c, *c2;

	/*
	 * For all but new connections, these links exist
	 */
	c = tcb->user;

	/*
	 * Demux for new state
	 */
	switch (new) {
	case ESTABLISHED:
		if (tcb->flags & CLONE) {
			c2 = create_conn(c->t_port);
			c2->t_tcb = tcb;
			tcb->user = c2;
		} else {
			c2 = 0;
		}
		cleario(&c->t_writers, 0, c2);
		break;

	case FINWAIT1:
	case FINWAIT2:
	case CLOSING:
	case LAST_ACK:
	case TIME_WAIT:
		if (c) {
			cleario(&c->t_readers, EIO, 0);
			cleario(&c->t_writers, EIO, 0);
		}
		break;

	case CLOSE_WAIT:
		/*
		 * Flush last buffers
		 */
		if (c) {
			fill_sendq(c);
		}
		close_tcp(tcb);
		break;
	
	case CLOSED:
		tcb->user = 0;
		del_tcp(tcb);
		if (c) {
			c->t_tcb = 0;
			cleario(&c->t_readers, EIO, 0);
			cleario(&c->t_writers, EIO, 0);
			uncreate_conn(c->t_port, c);
		}
		nport -= 1;
		break;
	}
}

/*
 * get_conn()
 *	Given client, return tcp_conn currently selected, or 0
 */
static struct tcp_conn *
get_conn(struct client *cl)
{
	ulong pos;
	struct tcp_port *t = cl->c_port;

	/*
	 * Pick session based on current file position
	 */
	pos = cl->c_pos;
	if (pos >= t->t_maxconn) {
		return(0);
	}
	return(cl->c_port->t_conns[pos]);
}

/*
 * inetfs_conn()
 *	Try to change connection mode for this TCP port
 */
static void
inetfs_conn(struct msg *m, struct client *cl, char *val)
{
	struct tcp_port *t = cl->c_port;
	struct tcp_conn *c;
	uint mode;

	/*
	 * Disconnection--shut TCP if open, return success
	 */
	if (!strcmp(val, "disconnect")) {
		c = get_conn(cl);
		if (c) {
			struct tcb *tcb;

			tcb = c->t_tcb;
			if (tcb) {
				close_tcp(tcb);
			}
		}
		m->m_arg = m->m_arg1 = m->m_nseg = 0;
		(void)msg_reply(m->m_sender, m);
		return;
	}

	/*
	 * Decode mode of connection
	 */
	if (!strcmp(val, "server")) {
		mode = TCP_SERVER;
	} else if (!strcmp(val, "passive")) {
		mode = TCP_PASSIVE;
	} else if (!strcmp(val, "active")) {
		/*
		 * Require a destination ip/port pair
		 */
		if (!t->t_fsock.address || !t->t_fsock.port) {
			msg_err(m->m_sender, "ip/no addr");
			return;
		}
		mode = TCP_ACTIVE;
	} else {
		msg_err(m->m_sender, EINVAL);
		return;
	}

	/*
	 * Don't permit if connection already initiated
	 */
	if ((t->t_maxconn > 0) && (mode != TCP_SERVER)) {
		msg_err(m->m_sender, EBUSY);
		return;
	}

	/*
	 * Get tcp_conn first
	 */
	c = get_conn(cl);
	if (c == 0) {
		/*
		 * No existing one, so make one
		 */
		c = create_conn(t);
		if (c == 0) {
			msg_err(m->m_sender, ENOMEM);
			return;
		}

		/*
		 * Request connection, with upcall connections
		 */
		c->t_tcb = open_tcp(&t->t_lsock,
			((t->t_fsock.address && t->t_fsock.port) ?
				(&t->t_fsock) : (NULLSOCK)),
			mode, 0,
			inetfs_rcv, inetfs_xmt, inetfs_state, 0, c);
		if (c->t_tcb == NULLTCB) {
			msg_err(m->m_sender, err2str(net_error));
			uncreate_conn(t, c);
			return;
		}
	}

	/*
	 * Queue as a "writer" for kicking off the connection
	 */
	cl->c_entry = ll_insert(&c->t_writers, cl);
	if (cl->c_entry == 0) {
		msg_err(m->m_sender, strerror());
		del_tcp(c->t_tcb); c->t_tcb = 0;
		uncreate_conn(t, c);
		return;
	}

	/*
	 * Record client message, wait for connection
	 */
	cl->c_msg = *m;
}

/*
 * check_select()
 *	See if there are select events to generate for this client
 */
static void
check_select(struct client *cl)
{
	struct selclient *scp = &cl->c_selfs;
	struct tcp_conn *c = get_conn(cl);
	struct tcb *tcb = c->t_tcb;
	uint events = 0;

	/*
	 * Look up TCP connection.  Generate no events if there's not
	 * a proper TCP connection.
	 */
	c = get_conn(cl);
	if (!c) {
		return;
	}
	tcb = c->t_tcb;
	if (!tcb) {
		return;
	}

	/*
	 * Check if there's readable content
	 */
	if (scp->sc_mask & ACC_READ) {
		if (tcb->rcvcnt) {
			events |= ACC_READ;
		}
	}

	/*
	 * Or if there's room to write() more
	 */
	if (scp->sc_mask & ACC_WRITE) {
		if (tcb->sndcnt < tcb->window) {
			events |= ACC_WRITE;
		}
	}

	/*
	 * TBD: Exceptional == OOB data
	 */

	/*
	 * Post events, if any
	 */
	if (events) {
		sc_event(scp, events);
	}
}

/*
 * inetfs_wstat()
 *	Handle status setting operations
 */
static void
inetfs_wstat(struct msg *m, struct client *cl)
{
	char *field, *val;
	struct tcp_port *t = cl->c_port;
	struct tcp_conn *c = get_conn(cl);

	if (do_wstat(m, &t->t_prot, cl->c_perm, &field, &val) == 0) {
		return;
	}
	switch (cl->c_dir) {
	case DIR_PORT:
		ASSERT_DEBUG(t, "inetfs_wstat: PORT null port");
		if (!val) {
			; /* Cause us to fall into error case */
		} else if (c &&
				sc_wstat(m, &cl->c_selfs, field, val) == 0) {
			if (cl->c_selfs.sc_mask) {
				(void)ll_insert(&c->t_selectors, cl);
				check_select(cl);
			}
			return;
		} else if (!strcmp(field, "conn")) {
			inetfs_conn(m, cl, val);
			return;
		} else if (!strcmp(field, "dest")) {
			t->t_fsock.address = aton(val);
			break;
		} else if (!strcmp(field, "destsock")) {
			t->t_fsock.port = atoi(val);
			break;
		} else if (!strcmp(field, "proxy")) {
			t->t_proxy = atoi(val);
			break;
		}

		/* VVV fall into error case VVV */

	default:
		msg_err(m->m_sender, EINVAL);
		return;
	}

	/*
	 * Return success
	 */
	m->m_buflen = m->m_nseg = m->m_arg = m->m_arg1 = 0;
	msg_reply(m->m_sender, m);
}

/*
 * add_dir()
 *	Add next entry to dir listing
 */
static int
add_dir(long port, struct tcp_port *t, char *buf)
{
	char *p;

	p = buf + strlen(buf);
	sprintf(p, "%d\n", t->t_lsock.port);
	return(0);
}

/*
 * inetfs_read_dir()
 *	Handle read requests when in a directory
 */
static void
inetfs_read_dir(struct msg *m, struct client *cl)
{
	char *buf;
	int len;

	switch (cl->c_dir) {
	case DIR_ROOT:
		buf = "tcp\n"; len = 4;
		break;
	case DIR_TCP:
		buf = alloca(nport * 6);
		buf[0] = '\0';
		hash_foreach(ports, add_dir, buf);
		len = strlen(buf);
		break;
	default:
		len = 0;
		break;
	}

	/*
	 * Adjust for what's been read
	 */
	buf += cl->c_pos;
	len -= cl->c_pos;

	/*
	 * Send back result, with buffer if any
	 */
	if (len > 0) {
		m->m_buf = buf;
		m->m_buflen = len;
		m->m_nseg = 1;
	} else {
		len = 0;
		m->m_nseg = 0;
	}
	m->m_arg = len;
	m->m_arg1 = 0;
	msg_reply(m->m_sender, m);
	cl->c_pos += len;
}

/*
 * inetfs_read()
 *	Read data from TCP stream into VSTa client
 */
static void
inetfs_read(struct msg *m, struct client *cl)
{
	struct tcp_port *t = cl->c_port;
	struct tcp_conn *c;

	/*
	 * If not in a port, it's a directory
	 */
	if (cl->c_dir != DIR_PORT) {
		inetfs_read_dir(m, cl);
		return;
	}

	/*
	 * If he doesn't care, pick current stream based on
	 * availability
	 */
	cl->c_selfs.sc_iocount += 1;
	if (m->m_arg1) {
		uint idx = cl->c_pos, found = 0;

		/*
		 * Scan for any TCB with data.  We use a "rover"
		 * to try and be fair about which connection gets
		 * processed next
		 */
		do {
			if (++idx > t->t_maxconn) {
				idx = 0;
			}
			c = t->t_conns[idx];
			if (c && c->t_tcb && c->t_tcb->rcvcnt) {
				found = 1;
				break;
			}
		} while (idx != cl->c_pos);
		cl->c_pos = idx;

		/*
		 * Nothing found; queue under the "any" queue.  Otherwise
		 * we fall down into the regular receive path, with our
		 * current position now reflecting a TCB with data.
		 */
		if (!found) {
			cl->c_entry = ll_insert(&t->t_any, cl);
			if (cl->c_entry == 0) {
				msg_err(m->m_sender, ENOMEM);
				return;
			}
			bcopy(m, &cl->c_msg, sizeof(*m));
			return;
		}
	}

	/*
	 * Get tcp_conn, validate
	 */
	c = get_conn(cl);
	if (c == 0) {
		msg_err(m->m_sender, EINVAL);
		return;
	}

	/*
	 * Queue as a reader, then see if we can get data yet
	 */
	cl->c_selfs.sc_needsel = 0;
	cl->c_entry = ll_insert(&c->t_readers, cl);
	if (cl->c_entry == 0) {
		msg_err(m->m_sender, ENOMEM);
		return;
	}
	bcopy(m, &cl->c_msg, sizeof(*m));
	inetfs_rcv(c->t_tcb, 0);
}

/*
 * fill_sendq()
 *	Take enough writers off the queue to keep our window full
 *
 * Completes M_WRITE's as they move to our TCB buffer
 */
static void
fill_sendq(struct tcp_conn *c)
{
	struct mbuf *mb;
	struct client *cl;
	struct llist *lp, *l;
	struct msg *m;
	struct tcb *tcb;
	uint size;

	lp = &c->t_writers;
	tcb = c->t_tcb;
	while (!LL_EMPTY(lp)) {
		/*
		 * Consider next queued I/O
		 */
		l = LL_NEXT(lp);
		cl = l->l_data;
		m = &cl->c_msg;

		/*
		 * If it'll fill more than the open window, don't
		 * pull it yet
		 */
		if ((tcb->sndcnt + m->m_arg) > tcb->window) {
			break;
		}

		/*
		 * Convert to mbuf format
		 */
		ll_delete(cl->c_entry); cl->c_entry = 0;
		mb = msg_to_mbuf(m);
		if (mb == 0) {
			msg_err(m->m_sender, ENOMEM);
			continue;
		}

		/*
		 * Sanity
		 */
		size = len_mbuf(mb);
		if (m->m_arg != size) {
			(void)free_p(mb);
			msg_err(m->m_sender, EINVAL);
			return;
		}

		/*
		 * Away it goes
		 */
		if (send_tcp(tcb, mb) < 0) {
			msg_err(m->m_sender, err2str(net_error));
			return;
		}

		/*
		 * Success.  If this is a proxy request, re-queue the
		 * writer so the result from proxyd will complete the
		 * operation.
		 */
		if (c->t_port->t_proxy) {
			if (cl->c_wait_reply) {
				m->m_arg1 = 0;
				inetfs_read(m, cl);
			}
		} else {
			m->m_arg = size;
			m->m_nseg = m->m_arg1 = 0;
			msg_reply(m->m_sender, m);
			cl->c_selfs.sc_needsel = 1;
		}
	}
}

/*
 * inetfs_write()
 *	Write message from VSTa client onto TCP stream
 */
static void
inetfs_write(struct msg *m, struct client *cl)
{
	struct tcp_port *t = cl->c_port;
	struct tcp_conn *c;
	struct tcb *tcb;

	/*
	 * Output is only legal once you have a particular TCP port open,
	 * and only if you have selected a valid connection index
	 */
	if (!t || !(c = get_conn(cl))) {
		msg_err(m->m_sender, EINVAL);
		return;
	}

	/*
	 * Cap I/O
	 */
	cl->c_selfs.sc_iocount += 1;
	if (m->m_arg == 0) {
		m->m_nseg = m->m_arg1 = 0;
		msg_reply(m->m_sender, m);
		return;
	}
	if ((ulong)(m->m_arg) > MAXIO) {
		msg_err(m->m_sender, E2BIG);
		return;
	}

	/*
	 * Get a linked list entry, queue us
	 */
	cl->c_selfs.sc_needsel = 0;
	cl->c_entry = ll_insert(&c->t_writers, cl);
	if (cl->c_entry == 0) {
		msg_err(m->m_sender, ENOMEM);
		return;
	}
	bcopy(m, &cl->c_msg, sizeof(*m));

	/*
	 * Let TCP pull as much into his window as possible
	 */
	fill_sendq(c);
}

/*
 * run_tbd()
 *	See if any of the pending TBD work can be accomplished
 */
static void
run_tbd(void)
{
	struct llist *l, *next;

	for (l = LL_NEXT(&tbdq); l != &tbdq; l = next) {
		struct tbd *t;

		t = l->l_data;
		next = LL_NEXT(l);
		if (t->tbd_cl->c_entry == 0) {
			ll_delete(l);
			(*t->tbd_fun)(&t->tbd_msg, t->tbd_cl);
			free(t);
		}
	}
}

/*
 * clear_tbd()
 *	Flush TBD queue during shutdown
 */
static void
clear_tbd(void)
{
	struct llist *l, *next;

	for (l = LL_NEXT(&tbdq); l != &tbdq; l = next) {
		next = LL_NEXT(l);
		free(l->l_data);
		ll_delete(l);
	}
}

/*
 * tbd()
 *	To Be Done.  Queue a client request for later processing
 */
static void
tbd(voidfun f, struct msg *mp, struct client *cl)
{
	struct tbd *t;

	t = malloc(sizeof(struct tbd));
	t->tbd_fun = f;
	bcopy(mp, &t->tbd_msg, sizeof(*mp));
	t->tbd_cl = cl;
	t->tbd_entry = ll_insert(&tbdq, t);
}

/*
 * do_proxy()
 *	Initiate a request to our remote proxy
 */
static void
do_proxy(struct msg *mp, struct client *cl)
{
	struct msg m;
	uint x;

	/*
	 * If a previous transaction is still pending, defer processing
	 */
	if (cl->c_entry) {
		tbd(do_proxy, mp, cl);
		return;
	}

	/*
	 * Handle a couple special cases
	 */
	switch (mp->m_op & MSG_MASK) {
	case M_DISCONNECT:
		dead_client(mp, cl);
		return;
	case M_DUP:
		/*
		 * We run the usual M_DUP stuff, but need to preserve
		 * the message so our remote side can see it, too.
		 */
		bcopy(mp, &m, sizeof(m));
		dup_client(mp, cl);
		bcopy(&m, mp, sizeof(m));
		cl->c_wait_reply = 0;
		break;
	default:
		cl->c_wait_reply = 1;
		break;
	}

	/*
	 * XXX
	 * Hmmm, to handle this otherwise would require teaching
	 * vsta_srv how to have more than one message pending to
	 * complete a user action.
	 */
	if (mp->m_nseg == MSGSEGS) {
		if (cl->c_wait_reply) {
			msg_err(mp->m_sender, E2BIG);
		}
		return;
	}

	/*
	 * Send on the actual payload
	 */
	m.m_sender = mp->m_sender;
	m.m_op = FS_WRITE;
	m.m_nseg = mp->m_nseg + 1;
	m.m_buf = mp;
	m.m_buflen = sizeof(*mp);
	m.m_arg1 = 0;
	bcopy(mp->m_seg, m.m_seg + 1, (MSGSEGS-1) * sizeof(seg_t));
	m.m_arg1 = m.m_arg = 0;
	for (x = 0; x < m.m_nseg; ++x) {
		m.m_arg += m.m_seg[x].s_buflen;
	}
	inetfs_write(&m, cl);
}

/*
 * proc_msg()
 *	Handle next message to server
 */
static void
proc_msg(struct msg *m)
{
	struct client *cl;

	/*
	 * Get state for client
	 */
	cl = hash_lookup(clients, m->m_sender);
	if (cl == 0) {
		msg_err(m->m_sender, EIO);
		return;
	}

	/*
	 * If there's a proxy server on the far side of the
	 * connection, forward data rather than handling the
	 * request locally.
	 */
	if (cl->c_port && cl->c_port->t_proxy) {
		do_proxy(m, cl);
		return;
	}

	/*
	 * Switch based on request
	 */
	switch (m->m_op & MSG_MASK) {
	case M_DISCONNECT:	/* Client done */
		dead_client(m, cl);
		break;
	case M_DUP:		/* File handle dup during exec() */
		dup_client(m, cl);
		break;

	case M_ABORT:		/* Aborted operation */
		abort_op(cl);
		m->m_arg = m->m_arg1 = m->m_nseg = 0;
		msg_reply(m->m_sender, m);
		break;

	case FS_OPEN:		/* Look up file from directory */
		if ((m->m_nseg != 1) || !valid_fname(m->m_buf,
				m->m_buflen)) {
			msg_err(m->m_sender, EINVAL);
			break;
		}
		inetfs_open(m, cl);
		break;

	case FS_ABSREAD:	/* Set position, then read */
		if (m->m_arg1 < 0) {
			msg_err(m->m_sender, EINVAL);
			break;
		}
		cl->c_pos = m->m_arg1;
		/* VVV fall into VVV */
	case FS_READ:		/* Read file */
		inetfs_read(m, cl);
		break;

	case FS_ABSWRITE:	/* Set position, then write */
		if (m->m_arg1 < 0) {
			msg_err(m->m_sender, EINVAL);
			break;
		}
		cl->c_pos = m->m_arg1;
		/* VVV fall into VVV */
	case FS_WRITE:		/* Write file */
		inetfs_write(m, cl);
		break;

	case FS_SEEK:		/* Set new file position */
		inetfs_seek(m, cl);
		break;

	case FS_STAT:		/* Tell about file */
		inetfs_stat(m, cl);
		break;
	case FS_WSTAT:		/* Set stuff on file */
		inetfs_wstat(m, cl);
		break;
	default:		/* Unknown */
		msg_err(m->m_sender, EINVAL);
		break;
	}
}

/*
 * port_daemon()
 *	Handler for TCP port access from VSTa filesystem
 */
static void
port_daemon(void)
{
	struct msg m;
	int x;

	/*
	 * Receive next message
	 */
	for (;;) {
		x = msg_receive(serv_port, &m);
		p_lock(&ka9q_lock);

		/*
		 * Port error, clean up and finish daemon
		 */
		if (x < 0) {
			perror("TCP port server");
			cleanup();
			v_lock(&ka9q_lock);
			_exit(1);
		}

		switch (m.m_op & MSG_MASK) {
		case M_CONNECT:		/* New client */
			new_client(&m);
			break;

		default:		/* Others */
			proc_msg(&m);
		}

		/*
		 * Spin the main loop
		 */
		while (do_mainloop())
			;

		/*
		 * Attempt progress on the clients
		 */
		if (!LL_EMPTY(&tbdq)) {
			run_tbd();
		}

		/*
		 * Release lock and loop
		 */
		v_lock(&ka9q_lock);
	}
}

/*
 * vsta1()
 *	Start VSTa /inet server
 */
void
vsta1(int argc, char **argv)
{
	port_name serv_name;

	/*
	 * Get client & port hash
	 */
	if (clients) {
		printf("Already running\n");
		return;
	}
	clients = hash_alloc(HASH_SIZE);
	ports = hash_alloc(HASH_SIZE);
	if (!clients || !ports) {
		printf("Start failed, no memory\n");
		cleanup();
		return;
	}
	ll_init(&tbdq);

	/*
	 * Get server port, advertise it
	 */
	serv_port = msg_port(0, &serv_name);
	if (serv_port < 0) {
		printf("Can't allocate port\n");
		cleanup();
		return;
	}
	if (namer_register("net/inet", serv_name) < 0) {
		printf("Can't register port name\n");
		cleanup();
		return;
	}

	/*
	 * Start watcher daemon for this service
	 */
	port_idx = vsta_daemon(port_daemon);
}

/*
 * vsta0()
 *	Shut down VSTa /inet server
 */
vsta0(void)
{
	cleanup();
}

/*
 * assemble_bytes()
 *	Pull in more bytes to a partial buffer
 */
static void
assemble_bytes(void *ptrp, uint *countp, uint count, struct tcp_conn *c)
{
	uint len;

	len = pullup(&c->t_readq, (char *)ptrp + *countp, count - *countp);
	*countp += len;
}

/*
 * send_proxy_data()
 *	Assemble proxy messages and msg_reply() them back to the client
 *
 * Also recognizes the FS_ERR case, and msg_err()'s it back instead.
 */
static void
send_proxy_data(struct tcp_conn *c, struct client *cl)
{
	int x;
	uint len;
	struct msg *mp;

	/*
	 * Try and assemble a complete message header
	 */
	while (c->t_readq && (cl->c_msg_bytes < sizeof(struct msg))) {
		assemble_bytes(&cl->c_msg, &cl->c_msg_bytes,
			sizeof(struct msg), c);
		cl->c_msg.m_buf = 0;
	}

	/*
	 * Don't have enough yet; back to the main loop
	 */
	if (cl->c_msg_bytes < sizeof(struct msg)) {
		return;
	}
	mp = &cl->c_msg;

	/*
	 * Assemble length from m_nseg and the buffers.  We can't
	 * use m_arg, because if we sent an FS_WRITE (no M_READ),
	 * there's no data to assemble back in this direction, and
	 * m_arg is the actual I/O result from the remote.
	 */
	for (x = 0, len = 0; x < mp->m_nseg; ++x) {
		len += mp->m_seg[x].s_buflen;
	}

	/*
	 * Handle too-large I/O responses
	 */
	if (len > MAXIO) {
		ll_delete(cl->c_entry); cl->c_entry = 0;
		cl->c_msg_bytes = 0;
		msg_err(mp->m_sender, E2BIG);
		if (mp->m_buf) {
			free(mp->m_buf);
		}
		return;
	}

	/*
	 * No I/O buffer to contain it yet; allocate.  Note
	 * that m_buf was explicitly set to 0 as we assembled
	 * the header, so it is a local flag of whether we've
	 * allocated the buffer yet.
	 */
	if (len && (mp->m_buf == 0)) {
		mp->m_buf = malloc(len);
		mp->m_buflen = len;
		mp->m_nseg = 1;
	}

	/*
	 * Assemble the body
	 */
	while (c->t_readq && (cl->c_body_bytes < len)) {
		assemble_bytes(mp->m_buf, &cl->c_body_bytes, len, c);
	}

	/*
	 * Body not arrived yet?  Return.
	 */
	if (cl->c_body_bytes < len) {
		return;
	}

	/*
	 * Reset for next time around
	 */
	cl->c_msg_bytes = cl->c_body_bytes = 0;

	/*
	 * FS_ERR; turn into a msg_err() back to the user
	 */
	if (mp->m_op == FS_ERR) {
		msg_err(mp->m_sender, mp->m_buf);
	} else {
		if (msg_reply(mp->m_sender, mp) < 0) {
			perror("msg_reply");
		}
	}

	/*
	 * Clean up resources and return
	 */
	if (mp->m_buf) {
		free(mp->m_buf);
	}
	ll_delete(cl->c_entry); cl->c_entry = 0;
}

/*
 * send_data()
 *	Move data from mbufs out to VSTa clients
 */
static void
send_data(struct tcp_conn *c, struct llist *q)
{
	struct msg *m;
	struct client *cl;
	uint nseg, mb_pullup = 0;

	/*
	 * While there's data and readers, move data
	 */
	while (c->t_readq && !LL_EMPTY(q)) {
		/*
		 * Dequeue next request
		 */
		cl = LL_NEXT(q)->l_data;

		/*
		 * If this is a waiter for a reply from the remote
		 * proxy daemon, do special assembly of the reply
		 * message.
		 */
		if (c->t_port->t_proxy) {
			send_proxy_data(c, cl);
			continue;
		}

		/*
		 * Otherwise this is consumption of an unstructured byte
		 * stream, so shovel out what we have and always complete
		 * the request.
		 */
		ll_delete(cl->c_entry); cl->c_entry = 0;

		/*
		 * Convert mbuf(s) into a reply VSTa message
		 */
		m = &cl->c_msg;
		mb_pullup = mbuf_to_msg(c->t_readq, m, m->m_arg);
		nseg = m->m_nseg;

		/*
		 * Send back to requestor
		 */
		m->m_arg1 = c->t_conn;
		msg_reply(m->m_sender, m);
		cl->c_selfs.sc_needsel = 1;

		/*
		 * Clean up consumed data
		 */
		while (nseg--) {
			/*
			 * For the last mbuf consumed, it may be
			 * a pullup if we couldn't use all the data
			 */
			if ((nseg == 0) && mb_pullup) {
				pullup(&c->t_readq, NULLCHAR, mb_pullup);
			} else {
				/*
				 * Otherwise just free the mbuf
				 */
				c->t_readq = free_mbuf(c->t_readq);
			}
		}
	}
}

/*
 * send_select()
 *	Send select() events for readable data
 */
static void
send_select(struct tcp_conn *c, struct llist *q, uint event)
{
	struct llist *l;
	struct client *cl;

	for (l = LL_NEXT(q); l != q; l = LL_NEXT(l)) {
		cl = l->l_data;
		sc_event(&cl->c_selfs, event);
	}
}

/*
 * inetfs_rcv()
 *	More data has arrived for a connection
 */
static void
inetfs_rcv(struct tcb *tcb, int16 cnt)
{
	struct tcp_conn *c = tcb->user;

	/*
	 * If we're cleaning up, ignore the data
	 */
	if (c == 0) {
		return;
	}

	/*
	 * If we consumed all the current data, take a new batch from TCP
	 */
	if (c->t_readq == 0) {
		recv_tcp(tcb, &c->t_readq, 0);
	}

	/*
	 * Send data off to clients
	 */
	send_data(c, &c->t_readers);

	/*
	 * If there's still data, see if an "any" reader is present
	 */
	if (c->t_readq) {
		struct tcp_port *t = c->t_port;

		if (!LL_EMPTY(&t->t_any)) {
			send_data(c, &t->t_any);
		}
	}

	/*
	 * Finally, awake anyone select()'ing for data here
	 */
	if (!LL_EMPTY(&c->t_selectors)) {
		send_select(c, &c->t_selectors, ACC_READ);
	}
}

/*
 * inetfs_xmt()
 *	Upcall when TCP finds room for more data
 */
static void
inetfs_xmt(struct tcb *tcb, int16 count)
{
	struct tcp_conn *c = tcb->user;

	fill_sendq(c);
	if (!LL_EMPTY(&c->t_selectors) && (tcb->sndcnt < tcb->window)) {
		send_select(c, &c->t_selectors, ACC_WRITE);
	}
}

/*
 * show_client()
 *	Display client status
 */
static int
show_client(long client, struct client *cl, int arg)
{
	static char *dirname[] = {"root", "TCP", "port"};

	printf("%d\t%s\t%d\t%x\t%x\t%ld\n",
		cl->c_perms[0].perm_uid,
		dirname[cl->c_dir],
		(cl->c_dir == DIR_PORT) ?
			(cl->c_port->t_lsock.port) : 0,
		(ulong)(cl->c_port)  & 0xFFFFFF,
		(ulong)(cl->c_entry) & 0xFFFFFF,
		client);
	return(0);
}

/*
 * listlen()
 *	Return length of a list
 */
static uint
listlen(struct llist *l)
{
	struct llist *l2 = LL_NEXT(l);
	uint len = 0;

	while (l2 != l) {
		len += 1;
		l2 = LL_NEXT(l2);
	}
	return(len);
}

/*
 * show_port()
 *	Display state of a tcp_port
 */
static int
show_port(long port, struct tcp_port *t, int arg)
{
	uint x;

	printf("%ld\t%d\t%d\n", port, t->t_refs, t->t_maxconn);
	for (x = 0; x < t->t_maxconn; ++x) {
		struct tcp_conn *c = t->t_conns[x];
		struct tcb *tcb;

		if (c == 0) {
			continue;
		}
		tcb = c->t_tcb;
		printf(" %2d rem: %s:%d #rd: %d #wrt: %d #rdq: %d\n",
			x,
			tcb ? 
				inet_ntoa(tcb->conn.remote.address)
			    :	"0.0.0.0",
			tcb ?
				tcb->conn.remote.port
			    :	0,
			listlen(&c->t_readers),
			listlen(&c->t_writers),
			len_mbuf(c->t_readq));
	}
	return(0);
}

/*
 * dovsta()
 *	Status queries from KA9Q command line
 */
int
dovsta(char *cmd)
{
	printf("========VSTa server port: %d\n", msg_portname(serv_name));
	printf("UID\tDir\tPort\t\tQueue\n");
	hash_foreach(clients, show_client, 0);
	printf("========# ports: %d\nPort\trefs\tmaxconn\n", nport);
	hash_foreach(ports, show_port, 0);
}
