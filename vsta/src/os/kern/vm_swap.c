/*
 * vm_swap.c
 *	Routines and data structures for the swap pseudo-device
 *
 * XXX swapdev must initially point to La-La land.  We'll have to
 * initially fake it until the system comes up far enough to get
 * some swap configured.  Perhaps just assume swap will be at least
 * 1 M, let people allocate, panic on pageout, and when we get our
 * first group of swap, insist it be >= 1 M, and "slip it in" as
 * the real space for the already-allocated swap blocks.
 */
#include <sys/types.h>
#include <sys/port.h>
#include <sys/msg.h>
#include <sys/vm.h>
#include <sys/perm.h>
#include <swap/swap.h>
#include <sys/mutex.h>
#include <sys/assert.h>

extern struct seg *kern_mem();
extern void *malloc();

struct portref *swapdev = 0;
sema_t swap_wait;
lock_t swap_lock;
static ulong swap_pending = 0L;	/* Pages consumed before swapper up */

/*
 * The most privileged protection
 */
static struct perm kern_perm = {0};

/*
 * pageio()
 *	Set up a synchronous page I/O
 *
 * This isn't used strictly for swap, though swapdev is probably
 * the most common destination for pageios.
 */
pageio(uint pfn, struct portref *pr, uint off, uint cnt, int op)
{
	struct sysmsg *sm;
	int error = 0;
	int holding_pr = 0, holding_port = 0;

	ASSERT_DEBUG((op == FS_ABSREAD) || (op == FS_ABSWRITE),
		"pageio: illegal op");

	ASSERT(pr, "pageio: swap but no swapdev");
	/*
	 * Construct a system message
	 */
	sm = malloc(sizeof(struct sysmsg));
	sm->m_sender = pr;
	sm->m_op = op;
	sm->m_nseg = 1;
	sm->m_arg = off;
	sm->m_seg[0] = kern_mem(ptov(ptob(pfn)), cnt);

	/*
	 * One at a time through the portref
	 */
	p_sema(&pr->p_sema, PRIHI);
	p_lock(&pr->p_lock, SPL0); holding_pr = 1;

	/*
	 * If port gone, I/O error
	 */
	if (pr->p_port == 0) {
		error = 1;
		goto out;
	}

	/*
	 * Set up our message transfer state
	 */
	set_sema(&pr->p_iowait, 0);
	pr->p_state = PS_IOWAIT;

	/*
	 * Put message on queue
	 */
	queue_msg(pr->p_port, sm);

	/*
	 * Now wait for the I/O to finish or be interrupted
	 */
	p_sema_v_lock(&pr->p_iowait, PRIHI, &pr->p_lock);
	holding_pr = 0;

	/*
	 * If the server indicates error, set it and leave
	 */
	if (sm->m_arg == -1) {
		error = 1;
		goto out;
	}
	ASSERT(sm->m_nseg == 0, "pageio: got segs back");

out:
	/*
	 * Clean up and return success/failure
	 */
	if (holding_pr) {
		v_lock(&pr->p_lock, SPL0);
	}
	if (sm) {
		free(sm);
	}
	v_sema(&pr->p_sema);
	return(error);
}

/*
 * set_swapdev()
 *	System call to set swap manager
 *
 * The calling process must have previously opened a portref to
 * the swap manager.  We verify permission and then steal the
 * portref and make it available as "swapdev".
 */
set_swapdev(port_t arg_port)
{
	struct portref *pr;
	extern struct portref *delete_portref();

	/*
	 * Make sure he's "root"
	 */
	if (!isroot()) {
		return(-1);
	}

	/*
	 * See if we already have one
	 */
	if (swapdev) {
		return(err(EBUSY));
	}

	/*
	 * Steal away his port
	 */
	pr = delete_portref(arg_port);
	if (!pr) {
		return(-1);
	}

	/*
	 * It becomes swapdev
	 */
	ASSERT(pr, "set_swapdev: swap daemon died");
	swapdev = pr;
	v_sema(&swapdev->p_sema);
	return(0);
}

/*
 * alloc_swap()
 *	Request a chunk of swap from the swap manager
 */
ulong
alloc_swap(uint pages)
{
	long args[2];

	/*
	 * If there appears to be swap usage pending and the
	 * swap manager appears to be up, account for the
	 * space now.
	 */
	args[0] = 0;
	if (swap_pending && swapdev) {
		(void)p_lock(&swap_lock, SPL0);
		if (swap_pending) {
			args[0] = swap_pending;
			swap_pending = 0;
		}
		v_lock(&swap_lock, SPL0);
	}

	/*
	 * We assume that the initial chunk of swap starts at 1--
	 * otherwise our bootup would have no way of assigning
	 * block numbers to the "pending" swap.
	 */
	if (args[0]) {
		ASSERT(alloc_swap(args[0]) == 1,
			"alloc_swap: pend != 1");
	}

	/*
	 * If no swap manager, run a "pending" tally
	 */
	if (!swapdev) {
		(void)p_lock(&swap_lock, SPL0);
		if (!swapdev) {
			args[0] = swap_pending+1;
			swap_pending += pages;
			v_lock(&swap_lock, SPL0);
			return(args[0]);
		}
		v_lock(&swap_lock, SPL0);
	}

	/*
	 * Loop until we get swap space
	 */
	for (;;) {
		ASSERT(swapdev, "alloc_swap: manager not ready");
		args[0] = pages; args[1] = 0;
		p_sema(&swapdev->p_sema, PRIHI);
		ASSERT(kernmsg_send(swapdev, SWAP_ALLOC, args) == 0,
			"alloc_swap: failed send");
		v_sema(&swapdev->p_sema);
		if (args[0] > 0) {
			return(args[0]);
		}
		p_sema(&swap_wait, PRIHI);
	}
}

/*
 * free_swap()
 *	Free back a chunk of swap to the swap manager
 */
void
free_swap(ulong block, uint pages)
{
	long args[2];

	ASSERT(swapdev, "free_swap: manager dead");
	args[0] = block;
	args[1] = pages;
	p_sema(&swapdev->p_sema, PRIHI);
	ASSERT(kernmsg_send(swapdev, SWAP_FREE, args) == 0,
		"free_swap: send failed");
	v_sema(&swapdev->p_sema);
	if (blocked_sema(&swap_wait)) {
		vall_sema(&swap_wait);
	}
}

/*
 * init_swap()
 *	Initialize swapping code
 *
 * Note, this does not mean we're getting the swap server running.
 * This routine is just to initialize some basic mutexes.
 */
void
init_swap(void)
{
	init_sema(&swap_wait); set_sema(&swap_wait, 0);
	init_lock(&swap_lock);
}
