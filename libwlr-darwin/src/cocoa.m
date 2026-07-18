/*
 * cocoa.m — all AppKit/Objective-C for the Darwin backend (D7 containment).
 *
 * Threading (D3): AppKit runs on the process main thread; the compositor runs
 * on a secondary thread started by wlr_darwin_application_run(). Window
 * operations invoked from the compositor thread are marshalled to the main
 * thread via dispatch_{sync,async}(dispatch_get_main_queue(), ...). The reverse
 * direction (frame ticks, input) is delivered to the compositor over the fds
 * handed in at window creation.
 *
 * Build: compiled as Objective-C with ARC. macOS only.
 */
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <unistd.h>

#include "cocoa.h"
#include "input.h"

/* DRM fourcc codes we understand, defined locally to avoid a libdrm include. */
#define DARWIN_DRM_FORMAT_XRGB8888 0x34325258 /* 'XR24' */
#define DARWIN_DRM_FORMAT_ARGB8888 0x34325241 /* 'AR24' */

/* ---- event-capturing view ------------------------------------------------- */

@interface WlrDarwinView : NSView
@property (assign) int inputFd; /* write end of the main->compositor bridge */
@property (strong) NSTrackingArea *tracking;
@end

@implementation WlrDarwinView

- (BOOL)isFlipped { return YES; }              /* top-left origin, like Wayland */
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }

- (void)emit:(struct darwin_input_event)ev {
	if (self.inputFd >= 0) {
		(void)write(self.inputFd, &ev, sizeof(ev));
	}
}

static uint32_t event_time_msec(NSEvent *e) {
	return (uint32_t)(e.timestamp * 1000.0);
}

/* -- keyboard: kVK -> evdev, xkb (compositor-side) does the rest -- */

- (void)sendKeyCode:(uint16_t)kvk pressed:(BOOL)pressed time:(uint32_t)t {
	uint32_t evdev = darwin_kvk_to_evdev(kvk);
	if (evdev == 0) {
		return;
	}
	struct darwin_input_event ev = {
		.type = DARWIN_INPUT_KEY, .time_msec = t,
		.code = evdev, .state = pressed ? 1 : 0,
	};
	[self emit:ev];
}

- (void)keyDown:(NSEvent *)e {
	if (e.isARepeat) {
		return; /* key repeat is client-side (D5b) */
	}
	[self sendKeyCode:e.keyCode pressed:YES time:event_time_msec(e)];
}

- (void)keyUp:(NSEvent *)e {
	[self sendKeyCode:e.keyCode pressed:NO time:event_time_msec(e)];
}

static NSEventModifierFlags mask_for_keycode(uint16_t kvk) {
	switch (kvk) {
	case 0x38: case 0x3C: return NSEventModifierFlagShift;
	case 0x3B: case 0x3E: return NSEventModifierFlagControl;
	case 0x3A: case 0x3D: return NSEventModifierFlagOption;
	case 0x37: case 0x36: return NSEventModifierFlagCommand;
	case 0x39:            return NSEventModifierFlagCapsLock;
	default:              return 0;
	}
}

- (void)flagsChanged:(NSEvent *)e {
	NSEventModifierFlags mask = mask_for_keycode(e.keyCode);
	if (mask == 0) {
		return;
	}
	BOOL down = (e.modifierFlags & mask) != 0;
	[self sendKeyCode:e.keyCode pressed:down time:event_time_msec(e)];
}

/* -- pointer -- */

- (void)sendMotion:(NSEvent *)e {
	NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
	NSSize sz = self.bounds.size;
	if (sz.width <= 0 || sz.height <= 0) {
		return;
	}
	double x = p.x / sz.width, y = p.y / sz.height;
	x = x < 0 ? 0 : (x > 1 ? 1 : x);
	y = y < 0 ? 0 : (y > 1 ? 1 : y);
	struct darwin_input_event ev = {
		.type = DARWIN_INPUT_MOTION_ABS, .time_msec = event_time_msec(e),
		.x = x, .y = y,
	};
	[self emit:ev];
}

