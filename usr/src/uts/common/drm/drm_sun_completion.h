/*
 * Copyright (c) 2026 Toasty.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef	_DRM_SUN_COMPLETION_H
#define	_DRM_SUN_COMPLETION_H

/*
 * Linux struct completion replacement.
 *
 * A one-shot (or re-initializable) synchronization primitive.
 * Uses illumos-native kmutex_t + kcondvar_t.
 */

#include <sys/types.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct drm_completion {
	kmutex_t	dc_lock;
	kcondvar_t	dc_cv;
	boolean_t	dc_done;
} drm_completion_t;

/*
 * Linux name aliases.
 */
#define	completion		drm_completion

static inline void
init_completion(drm_completion_t *dc)
{
	mutex_init(&dc->dc_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&dc->dc_cv, NULL, CV_DRIVER, NULL);
	dc->dc_done = B_FALSE;
}

static inline void
destroy_completion(drm_completion_t *dc)
{
	cv_destroy(&dc->dc_cv);
	mutex_destroy(&dc->dc_lock);
}

static inline void
reinit_completion(drm_completion_t *dc)
{
	mutex_enter(&dc->dc_lock);
	dc->dc_done = B_FALSE;
	mutex_exit(&dc->dc_lock);
}

static inline void
complete(drm_completion_t *dc)
{
	mutex_enter(&dc->dc_lock);
	dc->dc_done = B_TRUE;
	cv_signal(&dc->dc_cv);
	mutex_exit(&dc->dc_lock);
}

static inline void
complete_all(drm_completion_t *dc)
{
	mutex_enter(&dc->dc_lock);
	dc->dc_done = B_TRUE;
	cv_broadcast(&dc->dc_cv);
	mutex_exit(&dc->dc_lock);
}

/*
 * Wait for completion (non-interruptible, indefinite).
 */
static inline void
wait_for_completion(drm_completion_t *dc)
{
	mutex_enter(&dc->dc_lock);
	while (!dc->dc_done)
		cv_wait(&dc->dc_cv, &dc->dc_lock);
	mutex_exit(&dc->dc_lock);
}

/*
 * Wait for completion with timeout.
 * Returns remaining time in jiffies (>0 on success), 0 on timeout.
 * Mirrors Linux semantics: timeout is in jiffies (clock_t on illumos).
 */
static inline long
wait_for_completion_timeout(drm_completion_t *dc, long timeout_jiffies)
{
	clock_t		deadline;
	clock_t		remaining;
	int		ret;

	mutex_enter(&dc->dc_lock);
	if (dc->dc_done) {
		mutex_exit(&dc->dc_lock);
		return (timeout_jiffies > 0 ? timeout_jiffies : 1);
	}
	deadline = ddi_get_lbolt() + (clock_t)timeout_jiffies;
	while (!dc->dc_done) {
		ret = cv_timedwait(&dc->dc_cv, &dc->dc_lock, deadline);
		if (ret == -1) {
			/* Timeout */
			mutex_exit(&dc->dc_lock);
			return (dc->dc_done ? 1 : 0);
		}
	}
	remaining = deadline - ddi_get_lbolt();
	mutex_exit(&dc->dc_lock);
	return (remaining > 0 ? remaining : 1);
}

/*
 * Wait for completion (interruptible, indefinite).
 * Returns 0 on success, -ERESTARTSYS on signal.
 */
static inline int
wait_for_completion_interruptible(drm_completion_t *dc)
{
	int	ret;

	mutex_enter(&dc->dc_lock);
	while (!dc->dc_done) {
		ret = cv_wait_sig(&dc->dc_cv, &dc->dc_lock);
		if (ret == 0) {
			/* Interrupted by signal */
			mutex_exit(&dc->dc_lock);
			return (-4); /* -EINTR */
		}
	}
	mutex_exit(&dc->dc_lock);
	return (0);
}

/*
 * Wait for completion (interruptible, with timeout).
 * Returns remaining jiffies (>0 on success), 0 on timeout,
 * -ERESTARTSYS on signal.
 */
