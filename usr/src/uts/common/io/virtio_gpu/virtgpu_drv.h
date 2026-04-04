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
 * Ported from Linux drivers/gpu/drm/virtio/virtgpu_drv.h for illumos.
 *
 * Key changes from Linux:
 *   - struct virtio_device *vdev  -> virtio_t *vio + dev_info_t *dip
 *   - struct virtqueue *vq        -> virtio_queue_t *vq
 *   - struct kmem_cache           -> kmem_cache_t (same API on illumos)
 *   - spinlock_t                  -> kmutex_t
 *   - struct ida                  -> simple atomic counter
 *   - wait_queue_head_t           -> drm_wait_queue (from drmP.h)
 *   - struct work_struct          -> drm_sun_workqueue work_struct
 *   - Display structures deferred (Phase 5)
 */

#ifndef	VIRTIO_DRV_H
#define	VIRTIO_DRV_H

#include "drmP.h"
#include "drm_sun_print.h"
#include "drm_sun_gem_modern.h"
#include "virtio_gpu_hw.h"
#include "virtgpu_drm.h"

/* illumos virtio framework */
#include <sys/virtio/virtio.h>

#define	DRIVER_NAME	"virtio_gpu"
#define	DRIVER_DESC	"virtio GPU"

#define	DRIVER_MAJOR		0
#define	DRIVER_MINOR		1
#define	DRIVER_PATCHLEVEL	0

#define	STATE_INITIALIZING	0
#define	STATE_OK		1
#define	STATE_ERR		2

#define	MAX_CAPSET_ID		63
#define	MAX_RINGS		64

#define	DEBUG_NAME_MAX_LEN	65

/* ---- Object parameters ---- */

struct virtio_gpu_object_params {
	unsigned long	size;
	boolean_t	dumb;
	boolean_t	virgl;
	boolean_t	blob;

	/* classic resources */
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t target;
	uint32_t bind;
	uint32_t depth;
	uint32_t array_size;
	uint32_t last_level;
	uint32_t nr_samples;
	uint32_t flags;

	/* blob resources */
	uint32_t ctx_id;
	uint32_t blob_mem;
	uint32_t blob_flags;
	uint64_t blob_id;
};

/* ---- GEM objects ---- */

/*
 * illumos GEM shmem object.  Replaces Linux drm_gem_shmem_object.
 * Backing memory is allocated via ddi_dma_mem_alloc (Phase 2).
 */
struct drm_gem_shmem_object {
	struct drm_gem_object	base;
	struct dma_resv		_resv;

	/* DMA backing (populated in Phase 2) */
	ddi_dma_handle_t	dma_hdl;
	ddi_acc_handle_t	acc_hdl;
	caddr_t			vaddr;
	size_t			real_size;

	unsigned int		pages_use_count;
	unsigned int		pages_pin_count;
};

#define	to_drm_gem_shmem_obj(obj) \
	container_of((obj), struct drm_gem_shmem_object, base)

/*
 * Minimal scatter-gather table for virtio-gpu.
 * Built from DDI DMA cookies when backing pages are pinned.
 */
struct scatterlist {
	uint64_t	dma_address;
	uint32_t	length;
	uint32_t	_pad;
};

struct sg_table {
	struct scatterlist	*sgl;
	unsigned int		nents;
	unsigned int		orig_nents;
};

struct virtio_gpu_object {
	struct drm_gem_shmem_object base;
	struct sg_table		*sgt;
	uint32_t		hw_res_handle;
	boolean_t		dumb;
	boolean_t		created;
	boolean_t		attached;
	boolean_t		host3d_blob;
	boolean_t		guest_blob;
	uint32_t		blob_mem;
	uint32_t		blob_flags;

	int			uuid_state;
	uint8_t			uuid[16];
};

#define	gem_to_virtio_gpu_obj(gobj) \
	container_of((gobj), struct virtio_gpu_object, base.base)

struct virtio_gpu_object_shmem {
	struct virtio_gpu_object base;
};

#define	to_virtio_gpu_shmem(obj) \
	container_of((obj), struct virtio_gpu_object_shmem, base)

/* ---- Object arrays (multi-BO locking) ---- */

struct virtio_gpu_object_array {
	struct ww_acquire_ctx	ticket;
	struct list_head	next;
	uint32_t		nents;
	uint32_t		total;
	struct drm_gem_object	*objs[];
};

