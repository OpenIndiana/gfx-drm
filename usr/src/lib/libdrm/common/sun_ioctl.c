/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Till Wegmueller
 */

/*
 * Core ioctls whose argument structure grew between the uAPI headers of
 * libdrm 2.4.109 and those of this libdrm.
 *
 * The illumos ioctl encoding carries the argument size (its low eight
 * bits), so a grown structure has a new command number.  Linux's
 * drm_ioctl() accepts every size of a command and zero-extends or
 * truncates the argument; a kernel that only knows the older size answers
 * the new number with ENOTTY on illumos.  The Rust DRM core of R0 to R4
 * (whose uAPI was generated from the libdrm 2.4.109 headers) is such a
 * kernel.  When a command below fails with ENOTTY and the fields added
 * after the older size are all zero, which is what every libdrm wrapper
 * sends, the call is repeated with the older size: the kernel then sees
 * exactly the request it knows, as Linux would have truncated it.
 * Arguments that use the new fields are not retried.
 *
 * The gfx-drm kernel (drm, i915) implements none of these commands.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioccom.h>
#include <sys/ioctl.h>

#include "xf86drm.h"
#include "sun_devinfo.h"

static const struct {
	unsigned long	cmd;		/* the command with the current size */
	size_t		oldsize;	/* the size in libdrm 2.4.109 */
	size_t		size;
} drm_sun_grown[] = {
	{ DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD,
	    offsetof(struct drm_syncobj_handle, point),
	    sizeof (struct drm_syncobj_handle) },
	{ DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE,
	    offsetof(struct drm_syncobj_handle, point),
	    sizeof (struct drm_syncobj_handle) },
	{ DRM_IOCTL_SYNCOBJ_WAIT,
	    offsetof(struct drm_syncobj_wait, deadline_nsec),
	    sizeof (struct drm_syncobj_wait) },
	{ DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT,
	    offsetof(struct drm_syncobj_timeline_wait, deadline_nsec),
	    sizeof (struct drm_syncobj_timeline_wait) },
};

/*
 * Called by drmIoctl() after 'request' failed with ENOTTY.  Returns the
 * result of the retried ioctl, or -1 with errno ENOTTY.
 */
int
drmSunIoctlCompat(int fd, unsigned long request, void *arg)
{
	const unsigned char *p = arg;
	unsigned long oldreq;
	size_t i, j;
	int ret;

	for (i = 0; i < sizeof (drm_sun_grown) / sizeof (drm_sun_grown[0]);
	    i++) {
		if (drm_sun_grown[i].cmd == request)
			break;
	}
	if (i == sizeof (drm_sun_grown) / sizeof (drm_sun_grown[0]) ||
	    arg == NULL) {
		errno = ENOTTY;
		return (-1);
	}
	for (j = drm_sun_grown[i].oldsize; j < drm_sun_grown[i].size; j++) {
		if (p[j] != 0) {
			errno = ENOTTY;
			return (-1);
		}
	}

	oldreq = (request & ~((unsigned long)IOCPARM_MASK << 16)) |
	    ((drm_sun_grown[i].oldsize & IOCPARM_MASK) << 16);
	do {
		ret = ioctl(fd, oldreq, arg);
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
	return (ret);
}
