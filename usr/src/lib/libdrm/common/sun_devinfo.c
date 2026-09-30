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
 * The illumos device layer of libdrm: how xf86drm.c finds DRM nodes, their
 * types, the device they belong to and its bus.  Linux answers these
 * questions from sysfs; here the answers come from the /dev/dri links and
 * from libdevinfo.
 *
 * Nodes.  devfsadm(8) (SUNW_drm_link, cmd/devfsadm/drm_link_i386.c) links
 * every minor node of type DDI_NT_DISPLAY_DRM ("ddi_display:drm") into
 * /dev/dri, using the minor name:
 *
 *	driver			minor name	/dev/dri link	node minor
 *	Rust DRM core		card<N>		card<N>		N
 *	(rdrmnull, virtio_gpu)	renderD<128+N>	renderD<128+N>	128 + N
 *	gfx-drm drm/i915	drm<N>		card<N>		N
 *				controlD<N>	controlD<N>	64 + N
 *
 * The directory is the only place that knows the name libdrm and Mesa
 * use for a node, so it is the index: a node's type comes from its link
 * name (card, controlD, renderD), never from its minor number, whose
 * layout differs between the two kernels.
 *
 * Open files.  Both kernels clone every open (drm_sun_open() in gfx-drm,
 * CharDevice::clone_open in the Rust core): the file's dev_t carries the
 * node minor in its low nine bits and a clone id above them.  A file is
 * mapped to its node by the major number and the node bits only
 * (drmSunSameNode()), so every open of a node, not just the first,
 * finds it.  Majors are dynamic and differ per driver, so there is no
 * DRM_MAJOR on illumos.
 *
 * Devices.  The link resolves to /devices/<devfs path>:<minor name>.
 * Nodes with the same devfs path belong to the same device (card and
 * render node pairing).  The device node itself is read with libdevinfo:
 * the node is accepted as a DRM node only if the device has a minor of
 * type DDI_NT_DISPLAY_DRM with the node's dev_t (whatever the device node
 * is called: VGA-class functions are "display", other functions such as
 * virtio-gpu (class 0x0380) are "pci1af4,1100" and the like).  A device
 * whose parent nexus has device_type "pci" or "pciex" is a PCI device:
 * bus, device and function come from its "reg" (or "assigned-addresses")
 * property, the IDs from its PCI properties.  A child of the pseudo nexus
 * (such as rdrmnull) is reported as a DRM_BUS_FAUX device named after its
 * devfs node ("rdrmnull@0"), as Linux reports vgem and vkms.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libdevinfo.h>
#include <sys/ioccom.h>
#include <sys/mkdev.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "xf86drm.h"
#include "sun_devinfo.h"

#ifndef DDI_NT_DISPLAY_DRM
#define	DDI_NT_DISPLAY_DRM	"ddi_display:drm"
#endif

#define	SUN_DRM_DIR		"/dev/dri"
#define	SUN_DEVICES		"/devices"

/* Fields of phys.hi of a PCI "reg" entry (IEEE 1275 PCI binding). */
#define	SUN_PCI_REG_BUS(hi)	(((hi) >> 16) & 0xff)
#define	SUN_PCI_REG_DEV(hi)	(((hi) >> 11) & 0x1f)
#define	SUN_PCI_REG_FUNC(hi)	(((hi) >> 8) & 0x7)
#define	SUN_PCI_REG_INTS	5	/* ints per "reg" entry */

/* One /dev/dri node. */
struct sun_node {
	char	sn_name[32];		/* "card0" */
	int	sn_type;		/* DRM_NODE_* */
	dev_t	sn_dev;			/* major, node minor */
	char	sn_devfs[MAXPATHLEN];	/* "/pci@0,0/pci1af4,1100@10" */
};

bool
drmSunSameNode(dev_t a, dev_t b)
{
	return (major(a) == major(b) &&
	    (minor(a) & DRM_SUN_NODE_MINOR_MASK) ==
	    (minor(b) & DRM_SUN_NODE_MINOR_MASK));
}

/*
 * The node type of a /dev/dri name: card<N>, controlD<N> or renderD<N>
 * with nothing after the digits.  -1 for anything else.
 */
