#include <ananas/types.h>
#include <ananas/waitqueue.h>
#include <ananas/lock.h>
#include <ananas/pcpu.h>
#include <ananas/schedule.h>
#include <ananas/lib.h>
#include <ananas/mm.h>

#define WAITER_QUEUE_SIZE 100

static struct SPINLOCK spl_freelist;
static struct WAIT_QUEUE w_freelist;

void
waitqueue_init(struct WAIT_QUEUE* wq)
{
	if (wq == NULL) {
		/* Create the freelist with WAITER_QUEUE_SIZE items */
		struct WAITER* w_items = kmalloc(sizeof(struct WAIT_QUEUE) * WAITER_QUEUE_SIZE);
		DQUEUE_INIT(&w_freelist);
		for (int i = 0; i < WAITER_QUEUE_SIZE; i++) {
			DQUEUE_ADD_TAIL(&w_freelist, w_items);
			w_items++;
		}

		spinlock_init(&spl_freelist);
		return;
	}

	DQUEUE_INIT(wq);
	spinlock_init(&wq->wq_lock);
}

void
waitqueue_reset_waiter(struct WAITER* w)
{
	struct THREAD* t = PCPU_GET(curthread);
	KASSERT(w->w_thread == t, "waiter does not belong to our thread");

	spinlock_lock(&w->w_lock);
	w->w_signalled = 0;
	spinlock_unlock(&w->w_lock);
}

struct WAITER*
waitqueue_add(struct WAIT_QUEUE* wq)
{
	/* Fetch a fresh new waiter */
	spinlock_lock(&spl_freelist);
	KASSERT(!DQUEUE_EMPTY(&w_freelist), "out of waiters"); /* XXX deal with this */
	struct WAITER* w = DQUEUE_HEAD(&w_freelist);
	DQUEUE_POP_HEAD(&w_freelist);
	spinlock_unlock(&spl_freelist);

	/* Initialize the waiter */
	w->w_thread = PCPU_GET(curthread);
	w->w_wq = wq;
	w->w_signalled = 0;

	/* Append our waiter to the queue */
	spinlock_lock(&wq->wq_lock);
	DQUEUE_ADD_TAIL(wq, w);
	spinlock_unlock(&wq->wq_lock);

	return w;
}

void
waitqueue_remove(struct WAITER* w)
{
	struct THREAD* t = PCPU_GET(curthread);
	KASSERT(w->w_thread == t, "waiter does not belong to our thread");

	/* Remove our waiter from the waitqueue */
	spinlock_lock(&w->w_wq->wq_lock);
	KASSERT(!DQUEUE_EMPTY(w->w_wq), "queue cannot be empty");
	DQUEUE_REMOVE(w->w_wq, w);
	spinlock_unlock(&w->w_wq->wq_lock);

	/* Hook it up to the freelist */
	spinlock_lock(&spl_freelist);
	DQUEUE_ADD_TAIL(&w_freelist, w);
	spinlock_unlock(&spl_freelist);
}

void
waitqueue_wait(struct WAITER* w)
{
	struct THREAD* t = PCPU_GET(curthread);
	KASSERT(w->w_thread == t, "waiter does not belong to our thread");

	if (t == NULL)
		return;

	/* Wait until we are adequately scheduled */
	for (;;) {
		/*
		 * Note that we must be cautious here: once a thread has called
		 * thread_suspend(), there's a possibility that the scheduler irq kicks in
		 * and the thread will not continue.
		 *
		 * This is troublesome, as we need to *atomically* check whether w_signalled is
		 * zero _and_ suspend the thread if it is. The invariant that needs to be
	 	 * honored is that thread_suspend() is only called iff w_signalled = 0.
		 *
		 * This solves the possible race of:
		 *
		 * 1) <thread t> w_signalled is zero ...
		 * 2) <irq> waitqueue_signal() gets called, increments
		 *    w_signalled and resumes the thread.
		 * 3) <thread t> w_signalled was zero, so we suspend.
		 *
		 * The end result is that <thread t> never wakes up; this problem is solved
		 * by using a spinlock to ensure the signalled state doesn't change while
	 	 * our thread is being suspended  - note that this spinlock must be
		 * unpremptible because the scheduler irq can otherwise kick in while
		 * we still need to unlock the spinlock, causing a deadlock.
		 */
		int state = spinlock_lock_unpremptible(&w->w_lock);
		if (w->w_signalled > 0) {
			spinlock_unlock_unpremptible(&w->w_lock, state);
			break;
		}
		thread_suspend(t);
		spinlock_unlock_unpremptible(&w->w_lock, state);
		reschedule();
	}
}

void
waitqueue_signal(struct WAIT_QUEUE* wq)
{
	spinlock_lock(&wq->wq_lock);
	KASSERT(!DQUEUE_EMPTY(wq), "signalling without waiters");
	DQUEUE_FOREACH(wq, w, struct WAITER) {
		spinlock_lock(&w->w_lock);

		/* Signal the waiter */
		w->w_signalled++;

		/* Wake up the corresponding thread */
		thread_resume(w->w_thread);

		spinlock_unlock(&w->w_lock);
	}
	spinlock_unlock(&wq->wq_lock);
}

/* vim:set ts=2 sw=2: */
