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
 * Checks the illumos device layer of libdrm (lib/libdrm/common/
 * sun_devinfo.c) through the public libdrm interface, for every node in
 * /dev/dri or for the nodes named on the command line:
 *
 *  - drmGetNodeTypeFromFd() agrees with the node's name;
 *  - drmGetDeviceNameFromFd2() gives the node back, for the first open and
 *    for a second open, whose dev_t differs in the clone bits;
 *  - drmGetDeviceNameFromFd() names primary nodes only;
 *  - drmGetDevice2() finds the device from either open, and the two
 *    results are equal;
 *  - the device lists the node, and drmGetPrimaryDeviceNameFromFd() and
 *    drmGetRenderDeviceNameFromFd() name the device's nodes;
 *  - drmGetDevices2() lists the device.
 *
 * Prints one PASS or FAIL line per check, the bus information of each
 * device, and exits non-zero if any check failed.  Run it as root.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mkdev.h>
#include <sys/stat.h>

#include "xf86drm.h"

#define	MAX_DEVICES	64

static int failures;
static int passes;

static void
check(int ok, const char *node, const char *what)
{
	(void) printf("%s: %s: %s\n", ok ? "PASS" : "FAIL", node, what);
	if (ok)
		passes++;
	else
		failures++;
}

static int
streq(const char *a, const char *b)
{
	return (a != NULL && b != NULL && strcmp(a, b) == 0);
}

static int
name_type(const char *name)
{
	if (strncmp(name, "card", 4) == 0)
		return (DRM_NODE_PRIMARY);
	if (strncmp(name, "renderD", 7) == 0)
		return (DRM_NODE_RENDER);
	if (strncmp(name, "controlD", 8) == 0)
		return (DRM_NODE_CONTROL);
	return (-1);
}

static void
print_device(const char *node, drmDevicePtr dev)
{
	int i;

	(void) printf("INFO: %s: available_nodes 0x%x", node,
	    dev->available_nodes);
	for (i = 0; i < DRM_NODE_MAX; i++) {
		if (dev->available_nodes & (1 << i))
			(void) printf(" [%d]=%s", i, dev->nodes[i]);
	}
	(void) printf("\n");

	switch (dev->bustype) {
	case DRM_BUS_PCI:
		(void) printf("INFO: %s: pci %04x:%02x:%02x.%u "
		    "vendor 0x%04x device 0x%04x subvendor 0x%04x "
		    "subdevice 0x%04x revision 0x%02x\n", node,
		    dev->businfo.pci->domain, dev->businfo.pci->bus,
		    dev->businfo.pci->dev, dev->businfo.pci->func,
		    dev->deviceinfo.pci->vendor_id,
		    dev->deviceinfo.pci->device_id,
		    dev->deviceinfo.pci->subvendor_id,
		    dev->deviceinfo.pci->subdevice_id,
		    dev->deviceinfo.pci->revision_id);
		break;
	case DRM_BUS_FAUX:
		(void) printf("INFO: %s: faux %s\n", node,
		    dev->businfo.faux->name);
		break;
	default:
		(void) printf("INFO: %s: bus type %d\n", node, dev->bustype);
		break;
	}
}