- (void)mouseMoved:(NSEvent *)e { [self sendMotion:e]; }
- (void)mouseDragged:(NSEvent *)e { [self sendMotion:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self sendMotion:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self sendMotion:e]; }

- (void)sendButton:(uint32_t)button pressed:(BOOL)pressed time:(uint32_t)t {
	struct darwin_input_event ev = {
		.type = DARWIN_INPUT_BUTTON, .time_msec = t,
		.code = button, .state = pressed ? 1 : 0,
	};
	[self emit:ev];
}

- (void)mouseDown:(NSEvent *)e { [self sendButton:BTN_LEFT pressed:YES time:event_time_msec(e)]; }
- (void)mouseUp:(NSEvent *)e { [self sendButton:BTN_LEFT pressed:NO time:event_time_msec(e)]; }
- (void)rightMouseDown:(NSEvent *)e { [self sendButton:BTN_RIGHT pressed:YES time:event_time_msec(e)]; }
- (void)rightMouseUp:(NSEvent *)e { [self sendButton:BTN_RIGHT pressed:NO time:event_time_msec(e)]; }
- (void)otherMouseDown:(NSEvent *)e { [self sendButton:BTN_MIDDLE pressed:YES time:event_time_msec(e)]; }
- (void)otherMouseUp:(NSEvent *)e { [self sendButton:BTN_MIDDLE pressed:NO time:event_time_msec(e)]; }

- (void)scrollWheel:(NSEvent *)e {
	uint32_t t = event_time_msec(e);
	BOOL precise = e.hasPreciseScrollingDeltas;
	uint32_t source = precise ? 1u /* FINGER */ : 0u /* WHEEL */;
	/* Wayland's positive axis is down/right; macOS deltas are inverted. */
	double dy = -e.scrollingDeltaY;
	double dx = -e.scrollingDeltaX;
	if (dy != 0) {
		struct darwin_input_event ev = {
			.type = DARWIN_INPUT_AXIS, .time_msec = t,
			.state = 0 /* VERTICAL */, .aux = source,
			.x = precise ? dy : dy * 10.0,
			.discrete = precise ? 0 : (dy > 0 ? 120 : -120),
		};
		[self emit:ev];
	}
	if (dx != 0) {
		struct darwin_input_event ev = {
			.type = DARWIN_INPUT_AXIS, .time_msec = t,
			.state = 1 /* HORIZONTAL */, .aux = source,
			.x = precise ? dx : dx * 10.0,
			.discrete = precise ? 0 : (dx > 0 ? 120 : -120),
		};
		[self emit:ev];
	}
}

- (void)updateTrackingAreas {
	[super updateTrackingAreas];
	if (self.tracking) {
		[self removeTrackingArea:self.tracking];
	}
	NSTrackingAreaOptions opts = NSTrackingActiveInKeyWindow |
		NSTrackingMouseMoved | NSTrackingInVisibleRect;
	self.tracking = [[NSTrackingArea alloc] initWithRect:self.bounds
		options:opts owner:self userInfo:nil];
	[self addTrackingArea:self.tracking];
}

@end

/* ---- window object -------------------------------------------------------- */

@interface WlrDarwinWindow : NSObject {
@public
	NSWindow *window;
	WlrDarwinView *view;
	CALayer *layer;
	CVDisplayLinkRef displayLink;
	int frameFd;  /* write end: one byte per display tick (D6) */
	int inputFd;  /* write end: serialized input events (D3/W5) */
}
@end

@implementation WlrDarwinWindow
@end

/* CVDisplayLink runs on its own thread; poke the compositor's frame fd. */
static CVReturn display_link_cb(CVDisplayLinkRef link, const CVTimeStamp *now,
		const CVTimeStamp *out, CVOptionFlags flagsIn, CVOptionFlags *flagsOut,
		void *ctx) {
	WlrDarwinWindow *win = (__bridge WlrDarwinWindow *)ctx;
	char b = 1;
	(void)write(win->frameFd, &b, 1);
	return kCVReturnSuccess;
}

