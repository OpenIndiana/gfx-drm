/*
 * Copyright (C) 2015 Red Hat, Inc.
 * All Rights Reserved.
 *
 * Ported from Linux drivers/gpu/drm/virtio/virtgpu_object.c for illumos.
 */

#include "virtgpu_drv.h"

static atomic_t virtio_gpu_resource_seqno = {0};

int
virtio_gpu_resource_id_get(struct virtio_gpu_device *vgdev, uint32_t *resid)
{
	/*
	 * Always use monotonic IDs to avoid reuse.
	 * virglrenderer < 0.8 has bugs with ID reuse.
	 */
	int handle = atomic_inc_32_nv(
	    (volatile uint32_t *)&virtio_gpu_resource_seqno);
	*resid = (uint32_t)handle + 1;
	return (0);
}

void
virtio_gpu_cleanup_object(struct virtio_gpu_object *bo)
{
	struct drm_gem_shmem_object *shmem = &bo->base;

	if (shmem->dma_hdl != NULL) {
		(void) ddi_dma_unbind_handle(shmem->dma_hdl);
		ddi_dma_mem_free(&shmem->acc_hdl);
		ddi_dma_free_handle(&shmem->dma_hdl);
		shmem->dma_hdl = NULL;
	}

	if (bo->sgt != NULL) {
		if (bo->sgt->sgl != NULL)
			kmem_free(bo->sgt->sgl,
			    bo->sgt->nents * sizeof (struct scatterlist));
		kmem_free(bo->sgt, sizeof (struct sg_table));
		bo->sgt = NULL;
	}

	dma_resv_fini(&shmem->_resv);
	kmem_free(bo, sizeof (struct virtio_gpu_object_shmem));
}

static void
virtio_gpu_free_object(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;

	if (bo->created) {
		virtio_gpu_cmd_unref_resource(vgdev, bo);
		virtio_gpu_notify(vgdev);
		/* completion handler calls virtio_gpu_cleanup_object() */
		return;
	}
	virtio_gpu_cleanup_object(bo);
}

boolean_t
virtio_gpu_is_shmem(struct virtio_gpu_object *bo)
{
	return (B_TRUE); /* all objects are shmem on illumos for now */
}

/*
 * Build scatter-gather table from DMA cookies.
 */
static int
virtio_gpu_object_shmem_init(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object *bo,
    struct virtio_gpu_mem_entry **ents_p, unsigned int *nents_p)
{
	struct drm_gem_shmem_object *shmem = &bo->base;
	ddi_dma_cookie_t cookie;
	const ddi_dma_cookie_t *cp;
	uint_t ncookies;
	unsigned int i;
	struct virtio_gpu_mem_entry *ents;

	/* Bind to get cookies */
	if (ddi_dma_addr_bind_handle(shmem->dma_hdl, NULL,
	    shmem->vaddr, shmem->real_size,
	    DDI_DMA_RDWR | DDI_DMA_CONSISTENT,
	    DDI_DMA_SLEEP, NULL, &cookie, &ncookies) != DDI_DMA_MAPPED) {
		DRM_ERROR("virtio_gpu: ddi_dma_addr_bind_handle failed\n");
		return (-ENOMEM);
	}

	ents = kmem_zalloc(ncookies * sizeof (*ents), KM_SLEEP);

	/* First cookie */
	ents[0].addr = cpu_to_le64(cookie.dmac_laddress);
	ents[0].length = cpu_to_le32((uint32_t)cookie.dmac_size);
	ents[0].padding = 0;

	/* Remaining cookies */
	cp = NULL;
	for (i = 1; i < ncookies; i++) {
		cp = ddi_dma_cookie_iter(shmem->dma_hdl, cp);
		if (cp == NULL)
			break;
		ents[i].addr = cpu_to_le64(cp->dmac_laddress);
		ents[i].length = cpu_to_le32((uint32_t)cp->dmac_size);
		ents[i].padding = 0;
	}

	*ents_p = ents;
	*nents_p = ncookies;
	return (0);
}

int
virtio_gpu_object_create(struct virtio_gpu_device *vgdev,
    struct virtio_gpu_object_params *params,
    struct virtio_gpu_object **bo_ptr,
    struct virtio_gpu_fence *fence)
{
	struct virtio_gpu_object_shmem *shmem_wrapper;
	struct virtio_gpu_object *bo;
	struct drm_gem_shmem_object *shmem;
	struct virtio_gpu_object_array *objs = NULL;
	struct virtio_gpu_mem_entry *ents = NULL;
	unsigned int nents;
	size_t alloc_size;
	int ret;

