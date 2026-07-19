#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include <wayland-server-protocol.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "darwin.h"

#define DARWIN_DEFAULT_REFRESH 60000 // mHz

static const uint32_t SUPPORTED_OUTPUT_STATE =
	WLR_OUTPUT_STATE_BACKEND_OPTIONAL |
	WLR_OUTPUT_STATE_BUFFER |
	WLR_OUTPUT_STATE_ENABLED |
	WLR_OUTPUT_STATE_MODE |
	WLR_OUTPUT_STATE_SCALE;

static struct wlr_darwin_output *darwin_output_from_output(
		struct wlr_output *wlr_output) {
	assert(wlr_output->impl == &darwin_output_impl);
	struct wlr_darwin_output *output =
		wl_container_of(wlr_output, output, wlr_output);
	return output;
}

static bool output_test(struct wlr_output *wlr_output,
		const struct wlr_output_state *state) {
	uint32_t unsupported = state->committed & ~SUPPORTED_OUTPUT_STATE;
	if (unsupported != 0) {
		wlr_log(WLR_DEBUG, "Unsupported output state fields: 0x%"PRIx32,
			unsupported);
		return false;
	}
	if (state->committed & WLR_OUTPUT_STATE_MODE) {
		assert(state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM);
	}
	return true;
}

static void output_present_buffer(struct wlr_darwin_output *output,
		struct wlr_buffer *buffer) {
	/* Zero-copy: our own IOSurface buffer goes straight to the CALayer. */
	darwin_iosurface *surface = darwin_buffer_get_iosurface(buffer);
	if (surface != NULL) {
		darwin_cocoa_window_present_iosurface(output->window, surface);
		return;
	}

	/* Copy fallback for foreign buffers (e.g. the plain shm allocator). */
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		wlr_log(WLR_ERROR, "Darwin output: buffer is not CPU-readable");
		return;
	}
	darwin_cocoa_window_present(output->window, data, buffer->width,
		buffer->height, (uint32_t)stride, format);
	wlr_buffer_end_data_ptr_access(buffer);
}

static bool output_commit(struct wlr_output *wlr_output,
		const struct wlr_output_state *state) {
	struct wlr_darwin_output *output = darwin_output_from_output(wlr_output);

	if (!output_test(wlr_output, state)) {
		return false;
	}

	if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
		output_present_buffer(output, state->buffer);

		/*
		 * The frame is handed to WindowServer now but turns to light at the
		 * next vsync. Defer the presentation-feedback event to that display-link
		 * tick (handle_frame) so it carries the real timestamp and vsync count.
		 * commit_seq is this commit's — wlroots increments it after impl.commit.
		 */
		output->present_pending = true;
		output->present_commit_seq = wlr_output->commit_seq + 1;
		output->present_zero_copy =
			darwin_buffer_get_iosurface(state->buffer) != NULL;
	}

	return true;
}

/*
 * Hardware cursor: hand the cursor buffer to a dedicated overlay CALayer
 * (cocoa.m) instead of compositing it into the frame, so cursor motion never
 * redraws the scene. Zero-copy for our own IOSurface buffers; copy fallback for
 * foreign ones. hotspot_x/y and the position are in output backing pixels.
 */
static bool output_set_cursor(struct wlr_output *wlr_output,
		struct wlr_buffer *buffer, int hotspot_x, int hotspot_y) {
	struct wlr_darwin_output *output = darwin_output_from_output(wlr_output);

	if (buffer == NULL) {
		darwin_cocoa_window_set_cursor_surface(output->window, NULL,
			0, 0, 0, 0);
		return true;
	}

	darwin_iosurface *surface = darwin_buffer_get_iosurface(buffer);
	if (surface != NULL) {
		darwin_cocoa_window_set_cursor_surface(output->window, surface,
			buffer->width, buffer->height, hotspot_x, hotspot_y);
		return true;
	}

	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		wlr_log(WLR_ERROR, "Darwin cursor: buffer is not CPU-readable");
		return false;
	}
	darwin_cocoa_window_set_cursor_pixels(output->window, data, buffer->width,
		buffer->height, (int)stride, format, hotspot_x, hotspot_y);
	wlr_buffer_end_data_ptr_access(buffer);
	return true;
}

