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

#ifndef	_DRM_SUN_DMA_RESV_H
#define	_DRM_SUN_DMA_RESV_H

/*
 * dma_resv -- reservation object for buffer synchronization.
 *
 * Associates a set of dma_fences with a buffer object. Protected by
 * a ww_mutex for deadlock-safe multi-buffer locking.
 *
 * Usage levels (lower numeric = higher priority):
 *   KERNEL(0) < WRITE(1) < READ(2) < BOOKKEEP(3)
 * Querying at level N returns all fences at levels <= N.
 *
 * Simplification vs Linux: no RCU, no unlocked iteration.
 * All fence list access requires holding the ww_mutex lock.
 */

#include <sys/types.h>
#include "drm_sun_ww_mutex.h"
#include "drm_sun_dma_fence.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dma_resv_usage {
	DMA_RESV_USAGE_KERNEL	= 0,
	DMA_RESV_USAGE_WRITE	= 1,
	DMA_RESV_USAGE_READ	= 2,
	DMA_RESV_USAGE_BOOKKEEP = 3
} dma_resv_usage_t;

typedef struct dma_resv_fence_entry {
	dma_fence_t		*drfe_fence;
	dma_resv_usage_t	drfe_usage;
} dma_resv_fence_entry_t;

typedef struct dma_resv_list {
	uint32_t		drl_num_fences;
	uint32_t		drl_max_fences;
	dma_resv_fence_entry_t	drl_entries[];	/* flex array */
} dma_resv_list_t;

extern ww_class_t reservation_ww_class;

typedef struct dma_resv {
	ww_mutex_t		dr_lock;
	dma_resv_list_t		*dr_fences;
} dma_resv_t;

/*
 * Iterator for locked traversal of a dma_resv's fences.
 * Caller must hold dma_resv_lock.
 */
typedef struct dma_resv_iter {
	dma_resv_t		*dri_resv;
	dma_resv_usage_t	dri_usage;
	uint32_t		dri_index;
	dma_fence_t		*dri_fence;
	dma_resv_usage_t	dri_fence_usage;
} dma_resv_iter_t;

/*
 * Functions implemented in drm_sun_dma_resv.c
 */
extern void dma_resv_init(dma_resv_t *resv);
extern void dma_resv_fini(dma_resv_t *resv);
extern int dma_resv_reserve_fences(dma_resv_t *resv, uint32_t num_fences);
extern void dma_resv_add_fence(dma_resv_t *resv, dma_fence_t *fence,
    dma_resv_usage_t usage);
extern boolean_t dma_resv_wait_timeout(dma_resv_t *resv,
    dma_resv_usage_t usage, boolean_t intr, hrtime_t timeout_ns);
extern boolean_t dma_resv_test_signaled(dma_resv_t *resv,
    dma_resv_usage_t usage);
extern void dma_resv_iter_begin(dma_resv_iter_t *iter, dma_resv_t *resv,
    dma_resv_usage_t usage);
extern dma_fence_t *dma_resv_iter_next(dma_resv_iter_t *iter);
extern void dma_resv_iter_end(dma_resv_iter_t *iter);

/*
 * Iterate over fences matching a usage level (locked).
 */
#define	dma_resv_for_each_fence(_iter, _resv, _usage, _fence)		\
	for (dma_resv_iter_begin((_iter), (_resv), (_usage));		\
	    ((_fence) = dma_resv_iter_next((_iter))) != NULL; )

/*
 * Locking -- thin wrappers around ww_mutex.
 */
static inline int
dma_resv_lock(dma_resv_t *resv, ww_acquire_ctx_t *ctx)
{
	return (ww_mutex_lock(&resv->dr_lock, ctx));
}

static inline int
dma_resv_lock_interruptible(dma_resv_t *resv, ww_acquire_ctx_t *ctx)
{
	return (ww_mutex_lock_interruptible(&resv->dr_lock, ctx));
}

static inline void
dma_resv_lock_slow(dma_resv_t *resv, ww_acquire_ctx_t *ctx)
{
	(void) ww_mutex_lock_slow(&resv->dr_lock, ctx);
}

static inline int
dma_resv_lock_slow_interruptible(dma_resv_t *resv, ww_acquire_ctx_t *ctx)
{
	return (ww_mutex_lock_slow_interruptible(&resv->dr_lock, ctx));
}

static inline boolean_t
dma_resv_trylock(dma_resv_t *resv)
{
	return (ww_mutex_trylock(&resv->dr_lock));
}

static inline void
dma_resv_unlock(dma_resv_t *resv)
{
	ww_mutex_unlock(&resv->dr_lock);
}

static inline boolean_t
dma_resv_is_locked(dma_resv_t *resv)
{
	return (ww_mutex_is_locked(&resv->dr_lock));
}

#ifdef __cplusplus
}
#endif

#endif	/* _DRM_SUN_DMA_RESV_H */
