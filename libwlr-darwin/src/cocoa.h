/*
 * C <-> Objective-C boundary (all ObjC/AppKit is confined to cocoa.m).
 *
 * These functions are called from the pure-C backend/output code and are
 * implemented in cocoa.m. Window operations are dispatched to the main thread
 * internally, so callers invoke them from the compositor thread.
 */
#ifndef WLR_DARWIN_COCOA_H
#define WLR_DARWIN_COCOA_H

#include <stdbool.h>
#include <stdint.h>

/* Opaque handle to a window (NSWindow + NSView + CALayer), owned by cocoa.m. */
typedef struct darwin_cocoa_window darwin_cocoa_window;

/* Output geometry in backing pixels plus the HiDPI scale (backingScaleFactor). */
struct darwin_output_geometry {
	uint32_t width_px;
	uint32_t height_px;
	double scale;
};

/*
 * One record per display-link tick (vsync). Carries the timing the compositor
 * needs to drive frame + presentation feedback: the vsync timestamp (in the
 * CLOCK_MONOTONIC domain, from CVTimeStamp.hostTime), the refresh interval, and
 * a monotonic vsync counter.
 */
struct darwin_frame_info {
	int64_t when_ns;    // vsync time; 0 if the display link gave no host time
	int64_t refresh_ns; // refresh interval; 0 if unknown
	uint64_t seq;       // monotonic vsync counter
};

/*
 * Create a window of w x h logical points. Runs on the main thread (dispatched
 * + waited on). Fills *out_geom with the initial backing-pixel size and scale.
 *
 * frame_event_fd: one struct darwin_frame_info per display-link tick (the frame
 * clock + presentation timing).
 * input_event_fd: serialized input events (main->compositor bridge).
 * resize_event_fd: cocoa.m writes a struct darwin_output_geometry whenever the
 * window is resized or its backing scale changes; the backend turns each into a
 * wlr_output_send_request_state().
 */
darwin_cocoa_window *darwin_cocoa_window_create(unsigned int w, unsigned int h,
	int frame_event_fd, int input_event_fd, int resize_event_fd,
	struct darwin_output_geometry *out_geom);

/*
 * Present a mapped CPU pixel buffer to the window's CALayer — the software copy
 * fallback for foreign buffers; the zero-copy path is
 * darwin_cocoa_window_present_iosurface. `format` is a DRM fourcc; only LINEAR
 * BGRA/XRGB is handled.
 */
void darwin_cocoa_window_present(darwin_cocoa_window *win, const void *data,
	uint32_t width, uint32_t height, uint32_t stride, uint32_t drm_format);

void darwin_cocoa_window_destroy(darwin_cocoa_window *win);

/* ---- IOSurface (zero-copy software path) ---------------------------------- */

/* Opaque wrapper around an IOSurfaceRef. */
typedef struct darwin_iosurface darwin_iosurface;

/*
 * Create an IOSurface for a DRM fourcc (LINEAR BGRA/XRGB only for now).
 * Returns NULL on failure; *out_stride receives the actual bytes-per-row.
 */
darwin_iosurface *darwin_iosurface_create(uint32_t width, uint32_t height,
	uint32_t drm_format, uint32_t *out_stride);

/* CPU access: lock returns the base address; pair each lock with an unlock. */
void *darwin_iosurface_lock(darwin_iosurface *surface, bool write);
void darwin_iosurface_unlock(darwin_iosurface *surface, bool write);
void darwin_iosurface_destroy(darwin_iosurface *surface);

/* The underlying IOSurfaceRef (as an opaque pointer), for the Metal renderer. */
void *darwin_iosurface_ref(darwin_iosurface *surface);

/*
 * Zero-copy present: assign the IOSurface directly to the window's
 * CALayer.contents (no copy). The compositor renders into IOSurface memory via
 * the allocator, so this just hands the surface to the layer.
 */
void darwin_cocoa_window_present_iosurface(darwin_cocoa_window *win,
	darwin_iosurface *surface);

/* ---- hardware cursor (overlay CALayer) ------------------------------------ */

/*
 * Hardware-cursor fast path: the cursor lives on its own CALayer above the
 * content layer, so moving it only repositions that layer — the scene is never
 * recomposited. All coordinates are in output backing pixels (top-left origin,
 * matching the flipped content view); hotspot and size are in cursor-buffer
 * pixels.
 *
 * set_cursor_surface: zero-copy — the IOSurface becomes the cursor layer's
 *   contents. A NULL surface hides the cursor.
 * set_cursor_pixels: copy path for foreign (non-IOSurface) cursor buffers.
 * move_cursor: reposition only (no redraw).
 */
void darwin_cocoa_window_set_cursor_surface(darwin_cocoa_window *win,
	darwin_iosurface *surface, int width, int height,
	int hotspot_x, int hotspot_y);
void darwin_cocoa_window_set_cursor_pixels(darwin_cocoa_window *win,
	const void *data, int width, int height, int stride, uint32_t drm_format,
	int hotspot_x, int hotspot_y);
void darwin_cocoa_window_move_cursor(darwin_cocoa_window *win, int x, int y);

#endif
