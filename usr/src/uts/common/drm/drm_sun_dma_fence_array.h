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

#ifndef	_DRM_SUN_DMA_FENCE_ARRAY_H
#define	_DRM_SUN_DMA_FENCE_ARRAY_H

/*
 * dma_fence_array -- aggregates N fences into one.
 *
 * By default signals when ALL child fences signal. With signal_on_any
 * set, signals when ANY child fence signals.
 *
 * Uses a pending counter decremented by callbacks on each child.
 * When the counter reaches zero, the parent is signaled via a deferred
 * taskq dispatch (to avoid lock ordering issues).
 */

#include <sys/types.h>
#include <sys/taskq.h>
#include "drm_sun_dma_fence.h"

#ifdef __cplusplus
extern "C" {
#endif

struct dma_fence_array;

typedef struct dma_fence_array_cb {
	dma_fence_cb_t		fac_cb;
	struct dma_fence_array	*fac_array;
} dma_fence_array_cb_t;

typedef struct dma_fence_array {
	dma_fence_t		dfa_base;
	kmutex_t		dfa_lock;
	uint32_t		dfa_num_fences;
	volatile uint32_t	dfa_num_pending;
	dma_fence_t		**dfa_fences;
	boolean_t		dfa_signal_on_any;
	taskq_ent_t		dfa_tqent; /* preallocated for deferred signal */
	dma_fence_array_cb_t	dfa_callbacks[]; /* flex array [num_fences] */
} dma_fence_array_t;

extern const dma_fence_ops_t dma_fence_array_ops;

extern dma_fence_array_t *dma_fence_array_create(uint32_t num_fences,
    dma_fence_t **fences, uint64_t context, uint64_t seqno,
    boolean_t signal_on_any);

static inline dma_fence_array_t *
to_dma_fence_array(dma_fence_t *fence)
{
	if (fence == NULL || fence->fence_ops != &dma_fence_array_ops)
		return (NULL);
	return ((dma_fence_array_t *)((char *)fence -
	    offsetof(dma_fence_array_t, dfa_base)));
}

#ifdef __cplusplus
}
#endif

#endif	/* _DRM_SUN_DMA_FENCE_ARRAY_H */
