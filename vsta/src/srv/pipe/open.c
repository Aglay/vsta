/*
 * open.c
 *	Routines for opening, closing, creating  and deleting files
 *
 * Note that we don't allow subdirectories for BFS, which simplifies
 * things.
 */
#include <pipe/pipe.h>
#include <lib/llist.h>
#include <lib/hash.h>
#include <std.h>
#include <sys/assert.h>

extern struct llist files;	/* All files in FS */

/*
 * dir_lookup()
 *	Look up name in list of all files in this FS
 */
struct pipe *
dir_lookup(char *name)
{
	struct llist *l;
	struct pipe *o;
	ulong x;

	if (sscanf(name, "%ld", &x) != 1) {
		return(0);
	}
	o = (struct pipe *)x;
	for (l = LL_NEXT(&files); l != &files; l = LL_NEXT(l)) {
		if (o == l->l_data) {
			return(o);
		}
	}
	return(0);
}

/*
 * freeup()
 *	Free all memory associated with a file
 */
static void
freeup(struct pipe *o)
{
	if (o == 0) {
		return;
	}
	if (o->p_entry) {
		ll_delete(o->p_entry);
	}
	free(o);
}

/*
 * dir_newfile()
 *	Create new entry in filesystem
 */
struct pipe *
dir_newfile(struct file *f, char *name)
{
	struct pipe *o;
	struct prot *p;

	/*
	 * Get new node
	 */
	o = malloc(sizeof(struct pipe));
	if (o == 0) {
		return(0);
	}
	bzero(o, sizeof(struct pipe));
	ll_init(&o->p_readers);
	ll_init(&o->p_writers);
	o->p_nwrite = 1;

	/*
	 * Insert in dir chain
	 */
	o->p_entry = ll_insert(&files, o);
	if (o->p_entry == 0) {
		freeup(o);
		return(0);
	}

	/*
	 * Use 0'th perm as our prot, require full match
	 */
	p = &o->p_prot;
	bzero(p, sizeof(*p));
	p->prot_len = PERM_LEN(&f->f_perms[0]);
	bcopy(f->f_perms[0].perm_id, p->prot_id, PERMLEN);
	p->prot_bits[p->prot_len-1] =
		ACC_READ|ACC_WRITE|ACC_CHMOD;
	o->p_owner = f->f_perms[0].perm_uid;
	return(o);
}

/*
 * pipe_open()
 *	Main entry for processing an open message
 */
void
pipe_open(struct msg *m, struct file *f)
{
	struct pipe *o;
	uint x;

	/*
	 * Have to be in root dir to open down into a file
	 */
	if (f->f_file) {
		msg_err(m->m_sender, ENOTDIR);
		return;
	}

	/*
	 * No subdirs in a pipe filesystem
	 */
	if (m->m_arg & ACC_DIR) {
		msg_err(m->m_sender, EINVAL);
		return;
	}

	/*
	 * Look up name
	 */
	if ((m->m_buflen != 2) || (((char *)(m->m_buf))[0] != '#')) {
		o = dir_lookup(m->m_buf);
		if (!o) {
			msg_err(m->m_sender, ESRCH);
			return;
		}
	} else {
		o = 0;
	}

	/*
	 * No such file--do they want to create?
	 */
	if (!o && !(m->m_arg & ACC_CREATE)) {
		msg_err(m->m_sender, ESRCH);
		return;
	}

	/*
	 * If it's a new file, allocate the entry now.
	 */
	if (!o) {
		/*
		 * Failure?
		 */
		if ((o = dir_newfile(f, m->m_buf)) == 0) {
			msg_err(m->m_sender, ENOMEM);
			return;
		}

		/*
		 * Move to new node
		 */
		f->f_file = o; o->p_refs += 1;
		f->f_perm = ACC_READ|ACC_WRITE|ACC_CHMOD;
		m->m_nseg = m->m_arg = m->m_arg1 = 0;
		msg_reply(m->m_sender, m);
		return;
	}

	/*
	 * Check permission
	 */
	x = perm_calc(f->f_perms, f->f_nperm, &o->p_prot);
	if ((m->m_arg & x) != m->m_arg) {
		msg_err(m->m_sender, EPERM);
		return;
	}

	/*
	 * Move to this file
	 */
	f->f_file = o; o->p_refs += 1;
	f->f_perm = m->m_arg | (x & ACC_CHMOD);
	m->m_nseg = m->m_arg = m->m_arg1 = 0;
	msg_reply(m->m_sender, m);
}

/*
 * pipe_close()
 *	Do closing actions on a file
 */
void
pipe_close(struct file *f)
{
	struct pipe *o;

	/*
	 * No ref count on dir
	 */
	o = f->f_file;
	if (o == 0) {
		return;
	}

	/*
	 * Free a ref.  No more clients--free node.
	 */
	o->p_refs -= 1;
	if (o->p_refs == 0) {
		freeup(o);
		return;
	}

	/*
	 * Close of last writer--bomb all pending readers
	 */
	o->p_nwrite -= 1;
	if (o->p_nwrite > 0) {
		return;
	}
	while (!LL_EMPTY(&o->p_readers)) {
		struct msg *m;
		struct file *f2;

		f2 = LL_NEXT(&o->p_readers)->l_data;
		ASSERT_DEBUG(f2->f_q, "pipe_close: !busy");
		ll_delete(f2->f_q);
		f2->f_q = 0;
		m = &f2->f_msg;
		m->m_arg = m->m_arg1 = m->m_nseg = 0;
		msg_reply(m->m_sender, m);
	}
}