static bool output_move_cursor(struct wlr_output *wlr_output, int x, int y) {
	struct wlr_darwin_output *output = darwin_output_from_output(wlr_output);
	darwin_cocoa_window_move_cursor(output->window, x, y);
	return true;
}

static void output_destroy(struct wlr_output *wlr_output) {
	struct wlr_darwin_output *output = darwin_output_from_output(wlr_output);

	if (output->window) {
		darwin_cocoa_window_destroy(output->window);
	}
	if (output->input_source) {
		wl_event_source_remove(output->input_source);
	}
	if (output->frame_source) {
		wl_event_source_remove(output->frame_source);
	}
	if (output->resize_source) {
		wl_event_source_remove(output->resize_source);
	}
	if (output->frame_timer) {
		wl_event_source_remove(output->frame_timer);
	}
	for (int i = 0; i < 2; i++) {
		if (output->input_fd[i] >= 0) {
			close(output->input_fd[i]);
		}
		if (output->frame_fd[i] >= 0) {
			close(output->frame_fd[i]);
		}
		if (output->resize_fd[i] >= 0) {
			close(output->resize_fd[i]);
		}
	}

	wlr_pointer_finish(&output->pointer);
	wl_list_remove(&output->link);
	wlr_output_finish(wlr_output);
	free(output);
}

const struct wlr_output_impl darwin_output_impl = {
	.destroy = output_destroy,
	.test = output_test,
	.commit = output_commit,
	.set_cursor = output_set_cursor,
	.move_cursor = output_move_cursor,
};

bool wlr_output_is_darwin(const struct wlr_output *wlr_output) {
	return wlr_output->impl == &darwin_output_impl;
}

static const struct wlr_pointer_impl pointer_impl = {
	.name = "darwin-pointer",
};

/*
 * Bridge: translate one decoded input record into wlr input events. Key
 * events go to the backend's single keyboard; pointer events to this output's
 * own pointer (output_name binds absolute motion to the right window).
 */
static void dispatch_input_event(struct wlr_darwin_output *output,
		const struct darwin_input_event *ev) {
	struct wlr_pointer *pointer = &output->pointer;
	switch (ev->type) {
	case DARWIN_INPUT_KEY:; {
		struct wlr_keyboard_key_event key = {
			.time_msec = ev->time_msec,
			.keycode = ev->code,
			.update_state = true,
			.state = ev->state ? WL_KEYBOARD_KEY_STATE_PRESSED
					   : WL_KEYBOARD_KEY_STATE_RELEASED,
		};
		wlr_keyboard_notify_key(&output->backend->keyboard, &key);
		break;
	}
	case DARWIN_INPUT_MOTION_ABS:; {
		struct wlr_pointer_motion_absolute_event motion = {
			.pointer = pointer,
			.time_msec = ev->time_msec,
			.x = ev->x,
			.y = ev->y,
		};
		wl_signal_emit_mutable(&pointer->events.motion_absolute, &motion);
		wl_signal_emit_mutable(&pointer->events.frame, pointer);
		break;
	}
	case DARWIN_INPUT_BUTTON:; {
		struct wlr_pointer_button_event button = {
			.pointer = pointer,
			.time_msec = ev->time_msec,
			.button = ev->code,
			.state = ev->state ? WL_POINTER_BUTTON_STATE_PRESSED
					   : WL_POINTER_BUTTON_STATE_RELEASED,
		};
		wl_signal_emit_mutable(&pointer->events.button, &button);
		wl_signal_emit_mutable(&pointer->events.frame, pointer);
		break;
	}
	case DARWIN_INPUT_AXIS:; {
		struct wlr_pointer_axis_event axis = {
			.pointer = pointer,
			.time_msec = ev->time_msec,
			.source = ev->aux,
			.orientation = ev->state,
			.relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
			.delta = ev->x,
			.delta_discrete = ev->discrete,
		};
		wl_signal_emit_mutable(&pointer->events.axis, &axis);
		wl_signal_emit_mutable(&pointer->events.frame, pointer);
		break;
	}
	case DARWIN_INPUT_PINCH_BEGIN:; {
		struct wlr_pointer_pinch_begin_event begin = {
			.pointer = pointer,
			.time_msec = ev->time_msec,
			.fingers = ev->code,
		};
		wl_signal_emit_mutable(&pointer->events.pinch_begin, &begin);
		break;
	}
	case DARWIN_INPUT_PINCH_UPDATE:; {
		struct wlr_pointer_pinch_update_event update = {
			.pointer = pointer,
			.time_msec = ev->time_msec,
			.fingers = ev->code,
			.dx = ev->x,
			.dy = ev->y,
			.scale = ev->f0,
			.rotation = ev->f1,
		};
		wl_signal_emit_mutable(&pointer->events.pinch_update, &update);
		break;
	}
	case DARWIN_INPUT_PINCH_END:; {
		struct wlr_pointer_pinch_end_event end = {
			.pointer = pointer,
			.time_msec = ev->time_msec,
			.cancelled = false,
		};
		wl_signal_emit_mutable(&pointer->events.pinch_end, &end);
		break;
	}
	}
}

