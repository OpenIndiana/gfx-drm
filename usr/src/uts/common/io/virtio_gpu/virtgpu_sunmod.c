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

/*
 * virtio-gpu DRM driver DDI entry point for illumos.
 *
 * This is the module's _init/_fini/_info entry point and the
 * DDI attach/detach lifecycle.  It follows the gfx-drm pattern
 * established by the i915 driver port.
 */

#include "drm_sunmod.h"
#include "virtgpu_drv.h"

/* ---- Forward declarations ---- */

static int	virtgpu_attach(dev_info_t *, ddi_attach_cmd_t);
static int	virtgpu_detach(dev_info_t *, ddi_detach_cmd_t);
static int	virtgpu_quiesce(dev_info_t *);
static int	virtgpu_info(dev_info_t *, ddi_info_cmd_t, void *, void **);

/* ---- drm_driver for virtio-gpu ---- */

static struct drm_driver virtgpu_driver = {
	.load		= NULL,
	.unload		= NULL,
	.open		= virtio_gpu_driver_open,
	.postclose	= virtio_gpu_driver_postclose,

	.gem_free_object = NULL,	/* handled via cleanup_object */
	.dumb_create	= virtio_gpu_mode_dumb_create,

	.major		= DRIVER_MAJOR,
	.minor		= DRIVER_MINOR,
	.patchlevel	= DRIVER_PATCHLEVEL,
	.name		= DRIVER_NAME,
	.desc		= DRIVER_DESC,
	.date		= "20260404",

	.driver_features = DRIVER_GEM | DRIVER_RENDER,

	.ioctls		= virtio_gpu_ioctls,
	.num_ioctls	= DRM_VIRTIO_NUM_IOCTLS,
};

/* ---- cb_ops / dev_ops ---- */

static struct cb_ops virtgpu_cb_ops = {
	drm_sun_open,		/* open */
	drm_sun_close,		/* close */
	nodev,			/* strategy */
	nodev,			/* print */
	nodev,			/* dump */
	drm_sun_read,		/* read */
	nodev,			/* write */
	drm_sun_ioctl,		/* ioctl */
	drm_sun_devmap,		/* devmap */
	nodev,			/* mmap */
	nodev,			/* segmap */
	drm_sun_chpoll,		/* chpoll */
	ddi_prop_op,		/* prop_op */
	NULL,			/* streamtab */
	D_NEW | D_MTSAFE,	/* flags */
	CB_REV,
	nodev,			/* aread */
	nodev,			/* awrite */
};

static struct dev_ops virtgpu_dev_ops = {
	DEVO_REV,
	0,			/* refcnt */
	virtgpu_info,		/* getinfo */
	nulldev,		/* identify */
	nulldev,		/* probe */
	virtgpu_attach,		/* attach */
	virtgpu_detach,		/* detach */
	nodev,			/* reset */
	&virtgpu_cb_ops,	/* cb_ops */
	NULL,			/* bus_ops */
	NULL,			/* power */
	virtgpu_quiesce,	/* quiesce */
};

/* ---- Module linkage ---- */

static struct modldrv virtgpu_modldrv = {
	&mod_driverops,
	"virtio GPU DRM " DRIVER_NAME,
	&virtgpu_dev_ops,
};

static struct modlinkage virtgpu_modlinkage = {
	MODREV_1,
	{ &virtgpu_modldrv, NULL }
};

/* Per-instance soft state */
static void *virtgpu_statep;

int
_init(void)
{
	int ret;

	ret = ddi_soft_state_init(&virtgpu_statep,
	    sizeof (drm_inst_state_t), 1);
	if (ret != 0)
		return (ret);

	drm_sun_workqueue_init();

	ret = mod_install(&virtgpu_modlinkage);
	if (ret != 0) {
		drm_sun_workqueue_fini();
		ddi_soft_state_fini(&virtgpu_statep);
	}

	return (ret);
}

int
_fini(void)
{
	int ret;

	ret = mod_remove(&virtgpu_modlinkage);
	if (ret == 0) {
		drm_sun_workqueue_fini();
		ddi_soft_state_fini(&virtgpu_statep);
	}

	return (ret);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&virtgpu_modlinkage, modinfop));
}

/* ---- DDI Attach/Detach ---- */

