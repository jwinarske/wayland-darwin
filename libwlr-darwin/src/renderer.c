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
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/box.h>
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

struct wlr_darwin_metal_texture {
	struct wlr_texture base;
	struct wlr_darwin_metal_renderer *renderer;
	darwin_metal_texture *tex;
};

static const struct wlr_renderer_impl renderer_impl;
static const struct wlr_render_pass_impl pass_impl;
static const struct wlr_texture_impl texture_impl;

static struct wlr_darwin_metal_texture *darwin_texture_from_texture(
		struct wlr_texture *wlr_texture) {
	struct wlr_darwin_metal_texture *texture =
		wl_container_of(wlr_texture, texture, base);
	return texture;
}

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

/* Map an output-quad corner (cx,cy in {0,1}, top-left origin) to a normalized
 * texture coordinate under a wl_output_transform applied to the source. */
static void transform_corner(enum wl_output_transform tr, float cx, float cy,
		float *tx, float *ty) {
	switch (tr) {
	case WL_OUTPUT_TRANSFORM_90:          *tx = cy;     *ty = 1 - cx; break;
	case WL_OUTPUT_TRANSFORM_180:         *tx = 1 - cx; *ty = 1 - cy; break;
	case WL_OUTPUT_TRANSFORM_270:         *tx = 1 - cy; *ty = cx;     break;
	case WL_OUTPUT_TRANSFORM_FLIPPED:     *tx = 1 - cx; *ty = cy;     break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:  *tx = cy;     *ty = cx;     break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180: *tx = cx;     *ty = 1 - cy; break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270: *tx = 1 - cy; *ty = 1 - cx; break;
	case WL_OUTPUT_TRANSFORM_NORMAL:
	default:                              *tx = cx;     *ty = cy;     break;
	}
}

static void pass_add_texture(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_texture_options *options) {
	struct wlr_darwin_metal_pass *pass = wl_container_of(wlr_pass, pass, base);
	struct wlr_darwin_metal_texture *texture =
		darwin_texture_from_texture(options->texture);

	/* Destination box; width/height default to the texture size. */
	struct wlr_box dst = options->dst_box;
	if (dst.width == 0) {
		dst.width = options->texture->width;
	}
	if (dst.height == 0) {
		dst.height = options->texture->height;
	}

	/* Source box (texture pixels) -> normalized. Empty = whole texture. */
	float tw = options->texture->width, th = options->texture->height;
	float sx = 0.0f, sy = 0.0f, sw = 1.0f, sh = 1.0f;
	if (options->src_box.width > 0 && options->src_box.height > 0) {
		sx = options->src_box.x / tw;
		sy = options->src_box.y / th;
		sw = options->src_box.width / tw;
		sh = options->src_box.height / th;
	}

	/* Bake the transform into the four destination-corner UVs (TL,TR,BL,BR). */
	static const float corners[4][2] = { {0, 0}, {1, 0}, {0, 1}, {1, 1} };
	float uv[8];
	for (int i = 0; i < 4; i++) {
		float txc, tyc;
		transform_corner(options->transform, corners[i][0], corners[i][1],
			&txc, &tyc);
		uv[i * 2 + 0] = sx + txc * sw;
		uv[i * 2 + 1] = sy + tyc * sh;
	}

	float alpha = options->alpha != NULL ? *options->alpha : 1.0f;
	int nearest = options->filter_mode == WLR_SCALE_FILTER_NEAREST;

	darwin_metal_pass_texture(pass->pass, texture->tex,
		dst.x, dst.y, dst.width, dst.height, uv, alpha, nearest);
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

/* -- texture -- */

static bool texture_update_from_buffer(struct wlr_texture *wlr_texture,
		struct wlr_buffer *buffer, const pixman_region32_t *damage) {
	struct wlr_darwin_metal_texture *texture =
		darwin_texture_from_texture(wlr_texture);
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		return false;
	}
	/* Full re-upload; damage-region upload is a later optimization. */
	bool ok = darwin_metal_texture_update(texture->tex, data, (uint32_t)stride);
	wlr_buffer_end_data_ptr_access(buffer);
	return ok;
}

static bool texture_read_pixels(struct wlr_texture *wlr_texture,
		const struct wlr_texture_read_pixels_options *options) {
	struct wlr_darwin_metal_texture *texture =
		darwin_texture_from_texture(wlr_texture);
	struct wlr_box src = options->src_box;
	if (src.width == 0 || src.height == 0) {
		src.x = 0;
		src.y = 0;
		src.width = wlr_texture->width;
		src.height = wlr_texture->height;
	}
	uint8_t *dst = (uint8_t *)options->data +
		(size_t)options->dst_y * options->stride + (size_t)options->dst_x * 4;
	return darwin_metal_texture_read(texture->tex, dst, options->stride,
		src.x, src.y, src.width, src.height);
}

static uint32_t texture_preferred_read_format(struct wlr_texture *wlr_texture) {
	return DRM_FORMAT_XRGB8888;
}

static void texture_destroy(struct wlr_texture *wlr_texture) {
	struct wlr_darwin_metal_texture *texture =
		darwin_texture_from_texture(wlr_texture);
	darwin_metal_texture_destroy(texture->tex);
	free(texture);
}

static const struct wlr_texture_impl texture_impl = {
	.update_from_buffer = texture_update_from_buffer,
	.read_pixels = texture_read_pixels,
	.preferred_read_format = texture_preferred_read_format,
	.destroy = texture_destroy,
};

static struct wlr_texture *texture_from_buffer(struct wlr_renderer *wlr,
		struct wlr_buffer *buffer) {
	struct wlr_darwin_metal_renderer *r = renderer_from(wlr);

	darwin_metal_texture *mtex;
	darwin_iosurface *surface = darwin_buffer_get_iosurface(buffer);
	if (surface != NULL) {
		/* Zero-copy: sample the IOSurface directly, no upload. */
		mtex = darwin_metal_texture_from_iosurface(r->metal,
			darwin_iosurface_ref(surface), buffer->width, buffer->height);
	} else {
		void *data;
		uint32_t format;
		size_t stride;
		if (!wlr_buffer_begin_data_ptr_access(buffer,
				WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
			wlr_log(WLR_ERROR, "Metal: client buffer is not CPU-readable");
			return NULL;
		}
		mtex = darwin_metal_texture_create(r->metal, buffer->width,
			buffer->height, format, data, (uint32_t)stride);
		wlr_buffer_end_data_ptr_access(buffer);
	}
	if (mtex == NULL) {
		return NULL;
	}

	struct wlr_darwin_metal_texture *texture = calloc(1, sizeof(*texture));
	if (texture == NULL) {
		darwin_metal_texture_destroy(mtex);
		return NULL;
	}
	texture->renderer = r;
	texture->tex = mtex;
	wlr_texture_init(&texture->base, &r->base, &texture_impl,
		buffer->width, buffer->height);
	return &texture->base;
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
