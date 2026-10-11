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

#ifndef	_DRM_SUN_GEM_MODERN_H
#define	_DRM_SUN_GEM_MODERN_H

/*
 * Modern DRM GEM object compatibility layer.
 *
 * The existing drm_gem_object in drmP.h (from 3.14) lacks:
 *   - const struct drm_gem_object_funcs *funcs  (per-object vtable)
 *   - struct dma_resv *resv  (reservation object)
 *
 * Rather than modifying drmP.h (which would affect the working i915 driver),
 * we define compatibility macros and require modern drivers to embed their
 * own dma_resv and funcs in their driver-private object structures.
 *
 * For virtio-gpu, the virtio_gpu_object embeds a drm_gem_shmem_object
 * which embeds a drm_gem_object plus its own _resv and funcs.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Modern GEM object function table.
 *
 * In Linux 5.9+, each GEM object has a funcs pointer for per-object
 * operations.  The drm core dispatches through this instead of through
 * drm_driver callbacks.
 */
struct drm_gem_object_funcs {
	void (*free)(struct drm_gem_object *obj);
	int (*open)(struct drm_gem_object *obj, struct drm_file *file);
	void (*close)(struct drm_gem_object *obj, struct drm_file *file);
	void (*print_info)(struct drm_printer *p, unsigned int indent,
	    const struct drm_gem_object *obj);
	int (*pin)(struct drm_gem_object *obj);
	void (*unpin)(struct drm_gem_object *obj);
	int (*vmap)(struct drm_gem_object *obj, void **vaddr);
	void (*vunmap)(struct drm_gem_object *obj, void *vaddr);
	int (*mmap)(struct drm_gem_object *obj, void *vma);
};

/* Forward declaration for drm_printer (unused on illumos for now) */
struct drm_printer;

/*
 * Modern GEM reference counting.
 *
 * Map modern drm_gem_object_get/put to existing kref-based refcounting.
 * The existing drmP.h uses drm_gem_object_reference/unreference.
 */
#define	drm_gem_object_get(obj)		\
	kref_get(&(obj)->refcount)

/*
 * drm_gem_object_put -- drop reference, potentially freeing.
 * The existing code uses drm_gem_object_unreference_unlocked().
 * We map directly to a kref_put with a release callback.
 *
 * Note: In the modern DRM model, obj->funcs->free() is the release
 * function.  For now, we delegate to the existing gem_free_object
 * driver callback since the 3.14 core expects that.
 */
extern void drm_gem_object_release(struct drm_gem_object *obj);

static inline void
drm_gem_object_put(struct drm_gem_object *obj)
{
	if (obj)
		drm_gem_object_unreference_unlocked(obj);
}

/*
 * drm_gem_handle_create -- create a userspace handle for a GEM object.
 * Already exists in the 3.14 DRM core.
 */
extern int drm_gem_handle_create(struct drm_file *file_priv,
    struct drm_gem_object *obj, uint32_t *handlep);

/*
 * drm_gem_handle_delete -- remove a userspace handle.
 */
extern int drm_gem_handle_delete(struct drm_file *filp, uint32_t handle);

/*
 * drm_gem_object_lookup -- find GEM object by handle.
 * The 3.14 version takes (dev, filp, handle); modern takes (filp, handle).
 * Provide a 2-arg compatibility macro.
 */
extern struct drm_gem_object *
drm_gem_object_lookup_3arg(struct drm_device *dev, struct drm_file *filp,
    uint32_t handle);

#define	drm_gem_object_lookup(filp, handle)	\
	drm_gem_object_lookup_3arg(NULL, (filp), (handle))

/*
 * DRIVER_GEM and DRIVER_RENDER feature flags.
 * DRIVER_GEM (0x1000) already exists in drmP.h.
 * Add DRIVER_RENDER for render node support.
 */
#ifndef DRIVER_RENDER
#define	DRIVER_RENDER	0x10000
#endif

#ifndef DRIVER_ATOMIC
#define	DRIVER_ATOMIC	0x20000
#endif

#ifndef DRIVER_SYNCOBJ
#define	DRIVER_SYNCOBJ	0x40000
#endif

/*
 * drm_dev_enter / drm_dev_exit -- hotplug guard.
 * The virtio-gpu device is never hotplugged in a VM.
 * Stub: always succeed.
 */
static inline boolean_t
drm_dev_enter(struct drm_device *dev, int *idx)
{
	*idx = 0;
	return (B_TRUE);
}

static inline void
drm_dev_exit(int idx)
{
}

/*
 * drm_dev_unplug / drm_dev_is_unplugged -- device removal.
 * Stub for non-hotplug drivers.
 */
static inline boolean_t
drm_dev_is_unplugged(struct drm_device *dev)
{
	return (B_FALSE);
}

#ifdef __cplusplus
}
#endif

#endif /* _DRM_SUN_GEM_MODERN_H */
