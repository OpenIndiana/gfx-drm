/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 The illumos DRM Authors
 */

/*
 * dma_fence implementation for illumos.
 *
 * The signal path is the hot path, called from GPU interrupt handlers:
 *
 *   1. atomic_cas_32 sets SIGNALED (exactly one caller wins)
 *   2. mutex_enter(fence->fence_lock) -- adaptive, safe at PIL <= 10
 *   3. Record timestamp, move cb list, dispatch callbacks
 *   4. mutex_exit(fence->fence_lock)
 *   5. cv_broadcast on fence_wait_cv to wake blocked waiters
 *
 * Callbacks run under fence_lock and must be fast and non-blocking.
 */

#include <sys/types.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/atomic.h>
#include <sys/list.h>
#include <sys/time.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/cmn_err.h>
#include "drm_sun_dma_fence.h"

/* Global context counter, atomically incremented */
static volatile uint64_t dma_fence_context_counter = 1;

/*
 * Allocate num contiguous globally unique context IDs.
 */
uint64_t
dma_fence_context_alloc(unsigned int num)
{
	return (atomic_add_64_nv(&dma_fence_context_counter, num) - num);
}

/*
 * Initialize a fence. Caller provides:
 *   - ops: mandatory (get_driver_name and get_timeline_name required)
 *   - lock: pointer to a shared kmutex_t for this context
 *   - context: from dma_fence_context_alloc()
 *   - seqno: monotonically increasing within context
 *
 * The fence starts with refcount 1, unsignaled.
 */
void
dma_fence_init(dma_fence_t *fence, const dma_fence_ops_t *ops,
    kmutex_t *lock, uint64_t context, uint64_t seqno)
{
	ASSERT(ops != NULL);
	ASSERT(ops->get_driver_name != NULL);
	ASSERT(ops->get_timeline_name != NULL);
	ASSERT(lock != NULL);

	fence->fence_lock = lock;
	fence->fence_ops = ops;
	drm_ref_init(&fence->fence_ref);
	fence->fence_flags = 0;
	fence->fence_error = 0;
	fence->fence_context = context;
	fence->fence_seqno = seqno;
	fence->fence_timestamp = 0;

	list_create(&fence->fence_cb_list, sizeof (dma_fence_cb_t),
	    offsetof(dma_fence_cb_t, fcb_node));

	mutex_init(&fence->fence_wait_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&fence->fence_wait_cv, NULL, CV_DEFAULT, NULL);
}

/*
 * Enable signaling if not already done. Called under fence_lock.
 * Returns B_TRUE if signaling is enabled (or fence not yet signaled).
 * Returns B_FALSE if fence is already signaled.
 */
static boolean_t
dma_fence_enable_signaling_locked(dma_fence_t *fence)
{
	uint32_t old, new;

	if (fence->fence_flags & DMA_FENCE_FLAG_SIGNALED)
		return (B_FALSE);

	/* Set ENABLE_SIGNAL bit atomically */
	do {
		old = fence->fence_flags;
		if (old & DMA_FENCE_FLAG_SIGNALED)
			return (B_FALSE);
		if (old & DMA_FENCE_FLAG_ENABLE_SIGNAL)
			return (B_TRUE); /* already enabled */
		new = old | DMA_FENCE_FLAG_ENABLE_SIGNAL;
	} while (atomic_cas_32(&fence->fence_flags, old, new) != old);

	/* First time: call driver's enable_signaling */
	if (fence->fence_ops->enable_signaling != NULL) {
		if (!fence->fence_ops->enable_signaling(fence)) {
			/* Driver says already done or error */
			(void) dma_fence_signal_locked(fence);
			return (B_FALSE);
		}
	}

	return (B_TRUE);
}

/*
 * Signal a fence. Called with fence_lock already held.
 *
 * Returns 0 on success (this call was the one that signaled it),
 * or -EINVAL if the fence was already signaled.
 */
