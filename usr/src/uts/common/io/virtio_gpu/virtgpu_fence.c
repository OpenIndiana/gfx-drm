/*
 * Copyright (C) 2015 Red Hat, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Ported from Linux drivers/gpu/drm/virtio/virtgpu_fence.c for illumos.
 * Uses drm_sun_dma_fence shim for fence operations.
 */

#include "virtgpu_drv.h"

#define	to_virtio_gpu_fence(x) \
	container_of(x, struct virtio_gpu_fence, f)

static const char *
virtio_gpu_get_driver_name(dma_fence_t *f)
{
	return ("virtio_gpu");
}

static const char *
virtio_gpu_get_timeline_name(dma_fence_t *f)
{
	return ("controlq");
}

static boolean_t
virtio_gpu_fence_signaled(dma_fence_t *f)
{
	WARN_ON_ONCE(f->fence_seqno == 0);
	return (B_FALSE);
}

static const dma_fence_ops_t virtio_gpu_fence_ops = {
	.get_driver_name	= virtio_gpu_get_driver_name,
	.get_timeline_name	= virtio_gpu_get_timeline_name,
	.signaled		= virtio_gpu_fence_signaled,
};

struct virtio_gpu_fence *
virtio_gpu_fence_alloc(struct virtio_gpu_device *vgdev,
    uint64_t base_fence_ctx, uint32_t ring_idx)
{
	uint64_t fence_context = base_fence_ctx + ring_idx;
	struct virtio_gpu_fence_driver *drv = &vgdev->fence_drv;
	struct virtio_gpu_fence *fence;

	fence = kmem_zalloc(sizeof (*fence), KM_SLEEP);

	fence->drv = drv;
	fence->ring_idx = ring_idx;
	fence->emit_fence_info = (base_fence_ctx != drv->context);

	/*
	 * Partially initialize -- seqno is unknown until emit time.
	 * The fence must not be used outside the driver until
	 * virtio_gpu_fence_emit() is called.
	 */
	dma_fence_init(&fence->f, &virtio_gpu_fence_ops, &drv->lock,
	    fence_context, 0);

	return (fence);
}

void
virtio_gpu_fence_emit(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_ctrl_hdr *cmd_hdr,
    struct virtio_gpu_fence *fence)
{
	struct virtio_gpu_fence_driver *drv = &vgdev->fence_drv;

	mutex_enter(&drv->lock);
	fence->fence_id = fence->f.fence_seqno = ++drv->current_fence_id;
	dma_fence_get(&fence->f);
	list_insert_tail(&drv->fences, &fence->node);
	mutex_exit(&drv->lock);

	cmd_hdr->flags |= cpu_to_le32(VIRTIO_GPU_FLAG_FENCE);
	cmd_hdr->fence_id = cpu_to_le64(fence->fence_id);

	if (fence->emit_fence_info) {
		cmd_hdr->flags |= cpu_to_le32(VIRTIO_GPU_FLAG_INFO_RING_IDX);
		cmd_hdr->ring_idx = (uint8_t)fence->ring_idx;
	}
}

void
virtio_gpu_fence_event_process(struct virtio_gpu_device *vgdev,
    uint64_t fence_id)
{
	struct virtio_gpu_fence_driver *drv = &vgdev->fence_drv;
	struct virtio_gpu_fence *signaled, *curr, *tmp;

	mutex_enter(&drv->lock);
	atomic64_set(&vgdev->fence_drv.last_fence_id, fence_id);

	/*
	 * Walk the fence list looking for the fence with the matching ID.
	 * Signal that fence plus any earlier fences in the same context.
	 *
	 * NOTE: The list_head-based iteration from Linux is replaced with
	 * a simpler approach for now.  The list_head in drm_sun uses illumos
	 * list_t semantics.  For initial bring-up, we iterate manually.
	 *
	 * TODO: Implement proper list_for_each_entry_safe using illumos list_t.
	 * For now, this is a placeholder that will be completed when the
	 * fence list management is fully ported.
	 */

	/* Placeholder: signal all fences up to fence_id */
	/* This will be properly implemented with list iteration in Phase 1.5 */

	mutex_exit(&drv->lock);
}
