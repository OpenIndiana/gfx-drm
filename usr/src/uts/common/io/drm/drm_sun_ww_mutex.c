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
 * Wait-Die deadlock avoidance mutex implementation for illumos.
 *
 * This implements the Wait-Die algorithm used by Linux's ww_mutex,
 * which is required by dma_resv for deadlock-free multi-buffer locking.
 *
 * The internal kmutex_t (wm_lock) protects the logical state of the
 * ww_mutex (wm_locked, wm_ctx). It is held only briefly during the
 * lock/unlock protocol. The condvar (wm_cv) serializes waiters.
 *
 * This mutex is NEVER used from interrupt context.
 */

#include <sys/types.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/atomic.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include "drm_sun_ww_mutex.h"

void
ww_mutex_init(ww_mutex_t *wm, ww_class_t *cls)
{
	mutex_init(&wm->wm_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&wm->wm_cv, NULL, CV_DEFAULT, NULL);
	wm->wm_class = cls;
	wm->wm_ctx = NULL;
	wm->wm_locked = B_FALSE;
}

void
ww_mutex_destroy(ww_mutex_t *wm)
{
	ASSERT(!wm->wm_locked);
	cv_destroy(&wm->wm_cv);
	mutex_destroy(&wm->wm_lock);
}

/*
 * Lock a ww_mutex within a transaction context.
 *
 * Returns:
 *   0         -- locked successfully
 *   -EALREADY -- already held by this context
 *   -EDEADLK  -- must back off (we're younger than the holder)
 *
 * ctx may be NULL for a context-less lock (no deadlock avoidance,
 * blocks unconditionally).
 */
int
ww_mutex_lock(ww_mutex_t *wm, ww_acquire_ctx_t *ctx)
{
	mutex_enter(&wm->wm_lock);

	/* Fast path: unlocked */
	if (!wm->wm_locked) {
		wm->wm_locked = B_TRUE;
		wm->wm_ctx = ctx;
		if (ctx != NULL)
			ctx->wac_acquired++;
		mutex_exit(&wm->wm_lock);
		return (0);
	}

	/* Already held by this context? */
	if (ctx != NULL && wm->wm_ctx == ctx) {
		mutex_exit(&wm->wm_lock);
		return (-EALREADY);
	}

	/*
	 * Wait-Die: if we're younger (higher stamp) than the holder,
	 * die immediately. The holder has lower stamp = higher priority.
	 */
	if (ctx != NULL && wm->wm_ctx != NULL &&
	    ctx->wac_stamp > wm->wm_ctx->wac_stamp) {
		mutex_exit(&wm->wm_lock);
		return (-EDEADLK);
	}

	/* We're older or no context -- wait */
	while (wm->wm_locked)
		cv_wait(&wm->wm_cv, &wm->wm_lock);

	wm->wm_locked = B_TRUE;
	wm->wm_ctx = ctx;
	if (ctx != NULL)
		ctx->wac_acquired++;
	mutex_exit(&wm->wm_lock);
	return (0);
}

/*
 * Interruptible variant. Also returns -EINTR if a signal is delivered.
 */
int
ww_mutex_lock_interruptible(ww_mutex_t *wm, ww_acquire_ctx_t *ctx)
{
	mutex_enter(&wm->wm_lock);

	if (!wm->wm_locked) {
		wm->wm_locked = B_TRUE;
		wm->wm_ctx = ctx;
		if (ctx != NULL)
			ctx->wac_acquired++;
		mutex_exit(&wm->wm_lock);
		return (0);
	}

	if (ctx != NULL && wm->wm_ctx == ctx) {
		mutex_exit(&wm->wm_lock);
		return (-EALREADY);
	}

	if (ctx != NULL && wm->wm_ctx != NULL &&
	    ctx->wac_stamp > wm->wm_ctx->wac_stamp) {
		mutex_exit(&wm->wm_lock);
		return (-EDEADLK);
	}

	while (wm->wm_locked) {
		if (cv_wait_sig(&wm->wm_cv, &wm->wm_lock) == 0) {
			mutex_exit(&wm->wm_lock);
			return (-EINTR);
		}
	}

	wm->wm_locked = B_TRUE;
	wm->wm_ctx = ctx;
	if (ctx != NULL)
		ctx->wac_acquired++;
	mutex_exit(&wm->wm_lock);
	return (0);
}

/*
 * Slow path: called after receiving -EDEADLK and releasing all locks.
 * Blocks until the mutex is available. Never returns -EDEADLK because
 * the caller holds no other locks (deadlock is impossible).
 */
int
ww_mutex_lock_slow(ww_mutex_t *wm, ww_acquire_ctx_t *ctx)
{
	ASSERT(ctx != NULL);
	ASSERT3U(ctx->wac_acquired, ==, 0);

	mutex_enter(&wm->wm_lock);

	while (wm->wm_locked)
		cv_wait(&wm->wm_cv, &wm->wm_lock);

	wm->wm_locked = B_TRUE;
	wm->wm_ctx = ctx;
	ctx->wac_acquired++;
	mutex_exit(&wm->wm_lock);
	return (0);
}

int
ww_mutex_lock_slow_interruptible(ww_mutex_t *wm, ww_acquire_ctx_t *ctx)
{
	ASSERT(ctx != NULL);
	ASSERT3U(ctx->wac_acquired, ==, 0);

	mutex_enter(&wm->wm_lock);

	while (wm->wm_locked) {
		if (cv_wait_sig(&wm->wm_cv, &wm->wm_lock) == 0) {
			mutex_exit(&wm->wm_lock);
			return (-EINTR);
		}
	}

	wm->wm_locked = B_TRUE;
	wm->wm_ctx = ctx;
	ctx->wac_acquired++;
	mutex_exit(&wm->wm_lock);
	return (0);
}

/*
 * Trylock. No context -- cannot participate in Wait-Die ordering.
 * Returns B_TRUE on success, B_FALSE if already locked.
 */
boolean_t
ww_mutex_trylock(ww_mutex_t *wm)
{
	boolean_t acquired = B_FALSE;

	mutex_enter(&wm->wm_lock);
	if (!wm->wm_locked) {
		wm->wm_locked = B_TRUE;
		wm->wm_ctx = NULL;
		acquired = B_TRUE;
	}
	mutex_exit(&wm->wm_lock);
	return (acquired);
}

/*
 * Unlock a ww_mutex. Wakes one waiter (if any).
 */
void
ww_mutex_unlock(ww_mutex_t *wm)
{
	mutex_enter(&wm->wm_lock);
	ASSERT(wm->wm_locked);

	if (wm->wm_ctx != NULL) {
		ASSERT3U(wm->wm_ctx->wac_acquired, >, 0);
		wm->wm_ctx->wac_acquired--;
	}

	wm->wm_ctx = NULL;
	wm->wm_locked = B_FALSE;
	cv_broadcast(&wm->wm_cv);
	mutex_exit(&wm->wm_lock);
}