static int
virtgpu_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	struct virtio_gpu_device *vgdev;
	drm_inst_state_t *mstate;
	struct drm_device *dev;
	int instance;
	int ret;

	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);

	instance = ddi_get_instance(dip);

	ret = ddi_soft_state_zalloc(virtgpu_statep, instance);
	if (ret != DDI_SUCCESS)
		return (DDI_FAILURE);

	mstate = ddi_get_soft_state(virtgpu_statep, instance);
	mstate->mis_dip = dip;
	mstate->mis_major = ddi_driver_major(dip);

	/*
	 * Initialize virtio framework.
	 * virtio_init takes only dip; feature negotiation is separate.
	 */
	virtio_t *vio = virtio_init(dip);
	if (vio == NULL) {
		cmn_err(CE_WARN, "virtio_gpu: virtio_init failed");
		ddi_soft_state_free(virtgpu_statep, instance);
		return (DDI_FAILURE);
	}

	/* Negotiate features */
	if (!virtio_init_features(vio, VIRTGPU_WANTED_FEATURES, B_TRUE)) {
		cmn_err(CE_WARN, "virtio_gpu: feature negotiation failed");
		virtio_fini(vio, B_TRUE);
		ddi_soft_state_free(virtgpu_statep, instance);
		return (DDI_FAILURE);
	}

	/*
	 * Allocate DRM device.
	 * The gfx-drm framework expects certain fields to be set up.
	 */
	dev = kmem_zalloc(sizeof (*dev), KM_SLEEP);
	dev->devinfo = dip;
	dev->driver = &virtgpu_driver;
	mutex_init(&dev->struct_mutex, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&dev->object_name_lock, NULL, MUTEX_DRIVER, NULL);
	INIT_LIST_HEAD(&dev->filelist);
	INIT_LIST_HEAD(&dev->maplist);
	idr_init(&dev->object_name_idr);

	/* Allocate virtio-gpu device state */
	vgdev = kmem_zalloc(sizeof (*vgdev), KM_SLEEP);
	vgdev->ddev = dev;
	vgdev->vio = vio;
	vgdev->dip = dip;
	dev->dev_private = vgdev;

	mstate->mis_devp = dev;

	/* Initialize the driver (queues, features, capsets) */
	ret = virtio_gpu_init(vgdev);
	if (ret != 0) {
		cmn_err(CE_WARN, "virtio_gpu: virtio_gpu_init failed (%d)",
		    ret);
		goto err_fini;
	}

	/* Create DRM minor nodes */
	ret = ddi_create_minor_node(dip, "drm", S_IFCHR,
	    DRM_MINOR_ID_BASE_RENDER + instance,
	    DDI_NT_DISPLAY, 0);
	if (ret != DDI_SUCCESS) {
		cmn_err(CE_WARN, "virtio_gpu: failed to create minor node");
		goto err_deinit;
	}

	DRM_INFO("virtio_gpu: attached instance %d\n", instance);
	return (DDI_SUCCESS);

err_deinit:
	virtio_gpu_deinit(vgdev);
err_fini:
	virtio_fini(vio, B_TRUE);
	kmem_free(vgdev, sizeof (*vgdev));
	kmem_free(dev, sizeof (*dev));
	ddi_soft_state_free(virtgpu_statep, instance);
	return (DDI_FAILURE);
}

static int
virtgpu_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	drm_inst_state_t *mstate;
	struct drm_device *dev;
	struct virtio_gpu_device *vgdev;
	int instance;

	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);

	instance = ddi_get_instance(dip);
	mstate = ddi_get_soft_state(virtgpu_statep, instance);
	if (mstate == NULL)
		return (DDI_FAILURE);

	dev = mstate->mis_devp;
	if (dev == NULL)
		return (DDI_FAILURE);

	vgdev = dev->dev_private;

	ddi_remove_minor_node(dip, NULL);

	if (vgdev != NULL) {
		virtio_gpu_deinit(vgdev);
		virtio_gpu_free_vbufs(vgdev);
		virtio_fini(vgdev->vio, B_FALSE);
		kmem_free(vgdev, sizeof (*vgdev));
	}

	mutex_destroy(&dev->struct_mutex);
	mutex_destroy(&dev->object_name_lock);
	kmem_free(dev, sizeof (*dev));

	mstate->mis_devp = NULL;
	ddi_soft_state_free(virtgpu_statep, instance);

	return (DDI_SUCCESS);
}

static int
virtgpu_quiesce(dev_info_t *dip)
{
	/* Reset the virtio device for fast reboot */
	drm_inst_state_t *mstate;
	struct virtio_gpu_device *vgdev;
	int instance = ddi_get_instance(dip);

	mstate = ddi_get_soft_state(virtgpu_statep, instance);
	if (mstate == NULL || mstate->mis_devp == NULL)
		return (DDI_SUCCESS);

	vgdev = mstate->mis_devp->dev_private;
	if (vgdev != NULL && vgdev->vio != NULL)
		virtio_device_reset(vgdev->vio);

	return (DDI_SUCCESS);
}

static int
virtgpu_info(dev_info_t *dip, ddi_info_cmd_t infocmd, void *arg,
    void **result)
{
	drm_inst_state_t *mstate;
	int instance, error = DDI_FAILURE;

	instance = DRM_DEV2MINOR((dev_t)arg);
	if (instance < 0 || instance > DRM_MAX_INSTANCES)
		return (DDI_FAILURE);

	switch (infocmd) {
	case DDI_INFO_DEVT2DEVINFO:
		mstate = ddi_get_soft_state(virtgpu_statep, instance);
		if (mstate != NULL && mstate->mis_dip != NULL) {
			*result = (void *)mstate->mis_dip;
			error = DDI_SUCCESS;
		}
		break;
	case DDI_INFO_DEVT2INSTANCE:
		*result = (void *)(uintptr_t)instance;
		error = DDI_SUCCESS;
		break;
	default:
		break;
	}

	return (error);
}