static void
test_node(const char *path, drmDevicePtr *all, int nall)
{
	const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	int type = name_type(name);
	drmDevicePtr dev = NULL, dev2 = NULL;
	struct stat st1, st2;
	char *s;
	int fd, fd2, ret, i, found;

	if (type == DRM_NODE_CONTROL) {
		(void) printf("SKIP: %s: control nodes are not enumerated\n",
		    path);
		return;
	}

	if ((fd = open(path, O_RDWR)) < 0) {
		(void) printf("FAIL: %s: open: %s\n", path, strerror(errno));
		failures++;
		return;
	}
	if ((fd2 = open(path, O_RDWR)) < 0) {
		(void) printf("FAIL: %s: second open: %s\n", path,
		    strerror(errno));
		failures++;
		(void) close(fd);
		return;
	}
	(void) fstat(fd, &st1);
	(void) fstat(fd2, &st2);
	(void) printf("INFO: %s: opens have major %u minors 0x%x 0x%x\n",
	    path, (unsigned int)major(st1.st_rdev),
	    (unsigned int)minor(st1.st_rdev), (unsigned int)minor(st2.st_rdev));

	check(drmGetNodeTypeFromFd(fd) == type, path,
	    "drmGetNodeTypeFromFd matches the node name");
	check(drmGetNodeTypeFromFd(fd2) == type, path,
	    "drmGetNodeTypeFromFd of the second open matches the node name");

	s = drmGetDeviceNameFromFd2(fd);
	check(streq(s, path), path, "drmGetDeviceNameFromFd2 gives the node");
	free(s);
	s = drmGetDeviceNameFromFd2(fd2);
	check(streq(s, path), path,
	    "drmGetDeviceNameFromFd2 of the second open gives the node");
	free(s);

	s = drmGetDeviceNameFromFd(fd2);
	if (type == DRM_NODE_PRIMARY)
		check(streq(s, path), path,
		    "drmGetDeviceNameFromFd gives the primary node");
	else
		check(s == NULL, path,
		    "drmGetDeviceNameFromFd is NULL for a render node");
	free(s);

	ret = drmGetDevice2(fd, DRM_DEVICE_GET_PCI_REVISION, &dev);
	check(ret == 0, path, "drmGetDevice2");
	ret = drmGetDevice2(fd2, DRM_DEVICE_GET_PCI_REVISION, &dev2);
	check(ret == 0, path, "drmGetDevice2 of the second open");
	if (dev != NULL && dev2 != NULL)
		check(drmDevicesEqual(dev, dev2), path,
		    "both opens give the same device");

	if (dev != NULL) {
		print_device(path, dev);
		check((dev->available_nodes & (1 << type)) != 0 &&
		    streq(dev->nodes[type], path), path,
		    "the device lists the node");

		s = drmGetPrimaryDeviceNameFromFd(fd2);
		if (dev->available_nodes & (1 << DRM_NODE_PRIMARY))
			check(streq(s, dev->nodes[DRM_NODE_PRIMARY]), path,
			    "drmGetPrimaryDeviceNameFromFd names the device's "
			    "primary node");
		else
			check(s == NULL, path,
			    "drmGetPrimaryDeviceNameFromFd is NULL without a "
			    "primary node");
		free(s);

		s = drmGetRenderDeviceNameFromFd(fd2);
		if (dev->available_nodes & (1 << DRM_NODE_RENDER))
			check(streq(s, dev->nodes[DRM_NODE_RENDER]), path,
			    "drmGetRenderDeviceNameFromFd names the device's "
			    "render node");
		else
			check(s == NULL, path,
			    "drmGetRenderDeviceNameFromFd is NULL without a "
			    "render node");
		free(s);

		for (found = 0, i = 0; i < nall; i++) {
			if (drmDevicesEqual(all[i], dev))
				found++;
		}
		check(found == 1, path, "drmGetDevices2 lists the device once");
	}

	drmFreeDevice(&dev);
	drmFreeDevice(&dev2);
	(void) close(fd2);
	(void) close(fd);
}

int
main(int argc, char **argv)
{
	drmDevicePtr all[MAX_DEVICES];
	char path[512];
	struct dirent *de;
	DIR *dir;
	int nall, n, i;

	n = drmGetDevices2(0, NULL, 0);
	nall = drmGetDevices2(DRM_DEVICE_GET_PCI_REVISION, all, MAX_DEVICES);
	(void) printf("INFO: drmGetDevices2 counts %d devices, returns %d\n",
	    n, nall);
	check(nall > 0 && nall == n, "drmGetDevices2",
	    "finds devices and counts them consistently");
	if (nall < 0)
		nall = 0;
	for (i = 0; i < nall; i++)
		print_device("drmGetDevices2", all[i]);

	if (argc > 1) {
		for (i = 1; i < argc; i++)
			test_node(argv[i], all, nall);
	} else if ((dir = opendir("/dev/dri")) != NULL) {
		while ((de = readdir(dir)) != NULL) {
			if (name_type(de->d_name) < 0)
				continue;
			(void) snprintf(path, sizeof (path), "/dev/dri/%s",
			    de->d_name);
			test_node(path, all, nall);
		}
		(void) closedir(dir);
	} else {
		(void) printf("FAIL: /dev/dri: %s\n", strerror(errno));
		failures++;
	}

	drmFreeDevices(all, nall);
	(void) printf("getsundev: %d passed, %d failed\n", passes, failures);
	return (failures ? 1 : 0);
}