	*bo_ptr = NULL;

	alloc_size = P2ROUNDUP(params->size, PAGESIZE);
	if (alloc_size == 0)
		alloc_size = PAGESIZE;

	/* Allocate the wrapper object */
	shmem_wrapper = kmem_zalloc(sizeof (*shmem_wrapper), KM_SLEEP);
	bo = &shmem_wrapper->base;
	shmem = &bo->base;

	/* Initialize the base GEM object minimally */
	shmem->base.dev = vgdev->ddev;
	shmem->base.size = alloc_size;
	kref_init(&shmem->base.refcount);

	/* Initialize reservation object */
	dma_resv_init(&shmem->_resv);

	/* Allocate DMA-capable backing memory */
	ret = ddi_dma_alloc_handle(vgdev->dip, &drm_gem_shmem_dma_attr,
	    DDI_DMA_SLEEP, NULL, &shmem->dma_hdl);
	if (ret != DDI_SUCCESS) {
		DRM_ERROR("virtio_gpu: ddi_dma_alloc_handle failed\n");
		ret = -ENOMEM;
		goto err_free;
	}

	{
		ddi_device_acc_attr_t acc = {
			.devacc_attr_version = DDI_DEVICE_ATTR_V1,
			.devacc_attr_endian_flags = DDI_NEVERSWAP_ACC,
			.devacc_attr_dataorder = DDI_STRICTORDER_ACC,
		};

		ret = ddi_dma_mem_alloc(shmem->dma_hdl, alloc_size, &acc,
		    DDI_DMA_CONSISTENT, DDI_DMA_SLEEP, NULL,
		    &shmem->vaddr, &shmem->real_size, &shmem->acc_hdl);
	}
	if (ret != DDI_SUCCESS) {
		DRM_ERROR("virtio_gpu: ddi_dma_mem_alloc failed (%lu bytes)\n",
		    (unsigned long)alloc_size);
		ddi_dma_free_handle(&shmem->dma_hdl);
		shmem->dma_hdl = NULL;
		ret = -ENOMEM;
		goto err_free;
	}

	/* Zero the allocation */
	bzero(shmem->vaddr, shmem->real_size);

	/* Get resource ID */
	ret = virtio_gpu_resource_id_get(vgdev, &bo->hw_res_handle);
	if (ret < 0)
		goto err_free_dma;

	bo->dumb = params->dumb;

	/* Build scatter-gather entries from DMA cookies */
	ret = virtio_gpu_object_shmem_init(vgdev, bo, &ents, &nents);
	if (ret != 0)
		goto err_free_dma;

	/* Set up fence if requested */
	if (fence != NULL) {
		objs = virtio_gpu_array_alloc(1);
		if (objs == NULL) {
			ret = -ENOMEM;
			goto err_free_ents;
		}
		virtio_gpu_array_add_obj(objs, &bo->base.base);

		ret = virtio_gpu_array_lock_resv(objs);
		if (ret != 0) {
			virtio_gpu_array_put_free(objs);
			goto err_free_ents;
		}
	}

	/* Send resource creation command to host */
	if (params->virgl) {
		virtio_gpu_cmd_resource_create_3d(vgdev, bo, params,
		    objs, fence);
		virtio_gpu_object_attach(vgdev, bo, ents, nents);
	} else {
		virtio_gpu_cmd_create_resource(vgdev, bo, params,
		    objs, fence);
		virtio_gpu_object_attach(vgdev, bo, ents, nents);
	}

	*bo_ptr = bo;
	return (0);

err_free_ents:
	kmem_free(ents, nents * sizeof (*ents));
err_free_dma:
	(void) ddi_dma_unbind_handle(shmem->dma_hdl);
	ddi_dma_mem_free(&shmem->acc_hdl);
	ddi_dma_free_handle(&shmem->dma_hdl);
	shmem->dma_hdl = NULL;
err_free:
	dma_resv_fini(&shmem->_resv);
	kmem_free(shmem_wrapper, sizeof (*shmem_wrapper));
	return (ret);
}
