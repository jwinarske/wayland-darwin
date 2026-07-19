/*
 * Darwin compat shims, force-included (-include) when building the examples on
 * macOS. Provides the Linux-only syscalls a few example clients use, so the
 * upstream sources build unchanged. No-op on non-Apple platforms.
 *
 * (timerfd_create is provided by epoll-shim, not here.)
 */
#ifndef WLR_DARWIN_COMPAT_SYSCALLS_H
#define WLR_DARWIN_COMPAT_SYSCALLS_H

#if defined(__APPLE__)

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#define MFD_ALLOW_SEALING 0x0002U
#endif

/*
 * memfd_create via an unlinked temp file. Unlike a POSIX shm object, a regular
 * file has no ftruncate-once restriction and can be resized freely; the fd is
 * mmap-able and passable over SCM_RIGHTS, which is all clients need.
 */
static inline int memfd_create(const char *name, unsigned int flags) {
	(void)name;
	(void)flags;
	const char *dir = getenv("TMPDIR");
	if (dir == NULL || *dir == '\0') {
		dir = "/tmp";
	}
	char tmpl[1024];
	snprintf(tmpl, sizeof(tmpl), "%s/wcx-memfd-XXXXXX", dir);
	int fd = mkstemp(tmpl);
	if (fd < 0) {
		return -1;
	}
	unlink(tmpl);
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}

/* pipe2 via pipe + fcntl (macOS honours O_CLOEXEC / O_NONBLOCK there). */
static inline int pipe2(int fds[2], int flags) {
	if (pipe(fds) != 0) {
		return -1;
	}
	for (int i = 0; i < 2; i++) {
		if (flags & O_CLOEXEC) {
			(void)fcntl(fds[i], F_SETFD, FD_CLOEXEC);
		}
		if (flags & O_NONBLOCK) {
			(void)fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
		}
	}
	return 0;
}

#endif /* __APPLE__ */
#endif
