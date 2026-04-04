/*
 * Copyright (C) 2015 Red Hat, Inc.
 * All Rights Reserved.
 *
 * Ported from Linux drivers/gpu/drm/virtio/virtgpu_ioctl.c for illumos.
 *
 * Ioctl handlers use illumos ddi_copyin/ddi_copyout instead of
 * Linux copy_from_user/copy_to_user.  The ioctl table uses the
 * gfx-drm DRM_IOCTL_DEF format with copyin32/copyout32 set to NULL.
 */

#include "virtgpu_drv.h"

static int
virtio_gpu_map_ioctl(DRM_IOCTL_ARGS)
{
	struct drm_virtgpu_map *args = data;

	/*
	 * Map offset for userspace mmap.
	 * For initial bring-up, return the GEM object's fake offset.
	 * Full devmap support comes with Phase 5.
	 */
	args->offset = (uint64_t)args->handle << DRM_PAGE_SHIFT;
	return (0);
}

static int
virtio_gpu_getparam_ioctl(DRM_IOCTL_ARGS)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct drm_virtgpu_getparam *param = data;
	int value;

	switch (param->param) {
	case VIRTGPU_PARAM_3D_FEATURES:
		value = vgdev->has_virgl_3d ? 1 : 0;
		break;
	case VIRTGPU_PARAM_CAPSET_QUERY_FIX:
		value = 1;
		break;
	case VIRTGPU_PARAM_RESOURCE_BLOB:
		value = vgdev->has_resource_blob ? 1 : 0;
		break;
	case VIRTGPU_PARAM_HOST_VISIBLE:
		value = vgdev->has_host_visible ? 1 : 0;
		break;
	case VIRTGPU_PARAM_CROSS_DEVICE:
		value = vgdev->has_resource_assign_uuid ? 1 : 0;
		break;
	case VIRTGPU_PARAM_CONTEXT_INIT:
		value = vgdev->has_context_init ? 1 : 0;
		break;
	case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs:
		value = (int)vgdev->capset_id_mask;
		break;
	case VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME:
		value = vgdev->has_context_init ? 1 : 0;
		break;
	default:
		return (-EINVAL);
	}

	if (ddi_copyout(&value, (void *)(uintptr_t)param->value,
	    sizeof (int), 0) != 0)
		return (-EFAULT);

	return (0);
}

static int
virtio_gpu_resource_create_ioctl(DRM_IOCTL_ARGS)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct drm_virtgpu_resource_create *rc = data;
	struct virtio_gpu_object_params params;
	struct virtio_gpu_fence *fence;
	struct virtio_gpu_object *qobj;
	uint32_t handle = 0;
	int ret;

	bzero(&params, sizeof (params));

	if (vgdev->has_virgl_3d) {
		params.virgl = B_TRUE;
		params.target = rc->target;
		params.bind = rc->bind;
		params.depth = rc->depth;
		params.array_size = rc->array_size;
		params.last_level = rc->last_level;
		params.nr_samples = rc->nr_samples;
		params.flags = rc->flags;
	} else {
		if (rc->depth > 1 || rc->nr_samples > 1 ||
		    rc->last_level > 1 || rc->target != 2 ||
		    rc->array_size > 1)
			return (-EINVAL);
	}

	params.format = rc->format;
	params.width = rc->width;
	params.height = rc->height;
	params.size = rc->size;
	if (params.size == 0)
		params.size = PAGESIZE;

	fence = virtio_gpu_fence_alloc(vgdev, vgdev->fence_drv.context, 0);
	if (fence == NULL)
		return (-ENOMEM);

	ret = virtio_gpu_object_create(vgdev, &params, &qobj, fence);
	dma_fence_put(&fence->f);
	if (ret < 0)
		return (ret);

	ret = drm_gem_handle_create(file, &qobj->base.base, &handle);
	if (ret != 0) {
		virtio_gpu_cleanup_object(qobj);
		return (ret);
	}

	rc->res_handle = qobj->hw_res_handle;
	rc->bo_handle = handle;

	drm_gem_object_put(&qobj->base.base);
	return (0);
}