/* ---- boundary API --------------------------------------------------------- */

darwin_cocoa_window *darwin_cocoa_window_create(unsigned int w, unsigned int h,
		int frame_event_fd, int input_event_fd) {
	__block WlrDarwinWindow *win = nil;
	dispatch_sync(dispatch_get_main_queue(), ^{
		NSRect rect = NSMakeRect(0, 0, w, h);
		NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
			NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
		NSWindow *nsw = [[NSWindow alloc] initWithContentRect:rect
			styleMask:style backing:NSBackingStoreBuffered defer:NO];
		nsw.title = @"wlroots";
		nsw.releasedWhenClosed = NO;

		WlrDarwinView *view = [[WlrDarwinView alloc] initWithFrame:rect];
		view.inputFd = input_event_fd;
		view.wantsLayer = YES;
		nsw.contentView = view;
		nsw.acceptsMouseMovedEvents = YES;

		CALayer *layer = view.layer;
		layer.contentsGravity = kCAGravityResize;
		layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);

		[nsw center];
		[nsw makeKeyAndOrderFront:nil];
		[nsw makeFirstResponder:view];

		win = [WlrDarwinWindow new];
		win->window = nsw;
		win->view = view;
		win->layer = layer;
		win->frameFd = frame_event_fd;
		win->inputFd = input_event_fd;

		/* D6 frame clock. TODO: CADisplayLink on macOS 14+ for lower overhead. */
		CVDisplayLinkCreateWithActiveCGDisplays(&win->displayLink);
		CVDisplayLinkSetOutputCallback(win->displayLink, display_link_cb,
			(__bridge void *)win);
		CVDisplayLinkStart(win->displayLink);
	});
	if (!win) {
		return NULL;
	}
	return (darwin_cocoa_window *)CFBridgingRetain(win);
}

void darwin_cocoa_window_present(darwin_cocoa_window *handle, const void *data,
		uint32_t width, uint32_t height, uint32_t stride, uint32_t drm_format) {
	WlrDarwinWindow *win = (__bridge WlrDarwinWindow *)handle;

	/*
	 * MVP software present: copy the mapped pixels into a CGImage now (the
	 * buffer mapping is only valid for the duration of this call) and assign it
	 * to the layer on the main thread.
	 *
	 * TODO(W4): the IOSurface allocator makes this zero-copy — assign the
	 * IOSurface-backed image directly to CALayer.contents with no copy.
	 */
	CGBitmapInfo info;
	switch (drm_format) {
	case DARWIN_DRM_FORMAT_XRGB8888:
		info = kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little;
		break;
	case DARWIN_DRM_FORMAT_ARGB8888:
		info = kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little;
		break;
	default:
		return; /* only LINEAR BGRA/XRGB handled for now */
	}

	CFDataRef pixels = CFDataCreate(NULL, data, (CFIndex)(stride * height));
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(pixels);
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGImageRef image = CGImageCreate(width, height, 8, 32, stride, cs, info,
		provider, NULL, false, kCGRenderingIntentDefault);
	CGColorSpaceRelease(cs);
	CGDataProviderRelease(provider);
	CFRelease(pixels);
	if (!image) {
		return;
	}

	dispatch_async(dispatch_get_main_queue(), ^{
		win->layer.contents = (__bridge id)image;
		CGImageRelease(image);
	});
}

void darwin_cocoa_window_destroy(darwin_cocoa_window *handle) {
	WlrDarwinWindow *win = (WlrDarwinWindow *)CFBridgingRelease(handle);
	dispatch_async(dispatch_get_main_queue(), ^{
		if (win->displayLink) {
			CVDisplayLinkStop(win->displayLink);
			CVDisplayLinkRelease(win->displayLink);
		}
		[win->window close];
	});
}

