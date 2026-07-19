/*
 * cocoa.m — all AppKit/Objective-C for the Darwin backend.
 *
 * Threading: AppKit runs on the process main thread; the compositor runs
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

#include <mach/mach_time.h>

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
@property (assign) int inputFd;  /* write end of the main->compositor bridge */
@property (assign) int resizeFd; /* write end for resize / backing-scale events */
@property (strong) NSTrackingArea *tracking;
@property (assign) BOOL pinchActive;
@property (assign) double pinchScale; /* accumulated magnification */
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
		return; /* key repeat is client-side */
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

/* -- gestures: magnify + rotate -> Wayland pinch (pointer-gestures-v1) -- */

- (void)handleGesturePhase:(NSEventPhase)phase scaleDelta:(double)ds
		rotationDelta:(double)dr time:(uint32_t)t {
	if (phase & NSEventPhaseBegan) {
		if (!self.pinchActive) {
			self.pinchActive = YES;
			self.pinchScale = 0.0;
			struct darwin_input_event ev = {
				.type = DARWIN_INPUT_PINCH_BEGIN, .time_msec = t, .code = 2 };
			[self emit:ev];
		}
	}
	if (phase & (NSEventPhaseBegan | NSEventPhaseChanged)) {
		self.pinchScale += ds;
		struct darwin_input_event ev = {
			.type = DARWIN_INPUT_PINCH_UPDATE, .time_msec = t, .code = 2,
			.f0 = 1.0 + self.pinchScale, .f1 = dr };
		[self emit:ev];
	}
	if (phase & (NSEventPhaseEnded | NSEventPhaseCancelled)) {
		if (self.pinchActive) {
			self.pinchActive = NO;
			struct darwin_input_event ev = {
				.type = DARWIN_INPUT_PINCH_END, .time_msec = t };
			[self emit:ev];
		}
	}
}

- (void)magnifyWithEvent:(NSEvent *)e {
	[self handleGesturePhase:e.phase scaleDelta:e.magnification
		rotationDelta:0 time:event_time_msec(e)];
}

