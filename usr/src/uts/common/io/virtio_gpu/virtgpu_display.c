/*
 * Copyright (C) 2015 Red Hat, Inc.
 * All Rights Reserved.
 *
 * Ported from Linux drivers/gpu/drm/virtio/virtgpu_display.c for illumos.
 *
 * Uses the minimal atomic shim (drm_sun_atomic.h) to satisfy the
 * virtio-gpu driver's modesetting needs without a full atomic framework.
 * Display output is virtual — the host (QEMU) composites the scanout.
 */

#include "virtgpu_drv.h"
#include "drm_sun_atomic.h"

#define	XRES_MIN	32
#define	YRES_MIN	32
#define	XRES_DEF	1024
#define	YRES_DEF	768
#define	XRES_MAX	8192
#define	YRES_MAX	8192

#define	drm_connector_to_virtio_gpu_output(x) \
	container_of(x, struct virtio_gpu_output, conn)

/* ---- CRTC ---- */

/*
 * CRTC mode_set callback.
 *
 * 3.14 uses .mode_set (NOT .mode_set_nofb which is post-4.x).
 * Signature: int mode_set(crtc, mode, adjusted_mode, x, y, old_fb)
 *
 * We send SET_SCANOUT to the host with the new mode dimensions.
 */
static int
virtio_gpu_crtc_mode_set(struct drm_crtc *crtc,
    struct drm_display_mode *mode,
    struct drm_display_mode *adjusted_mode,
    int x, int y, struct drm_framebuffer *old_fb)
{
	struct drm_device *dev = crtc->dev;
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_output *output = drm_crtc_to_virtio_gpu_output(crtc);

	virtio_gpu_cmd_set_scanout(vgdev, output->index, 0,
	    mode->hdisplay, mode->vdisplay, 0, 0);
	virtio_gpu_notify(vgdev);
	return (0);
}

static void
virtio_gpu_crtc_dpms(struct drm_crtc *crtc, int mode)
{
	/* DPMS control not needed for virtio-gpu */
}

static void
virtio_gpu_crtc_prepare(struct drm_crtc *crtc)
{
	/* Nothing to prepare */
}

static void
virtio_gpu_crtc_commit(struct drm_crtc *crtc)
{
	/* Nothing to commit */
}

static boolean_t
virtio_gpu_crtc_mode_fixup(struct drm_crtc *crtc,
    const struct drm_display_mode *mode,
    struct drm_display_mode *adjusted_mode)
{
	return (B_TRUE); /* Accept all modes */
}

static const struct drm_crtc_funcs virtio_gpu_crtc_funcs = {
	.set_config	= drm_crtc_helper_set_config,
	.destroy	= drm_crtc_cleanup,
};

static const struct drm_crtc_helper_funcs virtio_gpu_crtc_helper_funcs = {
	.dpms		= virtio_gpu_crtc_dpms,
	.mode_fixup	= virtio_gpu_crtc_mode_fixup,
	.mode_set	= virtio_gpu_crtc_mode_set,
	.prepare	= virtio_gpu_crtc_prepare,
	.commit		= virtio_gpu_crtc_commit,
};

/* ---- Encoder ---- */

static void
virtio_gpu_enc_dpms(struct drm_encoder *encoder, int mode)
{
	/* Virtual encoder, nothing to do */
}

static void
virtio_gpu_enc_mode_set(struct drm_encoder *encoder,
    struct drm_display_mode *mode,
    struct drm_display_mode *adjusted_mode)
{
	/* Virtual encoder, nothing to do */
}

static void
virtio_gpu_enc_prepare(struct drm_encoder *encoder)
{
}

static void
virtio_gpu_enc_commit(struct drm_encoder *encoder)
{
}

static const struct drm_encoder_helper_funcs virtio_gpu_enc_helper_funcs = {
	.dpms		= virtio_gpu_enc_dpms,
	.mode_set	= virtio_gpu_enc_mode_set,
	.prepare	= virtio_gpu_enc_prepare,
	.commit		= virtio_gpu_enc_commit,
};

/* ---- Connector ---- */

