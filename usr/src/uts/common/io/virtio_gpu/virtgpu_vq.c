/*
 * Copyright (C) 2015 Red Hat, Inc.
 * All Rights Reserved.
 *
 * Authors:
 *    Dave Airlie <airlied@redhat.com>
 *    Gerd Hoffmann <kraxel@redhat.com>
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
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Ported from Linux drivers/gpu/drm/virtio/virtgpu_vq.c for illumos.
 *
 * Core porting transformation:
 *
 * Linux submits commands by building scatter-gather lists:
 *   virtqueue_add_sgs(vq, sgs, outcnt, incnt, vbuf, GFP_ATOMIC);
 *
 * illumos builds descriptor chains incrementally:
 *   chain = virtio_chain_alloc(vq, KM_NOSLEEP);
 *   virtio_chain_append(chain, pa, len, VIRTIO_DIR_DEVICE_READS);
 *   virtio_chain_append(chain, pa, len, VIRTIO_DIR_DEVICE_WRITES);
 *   virtio_chain_data_set(chain, vbuf);
 *   virtio_chain_submit(chain, B_TRUE);
 *
 * Completion:
 *   Linux:   vbuf = virtqueue_get_buf(vq, &len);
 *   illumos: chain = virtio_queue_poll(vq);
 *            vbuf = virtio_chain_data(chain);
 *            virtio_chain_free(chain);
 *
 * Memory model:
 *   vbufs are allocated from a kmem_cache backed by virtio_dma_alloc()
 *   so their physical addresses are known for virtio_chain_append().
 */

#include "virtgpu_drv.h"

#define	MAX_INLINE_CMD_SIZE	96
#define	MAX_INLINE_RESP_SIZE	24
#define	VBUFFER_SIZE		(sizeof (struct virtio_gpu_vbuffer) \
				+ MAX_INLINE_CMD_SIZE \
				+ MAX_INLINE_RESP_SIZE)

static void
convert_to_hw_box(struct virtio_gpu_box *dst,
    const struct drm_virtgpu_3d_box *src)
{
	dst->x = cpu_to_le32(src->x);
	dst->y = cpu_to_le32(src->y);
	dst->z = cpu_to_le32(src->z);
	dst->w = cpu_to_le32(src->w);
	dst->h = cpu_to_le32(src->h);
	dst->d = cpu_to_le32(src->d);
}

/* ---- vbuf allocation ---- */

int
virtio_gpu_alloc_vbufs(struct virtio_gpu_device *vgdev)
{
	vgdev->vbufs = kmem_cache_create("virtio-gpu-vbufs",
	    VBUFFER_SIZE, 0, NULL, NULL, NULL, NULL, NULL, 0);
	if (vgdev->vbufs == NULL)
		return (-ENOMEM);
	return (0);
}

void
virtio_gpu_free_vbufs(struct virtio_gpu_device *vgdev)
{
	if (vgdev->vbufs != NULL) {
		kmem_cache_destroy(vgdev->vbufs);
		vgdev->vbufs = NULL;
	}
}

static struct virtio_gpu_vbuffer *
virtio_gpu_get_vbuf(struct virtio_gpu_device *vgdev,
    int size, int resp_size, void *resp_buf,
    virtio_gpu_resp_cb resp_cb)
{
	struct virtio_gpu_vbuffer *vbuf;

	vbuf = kmem_cache_alloc(vgdev->vbufs, KM_SLEEP);
	bzero(vbuf, VBUFFER_SIZE);

	ASSERT(size <= MAX_INLINE_CMD_SIZE);
	ASSERT(size >= (int)sizeof (struct virtio_gpu_ctrl_hdr));

	vbuf->buf = (char *)vbuf + sizeof (*vbuf);
	vbuf->size = size;
	vbuf->resp_cb = resp_cb;
	vbuf->resp_size = resp_size;

	if (resp_size <= MAX_INLINE_RESP_SIZE)
		vbuf->resp_buf = (char *)vbuf->buf + size;
	else
		vbuf->resp_buf = resp_buf;

	ASSERT(vbuf->resp_buf != NULL);

	/*
	 * Allocate DMA memory for this vbuf so we know its PA.
	 * The entire VBUFFER_SIZE block (cmd + inline resp) is in
	 * one contiguous DMA allocation.
	 */
	vbuf->vdma = virtio_dma_alloc(vgdev->vio, VBUFFER_SIZE,
	    &virtio_dma_attr_sgl, DDI_DMA_CONSISTENT | DDI_DMA_RDWR,
	    KM_SLEEP);
	if (vbuf->vdma != NULL) {
		/*
		 * Copy the vbuf layout into DMA memory.
		 * The cmd and resp buffers inside the vbuf point to offsets
		 * within the kmem_cache allocation.  We need to use the DMA
		 * VA instead for the actual data that goes on the virtqueue.
		 *
		 * For now, we use the kmem_cache vbuf for CPU access and
		 * the vdma for the PA.  The memcpy happens at submit time.
		 */
	}

	return (vbuf);
}

