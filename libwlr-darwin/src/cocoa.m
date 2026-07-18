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

#include <unistd.h>

#include "cocoa.h"

/* DRM fourcc codes we understand, defined locally to avoid a libdrm include. */
#define DARWIN_DRM_FORMAT_XRGB8888 0x34325258 /* 'XR24' */
#define DARWIN_DRM_FORMAT_ARGB8888 0x34325241 /* 'AR24' */

/* ---- window object -------------------------------------------------------- */

@interface WlrDarwinWindow : NSObject {
@public
	NSWindow *window;
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

		NSView *view = nsw.contentView;
		view.wantsLayer = YES;
		CALayer *layer = view.layer;
		layer.contentsGravity = kCAGravityResize;
		layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);

		[nsw center];
		[nsw makeKeyAndOrderFront:nil];

		win = [WlrDarwinWindow new];
		win->window = nsw;
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
