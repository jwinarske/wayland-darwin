/*
 * NSPasteboard <-> wl_data_device (selection/clipboard) bridge.
 *
 * A compositor helper (created with a wlr_seat), text-only for now:
 *
 *  - Wayland -> macOS: when the seat selection changes to a client source that
 *    offers text, pipe its data out and write it to NSPasteboard.
 *  - macOS -> Wayland: poll the pasteboard change count; on a real change,
 *    publish a wlr_data_source backed by the pasteboard text as the selection.
 *
 * Loop avoidance: our own source is skipped in the Wayland->macOS path, and the
 * change count we cause writing the pasteboard is recorded so the poll ignores
 * it.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "wlr-darwin.h"
#include "pasteboard.h"

static const char *const TEXT_MIMES[] = {
	"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT",
};
#define N_TEXT_MIMES (sizeof(TEXT_MIMES) / sizeof(TEXT_MIMES[0]))

struct wlr_darwin_data_bridge {
	struct wlr_seat *seat;
	struct wl_event_loop *loop;
	struct wl_listener set_selection;
	struct wl_listener seat_destroy;
	struct wl_event_source *poll_timer;
	int64_t last_change_count;
};

/* --- our data source: macOS pasteboard text -> Wayland clients --- */

struct pasteboard_source {
	struct wlr_data_source base;
	char *text; /* UTF-8, owned */
};

static const struct wlr_data_source_impl source_impl;

static void source_send(struct wlr_data_source *wlr_source, const char *mime_type,
		int32_t fd) {
	struct pasteboard_source *source =
		wl_container_of(wlr_source, source, base);
	const char *text = source->text ? source->text : "";
	size_t len = strlen(text), off = 0;
	/* Text is small; a blocking write is acceptable for the MVP. */
	while (off < len) {
		ssize_t n = write(fd, text + off, len - off);
		if (n <= 0) {
			break;
		}
		off += (size_t)n;
	}
	close(fd);
}

static void source_destroy(struct wlr_data_source *wlr_source) {
	struct pasteboard_source *source =
		wl_container_of(wlr_source, source, base);
	free(source->text);
	free(source);
}

static const struct wlr_data_source_impl source_impl = {
	.send = source_send,
	.destroy = source_destroy,
};

static bool source_is_ours(struct wlr_data_source *source) {
	return source != NULL && source->impl == &source_impl;
}

static bool source_offer(struct pasteboard_source *source, const char *mime) {
	char **p = wl_array_add(&source->base.mime_types, sizeof(char *));
	if (p == NULL) {
		return false;
	}
	*p = strdup(mime);
	return *p != NULL;
}

/* --- macOS -> Wayland: poll the pasteboard, publish a selection --- */

static int handle_poll(void *data) {
	struct wlr_darwin_data_bridge *bridge = data;
	int64_t count = darwin_pasteboard_change_count();
	if (count != bridge->last_change_count) {
		bridge->last_change_count = count;
		char *text = darwin_pasteboard_get_text();
		if (text != NULL) {
			struct pasteboard_source *source = calloc(1, sizeof(*source));
			if (source != NULL) {
				source->text = text;
				wlr_data_source_init(&source->base, &source_impl);
				for (size_t i = 0; i < N_TEXT_MIMES; i++) {
					source_offer(source, TEXT_MIMES[i]);
				}
				uint32_t serial = wl_display_next_serial(bridge->seat->display);
				wlr_seat_set_selection(bridge->seat, &source->base, serial);
			} else {
				free(text);
			}
		}
	}
	wl_event_source_timer_update(bridge->poll_timer, 500);
	return 0;
}

/* --- Wayland -> macOS: read the client selection, write the pasteboard --- */

struct clipboard_read {
	struct wlr_darwin_data_bridge *bridge;
	int fd;
	struct wl_event_source *source;
	char *buf;
	size_t len, cap;
};

static void clipboard_read_finish(struct clipboard_read *r) {
	r->buf[r->len] = '\0';
	r->bridge->last_change_count = darwin_pasteboard_set_text(r->buf);
	wl_event_source_remove(r->source);
	close(r->fd);
	free(r->buf);
	free(r);
}