static struct virtio_gpu_ctrl_hdr *
virtio_gpu_vbuf_ctrl_hdr(struct virtio_gpu_vbuffer *vbuf)
{
	return ((struct virtio_gpu_ctrl_hdr *)vbuf->buf);
}

static void *
virtio_gpu_alloc_cmd_resp(struct virtio_gpu_device *vgdev,
    virtio_gpu_resp_cb cb,
    struct virtio_gpu_vbuffer **vbuffer_p,
    int cmd_size, int resp_size, void *resp_buf)
{
	struct virtio_gpu_vbuffer *vbuf;

	vbuf = virtio_gpu_get_vbuf(vgdev, cmd_size, resp_size, resp_buf, cb);
	*vbuffer_p = vbuf;
	return (vbuf->buf);
}

static void *
virtio_gpu_alloc_cmd(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_vbuffer **vbuffer_p, int size)
{
	return (virtio_gpu_alloc_cmd_resp(vgdev, NULL, vbuffer_p, size,
	    sizeof (struct virtio_gpu_ctrl_hdr), NULL));
}

static void *
virtio_gpu_alloc_cmd_cb(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_vbuffer **vbuffer_p, int size,
    virtio_gpu_resp_cb cb)
{
	return (virtio_gpu_alloc_cmd_resp(vgdev, cb, vbuffer_p, size,
	    sizeof (struct virtio_gpu_ctrl_hdr), NULL));
}

static void
free_vbuf(struct virtio_gpu_device *vgdev, struct virtio_gpu_vbuffer *vbuf)
{
	if (vbuf->resp_size > MAX_INLINE_RESP_SIZE && vbuf->resp_buf)
		kmem_free(vbuf->resp_buf, vbuf->resp_size);
	if (vbuf->data_buf)
		kmem_free(vbuf->data_buf, vbuf->data_size);
	if (vbuf->vdma)
		virtio_dma_free(vbuf->vdma);
	kmem_cache_free(vgdev->vbufs, vbuf);
}

/* ---- Command submission via illumos virtio chain API ---- */

/*
 * Submit a vbuf to the control virtqueue.
 *
 * This is the core porting transformation.  Linux uses:
 *   virtqueue_add_sgs(vq, sgs, outcnt, incnt, vbuf, GFP_ATOMIC);
 *
 * We translate to:
 *   chain = virtio_chain_alloc(vq);
 *   virtio_chain_append(chain, cmd_pa, cmd_len, DEVICE_READS);
 *   [virtio_chain_append(chain, data_pa, data_len, DEVICE_READS);]
 *   virtio_chain_append(chain, resp_pa, resp_len, DEVICE_WRITES);
 *   virtio_chain_data_set(chain, vbuf);
 *   virtio_chain_submit(chain, B_TRUE);
 *
 * For initial bring-up, we use the DMA VA from virtio_dma_alloc
 * and copy data between the kmem vbuf and the DMA buffer.
 */
