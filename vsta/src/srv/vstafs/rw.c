/*
 * rw.c
 *	Routines for operating on the data in a file
 *
 * The storage allocation philosophy used is to add EXTSIZ chunks of data
 * to the file when possible, and mark the file's length as such.  On close,
 * the trailing part of the file is truncated and the file's true length
 * updated.  This technique also means that after a crash the file's header
 * accurately describes the data associated with the file.
 */
#include <vstafs/vstafs.h>
#include <vstafs/alloc.h>
#include <vstafs/buf.h>
#include <lib/hash.h>
#include <lib/llist.h>
#include <std.h>
#include <sys/assert.h>

/*
 * file_grow()
 *	Extend the block allocation to reach the indicated size
 *
 * newsize is in sectors.  Returns 0 on success, 1 on failure.
 */
static int
file_grow(struct fs_file *fs, ulong newsize)
{
	ulong incr, got;
	struct alloc *a;
	daddr_t from;

	/*
	 * Find extent containing this position
	 */
	ASSERT_DEBUG(fs->fs_nblk > 0, "file_grow: no extents");
	a = &fs->fs_blks[fs->fs_nblk-1];

	/*
	 * Extend contiguously as far as possible.
	 */
	from = btors(fs->fs_len);
	ASSERT_DEBUG(newsize > from, "file_grow: shrink");
	incr = newsize - from;
	if (incr < EXTSIZ) {
		incr = EXTSIZ;
	}
	got = take_block(a->a_start + a->a_len, incr);
	if (got > 0) {
		a->a_len += got;
		from += got;
		fs->fs_len = stob(from);
	}

	/*
	 * If we've gotten at least the required amount, return success
	 * now.
	 */
	if (from >= newsize) {
		return(0);
	}

	/*
	 * Fail if there's no more slots for extents
	 */
	if (fs->fs_nblk >= MAXEXT) {
		return(1);
	}

	/*
	 * Sigh.  Another extent gets eaten.  Grab a full EXTSIZ chunk.
	 * We could quibble about shrinking this, but if you're down to
	 * your last 64K contiguous space, it's seriously time to defrag
	 * your disk.
	 */
	a += 1;
	a->a_start = alloc_block(EXTSIZ);
	if (a->a_start == 0) {
		return(1);
	}

	/*
	 * Mark the storage in our fs_file, and return success
	 */
	a->a_len = EXTSIZ;
	fs->fs_nblk += 1;
	fs->fs_len = stob(from+EXTSIZ);
	return(0);
}

/*
 * bmap()
 *	Access the right buffer for the given data
 *
 * If necessary, grow the file.  Figure out which buffer contains
 * the data, and map this buffer in.
 *
 * We return 0 on error, otherwise the struct buf corresponding to
 * the returned data range.  On success, *blkp points to the appropriate
 * spot within the buffer, and *stepp holds the number of bytes available
 * at this location.
 */
static struct buf *
bmap(struct fs_file *fs, ulong pos, uint cnt, char **blkp, uint *stepp)
{
	struct alloc *a;
	uint x;
	ulong osize, nsize, extoff, len;
	daddr_t extstart, start;
	struct buf *b;

	/*
	 * Grow file if needed
	 */
	if (pos > fs->fs_len) {
		/*
		 * Calculate growth.  If more blocks are needed, get
		 * them now.  Otherwise just fiddle the file length.
		 */
		osize = btors(fs->fs_len);
		nsize = btors(pos + cnt);
		if (nsize > osize) {
			if (file_setsize(fs, nsize)) {
				return(0);
			}
		} else {
			fs->fs_len = pos+cnt;
		}
	}

	/*
	 * Find the appropriate extent.  As we go, update "extoff"
	 * so that when we find the right extent we will then
	 * know how far into the extent our data exists.
	 */
	a = &fs->fs_blks[0];
	extoff = nsize = btos(pos);
	for (x = 0; x < fs->fs_nblk; ++x,++a) {
		if ((nsize >= a->a_start) && (nsize < (a->a_start+a->a_len))) {
			break;
		}
		extoff -= a->a_len;
	}
	ASSERT_DEBUG(x < fs->fs_nblk, "bmap: no extent");

	/*
	 * Find the appropriate EXTSIZ part of the extent to use, since
	 * our buffer pool operates on chunks of EXTSIZ in length.
	 */
	extstart = (extoff/EXTSIZ)*EXTSIZ;
	start = a->a_start + extstart;
	len = a->a_len - extstart;
	if (len > EXTSIZ) {
		len = EXTSIZ;
	}
	b = find_buf(start, len);
	if (b == 0) {
		return(0);
	}

	/*
	 * Map in the part we need, fill in its location and length
	 */
	len = len - (extoff % EXTSIZ);
	*blkp = index_buf(b, extoff % EXTSIZ, len);
	if (*blkp == 0) {
		return(0);
	}
	*blkp += (pos % SECSZ);
	x = stob(len) - (pos % SECSZ);
	if (x > cnt) {
		*stepp = cnt;
	} else {
		*stepp = x;
	}
	return(b);
}

