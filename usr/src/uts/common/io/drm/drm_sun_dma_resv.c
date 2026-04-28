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
 * dma_resv -- reservation object implementation for illumos.
 *
 * Associates a dynamically-sized set of dma_fences with a buffer
 * object, protected by a ww_mutex. The fence list is a simple
 * array that grows on demand via dma_resv_reserve_fences().
 *
 * All fence access requires holding the ww_mutex. For waiting,
 * we snapshot the fence list under the lock, take references,
 * drop the lock, then wait. This avoids holding the ww_mutex
 * while sleeping.
 */

#include <sys/types.h>
#include <sys/kmem.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include "drm_sun_dma_resv.h"

DEFINE_WW_CLASS(reservation_ww_class);

void
dma_resv_init(dma_resv_t *resv)
{
	ww_mutex_init(&resv->dr_lock, &reservation_ww_class);
	resv->dr_fences = NULL;
}

void
dma_resv_fini(dma_resv_t *resv)
{
	dma_resv_list_t *list = resv->dr_fences;

	if (list != NULL) {
		uint32_t i;
		for (i = 0; i < list->drl_num_fences; i++)
			dma_fence_put(list->drl_entries[i].drfe_fence);

		kmem_free(list, sizeof (dma_resv_list_t) +
		    list->drl_max_fences * sizeof (dma_resv_fence_entry_t));
	}

	ww_mutex_destroy(&resv->dr_lock);
}

/*
 * Pre-allocate space for num_fences additional fences.
 *
 * Must be called with dma_resv_lock held. Allocates with KM_SLEEP
 * so this is the only point in the add path that can block on memory.
 *
 * During reallocation, signaled fences are garbage-collected.
 *
 * Returns 0 on success.
 */
int
dma_resv_reserve_fences(dma_resv_t *resv, uint32_t num_fences)
{
	dma_resv_list_t *old_list, *new_list;
	uint32_t old_count, new_max, i, j;
	size_t alloc_sz;

	ASSERT(ww_mutex_is_locked(&resv->dr_lock));

	old_list = resv->dr_fences;
	old_count = (old_list != NULL) ? old_list->drl_num_fences : 0;

	/* Check if current list has enough space */
	if (old_list != NULL &&
	    (old_count + num_fences <= old_list->drl_max_fences))
		return (0);

	/*
	 * Allocate a new list. Size it to hold existing non-signaled
	 * fences plus the requested additional slots. Round up for
	 * growth headroom.
	 */
	new_max = old_count + num_fences;
	if (new_max < 8)
		new_max = 8;
	else
		new_max = P2ROUNDUP(new_max, 8);

	alloc_sz = sizeof (dma_resv_list_t) +
	    new_max * sizeof (dma_resv_fence_entry_t);
	new_list = kmem_zalloc(alloc_sz, KM_SLEEP);
	new_list->drl_max_fences = new_max;
	new_list->drl_num_fences = 0;

	/* Copy non-signaled fences to new list, GC signaled ones */
	if (old_list != NULL) {
		j = 0;
		for (i = 0; i < old_list->drl_num_fences; i++) {
			dma_fence_t *f = old_list->drl_entries[i].drfe_fence;
			if (dma_fence_is_signaled(f)) {
				dma_fence_put(f);
			} else {
				new_list->drl_entries[j].drfe_fence = f;
				new_list->drl_entries[j].drfe_usage =
				    old_list->drl_entries[i].drfe_usage;
				j++;
			}
		}
		new_list->drl_num_fences = j;

		kmem_free(old_list, sizeof (dma_resv_list_t) +
		    old_list->drl_max_fences *
		    sizeof (dma_resv_fence_entry_t));
	}

	resv->dr_fences = new_list;
	return (0);
}

/*
 * Add a fence to the reservation object.
 *
 * Must be called after dma_resv_reserve_fences() to guarantee space.
 * Takes a reference on the fence.
 *
 * If a fence from the same context with same-or-higher usage and a
 * later-or-equal seqno already exists, it replaces the old one.
 *
 * CANNOT FAIL.
 */
void
dma_resv_add_fence(dma_resv_t *resv, dma_fence_t *fence,
    dma_resv_usage_t usage)
{
	dma_resv_list_t *list;
	uint32_t i;
	dma_fence_t *old;

	ASSERT(ww_mutex_is_locked(&resv->dr_lock));
	ASSERT(fence != NULL);

	list = resv->dr_fences;
	ASSERT(list != NULL);

	/*
	 * Look for an existing fence from the same context that we
	 * can replace (same or higher usage, and the new fence is
	 * later or equal in seqno, or the old fence is signaled).
	 */
	for (i = 0; i < list->drl_num_fences; i++) {
		old = list->drl_entries[i].drfe_fence;

		if (old->fence_context != fence->fence_context)
			continue;

		if (list->drl_entries[i].drfe_usage <= usage &&
		    (dma_fence_is_signaled(old) ||
		    dma_fence_is_later(fence, old) ||
		    fence->fence_seqno == old->fence_seqno)) {
			/* Replace in-place */
			dma_fence_get(fence);
			list->drl_entries[i].drfe_fence = fence;
			list->drl_entries[i].drfe_usage = usage;
			dma_fence_put(old);
			return;
		}
	}

	/* Append */
	ASSERT3U(list->drl_num_fences, <, list->drl_max_fences);
	dma_fence_get(fence);
	list->drl_entries[list->drl_num_fences].drfe_fence = fence;
	list->drl_entries[list->drl_num_fences].drfe_usage = usage;

	/*
	 * membar_producer ensures the fence pointer is visible before
	 * the count increment, for any lockless readers (future use).
	 */
	membar_producer();
	list->drl_num_fences++;
}

