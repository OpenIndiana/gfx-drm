/*
 * virtio-gpu debugfs -- no-op on illumos.
 */

#include "virtgpu_drv.h"

void
virtio_gpu_debugfs_init(struct drm_minor *minor)
{
	/* illumos has no debugfs */
}