static int
sun_name_type(const char *name)
{
	static const struct {
		const char	*prefix;
		int		type;
	} prefixes[] = {
		{ "card",	DRM_NODE_PRIMARY },
		{ "controlD",	DRM_NODE_CONTROL },
		{ "renderD",	DRM_NODE_RENDER },
	};
	size_t i, len;
	const char *p;

	for (i = 0; i < sizeof (prefixes) / sizeof (prefixes[0]); i++) {
		len = strlen(prefixes[i].prefix);
		if (strncmp(name, prefixes[i].prefix, len) != 0)
			continue;
		p = name + len;
		if (*p == '\0')
			return (-1);
		for (; *p != '\0'; p++) {
			if (*p < '0' || *p > '9')
				return (-1);
		}
		return (prefixes[i].type);
	}
	return (-1);
}

/*
 * Fill in *sn for the /dev/dri entry name: its type, its node dev_t and
 * the devfs path of its device.  Returns 0 or -errno.
 */
static int
sun_node_read(const char *name, struct sun_node *sn)
{
	char path[MAXPATHLEN], real[MAXPATHLEN];
	struct stat st;
	char *colon;
	size_t plen = strlen(SUN_DEVICES);

	if ((sn->sn_type = sun_name_type(name)) < 0)
		return (-EINVAL);
	if (strlcpy(sn->sn_name, name, sizeof (sn->sn_name)) >=
	    sizeof (sn->sn_name))
		return (-ENAMETOOLONG);
	(void) snprintf(path, sizeof (path), "%s/%s", SUN_DRM_DIR, name);
	if (stat(path, &st) != 0)
		return (-errno);
	if (!S_ISCHR(st.st_mode))
		return (-ENODEV);
	sn->sn_dev = makedev(major(st.st_rdev),
	    minor(st.st_rdev) & DRM_SUN_NODE_MINOR_MASK);

	/* /devices/<devfs path>:<minor name> */
	if (realpath(path, real) == NULL)
		return (-errno);
	if (strncmp(real, SUN_DEVICES "/", plen + 1) != 0)
		return (-ENODEV);
	if ((colon = strrchr(real, ':')) == NULL || colon < real + plen)
		return (-ENODEV);
	*colon = '\0';
	if (strlcpy(sn->sn_devfs, real + plen, sizeof (sn->sn_devfs)) >=
	    sizeof (sn->sn_devfs))
		return (-ENAMETOOLONG);
	return (0);
}

/*
 * Find the /dev/dri node that matches: with devfs == NULL the node of
 * dev (clone bits ignored), otherwise the node of type 'type' of the
 * device at devfs.  Returns 0 or -errno.
 */
static int
sun_node_find(dev_t dev, const char *devfs, int type, struct sun_node *sn)
{
	struct dirent *de;
	DIR *dir;
	int ret = -ENODEV;

	if ((dir = opendir(SUN_DRM_DIR)) == NULL)
		return (-errno);
	while ((de = readdir(dir)) != NULL) {
		if (sun_name_type(de->d_name) < 0)
			continue;
		if (devfs == NULL) {
			if (sun_node_read(de->d_name, sn) != 0 ||
			    !drmSunSameNode(sn->sn_dev, dev))
				continue;
		} else {
			if (sun_name_type(de->d_name) != type ||
			    sun_node_read(de->d_name, sn) != 0 ||
			    strcmp(sn->sn_devfs, devfs) != 0)
				continue;
		}
		ret = 0;
		break;
	}
	(void) closedir(dir);
	return (ret);
}

/*
 * A libdevinfo snapshot of the device of a node: the snapshot is taken
 * at the device's parent so that the parent's properties are in it.
 */
struct sun_devinfo {
	di_node_t	sd_root;	/* the snapshot (parent node) */
	di_node_t	sd_node;	/* the device */
	di_node_t	sd_parent;
};

static void
sun_devinfo_fini(struct sun_devinfo *sd)
{
	if (sd->sd_root != DI_NODE_NIL)
		di_fini(sd->sd_root);
	sd->sd_root = sd->sd_node = sd->sd_parent = DI_NODE_NIL;
}

/*
 * Does the device node have a DRM minor node with this dev_t?
 */
