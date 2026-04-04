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

#ifndef	_DRM_SUN_PRINT_H
#define	_DRM_SUN_PRINT_H

/*
 * Modern DRM per-device logging macros.
 *
 * Linux 5.x+ DRM code uses drm_dbg(dev, ...), drm_err(dev, ...), etc.
 * instead of the older DRM_DEBUG/DRM_ERROR macros. Map them to the
 * existing drm_debug_print infrastructure defined in drmP.h.
 *
 * The DRM_ERROR, DRM_INFO, DRM_DEBUG macros already exist in drmP.h.
 * This header adds the modern per-device variants and rate-limited forms.
 */

#include <sys/cmn_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-device logging.  The 'dev' argument is ignored (we don't have
 * per-device debug categories on illumos); logs go through cmn_err.
 */
#define	drm_err(dev, fmt, ...)		\
	DRM_ERROR(fmt, ##__VA_ARGS__)

#define	drm_warn(dev, fmt, ...)		\
	DRM_ERROR(fmt, ##__VA_ARGS__)

#define	drm_info(dev, fmt, ...)		\
	DRM_INFO(fmt, ##__VA_ARGS__)

#define	drm_notice(dev, fmt, ...)	\
	DRM_INFO(fmt, ##__VA_ARGS__)

#define	drm_dbg(dev, fmt, ...)		\
	DRM_DEBUG(fmt, ##__VA_ARGS__)

#define	drm_dbg_kms(dev, fmt, ...)	\
	DRM_DEBUG_KMS(fmt, ##__VA_ARGS__)

#define	drm_dbg_driver(dev, fmt, ...)	\
	DRM_DEBUG_DRIVER(fmt, ##__VA_ARGS__)

/*
 * Rate-limited variants.  On illumos, we use a simple static counter
 * to suppress repeated messages.  Prints once per 16 calls.
 */
#define	DRM_ERROR_RATELIMITED(fmt, ...) do {		\
	static uint32_t _rl_count;			\
	if ((_rl_count++ & 0xF) == 0)			\
		DRM_ERROR(fmt, ##__VA_ARGS__);		\
} while (0)

#define	DRM_DEBUG_RATELIMITED(fmt, ...) do {		\
	static uint32_t _rl_count;			\
	if ((_rl_count++ & 0xF) == 0)			\
		DRM_DEBUG(fmt, ##__VA_ARGS__);		\
} while (0)

#define	drm_err_ratelimited(dev, fmt, ...)	\
	DRM_ERROR_RATELIMITED(fmt, ##__VA_ARGS__)

/*
 * dev_err / dev_warn / dev_info -- DDI device logging.
 * Linux uses these for generic device messages.  On illumos, map to cmn_err.
 * The 'dev' argument is a struct device * (ignored).
 */
#ifndef dev_err
#define	dev_err(dev, fmt, ...)		cmn_err(CE_WARN, fmt, ##__VA_ARGS__)
#define	dev_warn(dev, fmt, ...)		cmn_err(CE_WARN, fmt, ##__VA_ARGS__)
#define	dev_info(dev, fmt, ...)		cmn_err(CE_NOTE, fmt, ##__VA_ARGS__)
#define	dev_dbg(dev, fmt, ...)		/* nothing */
#endif

/*
 * WARN_ON_ONCE -- warn only on the first occurrence.
 */
#ifndef WARN_ON_ONCE
#define	WARN_ON_ONCE(cond) ({			\
	static boolean_t _warned;		\
	int _c = (cond);			\
	if (_c && !_warned) {			\
		_warned = B_TRUE;		\
		WARN_ON(1);			\
	}					\
	_c;					\
})
#endif

/*
 * WARN_ONCE -- same as WARN_ON_ONCE but with a message.
 */
#ifndef WARN_ONCE
#define	WARN_ONCE(cond, fmt, ...) ({		\
	static boolean_t _warned;		\
	int _c = (cond);			\
	if (_c && !_warned) {			\
		_warned = B_TRUE;		\
		DRM_ERROR(fmt, ##__VA_ARGS__);	\
	}					\
	_c;					\
})
#endif

#ifdef __cplusplus
}
#endif

#endif /* _DRM_SUN_PRINT_H */
