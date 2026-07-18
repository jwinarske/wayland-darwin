/*
 * kVK (Carbon/AppKit virtual key code) -> Linux evdev key code.
 *
 * xkbcommon speaks evdev codes, so this is the single translation table the
 * Darwin backend maintains (D5b). Compositor-owned xkb keymaps then turn these
 * into keysyms. Returns 0 for keys we don't map.
 */
#include <linux/input-event-codes.h>

#include "input.h"

/* Carbon virtual key codes (subset), from <Carbon/HIToolbox/Events.h>. */
uint32_t darwin_kvk_to_evdev(uint16_t kvk) {
	switch (kvk) {
	/* ANSI letters */
	case 0x00: return KEY_A;
	case 0x0B: return KEY_B;
	case 0x08: return KEY_C;
	case 0x02: return KEY_D;
	case 0x0E: return KEY_E;
	case 0x03: return KEY_F;
	case 0x05: return KEY_G;
	case 0x04: return KEY_H;
	case 0x22: return KEY_I;
	case 0x26: return KEY_J;
	case 0x28: return KEY_K;
	case 0x25: return KEY_L;
	case 0x2E: return KEY_M;
	case 0x2D: return KEY_N;
	case 0x1F: return KEY_O;
	case 0x23: return KEY_P;
	case 0x0C: return KEY_Q;
	case 0x0F: return KEY_R;
	case 0x01: return KEY_S;
	case 0x11: return KEY_T;
	case 0x20: return KEY_U;
	case 0x09: return KEY_V;
	case 0x0D: return KEY_W;
	case 0x07: return KEY_X;
	case 0x10: return KEY_Y;
	case 0x06: return KEY_Z;
	/* Number row */
	case 0x12: return KEY_1;
	case 0x13: return KEY_2;
	case 0x14: return KEY_3;
	case 0x15: return KEY_4;
	case 0x17: return KEY_5;
	case 0x16: return KEY_6;
	case 0x1A: return KEY_7;
	case 0x1C: return KEY_8;
	case 0x19: return KEY_9;
	case 0x1D: return KEY_0;
	/* Punctuation */
	case 0x1B: return KEY_MINUS;
	case 0x18: return KEY_EQUAL;
	case 0x21: return KEY_LEFTBRACE;
	case 0x1E: return KEY_RIGHTBRACE;
	case 0x2A: return KEY_BACKSLASH;
	case 0x29: return KEY_SEMICOLON;
	case 0x27: return KEY_APOSTROPHE;
	case 0x32: return KEY_GRAVE;
	case 0x2B: return KEY_COMMA;
	case 0x2F: return KEY_DOT;
	case 0x2C: return KEY_SLASH;
	/* Whitespace / editing */
	case 0x24: return KEY_ENTER;
	case 0x30: return KEY_TAB;
	case 0x31: return KEY_SPACE;
	case 0x33: return KEY_BACKSPACE;
	case 0x35: return KEY_ESC;
	case 0x75: return KEY_DELETE;
	/* Modifiers */
	case 0x37: return KEY_LEFTMETA;   /* Command */
	case 0x36: return KEY_RIGHTMETA;  /* Right Command */
	case 0x38: return KEY_LEFTSHIFT;
	case 0x3C: return KEY_RIGHTSHIFT;
	case 0x3A: return KEY_LEFTALT;    /* Option */
	case 0x3D: return KEY_RIGHTALT;
	case 0x3B: return KEY_LEFTCTRL;
	case 0x3E: return KEY_RIGHTCTRL;
	case 0x39: return KEY_CAPSLOCK;
	/* Navigation */
	case 0x7B: return KEY_LEFT;
	case 0x7C: return KEY_RIGHT;
	case 0x7D: return KEY_DOWN;
	case 0x7E: return KEY_UP;
	case 0x73: return KEY_HOME;
	case 0x77: return KEY_END;
	case 0x74: return KEY_PAGEUP;
	case 0x79: return KEY_PAGEDOWN;
	/* Function keys */
	case 0x7A: return KEY_F1;
	case 0x78: return KEY_F2;
	case 0x63: return KEY_F3;
	case 0x76: return KEY_F4;
	case 0x60: return KEY_F5;
	case 0x61: return KEY_F6;
	case 0x62: return KEY_F7;
	case 0x64: return KEY_F8;
	case 0x65: return KEY_F9;
	case 0x6D: return KEY_F10;
	case 0x67: return KEY_F11;
	case 0x6F: return KEY_F12;
	/* Keypad */
	case 0x52: return KEY_KP0;
	case 0x53: return KEY_KP1;
	case 0x54: return KEY_KP2;
	case 0x55: return KEY_KP3;
	case 0x56: return KEY_KP4;
	case 0x57: return KEY_KP5;
	case 0x58: return KEY_KP6;
	case 0x59: return KEY_KP7;
	case 0x5B: return KEY_KP8;
	case 0x5C: return KEY_KP9;
	case 0x45: return KEY_KPPLUS;
	case 0x4E: return KEY_KPMINUS;
	case 0x43: return KEY_KPASTERISK;
	case 0x4B: return KEY_KPSLASH;
	case 0x4C: return KEY_KPENTER;
	case 0x41: return KEY_KPDOT;
	case 0x51: return KEY_KPEQUAL;
	case 0x47: return KEY_NUMLOCK;
	/* Media */
	case 0x48: return KEY_VOLUMEUP;
	case 0x49: return KEY_VOLUMEDOWN;
	case 0x4A: return KEY_MUTE;
	default: return 0;
	}
}