/* Read fixed-size records off this output's bridge fd, handling partial reads. */
static int handle_input(int fd, uint32_t mask, void *data) {
	struct wlr_darwin_output *output = data;
	uint8_t chunk[4096];
	ssize_t n = read(fd, chunk, sizeof(chunk));
	if (n <= 0) {
		return 0;
	}

	const size_t rec = sizeof(struct darwin_input_event);
	size_t off = 0;
	while (off < (size_t)n) {
		size_t need = rec - output->input_buf_len;
		size_t avail = (size_t)n - off;
		size_t take = avail < need ? avail : need;
		memcpy(output->input_buf + output->input_buf_len, chunk + off, take);
		output->input_buf_len += take;
		off += take;
		if (output->input_buf_len == rec) {
			struct darwin_input_event ev;
			memcpy(&ev, output->input_buf, rec);
			output->input_buf_len = 0;
			dispatch_input_event(output, &ev);
		}
	}
	return 0;
}

/*
 * Frame clock: cocoa.m's CVDisplayLink writes a struct darwin_frame_info per
 * vsync. Emit presentation feedback for the frame committed since the last tick
 * (it turned to light at this vsync), then request the next frame. Ticks are
 * coalesced — the most recent one carries the current timing.
 */
static int handle_frame(int fd, uint32_t mask, void *data) {
	struct wlr_darwin_output *output = data;
	struct darwin_frame_info info, last;
	bool have = false;
	while (read(fd, &info, sizeof(info)) == (ssize_t)sizeof(info)) {
		last = info;
		have = true;
	}
	if (!have) {
		return 0;
	}

	if (output->present_pending) {
		struct timespec when;
		uint32_t flags = WLR_OUTPUT_PRESENT_VSYNC;
		if (last.when_ns > 0) {
			when.tv_sec = last.when_ns / 1000000000;
			when.tv_nsec = last.when_ns % 1000000000;
			flags |= WLR_OUTPUT_PRESENT_HW_CLOCK;
		} else {
			clock_gettime(CLOCK_MONOTONIC, &when);
		}
		if (output->present_zero_copy) {
			flags |= WLR_OUTPUT_PRESENT_ZERO_COPY;
		}
		struct wlr_output_event_present present = {
			.output = &output->wlr_output,
			.commit_seq = output->present_commit_seq,
			.presented = true,
			.when = when,
			.seq = (unsigned)last.seq,
			.refresh = (int)last.refresh_ns,
			.flags = flags,
		};
		wlr_output_send_present(&output->wlr_output, &present);
		output->present_pending = false;
	}

	wlr_output_send_frame(&output->wlr_output);
	return 0;
}