int
dma_fence_signal_locked(dma_fence_t *fence)
{
	uint32_t old, new;
	list_t cb_list;
	dma_fence_cb_t *cb;

	ASSERT(fence != NULL);

	/* One-shot: atomically set SIGNALED. Only one caller succeeds. */
	do {
		old = fence->fence_flags;
		if (old & DMA_FENCE_FLAG_SIGNALED)
			return (-EINVAL);
		new = old | DMA_FENCE_FLAG_SIGNALED | DMA_FENCE_FLAG_TIMESTAMP;
	} while (atomic_cas_32(&fence->fence_flags, old, new) != old);

	fence->fence_timestamp = gethrtime();

	/*
	 * Move the callback list to a local variable. This prevents
	 * callbacks from interfering with the fence's list during
	 * iteration, and allows the fence to be freed in a callback.
	 */
	list_create(&cb_list, sizeof (dma_fence_cb_t),
	    offsetof(dma_fence_cb_t, fcb_node));
	list_move_tail(&cb_list, &fence->fence_cb_list);

	/* Dispatch all callbacks under fence_lock */
	while ((cb = list_remove_head(&cb_list)) != NULL) {
		list_link_init(&cb->fcb_node);
		cb->fcb_func(fence, cb);
	}

	list_destroy(&cb_list);

	/* Wake blocked waiters */
	mutex_enter(&fence->fence_wait_lock);
	cv_broadcast(&fence->fence_wait_cv);
	mutex_exit(&fence->fence_wait_lock);

	return (0);
}

/*
 * Signal a fence. Acquires fence_lock internally.
 * Safe to call from interrupt context (adaptive mutex at PIL <= 10).
 */
int
dma_fence_signal(dma_fence_t *fence)
{
	int ret;

	ASSERT(fence != NULL);

	/* Quick check without lock */
	if (fence->fence_flags & DMA_FENCE_FLAG_SIGNALED)
		return (-EINVAL);

	mutex_enter(fence->fence_lock);
	ret = dma_fence_signal_locked(fence);
	mutex_exit(fence->fence_lock);

	return (ret);
}

/*
 * Register a callback on the fence.
 *
 * The cb struct must be caller-allocated (typically embedded in a
 * larger struct via container_of). The fcb_node field is used for
 * list linkage.
 *
 * Returns:
 *   0       -- callback registered, will be called on signal
 *   -ENOENT -- fence already signaled, callback NOT called
 */
int
dma_fence_add_callback(dma_fence_t *fence, dma_fence_cb_t *cb,
    dma_fence_func_t func)
{
	int ret = 0;

	ASSERT(fence != NULL);
	ASSERT(cb != NULL);
	ASSERT(func != NULL);

	/* Quick check: already signaled? */
	if (fence->fence_flags & DMA_FENCE_FLAG_SIGNALED)
		return (-ENOENT);

	mutex_enter(fence->fence_lock);

	if (!dma_fence_enable_signaling_locked(fence)) {
		/* Fence is signaled */
		list_link_init(&cb->fcb_node);
		ret = -ENOENT;
	} else {
		cb->fcb_func = func;
		list_insert_tail(&fence->fence_cb_list, cb);
	}

	mutex_exit(fence->fence_lock);
	return (ret);
}

/*
 * Remove a previously registered callback.
 *
 * Returns B_TRUE if the callback was removed from the list.
 * Returns B_FALSE if the fence was already signaled (callback was
 * already dispatched or is being dispatched).
 */
boolean_t
dma_fence_remove_callback(dma_fence_t *fence, dma_fence_cb_t *cb)
{
	boolean_t removed = B_FALSE;

	mutex_enter(fence->fence_lock);
	if (list_link_active(&cb->fcb_node)) {
		list_remove(&fence->fence_cb_list, cb);
		removed = B_TRUE;
	}
	mutex_exit(fence->fence_lock);

	return (removed);
}

/*
 * Enable software signaling. Ensures the driver's enable_signaling
 * callback has been called so an interrupt will fire.
 */
void
dma_fence_enable_sw_signaling(dma_fence_t *fence)
{
	if (fence->fence_flags & DMA_FENCE_FLAG_SIGNALED)
		return;

	mutex_enter(fence->fence_lock);
	(void) dma_fence_enable_signaling_locked(fence);
	mutex_exit(fence->fence_lock);
}

/*
 * Block until fence is signaled, timeout expires, or signal is delivered.
 *
 * Arguments:
 *   fence      -- the fence to wait on
 *   intr       -- B_TRUE for interruptible (returns -EINTR on signal)
 *   timeout_ns -- relative timeout in nanoseconds. 0 means wait forever.
 *
 * Returns:
 *   > 0  -- remaining time (always 1 if no timeout specified)
 *   0    -- timed out
 *   -EINTR -- interrupted by signal (only if intr == B_TRUE)
 */
