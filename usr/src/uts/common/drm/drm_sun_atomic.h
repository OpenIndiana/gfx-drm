/*
 * Copyright (c) 2026 Toasty.  All rights reserved.
 *
 * Minimal atomic modesetting shim for illumos.
 *
 * The virtio-gpu driver requires atomic modesetting but only uses a small
 * subset of the Linux atomic helper functions.  Rather than porting the
 * entire drm_atomic.c + drm_atomic_helper.c (~4,500 lines), we provide
 * thin stubs that satisfy the driver's needs.
 *
 * For virtio-gpu, atomic operations are simple: there's no multi-CRTC
 * transactions, no async commits, no complex state validation.  The
 * driver just needs set_config and page_flip to work.
 *
 * Functions provided as no-ops or trivial implementations:
 *   - State alloc/duplicate/destroy for CRTC, connector, plane
 *   - drm_atomic_helper_set_config -> calls crtc_helper mode_set
 *   - drm_atomic_helper_page_flip -> calls plane update
 *   - drm_atomic_helper_check -> returns 0
 *   - drm_atomic_helper_commit -> direct synchronous commit
 *   - drm_atomic_helper_dirtyfb -> calls plane atomic_update
 */

#ifndef	_DRM_SUN_ATOMIC_H
#define	_DRM_SUN_ATOMIC_H

#include "drmP.h"

/*
 * Minimal atomic state structures.
 * In full Linux these are complex, but for virtio-gpu we only need
 * enough to pass state between check and commit.
 */
struct drm_atomic_state {
	struct drm_device	*dev;
	/* Simplified: single CRTC/plane/connector state */
};

struct drm_crtc_state {
	struct drm_crtc		*crtc;
	boolean_t		enable;
	boolean_t		active;
	boolean_t		mode_changed;
	struct drm_display_mode	mode;
	struct drm_pending_vblank_event *event;
};

struct drm_plane_state {
	struct drm_plane	*plane;
	struct drm_crtc		*crtc;
	struct drm_framebuffer	*fb;
	int			crtc_x, crtc_y;
	unsigned int		crtc_w, crtc_h;
	uint32_t		src_x, src_y;
	uint32_t		src_w, src_h;
};

struct drm_connector_state {
	struct drm_connector	*connector;
	struct drm_crtc		*crtc;
};

/*
 * Atomic helper functions -- minimal implementations.
 * These are implemented as static inlines or in the display file.
 */

/* CRTC state management */
static inline void
drm_atomic_helper_crtc_reset(struct drm_crtc *crtc)
{
	/* No state to reset for our minimal implementation */
}

static inline struct drm_crtc_state *
drm_atomic_helper_crtc_duplicate_state(struct drm_crtc *crtc)
{
	return (NULL); /* Not used in our path */
}

static inline void
drm_atomic_helper_crtc_destroy_state(struct drm_crtc *crtc,
    struct drm_crtc_state *state)
{
	/* Nothing to free */
}

/* Connector state management */
static inline void
drm_atomic_helper_connector_reset(struct drm_connector *connector)
{
}

static inline struct drm_connector_state *
drm_atomic_helper_connector_duplicate_state(struct drm_connector *connector)
{
	return (NULL);
}

static inline void
drm_atomic_helper_connector_destroy_state(struct drm_connector *connector,
    struct drm_connector_state *state)
{
}

/* Plane state management */
static inline void
drm_atomic_helper_plane_reset(struct drm_plane *plane)
{
}

static inline void
drm_atomic_helper_plane_destroy_state(struct drm_plane *plane,
    struct drm_plane_state *state)
{
}

/*
 * Core atomic operations -- these need real implementations.
 * Provided as function declarations, implemented in virtgpu_display.c.
 */
extern int drm_atomic_helper_set_config(struct drm_mode_set *set);
extern int drm_atomic_helper_page_flip(struct drm_crtc *crtc,
    struct drm_framebuffer *fb,
    struct drm_pending_vblank_event *event,
    uint32_t flags);
extern int drm_atomic_helper_check(struct drm_device *dev,
    struct drm_atomic_state *state);
extern int drm_atomic_helper_commit(struct drm_device *dev,
    struct drm_atomic_state *state, boolean_t nonblock);
extern int drm_atomic_helper_dirtyfb(struct drm_framebuffer *fb,
    struct drm_file *file, unsigned flags, unsigned color,
    struct drm_clip_rect *clips, unsigned num_clips);

/*
 * Atomic state query helpers -- return NULL since we don't track state.
 */
static inline struct drm_crtc_state *
drm_atomic_get_new_crtc_state(struct drm_atomic_state *state,
    struct drm_crtc *crtc)
{
	return (NULL);
}

static inline boolean_t
drm_atomic_crtc_needs_modeset(struct drm_crtc_state *state)
{
	if (state == NULL)
		return (B_FALSE);
	return (state->mode_changed);
}

/*
 * Additional helpers used by the display code.
 */

/* Simple encoder: just an encoder with no special logic */
static inline int
drm_simple_encoder_init(struct drm_device *dev, struct drm_encoder *encoder,
    int encoder_type)
{
	return (drm_encoder_init(dev, encoder, NULL, encoder_type));
}

/* Connector EDID property */
static inline void
drm_connector_attach_edid_property(struct drm_connector *connector)
{
	/* EDID property support deferred */
}

/* Connector-encoder attachment */
static inline int
drm_connector_attach_encoder(struct drm_connector *connector,
    struct drm_encoder *encoder)
{
	/* Use the 3.14 API: drm_mode_connector_attach_encoder */
	return (drm_mode_connector_attach_encoder(connector, encoder));
}

/* CRTC init with planes (modern API, wraps 3.14 drm_crtc_init) */
static inline int
drm_crtc_init_with_planes(struct drm_device *dev, struct drm_crtc *crtc,
    struct drm_plane *primary, struct drm_plane *cursor,
    const struct drm_crtc_funcs *funcs, const char *name)
{
	/* 3.14 API doesn't take planes/name, just init the CRTC */
	return (drm_crtc_init(dev, crtc, funcs));
}

/* Mode config managed init (wraps 3.14 drm_mode_config_init) */
static inline int
drmm_mode_config_init(struct drm_device *dev)
{
	drm_mode_config_init(dev);
	return (0);
}

/* Mode config reset (no-op for our simplified state) */
static inline void
drm_mode_config_reset(struct drm_device *dev)
{
	/* No atomic state to reset */
}

/* VBlank timer funcs macro (no-op for minimal shim) */
#define	DRM_CRTC_VBLANK_TIMER_FUNCS

/* GEM framebuffer helpers */
static inline int
drm_gem_fb_create_handle(struct drm_framebuffer *fb, struct drm_file *file,
    unsigned int *handle)
{
	return (drm_gem_handle_create(file, fb->obj[0], handle));
}

static inline void
drm_gem_fb_destroy(struct drm_framebuffer *fb)
{
	if (fb->obj[0] != NULL)
		drm_gem_object_put(fb->obj[0]);
	drm_framebuffer_cleanup(fb);
	kfree(fb);
}

/* EDID helpers */
static inline int
drm_edid_connector_add_modes(struct drm_connector *connector)
{
	/* No EDID support in minimal shim */
	return (0);
}

static inline void
drm_edid_free(const void *edid)
{
	/* No-op */
}

#endif /* _DRM_SUN_ATOMIC_H */
