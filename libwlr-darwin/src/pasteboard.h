/*
 * C <-> ObjC boundary for the macOS system clipboard (NSPasteboard).
 * Implemented in pasteboard.m; NSPasteboard access is marshalled to the main
 * thread. Text (UTF-8) only for now.
 */
#ifndef WLR_DARWIN_PASTEBOARD_H
#define WLR_DARWIN_PASTEBOARD_H

#include <stdint.h>

/* Monotonic change counter; increments whenever the pasteboard is written. */
int64_t darwin_pasteboard_change_count(void);

/* Current pasteboard text as a malloc'd UTF-8 string (caller frees), or NULL. */
char *darwin_pasteboard_get_text(void);

/* Replace the pasteboard with UTF-8 text; returns the new change count. */
int64_t darwin_pasteboard_set_text(const char *utf8);

#endif
