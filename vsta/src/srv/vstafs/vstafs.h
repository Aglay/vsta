#ifndef VSTAFS_H
#define VSTAFS_H
/*
 * vstafs.h
 *	Definitions for the VSTa-specific filesystem
 *
 * The VSTa filesystem has the following features:
 *	Hierarchical, acyclic directory structure
 *	28-character filenames
 *	Automatic file versioning
 *	Hardened filesystem semantics
 *	Extent-based storage allocation
 */
#include <sys/perm.h>
#include <sys/fs.h>

#define SECSZ (512)		/* Basic size of allocation units */
#define MAXEXT (32)		/* Max # extents in a file */
#define MAXNAMLEN (28)		/* Max chars in dir entry name */
#define EXTSIZ (128)		/* File growth increment */
#define NCACHE (64)		/* Crank up if you have lots of users */
#define CORESEC (512)		/* Sectors to buffer in core at once */

/* Disk addresses, specified as a sector number */
typedef unsigned long daddr_t;

/* Conversion of units: bytes<->sectors */
#define btos(x) ((x) / SECSZ)
#define btors(x) (((x) + SECSZ-1) / SECSZ)
#define stob(x) ((x) * SECSZ)

/*
 * The first sector of a filesystem
 */
#define BASE_SEC ((daddr_t)0)
struct fs {
	ulong fs_magic;		/* Magic number */
	ulong fs_size;		/* # sectors in filesystem */
	ulong fs_extsize;	/* Contiguous space allocated on extension */
	daddr_t fs_free;	/* Start of free list */
};
#define FS_MAGIC (0xDEADFACE)	/* Value for fs_magic */

/*
 * Next sector is root directory
 */
#define ROOT_SEC ((daddr_t)1)

/*
 * Sector after is first block of free list entries
 */
#define FREE_SEC ((daddr_t)2)

/*
 * The <start,len> pair describing file extents and contiguous
 * chunks on the free list.
 */
struct alloc {
	daddr_t a_start;	/* Starting sector # */
	ulong a_len;		/* Length, in sectors */
};

/*
 * A free list sector
 */
struct free {
	daddr_t f_next;		/* Next sector of free list */
	uint f_nfree;		/* # free in this sector */
#define NALLOC ((SECSZ-2*sizeof(ulong))/sizeof(struct alloc))
	struct alloc		/* Zero or more */
		f_free[NALLOC];
};

/*
 * A directory is simply a file structured as fs_dirent records
 */
struct fs_dirent {
	char fs_name[MAXNAMLEN];	/* Name */
	daddr_t fs_clstart;		/* Starting cluster # */
};

/*
 * The first part of a file is a description of the file and its
 * block allocation.  The file's contents follows.
 */
struct fs_file {
	daddr_t fs_clstart;	/* Starting cluster # */
	daddr_t fs_prev;	/* Previous version of this file */
	ulong fs_rev;		/* Revision # */
	ulong fs_len;		/* File length in bytes */
	ushort fs_type;		/* Type of file */
	ushort fs_nlink;	/* # dir entries pointing to this */
	struct prot		/* Protection on this file */
		fs_prot;
	uint fs_nblk;		/* # extents */
	struct alloc		/* ...<start,off> tuples of extents */
		fs_blks[MAXEXT];
	char fs_data[0];	/* Data starts here */
};

/*
 * File types
 */
#define FT_FILE (1)
#define FT_DIR (2)

/* # bytes which reside at the tail of the file's information sector */
#define OFF_DATA (sizeof(struct fs_file))
#define OFF_SEC1 (SECSZ - ((int)(((struct fs_file *)0)->fs_data)))

/*
 * Structure of an open file in the filesystem
 */
struct openfile {
	daddr_t o_file;		/* 1st sector of file */
	ulong o_len;		/*  ...first extent's length */
	uint o_refs;		/* # references */
};

/*
 * Our per-client data structure
 */
struct file {
	struct openfile		/* Current file open */
		*f_file;
	ulong f_pos;		/* Current file offset */
	struct perm		/* Things we're allowed to do */
		f_perms[PROCPERMS];
	uint f_nperm;
	uint f_perm;		/*  ...for the current f_file */
};

/*
 * Globals and such to keep us honest
 */
extern void read_sec(daddr_t, void *), write_sec(daddr_t, void *);
extern void read_secs(daddr_t, void *, uint),
	write_secs(daddr_t, void *, uint);
extern int blkdev;
extern void vfs_open(struct msg *, struct file *),
	vfs_read(struct msg *, struct file *),
	vfs_write(struct msg *, struct file *),
	vfs_remove(struct msg *, struct file *),
	vfs_stat(struct msg *, struct file *),
	vfs_close(struct file *),
	vfs_fid(struct msg *, struct file *);
extern void binit(void), fat_init(void), dir_init(void);
extern void ref_node(struct openfile *), deref_node(struct openfile *);
extern struct openfile *alloc_node(daddr_t);
extern uint fs_perms(struct perm *, uint, struct openfile *);

#endif /* VSTAFS_H */
