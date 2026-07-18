/*
 * Metal renderer smoke — HEADLESS, so CI actually runs it.
 *
 * Phase 1: render a solid red rect into an IOSurface, read it back (add_rect).
 * Phase 2: upload a green texture, composite it over the target, read it back
 *          (texture_from_buffer + add_texture — the client-surface path).
 *
 * Proves the accelerated path renders correctly on the GPU into the exact
 * surface the backend presents. Exit 0 = PASS (or SKIP if no Metal device).
 */
#include <stdint.h>
#include <stdio.h>

#include <drm_fourcc.h>
#include <wayland-server-core.h>
#include <wlr-darwin.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

static struct wlr_buffer *make_buffer(struct wlr_allocator *alloc) {
	uint64_t mods[] = { DRM_FORMAT_MOD_LINEAR };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888, .len = 1, .capacity = 1, .modifiers = mods,
	};
	return wlr_allocator_create_buffer(alloc, 64, 64, &format);
}

/* Read BGRA of the top-left pixel. */
static bool read_pixel(struct wlr_buffer *buffer, uint8_t out[4]) {
	void *data;
	uint32_t fmt;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt, &stride)) {
		return false;
	}
	const uint8_t *px = data;
	for (int i = 0; i < 4; i++) {
		out[i] = px[i];
	}
	wlr_buffer_end_data_ptr_access(buffer);
	return true;
}

int
main(void)
{
	wlr_log_init(WLR_ERROR, NULL);

	struct wlr_renderer *renderer = wlr_darwin_metal_renderer_create();
	if (renderer == NULL) {
		printf("metal smoke: SKIP (no Metal device on this runner)\n");
		return 0;
	}
	struct wlr_allocator *alloc = wlr_darwin_allocator_create();
	struct wlr_buffer *target = make_buffer(alloc);
	if (alloc == NULL || target == NULL) {
		fprintf(stderr, "FAIL: setup\n");
		return 1;
	}

	/* ---- phase 1: solid red rect ---- */
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer, target, NULL);
	if (pass == NULL) { fprintf(stderr, "FAIL: begin\n"); return 1; }
	struct wlr_render_rect_options rect = {
		.box = { 0, 0, 64, 64 },
		.color = { .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f },
	};
	wlr_render_pass_add_rect(pass, &rect);
	if (!wlr_render_pass_submit(pass)) { fprintf(stderr, "FAIL: submit1\n"); return 1; }

	uint8_t p1[4];
	if (!read_pixel(target, p1)) { fprintf(stderr, "FAIL: read1\n"); return 1; }
	int red_ok = p1[2] > 200 && p1[1] < 60 && p1[0] < 60;
	printf("phase 1 (rect)    BGRA = %u,%u,%u,%u\n", p1[0], p1[1], p1[2], p1[3]);

	/* ---- phase 2: upload a green texture and composite it ---- */
	struct wlr_buffer *src = make_buffer(alloc);
	void *sdata;
	uint32_t sfmt;
	size_t sstride;
	if (src == NULL || !wlr_buffer_begin_data_ptr_access(src,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &sdata, &sfmt, &sstride)) {
		fprintf(stderr, "FAIL: src buffer\n");
		return 1;
	}
	uint8_t *sp = sdata;
	for (uint32_t y = 0; y < 64; y++) {
		for (uint32_t x = 0; x < 64; x++) {
			uint8_t *px = sp + y * sstride + x * 4; /* BGRA green */
			px[0] = 0; px[1] = 255; px[2] = 0; px[3] = 255;
		}
	}
	wlr_buffer_end_data_ptr_access(src);

	struct wlr_texture *tex = wlr_texture_from_buffer(renderer, src);
	if (tex == NULL) { fprintf(stderr, "FAIL: texture_from_buffer\n"); return 1; }

	struct wlr_render_pass *pass2 = wlr_renderer_begin_buffer_pass(renderer, target, NULL);
	struct wlr_render_texture_options topts = {
		.texture = tex,
		.dst_box = { 0, 0, 64, 64 },
	};
	wlr_render_pass_add_texture(pass2, &topts);
	if (!wlr_render_pass_submit(pass2)) { fprintf(stderr, "FAIL: submit2\n"); return 1; }

	uint8_t p2[4];
	if (!read_pixel(target, p2)) { fprintf(stderr, "FAIL: read2\n"); return 1; }
	int green_ok = p2[1] > 200 && p2[0] < 60 && p2[2] < 60;
	printf("phase 2 (texture) BGRA = %u,%u,%u,%u\n", p2[0], p2[1], p2[2], p2[3]);

	wlr_texture_destroy(tex);
	wlr_buffer_drop(src);
	wlr_buffer_drop(target);
	wlr_allocator_destroy(alloc);
	wlr_renderer_destroy(renderer);

	if (red_ok && green_ok) {
		printf("metal smoke: PASS (rect + textured client surface)\n");
		return 0;
	}
	printf("metal smoke: FAIL\n");
	return 1;
}
