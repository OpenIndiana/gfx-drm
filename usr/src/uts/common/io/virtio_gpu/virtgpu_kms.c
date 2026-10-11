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
 * Ported from Linux drivers/gpu/drm/virtio/virtgpu_kms.c for illumos.
 *
 * Key porting changes:
 *   - virtio_has_feature() -> virtio_features_present()
 *   - virtio_find_vqs()    -> two virtio_queue_alloc() calls
 *   - virtio_cread_le()    -> virtio_dev_get32()
 *   - virtio_device_ready() -> virtio_init_complete()
 *   - kzalloc              -> kmem_zalloc
 *   - ida_init/alloc/free  -> atomic counters
 */

#include "virtgpu_drv.h"

/*
 * Max scatter-gather segments per virtqueue entry.
 * Virtio-gpu commands are small (cmd + optional data + response),
 * so 16 is more than enough.
 */
#define	VIRTGPU_MAX_SEGS	16

static void
virtio_gpu_config_changed_work_func(struct work_struct *work)
{
	struct virtio_gpu_device *vgdev =
	    container_of(work, struct virtio_gpu_device,
	    config_changed_work);
	uint32_t events_read, events_clear = 0;

	/* Read config space for pending events */
	events_read = virtio_dev_get32(vgdev->vio,
	    offsetof(struct virtio_gpu_config, events_read));

	if (events_read & VIRTIO_GPU_EVENT_DISPLAY) {
		if (vgdev->num_scanouts) {
			if (vgdev->has_edid)
				virtio_gpu_cmd_get_edids(vgdev);
			virtio_gpu_cmd_get_display_info(vgdev);
			virtio_gpu_notify(vgdev);
		}
		events_clear |= VIRTIO_GPU_EVENT_DISPLAY;
	}

	/* Acknowledge events by writing to events_clear */
	virtio_dev_put32(vgdev->vio,
	    offsetof(struct virtio_gpu_config, events_clear),
	    events_clear);
}

static void
virtio_gpu_init_vq(struct virtio_gpu_queue *vgvq,
    void (*work_func)(struct work_struct *))
{
	mutex_init(&vgvq->qlock, NULL, MUTEX_DRIVER, NULL);
	init_waitqueue_head(&vgvq->ack_queue);
	INIT_WORK(&vgvq->dequeue_work, (taskq_func_t)work_func);
}

static void
virtio_gpu_get_capsets(struct virtio_gpu_device *vgdev, int num_capsets)
{
	int i, ret;
	boolean_t invalid_capset_id = B_FALSE;

	vgdev->capsets = drmm_kcalloc(vgdev->ddev, num_capsets,
	    sizeof (struct virtio_gpu_drv_capset), GFP_KERNEL);
	if (vgdev->capsets == NULL) {
		DRM_ERROR("failed to allocate cap sets\n");
		return;
	}

	for (i = 0; i < num_capsets; i++) {
		virtio_gpu_cmd_get_capset_info(vgdev, i);
		virtio_gpu_notify(vgdev);
		ret = wait_event_timeout(vgdev->resp_wq,
		    vgdev->capsets[i].id > 0,
		    drv_usectohz(5000000)); /* 5 seconds */

		if (vgdev->capsets[i].id == 0 ||
		    vgdev->capsets[i].id > MAX_CAPSET_ID)
			invalid_capset_id = B_TRUE;

		if (ret == 0)
			DRM_ERROR("timed out waiting for cap set %d\n", i);
		else if (invalid_capset_id)
			DRM_ERROR("invalid capset id %u\n",
			    vgdev->capsets[i].id);

		if (ret == 0 || invalid_capset_id) {
			mutex_enter(&vgdev->display_info_lock);
			vgdev->capsets = NULL;
			mutex_exit(&vgdev->display_info_lock);
			return;
		}

		vgdev->capset_id_mask |= 1ULL << vgdev->capsets[i].id;
		DRM_INFO("cap set %d: id %d, max-version %d, max-size %d\n",
		    i, vgdev->capsets[i].id,
		    vgdev->capsets[i].max_version,
		    vgdev->capsets[i].max_size);
	}

	vgdev->num_capsets = num_capsets;
}

/*
 * Initialize the virtio-gpu device.
 *
 * Called from attach(9E) after virtio_init() and feature negotiation.
 * The vgdev->vio and vgdev->dip must be set before calling this.
 */
