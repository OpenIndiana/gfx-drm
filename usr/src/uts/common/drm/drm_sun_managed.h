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

#ifndef	_DRM_SUN_MANAGED_H
#define	_DRM_SUN_MANAGED_H

/*
 * DRM managed resources (drmm_*) -- initial stubs.
 *
 * In Linux, drmm_* allocations are tracked and automatically freed when
 * the DRM device is destroyed.  For initial bring-up, we implement these
 * as plain kmem allocations.  Proper lifecycle tracking can be added later.
 *
 * This is safe for the virtio-gpu driver because all drmm_* resources are
 * allocated during attach and freed during detach -- the device lifecycle
 * is simple (no hotplug).
 */

#include <sys/types.h>
#include <sys/kmem.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * drmm_kzalloc -- allocate zeroed memory tied to device lifetime.
 * The 'dev' argument is currently unused (no tracking).
 * The 'flags' argument maps GFP_KERNEL -> KM_SLEEP.
 */
#define	drmm_kzalloc(dev, size, flags)	\
	kmem_zalloc((size), KM_SLEEP)

#define	drmm_kcalloc(dev, n, size, flags)	\
	kmem_zalloc((n) * (size), KM_SLEEP)

#define	drmm_kmalloc(dev, size, flags)	\
	kmem_alloc((size), KM_SLEEP)

/*
 * drmm_kfree -- free managed allocation.
 * With proper tracking, this would remove from the cleanup list.
 * Currently a no-op since we let device teardown handle cleanup.
 *
 * NOTE: This intentionally leaks until device destroy.  Fine for attach-time
 * allocations that live for the driver's entire lifetime.
 */
#define	drmm_kfree(dev, ptr)	/* no-op for now */

/*
 * drmm_add_action_or_reset -- register a cleanup callback.
 * Stub: returns 0 (success), does not actually register anything.
 * The driver's detach path must handle cleanup explicitly.
 */
typedef void (*drmm_action_fn)(struct drm_device *, void *);

static inline int
drmm_add_action_or_reset(struct drm_device *dev, drmm_action_fn action,
    void *data)
{
	/* TODO: implement proper cleanup tracking */
	return (0);
}

/*
 * drmm_mutex_init -- initialize a mutex tied to device lifetime.
 */
#define	drmm_mutex_init(dev, mtx)	\
	mutex_init((mtx), NULL, MUTEX_DRIVER, NULL)

#ifdef __cplusplus
}
#endif

#endif /* _DRM_SUN_MANAGED_H */
