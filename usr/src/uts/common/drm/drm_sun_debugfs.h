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

#ifndef	_DRM_SUN_DEBUGFS_H
#define	_DRM_SUN_DEBUGFS_H

/*
 * Linux debugfs stubs.
 *
 * illumos has no debugfs.  All debugfs-related functions become no-ops.
 * The virtio-gpu driver has a small virtgpu_debugfs.c that registers
 * debug info nodes; these compile to nothing on illumos.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque types that never get dereferenced */
struct dentry;
struct seq_file;

struct drm_info_list {
	const char	*name;
	int		(*show)(struct seq_file *, void *);
	uint32_t	driver_features;
	void		*data;
};

struct drm_info_node {
	struct drm_info_list	*info_ent;
	struct drm_minor	*minor;
};

/* seq_file stubs */
#define	seq_printf(m, fmt, ...)		/* nothing */
#define	seq_puts(m, s)			/* nothing */

/* debugfs creation stubs */
#define	debugfs_create_file(name, mode, parent, data, fops)	(NULL)
#define	debugfs_create_dir(name, parent)			(NULL)
#define	debugfs_remove(dentry)					/* nothing */
#define	debugfs_remove_recursive(dentry)				/* nothing */

/*
 * drm_debugfs_create_files -- register debug info with DRM core.
 * No-op on illumos.
 */
static inline void
drm_debugfs_create_files(const struct drm_info_list *files, int count,
    struct dentry *root, struct drm_minor *minor)
{
}

#ifdef __cplusplus
}
#endif

#endif /* _DRM_SUN_DEBUGFS_H */