static int
virtio_gpu_resource_info_ioctl(DRM_IOCTL_ARGS)
{
	struct drm_virtgpu_resource_info *ri = data;
	struct drm_gem_object *gobj;
	struct virtio_gpu_object *qobj;

	gobj = drm_gem_object_lookup(file, ri->bo_handle);
	if (gobj == NULL)
		return (-ENOENT);

	qobj = gem_to_virtio_gpu_obj(gobj);
	ri->size = (uint32_t)qobj->base.base.size;
	ri->res_handle = qobj->hw_res_handle;
	if (qobj->host3d_blob || qobj->guest_blob)
		ri->blob_mem = qobj->blob_mem;

	drm_gem_object_put(gobj);
	return (0);
}

static int
virtio_gpu_transfer_from_host_ioctl(DRM_IOCTL_ARGS)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;

	if (!vgdev->has_virgl_3d)
		return (-ENOSYS);

	/* Full 3D transfer implementation deferred */
	return (-ENOSYS);
}

static int
virtio_gpu_transfer_to_host_ioctl(DRM_IOCTL_ARGS)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct drm_virtgpu_3d_transfer_to_host *args = data;
	struct virtio_gpu_object *bo;
	struct virtio_gpu_object_array *objs;

	objs = virtio_gpu_array_from_handles(file, &args->bo_handle, 1);
	if (objs == NULL)
		return (-ENOENT);

	bo = gem_to_virtio_gpu_obj(objs->objs[0]);

	if (!vgdev->has_virgl_3d) {
		virtio_gpu_cmd_transfer_to_host_2d(vgdev,
		    args->offset, args->box.w, args->box.h,
		    args->box.x, args->box.y, objs, NULL);
	} else {
		/* 3D path deferred */
		virtio_gpu_array_put_free(objs);
		return (-ENOSYS);
	}

	virtio_gpu_notify(vgdev);
	return (0);
}

static int
virtio_gpu_wait_ioctl(DRM_IOCTL_ARGS)
{
	struct drm_virtgpu_3d_wait *args = data;
	struct drm_gem_object *gobj;
	struct virtio_gpu_object *bo;
	int ret;

	gobj = drm_gem_object_lookup(file, args->handle);
	if (gobj == NULL)
		return (-ENOENT);

	bo = gem_to_virtio_gpu_obj(gobj);

	if (args->flags & VIRTGPU_WAIT_NOWAIT) {
		ret = dma_resv_test_signaled(&bo->base._resv,
		    DMA_RESV_USAGE_READ);
	} else {
		ret = dma_resv_wait_timeout(&bo->base._resv,
		    DMA_RESV_USAGE_READ, B_TRUE,
		    drv_usectohz(15000000)); /* 15 seconds */
	}

	if (ret == 0)
		ret = -EBUSY;
	else if (ret > 0)
		ret = 0;

	drm_gem_object_put(gobj);
	return (ret);
}

static int
virtio_gpu_get_caps_ioctl(DRM_IOCTL_ARGS)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct drm_virtgpu_get_caps *args = data;
	unsigned int size, host_caps_size;
	int i, found_valid = -1;
	int ret;
	struct virtio_gpu_drv_cap_cache *cache_ent;

	if (vgdev->num_capsets == 0)
		return (-ENOSYS);
	if (args->size == 0)
		return (-EINVAL);

	mutex_enter(&vgdev->display_info_lock);
	for (i = 0; i < (int)vgdev->num_capsets; i++) {
		if (vgdev->capsets[i].id == args->cap_set_id &&
		    vgdev->capsets[i].max_version >= args->cap_set_ver) {
			found_valid = i;
			break;
		}
	}

	if (found_valid == -1) {
		mutex_exit(&vgdev->display_info_lock);
		return (-EINVAL);
	}

	host_caps_size = vgdev->capsets[found_valid].max_size;
	size = min(args->size, host_caps_size);

	/* Check cache */
	list_for_each_entry(cache_ent, struct virtio_gpu_drv_cap_cache,
	    &vgdev->cap_cache, head) {
		if (cache_ent->id == args->cap_set_id &&
		    cache_ent->version == args->cap_set_ver) {
			mutex_exit(&vgdev->display_info_lock);
			goto copy_exit;
		}
	}
	mutex_exit(&vgdev->display_info_lock);

	/* Not in cache, query from device */
	ret = virtio_gpu_cmd_get_capset(vgdev, found_valid,
	    args->cap_set_ver, &cache_ent);
	if (ret != 0)
		return (ret);
	virtio_gpu_notify(vgdev);

