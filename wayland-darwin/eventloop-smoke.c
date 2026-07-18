/*
 * Event-loop smoke test for libwayland on macOS (Darwin).
 *
 * macOS has no epoll/timerfd/signalfd/eventfd, and libwayland's wl_event_loop
 * is built on them (src/event-loop.c); here they run over epoll-shim/kqueue.
 * This test drives every shim-backed source type through one real dispatch
 * cycle:
 *
 *   - fd source      -> epoll_ctl / epoll_wait   (a pipe becomes readable)
 *   - timer source   -> timerfd                  (fires after 30 ms)
 *   - signal source  -> signalfd                 (SIGUSR1 raised on self)
 *   - idle source    -> loop bookkeeping         (runs once)
 *
 * The display's internal wakeup uses eventfd (src/wayland-server.c), which is
 * exercised implicitly whenever a wl_display is created; the full client<->
 * server roundtrip (socket SOCK_CLOEXEC + SCM_RIGHTS fd passing) is covered by
 * libwayland's own `meson test` suite, which the acceptance run also requires.
 *
 * Build (on macOS, against the libwayland built by build-macos.sh):
 *   cc g0-eventloop-smoke.c $(pkg-config --cflags --libs wayland-server) \
 *      $(pkg-config --cflags --libs epoll-shim) -o g0-eventloop-smoke
 * Exit status 0 = PASS.
 */
#include <wayland-server-core.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

struct fired {
	int fd_src;
	int timer_src;
	int signal_src;
	int idle_src;
};

static int
on_fd(int fd, uint32_t mask, void *data)
{
	struct fired *f = data;
	char buf[8];
	(void)mask;
	(void)read(fd, buf, sizeof buf);
	f->fd_src = 1;
	return 0;
}

static int
on_timer(void *data)
{
	struct fired *f = data;
	f->timer_src = 1;
	return 0;
}

static int
on_signal(int signal_number, void *data)
{
	struct fired *f = data;
	(void)signal_number;
	f->signal_src = 1;
	return 0;
}

static void
on_idle(void *data)
{
	struct fired *f = data;
	f->idle_src = 1;
}

int
main(void)
{
	struct fired fired = {0};
	int pipefd[2];

	if (pipe(pipefd) != 0) {
		perror("pipe");
		return 2;
	}

	struct wl_event_loop *loop = wl_event_loop_create();
	if (!loop) {
		fprintf(stderr, "wl_event_loop_create failed\n");
		return 2;
	}

	wl_event_loop_add_fd(loop, pipefd[0], WL_EVENT_READABLE, on_fd, &fired);

	struct wl_event_source *timer =
		wl_event_loop_add_timer(loop, on_timer, &fired);
	wl_event_source_timer_update(timer, 30 /* ms */);

	wl_event_loop_add_signal(loop, SIGUSR1, on_signal, &fired);
	wl_event_loop_add_idle(loop, on_idle, &fired);

	/* Make the fd and signal sources ready before we block. */
	(void)write(pipefd[1], "x", 1);
	raise(SIGUSR1);

	/* Dispatch a few times so the 30 ms timer has a chance to expire. */
	for (int i = 0; i < 20; i++) {
		wl_event_loop_dispatch(loop, 20 /* ms timeout */);
		if (fired.fd_src && fired.timer_src && fired.signal_src &&
		    fired.idle_src)
			break;
	}

	wl_event_loop_destroy(loop);
	close(pipefd[0]);
	close(pipefd[1]);

	printf("fd(epoll)=%d timer(timerfd)=%d signal(signalfd)=%d idle=%d\n",
	       fired.fd_src, fired.timer_src, fired.signal_src, fired.idle_src);

	if (fired.fd_src && fired.timer_src && fired.signal_src &&
	    fired.idle_src) {
		printf("event-loop smoke: PASS\n");
		return 0;
	}
	printf("event-loop smoke: FAIL\n");
	return 1;
}
