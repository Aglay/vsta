#ifndef _MACH_MUTEX_H
#define _MACH_MUTEX_H
/*
 * mutex.h
 *	Machine specific mutex facilities
 *
 * These functions are inlined throughout the rest of the code to provide
 * increased performance.
 *
 * One point worth noting is that some of the code has been expanded
 * slightly from what it might have been, but this results in an initial
 * decision about the execution path being based on one of the function
 * parameters.  As these parameters are usually constants it makes it
 * easy for the compiler's optimiser to remove the unwanted path and
 * actually end up with more compact code :-)
 */
#include <sys/percpu.h>
#include <sys/mutex.h>
#include <sys/assert.h>
#include "locore.h"

/*
 * ATOMIC_INCL()
 *	Increment an integer atomically
 */
inline static void
ATOMIC_INC(void *val)
{
	__asm__ __volatile__(
		"lock\n\t"
		"incl (%0)\n\t"
		: /* No output */
		: "r" (val)); 
}

/*
 * ATOMIC_DEC()
 *	Decrement an integer atomically
 */
inline static void
ATOMIC_DEC(void *val)
{
	__asm__ __volatile__(
		"lock\n\t"
		"decl (%0)\n\t"
		: /* No output */
		: "r" (val)); 
}

/*
 * ATOMIC_INCL()
 *	Increment a long integer atomically
 */
inline static void
ATOMIC_INCL(void *val)
{
	__asm__ __volatile__(
		"lock\n\t"
		"incl (%0)\n\t"
		: /* No output */
		: "r" (val)); 
}

/*
 * ATOMIC_DECL()
 *	Decrement a long integer atomically
 */
inline static void
ATOMIC_DECL(void *val)
{
	__asm__ __volatile__(
		"lock\n\t"
		"decl (%0)\n\t"
		: /* No output */
		: "r" (val)); 
}

/*
 * ATOMIC_INCL_CPU_LOCKS()
 *	Increment the number of locks held atomically
 */
inline static void
ATOMIC_INCL_CPU_LOCKS(void)
{
	__asm__ __volatile__(
		"lock\n\t"
		"incl %0\n\t"
		: /* No output */
		: "m" (cpu.pc_locks));
}

/*
 * ATOMIC_DECL_CPU_LOCKS()
 *	Decrement the number of locks held atomically
 */
inline static void
ATOMIC_DECL_CPU_LOCKS(void)
{
	__asm__ __volatile__(
		"lock\n\t"
		"decl %0\n\t"
		: /* No output */
		: "m" (cpu.pc_locks));
}

/*
 * p_lock()
 *	Take spinlock
 */
inline static spl_t
p_lock(lock_t *l, spl_t s)
{
	ASSERT_DEBUG(l->l_lock == 0, "p_lock: deadlock");

	if (s == SPLHI) {
		spl_t x;

		x = geti();
		cli();
		ATOMIC_INCL_CPU_LOCKS();
		l->l_lock = 1;
		return(x);
	} else {
		l->l_lock = 1;
		ATOMIC_INCL_CPU_LOCKS();
		return(SPL0);
	}
}

/*
 * p_lock_fast()
 *	Take a spinlock, but don't return anything
 */
inline static void
p_lock_fast(lock_t *l, spl_t s)
{
	ASSERT_DEBUG(l->l_lock == 0, "p_lock: deadlock");

	if (s == SPLHI) {
		cli();
	}
	ATOMIC_INCL_CPU_LOCKS();
	l->l_lock = 1;
}

/*
 * cp_lock()
 *	Conditional take of spinlock
 */
inline static spl_t
cp_lock(lock_t *l, spl_t s)
{
	if (s == SPLHI) {
		if (!l->l_lock) {
			spl_t x;

			x = geti();
			cli();
			ATOMIC_INCL_CPU_LOCKS();
			l->l_lock = 1;
			return(x);
		} else {
			return(-1);
		} 
	} else {
		if (!l->l_lock) {
			ATOMIC_INCL_CPU_LOCKS();
			l->l_lock = 1;
			return(SPL0);
		} else {
			return(-1);
		}
	}
}

/*
 * v_lock()
 *	Release spinlock
 */
inline static void
v_lock(lock_t *l, spl_t s)
{
	ASSERT_DEBUG(l->l_lock, "v_lock: not held");
	l->l_lock = 0;
	if (s == SPL0) {
		sti();
	}
	ATOMIC_DECL_CPU_LOCKS();
}

/*
 * init_lock()
 *	Initialize lock to "not held"
 */
inline static void
init_lock(lock_t *l)
{
	l->l_lock = 0;
}

/*
 * blocked_sema()
 *	Tell if anyone's sleeping on the semaphore
 */
inline static int
blocked_sema(sema_t *s)
{
	return (s->s_count < 0);
}

/*
 * init_sema()
 * 	Initialize semaphore
 *
 * The s_count starts at 1.
 */
inline static void
init_sema(sema_t *s)
{
	s->s_count = 1;
	s->s_sleepq = 0;
	init_lock(&s->s_lock);
}

/*
 * set_sema()
 *	Manually set the value for the semaphore count
 *
 * Use with care; if you strand someone on the queue your system
 * will start to act funny.  If DEBUG is on, it'll probably panic.
 */
inline static void
set_sema(sema_t *s, int cnt)
{
	s->s_count = cnt;
}

#endif /* _MACH_MUTEX_H */