static int handle_clipboard_read(int fd, uint32_t mask, void *data) {
	struct clipboard_read *r = data;
	for (;;) {
		if (r->len + 4096 + 1 > r->cap) {
			size_t cap = r->cap * 2;
			char *buf = realloc(r->buf, cap);
			if (buf == NULL) {
				clipboard_read_finish(r);
				return 0;
			}
			r->buf = buf;
			r->cap = cap;
		}
		ssize_t n = read(fd, r->buf + r->len, r->cap - r->len - 1);
		if (n > 0) {
			r->len += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EAGAIN) {
			return 0; /* wait for more */
		}
		clipboard_read_finish(r); /* EOF or error */
		return 0;
	}
}

static const char *pick_text_mime(struct wlr_data_source *source) {
	char **mime;
	wl_array_for_each(mime, &source->mime_types) {
		for (size_t i = 0; i < N_TEXT_MIMES; i++) {
			if (strcmp(*mime, TEXT_MIMES[i]) == 0) {
				return TEXT_MIMES[i];
			}
		}
	}
	return NULL;
}

static void handle_set_selection(struct wl_listener *listener, void *data) {
	struct wlr_darwin_data_bridge *bridge =
		wl_container_of(listener, bridge, set_selection);
	struct wlr_data_source *source = bridge->seat->selection_source;
	if (source == NULL || source_is_ours(source)) {
		return; /* cleared, or our own macOS-origin source */
	}
	const char *mime = pick_text_mime(source);
	if (mime == NULL) {
		return; /* nothing textual on offer */
	}

	int fds[2];
	if (pipe(fds) != 0) {
		return;
	}
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	wlr_data_source_send(source, mime, fds[1]);
	close(fds[1]);

	struct clipboard_read *r = calloc(1, sizeof(*r));
	if (r == NULL) {
		close(fds[0]);
		return;
	}
	r->bridge = bridge;
	r->fd = fds[0];
	r->cap = 4096;
	r->buf = malloc(r->cap);
	if (r->buf == NULL) {
		close(fds[0]);
		free(r);
		return;
	}
	r->source = wl_event_loop_add_fd(bridge->loop, fds[0], WL_EVENT_READABLE,
		handle_clipboard_read, r);
}

/* --- lifecycle --- */

static void handle_seat_destroy(struct wl_listener *listener, void *data) {
	struct wlr_darwin_data_bridge *bridge =
		wl_container_of(listener, bridge, seat_destroy);
	wlr_darwin_data_bridge_destroy(bridge);
}

struct wlr_darwin_data_bridge *wlr_darwin_data_bridge_create(
		struct wlr_seat *seat) {
	struct wlr_darwin_data_bridge *bridge = calloc(1, sizeof(*bridge));
	if (bridge == NULL) {
		return NULL;
	}
	bridge->seat = seat;
	bridge->loop = wl_display_get_event_loop(seat->display);
	/* Don't import the pre-existing pasteboard on startup. */
	bridge->last_change_count = darwin_pasteboard_change_count();

	bridge->set_selection.notify = handle_set_selection;
	wl_signal_add(&seat->events.set_selection, &bridge->set_selection);
	bridge->seat_destroy.notify = handle_seat_destroy;
	wl_signal_add(&seat->events.destroy, &bridge->seat_destroy);

	bridge->poll_timer = wl_event_loop_add_timer(bridge->loop, handle_poll, bridge);
	wl_event_source_timer_update(bridge->poll_timer, 500);

	wlr_log(WLR_INFO, "Darwin clipboard bridge (NSPasteboard) active");
	return bridge;
}

void wlr_darwin_data_bridge_destroy(struct wlr_darwin_data_bridge *bridge) {
	if (bridge == NULL) {
		return;
	}
	wl_list_remove(&bridge->set_selection.link);
	wl_list_remove(&bridge->seat_destroy.link);
	if (bridge->poll_timer != NULL) {
		wl_event_source_remove(bridge->poll_timer);
	}
	free(bridge);
}