/* ---- Virtqueue buffers ---- */

struct virtio_gpu_vbuffer;
struct virtio_gpu_device;

typedef void (*virtio_gpu_resp_cb)(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_vbuffer *vbuf);

struct virtio_gpu_vbuffer {
	char		*buf;		/* command buffer (inline after vbuf) */
	int		size;		/* command size */

	void		*data_buf;	/* optional data payload (caller-owned) */
	uint32_t	data_size;	/* data payload size */

	char		*resp_buf;	/* response buffer (inline or external) */
	int		resp_size;	/* response size */
	virtio_gpu_resp_cb resp_cb;	/* response callback */
	void		*resp_cb_data;	/* callback private data */

	struct virtio_gpu_object_array *objs;
	struct list_head list;

	uint32_t	seqno;

	/*
	 * illumos DMA backing.  Each vbuf gets a DMA allocation so we
	 * know the physical address for virtio_chain_append().
	 * vdma backs the inline cmd+resp region.
	 * data_dma backs the optional data payload (allocated at submit).
	 */
	virtio_dma_t	*vdma;		/* DMA for cmd+resp */
	virtio_dma_t	*data_dma;	/* DMA for data payload (if any) */
};

/* ---- Fence driver ---- */

struct virtio_gpu_fence_driver {
	atomic64_t	last_fence_id;
	uint64_t	current_fence_id;
	uint64_t	context;
	struct list_head fences;
	kmutex_t	lock;
};

struct virtio_gpu_fence {
	dma_fence_t		f;
	uint32_t		ring_idx;
	uint64_t		fence_id;
	boolean_t		emit_fence_info;
	struct virtio_gpu_fence_driver *drv;
	struct list_head	node;
};

/* ---- Virtqueue ---- */

struct virtio_gpu_queue {
	virtio_queue_t		*vq;		/* illumos virtqueue */
	kmutex_t		qlock;
	wait_queue_head_t	ack_queue;
	struct work_struct	dequeue_work;
	uint32_t		seqno;
};

/* ---- Capability sets ---- */

struct virtio_gpu_drv_capset {
	uint32_t id;
	uint32_t max_version;
	uint32_t max_size;
};

struct virtio_gpu_drv_cap_cache {
	struct list_head head;
	void		*caps_cache;
	uint32_t	id;
	uint32_t	version;
	uint32_t	size;
	atomic_t	is_valid;
};

/* ---- Display output (minimal for headless, expanded in Phase 5) ---- */

struct virtio_gpu_output {
	int			index;
	struct drm_crtc		crtc;
	struct drm_connector	conn;
	struct drm_encoder	enc;
	struct virtio_gpu_display_one info;
	struct virtio_gpu_update_cursor cursor;
	int			cur_x;
	int			cur_y;
	boolean_t		needs_modeset;
};

#define	drm_crtc_to_virtio_gpu_output(x) \
	container_of(x, struct virtio_gpu_output, crtc)

/* ---- Main device structure ---- */

struct virtio_gpu_device {
	struct drm_device	*ddev;

	/* illumos virtio framework handle */
	virtio_t		*vio;
	dev_info_t		*dip;

	struct virtio_gpu_output outputs[VIRTIO_GPU_MAX_SCANOUTS];
	uint32_t		num_scanouts;

	struct virtio_gpu_queue	ctrlq;
	struct virtio_gpu_queue	cursorq;
	kmem_cache_t		*vbufs;

	atomic_t		pending_commands;

	/* Resource ID allocator (simple atomic counter) */
	atomic_t		resource_id_counter;

	wait_queue_head_t	resp_wq;
	kmutex_t		display_info_lock;
	boolean_t		display_info_pending;

	struct virtio_gpu_fence_driver fence_drv;

	/* Context ID allocator */
	atomic_t		ctx_id_counter;

	boolean_t		has_virgl_3d;
	boolean_t		has_edid;
	boolean_t		has_indirect;
	boolean_t		has_resource_assign_uuid;
	boolean_t		has_resource_blob;
	boolean_t		has_host_visible;
	boolean_t		has_context_init;

	struct work_struct	config_changed_work;

	struct work_struct	obj_free_work;
	kmutex_t		obj_free_lock;
	struct list_head	obj_free_list;

