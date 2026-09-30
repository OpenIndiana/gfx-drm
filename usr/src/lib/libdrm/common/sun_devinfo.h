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

#ifndef _SUN_DEVINFO_H
#define	_SUN_DEVINFO_H

/*
 * The illumos device layer of libdrm, private to libdrm.so.
 * See sun_devinfo.c for the design.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "xf86drm.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The DRM drivers of illumos (gfx-drm's drm module and the Rust DRM core)
 * clone every open: the low nine bits of the minor number name the node
 * (card<N>, controlD<N>, renderD<128+N>), the bits above hold the clone id
 * of the open file.  Only the node bits identify a /dev/dri node.
 */
#define	DRM_SUN_NODE_MINOR_BITS	9
#define	DRM_SUN_NODE_MINOR_MASK	((1U << DRM_SUN_NODE_MINOR_BITS) - 1)

#define	DRM_SUN_HIDDEN	__attribute__((visibility("hidden")))

DRM_SUN_HIDDEN extern bool drmSunSameNode(dev_t, dev_t);
DRM_SUN_HIDDEN extern bool drmSunNodeIsDRM(unsigned int, unsigned int);
DRM_SUN_HIDDEN extern int drmSunMinorType(unsigned int, unsigned int);
DRM_SUN_HIDDEN extern char *drmSunNodeName(dev_t, int);
DRM_SUN_HIDDEN extern int drmSunSubsystemType(unsigned int, unsigned int);
DRM_SUN_HIDDEN extern int drmSunPciBusInfo(unsigned int, unsigned int,
    drmPciBusInfoPtr);
DRM_SUN_HIDDEN extern int drmSunPciDeviceInfo(unsigned int, unsigned int,
    drmPciDeviceInfoPtr);
DRM_SUN_HIDDEN extern int drmSunFauxBusInfo(unsigned int, unsigned int,
    char *, size_t);
DRM_SUN_HIDDEN extern int drmSunIoctlCompat(int, unsigned long, void *);

#ifdef __cplusplus
}
#endif

#endif /* _SUN_DEVINFO_H */