static bool
sun_devinfo_has_drm_minor(di_node_t node, dev_t dev)
{
	di_minor_t minor = DI_MINOR_NIL;
	const char *nodetype;

	while ((minor = di_minor_next(node, minor)) != DI_MINOR_NIL) {
		nodetype = di_minor_nodetype(minor);
		if (nodetype != NULL &&
		    strcmp(nodetype, DDI_NT_DISPLAY_DRM) == 0 &&
		    drmSunSameNode(di_minor_devt(minor), dev))
			return (true);
	}
	return (false);
}

/*
 * Look up the node (maj, min) in /dev/dri and its device with libdevinfo.
 * Succeeds only for DRM nodes (minor node type DDI_NT_DISPLAY_DRM).
 * Returns 0 or -errno; on success the caller calls sun_devinfo_fini().
 */
static int
sun_devinfo_init(unsigned int maj, unsigned int min, struct sun_node *sn,
    struct sun_devinfo *sd)
{
	char parent[MAXPATHLEN];
	char *slash, *path;
	di_node_t child;
	int ret;

	sd->sd_root = sd->sd_node = sd->sd_parent = DI_NODE_NIL;

	if ((ret = sun_node_find(makedev(maj, min), NULL, -1, sn)) != 0)
		return (ret);

	(void) strlcpy(parent, sn->sn_devfs, sizeof (parent));
	if ((slash = strrchr(parent, '/')) == NULL)
		return (-ENODEV);
	if (slash == parent)
		slash[1] = '\0';	/* a child of the root node */
	else
		*slash = '\0';

	sd->sd_root = di_init(parent, DINFOSUBTREE | DINFOMINOR | DINFOPROP);
	if (sd->sd_root == DI_NODE_NIL)
		return (errno != 0 ? -errno : -ENODEV);
	sd->sd_parent = sd->sd_root;

	for (child = di_child_node(sd->sd_root); child != DI_NODE_NIL;
	    child = di_sibling_node(child)) {
		if ((path = di_devfs_path(child)) == NULL)
			continue;
		ret = strcmp(path, sn->sn_devfs);
		di_devfs_path_free(path);
		if (ret == 0) {
			sd->sd_node = child;
			break;
		}
	}
	if (sd->sd_node == DI_NODE_NIL ||
	    !sun_devinfo_has_drm_minor(sd->sd_node, sn->sn_dev)) {
		sun_devinfo_fini(sd);
		return (-ENODEV);
	}
	return (0);
}

bool
drmSunNodeIsDRM(unsigned int maj, unsigned int min)
{
	struct sun_node sn;
	struct sun_devinfo sd;

	if (sun_devinfo_init(maj, min, &sn, &sd) != 0)
		return (false);
	sun_devinfo_fini(&sd);
	return (true);
}

int
drmSunMinorType(unsigned int maj, unsigned int min)
{
	struct sun_node sn;

	if (sun_node_find(makedev(maj, min), NULL, -1, &sn) != 0)
		return (-1);
	return (sn.sn_type);
}

/*
 * The /dev/dri path of the node dev (type < 0), or of the node of the
 * given type of the same device.  NULL if there is none.
 */
char *
drmSunNodeName(dev_t dev, int type)
{
	struct sun_node sn, other;
	char path[MAXPATHLEN];

	if (sun_node_find(dev, NULL, -1, &sn) != 0)
		return (NULL);
	if (type >= 0 && type != sn.sn_type) {
		if (sun_node_find(0, sn.sn_devfs, type, &other) != 0)
			return (NULL);
		sn = other;
	}
	(void) snprintf(path, sizeof (path), "%s/%s", SUN_DRM_DIR, sn.sn_name);
	return (strdup(path));
}

static bool
sun_parent_is_pci(di_node_t parent)
{
	char *types;
	int n, i;

	n = di_prop_lookup_strings(DDI_DEV_T_ANY, parent, "device_type",
	    &types);
	for (i = 0; i < n; i++) {
		if (strcmp(types, "pci") == 0 || strcmp(types, "pciex") == 0)
			return (true);
		types += strlen(types) + 1;
	}
	return (false);
}

static bool
sun_parent_is_pseudo(di_node_t parent)
{
	const char *name = di_node_name(parent);

	return (name != NULL && strcmp(name, "pseudo") == 0);
}