	struct virtio_gpu_drv_capset *capsets;
	uint32_t		num_capsets;
	uint64_t		capset_id_mask;
	struct list_head	cap_cache;

	kmutex_t		resource_export_lock;
	kmutex_t		host_visible_lock;
};

/* ---- Per-file private data ---- */

struct virtio_gpu_fpriv {
	uint32_t	ctx_id;
	uint32_t	context_init;
	boolean_t	context_created;
	uint32_t	num_rings;
	uint64_t	base_fence_ctx;
	uint64_t	ring_idx_mask;
	kmutex_t	context_lock;
	char		debug_name[DEBUG_NAME_MAX_LEN];
	boolean_t	explicit_debug_name;
};

/* ---- Function declarations ---- */

/* virtgpu_ioctl.c */
#define	DRM_VIRTIO_NUM_IOCTLS	12
extern drm_ioctl_desc_t virtio_gpu_ioctls[DRM_VIRTIO_NUM_IOCTLS];

/* virtgpu_kms.c */
int  virtio_gpu_init(struct virtio_gpu_device *vgdev);
void virtio_gpu_deinit(struct virtio_gpu_device *vgdev);
int  virtio_gpu_driver_open(struct drm_device *dev, struct drm_file *file);
void virtio_gpu_driver_postclose(struct drm_device *dev, struct drm_file *file);

/* virtgpu_gem.c */
int  virtio_gpu_mode_dumb_create(struct drm_file *file_priv,
    struct drm_device *dev, struct drm_mode_create_dumb *args);
struct virtio_gpu_object_array *virtio_gpu_array_alloc(uint32_t nents);
struct virtio_gpu_object_array *
    virtio_gpu_array_from_handles(struct drm_file *drm_file,
    uint32_t *handles, uint32_t nents);
void virtio_gpu_array_add_obj(struct virtio_gpu_object_array *objs,
    struct drm_gem_object *obj);
int  virtio_gpu_array_lock_resv(struct virtio_gpu_object_array *objs);
void virtio_gpu_array_unlock_resv(struct virtio_gpu_object_array *objs);
void virtio_gpu_array_add_fence(struct virtio_gpu_object_array *objs,
    dma_fence_t *fence);
void virtio_gpu_array_put_free(struct virtio_gpu_object_array *objs);
void virtio_gpu_array_put_free_delayed(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object_array *objs);
void virtio_gpu_array_put_free_work(struct work_struct *work);

/* virtgpu_vq.c */
int  virtio_gpu_alloc_vbufs(struct virtio_gpu_device *vgdev);
void virtio_gpu_free_vbufs(struct virtio_gpu_device *vgdev);
void virtio_gpu_cmd_create_resource(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object *bo,
    struct virtio_gpu_object_params *params,
    struct virtio_gpu_object_array *objs,
    struct virtio_gpu_fence *fence);
void virtio_gpu_cmd_unref_resource(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object *bo);
void virtio_gpu_cmd_transfer_to_host_2d(struct virtio_gpu_device *vgdev,
    uint64_t offset, uint32_t width, uint32_t height,
    uint32_t x, uint32_t y,
    struct virtio_gpu_object_array *objs,
    struct virtio_gpu_fence *fence);
void virtio_gpu_cmd_resource_flush(struct virtio_gpu_device *vgdev,
    uint32_t resource_id, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height,
    struct virtio_gpu_object_array *objs,
    struct virtio_gpu_fence *fence);
void virtio_gpu_cmd_set_scanout(struct virtio_gpu_device *vgdev,
    uint32_t scanout_id, uint32_t resource_id,
    uint32_t width, uint32_t height, uint32_t x, uint32_t y);
void virtio_gpu_object_attach(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object *obj,
    struct virtio_gpu_mem_entry *ents, unsigned int nents);
void virtio_gpu_object_detach(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object *obj, struct virtio_gpu_fence *fence);
void virtio_gpu_cursor_ping(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_output *output);
int  virtio_gpu_cmd_get_display_info(struct virtio_gpu_device *vgdev);
int  virtio_gpu_cmd_get_capset_info(struct virtio_gpu_device *vgdev, int idx);
int  virtio_gpu_cmd_get_capset(struct virtio_gpu_device *vgdev,
    int idx, int version, struct virtio_gpu_drv_cap_cache **cache_p);
