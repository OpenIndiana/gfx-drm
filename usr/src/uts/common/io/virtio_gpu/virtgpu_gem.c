/*
 * Copyright (C) 2015 Red Hat, Inc.
 * All Rights Reserved.
 *
 * Ported from Linux drivers/gpu/drm/virtio/virtgpu_gem.c for illumos.
 */

#include "virtgpu_drv.h"

static int
virtio_gpu_gem_create(struct drm_file *file, struct drm_device *dev,
    struct virtio_gpu_object_params *params,
    struct drm_gem_object **obj_p, uint32_t *handle_p)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_object *obj;
	uint32_t handle;
	int ret;

	ret = virtio_gpu_object_create(vgdev, params, &obj, NULL);
	if (ret < 0)
		return (ret);

	ret = drm_gem_handle_create(file, &obj->base.base, &handle);
	if (ret != 0) {
		virtio_gpu_cleanup_object(obj);
		return (ret);
	}

	*obj_p = &obj->base.base;
	/* handle holds the reference now */
	drm_gem_object_put(&obj->base.base);
	*handle_p = handle;
	return (0);
}

int
virtio_gpu_mode_dumb_create(struct drm_file *file_priv,
    struct drm_device *dev, struct drm_mode_create_dumb *args)
{
	struct drm_gem_object *gobj;
	struct virtio_gpu_object_params params;
	uint32_t pitch;
	int ret;

	if (args->bpp != 32)
		return (-EINVAL);

	pitch = args->width * 4;
	args->size = pitch * args->height;
	args->size = P2ROUNDUP(args->size, PAGESIZE);

	bzero(&params, sizeof (params));
	params.format = VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM;
	params.width = args->width;
	params.height = args->height;
	params.size = args->size;
	params.dumb = B_TRUE;

	ret = virtio_gpu_gem_create(file_priv, dev, &params, &gobj,
	    &args->handle);
	if (ret != 0)
		return (ret);

	args->pitch = pitch;
	return (0);
}

int
virtio_gpu_gem_object_open(struct drm_gem_object *obj, struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	struct virtio_gpu_fpriv *vfpriv = file->driver_priv;
	struct virtio_gpu_object_array *objs;

	if (!vgdev->has_virgl_3d)
		goto out_notify;

	if (vfpriv != NULL && vfpriv->context_created) {
		objs = virtio_gpu_array_alloc(1);
		if (objs == NULL)
			return (-ENOMEM);
		virtio_gpu_array_add_obj(objs, obj);
		virtio_gpu_cmd_context_attach_resource(vgdev,
		    vfpriv->ctx_id, objs);
	}

out_notify:
	virtio_gpu_notify(vgdev);
	return (0);
}

void
virtio_gpu_gem_object_close(struct drm_gem_object *obj, struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	struct virtio_gpu_fpriv *vfpriv = file->driver_priv;
	struct virtio_gpu_object_array *objs;

	if (!vgdev->has_virgl_3d || vfpriv == NULL)
		return;

	objs = virtio_gpu_array_alloc(1);
	if (objs == NULL)
		return;
	virtio_gpu_array_add_obj(objs, obj);
	virtio_gpu_cmd_context_detach_resource(vgdev, vfpriv->ctx_id, objs);
	virtio_gpu_notify(vgdev);
}

/* ---- Object array management ---- */

struct virtio_gpu_object_array *
virtio_gpu_array_alloc(uint32_t nents)
{
	struct virtio_gpu_object_array *objs;
	size_t sz;

	sz = sizeof (*objs) + nents * sizeof (struct drm_gem_object *);
	objs = kmem_zalloc(sz, KM_SLEEP);
	objs->nents = 0;
	objs->total = nents;
	return (objs);
}

static void
virtio_gpu_array_free(struct virtio_gpu_object_array *objs)
{
	size_t sz;

	sz = sizeof (*objs) + objs->total * sizeof (struct drm_gem_object *);
	kmem_free(objs, sz);
}