/*
 * do_write()
 *	Local routine to loop over a buffer and write it to a file
 *
 * Returns 0 on success, 1 on error.
 */
static int
do_write(struct openfile *o, ulong pos, char *buf, uint cnt)
{
	struct fs_file *fs;
	struct buf *b;
	int result = 0;

	/*
	 * Get access to file structure information, which resides in the
	 * first sector of the file.
	 */
	b = find_buf(o->o_file, o->o_len);
	lock_buf(b);
	fs = index_buf(b, 0, 1);

	/*
	 * Loop across each block, putting our data into place
	 */
	while (cnt > 0) {
		struct buf *b2;
		uint step;
		char *blkp;

		/*
		 * Find appropriate extent
		 */
		b2 = bmap(fs, pos+OFF_DATA, cnt, &blkp, &step);
		if (!b2) {
			result = 1;
			break;
		}

		/*
		 * Mirror the size of the first extent, so we can map
		 * it correctly out of the cache.
		 */
		o->o_len = fs->fs_blks[0].a_len;

		/*
		 * Cap at amount of I/O to do here
		 */
		if (step > cnt) {
			step = cnt;
		}

		/*
		 * Put contents into block, mark buffer modified
		 */
		bcopy(buf, blkp, step);
		dirty_buf(b2);

		/*
		 * Advance to next chunk
		 */
		pos += step;
		buf += step;
		cnt -= step;
	}

	/*
	 * This needs to stay up-to-date so we can pull the correct size
	 * out of the pool.
	 */
	o->o_len = fs->fs_blks[0].a_len;

	/*
	 * Free leading buf, return result
	 */
	unlock_buf(b);
	return(result);
}

/*
 * vfs_write()
 *	Write to an open file
 */
void
vfs_write(struct msg *m, struct file *f)
{
	struct openfile *o = f->f_file;
	uint x, cnt, err;

	/*
	 * Can only write to a true file, and only if open for writing.
	 */
	if (!o || !(f->f_perm & ACC_WRITE)) {
		msg_err(m->m_sender, EPERM);
		return;
	}

	/*
	 * Walk each segment of the message
	 */
	err = cnt = 0;
	for (x = 0; x < m->m_nseg; ++x) {
		seg_t *s;

		/*
		 * Write it
		 */
		s = &m->m_seg[x];
		if (do_write(o, f->f_pos, s->s_buf, s->s_buflen)) {
			err = 1;
			break;
		}

		/*
		 * Update position and count
		 */
		f->f_pos += s->s_buflen;
		cnt += s->s_buflen;
	}

	/*
	 * Done.  Return count of stuff written.
	 */
	if ((cnt == 0) && err) {
		msg_err(m->m_sender, strerror());
	} else {
		m->m_arg = cnt;
		m->m_arg1 = m->m_nseg = 0;
		msg_reply(m->m_sender, m);
	}
}

/*
 * vfs_readdir()
 *	Do reads on directory entries
 */
