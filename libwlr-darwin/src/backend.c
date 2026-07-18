#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>

#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/util/log.h>

#include "wlr-darwin.h"
#include "darwin.h"

static const struct wlr_backend_impl backend_impl;

struct wlr_darwin_backend *darwin_backend_from_backend(
		struct wlr_backend *wlr_backend) {
	assert(wlr_backend->impl == &backend_impl);
	struct wlr_darwin_backend *backend =
		wl_container_of(wlr_backend, backend, backend);
	return backend;
}

/*
 * D3 bridge: drain input events posted by cocoa.m from the main thread.
 * The wire format and translation to wlr_pointer/wlr_keyboard events is W5;
 * for now we just keep the pipe drained so the loop stays responsive.
 */
static int handle_events(int fd, uint32_t mask, void *data) {
	char buf[256];
	ssize_t n;
	do {
		n = read(fd, buf, sizeof(buf));
	} while (n == (ssize_t)sizeof(buf));
	return 0;
}

static bool backend_start(struct wlr_backend *wlr_backend) {
	struct wlr_darwin_backend *backend =
		darwin_backend_from_backend(wlr_backend);
	wlr_log(WLR_INFO, "Starting Darwin (Cocoa) backend");

	/* Create the initial output if the compositor didn't add one. */
	if (wl_list_empty(&backend->outputs)) {
		darwin_add_output(backend, 800, 600);
	}

	backend->started = true;

	struct wlr_darwin_output *output;
	wl_list_for_each(output, &backend->outputs, link) {
		wl_signal_emit_mutable(&backend->backend.events.new_output,
			&output->wlr_output);
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

	wlr_backend_finish(wlr_backend);

	wl_list_remove(&backend->event_loop_destroy.link);
	if (backend->event_source) {
		wl_event_source_remove(backend->event_source);
	}
	if (backend->event_fd[0] >= 0) {
		close(backend->event_fd[0]);
	}
	if (backend->event_fd[1] >= 0) {
		close(backend->event_fd[1]);
	}
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
	backend->event_fd[0] = backend->event_fd[1] = -1;

	/* main(AppKit) -> compositor input bridge (D3). */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, backend->event_fd) != 0) {
		wlr_log_errno(WLR_ERROR, "socketpair failed");
		free(backend);
		return NULL;
	}
	backend->event_source = wl_event_loop_add_fd(loop, backend->event_fd[0],
		WL_EVENT_READABLE, handle_events, backend);

	/* Software path for now; caps grow an IOSurface token in W4/W6. */
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
