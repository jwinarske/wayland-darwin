#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>

#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "darwin.h"

#define DARWIN_DEFAULT_REFRESH 60000 // mHz

static const uint32_t SUPPORTED_OUTPUT_STATE =
	WLR_OUTPUT_STATE_BACKEND_OPTIONAL |
	WLR_OUTPUT_STATE_BUFFER |
	WLR_OUTPUT_STATE_ENABLED |
	WLR_OUTPUT_STATE_MODE;

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
		 * TODO(D6/W7): move the present event to the CADisplayLink callback
		 * so `when`/`seq` carry the real hardware timestamp.
		 */
		struct wlr_output_event_present present = {
			.output = wlr_output,
			.commit_seq = wlr_output->commit_seq + 1,
			.presented = true,
		};
		wlr_output_send_present(wlr_output, &present);
	}

	return true;
}

static void output_destroy(struct wlr_output *wlr_output) {
	struct wlr_darwin_output *output = darwin_output_from_output(wlr_output);

	if (output->window) {
		darwin_cocoa_window_destroy(output->window);
	}
	if (output->frame_source) {
		wl_event_source_remove(output->frame_source);
	}
	if (output->frame_timer) {
		wl_event_source_remove(output->frame_timer);
	}
	if (output->frame_fd[0] >= 0) {
		close(output->frame_fd[0]);
	}
	if (output->frame_fd[1] >= 0) {
		close(output->frame_fd[1]);
	}

	wl_list_remove(&output->link);
	wlr_output_finish(wlr_output);
	free(output);
}

const struct wlr_output_impl darwin_output_impl = {
	.destroy = output_destroy,
	.test = output_test,
	.commit = output_commit,
	// set_cursor/move_cursor unimplemented in MVP: wlroots composites the
	// cursor into the frame (D1). A CALayer hardware cursor is a later fast path.
};

bool wlr_output_is_darwin(const struct wlr_output *wlr_output) {
	return wlr_output->impl == &darwin_output_impl;
}

/* D6 frame clock: cocoa.m's CADisplayLink writes a byte per tick. */
static int handle_frame(int fd, uint32_t mask, void *data) {
	struct wlr_darwin_output *output = data;
	char buf[64];
	ssize_t n;
	do {
		n = read(fd, buf, sizeof(buf));
	} while (n == (ssize_t)sizeof(buf));
	wlr_output_send_frame(&output->wlr_output);
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
	output->frame_fd[0] = output->frame_fd[1] = -1;

	/* Per-output frame-clock pipe fed by CADisplayLink in cocoa.m. */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, output->frame_fd) != 0) {
		wlr_log_errno(WLR_ERROR, "output socketpair failed");
		free(output);
		return NULL;
	}
	output->frame_source = wl_event_loop_add_fd(backend->loop,
		output->frame_fd[0], WL_EVENT_READABLE, handle_frame, output);

	output->window = darwin_cocoa_window_create(width, height,
		output->frame_fd[1], backend->event_fd[1]);
	if (!output->window) {
		wlr_log(WLR_ERROR, "Failed to create Cocoa window");
		wl_event_source_remove(output->frame_source);
		close(output->frame_fd[0]);
		close(output->frame_fd[1]);
		free(output);
		return NULL;
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, width, height,
		DARWIN_DEFAULT_REFRESH);

	wlr_output_init(&output->wlr_output, &backend->backend, &darwin_output_impl,
		backend->loop, &state);
	wlr_output_state_finish(&state);

	char name[64];
	snprintf(name, sizeof(name), "DARWIN-%zu", ++backend->last_output_num);
	wlr_output_set_name(&output->wlr_output, name);

	wl_list_insert(&backend->outputs, &output->link);

	if (backend->started) {
		wl_signal_emit_mutable(&backend->backend.events.new_output,
			&output->wlr_output);
	}
	return &output->wlr_output;
}
