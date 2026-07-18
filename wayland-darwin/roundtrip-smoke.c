/*
 * Client<->server roundtrip smoke for libwayland on macOS (Darwin).
 *
 * Proves libwayland-server and libwayland-client actually talk over a real
 * unix socket on Darwin: it creates a server display, fork()s a client that
 * connects and performs a wl_display_roundtrip (a sync request + reply), and
 * checks the client exits cleanly. This exercises the listening-socket accept
 * path (the cloexec fallback added for Darwin) and the wire protocol end to
 * end — the surface libwayland's own (ELF/Linux-bound) test suite would cover.
 *
 * Requires XDG_RUNTIME_DIR to be set (Wayland sockets live there); build-macos.sh
 * sets a default. Exit status 0 = PASS.
 */
#include <wayland-server-core.h>
#include <wayland-client-core.h>

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int
main(void)
{
	struct wl_display *server = wl_display_create();
	if (!server) {
		fprintf(stderr, "wl_display_create failed\n");
		return 2;
	}

	const char *sock = wl_display_add_socket_auto(server);
	if (!sock) {
		fprintf(stderr, "wl_display_add_socket_auto failed "
				"(is XDG_RUNTIME_DIR set?)\n");
		return 2;
	}
	printf("server listening on %s\n", sock);

	/* Copy the name before fork; it lives in server memory. */
	char name[64];
	snprintf(name, sizeof name, "%s", sock);

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return 2;
	}

	if (pid == 0) {
		/* Child: connect as a client and force a wire roundtrip. */
		struct wl_display *c = wl_display_connect(name);
		if (!c) {
			fprintf(stderr, "client: wl_display_connect failed\n");
			_exit(3);
		}
		/* wl_display_roundtrip issues wl_display.sync and waits for the
		 * reply — a full request/reply cycle over the socket. */
		int rc = wl_display_roundtrip(c);
		wl_display_disconnect(c);
		_exit(rc < 0 ? 4 : 0);
	}

	/* Parent: service the client until it exits (or we time out). */
	struct wl_event_loop *loop = wl_display_get_event_loop(server);
	int status = -1;
	int reaped = 0;
	for (int i = 0; i < 250; i++) {
		wl_event_loop_dispatch(loop, 20 /* ms */);
		wl_display_flush_clients(server);
		if (waitpid(pid, &status, WNOHANG) == pid) {
			reaped = 1;
			break;
		}
	}
	wl_display_destroy(server);

	if (reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		printf("roundtrip smoke: PASS\n");
		return 0;
	}
	fprintf(stderr, "roundtrip smoke: FAIL (reaped=%d status=%d)\n",
		reaped, status);
	return 1;
}
