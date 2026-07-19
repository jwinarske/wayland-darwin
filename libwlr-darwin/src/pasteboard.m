/*
 * pasteboard.m — NSPasteboard access for the clipboard bridge.
 *
 * AppKit is main-thread-affine, so each operation is marshalled to the main
 * thread (the bridge itself runs on the compositor thread).
 */
#import <AppKit/AppKit.h>

#include <stdlib.h>
#include <string.h>

#include "pasteboard.h"

int64_t darwin_pasteboard_change_count(void) {
	__block NSInteger count = 0;
	dispatch_sync(dispatch_get_main_queue(), ^{
		count = [NSPasteboard generalPasteboard].changeCount;
	});
	return (int64_t)count;
}

char *darwin_pasteboard_get_text(void) {
	__block char *out = NULL;
	dispatch_sync(dispatch_get_main_queue(), ^{
		NSString *s = [[NSPasteboard generalPasteboard]
			stringForType:NSPasteboardTypeString];
		const char *u = s.UTF8String;
		if (u != NULL) {
			out = strdup(u);
		}
	});
	return out;
}

int64_t darwin_pasteboard_set_text(const char *utf8) {
	__block NSInteger count = 0;
	dispatch_sync(dispatch_get_main_queue(), ^{
		NSPasteboard *pb = [NSPasteboard generalPasteboard];
		[pb clearContents];
		NSString *s = [NSString stringWithUTF8String:(utf8 ? utf8 : "")];
		[pb setString:(s ? s : @"") forType:NSPasteboardTypeString];
		count = pb.changeCount;
	});
	return (int64_t)count;
}
