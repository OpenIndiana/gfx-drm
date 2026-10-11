/*
 * virtio-gpu execbuffer (3D command submission) -- stub.
 * Full implementation deferred until virgl/venus 3D is needed.
 */

#include "virtgpu_drv.h"

int
virtio_gpu_execbuffer_ioctl(struct drm_device *dev, void *data,
    struct drm_file *file)
{
	return (-ENOSYS);
}