int
virtio_gpu_init(struct virtio_gpu_device *vgdev)
{
	uint32_t num_scanouts, num_capsets;
	int ret = 0;

	mutex_init(&vgdev->display_info_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&vgdev->resource_export_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&vgdev->host_visible_lock, NULL, MUTEX_DRIVER, NULL);
	atomic_set(&vgdev->ctx_id_counter, 0);
	atomic_set(&vgdev->resource_id_counter, 0);
	init_waitqueue_head(&vgdev->resp_wq);

	virtio_gpu_init_vq(&vgdev->ctrlq, virtio_gpu_dequeue_ctrl_func);
	virtio_gpu_init_vq(&vgdev->cursorq, virtio_gpu_dequeue_cursor_func);

	vgdev->fence_drv.context = dma_fence_context_alloc(1);
	mutex_init(&vgdev->fence_drv.lock, NULL, MUTEX_DRIVER, NULL);
	INIT_LIST_HEAD(&vgdev->fence_drv.fences);
	INIT_LIST_HEAD(&vgdev->cap_cache);

	INIT_WORK(&vgdev->config_changed_work,
	    (taskq_func_t)virtio_gpu_config_changed_work_func);
	INIT_WORK(&vgdev->obj_free_work,
	    (taskq_func_t)virtio_gpu_array_put_free_work);
	INIT_LIST_HEAD(&vgdev->obj_free_list);
	mutex_init(&vgdev->obj_free_lock, NULL, MUTEX_DRIVER, NULL);

	/* Check negotiated features */
	if (virtio_features_present(vgdev->vio,
	    (1ULL << VIRTIO_GPU_F_VIRGL)))
		vgdev->has_virgl_3d = B_TRUE;

	if (virtio_features_present(vgdev->vio,
	    (1ULL << VIRTIO_GPU_F_EDID)))
		vgdev->has_edid = B_TRUE;

	if (virtio_features_present(vgdev->vio,
	    (1ULL << VIRTIO_GPU_F_RESOURCE_UUID)))
		vgdev->has_resource_assign_uuid = B_TRUE;

	if (virtio_features_present(vgdev->vio,
	    (1ULL << VIRTIO_GPU_F_RESOURCE_BLOB)))
		vgdev->has_resource_blob = B_TRUE;

	if (virtio_features_present(vgdev->vio,
	    (1ULL << VIRTIO_GPU_F_CONTEXT_INIT)))
		vgdev->has_context_init = B_TRUE;

	DRM_INFO("features: %cvirgl %cedid %cresource_blob %ccontext_init\n",
	    vgdev->has_virgl_3d    ? '+' : '-',
	    vgdev->has_edid        ? '+' : '-',
	    vgdev->has_resource_blob ? '+' : '-',
	    vgdev->has_context_init ? '+' : '-');

	/*
	 * Allocate virtqueues.
	 *
	 * Linux:  virtio_find_vqs(vdev, 2, vqs, info, NULL);
	 * illumos: individual virtio_queue_alloc() calls.
	 *
	 * The interrupt handler is registered here -- when the host
	 * completes a command, virtgpu_ctrl_intr fires and schedules
	 * the dequeue work.
	 */
	vgdev->ctrlq.vq = virtio_queue_alloc(vgdev->vio, 0, "control",
	    virtgpu_ctrl_intr, vgdev, B_FALSE, VIRTGPU_MAX_SEGS);
	if (vgdev->ctrlq.vq == NULL) {
		DRM_ERROR("failed to alloc control virtqueue\n");
		ret = -ENOMEM;
		goto err_vqs;
	}

	vgdev->cursorq.vq = virtio_queue_alloc(vgdev->vio, 1, "cursor",
	    virtgpu_cursor_intr, vgdev, B_FALSE, VIRTGPU_MAX_SEGS);
	if (vgdev->cursorq.vq == NULL) {
		DRM_ERROR("failed to alloc cursor virtqueue\n");
		ret = -ENOMEM;
		goto err_vqs;
	}

	/* Finalize init, enable interrupts */
	if (virtio_init_complete(vgdev->vio, 0) != DDI_SUCCESS) {
		DRM_ERROR("virtio_init_complete failed\n");
		ret = -EIO;
		goto err_vqs;
	}

	/* Allocate vbuf cache */
	ret = virtio_gpu_alloc_vbufs(vgdev);
	if (ret != 0) {
		DRM_ERROR("failed to alloc vbufs\n");
		goto err_vbufs;
	}

	/* Read config space */
	num_scanouts = virtio_dev_get32(vgdev->vio,
	    offsetof(struct virtio_gpu_config, num_scanouts));
	vgdev->num_scanouts = min(num_scanouts,
	    (uint32_t)VIRTIO_GPU_MAX_SCANOUTS);

	if (vgdev->num_scanouts == 0) {
		DRM_INFO("KMS disabled (no scanouts)\n");
		vgdev->has_edid = B_FALSE;
	} else {
		DRM_INFO("number of scanouts: %d\n", num_scanouts);
	}

	num_capsets = virtio_dev_get32(vgdev->vio,
	    offsetof(struct virtio_gpu_config, num_capsets));
	DRM_INFO("number of cap sets: %d\n", num_capsets);

	/* Display init is deferred to Phase 5 */
	ret = virtio_gpu_modeset_init(vgdev);
	if (ret != 0) {
		DRM_ERROR("modeset init failed\n");
		goto err_scanouts;
	}

	/* Query capabilities */
	if (num_capsets)
		virtio_gpu_get_capsets(vgdev, num_capsets);

	if (vgdev->num_scanouts) {
		if (vgdev->has_edid)
			virtio_gpu_cmd_get_edids(vgdev);
		virtio_gpu_cmd_get_display_info(vgdev);
		virtio_gpu_notify(vgdev);
		(void) wait_event_timeout(vgdev->resp_wq,
		    !vgdev->display_info_pending,
		    drv_usectohz(5000000));
	}

	return (0);

err_scanouts:
	virtio_gpu_free_vbufs(vgdev);
err_vbufs:
	/* virtio_fini will clean up queues */
err_vqs:
	return (ret);
}

