/* Internal definitions shared by backend.c and output.c. */
#ifndef WLR_DARWIN_INTERNAL_H
#define WLR_DARWIN_INTERNAL_H

#include <stddef.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/interfaces/wlr_output.h>

#include "cocoa.h"

struct wlr_darwin_backend {
	struct wlr_backend backend;
	struct wl_event_loop *loop;
	struct wl_list outputs; // wlr_darwin_output.link
	bool started;
	size_t last_output_num;

	/*
	 * D3 thread bridge: cocoa.m (main thread) posts serialized input events
	 * into event_fd[1]; the compositor loop reads event_fd[0].
	 */
	int event_fd[2];
	struct wl_event_source *event_source;

	struct wl_listener event_loop_destroy;
};

struct wlr_darwin_output {
	struct wlr_output wlr_output;
	struct wlr_darwin_backend *backend;
	struct wl_list link;

	darwin_cocoa_window *window; // main-thread owned

	/*
	 * D6 frame clock. The real clock is CADisplayLink in cocoa.m writing to
	 * frame_fd[1]; frame_source (on frame_fd[0]) then calls send_frame. A
	 * plain timer is kept as a debug fallback.
	 */
	int frame_fd[2];
	struct wl_event_source *frame_source;
	struct wl_event_source *frame_timer;
	int frame_delay_ms;
};

struct wlr_darwin_backend *darwin_backend_from_backend(struct wlr_backend *b);
struct wlr_output *darwin_add_output(struct wlr_darwin_backend *backend,
	unsigned int width, unsigned int height);

extern const struct wlr_output_impl darwin_output_impl;

#endif
