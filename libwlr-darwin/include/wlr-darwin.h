/*
 * libwlr-darwin — a nested, windowed wlroots backend for macOS (Cocoa).
 *
 * Presents each wlr_output as an NSWindow. Out-of-tree: builds against wlroots'
 * public unstable interfaces, no wlroots fork required.
 *
 * Threading (normative): AppKit owns the process main thread; wl_display and
 * all wlroots calls live on a dedicated compositor thread. See
 * wlr_darwin_application_run().
 */
#ifndef WLR_DARWIN_H
#define WLR_DARWIN_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>

/**
 * Capture the process main thread for AppKit (NSApplication) and run the
 * compositor on a dedicated secondary thread.
 *
 * MUST be the first thing main() does — AppKit requires the true main thread,
 * and no wlroots/wl_display symbol may be touched from it. `compositor_main`
 * runs on the compositor thread: it should create a wl_display, call
 * wlr_darwin_backend_create() with that display's event loop, wire up the
 * compositor, and enter wl_display_run(). Its return value becomes the process
 * exit code. This function blocks in [NSApp run] until the compositor thread
 * returns, then tears AppKit down.
 *
 * A compositor's integration is ~3 lines: move the body of main() into a
 * compositor_main(void*) and call this from main().
 */
int wlr_darwin_application_run(int (*compositor_main)(void *data), void *data);

/**
 * Create a Cocoa windowed backend. Compositor thread only; pass the wl_display
 * event loop. Requires wlr_darwin_application_run() to be driving the main
 * thread. On start, the backend creates its initial output (window).
 */
struct wlr_backend *wlr_darwin_backend_create(struct wl_event_loop *loop);

/**
 * Add an output presented as an NSWindow of the given size. Compositor thread
 * only. Emits the backend's new_output signal if the backend is started.
 */
struct wlr_output *wlr_darwin_add_output(struct wlr_backend *backend,
	unsigned int width, unsigned int height);

/**
 * Create the IOSurface allocator. Buffers it produces are IOSurface-backed and
 * present zero-copy on a Darwin output (assigned directly to CALayer.contents).
 * Pass it to wlr_output_init_render(); the pixman renderer draws straight into
 * IOSurface memory. Caps: WLR_BUFFER_CAP_DATA_PTR.
 */
struct wlr_allocator *wlr_darwin_allocator_create(void);

bool wlr_backend_is_darwin(const struct wlr_backend *backend);
bool wlr_output_is_darwin(const struct wlr_output *output);
bool wlr_buffer_is_darwin(const struct wlr_buffer *buffer);

#endif