void
virtio_gpu_deinit(struct virtio_gpu_device *vgdev)
{
	flush_work(&vgdev->obj_free_work);
	flush_work(&vgdev->ctrlq.dequeue_work);
	flush_work(&vgdev->cursorq.dequeue_work);
	flush_work(&vgdev->config_changed_work);

	/* Reset the virtio device */
	virtio_device_reset(vgdev->vio);
}

static void
virtio_gpu_cleanup_cap_cache(struct virtio_gpu_device *vgdev)
{
	struct virtio_gpu_drv_cap_cache *cache_ent;

	while ((cache_ent = list_remove_head(&vgdev->cap_cache)) != NULL) {
		if (cache_ent->caps_cache)
			kmem_free(cache_ent->caps_cache, cache_ent->size);
		kmem_free(cache_ent, sizeof (*cache_ent));
	}
}

int
virtio_gpu_driver_open(struct drm_device *dev, struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_fpriv *vfpriv;

	/* can't create contexts without 3d renderer */
	if (!vgdev->has_virgl_3d)
		return (0);

	vfpriv = kmem_zalloc(sizeof (*vfpriv), KM_SLEEP);
	mutex_init(&vfpriv->context_lock, NULL, MUTEX_DRIVER, NULL);

	vfpriv->ctx_id = (uint32_t)atomic_inc_32_nv(
	    (volatile uint32_t *)&vgdev->ctx_id_counter);
	file->driver_priv = vfpriv;
	return (0);
}

void
virtio_gpu_driver_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_fpriv *vfpriv = file->driver_priv;

	if (!vgdev->has_virgl_3d)
		return;

	if (vfpriv == NULL)
		return;

	if (vfpriv->context_created) {
		virtio_gpu_cmd_context_destroy(vgdev, vfpriv->ctx_id);
		virtio_gpu_notify(vgdev);
	}

	mutex_destroy(&vfpriv->context_lock);
	kmem_free(vfpriv, sizeof (*vfpriv));
	file->driver_priv = NULL;
}

/*
 * Interrupt handlers for the virtio queues.
 * These are registered via virtio_queue_alloc() and fire when the
 * host completes processing a command.  They schedule the dequeue
 * work function which runs in process context.
 */
uint_t
virtgpu_ctrl_intr(caddr_t arg1, caddr_t arg2)
{
	struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)arg1;

	schedule_work(&vgdev->ctrlq.dequeue_work);
	return (DDI_INTR_CLAIMED);
}

uint_t
virtgpu_cursor_intr(caddr_t arg1, caddr_t arg2)
{
	struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)arg1;

	schedule_work(&vgdev->cursorq.dequeue_work);
	return (DDI_INTR_CLAIMED);
}
