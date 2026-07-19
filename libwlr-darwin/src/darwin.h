/* Internal definitions shared by backend.c and output.c. */
#ifndef WLR_DARWIN_INTERNAL_H
#define WLR_DARWIN_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>

#include "cocoa.h"
#include "input.h"

struct wlr_darwin_backend {
	struct wlr_backend backend;
	struct wl_event_loop *loop;
	struct wl_list outputs; // wlr_darwin_output.link
	bool started;
	size_t last_output_num;

	/* One virtual keyboard + pointer per backend (D5c). */
	struct wlr_keyboard keyboard;
	struct wlr_pointer pointer;

	/*
	 * D3 thread bridge: cocoa.m (main thread) posts serialized input events
	 * into event_fd[1]; the compositor loop reads event_fd[0]. input_buf holds
	 * a partially-read record across reads.
	 */
	int event_fd[2];
	struct wl_event_source *event_source;
	uint8_t input_buf[sizeof(struct darwin_input_event)];
	size_t input_buf_len;

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

	/* NSWindow resize / backing-scale change (cocoa.m -> compositor). */
	int resize_fd[2];
	struct wl_event_source *resize_source;
};

struct wlr_darwin_backend *darwin_backend_from_backend(struct wlr_backend *b);
struct wlr_output *darwin_add_output(struct wlr_darwin_backend *backend,
	unsigned int width, unsigned int height);

extern const struct wlr_output_impl darwin_output_impl;

/* allocator.c: returns the IOSurface if `buffer` is one of ours, else NULL. */
darwin_iosurface *darwin_buffer_get_iosurface(struct wlr_buffer *buffer);

#endif