- (void)rotateWithEvent:(NSEvent *)e {
	/* NSEvent.rotation is counter-clockwise degrees; Wayland is clockwise. */
	[self handleGesturePhase:e.phase scaleDelta:0
		rotationDelta:-e.rotation time:event_time_msec(e)];
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

/* Post the current backing-pixel size + scale to the compositor (HiDPI/resize). */
- (void)postGeometry {
	if (self.resizeFd < 0) {
		return;
	}
	double scale = self.window ? self.window.backingScaleFactor : 1.0;
	if (self.layer) {
		self.layer.contentsScale = scale; /* map pixel IOSurface -> point layer */
	}
	NSSize px = [self convertSizeToBacking:self.bounds.size];
	struct darwin_output_geometry geom = {
		.width_px = (uint32_t)(px.width + 0.5),
		.height_px = (uint32_t)(px.height + 0.5),
		.scale = scale,
	};
	(void)write(self.resizeFd, &geom, sizeof(geom));
}

- (void)setFrameSize:(NSSize)newSize {
	[super setFrameSize:newSize];
	[self postGeometry];
}

- (void)viewDidChangeBackingProperties {
	[super viewDidChangeBackingProperties];
	[self postGeometry];
}

@end

/* ---- window object -------------------------------------------------------- */

@interface WlrDarwinWindow : NSObject {
@public
	NSWindow *window;
	WlrDarwinView *view;
	CALayer *layer;
	CALayer *cursorLayer;  /* overlay above content; nil until first set_cursor */
	CVDisplayLinkRef displayLink;
	uint64_t vsyncSeq; /* monotonic vsync counter (display-link thread) */
	int frameFd;  /* write end: struct darwin_frame_info per display tick */
	int inputFd;  /* write end: serialized input events */
	/* Hardware cursor state (main-thread only). Hotspot/size in buffer pixels; */
	/* position in output backing pixels. */
	int cursorHotspotX, cursorHotspotY;
	int cursorW, cursorH;
	int cursorX, cursorY;
}
@end

@implementation WlrDarwinWindow
@end

/* mach_absolute_time units -> nanoseconds. CVTimeStamp.hostTime is in the same
 * units as mach_absolute_time(), which on Darwin shares CLOCK_MONOTONIC's base. */
static int64_t host_to_nsec(uint64_t host) {
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (int64_t)((__uint128_t)host * tb.numer / tb.denom);
}

/*
 * CVDisplayLink runs on its own thread; per vsync, hand the compositor the
 * timing it needs for the frame clock and presentation feedback. `now` is this
 * vsync — i.e. when the last committed frame turned to light.
 */
static CVReturn display_link_cb(CVDisplayLinkRef link, const CVTimeStamp *now,
		const CVTimeStamp *out, CVOptionFlags flagsIn, CVOptionFlags *flagsOut,
		void *ctx) {
	WlrDarwinWindow *win = (__bridge WlrDarwinWindow *)ctx;
	struct darwin_frame_info info = { .seq = ++win->vsyncSeq };
	if (now->flags & kCVTimeStampHostTimeValid) {
		info.when_ns = host_to_nsec(now->hostTime);
	}
	if ((now->flags & kCVTimeStampVideoRefreshPeriodValid) &&
			now->videoTimeScale != 0) {
		info.refresh_ns = (int64_t)(now->videoRefreshPeriod *
			1000000000.0 / now->videoTimeScale);
	}
	(void)write(win->frameFd, &info, sizeof(info));
	return kCVReturnSuccess;
}

/* ---- boundary API --------------------------------------------------------- */

darwin_cocoa_window *darwin_cocoa_window_create(unsigned int w, unsigned int h,
		int frame_event_fd, int input_event_fd, int resize_event_fd,
		struct darwin_output_geometry *out_geom) {
	__block WlrDarwinWindow *win = nil;
	__block struct darwin_output_geometry geom = { w, h, 1.0 };
	dispatch_sync(dispatch_get_main_queue(), ^{
		NSRect rect = NSMakeRect(0, 0, w, h); /* content size in points */
		NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
			NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
		NSWindow *nsw = [[NSWindow alloc] initWithContentRect:rect
			styleMask:style backing:NSBackingStoreBuffered defer:NO];
		nsw.title = @"wlroots";
		nsw.releasedWhenClosed = NO;

		WlrDarwinView *view = [[WlrDarwinView alloc] initWithFrame:rect];
		view.inputFd = input_event_fd;
		view.resizeFd = -1; /* suppress resize posts until the output exists */
		view.wantsLayer = YES;
		nsw.contentView = view;
		nsw.acceptsMouseMovedEvents = YES;

		double scale = nsw.backingScaleFactor;
		CALayer *layer = view.layer;
		layer.contentsGravity = kCAGravityResize;
		layer.contentsScale = scale;
		layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);

		[nsw center];
		[nsw makeKeyAndOrderFront:nil];
		[nsw makeFirstResponder:view];

		/* Initial backing-pixel geometry reported back to the caller. */
		NSSize px = [view convertSizeToBacking:view.bounds.size];
		geom.width_px = (uint32_t)(px.width + 0.5);
		geom.height_px = (uint32_t)(px.height + 0.5);
		geom.scale = scale;
		view.resizeFd = resize_event_fd; /* now live for subsequent resizes */

		win = [WlrDarwinWindow new];
		win->window = nsw;
		win->view = view;
		win->layer = layer;
		win->frameFd = frame_event_fd;
		win->inputFd = input_event_fd;

		/* Frame clock. TODO: CADisplayLink on macOS 14+ for lower overhead. */
		CVDisplayLinkCreateWithActiveCGDisplays(&win->displayLink);
		CVDisplayLinkSetOutputCallback(win->displayLink, display_link_cb,
			(__bridge void *)win);
		CVDisplayLinkStart(win->displayLink);
	});
	if (!win) {
		return NULL;
	}
	if (out_geom) {
		*out_geom = geom;
	}
	return (darwin_cocoa_window *)CFBridgingRetain(win);
}

/* Copy mapped pixels into a CGImage (LINEAR BGRA/XRGB only). Caller releases. */
static CGImageRef make_cgimage(const void *data, uint32_t width, uint32_t height,
		uint32_t stride, uint32_t drm_format) {
	CGBitmapInfo info;
	switch (drm_format) {
	case DARWIN_DRM_FORMAT_XRGB8888:
		info = kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little;
		break;
	case DARWIN_DRM_FORMAT_ARGB8888:
		info = kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little;
		break;
	default:
		return NULL; /* only LINEAR BGRA/XRGB handled for now */
	}

	CFDataRef pixels = CFDataCreate(NULL, data, (CFIndex)(stride * height));
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(pixels);
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGImageRef image = CGImageCreate(width, height, 8, 32, stride, cs, info,
		provider, NULL, false, kCGRenderingIntentDefault);
	CGColorSpaceRelease(cs);
	CGDataProviderRelease(provider);
	CFRelease(pixels);
	return image;
}

