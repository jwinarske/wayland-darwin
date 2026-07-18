/*
 * Headless render smoke for wlroots on macOS (Darwin).
 *
 * Proves the software compositing path is usable at runtime on Darwin, not just
 * linkable: it stands up the always-built headless backend + pixman renderer +
 * shm allocator (the D2 phase-1 stack), creates a 640x480 output, and renders
 * and commits one frame. This exercises the libdrm-compat shim live (format-set
 * setup calls drmGetFormatName; allocator autocreate relies on drmGetDevices2
 * returning 0 so it picks the shm allocator).
 *
 * Exit status 0 = PASS.
 */
#include <stdio.h>
#include <stdlib.h>

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#define CHECK(cond, msg) do { if (!(cond)) { \
	fprintf(stderr, "FAIL: %s\n", (msg)); return 1; } } while (0)

int
main(void)
{
	wlr_log_init(WLR_ERROR, NULL);

	struct wl_display *display = wl_display_create();
	CHECK(display, "wl_display_create");
	struct wl_event_loop *loop = wl_display_get_event_loop(display);

	struct wlr_backend *backend = wlr_headless_backend_create(loop);
	CHECK(backend, "wlr_headless_backend_create");

	struct wlr_renderer *renderer = wlr_pixman_renderer_create();
	CHECK(renderer, "wlr_pixman_renderer_create");

	struct wlr_allocator *allocator = wlr_allocator_autocreate(backend, renderer);
	CHECK(allocator, "wlr_allocator_autocreate (should pick shm)");

	struct wlr_output *output = wlr_headless_add_output(backend, 640, 480);
	CHECK(output, "wlr_headless_add_output");
	CHECK(wlr_output_init_render(output, allocator, renderer), "wlr_output_init_render");
	CHECK(wlr_backend_start(backend), "wlr_backend_start");

	/* Render one frame: clear the output to a solid colour. */
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_render_pass *pass =
		wlr_output_begin_render_pass(output, &state, NULL);
	CHECK(pass, "wlr_output_begin_render_pass");

	struct wlr_render_rect_options rect = {
		.box = { .x = 0, .y = 0, .width = 640, .height = 480 },
		.color = { .r = 0.10f, .g = 0.20f, .b = 0.30f, .a = 1.0f },
	};
	wlr_render_pass_add_rect(pass, &rect);
	CHECK(wlr_render_pass_submit(pass), "wlr_render_pass_submit");

	bool committed = wlr_output_commit_state(output, &state);
	wlr_output_state_finish(&state);
	CHECK(committed, "wlr_output_commit_state");

	printf("headless render smoke: PASS "
	       "(headless + pixman + shm, 640x480 frame committed)\n");

	wlr_allocator_destroy(allocator);
	wlr_renderer_destroy(renderer);
	wlr_backend_destroy(backend);
	wl_display_destroy(display);
	return 0;
}