/* ---- IOSurface -------------------------------------------------------------
 *
 * IOSurface is a C API; kept here so all Apple-framework code stays in one TU.
 */

struct darwin_iosurface {
	IOSurfaceRef ref;
};

static uint32_t drm_to_iosurface_pixel_format(uint32_t drm_format) {
	switch (drm_format) {
	case DARWIN_DRM_FORMAT_XRGB8888:
	case DARWIN_DRM_FORMAT_ARGB8888:
		/* DRM XR24/AR24 are little-endian BGRA in memory. */
		return 'BGRA';
	default:
		return 0;
	}
}

darwin_iosurface *darwin_iosurface_create(uint32_t width, uint32_t height,
		uint32_t drm_format, uint32_t *out_stride) {
	uint32_t pixel_format = drm_to_iosurface_pixel_format(drm_format);
	if (pixel_format == 0) {
		return NULL;
	}

	size_t bytes_per_row =
		IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, (size_t)width * 4);
	NSDictionary *props = @{
		(__bridge NSString *)kIOSurfaceWidth: @(width),
		(__bridge NSString *)kIOSurfaceHeight: @(height),
		(__bridge NSString *)kIOSurfaceBytesPerElement: @(4),
		(__bridge NSString *)kIOSurfaceBytesPerRow: @(bytes_per_row),
		(__bridge NSString *)kIOSurfacePixelFormat: @(pixel_format),
	};

	IOSurfaceRef ref = IOSurfaceCreate((__bridge CFDictionaryRef)props);
	if (ref == NULL) {
		return NULL;
	}

	struct darwin_iosurface *surface = calloc(1, sizeof(*surface));
	if (surface == NULL) {
		CFRelease(ref);
		return NULL;
	}
	surface->ref = ref;
	if (out_stride != NULL) {
		*out_stride = (uint32_t)IOSurfaceGetBytesPerRow(ref);
	}
	return surface;
}

void *darwin_iosurface_lock(darwin_iosurface *surface, bool write) {
	IOSurfaceLockOptions opts = write ? 0 : kIOSurfaceLockReadOnly;
	if (IOSurfaceLock(surface->ref, opts, NULL) != kIOReturnSuccess) {
		return NULL;
	}
	return IOSurfaceGetBaseAddress(surface->ref);
}

void darwin_iosurface_unlock(darwin_iosurface *surface, bool write) {
	IOSurfaceLockOptions opts = write ? 0 : kIOSurfaceLockReadOnly;
	IOSurfaceUnlock(surface->ref, opts, NULL);
}

void darwin_iosurface_destroy(darwin_iosurface *surface) {
	if (surface == NULL) {
		return;
	}
	CFRelease(surface->ref);
	free(surface);
}

void darwin_cocoa_window_present_iosurface(darwin_cocoa_window *handle,
		darwin_iosurface *surface) {
	WlrDarwinWindow *win = (__bridge WlrDarwinWindow *)handle;
	IOSurfaceRef ref = surface->ref;
	CFRetain(ref); /* hold across the async hand-off */
	dispatch_async(dispatch_get_main_queue(), ^{
		win->layer.contents = (__bridge id)ref;
		CFRelease(ref);
	});
}

/* ---- trampoline (public API) ---------------------------------------------- */

static int g_return_code = 0;

int wlr_darwin_application_run(int (*compositor_main)(void *), void *data) {
	@autoreleasepool {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

		NSThread *thread = [[NSThread alloc] initWithBlock:^{
			int rc = compositor_main(data);
			dispatch_async(dispatch_get_main_queue(), ^{
				g_return_code = rc;
				[NSApp stop:nil];
				/* stop: only takes effect after the next event; nudge it. */
				NSEvent *nudge = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
					location:NSZeroPoint modifierFlags:0 timestamp:0
					windowNumber:0 context:nil subtype:0 data1:0 data2:0];
				[NSApp postEvent:nudge atStart:YES];
			});
		}];
		[thread start];

		[NSApp run];
		return g_return_code;
	}
}
