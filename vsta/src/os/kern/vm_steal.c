/*
 * vm_steal.c
 *	Routines for stealing memory when needed
 *
 * There is no such thing as swapping in VSTa.  Just page stealing.
 *
 * The basic mechanism is a two-handed clock algorithm.  This technique
 * provides a smooth mechanism for reaping pages across a wide range
 * of configurations.  However, in a multi-CPU environment it certainly
 * presents some challenges.
 *
 * The per-page information includes an attach list data structure.
 * This is used by the basic pageout algorithm to enumerate the
 * current users of a given page.  The pageout daemon enumerates
 * each page set for a page, updating the central page's notion
 * of its modifed and referenced bits.  For the forward hand, it
 * clears the referenced bit.  For the back hand, it reaps the page
 * if the referenced bit is still clear.
 *
 * Locking is tricky.  The attach list uses the page lock.  Because it
 * is taken before the slot locks in the page sets, page sets which
 * are busy must be skipped.   Especially critical is the TLB shoot-
 * down implied by the deletion of a translation in a multi-threaded
 * process.  The HAT for such an implementation will not be trivial.
 */
#include <sys/types.h>
#include <mach/param.h>
#include <sys/pview.h>
#include <sys/pset.h>
#include <sys/core.h>
#include <sys/fs.h>
#include <sys/vm.h>
#include <sys/malloc.h>
#include <sys/misc.h>
#include <sys/assert.h>
#include "../mach/mutex.h"
#include "pset.h"

/* #define WATCHMEM /* */

#define CONTAINS(base, cnt, num) \
	(((num) >= (base)) && ((num) < ((base)+(cnt))))

/* Entries to avoid */
#define BAD(c) ((c)->c_flags & (C_SYS|C_BAD|C_WIRED))

/*
 * The following are in terms of 1/nth memory
 */
#define SPREAD 8		/* Distance between hands */
#define DESFREE 8		/* When less than this, start stealing */
#define MINFREE 16		/* When less than this, synch pushes */
				/* AT MOST: */
#define SMALLSCAN 32		/* Scan this much when free > DESFREE */
#define MEDSCAN 16		/*  ... when free > MINFREE */
#define LARGESCAN 4		/*  ... when free < MINFREE */

#define PAGEOUT_SECS (5)	/* Interval to run pageout() */

extern uint freemem, totalmem;		/* Free and total pages in system */
					/*  total does not include C_SYS */
uint pageout_secs = 0;		/* Set to PAGEOUT_SECS when ready */
uint desfree, minfree;		/* Initial values calculated on boot */
extern struct portref
	*swapdev;		/* To tell when it's been enabled */

/*
 * getbits()
 *	Unvirtualize all mappings for a given slot
 *
 * Frees the attach list elements as well.
 */
static int
getbits(struct perpage *pp)
{
	struct atl *a;
	struct pview *pv;
	struct vas *vas;
	int flags = 0;

	for (a = pp->pp_atl; a; a = a->a_next) {

		/*
		 * No VAS == cached view, ignore
		 */
		pv = a->a_pview;
		vas = pv->p_vas;
		if (!vas) {
			continue;
		}

		/*
		 * Reap ref/mod bits from HAT, which clears them
		 * at that level.
		 */
		flags |= hat_getbits(pv,
			(char *)pv->p_vaddr + ptob(a->a_idx));
	}
	return(flags);
}

/*
 * unvirt()
 *	Unvirtualize all mappings for a given slot
 *
 * Frees the attach list elements as well.
 */
static int
unvirt(struct perpage *pp)
{
	struct atl *a, *an, **ap;
	int flags = 0;

	ap = &pp->pp_atl;
	for (a = *ap; a; a = an) {
		struct pview *pv;
		struct vas *vas;

		an = a->a_next;
		pv = a->a_pview;
		vas = pv->p_vas;

		/*
		 * Don't steal locked memory
		 */
		if (vas) {
			/*
			 * Leave entry be if memory locked
			 */
			if (vas->v_flags & VF_MEMLOCK) {
				ap = &a->a_next;
				continue;
			}

			/*
			 * Delete translation, gather latest
			 * ref/mod state
			 */
			hat_deletetrans(pv, (char *)pv->p_vaddr +
				ptob(a->a_idx), pp->pp_pfn);
			flags |= hat_getbits(pv,
				(char *)pv->p_vaddr + ptob(a->a_idx));
		}

		/*
		 * Free attach list element, update ref count
		 */
		*ap = an;
		FREE(a, MT_ATL);
		ASSERT_DEBUG(pp->pp_refs > 0, "unvirt: underflow");
		pp->pp_refs -= 1;
	}
	return(flags);
}

/*
 * walk_master()
 *	Apply a function to each appropriate master COW page slot
 *
 * The resulting bits are placed back into pp_flags.  The "fn" may
 * delete HAT translations as we assemble pp_flags.
 */
