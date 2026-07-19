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

	/*
	 * One virtual keyboard for the backend (keyboard focus is compositor-
	 * managed, not output-relative). The pointer is per-output — each window is
	 * its own output, and absolute motion is relative to that window — mirroring
	 * the nested X11 backend.
	 */
	struct wlr_keyboard keyboard;

	struct wl_listener event_loop_destroy;
};

struct wlr_darwin_output {
	struct wlr_output wlr_output;
	struct wlr_darwin_backend *backend;
	struct wl_list link;

	darwin_cocoa_window *window; // main-thread owned

	/* Per-output pointer; output_name binds its absolute motion to this output. */
	struct wlr_pointer pointer;

	/*
	 * Thread bridge: cocoa.m (main thread) posts this window's serialized
	 * input events into input_fd[1]; the compositor loop reads input_fd[0].
	 * input_buf holds a partially-read record across reads. Key events are
	 * routed to the backend keyboard; pointer events to this output's pointer.
	 */
	int input_fd[2];
	struct wl_event_source *input_source;
	uint8_t input_buf[sizeof(struct darwin_input_event)];
	size_t input_buf_len;

	/*
	 * Frame clock. The real clock is CADisplayLink in cocoa.m writing a
	 * struct darwin_frame_info to frame_fd[1]; frame_source (on frame_fd[0])
	 * then sends the frame + presentation-feedback events. A plain timer is
	 * kept as a debug fallback.
	 */
	int frame_fd[2];
	struct wl_event_source *frame_source;
	struct wl_event_source *frame_timer;
	int frame_delay_ms;

	/*
	 * A committed frame awaiting presentation feedback. Set on commit, drained
	 * on the next vsync tick (which is when the frame turned to light).
	 */
	bool present_pending;
	uint32_t present_commit_seq;
	bool present_zero_copy;

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