static void
vfs_readdir(struct msg *m, struct file *f)
{
	char *buf;
	uint len, bufcnt;
	struct buf *b;
	struct fs_file *fs;
	uint step;

	/*
	 * If the current seek position doesn't match a directory
	 * entry boundary, blow them out of the water.
	 */
	if ((f->f_pos % sizeof(struct fs_dirent)) != 0) {
		msg_err(m->m_sender, EINVAL);
		return;
	}

	/*
	 * Get a buffer of the requested size, but put a sanity
	 * cap on it.
	 */
	len = m->m_arg;
	if (len > 256) {
		len = 256;
	}
	if ((buf = malloc(len+1)) == 0) {
		msg_err(m->m_sender, strerror());
		return;
	}
	buf[0] = '\0';

	/*
	 * Map in the directory's structure
	 */
	b = find_buf(f->f_file->o_file, f->f_file->o_len);
	lock_buf(b);
	fs = index_buf(b, 0, 1);

	/*
	 * Assemble as many names as will fit, starting at
	 * given byte offset.
	 */
	bufcnt = 0;
	step = 0;
	for (;;) {
		struct fs_dirent *d;
		uint slen;

		/*
		 * Map in next run of entries if need more
		 */
		if (step < sizeof(struct fs_dirent)) {
			char *p;

			if (!bmap(fs, f->f_pos, len-bufcnt, &p, &step)) {
				break;
			}
			ASSERT_DEBUG(step >= sizeof(*d),
				"vfs_readdir: bad size");
			d = (struct fs_dirent *)p;
		}

		/*
		 * Check that it'll fit.  Leave loop when it doesn't.
		 */
		slen = strlen(d->fs_name)+1;
		if ((bufcnt + slen) >= len) {
			break;
		}

		/*
		 * Add name and update counters
		 */
		sprintf(buf + bufcnt, "%s\n", d->fs_name);
		bufcnt += slen;
		f->f_pos += sizeof(struct fs_dirent);
		step -= sizeof(struct fs_dirent);
		++d;
	}

	/*
	 * Done with dir, allow contents to be reclaimed
	 */
	unlock_buf(b);

	/*
	 * Send back results
	 */
	m->m_buf = buf;
	m->m_arg = m->m_buflen = bufcnt;
	m->m_nseg = ((bufcnt > 0) ? 1 : 0);
	m->m_arg1 = 0;
	msg_reply(m->m_sender, m);
	free(buf);
}

/*
 * vfs_read()
 *	Read bytes out of the current file or directory
 *
 * Directories get their own routine.
 */
void
vfs_read(struct msg *m, struct file *f)
{
	struct buf *b, *bufs[MSGSEGS];
	struct fs_file *fs;
	uint lim, cnt, nseg;

	/*
	 * Map in file structure info
	 */
	b = find_buf(f->f_file->o_file, f->f_file->o_len);
	fs = index_buf(b, 0, 1);

	/*
	 * Directory--only one is the root
	 */
	if (fs->fs_type == FT_DIR) {
		vfs_readdir(m, f);
		return;
	}

	/*
	 * Access?
	 */
	if (!(f->f_perm & ACC_READ)) {
		msg_err(m->m_sender, EPERM);
		return;
	}

	/*
	 * EOF?
	 */
	if (f->f_pos >= fs->fs_len) {
		m->m_arg = m->m_arg1 = m->m_buflen = m->m_nseg = 0;
		msg_reply(m->m_sender, m);
		return;
	}

	/*
	 * Hold fs_file until we're all done
	 */
	lock_buf(b);

	/*
	 * Calculate # bytes to get
	 */
	lim = m->m_arg;
	if (lim > (fs->fs_len - f->f_pos)) {
		lim = fs->fs_len - f->f_pos;
	}

	/*
	 * Build message segments
	 */
	cnt = 0;
	for (nseg = 0; (cnt < lim) && (nseg < MSGSEGS); ++nseg) {
		uint sz;
		struct buf *b2;
		char *p;

		/*
		 * Get next block of data
		 */
		b2 = bufs[nseg] = bmap(fs, f->f_pos, lim-cnt, &p, &sz);
		if (b2 == 0) {
			break;
		}
		if (b2 != b) {
			lock_buf(b2);
		}

		/*
		 * Put into next message segment
		 */
		m->m_seg[nseg].s_buf = p;
		m->m_seg[nseg].s_buflen = sz;

		/*
		 * Advance counter
		 */
		cnt += sz;
		f->f_pos += sz;
	}

	/*
	 * Send back reply
	 */
	m->m_arg = cnt;
	m->m_nseg = nseg;
	m->m_arg1 = 0;
	msg_reply(m->m_sender, m);

	/*
	 * Free up bufs
	 */
	for (cnt = 0; cnt < nseg; ++cnt) {
		struct buf *b2;

		if ((b2 = bufs[cnt]) != b) {
			unlock_buf(b2);
		}
	}
	unlock_buf(b);
}