static int
virtio_gpu_queue_fenced_ctrl_buffer(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_vbuffer *vbuf, struct virtio_gpu_fence *fence)
{
	virtio_chain_t *vic;
	uint64_t cmd_pa, resp_pa;

	if (vbuf->vdma == NULL) {
		DRM_ERROR("vbuf has no DMA backing\n");
		free_vbuf(vgdev, vbuf);
		return (-ENOMEM);
	}

	/* Set fence ID if fenced */
	mutex_enter(&vgdev->ctrlq.qlock);

	if (fence != NULL) {
		virtio_gpu_fence_emit(vgdev,
		    virtio_gpu_vbuf_ctrl_hdr(vbuf), fence);
		if (vbuf->objs != NULL) {
			virtio_gpu_array_add_fence(vbuf->objs, &fence->f);
			virtio_gpu_array_unlock_resv(vbuf->objs);
		}
	}

	/*
	 * Copy cmd data to DMA buffer.
	 * The DMA VA is at virtio_dma_va(vbuf->vdma).
	 * Layout: [cmd (vbuf->size)] [resp (vbuf->resp_size)]
	 */
	bcopy(vbuf->buf, virtio_dma_va(vbuf->vdma), vbuf->size);

	cmd_pa = virtio_dma_cookie_pa(vbuf->vdma, 0);
	resp_pa = cmd_pa + (uint64_t)vbuf->size;

	/* Build the descriptor chain */
	vic = virtio_chain_alloc(vgdev->ctrlq.vq, KM_SLEEP);
	if (vic == NULL) {
		mutex_exit(&vgdev->ctrlq.qlock);
		DRM_ERROR("failed to alloc virtio chain\n");
		free_vbuf(vgdev, vbuf);
		return (-ENOMEM);
	}

	/* Command header (device reads) */
	if (virtio_chain_append(vic, cmd_pa, vbuf->size,
	    VIRTIO_DIR_DEVICE_READS) != DDI_SUCCESS) {
		virtio_chain_free(vic);
		mutex_exit(&vgdev->ctrlq.qlock);
		DRM_ERROR("failed to append cmd to chain\n");
		free_vbuf(vgdev, vbuf);
		return (-ENOMEM);
	}

	/* Optional data payload (device reads) */
	if (vbuf->data_size > 0 && vbuf->data_buf != NULL) {
		/*
		 * TODO: For data payloads, we need a separate DMA allocation.
		 * For initial bring-up, large data payloads (execbuffer etc.)
		 * are deferred.  Small inline data is not used by basic
		 * 2D commands.
		 */
	}

	/* Response buffer (device writes) */
	if (vbuf->resp_size > 0) {
		if (virtio_chain_append(vic, resp_pa, vbuf->resp_size,
		    VIRTIO_DIR_DEVICE_WRITES) != DDI_SUCCESS) {
			virtio_chain_free(vic);
			mutex_exit(&vgdev->ctrlq.qlock);
			DRM_ERROR("failed to append resp to chain\n");
			free_vbuf(vgdev, vbuf);
			return (-ENOMEM);
		}
	}

	/* Attach vbuf as private data for retrieval at completion */
	virtio_chain_data_set(vic, vbuf);

	vbuf->seqno = ++vgdev->ctrlq.seqno;
	atomic_inc_32((volatile uint32_t *)&vgdev->pending_commands);

	/* Submit and notify */
	virtio_chain_submit(vic, B_TRUE);

	mutex_exit(&vgdev->ctrlq.qlock);
	return (0);
}

static int
virtio_gpu_queue_ctrl_buffer(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_vbuffer *vbuf)
{
	return (virtio_gpu_queue_fenced_ctrl_buffer(vgdev, vbuf, NULL));
}

/* ---- Dequeue (completion) handling ---- */

