/*
 * Input bridge wire format (main -> compositor).
 *
 * cocoa.m (main thread) captures NSEvents, translates them to these fixed-size
 * records, and writes them to the backend's input fd. backend.c (compositor
 * thread) reads the records and emits wlr_keyboard / wlr_pointer events.
 *
 * Both ends are in the same binary, so the struct layout matches.
 */
#ifndef WLR_DARWIN_INPUT_H
#define WLR_DARWIN_INPUT_H

#include <stdint.h>

enum darwin_input_type {
	DARWIN_INPUT_KEY = 1,     // code = evdev keycode, state = pressed
	DARWIN_INPUT_MOTION_ABS,  // x, y in [0,1] across the output
	DARWIN_INPUT_BUTTON,      // code = evdev BTN_*, state = pressed
	DARWIN_INPUT_AXIS,        // state = orientation, aux = source, x = delta
	DARWIN_INPUT_PINCH_BEGIN, // code = fingers
	DARWIN_INPUT_PINCH_UPDATE,// x,y = dx,dy; f0 = scale (abs); f1 = rotation (deg)
	DARWIN_INPUT_PINCH_END,
};

struct darwin_input_event {
	uint32_t type;
	uint32_t time_msec;
	uint32_t code;
	uint32_t state;
	uint32_t aux;
	int32_t discrete;
	double x, y;
	double f0, f1; // pinch scale / rotation (and future gesture params)
};

/*
 * Translate a Carbon/AppKit virtual key code (NSEvent.keyCode) to a Linux evdev
 * key code (xkbcommon's pivot); returns 0 if unmapped. The single maintained
 * translation table.
 */
uint32_t darwin_kvk_to_evdev(uint16_t kvk);

#endif
