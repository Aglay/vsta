#ifndef _PROC_H
#define _PROC_H
/*
 * proc.h
 *	Per-process data
 */
#include <sys/perm.h>
#include <sys/param.h>
#include <lib/llist.h>
#include <sys/mutex.h>

/*
 * Permission bits for current process
 */
#define P_PRIO 1	/* Can set process priority */
#define P_SIG 2		/*  ... deliver events */
#define P_KILL 4	/*  ... terminate */
#define P_STAT 8	/*  ... query status */
#define P_DEBUG 16	/*  ... debug */

/*
 * Description of a process group
 */
struct pgrp {
	uint pg_nmember;	/* # PID's in group */
	uint pg_nelem;		/* # PID's in pg_members[] array */
	ulong *pg_members;	/* Pointer to linear array */
	sema_t pg_sema;		/* Mutex for pgrp */
};
#define PG_GROWTH (20)		/* # slots pg_members[] grows by */

/*
 * Status a child leaves behind on exit()
 */
struct exitst {
	ulong e_pid;		/* PID of exit() */
	int e_code;		/* Argument to exit() */
	ulong e_usr, e_sys;	/* CPU time in user and sys */
	struct exitst *e_next;	/* Next in list */
};

/*
 * An exit group.  All children of the same parent belong to the
 * same exit group.  When children exit, they will leave an exit
 * status message linked here if the parent is still alive.
 */
struct exitgrp {
	struct proc *e_parent;	/* Pointer to parent of group */
	sema_t e_sema;		/* Sema bumped on each exit */
	struct exitst *e_stat;	/* Status of each exit() */
	ulong e_refs;		/* # references (parent + children) */
	lock_t e_lock;		/* Mutex for fiddling */
};

/*
 * The actual per-process state
 */
struct proc {
	ulong p_pid;		/* Our process ID */
	struct perm		/* Permissions granted process */
		p_ids[PROCPERMS];
	sema_t p_sema;		/* Semaphore on proc structure */
	struct prot p_prot;	/* Protections of this process */
	struct thread		/* List of threads in this process */
		*p_threads;
	struct vas *p_vas;	/* Virtual address space of process */
	struct sched *p_runq;	/* Scheduling node for all threads */
	struct port		/* Ports this proc owns */
		*p_ports[PROCPORTS];
	struct portref		/* "files" open by this process */
		*p_open[PROCOPENS];
	struct hash		/* Portrefs attached to our ports */
		*p_prefs;
	ushort p_nopen;		/*  ...# currently open */
	struct proc		/* Linked list of all processes */
		*p_allnext;
	ulong p_sys, p_usr;	/* Cumulative time for all prev threads */
	voidfun p_handler;	/* Handler for events */
	struct pgrp		/* Process group for proc */
		*p_pgrp;
	struct exitgrp		/* Exit groups */
		*p_children,	/*  ...the one our children use */
		*p_parent;	/*  ...the one we're a child of */
};

#ifdef KERNEL
extern ulong allocpid(void);
extern int alloc_open(struct proc *);
extern void free_open(struct proc *, int);
extern void join_pgrp(struct pgrp *, ulong),
	leave_pgrp(struct pgrp *, ulong);
extern struct pgrp *alloc_pgrp(void);

/*
 * notify() syscall and special value for thread ID to signal whole
 * process group instead.
 */
extern notify(ulong, ulong, char *, int);
#define NOTIFY_PG ((ulong)-1)

/*
 * Functions for fiddling exit groups
 */
extern struct exitgrp *alloc_exitgrp(struct proc *);
extern void deref_exitgrp(struct exitgrp *);
extern void noparent_exitgrp(struct exitgrp *);
extern void post_exitgrp(struct exitgrp *, struct proc *, int);
extern struct exitst *wait_exitgrp(struct exitgrp *);

#endif /* KERNEL */

#endif /* _PROC_H */
