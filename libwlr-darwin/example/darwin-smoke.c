/*
 * darwin-smoke — minimal libwlr-darwin compositor.
 *
 * Opens a native macOS window (a wlr_output) and renders a solid colour into it
 * every frame via the pixman renderer + shm allocator, presented through the
 * Cocoa backend. Exercises the whole W3 path: the application trampoline, the
 * compositor/main thread split, window creation, the frame clock, and present.
 *
 * This has no Wayland clients yet — it is the backend bring-up demo.
 */
#include <stdlib.h>

#include <wayland-server-core.h>
#include <wlr-darwin.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

struct demo {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wl_listener new_output;
};

struct demo_output {
	struct wlr_output *output;
	struct demo *demo;
	struct wl_listener frame;
};

static void output_frame(struct wl_listener *listener, void *data) {
	struct demo_output *o = wl_container_of(listener, o, frame);

	struct wlr_output_state state;
	wlr_output_state_init(&state);

	struct wlr_render_pass *pass =
		wlr_output_begin_render_pass(o->output, &state, NULL);
	if (pass != NULL) {
		struct wlr_render_rect_options rect = {
			.box = { 0, 0, o->output->width, o->output->height },
			.color = { .r = 0.10f, .g = 0.30f, .b = 0.60f, .a = 1.0f },
		};
		wlr_render_pass_add_rect(pass, &rect);
		wlr_render_pass_submit(pass);
		wlr_output_commit_state(o->output, &state);
	}
	wlr_output_state_finish(&state);
}

static void handle_new_output(struct wl_listener *listener, void *data) {
	struct demo *demo = wl_container_of(listener, demo, new_output);
	struct wlr_output *output = data;

	wlr_output_init_render(output, demo->allocator, demo->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_commit_state(output, &state);
	wlr_output_state_finish(&state);

	struct demo_output *o = calloc(1, sizeof(*o));
	o->output = output;
	o->demo = demo;
	o->frame.notify = output_frame;
	wl_signal_add(&output->events.frame, &o->frame);
}

static int compositor_main(void *data) {
	wlr_log_init(WLR_INFO, NULL);

	struct demo demo = {0};
	demo.display = wl_display_create();
	struct wl_event_loop *loop = wl_display_get_event_loop(demo.display);

	demo.backend = wlr_darwin_backend_create(loop);
	demo.renderer = wlr_pixman_renderer_create();
	demo.allocator = wlr_allocator_autocreate(demo.backend, demo.renderer);

	demo.new_output.notify = handle_new_output;
	wl_signal_add(&demo.backend->events.new_output, &demo.new_output);

	if (!wlr_backend_start(demo.backend)) {
		wlr_log(WLR_ERROR, "failed to start Darwin backend");
		return 1;
	}

	wl_display_run(demo.display);
	wl_display_destroy(demo.display);
	return 0;
}

int main(void) {
	return wlr_darwin_application_run(compositor_main, NULL);
}
