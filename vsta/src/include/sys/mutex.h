#ifndef _MUTEX_H
#define _MUTEX_H
/*
 * mutex.h
 *	Both sleep- and spin-oriented mutual exclusion mechanisms
 */
#include <sys/types.h>

/*
 * A spinlock
 */
typedef struct lock {
	uchar l_lock;
} lock_t;

/*
 *  A sleeping semaphore
 *
 * The s_count can be viewed as a count of the number of p_sema()'s
 * which would go through before a p_sema() would be blocked.
 */
typedef struct sema {
	lock_t s_lock;		/* For manipulating the semaphore */
	int s_count;		/* Count */
	struct thread		/* List of threads waiting */
		*s_sleepq;
} sema_t;

/*
 * Constants
 */
typedef uint spl_t;
#define SPL0 (0)		/* Spin with interrupts enabled */
#define SPLHI (0xFF)		/* Spin with interrupts disabled */
typedef uint pri_t;
#define PRILO (0)		/* Sleep interruptibly */
#define PRICATCH (0x7F)		/* PRILO, but p_sema returns error code */
#define PRIHI (0xFF)		/* Sleep uninterruptibly */

/*
 * Routines
 */
spl_t p_lock(lock_t *, spl_t);		/* For locks */
spl_t cp_lock(lock_t *, spl_t);
void v_lock(lock_t *, spl_t);
void init_lock(lock_t *);
int p_sema(sema_t *, pri_t);		/* For semaphores */
int cp_sema(sema_t *);
void v_sema(sema_t *);
void vall_sema(sema_t *);
int blocked_sema(sema_t *);
void init_sema(sema_t *);
void set_sema(sema_t *, int);
int p_sema_v_lock(sema_t *, pri_t, lock_t *);

/*
 * Atomic increment/decrement.  Sometimes saves you a full lock operation.
 */
extern void ATOMIC_INC(void *);
extern void ATOMIC_DEC(void *);

#endif /* _MUTEX_H */
