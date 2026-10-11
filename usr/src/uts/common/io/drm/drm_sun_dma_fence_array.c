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
 * dma_fence_array -- wait-all (or wait-any) fence combinator.
 *
 * When enable_signaling is called, registers a callback on each child
 * fence. Each callback decrements a pending counter. When the counter
 * reaches zero, the parent array fence is signaled.
 *
 * The parent signal is deferred to a taskq to avoid lock ordering
 * issues (the child callback runs under the child's fence_lock, and
 * signaling the parent would try to acquire the parent's fence_lock).
 */

#include <sys/types.h>
#include <sys/kmem.h>
#include <sys/atomic.h>
#include <sys/taskq.h>
#include <sys/systm.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include "drm_sun_dma_fence_array.h"

/* Forward declarations */
static const char *dma_fence_array_get_driver_name(dma_fence_t *);
static const char *dma_fence_array_get_timeline_name(dma_fence_t *);
static boolean_t dma_fence_array_enable_signaling(dma_fence_t *);
static boolean_t dma_fence_array_signaled(dma_fence_t *);
static void dma_fence_array_release(dma_fence_t *);

const dma_fence_ops_t dma_fence_array_ops = {
	.get_driver_name	= dma_fence_array_get_driver_name,
	.get_timeline_name	= dma_fence_array_get_timeline_name,
	.enable_signaling	= dma_fence_array_enable_signaling,
	.signaled		= dma_fence_array_signaled,
	.release		= dma_fence_array_release,
};

static const char *
dma_fence_array_get_driver_name(dma_fence_t *fence)
{
	return ("dma_fence_array");
}

static const char *
dma_fence_array_get_timeline_name(dma_fence_t *fence)
{
	return ("unbound");
}

/*
 * Taskq callback: signal the parent array fence.
 * This runs in process context, safe to acquire any lock.
 */
static void
dma_fence_array_signal_work(void *arg)
{
	dma_fence_array_t *array = arg;

	(void) dma_fence_signal(&array->dfa_base);
	dma_fence_put(&array->dfa_base);
}

/*
 * Callback invoked when a child fence signals.
 */
static void
dma_fence_array_cb_func(dma_fence_t *fence, dma_fence_cb_t *cb)
{
	dma_fence_array_cb_t *array_cb =
	    (dma_fence_array_cb_t *)((char *)cb -
	    offsetof(dma_fence_array_cb_t, fac_cb));
	dma_fence_array_t *array = array_cb->fac_array;

	/*
	 * Propagate error from child to parent (first error wins).
	 */
	if (fence->fence_error != 0 && array->dfa_base.fence_error == 0)
		array->dfa_base.fence_error = fence->fence_error;

	if (atomic_dec_32_nv(&array->dfa_num_pending) == 0) {
		/*
		 * Last child signaled. Defer the parent signal to a taskq
		 * because we're running under the child's fence_lock and
		 * cannot safely acquire the parent's lock here.
		 *
		 * The reference taken in enable_signaling keeps the array
		 * alive until the taskq callback runs.
		 */
		taskq_init_ent(&array->dfa_tqent);
		taskq_dispatch_ent(system_taskq,
		    dma_fence_array_signal_work, array,
		    TQ_NOSLEEP, &array->dfa_tqent);
	} else {
		/* Not the last one -- drop the reference from enable */
		dma_fence_put(&array->dfa_base);
	}
}

/*
 * Enable signaling: register callbacks on all child fences.
 * Called under the array's fence_lock.
 *
 * For each child, takes a reference on the array. When the child
 * fires its callback, it drops that reference (or the last child
 * defers it to the taskq signal callback which drops it).
 */
static boolean_t
dma_fence_array_enable_signaling(dma_fence_t *fence)
{
	dma_fence_array_t *array =
	    (dma_fence_array_t *)((char *)fence -
	    offsetof(dma_fence_array_t, dfa_base));
	uint32_t i;

	for (i = 0; i < array->dfa_num_fences; i++) {
		dma_fence_array_cb_t *acb = &array->dfa_callbacks[i];
		acb->fac_array = array;

		/*
		 * Take a reference on the array for each callback.
		 * This keeps the array alive until all callbacks fire.
		 */
		dma_fence_get(&array->dfa_base);

		if (dma_fence_add_callback(array->dfa_fences[i],
		    &acb->fac_cb, dma_fence_array_cb_func) != 0) {
			/*
			 * Child already signaled (-ENOENT).
			 * Decrement pending and drop our reference.
			 */
			if (array->dfa_fences[i]->fence_error != 0 &&
			    array->dfa_base.fence_error == 0)
				array->dfa_base.fence_error =
				    array->dfa_fences[i]->fence_error;

			if (atomic_dec_32_nv(&array->dfa_num_pending) == 0) {
				/* All children already signaled */
				dma_fence_put(&array->dfa_base);
				return (B_FALSE);
			}
			dma_fence_put(&array->dfa_base);
		}
	}

	return (B_TRUE);
}

/*
 * Fast poll: check if the pending count is zero.
 */
static boolean_t
dma_fence_array_signaled(dma_fence_t *fence)
{
	dma_fence_array_t *array =
	    (dma_fence_array_t *)((char *)fence -
	    offsetof(dma_fence_array_t, dfa_base));

	return (array->dfa_num_pending == 0);
}

/*
 * Release: drop references on all child fences, free the array.
 */
static void
dma_fence_array_release(dma_fence_t *fence)
{
	dma_fence_array_t *array =
	    (dma_fence_array_t *)((char *)fence -
	    offsetof(dma_fence_array_t, dfa_base));
	uint32_t i;
	size_t alloc_sz;

	for (i = 0; i < array->dfa_num_fences; i++)
		dma_fence_put(array->dfa_fences[i]);

	kmem_free(array->dfa_fences,
	    array->dfa_num_fences * sizeof (dma_fence_t *));

	/* Destroy the base fence internals */
	list_destroy(&array->dfa_base.fence_cb_list);
	cv_destroy(&array->dfa_base.fence_wait_cv);
	mutex_destroy(&array->dfa_base.fence_wait_lock);
	mutex_destroy(&array->dfa_lock);

	alloc_sz = sizeof (dma_fence_array_t) +
	    array->dfa_num_fences * sizeof (dma_fence_array_cb_t);
	kmem_free(array, alloc_sz);
}

/*
 * Create a fence array.
 *
 * Arguments:
 *   num_fences    -- number of child fences
 *   fences        -- array of dma_fence_t pointers (TAKES OWNERSHIP --
 *                    the array takes a reference on each; the caller's
 *                    references are consumed)
 *   context       -- from dma_fence_context_alloc(), or 0
 *   seqno         -- sequence number within context
 *   signal_on_any -- B_TRUE to signal when ANY child signals
 *
 * Returns NULL on allocation failure.
 */
dma_fence_array_t *
dma_fence_array_create(uint32_t num_fences, dma_fence_t **fences,
    uint64_t context, uint64_t seqno, boolean_t signal_on_any)
{
	dma_fence_array_t *array;
	size_t alloc_sz;

	ASSERT(num_fences > 0);
	ASSERT(fences != NULL);

	alloc_sz = sizeof (dma_fence_array_t) +
	    num_fences * sizeof (dma_fence_array_cb_t);
	array = kmem_zalloc(alloc_sz, KM_NOSLEEP);
	if (array == NULL)
		return (NULL);

	mutex_init(&array->dfa_lock, NULL, MUTEX_DEFAULT, NULL);
	array->dfa_num_fences = num_fences;
	array->dfa_num_pending = signal_on_any ? 1 : num_fences;
	array->dfa_fences = fences; /* takes ownership of the array */
	array->dfa_signal_on_any = signal_on_any;

	dma_fence_init(&array->dfa_base, &dma_fence_array_ops,
	    &array->dfa_lock, context, seqno);

	return (array);
}