struct virtio_gpu_object_array *
virtio_gpu_array_from_handles(struct drm_file *drm_file,
    uint32_t *handles, uint32_t nents)
{
	struct virtio_gpu_object_array *objs;
	uint32_t i;

	objs = virtio_gpu_array_alloc(nents);
	if (objs == NULL)
		return (NULL);

	for (i = 0; i < nents; i++) {
		objs->objs[i] = drm_gem_object_lookup(drm_file, handles[i]);
		if (objs->objs[i] == NULL) {
			objs->nents = i;
			virtio_gpu_array_put_free(objs);
			return (NULL);
		}
	}
	objs->nents = nents;
	return (objs);
}

void
virtio_gpu_array_add_obj(struct virtio_gpu_object_array *objs,
    struct drm_gem_object *obj)
{
	if (objs->nents >= objs->total) {
		WARN_ON(1);
		return;
	}
	drm_gem_object_get(obj);
	objs->objs[objs->nents] = obj;
	objs->nents++;
}

int
virtio_gpu_array_lock_resv(struct virtio_gpu_object_array *objs)
{
	struct virtio_gpu_object *bo;
	unsigned int i;
	int ret;

	if (objs->nents == 1) {
		bo = gem_to_virtio_gpu_obj(objs->objs[0]);
		ret = dma_resv_lock_interruptible(&bo->base._resv, NULL);
	} else {
		/*
		 * Multi-object locking would use ww_mutex acquire context.
		 * For simplicity, lock each individually for now.
		 */
		for (i = 0; i < objs->nents; i++) {
			bo = gem_to_virtio_gpu_obj(objs->objs[i]);
			ret = dma_resv_lock(&bo->base._resv, NULL);
			if (ret != 0) {
				/* Unlock already-locked objects */
				while (i-- > 0) {
					bo = gem_to_virtio_gpu_obj(
					    objs->objs[i]);
					dma_resv_unlock(&bo->base._resv);
				}
				return (ret);
			}
		}
		ret = 0;
	}

	if (ret != 0)
		return (ret);

	for (i = 0; i < objs->nents; i++) {
		bo = gem_to_virtio_gpu_obj(objs->objs[i]);
		ret = dma_resv_reserve_fences(&bo->base._resv, 1);
		if (ret != 0) {
			virtio_gpu_array_unlock_resv(objs);
			return (ret);
		}
	}

	return (0);
}

void
virtio_gpu_array_unlock_resv(struct virtio_gpu_object_array *objs)
{
	unsigned int i;
	struct virtio_gpu_object *bo;

	for (i = 0; i < objs->nents; i++) {
		bo = gem_to_virtio_gpu_obj(objs->objs[i]);
		dma_resv_unlock(&bo->base._resv);
	}
}

void
virtio_gpu_array_add_fence(struct virtio_gpu_object_array *objs,
    dma_fence_t *fence)
{
	unsigned int i;
	struct virtio_gpu_object *bo;

	for (i = 0; i < objs->nents; i++) {
		bo = gem_to_virtio_gpu_obj(objs->objs[i]);
		dma_resv_add_fence(&bo->base._resv, fence,
		    DMA_RESV_USAGE_WRITE);
	}
}

void
virtio_gpu_array_put_free(struct virtio_gpu_object_array *objs)
{
	uint32_t i;

	if (objs == NULL)
		return;

	for (i = 0; i < objs->nents; i++)
		drm_gem_object_put(objs->objs[i]);
	virtio_gpu_array_free(objs);
}

void
virtio_gpu_array_put_free_delayed(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object_array *objs)
{
	mutex_enter(&vgdev->obj_free_lock);
	list_add_tail(&objs->next, &vgdev->obj_free_list, objs);
	mutex_exit(&vgdev->obj_free_lock);
	schedule_work(&vgdev->obj_free_work);
}

void
virtio_gpu_array_put_free_work(struct work_struct *work)
{
	struct virtio_gpu_device *vgdev =
	    container_of(work, struct virtio_gpu_device, obj_free_work);
	struct virtio_gpu_object_array *objs;

	mutex_enter(&vgdev->obj_free_lock);
	while (!list_empty(&vgdev->obj_free_list)) {
		objs = list_first_entry(&vgdev->obj_free_list,
		    struct virtio_gpu_object_array, next);
		list_del(&objs->next);
		mutex_exit(&vgdev->obj_free_lock);
		virtio_gpu_array_put_free(objs);
		mutex_enter(&vgdev->obj_free_lock);
	}
	mutex_exit(&vgdev->obj_free_lock);
}
