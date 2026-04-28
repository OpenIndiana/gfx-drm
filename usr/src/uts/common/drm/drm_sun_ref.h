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

#ifndef	_DRM_SUN_REF_H
#define	_DRM_SUN_REF_H

/*
 * Atomic reference counting for DRM objects.
 *
 * Replaces Linux kref. All operations are lock-free and safe from
 * any context, including interrupt handlers.
 */

#include <sys/types.h>
#include <sys/atomic.h>
#include <sys/debug.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct drm_ref {
	volatile uint32_t	ref_count;
} drm_ref_t;

static inline void
drm_ref_init(drm_ref_t *ref)
{
	ref->ref_count = 1;
}

static inline void
drm_ref_get(drm_ref_t *ref)
{
	atomic_inc_32(&ref->ref_count);
}

/*
 * Decrement refcount. Returns B_TRUE if this was the last reference
 * (count dropped to 0), meaning the caller should free the object.
 */
static inline boolean_t
drm_ref_put(drm_ref_t *ref)
{
	uint32_t old = atomic_dec_32_nv(&ref->ref_count);
	ASSERT3S((int32_t)old, >=, 0);
	return (old == 0);
}

/*
 * Try to acquire a reference. Returns B_FALSE if the refcount was
 * already 0 (object is being destroyed). Uses a CAS loop to avoid
 * racing with the final drm_ref_put.
 *
 * This is the equivalent of Linux kref_get_unless_zero().
 */
static inline boolean_t
drm_ref_get_unless_zero(drm_ref_t *ref)
{
	uint32_t old;

	do {
		old = ref->ref_count;
		if (old == 0)
			return (B_FALSE);
	} while (atomic_cas_32(&ref->ref_count, old, old + 1) != old);

	return (B_TRUE);
}

static inline uint32_t
drm_ref_read(drm_ref_t *ref)
{
	return (ref->ref_count);
}

#ifdef __cplusplus
}
#endif

#endif	/* _DRM_SUN_REF_H */