/* NSWindow resize / backing-scale change: request the new mode + scale. */
static int handle_resize(int fd, uint32_t mask, void *data) {
	struct wlr_darwin_output *output = data;
	struct darwin_output_geometry geom, last;
	bool have = false;
	/* Drain all pending records; the most recent wins (coalesce live resize). */
	while (read(fd, &geom, sizeof(geom)) == (ssize_t)sizeof(geom)) {
		last = geom;
		have = true;
	}
	if (!have || last.width_px == 0 || last.height_px == 0) {
		return 0;
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, last.width_px, last.height_px,
		DARWIN_DEFAULT_REFRESH);
	wlr_output_state_set_scale(&state, (float)last.scale);
	wlr_output_send_request_state(&output->wlr_output, &state);
	wlr_output_state_finish(&state);
	return 0;
}

struct wlr_output *darwin_add_output(struct wlr_darwin_backend *backend,
		unsigned int width, unsigned int height) {
	struct wlr_darwin_output *output = calloc(1, sizeof(*output));
	if (!output) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_darwin_output");
		return NULL;
	}
	output->backend = backend;
	output->input_fd[0] = output->input_fd[1] = -1;
	output->frame_fd[0] = output->frame_fd[1] = -1;
	output->resize_fd[0] = output->resize_fd[1] = -1;

	/* Per-output pipes: input (main->compositor), frame clock, resize. */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, output->input_fd) != 0 ||
			socketpair(AF_UNIX, SOCK_STREAM, 0, output->frame_fd) != 0 ||
			socketpair(AF_UNIX, SOCK_STREAM, 0, output->resize_fd) != 0) {
		wlr_log_errno(WLR_ERROR, "output socketpair failed");
		goto err;
	}
	output->input_source = wl_event_loop_add_fd(backend->loop,
		output->input_fd[0], WL_EVENT_READABLE, handle_input, output);
	output->frame_source = wl_event_loop_add_fd(backend->loop,
		output->frame_fd[0], WL_EVENT_READABLE, handle_frame, output);
	output->resize_source = wl_event_loop_add_fd(backend->loop,
		output->resize_fd[0], WL_EVENT_READABLE, handle_resize, output);

	struct darwin_output_geometry geom = { .width_px = width,
		.height_px = height, .scale = 1.0 };
	output->window = darwin_cocoa_window_create(width, height,
		output->frame_fd[1], output->input_fd[1], output->resize_fd[1], &geom);
	if (!output->window) {
		wlr_log(WLR_ERROR, "Failed to create Cocoa window");
		goto err;
	}

	/* Report the backing-pixel resolution as the mode and the HiDPI scale. */
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, geom.width_px, geom.height_px,
		DARWIN_DEFAULT_REFRESH);
	wlr_output_state_set_scale(&state, (float)geom.scale);

	wlr_output_init(&output->wlr_output, &backend->backend, &darwin_output_impl,
		backend->loop, &state);
	wlr_output_state_finish(&state);

	char name[64];
	snprintf(name, sizeof(name), "DARWIN-%zu", ++backend->last_output_num);
	wlr_output_set_name(&output->wlr_output, name);

	/* Per-output pointer, bound to this output so absolute motion lands here. */
	wlr_pointer_init(&output->pointer, &pointer_impl, "darwin-pointer");
	output->pointer.output_name = strdup(output->wlr_output.name);

	wl_list_insert(&backend->outputs, &output->link);

	if (backend->started) {
		wl_signal_emit_mutable(&backend->backend.events.new_output,
			&output->wlr_output);
		wl_signal_emit_mutable(&backend->backend.events.new_input,
			&output->pointer.base);
	}
	return &output->wlr_output;

err:
	if (output->input_source) {
		wl_event_source_remove(output->input_source);
	}
	if (output->frame_source) {
		wl_event_source_remove(output->frame_source);
	}
	if (output->resize_source) {
		wl_event_source_remove(output->resize_source);
	}
	for (int i = 0; i < 2; i++) {
		if (output->input_fd[i] >= 0) {
			close(output->input_fd[i]);
		}
		if (output->frame_fd[i] >= 0) {
			close(output->frame_fd[i]);
		}
		if (output->resize_fd[i] >= 0) {
			close(output->resize_fd[i]);
		}
	}
	free(output);
	return NULL;
}
