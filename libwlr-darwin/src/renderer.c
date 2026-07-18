/*
 * Metal renderer (W6) — pure-C wlroots renderer/pass implementation that drives
 * Metal (metal.m) to render into IOSurface buffers.
 *
 * Increment 1: solid-colour rects into an IOSurface render target, plus format
 * negotiation and read-back-via-IOSurface. Texturing client surfaces
 * (add_texture / texture_from_buffer) is the next increment.
 *
 * Formats are LINEAR-only (A2): DRM XRGB8888 / ARGB8888 <-> MTLPixelFormatBGRA8Unorm.
 */
#include <assert.h>
#include <stdlib.h>

#include <drm_fourcc.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/interface.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

#include "wlr-darwin.h"
#include "darwin.h"
#include "metal.h"

struct wlr_darwin_metal_renderer {
	struct wlr_renderer base;
	darwin_metal *metal;
	struct wlr_drm_format_set formats;
};

struct wlr_darwin_metal_pass {
	struct wlr_render_pass base;
	struct wlr_buffer *buffer;
	darwin_metal_pass *pass;
};

static const struct wlr_renderer_impl renderer_impl;
static const struct wlr_render_pass_impl pass_impl;

static struct wlr_darwin_metal_renderer *renderer_from(struct wlr_renderer *wlr) {
	/* wlr_renderer.impl is private out-of-tree; base is the first member. */
	struct wlr_darwin_metal_renderer *r = wl_container_of(wlr, r, base);
	return r;
}

/* -- formats -- */

static void init_formats(struct wlr_drm_format_set *set) {
	const uint32_t fmts[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
	for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
		wlr_drm_format_set_add(set, fmts[i], DRM_FORMAT_MOD_LINEAR);
		wlr_drm_format_set_add(set, fmts[i], DRM_FORMAT_MOD_INVALID);
	}
}

static const struct wlr_drm_format_set *get_render_formats(
		struct wlr_renderer *wlr) {
	return &renderer_from(wlr)->formats;
}

static const struct wlr_drm_format_set *get_texture_formats(
		struct wlr_renderer *wlr, uint32_t buffer_caps) {
	return &renderer_from(wlr)->formats;
}

static int get_drm_fd(struct wlr_renderer *wlr) {
	return -1;
}

static void renderer_destroy(struct wlr_renderer *wlr) {
	struct wlr_darwin_metal_renderer *r = renderer_from(wlr);
	wlr_drm_format_set_finish(&r->formats);
	darwin_metal_destroy(r->metal);
	free(r);
}

/* -- render pass -- */

static void pass_add_rect(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_rect_options *options) {
	struct wlr_darwin_metal_pass *pass = wl_container_of(wlr_pass, pass, base);
	int blend = options->blend_mode != WLR_RENDER_BLEND_MODE_NONE;
	darwin_metal_pass_rect(pass->pass,
		options->box.x, options->box.y, options->box.width, options->box.height,
		options->color.r, options->color.g, options->color.b, options->color.a,
		blend);
}

static void pass_add_texture(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_texture_options *options) {
	/* TODO(increment 2): sample client textures onto a quad. */
}

static bool pass_submit(struct wlr_render_pass *wlr_pass) {
	struct wlr_darwin_metal_pass *pass = wl_container_of(wlr_pass, pass, base);
	bool ok = darwin_metal_pass_submit(pass->pass);
	wlr_buffer_unlock(pass->buffer);
	free(pass);
	return ok;
}

static const struct wlr_render_pass_impl pass_impl = {
	.submit = pass_submit,
	.add_texture = pass_add_texture,
	.add_rect = pass_add_rect,
};

static struct wlr_render_pass *begin_buffer_pass(struct wlr_renderer *wlr,
		struct wlr_buffer *buffer,
		const struct wlr_buffer_pass_options *options) {
	struct wlr_darwin_metal_renderer *r = renderer_from(wlr);

	darwin_iosurface *surface = darwin_buffer_get_iosurface(buffer);
	if (surface == NULL) {
		wlr_log(WLR_ERROR,
			"Metal renderer requires an IOSurface render target "
			"(use wlr_darwin_allocator_create)");
		return NULL;
	}

	darwin_metal_pass *mpass = darwin_metal_begin(r->metal,
		darwin_iosurface_ref(surface), buffer->width, buffer->height);
	if (mpass == NULL) {
		return NULL;
	}

	struct wlr_darwin_metal_pass *pass = calloc(1, sizeof(*pass));
	if (pass == NULL) {
		darwin_metal_pass_submit(mpass);
		return NULL;
	}
	wlr_render_pass_init(&pass->base, &pass_impl);
	pass->buffer = buffer;
	pass->pass = mpass;
	wlr_buffer_lock(buffer);
	return &pass->base;
}

static struct wlr_texture *texture_from_buffer(struct wlr_renderer *wlr,
		struct wlr_buffer *buffer) {
	/* TODO(increment 2): shm upload / IOSurface wrap for client textures. */
	return NULL;
}

static const struct wlr_renderer_impl renderer_impl = {
	.get_render_formats = get_render_formats,
	.get_texture_formats = get_texture_formats,
	.get_drm_fd = get_drm_fd,
	.destroy = renderer_destroy,
	.begin_buffer_pass = begin_buffer_pass,
	.texture_from_buffer = texture_from_buffer,
};

struct wlr_renderer *wlr_darwin_metal_renderer_create(void) {
	darwin_metal *metal = darwin_metal_create();
	if (metal == NULL) {
		wlr_log(WLR_ERROR, "Failed to create Metal device");
		return NULL;
	}

	struct wlr_darwin_metal_renderer *r = calloc(1, sizeof(*r));
	if (r == NULL) {
		darwin_metal_destroy(metal);
		return NULL;
	}
	r->metal = metal;
	init_formats(&r->formats);
	wlr_renderer_init(&r->base, &renderer_impl, WLR_BUFFER_CAP_DATA_PTR);
	return &r->base;
}