void
virtio_gpu_dequeue_ctrl_func(struct work_struct *work)
{
	struct virtio_gpu_device *vgdev =
	    container_of(work, struct virtio_gpu_device,
	    ctrlq.dequeue_work);
	virtio_chain_t *vic;
	struct virtio_gpu_vbuffer *vbuf;
	struct virtio_gpu_ctrl_hdr *resp;
	uint64_t fence_id;

	mutex_enter(&vgdev->ctrlq.qlock);

	while ((vic = virtio_queue_poll(vgdev->ctrlq.vq)) != NULL) {
		vbuf = virtio_chain_data(vic);

		/*
		 * Copy response back from DMA buffer to vbuf.
		 * The response sits at offset vbuf->size in the DMA buffer.
		 */
		if (vbuf != NULL && vbuf->vdma != NULL && vbuf->resp_size > 0) {
			bcopy((char *)virtio_dma_va(vbuf->vdma) + vbuf->size,
			    vbuf->resp_buf, vbuf->resp_size);
		}

		virtio_chain_free(vic);

		if (vbuf == NULL)
			continue;

		resp = (struct virtio_gpu_ctrl_hdr *)vbuf->resp_buf;

		if (resp->type != cpu_to_le32(VIRTIO_GPU_RESP_OK_NODATA)) {
			if (le32_to_cpu(resp->type) >=
			    VIRTIO_GPU_RESP_ERR_UNSPEC) {
				struct virtio_gpu_ctrl_hdr *cmd;

				cmd = virtio_gpu_vbuf_ctrl_hdr(vbuf);
				DRM_ERROR_RATELIMITED(
				    "response 0x%x (command 0x%x)\n",
				    le32_to_cpu(resp->type),
				    le32_to_cpu(cmd->type));
			} else {
				DRM_DEBUG("response 0x%x\n",
				    le32_to_cpu(resp->type));
			}
		}

		if (resp->flags & cpu_to_le32(VIRTIO_GPU_FLAG_FENCE)) {
			fence_id = le64_to_cpu(resp->fence_id);
			virtio_gpu_fence_event_process(vgdev, fence_id);
		}

		if (vbuf->resp_cb != NULL)
			vbuf->resp_cb(vgdev, vbuf);

		if (vbuf->objs != NULL)
			virtio_gpu_array_put_free_delayed(vgdev, vbuf->objs);

		free_vbuf(vgdev, vbuf);
	}

	mutex_exit(&vgdev->ctrlq.qlock);

	wake_up(&vgdev->ctrlq.ack_queue);
}

void
virtio_gpu_dequeue_cursor_func(struct work_struct *work)
{
	struct virtio_gpu_device *vgdev =
	    container_of(work, struct virtio_gpu_device,
	    cursorq.dequeue_work);
	virtio_chain_t *vic;
	struct virtio_gpu_vbuffer *vbuf;

	mutex_enter(&vgdev->cursorq.qlock);

	while ((vic = virtio_queue_poll(vgdev->cursorq.vq)) != NULL) {
		vbuf = virtio_chain_data(vic);
		virtio_chain_free(vic);
		if (vbuf != NULL)
			free_vbuf(vgdev, vbuf);
	}

	mutex_exit(&vgdev->cursorq.qlock);

	wake_up(&vgdev->cursorq.ack_queue);
}

/* ---- Notify ---- */

void
virtio_gpu_notify(struct virtio_gpu_device *vgdev)
{
	if (!atomic_read(&vgdev->pending_commands))
		return;

	mutex_enter(&vgdev->ctrlq.qlock);
	atomic_set(&vgdev->pending_commands, 0);
	/*
	 * On illumos, notification is implicit in virtio_chain_submit()
	 * when the notify flag is B_TRUE.  This function exists for
	 * deferred notification patterns.  For now, it's a flush point.
	 */
	virtio_queue_flush(vgdev->ctrlq.vq);
	mutex_exit(&vgdev->ctrlq.qlock);
}

/* ---- GPU commands ---- */

void
virtio_gpu_cmd_create_resource(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object *bo,
    struct virtio_gpu_object_params *params,
    struct virtio_gpu_object_array *objs,
    struct virtio_gpu_fence *fence)
{
	struct virtio_gpu_resource_create_2d *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof (*cmd_p));
	bzero(cmd_p, sizeof (*cmd_p));
	vbuf->objs = objs;

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
	cmd_p->resource_id = cpu_to_le32(bo->hw_res_handle);
	cmd_p->format = cpu_to_le32(params->format);
	cmd_p->width = cpu_to_le32(params->width);
	cmd_p->height = cpu_to_le32(params->height);

	(void) virtio_gpu_queue_fenced_ctrl_buffer(vgdev, vbuf, fence);
	bo->created = B_TRUE;
}