copy_exit:
	ret = wait_event_timeout(vgdev->resp_wq,
	    atomic_read(&cache_ent->is_valid),
	    drv_usectohz(5000000));
	if (ret == 0)
		return (-EBUSY);

	smp_rmb();

	if (ddi_copyout(cache_ent->caps_cache,
	    (void *)(uintptr_t)args->addr, size, 0) != 0)
		return (-EFAULT);

	return (0);
}

static int
virtio_gpu_resource_create_blob_ioctl(DRM_IOCTL_ARGS)
{
	/* Blob resource creation deferred */
	return (-ENOSYS);
}

static int
virtio_gpu_context_init_ioctl(DRM_IOCTL_ARGS)
{
	/* Context init deferred */
	return (-ENOSYS);
}

/*
 * Ioctl table using the gfx-drm DRM_IOCTL_DEF format.
 * copyin32/copyout32 are NULL (64-bit only for now).
 */
drm_ioctl_desc_t virtio_gpu_ioctls[DRM_VIRTIO_NUM_IOCTLS] = {
	DRM_IOCTL_DEF(DRM_IOCTL_VIRTGPU_MAP,
	    virtio_gpu_map_ioctl, DRM_AUTH, NULL, NULL),
	DRM_IOCTL_DEF(DRM_IOCTL_VIRTGPU_EXECBUFFER,
	    virtio_gpu_execbuffer_ioctl, DRM_AUTH, NULL, NULL),
	DRM_IOCTL_DEF(DRM_IOCTL_VIRTGPU_GETPARAM,
	    virtio_gpu_getparam_ioctl, DRM_AUTH, NULL, NULL),
	DRM_IOCTL_DEF(DRM_IOCTL_VIRTGPU_RESOURCE_CREATE,
	    virtio_gpu_resource_create_ioctl, DRM_AUTH, NULL, NULL),
	DRM_IOCTL_DEF(DRM_IOCTL_VIRTGPU_RESOURCE_INFO,
	    virtio_gpu_resource_info_ioctl, DRM_AUTH, NULL, NULL),
	DRM_IOCTL_DEF(DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST,
	    virtio_gpu_transfer_from_host_ioctl, DRM_AUTH, NULL, NULL),
	DRM_IOCTL_DEF(DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST,
	    virtio_gpu_transfer_to_host_ioctl, DRM_AUTH, NULL, NULL),
	DRM_IOCTL_DEF(DRM_IOCTL_VIRTGPU_WAIT,
	    virtio_gpu_wait_ioctl, DRM_AUTH, NULL, NULL),
	DRM_IOCTL_DEF(DRM_IOCTL_VIRTGPU_GET_CAPS,
	    virtio_gpu_get_caps_ioctl, DRM_AUTH, NULL, NULL),
	DRM_IOCTL_DEF(DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB,
	    virtio_gpu_resource_create_blob_ioctl, DRM_AUTH, NULL, NULL),
	DRM_IOCTL_DEF(DRM_IOCTL_VIRTGPU_CONTEXT_INIT,
	    virtio_gpu_context_init_ioctl, DRM_AUTH, NULL, NULL),
};
