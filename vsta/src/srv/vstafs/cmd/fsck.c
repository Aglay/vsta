/*
 * fsck.c
 *	A simple/dumb file system checker
 */
#include <stat.h>
#include <std.h>
#include <fcntl.h>
#include <unistd.h>
#include "../vstafs.h"

#define NBBY (8)		/* # bits in a byte */

static int fd;			/* Open block device */
static daddr_t max_blk;		/* Highest block # in filesystem */
static char *freemap;		/* Bitmap of allocated blocks */
static char *allocmap;		/*  ...of blocks in files */

/*
 * get_sec()
 *	Return the named sector, maintain a little cache
 */
static void *
get_sec(daddr_t sec)
{
	static void *buf = 0;
	static daddr_t lastsec;
	off_t pos;

	if (buf && (lastsec == sec)) {
		return(buf);
	}
	buf = malloc(SECSZ);
	if (!buf) {
		perror("malloc secbuf");
		exit(1);
	}
	pos = stob(sec);
	if (lseek(fd, pos, SEEK_SET) != pos) {
printf("Error seeking to sector %ld: %s\n", sec, strerror());
		exit(1);
	}
	if (read(fd, buf, SECSZ) != SECSZ) {
printf("Error reading sector %ld: %s\n", sec, strerror());
		exit(1);
	}
	return(buf);
}

/*
 * setbit()
 *	Set bit in map corresponding to the given blocks
 */
static void
setbit(char *map, daddr_t d, ulong cnt)
{
	while (cnt-- != 0) {
		map[d / NBBY] |= (1 << (d % NBBY));
		d += 1;
	}
}

/*
 * getbit()
 *	Return bit in map for block
 *
 * Returns non-zero if any of the range is found set, otherwise 0.
 */
static int
getbit(char *map, daddr_t d, ulong cnt)
{
	while (cnt-- != 0) {
		if (map[d / NBBY] & (1 << (d % NBBY))) {
			return(1);
		}
		d += 1;
	}
	return(0);
}

/*
 * check_root()
 *	Read in root of filesystem, verify
 *
 * Also allocates some data structures.
 */
static void
check_root(void)
{
	struct fs *fs;
	struct stat sb;

	printf("Read root of filesystem\n");

	/*
	 * Get base sector, basic verification
	 */
	fs = get_sec(BASE_SEC);
	if (fs->fs_magic != FS_MAGIC) {
		printf("Filesystem has bad magic number.\n");
		exit(1);
	}

	/*
	 * See if size matches overall size
	 */
	if (fs->fs_size == 0) {
		printf("Filesystem has zero size!\n");
		exit(1);
	}
	if (fstat(fd, &sb) < 0) {
		perror("stat");
		printf("Couldn't stat device.\n");
		exit(1);
	}
	if (fs->fs_size > btors(sb.st_size)) {
		printf("Filesystem larger than underlying disk.\n");
		exit(1);
	}
	max_blk = fs->fs_size-1;

	/*
	 * Get the free block bitmap
	 */
	freemap = calloc(fs->fs_size/NBBY + 1, 1);
	allocmap = calloc(fs->fs_size/NBBY + 1, 1);
	if (!freemap || !allocmap) {
		perror("malloc");
		printf("No memory for bitmaps.\n");
		exit(1);
	}
}

/*
 * valid_block()
 *	Tell if given block number lies within filesystem
 */
static int
valid_block(daddr_t d)
{
	if (d < ROOT_SEC) {
		return(0);
	}
	if (d > max_blk+1) {
		return(0);
	}
	return(1);
}

/*
 * check_freelist()
 *	Read in free list blocks, check
 *
 * When this routine finishes, freemap represents which blocks
 * are free.  That is, a 1 means it's present on the freelist.
 */
