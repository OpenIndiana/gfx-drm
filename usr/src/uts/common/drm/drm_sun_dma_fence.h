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

#ifndef	_DRM_SUN_DMA_FENCE_H
#define	_DRM_SUN_DMA_FENCE_H

/*
 * dma_fence -- GPU synchronization primitive for illumos.
 *
 * A one-shot signalable future. A producer creates a fence, hands it to
 * consumers, and later signals it (typically from a GPU interrupt handler).
 * Consumers can register callbacks, block-wait, or poll.
 *
 * Once signaled, a fence stays signaled forever.
 *
 * Locking model:
 *   - fence_lock: pointer to an EXTERNAL kmutex_t shared across fences
 *     in the same GPU context. Protects the callback list. Adaptive
 *     mutex, safe from device interrupt handlers (PIL <= LOCK_LEVEL).
 *   - fence_wait_lock: per-fence lock associated with the condvar.
 *     Needed because cv_wait requires a stable lock, but fence_lock
 *     is a shared pointer.
 *
 * Callback contract:
 *   Callbacks execute under fence_lock. They MUST NOT sleep, MUST NOT
 *   allocate with KM_SLEEP, and MUST complete quickly. If real work is
 *   needed, dispatch to a taskq from the callback.
 */

#include <sys/types.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/atomic.h>
#include <sys/list.h>
#include <sys/time.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include "drm_sun_ref.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct dma_fence;
struct dma_fence_cb;

typedef void (*dma_fence_func_t)(struct dma_fence *, struct dma_fence_cb *);

typedef struct dma_fence_cb {
	list_node_t		fcb_node;
	dma_fence_func_t	fcb_func;
} dma_fence_cb_t;

typedef struct dma_fence_ops {
	const char *(*get_driver_name)(struct dma_fence *);
	const char *(*get_timeline_name)(struct dma_fence *);

	/*
	 * Called under fence_lock when first waiter/callback registers.
	 * Driver should arrange for dma_fence_signal() to be called when
	 * HW completes (e.g., enable interrupt). Returns B_TRUE on success.
	 */
	boolean_t (*enable_signaling)(struct dma_fence *);

	/*
	 * Fast poll, called WITHOUT fence_lock. Returns B_TRUE if done.
	 */
	boolean_t (*signaled)(struct dma_fence *);

	/*
	 * Custom release. Called when refcount drops to 0, from any context.
	 * If NULL, dma_fence_free() is used.
	 */
	void (*release)(struct dma_fence *);

	/*
	 * Deadline hint. Called without fence_lock, possibly concurrently.
	 */
	void (*set_deadline)(struct dma_fence *, hrtime_t);
} dma_fence_ops_t;

/* Fence flag bits (manipulated atomically) */
#define	DMA_FENCE_FLAG_SIGNALED		(1U << 0)
#define	DMA_FENCE_FLAG_TIMESTAMP	(1U << 1)
#define	DMA_FENCE_FLAG_ENABLE_SIGNAL	(1U << 2)
#define	DMA_FENCE_FLAG_USER_BITS	3

typedef struct dma_fence {
	kmutex_t		*fence_lock;	/* shared context lock (ptr) */
	const dma_fence_ops_t	*fence_ops;
	drm_ref_t		fence_ref;
	volatile uint32_t	fence_flags;
	int			fence_error;
	uint64_t		fence_context;
	uint64_t		fence_seqno;
	hrtime_t		fence_timestamp;
	list_t			fence_cb_list;
	kmutex_t		fence_wait_lock;
	kcondvar_t		fence_wait_cv;
} dma_fence_t;

/*
 * Functions implemented in drm_sun_dma_fence.c
 */
extern uint64_t dma_fence_context_alloc(unsigned int num);
extern void dma_fence_init(dma_fence_t *fence, const dma_fence_ops_t *ops,
    kmutex_t *lock, uint64_t context, uint64_t seqno);
extern int dma_fence_signal(dma_fence_t *fence);
extern int dma_fence_signal_locked(dma_fence_t *fence);
extern int dma_fence_add_callback(dma_fence_t *fence, dma_fence_cb_t *cb,
    dma_fence_func_t func);
extern boolean_t dma_fence_remove_callback(dma_fence_t *fence,
    dma_fence_cb_t *cb);
extern clock_t dma_fence_wait_timeout(dma_fence_t *fence, boolean_t intr,
    hrtime_t timeout_ns);
extern void dma_fence_enable_sw_signaling(dma_fence_t *fence);
extern void dma_fence_release(dma_fence_t *fence);
extern void dma_fence_free(dma_fence_t *fence);

/*
 * Inline refcounting
 */
static inline void
dma_fence_get(dma_fence_t *fence)
{
	if (fence != NULL)
		drm_ref_get(&fence->fence_ref);
}

static inline dma_fence_t *
dma_fence_get_ref(dma_fence_t *fence)
{
	if (fence != NULL)
		drm_ref_get(&fence->fence_ref);
	return (fence);
}

static inline void
dma_fence_put(dma_fence_t *fence)
{
	if (fence != NULL && drm_ref_put(&fence->fence_ref))
		dma_fence_release(fence);
}

static inline dma_fence_t *
dma_fence_get_unless_zero(dma_fence_t *fence)
{
	if (fence != NULL && drm_ref_get_unless_zero(&fence->fence_ref))
		return (fence);
	return (NULL);
}

/*
 * Test if signaled. May call ops->signaled() as a fast poll.
 * If the fast poll reports signaled, triggers the full signal path.
 */
static inline boolean_t
dma_fence_is_signaled(dma_fence_t *fence)
{
	if (fence->fence_flags & DMA_FENCE_FLAG_SIGNALED)
		return (B_TRUE);
	if (fence->fence_ops->signaled != NULL &&
	    fence->fence_ops->signaled(fence)) {
		(void) dma_fence_signal(fence);
		return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * Convenience: wait forever (or until interrupted).
 * Returns 0 on success, negative errno on error.
 */
static inline int
dma_fence_wait(dma_fence_t *fence, boolean_t intr)
{
	clock_t ret = dma_fence_wait_timeout(fence, intr, 0);
	return (ret < 0 ? (int)ret : 0);
}

/*
 * Is f1 later than f2 within the same context?
 * Uses signed comparison for wrapping seqno.
 */
static inline boolean_t
dma_fence_is_later(dma_fence_t *f1, dma_fence_t *f2)
{
	ASSERT3U(f1->fence_context, ==, f2->fence_context);
	return ((int64_t)(f1->fence_seqno - f2->fence_seqno) > 0);
}

/*
 * Set an error on the fence. MUST be called BEFORE signaling.
 */
static inline void
dma_fence_set_error(dma_fence_t *fence, int error)
{
	ASSERT(!(fence->fence_flags & DMA_FENCE_FLAG_SIGNALED));
	ASSERT(error <= 0);
	fence->fence_error = error;
}

#ifdef __cplusplus
}
#endif

#endif	/* _DRM_SUN_DMA_FENCE_H */