int
drmSunSubsystemType(unsigned int maj, unsigned int min)
{
	struct sun_node sn;
	struct sun_devinfo sd;
	int ret;

	if ((ret = sun_devinfo_init(maj, min, &sn, &sd)) != 0)
		return (ret);
	if (sun_parent_is_pci(sd.sd_parent))
		ret = DRM_BUS_PCI;
	else if (sun_parent_is_pseudo(sd.sd_parent))
		ret = DRM_BUS_FAUX;
	else
		ret = -EINVAL;
	sun_devinfo_fini(&sd);
	return (ret);
}

static int
sun_prop_int(di_node_t node, const char *name, int *valp)
{
	int *vals;

	if (di_prop_lookup_ints(DDI_DEV_T_ANY, node, name, &vals) < 1)
		return (-ENOENT);
	*valp = vals[0];
	return (0);
}

int
drmSunPciBusInfo(unsigned int maj, unsigned int min, drmPciBusInfoPtr info)
{
	struct sun_node sn;
	struct sun_devinfo sd;
	int *regs;
	int n, ret;

	if ((ret = sun_devinfo_init(maj, min, &sn, &sd)) != 0)
		return (ret);
	if (!sun_parent_is_pci(sd.sd_parent)) {
		sun_devinfo_fini(&sd);
		return (-EINVAL);
	}
	n = di_prop_lookup_ints(DDI_DEV_T_ANY, sd.sd_node, "reg", &regs);
	if (n < SUN_PCI_REG_INTS) {
		n = di_prop_lookup_ints(DDI_DEV_T_ANY, sd.sd_node,
		    "assigned-addresses", &regs);
	}
	if (n < SUN_PCI_REG_INTS) {
		sun_devinfo_fini(&sd);
		return (-ENOENT);
	}
	/*
	 * illumos numbers PCI buses per host bridge without a segment
	 * (domain) number; like OpenBSD's and DragonFly's libdrm, report
	 * domain 0.
	 */
	info->domain = 0;
	info->bus = SUN_PCI_REG_BUS((unsigned int)regs[0]);
	info->dev = SUN_PCI_REG_DEV((unsigned int)regs[0]);
	info->func = SUN_PCI_REG_FUNC((unsigned int)regs[0]);
	sun_devinfo_fini(&sd);
	return (0);
}

int
drmSunPciDeviceInfo(unsigned int maj, unsigned int min,
    drmPciDeviceInfoPtr device)
{
	struct sun_node sn;
	struct sun_devinfo sd;
	int vendor, dev, val, ret;

	if ((ret = sun_devinfo_init(maj, min, &sn, &sd)) != 0)
		return (ret);
	if (!sun_parent_is_pci(sd.sd_parent) ||
	    sun_prop_int(sd.sd_node, "vendor-id", &vendor) != 0 ||
	    sun_prop_int(sd.sd_node, "device-id", &dev) != 0) {
		sun_devinfo_fini(&sd);
		return (-EINVAL);
	}
	(void) memset(device, 0, sizeof (*device));
	device->vendor_id = (uint16_t)vendor;
	device->device_id = (uint16_t)dev;
	if (sun_prop_int(sd.sd_node, "subsystem-vendor-id", &val) == 0)
		device->subvendor_id = (uint16_t)val;
	if (sun_prop_int(sd.sd_node, "subsystem-id", &val) == 0)
		device->subdevice_id = (uint16_t)val;
	if (sun_prop_int(sd.sd_node, "revision-id", &val) == 0)
		device->revision_id = (uint8_t)val;
	sun_devinfo_fini(&sd);
	return (0);
}

int
drmSunFauxBusInfo(unsigned int maj, unsigned int min, char *name, size_t len)
{
	struct sun_node sn;
	struct sun_devinfo sd;
	const char *base;
	int ret;

	if ((ret = sun_devinfo_init(maj, min, &sn, &sd)) != 0)
		return (ret);
	sun_devinfo_fini(&sd);
	/* "/pseudo/rdrmnull@0" -> "rdrmnull@0" */
	base = strrchr(sn.sn_devfs, '/');
	base = (base == NULL) ? sn.sn_devfs : base + 1;
	if (*base == '\0')
		return (-ENOENT);
	(void) strlcpy(name, base, len);
	return (0);
}