static int
virtio_gpu_conn_get_modes(struct drm_connector *connector)
{
	struct virtio_gpu_output *output =
	    drm_connector_to_virtio_gpu_output(connector);
	struct drm_display_mode *mode;
	int count, width, height;

	width  = le32_to_cpu(output->info.r.width);
	height = le32_to_cpu(output->info.r.height);

	count = drm_add_modes_noedid(connector, XRES_MAX, YRES_MAX);

	if (width == 0 || height == 0) {
		drm_set_preferred_mode(connector, XRES_DEF, YRES_DEF);
	} else {
		DRM_DEBUG("add mode: %dx%d\n", width, height);
		mode = drm_cvt_mode(connector->dev, width, height, 60,
		    false, false, false);
		if (mode != NULL) {
			mode->type |= DRM_MODE_TYPE_PREFERRED;
			drm_mode_probed_add(connector, mode);
			count++;
		}
	}

	return (count);
}

static enum drm_connector_status
virtio_gpu_conn_detect(struct drm_connector *connector, bool force)
{
	struct virtio_gpu_output *output =
	    drm_connector_to_virtio_gpu_output(connector);

	if (output->info.enabled)
		return (connector_status_connected);
	else
		return (connector_status_disconnected);
}

static void
virtio_gpu_conn_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs virtio_gpu_connector_funcs = {
	.detect		= virtio_gpu_conn_detect,
	.fill_modes	= drm_helper_probe_single_connector_modes,
	.destroy	= virtio_gpu_conn_destroy,
};

static const struct drm_connector_helper_funcs virtio_gpu_conn_helper_funcs = {
	.get_modes	= virtio_gpu_conn_get_modes,
};

/* ---- Output init ---- */

static int
vgdev_output_init(struct virtio_gpu_device *vgdev, int index)
{
	struct drm_device *dev = vgdev->ddev;
	struct virtio_gpu_output *output = &vgdev->outputs[index];
	struct drm_connector *connector = &output->conn;
	struct drm_encoder *encoder = &output->enc;
	struct drm_crtc *crtc = &output->crtc;
	int ret;

	output->index = index;
	if (index == 0) {
		output->info.enabled = cpu_to_le32(1);
		output->info.r.width = cpu_to_le32(XRES_DEF);
		output->info.r.height = cpu_to_le32(YRES_DEF);
	}

	/* Initialize CRTC */
	ret = drm_crtc_init(dev, crtc, &virtio_gpu_crtc_funcs);
	if (ret != 0)
		return (ret);
	drm_crtc_helper_add(crtc, &virtio_gpu_crtc_helper_funcs);

	/* Initialize connector */
	ret = drm_connector_init(dev, connector,
	    &virtio_gpu_connector_funcs, DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret != 0)
		return (ret);
	drm_connector_helper_add(connector, &virtio_gpu_conn_helper_funcs);

	/* Initialize encoder using simple encoder from atomic shim */
	ret = drm_simple_encoder_init(dev, encoder,
	    DRM_MODE_ENCODER_VIRTUAL);
	if (ret != 0)
		return (ret);
	drm_encoder_helper_add(encoder, &virtio_gpu_enc_helper_funcs);
	encoder->possible_crtcs = 1 << index;

	drm_connector_attach_encoder(connector, encoder);
	drm_connector_register(connector);

	return (0);
}

/* ---- Mode config ---- */

static const struct drm_mode_config_funcs virtio_gpu_mode_funcs = {
	.fb_create	= NULL, /* Default framebuffer creation */
};

int
virtio_gpu_modeset_init(struct virtio_gpu_device *vgdev)
{
	int i, ret;

	if (vgdev->num_scanouts == 0)
		return (0);

	drm_mode_config_init(vgdev->ddev);

	vgdev->ddev->mode_config.funcs = &virtio_gpu_mode_funcs;
	vgdev->ddev->mode_config.min_width = XRES_MIN;
	vgdev->ddev->mode_config.min_height = YRES_MIN;
	vgdev->ddev->mode_config.max_width = XRES_MAX;
	vgdev->ddev->mode_config.max_height = YRES_MAX;

	for (i = 0; i < (int)vgdev->num_scanouts; i++) {
		ret = vgdev_output_init(vgdev, i);
		if (ret != 0) {
			DRM_ERROR("output %d init failed: %d\n", i, ret);
			return (ret);
		}
	}

	ret = drm_vblank_init(vgdev->ddev, vgdev->num_scanouts);
	if (ret != 0)
		DRM_ERROR("vblank init failed: %d\n", ret);

	DRM_INFO("virtio_gpu: display initialized with %d scanouts\n",
	    vgdev->num_scanouts);
	return (0);
}

void
virtio_gpu_modeset_fini(struct virtio_gpu_device *vgdev)
{
	if (vgdev->num_scanouts == 0)
		return;

	drm_mode_config_cleanup(vgdev->ddev);
}