static void
check_freelist(void)
{
	struct free *fr;
	daddr_t next, highest = FREE_SEC+1;

	printf("Scan freelist\n");

	/*
	 * Walk all sectors chained in free list
	 */
	next = FREE_SEC;
	do {
		struct alloc *a;
		uint x;

		/*
		 * Get next sector, sanity check its format
		 */
		fr = get_sec(next);
		if (fr->f_nfree > NALLOC) {
printf("Free list sector %ld claims %d free slots\n",
	next, fr->f_nfree);
			exit(1);
		}

		/*
		 * Now walk each slot & sanity check the members
		 */
		a = fr->f_free;
		for (x = 0; x < fr->f_nfree; ++x) {
			/*
			 * Verify start and end of range lie within
			 * the filesystem.
			 */
			if (!valid_block(a->a_start)) {
printf("Free list sector %ld slot %d lists invalid block %ld\n",
	next, x, a->a_start);
				exit(1);
			}
			if (!valid_block(a->a_start + a->a_len)) {
printf("Free list sector %ld slot %d lists invalid block length %ld\n",
	next, x, a->a_len);
				exit(1);
			}

			/*
			 * Verify block list is still sorted.
			 */
			if (a->a_start < highest) {
				if (a->a_start <= FREE_SEC) {
printf("Free list sector %ld slot %d lists bad block %ld\n",
	next, x, a->a_start);
					exit(1);
				}
printf("Free list sector %ld slot %d has out-of-order block %ld\n",
	next, x, a->a_start);
				exit(1);
			}
			highest = a->a_start + a->a_len;

			/*
			 * Mark free blocks into freemap.  Note that
			 * we don't check for the bit being set already,
			 * because our sort check above guarantees this.
			 */
			setbit(freemap, a->a_start, a->a_len);
		}
	} while (next = fr->f_next);
}

/*
 * check_fsalloc()
 *	Check each fs_blks[] entry for sanity
 *
 * Returns 1 for error, 0 for OK.
 */
static int
check_fsalloc(char *name, struct fs_file *fs)
{
	uint x;
	ulong blklen = 0;

	for (x = 0; x < fs->fs_nblk; ++x) {
		struct alloc *a;

		a = &fs->fs_blks[x];

		/*
		 * Run tally of length based on blocks
		 */
		blklen += a->a_len;

		/*
		 * Basic sanity check on block range
		 */
		if (!valid_block(a->a_start)) {
printf("File %s block %ld invalid.\n", name, a->a_start);
			return(1);
		}
		if (a->a_len > max_blk) {
printf("File %s block %ld length invalid.\n", name, a->a_start);
			return(1);
		}
		if (!valid_block(a->a_start + a->a_len)) {
printf("File %s block range %ld..%ld invalid.\n",
	name, a->a_start, a->a_start + a->a_len - 1);
			return(1);
		}

		/*
		 * See if any have been found in another file's allocation
		 */
		if (getbit(allocmap, a->a_start, a->a_len)) {
printf("File %s blocks %ld..%ld doubly allocated.\n",
	a->a_start, a->a_start + a->a_len - 1);
			return(1);
		}

		/*
		 * See if any of them are present on the freelist
		 */
		if (getbit(freemap, a->a_start, a->a_len)) {
printf("File %s blocks %ld..%ld conflict with freelist.\n",
	a->a_start, a->a_start + a->a_len - 1);
			return(1);
		}

		/*
		 * All's well, flag them in both allocmap and freemap.
		 * We mark them in freemap so when we're done we can
		 * scan for blocks which have been found neither on
		 * the freelist nor in a file.
		 */
		setbit(allocmap, a->a_start, a->a_len);
		setbit(freemap, a->a_start, a->a_len);
	}

	/*
	 * Sanity check fs_len vs. block allocation
	 */
	if (btors(fs->fs_len) > blklen) {
printf("File %s has length %ld but only blocks for %ld\n",
	name, fs->fs_len, stob(blklen));
		return(1);
	}
	if (btors(fs->fs_len) != blklen) {
printf("File %s has %ld excess blocks off end\n",
	name, blklen - btors(fs->fs_len));
		return(1);
	}

	return(0);
}

/*
 * check_dirent()
 *	Tell if the named directory entry is sane
 *
 * Returns 1 on problem, 0 on OK.
 */
static int
check_dirent(struct fs_dirent *d)
{
	uint x;

	/*
	 * The starting sector has already been sanity checked
	 */

	/*
	 * Name must be ASCII characters, and '\0'-terminated
	 */
	for (x = 0; x < MAXNAMLEN; ++x) {
		char c;

		/*
		 * High bit in first means "file deleted", but that
		 * shouldn't get here.
		 * XXX I don't think I'm skipping deleted entries yet
		 */
		c = d->fs_name[x];
		if (c & 0x80) {
			return(1);
		}

		/*
		 * Printable are fine.  Yes, we allow '/'--this could
		 * be supported if you tweak your open() lookup loop to
		 * use another path seperator character.
		 */
		if ((c >= '\1') && (c < 0x7F)) {
			continue;
		}

		/*
		 * End of name.  There has to be at least one character.
		 */
		if (c == '\0') {
			if (x == 0) {
				return(1);
			}
			return(0);
		}
	}

	/*
	 * If we drop out the bottom, we never found the '\0', so
	 * gripe about the entry.
	 */
	return(1);
}