static inline long
wait_for_completion_interruptible_timeout(drm_completion_t *dc,
    long timeout_jiffies)
{
	clock_t		deadline;
	clock_t		remaining;
	int		ret;

	mutex_enter(&dc->dc_lock);
	if (dc->dc_done) {
		mutex_exit(&dc->dc_lock);
		return (timeout_jiffies > 0 ? timeout_jiffies : 1);
	}
	deadline = ddi_get_lbolt() + (clock_t)timeout_jiffies;
	while (!dc->dc_done) {
		ret = cv_timedwait_sig(&dc->dc_cv, &dc->dc_lock, deadline);
		if (ret == -1) {
			/* Timeout */
			mutex_exit(&dc->dc_lock);
			return (dc->dc_done ? 1 : 0);
		}
		if (ret == 0) {
			/* Signal */
			mutex_exit(&dc->dc_lock);
			return (-4); /* -EINTR */
		}
	}
	remaining = deadline - ddi_get_lbolt();
	mutex_exit(&dc->dc_lock);
	return (remaining > 0 ? remaining : 1);
}

static inline boolean_t
completion_done(drm_completion_t *dc)
{
	boolean_t done;

	mutex_enter(&dc->dc_lock);
	done = dc->dc_done;
	mutex_exit(&dc->dc_lock);
	return (done);
}

/*
 * wait_event / wake_up family.
 *
 * These extend the existing DRM_WAIT_ON / DRM_WAKEUP macros in drmP.h.
 * Modern virtio-gpu code uses the Linux-style wait_event() / wake_up()
 * macros rather than the DRM_WAIT_ON style.
 *
 * wait_queue_head_t is already defined in drmP.h as:
 *   struct drm_wait_queue { kcondvar_t cv; kmutex_t lock; };
 */

#define	init_waitqueue_head(wqh)	\
	do {				\
		mutex_init(&(wqh)->lock, NULL, MUTEX_DRIVER, NULL);	\
		cv_init(&(wqh)->cv, NULL, CV_DRIVER, NULL);		\
	} while (0)

#define	destroy_waitqueue_head(wqh)	\
	do {				\
		cv_destroy(&(wqh)->cv);		\
		mutex_destroy(&(wqh)->lock);	\
	} while (0)

/*
 * wake_up -- wake all waiters on a wait_queue_head_t.
 * (wake_up_all is already #defined to DRM_WAKEUP in drmP.h)
 */
#ifndef wake_up
#define	wake_up(wqh)			\
	do {				\
		mutex_enter(&(wqh)->lock);	\
		cv_broadcast(&(wqh)->cv);	\
		mutex_exit(&(wqh)->lock);	\
	} while (0)
#endif

/*
 * wait_event -- block until condition is true (non-interruptible).
 */
#define	wait_event(wqh, condition)	\
	do {				\
		mutex_enter(&(wqh).lock);	\
		while (!(condition))		\
			cv_wait(&(wqh).cv, &(wqh).lock);	\
		mutex_exit(&(wqh).lock);	\
	} while (0)

/*
 * wait_event_timeout -- block until condition or timeout.
 * Returns 0 on timeout, remaining jiffies on success (at least 1).
 */
#define	wait_event_timeout(wqh, condition, timeout)	({		\
	long __ret = (timeout);						\
	if (!(condition)) {						\
		clock_t __deadline = ddi_get_lbolt() + (clock_t)(timeout); \
		mutex_enter(&(wqh).lock);				\
		while (!(condition)) {					\
			if (cv_timedwait(&(wqh).cv,			\
			    &(wqh).lock, __deadline) == -1) {		\
				__ret = (condition) ? 1 : 0;		\
				break;					\
			}						\
			__ret = __deadline - ddi_get_lbolt();		\
			if (__ret <= 0) {				\
				__ret = (condition) ? 1 : 0;		\
				break;					\
			}						\
		}							\
		if (__ret > 0 && (condition))				\
			__ret = __deadline - ddi_get_lbolt();		\
		if (__ret <= 0 && (condition))				\
			__ret = 1;					\
		mutex_exit(&(wqh).lock);				\
	}								\
	__ret;								\
})

/*
 * wait_event_interruptible -- block until condition or signal.
 * Returns 0 on success, -ERESTARTSYS on signal.
 */
#define	wait_event_interruptible(wqh, condition)	({		\
	int __ret = 0;							\
	if (!(condition)) {						\
		mutex_enter(&(wqh).lock);				\
		while (!(condition)) {					\
			if (cv_wait_sig(&(wqh).cv,			\
			    &(wqh).lock) == 0) {			\
				__ret = -4; /* -EINTR */		\
				break;					\
			}						\
		}							\
		mutex_exit(&(wqh).lock);				\
	}								\
	__ret;								\
})

#ifdef __cplusplus
}
#endif

#endif /* _DRM_SUN_COMPLETION_H */