static void
virtio_gpu_cmd_unref_cb(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_vbuffer *vbuf)
{
	struct virtio_gpu_object *bo;

	bo = vbuf->resp_cb_data;
	vbuf->resp_cb_data = NULL;
	virtio_gpu_cleanup_object(bo);
}

void
virtio_gpu_cmd_unref_resource(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object *bo)
{
	struct virtio_gpu_resource_unref *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;
	int ret;

	cmd_p = virtio_gpu_alloc_cmd_cb(vgdev, &vbuf, sizeof (*cmd_p),
	    virtio_gpu_cmd_unref_cb);
	bzero(cmd_p, sizeof (*cmd_p));

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_UNREF);
	cmd_p->resource_id = cpu_to_le32(bo->hw_res_handle);

	vbuf->resp_cb_data = bo;
	ret = virtio_gpu_queue_ctrl_buffer(vgdev, vbuf);
	if (ret < 0)
		virtio_gpu_cleanup_object(bo);
}

void
virtio_gpu_cmd_set_scanout(struct virtio_gpu_device *vgdev,
    uint32_t scanout_id, uint32_t resource_id,
    uint32_t width, uint32_t height, uint32_t x, uint32_t y)
{
	struct virtio_gpu_set_scanout *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof (*cmd_p));
	bzero(cmd_p, sizeof (*cmd_p));

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_SET_SCANOUT);
	cmd_p->resource_id = cpu_to_le32(resource_id);
	cmd_p->scanout_id = cpu_to_le32(scanout_id);
	cmd_p->r.width = cpu_to_le32(width);
	cmd_p->r.height = cpu_to_le32(height);
	cmd_p->r.x = cpu_to_le32(x);
	cmd_p->r.y = cpu_to_le32(y);

	(void) virtio_gpu_queue_ctrl_buffer(vgdev, vbuf);
}

void
virtio_gpu_cmd_resource_flush(struct virtio_gpu_device *vgdev,
    uint32_t resource_id, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height,
    struct virtio_gpu_object_array *objs,
    struct virtio_gpu_fence *fence)
{
	struct virtio_gpu_resource_flush *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof (*cmd_p));
	bzero(cmd_p, sizeof (*cmd_p));
	vbuf->objs = objs;

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_FLUSH);
	cmd_p->resource_id = cpu_to_le32(resource_id);
	cmd_p->r.width = cpu_to_le32(width);
	cmd_p->r.height = cpu_to_le32(height);
	cmd_p->r.x = cpu_to_le32(x);
	cmd_p->r.y = cpu_to_le32(y);

	(void) virtio_gpu_queue_fenced_ctrl_buffer(vgdev, vbuf, fence);
}

void
virtio_gpu_cmd_transfer_to_host_2d(struct virtio_gpu_device *vgdev,
    uint64_t offset, uint32_t width, uint32_t height,
    uint32_t x, uint32_t y,
    struct virtio_gpu_object_array *objs,
    struct virtio_gpu_fence *fence)
{
	struct virtio_gpu_transfer_to_host_2d *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;
	struct virtio_gpu_object *bo;

	bo = gem_to_virtio_gpu_obj(objs->objs[0]);

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof (*cmd_p));
	bzero(cmd_p, sizeof (*cmd_p));
	vbuf->objs = objs;

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
	cmd_p->resource_id = cpu_to_le32(bo->hw_res_handle);
	cmd_p->offset = cpu_to_le64(offset);
	cmd_p->r.width = cpu_to_le32(width);
	cmd_p->r.height = cpu_to_le32(height);
	cmd_p->r.x = cpu_to_le32(x);
	cmd_p->r.y = cpu_to_le32(y);

	(void) virtio_gpu_queue_fenced_ctrl_buffer(vgdev, vbuf, fence);
}

