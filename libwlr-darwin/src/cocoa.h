/*
 * C <-> Objective-C boundary (D7: all ObjC/AppKit is confined to cocoa.m).
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

/*
 * Create a window of w x h. Runs on the main thread (dispatched + waited on).
 *
 * frame_event_fd: cocoa.m writes one byte to this fd on every display-link tick
 * (the D6 frame clock); the backend's event loop reads it and calls
 * wlr_output_send_frame(). input_event_fd: cocoa.m writes serialized input
 * events here (the D3 main->compositor bridge).
 */
darwin_cocoa_window *darwin_cocoa_window_create(unsigned int w, unsigned int h,
	int frame_event_fd, int input_event_fd);

/*
 * Present a mapped CPU pixel buffer to the window's CALayer.
 *
 * MVP software path (copy). TODO(W4): replace with a zero-copy IOSurface
 * assignment to CALayer.contents once the IOSurface allocator lands.
 * `format` is a DRM fourcc; only LINEAR BGRA/XRGB is handled for now.
 */
void darwin_cocoa_window_present(darwin_cocoa_window *win, const void *data,
	uint32_t width, uint32_t height, uint32_t stride, uint32_t drm_format);

void darwin_cocoa_window_destroy(darwin_cocoa_window *win);

#endif