int  virtio_gpu_cmd_get_edids(struct virtio_gpu_device *vgdev);
void virtio_gpu_cmd_context_create(struct virtio_gpu_device *vgdev,
    uint32_t id, uint32_t context_init, uint32_t nlen, const char *name);
void virtio_gpu_cmd_context_destroy(struct virtio_gpu_device *vgdev,
    uint32_t id);
void virtio_gpu_cmd_context_attach_resource(struct virtio_gpu_device *vgdev,
    uint32_t ctx_id, struct virtio_gpu_object_array *objs);
void virtio_gpu_cmd_context_detach_resource(struct virtio_gpu_device *vgdev,
    uint32_t ctx_id, struct virtio_gpu_object_array *objs);
void virtio_gpu_cmd_submit(struct virtio_gpu_device *vgdev,
    void *data, uint32_t data_size, uint32_t ctx_id,
    struct virtio_gpu_object_array *objs,
    struct virtio_gpu_fence *fence);
void virtio_gpu_cmd_transfer_from_host_3d(struct virtio_gpu_device *vgdev,
    uint32_t ctx_id, uint64_t offset, uint32_t level,
    uint32_t stride, uint32_t layer_stride,
    struct drm_virtgpu_3d_box *box,
    struct virtio_gpu_object_array *objs,
    struct virtio_gpu_fence *fence);
void virtio_gpu_cmd_transfer_to_host_3d(struct virtio_gpu_device *vgdev,
    uint32_t ctx_id, uint64_t offset, uint32_t level,
    uint32_t stride, uint32_t layer_stride,
    struct drm_virtgpu_3d_box *box,
    struct virtio_gpu_object_array *objs,
    struct virtio_gpu_fence *fence);
void virtio_gpu_cmd_resource_create_3d(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object *bo,
    struct virtio_gpu_object_params *params,
    struct virtio_gpu_object_array *objs,
    struct virtio_gpu_fence *fence);
void virtio_gpu_dequeue_ctrl_func(struct work_struct *work);
void virtio_gpu_dequeue_cursor_func(struct work_struct *work);
void virtio_gpu_notify(struct virtio_gpu_device *vgdev);

void virtio_gpu_cmd_resource_create_blob(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object *bo,
    struct virtio_gpu_object_params *params,
    struct virtio_gpu_mem_entry *ents, uint32_t nents);

/* virtgpu_fence.c */
struct virtio_gpu_fence *virtio_gpu_fence_alloc(
    struct virtio_gpu_device *vgdev,
    uint64_t base_fence_ctx, uint32_t ring_idx);
void virtio_gpu_fence_emit(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_ctrl_hdr *cmd_hdr,
    struct virtio_gpu_fence *fence);
void virtio_gpu_fence_event_process(struct virtio_gpu_device *vgdev,
    uint64_t fence_id);

/* virtgpu_object.c */
void virtio_gpu_cleanup_object(struct virtio_gpu_object *bo);
int  virtio_gpu_object_create(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object_params *params,
    struct virtio_gpu_object **bo_ptr,
    struct virtio_gpu_fence *fence);
boolean_t virtio_gpu_is_shmem(struct virtio_gpu_object *bo);
int  virtio_gpu_resource_id_get(struct virtio_gpu_device *vgdev,
    uint32_t *resid);

/* virtgpu_debugfs.c */
void virtio_gpu_debugfs_init(struct drm_minor *minor);

/* virtgpu_submit.c */
int  virtio_gpu_execbuffer_ioctl(struct drm_device *dev, void *data,
    struct drm_file *file);

/* virtgpu_display.c (stub for now) */
int  virtio_gpu_modeset_init(struct virtio_gpu_device *vgdev);
void virtio_gpu_modeset_fini(struct virtio_gpu_device *vgdev);

/*
 * Interrupt handlers -- registered with virtio_queue_alloc().
 * These are the illumos equivalents of virtio_gpu_ctrl_ack / cursor_ack.
 */
uint_t virtgpu_ctrl_intr(caddr_t arg1, caddr_t arg2);
uint_t virtgpu_cursor_intr(caddr_t arg1, caddr_t arg2);

#endif /* VIRTIO_DRV_H */
