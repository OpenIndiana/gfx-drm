/*
 * virtio-gpu display/modesetting -- stub for headless (render-only).
 * Full display support deferred to Phase 5.
 */

#include "virtgpu_drv.h"

int
virtio_gpu_modeset_init(struct virtio_gpu_device *vgdev)
{
	/* No display initialization for headless render mode */
	return (0);
}

void
virtio_gpu_modeset_fini(struct virtio_gpu_device *vgdev)
{
	/* Nothing to clean up */
}
