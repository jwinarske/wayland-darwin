/*
 * Metal renderer smoke — HEADLESS, so CI actually runs it.
 *
 * Creates the Metal renderer + IOSurface allocator, allocates a 64x64 buffer,
 * renders a solid red rect into it via a Metal render pass, then reads the
 * IOSurface back on the CPU and checks the pixel. Proves the accelerated path
 * (MTLDevice, pipeline, IOSurface render target, GPU->CPU coherency) works.
 *
 * Exit 0 = PASS (or SKIP if the runner has no Metal device).
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
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

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
	if (alloc == NULL) {
		fprintf(stderr, "FAIL: allocator\n");
		return 1;
	}

	uint64_t mods[] = { DRM_FORMAT_MOD_LINEAR };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1, .capacity = 1, .modifiers = mods,
	};
	struct wlr_buffer *buffer = wlr_allocator_create_buffer(alloc, 64, 64, &format);
	if (buffer == NULL) {
		fprintf(stderr, "FAIL: create_buffer\n");
		return 1;
	}

	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(renderer, buffer, NULL);
	if (pass == NULL) {
		fprintf(stderr, "FAIL: begin_buffer_pass\n");
		return 1;
	}
	struct wlr_render_rect_options rect = {
		.box = { 0, 0, 64, 64 },
		.color = { .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f },
	};
	wlr_render_pass_add_rect(pass, &rect);
	if (!wlr_render_pass_submit(pass)) {
		fprintf(stderr, "FAIL: submit\n");
		return 1;
	}

	/* Read the IOSurface back on the CPU. */
	void *data;
	uint32_t fmt;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt, &stride)) {
		fprintf(stderr, "FAIL: readback\n");
		return 1;
	}
	const uint8_t *px = data; /* BGRA */
	int ok = px[2] > 200 && px[1] < 60 && px[0] < 60;
	printf("center pixel BGRA = %u,%u,%u,%u\n", px[0], px[1], px[2], px[3]);
	wlr_buffer_end_data_ptr_access(buffer);

	wlr_buffer_drop(buffer);
	wlr_allocator_destroy(alloc);
	wlr_renderer_destroy(renderer);

	printf(ok ? "metal smoke: PASS (rendered red into IOSurface via Metal)\n"
		  : "metal smoke: FAIL (unexpected pixel)\n");
	return ok ? 0 : 1;
}