static void
walk_master(intfun fn, struct pset *ps, struct perpage *pp, uint idx)
{
	struct pset *ps2;
	struct perpage *pp2;
	uint idx2;

	/*
	 * First handle direct references
	 */
	pp->pp_flags |= (*fn)(pp);

	/*
	 * Walk each potential COW reference to our slot
	 */
	for (ps2 = ps->p_cowsets; ps2 && pp->pp_refs; ps2 = ps2->p_cowsets) {
		/*
		 * If the pset doesn't cover our range of the master,
		 * it can't be involved so ignore it.
		 */
		if (!CONTAINS(ps2->p_off, ps2->p_len, idx)) {
			continue;
		}

		/*
		 * Try to lock.  Since the canonical order is
		 * cow->master, we must back off on potential
		 * deadlock.
		 */
		if (cp_lock(&ps2->p_lock, SPL0)) {
			continue;
		}

		/*
		 * Examine the page slot indicated.  Again, lock with
		 * care as we're going rather backwards.  We back off
		 * if we would have hit deadlock.
		 */
		idx2 = idx - ps2->p_off;
		pp2 = find_pp(ps2,  idx2);
		if (clock_slot(ps2, pp2)) {
			v_lock(&ps2->p_lock, SPL0);
			continue;
		}

		/*
		 * We process only pages associated with
		 * the COW set here.  Pages are otherwise
		 * normal data pages.
		 */
		if (pp2->pp_flags & PP_COW) {
			ASSERT_DEBUG(pp2->pp_refs, "steal_master: pp2 !refs");

			pp->pp_flags |= (*fn)(pp2);
			if (pp2->pp_refs == 0) {
				pp2->pp_flags &= ~(PP_COW|PP_V|PP_R);
				pp->pp_refs -= 1;
			}

		}
		unlock_slot(ps2, pp2);
	}
}

/*
 * steal_master()
 *	Handle stealing of pages from master copy of COW
 */
steal_master(struct pset *ps, struct perpage *pp, uint idx,
		int trouble, intfun steal)
{
	/*
	 * Accumulate state of modification without tearing
	 * anything down.  This gives us an approximation of
	 * the page's state.
	 */
	walk_master(getbits, ps, pp, idx);

	/*
	 * If we don't want to steal it yet, just clear ref bit (COW master
	 * can't be modified!)
	 */
	ASSERT_DEBUG((pp->pp_flags & PP_M) == 0, "steal_master: modified");
	if (!(*steal)(pp->pp_flags, trouble)) {
		pp->pp_flags &= ~(PP_R);
		return;
	}

	/*
	 * If we can successfully steal all translations, free the memory
	 */
	walk_master(unvirt, ps, pp, idx);
	ASSERT_DEBUG((pp->pp_flags & PP_M) == 0, "steal_master: modified");
	if (pp->pp_refs == 0) {
		free_page(pp->pp_pfn);
		pp->pp_flags &= ~(PP_R|PP_V);
	}
}

/*
 * do_hand()
 *	Do hand algorithm
 *
 * Steal pages as appropriate, do all the fancy locking.
 */
static void
do_hand(struct core *c, int trouble, intfun steal)
{
	struct pset *ps;
	struct perpage *pp;
	uint idx, pgidx;
	int slot_held;

	/*
	 * No point fussing over inactive pages, eh?
	 */
	if ((c->c_flags & C_ALLOC) == 0) {
		return;
	}

	/*
	 * Lock physical page
	 */
	pgidx = c-core;
	if (clock_page(pgidx)) {
		return;
	}

	/*
	 * Avoid bad pages and those which are changing state
	 */
	ps = c->c_pset;
	if (BAD(c) || (ps == 0)) {
		unlock_page(pgidx);
		return;
	}
	idx = c->c_psidx;

	/*
	 * Lock pset
	 */
	p_lock_fast(&ps->p_lock, SPL0);

	/*
	 * 0 references means we've come upon this pset in the process
	 * of creation or tear-down.  Leave it alone.
	 */
	if (ps->p_refs == 0) {
		v_lock(&ps->p_lock, SPL0_SAME);
		unlock_page(pgidx);
		return;
	}

	/*
	 * Access indicated slot
	 */
	pp = find_pp(ps, idx);
	if (clock_slot(ps, pp)) {
		v_lock(&ps->p_lock, SPL0_SAME);
		unlock_page(pgidx);
		return;
	}
	slot_held = 1;

	/*
	 * The core pointer to a pset should point to the *master*
	 * pset for copy-on-write situations.
	 */
	ASSERT_DEBUG((pp->pp_flags & PP_COW) == 0,
		"do_hand: cow in set_core");

	/*
	 * If this is the target for COW psets, several assumptions
	 * can be made, so handle in its own routine.
	 */
	if ((ps->p_type != PT_COW) && ps->p_cowsets) {
		steal_master(ps, pp, idx, trouble, steal);
		goto out;
	}

	/*
	 * Scan each attached view of the slot, update our notion of
	 * page state.  Take away all translations so our notion will
	 * remain correct until we release the page slot.
	 */
	pp->pp_flags |= getbits(pp);

	/*
	 * See if we'd like to take a shot at stealing this page
	 */
	if (!(*steal)(pp->pp_flags, trouble)) {
		/*
		 * Nope, we don't want it yet.  If trouble's brewing,
		 * schedule an async flush of the dirty page.
		 */
		if (trouble && (pp->pp_flags & PP_M)) {
			(*(ps->p_ops->psop_writeslot))(ps,
				pp, idx, iodone_unlock);
			pp->pp_flags &= ~(PP_M);
			slot_held = 0;	/* Released in iodone_unlock */
		}

		/*
		 * Clear the
		 * ref bits so we can estimate usage in the future
		 * when things get tight.
		 */
		pp->pp_flags &= ~(PP_R);
		goto out;
	}

	/*
	 * Try and grab it away
	 */
	pp->pp_flags |= unvirt(pp);

	/*
	 * If there are refs, it can only mean the page is involved
	 * in a memory-locked region.  Leave it alone.
	 */
	if (pp->pp_refs > 0) {
		goto out;
	}

	/*
	 * If any trouble, see if this page is should be
	 * stolen.
	 */
	if (!(pp->pp_flags & PP_M)) {
		/*
		 * Yup.  Take it away.
		 */
		free_page(pp->pp_pfn);
		pp->pp_flags &= ~(PP_V|PP_R);
	} else {
		/*
		 * It's dirty, so clean and free
		 */
		ASSERT_DEBUG(swapdev, "do_hand: !swapdev");
		(*(ps->p_ops->psop_writeslot))(ps, pp, idx, iodone_free);
		slot_held = 0;	/* Released in iodone_free */
	}
out:
	if (slot_held) {
		unlock_slot(ps, pp);
	}
	unlock_page(pgidx);
}

