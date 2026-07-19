/*
 * Darwin compat: macOS has no <sys/sysmacros.h>, but provides the major() /
 * minor() / makedev() macros some examples use from <sys/types.h>.
 */
#ifndef WLR_DARWIN_COMPAT_SYS_SYSMACROS_H
#define WLR_DARWIN_COMPAT_SYS_SYSMACROS_H
#include <sys/types.h>
#endif