void
virtio_gpu_object_attach(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object *obj,
    struct virtio_gpu_mem_entry *ents, unsigned int nents)
{
	struct virtio_gpu_resource_attach_backing *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof (*cmd_p));
	bzero(cmd_p, sizeof (*cmd_p));

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
	cmd_p->resource_id = cpu_to_le32(obj->hw_res_handle);
	cmd_p->nr_entries = cpu_to_le32(nents);

	/*
	 * The mem_entry array follows as a data payload.
	 * TODO: Attach data_buf with the ents array for the virtqueue.
	 * For initial bring-up, this needs the data_buf DMA path.
	 */
	vbuf->data_buf = ents;
	vbuf->data_size = sizeof (*ents) * nents;

	(void) virtio_gpu_queue_ctrl_buffer(vgdev, vbuf);
	obj->attached = B_TRUE;
}

void
virtio_gpu_object_detach(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object *obj, struct virtio_gpu_fence *fence)
{
	struct virtio_gpu_resource_detach_backing *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof (*cmd_p));
	bzero(cmd_p, sizeof (*cmd_p));

	cmd_p->hdr.type =
	    cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
	cmd_p->resource_id = cpu_to_le32(obj->hw_res_handle);

	(void) virtio_gpu_queue_fenced_ctrl_buffer(vgdev, vbuf, fence);
}

/* ---- Capset queries (response callbacks) ---- */

static void
virtio_gpu_cmd_capset_info_cb(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_vbuffer *vbuf)
{
	struct virtio_gpu_resp_capset_info *resp =
	    (struct virtio_gpu_resp_capset_info *)vbuf->resp_buf;
	int i = le32_to_cpu(vbuf->seqno); /* index stored in seqno */

	/* Bounds check */
	if (vgdev->capsets == NULL)
		return;

	mutex_enter(&vgdev->display_info_lock);
	if (i < (int)vgdev->num_capsets || vgdev->capsets != NULL) {
		/* capset_info responses are indexed by the capset_index */
		/* For now, parse from the response directly */
		/* The caller will fill vgdev->capsets[i] */
	}
	mutex_exit(&vgdev->display_info_lock);

	wake_up(&vgdev->resp_wq);
}

int
virtio_gpu_cmd_get_capset_info(struct virtio_gpu_device *vgdev, int idx)
{
	struct virtio_gpu_get_capset_info *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd_cb(vgdev, &vbuf, sizeof (*cmd_p),
	    virtio_gpu_cmd_capset_info_cb);
	bzero(cmd_p, sizeof (*cmd_p));

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_GET_CAPSET_INFO);
	cmd_p->capset_index = cpu_to_le32(idx);

	/* Store index for the callback */
	vbuf->seqno = idx;

	return (virtio_gpu_queue_ctrl_buffer(vgdev, vbuf));
}

int
virtio_gpu_cmd_get_capset(struct virtio_gpu_device *vgdev,
    int idx, int version, struct virtio_gpu_drv_cap_cache **cache_p)
{
	/* TODO: Implement capset retrieval */
	return (-ENOSYS);
}

/* ---- Display info ---- */

static void
virtio_gpu_cmd_get_display_info_cb(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_vbuffer *vbuf)
{
	struct virtio_gpu_resp_display_info *resp =
	    (struct virtio_gpu_resp_display_info *)vbuf->resp_buf;
	uint32_t i;

	mutex_enter(&vgdev->display_info_lock);
	for (i = 0; i < vgdev->num_scanouts; i++) {
		/* TODO: Store display info in vgdev->outputs[i] */
	}
	vgdev->display_info_pending = B_FALSE;
	mutex_exit(&vgdev->display_info_lock);

	wake_up(&vgdev->resp_wq);
}

int
virtio_gpu_cmd_get_display_info(struct virtio_gpu_device *vgdev)
{
	struct virtio_gpu_ctrl_hdr *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;
	void *resp_buf;

	resp_buf = kmem_zalloc(sizeof (struct virtio_gpu_resp_display_info),
	    KM_SLEEP);

	cmd_p = virtio_gpu_alloc_cmd_resp(vgdev,
	    virtio_gpu_cmd_get_display_info_cb, &vbuf,
	    sizeof (*cmd_p),
	    sizeof (struct virtio_gpu_resp_display_info),
	    resp_buf);
	bzero(cmd_p, sizeof (*cmd_p));

	cmd_p->type = cpu_to_le32(VIRTIO_GPU_CMD_GET_DISPLAY_INFO);

	vgdev->display_info_pending = B_TRUE;
	return (virtio_gpu_queue_ctrl_buffer(vgdev, vbuf));
}

