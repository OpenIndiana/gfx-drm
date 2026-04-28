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

#ifndef	_DRM_SUN_GEM_SHMEM_H
#define	_DRM_SUN_GEM_SHMEM_H

/*
 * illumos GEM shmem helper.
 *
 * Replaces Linux drm_gem_shmem_helper which uses shmem_file_setup()
 * and struct page arrays.  On illumos we use ddi_dma_mem_alloc() for
 * DMA-capable anonymous memory.
 *
 * The struct drm_gem_shmem_object is defined in virtgpu_drv.h since
 * it is specific to the virtio-gpu driver's embedding pattern.
 * This header provides the helper functions.
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>

struct drm_gem_shmem_object;
struct drm_device;
struct sg_table;

/*
 * DMA attributes for GEM shmem allocations.
 * 64-bit addressing, page-aligned, suitable for virtio scatter-gather.
 */
extern ddi_dma_attr_t drm_gem_shmem_dma_attr;

/*
 * Allocate a GEM shmem object with DMA-capable backing memory.
 * Returns 0 on success, negative errno on failure.
 */
int drm_gem_shmem_create_illumos(struct drm_device *dev,
    struct drm_gem_shmem_object *shmem, size_t size);

/*
 * Free a GEM shmem object's DMA backing.
 */
void drm_gem_shmem_free_illumos(struct drm_gem_shmem_object *shmem);

/*
 * Build a scatter-gather table from the DMA cookies.
 * Returns the sg_table (caller must free), or NULL on failure.
 */
struct sg_table *drm_gem_shmem_get_pages_sgt_illumos(
    struct drm_gem_shmem_object *shmem);

/*
 * Get kernel virtual address for CPU access.
 */
void *drm_gem_shmem_vmap_illumos(struct drm_gem_shmem_object *shmem);

/*
 * Release kernel virtual mapping (no-op, VA is always valid).
 */
void drm_gem_shmem_vunmap_illumos(struct drm_gem_shmem_object *shmem);

#endif /* _DRM_SUN_GEM_SHMEM_H */