/*
 * Wait for all fences at the given usage level (and higher priority).
 *
 * Takes the lock briefly to snapshot the fence list, then waits
 * outside the lock. Returns B_TRUE if all fences signaled, B_FALSE
 * on timeout or interrupt.
 */
boolean_t
dma_resv_wait_timeout(dma_resv_t *resv, dma_resv_usage_t usage,
    boolean_t intr, hrtime_t timeout_ns)
{
	dma_fence_t **snap;
	uint32_t snap_count, i;
	boolean_t result = B_TRUE;
	hrtime_t remaining;

	/* Snapshot under lock */
	(void) dma_resv_lock(resv, NULL);

	if (resv->dr_fences == NULL || resv->dr_fences->drl_num_fences == 0) {
		dma_resv_unlock(resv);
		return (B_TRUE);
	}

	/* Count matching fences */
	snap_count = 0;
	for (i = 0; i < resv->dr_fences->drl_num_fences; i++) {
		if (resv->dr_fences->drl_entries[i].drfe_usage <= usage)
			snap_count++;
	}

	if (snap_count == 0) {
		dma_resv_unlock(resv);
		return (B_TRUE);
	}

	/* Snapshot with references */
	snap = kmem_zalloc(snap_count * sizeof (dma_fence_t *), KM_SLEEP);
	snap_count = 0;
	for (i = 0; i < resv->dr_fences->drl_num_fences; i++) {
		if (resv->dr_fences->drl_entries[i].drfe_usage <= usage) {
			snap[snap_count] =
			    resv->dr_fences->drl_entries[i].drfe_fence;
			dma_fence_get(snap[snap_count]);
			snap_count++;
		}
	}

	dma_resv_unlock(resv);

	/* Wait outside lock */
	remaining = timeout_ns;
	for (i = 0; i < snap_count; i++) {
		clock_t ret;

		if (dma_fence_is_signaled(snap[i]))
			continue;

		ret = dma_fence_wait_timeout(snap[i], intr, remaining);
		if (ret < 0 || ret == 0) {
			result = B_FALSE;
			break;
		}

		/* Update remaining time for next fence */
		if (timeout_ns > 0 && ret > 0)
			remaining = ret;
	}

	/* Drop references */
	for (i = 0; i < snap_count; i++)
		dma_fence_put(snap[i]);
	kmem_free(snap, snap_count * sizeof (dma_fence_t *));

	return (result);
}

/*
 * Test if all fences at the given usage level are signaled.
 */
boolean_t
dma_resv_test_signaled(dma_resv_t *resv, dma_resv_usage_t usage)
{
	boolean_t result = B_TRUE;
	uint32_t i;

	(void) dma_resv_lock(resv, NULL);

	if (resv->dr_fences != NULL) {
		for (i = 0; i < resv->dr_fences->drl_num_fences; i++) {
			if (resv->dr_fences->drl_entries[i].drfe_usage <=
			    usage &&
			    !dma_fence_is_signaled(
			    resv->dr_fences->drl_entries[i].drfe_fence)) {
				result = B_FALSE;
				break;
			}
		}
	}

	dma_resv_unlock(resv);
	return (result);
}

/*
 * Locked iterator. Caller must hold dma_resv_lock.
 */
void
dma_resv_iter_begin(dma_resv_iter_t *iter, dma_resv_t *resv,
    dma_resv_usage_t usage)
{
	ASSERT(ww_mutex_is_locked(&resv->dr_lock));

	iter->dri_resv = resv;
	iter->dri_usage = usage;
	iter->dri_index = 0;
	iter->dri_fence = NULL;
	iter->dri_fence_usage = DMA_RESV_USAGE_BOOKKEEP;
}

dma_fence_t *
dma_resv_iter_next(dma_resv_iter_t *iter)
{
	dma_resv_list_t *list = iter->dri_resv->dr_fences;

	if (list == NULL)
		return (NULL);

	while (iter->dri_index < list->drl_num_fences) {
		uint32_t idx = iter->dri_index++;
		dma_resv_fence_entry_t *entry = &list->drl_entries[idx];

		if (entry->drfe_usage <= iter->dri_usage) {
			iter->dri_fence = entry->drfe_fence;
			iter->dri_fence_usage = entry->drfe_usage;
			return (iter->dri_fence);
		}
	}

	iter->dri_fence = NULL;
	return (NULL);
}

void
dma_resv_iter_end(dma_resv_iter_t *iter)
{
	iter->dri_fence = NULL;
}
