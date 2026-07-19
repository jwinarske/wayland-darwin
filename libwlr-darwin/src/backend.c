#include <assert.h>
#include <stdlib.h>

#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/util/log.h>

#include "wlr-darwin.h"
#include "darwin.h"

static const struct wlr_keyboard_impl keyboard_impl = {
	.name = "darwin-keyboard",
};

static const struct wlr_backend_impl backend_impl;

struct wlr_darwin_backend *darwin_backend_from_backend(
		struct wlr_backend *wlr_backend) {
	assert(wlr_backend->impl == &backend_impl);
	struct wlr_darwin_backend *backend =
		wl_container_of(wlr_backend, backend, backend);
	return backend;
}

static bool backend_start(struct wlr_backend *wlr_backend) {
	struct wlr_darwin_backend *backend =
		darwin_backend_from_backend(wlr_backend);
	wlr_log(WLR_INFO, "Starting Darwin (Cocoa) backend");

	/*
	 * If the compositor added no outputs itself, create WLR_DARWIN_OUTPUTS of
	 * them (default 1) — one NSWindow each — mirroring the X11 backend's
	 * WLR_X11_OUTPUTS. Each is an independent wlr_output with its own pointer.
	 */
	if (wl_list_empty(&backend->outputs)) {
		unsigned int count = 1;
		const char *env = getenv("WLR_DARWIN_OUTPUTS");
		if (env != NULL) {
			int v = atoi(env);
			if (v > 0) {
				count = (unsigned int)v;
			}
		}
		for (unsigned int i = 0; i < count; i++) {
			darwin_add_output(backend, 800, 600);
		}
	}

	backend->started = true;

	wl_signal_emit_mutable(&backend->backend.events.new_input,
		&backend->keyboard.base);

	struct wlr_darwin_output *output;
	wl_list_for_each(output, &backend->outputs, link) {
		wl_signal_emit_mutable(&backend->backend.events.new_output,
			&output->wlr_output);
		wl_signal_emit_mutable(&backend->backend.events.new_input,
			&output->pointer.base);
	}
	return true;
}

static void backend_destroy(struct wlr_backend *wlr_backend) {
	if (!wlr_backend) {
		return;
	}
	struct wlr_darwin_backend *backend =
		darwin_backend_from_backend(wlr_backend);

	struct wlr_darwin_output *output, *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &backend->outputs, link) {
		wlr_output_destroy(&output->wlr_output);
	}

	wlr_keyboard_finish(&backend->keyboard);

	wlr_backend_finish(wlr_backend);

	wl_list_remove(&backend->event_loop_destroy.link);
	free(backend);
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	// test/commit/get_drm_fd intentionally unimplemented: no DRM, and output
	// commits go through wlr_output_impl.commit (per-output present).
};

static void handle_event_loop_destroy(struct wl_listener *listener, void *data) {
	struct wlr_darwin_backend *backend =
		wl_container_of(listener, backend, event_loop_destroy);
	backend_destroy(&backend->backend);
}

struct wlr_backend *wlr_darwin_backend_create(struct wl_event_loop *loop) {
	wlr_log(WLR_INFO, "Creating Darwin (Cocoa) backend");

	struct wlr_darwin_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_darwin_backend");
		return NULL;
	}

	wlr_backend_init(&backend->backend, &backend_impl);
	backend->loop = loop;
	wl_list_init(&backend->outputs);

	/* One virtual keyboard for the backend; pointers are per-output. */
	wlr_keyboard_init(&backend->keyboard, &keyboard_impl, "darwin-keyboard");

	backend->backend.buffer_caps = WLR_BUFFER_CAP_DATA_PTR | WLR_BUFFER_CAP_SHM;

	backend->event_loop_destroy.notify = handle_event_loop_destroy;
	wl_event_loop_add_destroy_listener(loop, &backend->event_loop_destroy);

	return &backend->backend;
}

struct wlr_output *wlr_darwin_add_output(struct wlr_backend *wlr_backend,
		unsigned int width, unsigned int height) {
	struct wlr_darwin_backend *backend =
		darwin_backend_from_backend(wlr_backend);
	return darwin_add_output(backend, width, height);
}

bool wlr_backend_is_darwin(const struct wlr_backend *backend) {
	return backend->impl == &backend_impl;
}
