/*
 * open.c
 *	Routines for opening, closing, creating  and deleting files
 */
#include <dos/dos.h>
#include <sys/fs.h>
#include <sys/assert.h>
#include <std.h>

extern struct node *rootdir;

/*
 * move_file()
 *	Transfer the current file of a struct file to the given node
 */
static void
move_file(struct file *f, struct node *n)
{
	deref_node(f->f_node);
	f->f_node = n;
	/* new node already ref'ed on lookup */
	f->f_pos = 0;
}

/*
 * dos_open()
 *	Main entry for processing an open message
 */
void
dos_open(struct msg *m, struct file *f)
{
	struct node *n;
	int newfile = 0;

	/*
	 * Need a buffer
	 */
	if (m->m_nseg != 1) {
		msg_err(m->m_sender, EINVAL);
		return;
	}

	/*
	 * Have to be in dir to open down into a file
	 */
	if (f->f_node->n_type != T_DIR) {
		msg_err(m->m_sender, ENOTDIR);
		return;
	}

	/*
	 * Check for permission
	 */
	if (m->m_arg & (ACC_WRITE|ACC_CREATE|ACC_DIR)) {
		/*
		 * Insufficient priveleges
		 */
		if ((f->f_perm & ACC_WRITE) == 0) {
			msg_err(m->m_sender, EPERM);
			return;
		}
	}

	/*
	 * Look up name
	 */
	n = dir_look(f->f_node, m->m_buf);

	/*
	 * No such file--do they want to create?
	 */
	if (!n && !(m->m_arg & ACC_CREATE)) {
		msg_err(m->m_sender, ESRCH);
		return;
	}

	/*
	 * If it's a new file, allocate the entry now.
	 */
	if (!n) {
		n = dir_newfile(f, m->m_buf, m->m_arg & ACC_DIR);
		if (!n) {
			msg_err(m->m_sender, strerror());
			return;
		}
		newfile = 1;
	}

	/*
	 * If they want to use the existing file, set up the
	 * node and let them go for it.  Note that this case
	 * MUST be !newfile, or it would have been caught above.
	 */
	if (!(m->m_arg & ACC_CREATE)) {
		goto success;
	}

	/*
	 * Creation is desired.  If there's an existing file, free
	 * its storage.
	 */
	if (!newfile) {
		/*
		 * Can't rewrite a directory like this
		 */
		if (n->n_type == T_DIR) {
			msg_err(m->m_sender, EEXIST);
			deref_node(n);
			return;
		}
		clust_setlen(n->n_clust, 0L);
		n->n_len = 0;
	}
success:
	move_file(f, n);
	m->m_nseg = 0;
	m->m_arg1 = m->m_arg = 0;
	msg_reply(m->m_sender, m);
}

/*
 * dos_close()
 *	Do closing actions on a file
 */
void
dos_close(struct file *f)
{
	deref_node(f->f_node);
}

/*
 * dos_remove()
 *	Remove an entry in the current directory
 */
void
dos_remove(struct msg *m, struct file *f)
{
	struct node *n;
	void *handle;
	struct directory *d;

	/*
	 * Need a buffer
	 */
	if (m->m_nseg != 1) {
		msg_err(m->m_sender, EINVAL);
		return;
	}

	/*
	 * Have to have write permission
	 */
	if (!(f->f_perm & ACC_WRITE)) {
		msg_err(m->m_sender, EPERM);
		return;
	}

	/*
	 * Have to be in a directory
	 */
	if (f->f_node->n_type != T_DIR) {
		msg_err(m->m_sender, EINVAL);
		return;
	}

	/*
	 * Look up entry.  Bail if no such file.
	 */
	n = dir_look(f->f_node, m->m_buf);
	if (n == 0) {
		msg_err(m->m_sender, ESRCH);
		return;
	}

	/*
	 * We must be only access
	 */
	if (n->n_refs > 1) {
		deref_node(n);
		msg_err(m->m_sender, EBUSY);
		return;
	}

	/*
	 * Directories--only allowed when empty
	 */
	if (n->n_type == T_DIR) {
		if (!dir_empty(n)) {
			deref_node(n);
			msg_err(m->m_sender, EBUSY);
			return;
		}
	}

	/*
	 * Ask dir routines to remove
	 */
	dir_remove(n);

	/*
	 * Throw away his storage, mark him as already cleaned
	 */
	clust_setlen(n->n_clust, 0L);
	n->n_flags |= N_DEL;

	/*
	 * Done with node
	 */
	deref_node(n);

	/*
	 * Return success
	 */
	m->m_buflen = m->m_arg = m->m_arg1 = m->m_nseg = 0;
	msg_reply(m->m_sender, m);
}