int
virtio_gpu_cmd_get_edids(struct virtio_gpu_device *vgdev)
{
	/* TODO: EDID retrieval - deferred */
	return (0);
}

/* ---- 3D context commands (stubs for now) ---- */

void
virtio_gpu_cmd_context_create(struct virtio_gpu_device *vgdev,
    uint32_t id, uint32_t context_init, uint32_t nlen, const char *name)
{
	struct virtio_gpu_ctx_create *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof (*cmd_p));
	bzero(cmd_p, sizeof (*cmd_p));

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_CTX_CREATE);
	cmd_p->hdr.ctx_id = cpu_to_le32(id);
	cmd_p->nlen = cpu_to_le32(nlen);
	cmd_p->context_init = cpu_to_le32(context_init);
	if (nlen > 0)
		(void) strncpy(cmd_p->debug_name, name,
		    sizeof (cmd_p->debug_name) - 1);

	(void) virtio_gpu_queue_ctrl_buffer(vgdev, vbuf);
}

void
virtio_gpu_cmd_context_destroy(struct virtio_gpu_device *vgdev, uint32_t id)
{
	struct virtio_gpu_ctx_destroy *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof (*cmd_p));
	bzero(cmd_p, sizeof (*cmd_p));

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_CTX_DESTROY);
	cmd_p->hdr.ctx_id = cpu_to_le32(id);

	(void) virtio_gpu_queue_ctrl_buffer(vgdev, vbuf);
}

void
virtio_gpu_cmd_context_attach_resource(struct virtio_gpu_device *vgdev,
    uint32_t ctx_id, struct virtio_gpu_object_array *objs)
{
	/* TODO */
}

void
virtio_gpu_cmd_context_detach_resource(struct virtio_gpu_device *vgdev,
    uint32_t ctx_id, struct virtio_gpu_object_array *objs)
{
	/* TODO */
}

void
virtio_gpu_cmd_submit(struct virtio_gpu_device *vgdev,
    void *data, uint32_t data_size, uint32_t ctx_id,
    struct virtio_gpu_object_array *objs,
    struct virtio_gpu_fence *fence)
{
	/* TODO: 3D command submission - deferred to Phase 3 */
}

void
virtio_gpu_cmd_transfer_from_host_3d(struct virtio_gpu_device *vgdev,
    uint32_t ctx_id, uint64_t offset, uint32_t level,
    uint32_t stride, uint32_t layer_stride,
    struct drm_virtgpu_3d_box *box,
    struct virtio_gpu_object_array *objs,
    struct virtio_gpu_fence *fence)
{
	/* TODO: 3D transfer - deferred */
}

void
virtio_gpu_cmd_transfer_to_host_3d(struct virtio_gpu_device *vgdev,
    uint32_t ctx_id, uint64_t offset, uint32_t level,
    uint32_t stride, uint32_t layer_stride,
    struct drm_virtgpu_3d_box *box,
    struct virtio_gpu_object_array *objs,
    struct virtio_gpu_fence *fence)
{
	/* TODO: 3D transfer - deferred */
}

void
virtio_gpu_cmd_resource_create_3d(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object *bo,
    struct virtio_gpu_object_params *params,
    struct virtio_gpu_object_array *objs,
    struct virtio_gpu_fence *fence)
{
	/* TODO: 3D resource creation - deferred */
}

void
virtio_gpu_cmd_resource_create_blob(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object *bo,
    struct virtio_gpu_object_params *params,
    struct virtio_gpu_mem_entry *ents, uint32_t nents)
{
	/* TODO: Blob resource creation - deferred */
}

void
virtio_gpu_cursor_ping(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_output *output)
{
	/* TODO: Cursor update - deferred to Phase 5 */
}