/*
 * check_fsdir()
 *	Check each directory entry in the given file
 *
 * Like check_tree(), returns 1 on problem, 0 on OK.
 */
static int
check_fsdir(daddr_t sec, char *name)
{
	ulong idx = sizeof(struct fs_file), file_len;
	struct fs_file *fs;
	uint x;

	/*
	 * Snapshot the two size fields, so we can refer to them
	 * even when our sector buffer may have been reused
	 */
	fs = get_sec(sec);
	file_len = fs->fs_len;

	/*
	 * An easy initial check
	 */
	if ((file_len % sizeof(struct fs_dirent)) != 0) {
printf("Error: directory %s has unaligned length %ld\n", name, file_len);
		return(1);
	}

	/*
	 * Walk through each directory entry, checking sanity
	 */
	for (x = 0; ; ++x) {
		daddr_t blk, blkend;
		struct alloc *a;

		/*
		 * Drop out if we've walked all the blocks
		 */
		fs = get_sec(sec);
		if (x >= fs->fs_nblk) {
			break;
		}

		/*
		 * Look at next contiguous allocation extent
		 */
		a = &fs->fs_blks[x];
		blk = a->a_start;
		blkend = blk + a->a_len;
		while (blk < blkend) {
			struct fs_dirent *d, *dend;

			/*
			 * Position at top of this sector.  For the first
			 * sector in a file, skip the fs_file part.
			 */
			d = get_sec(blk);
			dend = (struct fs_dirent *)((char *)d + SECSZ);
			if (idx == sizeof(struct fs_file)) {
				d = (struct fs_dirent *)((char *)d + idx);
			}

			/*
			 * Walk the entries, sanity checking
			 */
			while (d < dend) {
				if (idx >= file_len) {
					break;
				}
				if (check_dirent(d)) {
printf("Corrupt directory entry file %s position %ld\n",
	name, idx - sizeof(struct fs_file));
					return(1);
				}
				idx += sizeof(struct fs_dirent);
			}
		}
	}
	return(0);
}

/*
 * check_tree()
 *	Walk the directory tree, verify connectivity
 *
 * Also checks that each block is marked allocated, and verifies
 * that each block lives under only one file.
 *
 * Returns 0 if entry was OK or fixable, 1 if the entry is entirely
 * hosed.
 */
static int
check_tree(daddr_t sec, char *name)
{
	struct fs_file *fs;

	printf("Scan directory tree\n");

	/*
	 * Basic sanity check on starting sector
	 */
	if (sec > max_blk) {
printf("Invalid starting block for %s: %ld\n", name, sec);
		return(1);
	}

	/*
	 * Check filetype
	 */
	fs = get_sec(sec);
	if ((fs->fs_type != FT_FILE) && (fs->fs_type != FT_DIR)) {
printf("Unknown file node for %s at %ld\n", name, sec);
		return(1);
	}

	/*
	 * Make sure there's an fs_blk[0], and a length
	 */
	if (fs->fs_nblk == 0) {
printf("No blocks allocated for %s at %ld\n", name, sec);
		return(1);
	}
	if (fs->fs_len < sizeof(struct fs_file)) {
printf("Internal file length too short for %s--%ld\n", name, fs->fs_len);
		return(1);
	}

	/*
	 * Verify that first sector is this fs_file
	 */
	if (fs->fs_blks[0].a_start != sec) {
printf("Dir entry for %s at block %ld mismatches alloc information\n",
	name, sec);
		return(1);
	}

	/*
	 * Things look basically sane.  Check each allocated extent.
	 */
	if (check_fsalloc(name, fs)) {
		return(1);
	}

	/*
	 * If this is a file, we're now happy with its contents.
	 */
	if (fs->fs_type == FT_FILE) {
		return(0);
	}

	/*
	 * For directories, check each fs_dirent
	 */
	return(check_fsdir(sec, name));
}

main(int argc, char **argv)
{
	if (argc != 2) {
		printf("Usage is: %s <disk>\n", argv[0]);
		exit(1);
	}
	if ((fd = open(argv[1], O_READ)) < 0) {
		perror(argv[1]);
		exit(1);
	}
	check_root();
	check_freelist();
	check_tree(ROOT_SEC, "/");
}
