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

#ifndef	_DRM_SUN_WW_MUTEX_H
#define	_DRM_SUN_WW_MUTEX_H

/*
 * Wait-Die deadlock avoidance mutex for illumos.
 *
 * Used by dma_resv to lock multiple buffer objects in arbitrary order
 * without deadlock. Implements the Wait-Die algorithm:
 *
 *   - Each transaction gets a monotonically increasing stamp.
 *     Lower stamp = older = higher priority.
 *   - When A tries to lock a mutex held by B:
 *       If A is older (A.stamp < B.stamp): A waits.
 *       If A is younger (A.stamp > B.stamp): A dies (-EDEADLK).
 *   - After -EDEADLK, A releases all locks and retries via the
 *     slow path (which always blocks, never returns -EDEADLK).
 */

#include <sys/types.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/atomic.h>
#include <sys/debug.h>
#include <sys/errno.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ww_class {
	volatile uint64_t	wc_stamp;
	const char		*wc_name;
} ww_class_t;

#define	DEFINE_WW_CLASS(_name)						\
	ww_class_t _name = { .wc_stamp = 0, .wc_name = #_name }

typedef struct ww_acquire_ctx {
	uint64_t		wac_stamp;
	uint32_t		wac_acquired;
	const ww_class_t	*wac_class;
} ww_acquire_ctx_t;

typedef struct ww_mutex {
	kmutex_t		wm_lock;	/* protects wm_locked, wm_ctx */
	kcondvar_t		wm_cv;
	const ww_class_t	*wm_class;
	ww_acquire_ctx_t	*wm_ctx;	/* current owner's ctx or NULL */
	boolean_t		wm_locked;
} ww_mutex_t;

static inline void
ww_acquire_init(ww_acquire_ctx_t *ctx, ww_class_t *cls)
{
	ctx->wac_stamp = atomic_inc_64_nv(&cls->wc_stamp);
	ctx->wac_acquired = 0;
	ctx->wac_class = cls;
}

static inline void
ww_acquire_fini(ww_acquire_ctx_t *ctx)
{
	ASSERT3U(ctx->wac_acquired, ==, 0);
}

static inline boolean_t
ww_mutex_is_locked(ww_mutex_t *wm)
{
	return (wm->wm_locked);
}

/*
 * Functions implemented in drm_sun_ww_mutex.c
 */
extern void ww_mutex_init(ww_mutex_t *wm, ww_class_t *cls);
extern void ww_mutex_destroy(ww_mutex_t *wm);
extern int ww_mutex_lock(ww_mutex_t *wm, ww_acquire_ctx_t *ctx);
extern int ww_mutex_lock_interruptible(ww_mutex_t *wm,
    ww_acquire_ctx_t *ctx);
extern int ww_mutex_lock_slow(ww_mutex_t *wm, ww_acquire_ctx_t *ctx);
extern int ww_mutex_lock_slow_interruptible(ww_mutex_t *wm,
    ww_acquire_ctx_t *ctx);
extern boolean_t ww_mutex_trylock(ww_mutex_t *wm);
extern void ww_mutex_unlock(ww_mutex_t *wm);

#ifdef __cplusplus
}
#endif

#endif	/* _DRM_SUN_WW_MUTEX_H */