void darwin_cocoa_window_present(darwin_cocoa_window *handle, const void *data,
		uint32_t width, uint32_t height, uint32_t stride, uint32_t drm_format) {
	WlrDarwinWindow *win = (__bridge WlrDarwinWindow *)handle;

	/*
	 * Software present: copy the mapped pixels into a CGImage now (the buffer
	 * mapping is only valid for the duration of this call) and assign it to the
	 * layer on the main thread. The zero-copy path
	 * (darwin_cocoa_window_present_iosurface) avoids this copy for our own
	 * IOSurface-backed buffers.
	 */
	CGImageRef image = make_cgimage(data, width, height, stride, drm_format);
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

void *darwin_iosurface_ref(darwin_iosurface *surface) {
	return surface != NULL ? (void *)surface->ref : NULL;
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

/* ---- hardware cursor (overlay CALayer) ------------------------------------
 *
 * The cursor is a sublayer of the content layer. Because the content view is
 * flipped (isFlipped == YES), the backing layer's sublayers use a top-left
 * origin and render contents upright — the same convention as the primary
 * IOSurface — so no manual Y flip is needed. All cursor mutations run on the
 * main thread inside an action-disabled CATransaction, so repositioning is
 * instantaneous (no implicit move animation).
 *
 * Must be called on the main thread.
 */
static void cursor_ensure_layer(WlrDarwinWindow *win) {
	if (win->cursorLayer != nil) {
		return;
	}
	CALayer *c = [CALayer layer];
	c.contentsGravity = kCAGravityResize;
	[win->layer addSublayer:c]; /* sublayers composite above the layer's contents */
	win->cursorLayer = c;
}

/* Position the cursor layer from the last hotspot/size/position. Main thread. */
static void cursor_reposition(WlrDarwinWindow *win) {
	if (win->cursorLayer == nil) {
		return;
	}
	CGFloat scale = win->layer.contentsScale;
	if (scale <= 0.0) {
		scale = 1.0;
	}
	win->cursorLayer.contentsScale = scale;
	win->cursorLayer.frame = CGRectMake(
		(win->cursorX - win->cursorHotspotX) / scale,
		(win->cursorY - win->cursorHotspotY) / scale,
		win->cursorW / scale, win->cursorH / scale);
}

void darwin_cocoa_window_set_cursor_surface(darwin_cocoa_window *handle,
		darwin_iosurface *surface, int width, int height,
		int hotspot_x, int hotspot_y) {
	WlrDarwinWindow *win = (__bridge WlrDarwinWindow *)handle;
	IOSurfaceRef ref = surface != NULL ? surface->ref : NULL;
	if (ref != NULL) {
		CFRetain(ref); /* hold across the async hand-off */
	}
	dispatch_async(dispatch_get_main_queue(), ^{
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		if (ref == NULL) {
			win->cursorLayer.hidden = YES; /* no-op if never created */
		} else {
			cursor_ensure_layer(win);
			win->cursorHotspotX = hotspot_x;
			win->cursorHotspotY = hotspot_y;
			win->cursorW = width;
			win->cursorH = height;
			win->cursorLayer.contents = (__bridge id)ref;
			win->cursorLayer.hidden = NO;
			cursor_reposition(win);
			CFRelease(ref);
		}
		[CATransaction commit];
	});
}

void darwin_cocoa_window_set_cursor_pixels(darwin_cocoa_window *handle,
		const void *data, int width, int height, int stride,
		uint32_t drm_format, int hotspot_x, int hotspot_y) {
	WlrDarwinWindow *win = (__bridge WlrDarwinWindow *)handle;
	CGImageRef image = make_cgimage(data, width, height, (uint32_t)stride,
		drm_format);
	if (image == NULL) {
		return;
	}
	dispatch_async(dispatch_get_main_queue(), ^{
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		cursor_ensure_layer(win);
		win->cursorHotspotX = hotspot_x;
		win->cursorHotspotY = hotspot_y;
		win->cursorW = width;
		win->cursorH = height;
		win->cursorLayer.contents = (__bridge id)image;
		win->cursorLayer.hidden = NO;
		cursor_reposition(win);
		[CATransaction commit];
		CGImageRelease(image);
	});
}

void darwin_cocoa_window_move_cursor(darwin_cocoa_window *handle, int x, int y) {
	WlrDarwinWindow *win = (__bridge WlrDarwinWindow *)handle;
	dispatch_async(dispatch_get_main_queue(), ^{
		win->cursorX = x;
		win->cursorY = y;
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		cursor_reposition(win);
		[CATransaction commit];
	});
}

/* ---- trampoline (public API) ---------------------------------------------- */

static int g_return_code = 0;

/*
 * A minimal main menu. Required for a Regular-policy app: Command-key events are
 * routed through the menu for key-equivalents, so a nil mainMenu can fault when
 * Command is pressed. The single Quit item also gives the window a clean way to
 * terminate.
 */
static void install_main_menu(void) {
	NSMenu *menubar = [[NSMenu alloc] init];
	NSMenuItem *appItem = [[NSMenuItem alloc] init];
	[menubar addItem:appItem];

	NSMenu *appMenu = [[NSMenu alloc] init];
	NSString *name = [[NSProcessInfo processInfo] processName];
	[appMenu addItemWithTitle:[@"Quit " stringByAppendingString:name]
		action:@selector(terminate:) keyEquivalent:@"q"];
	appItem.submenu = appMenu;

	NSApp.mainMenu = menubar;
}

int wlr_darwin_application_run(int (*compositor_main)(void *), void *data) {
	@autoreleasepool {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
		install_main_menu();

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