clock_t
dma_fence_wait_timeout(dma_fence_t *fence, boolean_t intr,
    hrtime_t timeout_ns)
{
	clock_t ret;
	hrtime_t deadline;
	boolean_t has_timeout;

	ASSERT(fence != NULL);

	/* Already signaled? */
	if (dma_fence_is_signaled(fence))
		return (timeout_ns > 0 ? timeout_ns : 1);

	/* Enable signaling so the driver arms its interrupt */
	dma_fence_enable_sw_signaling(fence);

	/* Already signaled after enabling? */
	if (dma_fence_is_signaled(fence))
		return (timeout_ns > 0 ? timeout_ns : 1);

	has_timeout = (timeout_ns > 0);
	if (has_timeout)
		deadline = gethrtime() + timeout_ns;

	mutex_enter(&fence->fence_wait_lock);

	while (!dma_fence_is_signaled(fence)) {
		if (has_timeout) {
			hrtime_t remaining = deadline - gethrtime();
			if (remaining <= 0) {
				/* Timed out */
				mutex_exit(&fence->fence_wait_lock);
				return (0);
			}

			if (intr) {
				ret = cv_timedwait_sig_hrtime(
				    &fence->fence_wait_cv,
				    &fence->fence_wait_lock,
				    deadline);
			} else {
				ret = cv_timedwait_hires(
				    &fence->fence_wait_cv,
				    &fence->fence_wait_lock,
				    deadline, NANOSEC / 1000, 0);
			}
		} else {
			if (intr) {
				ret = cv_wait_sig(&fence->fence_wait_cv,
				    &fence->fence_wait_lock);
			} else {
				cv_wait(&fence->fence_wait_cv,
				    &fence->fence_wait_lock);
				ret = 1; /* normal wakeup */
			}
		}

		/*
		 * Check for signal delivery (interruptible waits).
		 * cv_wait_sig returns 0 on signal.
		 * cv_timedwait_sig_hrtime returns 0 on signal.
		 */
		if (intr && ret == 0) {
			mutex_exit(&fence->fence_wait_lock);
			return (-EINTR);
		}

		/*
		 * cv_timedwait_hires returns -1 on timeout.
		 * cv_timedwait_sig_hrtime returns -1 on timeout.
		 */
		if (has_timeout && ret == -1) {
			/* Check once more before declaring timeout */
			if (dma_fence_is_signaled(fence))
				break;
			mutex_exit(&fence->fence_wait_lock);
			return (0);
		}
	}

	mutex_exit(&fence->fence_wait_lock);

	if (has_timeout) {
		hrtime_t remaining = deadline - gethrtime();
		return (remaining > 0 ? remaining : 1);
	}
	return (1);
}

/*
 * Release a fence (called when refcount drops to 0).
 */
void
dma_fence_release(dma_fence_t *fence)
{
	ASSERT(fence != NULL);

	/*
	 * If there are outstanding callbacks and the fence is not signaled,
	 * something went very wrong (refcounting bug). Force-signal to
	 * prevent dangling callback references.
	 */
	if (!list_is_empty(&fence->fence_cb_list) &&
	    !(fence->fence_flags & DMA_FENCE_FLAG_SIGNALED)) {
		cmn_err(CE_WARN, "dma_fence_release: fence %p has "
		    "unsignaled callbacks, force-signaling", (void *)fence);
		fence->fence_error = -EDEADLK;
		(void) dma_fence_signal(fence);
	}

	if (fence->fence_ops->release != NULL) {
		fence->fence_ops->release(fence);
	} else {
		dma_fence_free(fence);
	}
}

/*
 * Default free for fences allocated with kmem_zalloc(sizeof(dma_fence_t)).
 *
 * Drivers that embed dma_fence_t in a larger struct MUST provide their
 * own release op -- this function only knows about sizeof(dma_fence_t).
 */
void
dma_fence_free(dma_fence_t *fence)
{
	list_destroy(&fence->fence_cb_list);
	cv_destroy(&fence->fence_wait_cv);
	mutex_destroy(&fence->fence_wait_lock);
	kmem_free(fence, sizeof (dma_fence_t));
}