/*
 * steal1()
 *	Steal algorithm for hand #1
 */
static int
steal1(int flags, int trouble)
{
	return (trouble > 1);
}

/*
 * steal2()
 *	Steal algorithm for hand #2
 */
static int
steal2(int flags, int trouble)
{
	return (trouble && !(flags & (PP_R|PP_M)));
}

/*
 * pageout()
 *	Endless routine to detect memory shortages and steal pages
 */
pageout(void)
{
	struct core *base, *top, *hand1, *hand2;
	int trouble, npg;
	int troub_cnt[3];

/*
 * Not sure if these need to be macros; have to move "top" and "base"
 * to globals to do it in its own function.
 */
#define INC(hand) {if (++(hand) >= top) hand = base; }
#define ADVANCE(hand) \
	{ INC(hand); \
		while (BAD(hand)) { \
			INC(hand); \
		} \
	}

	/*
	 * Check for mistake.  Not designed to be MP safe; "shouldn't
	 * happen", anyway.
	 */
	if (pageout_secs) {
		return(err(EBUSY));
	}
	pageout_secs = PAGEOUT_SECS;

	/*
	 * Skip to first usable page, also record top
	 */
	for (base = core; !BAD(base); ++base)
		;
	top = coreNCORE;

	/*
	 * Set clock hands
	 */
	hand1 = hand2 = base;
	for (npg = totalmem/SPREAD; npg > 0; npg -= 1) {
		ADVANCE(hand1);
	}
	npg = 0;

	/*
	 * Calculate scan counts for each situation
	 */
	troub_cnt[0] = totalmem/SMALLSCAN;
	troub_cnt[1] = totalmem/MEDSCAN;
	troub_cnt[2] = totalmem/LARGESCAN;
	minfree = totalmem/MINFREE;
	desfree = totalmem/DESFREE;

	/*
	 * Main loop
	 */
	for (;;) {
		/*
		 * Categorize the current memory situation
		 */
		if (freemem < minfree) {
			trouble = 2;
		} else if (freemem < desfree) {
			trouble = 1;
		} else {
			trouble = 0;
		}

		/*
		 * Sleep after the appropriate number of iterations
		 */
		if (npg > troub_cnt[trouble]) {
			npg = 0;
#ifdef WATCHMEM
			{
				ulong ofree, odes, omin, otroub;

				if ((freemem != ofree) ||
					(desfree != odes) ||
					(minfree != omin) ||
					(trouble != otroub)) {
					printf("pageout free %d des %d " \
						"min %d trouble %d\n",
						freemem, desfree,
						minfree, trouble);
					ofree = freemem;
					odes = desfree;
					omin = minfree;
					otroub = trouble;
				}
			}
#endif

			/*
			 * Suspend until the next interval
			 */
			interval_sleep(pageout_secs);
		}

		/*
		 * Do the two hand algorithms
		 */
		do_hand(hand1, trouble, steal1);
		ADVANCE(hand1);
		do_hand(hand2, trouble, steal2);
		ADVANCE(hand2);
		npg += 1;
	}
}
