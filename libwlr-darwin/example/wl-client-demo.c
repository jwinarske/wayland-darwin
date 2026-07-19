/*
 * wl-client-demo — a minimal Wayland client for macOS.
 *
 * A pure libwayland-client program (no wlroots): connects to WAYLAND_DISPLAY,
 * creates an xdg-shell toplevel, allocates a shared-memory buffer, draws a
 * checkerboard, and commits. Use it to give darwin-tinywl something to show:
 *
 *     ./darwin-tinywl &                 # prints WAYLAND_DISPLAY=wayland-1
 *     WAYLAND_DISPLAY=wayland-1 ./wl-client-demo
 *
 * shm buffers use shm_open (portable to macOS); the fd is passed to the
 * compositor over the wayland socket (SCM_RIGHTS) and mmap'd there.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

#define WIDTH 640
#define HEIGHT 480

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static int running = 1;

/* -- shm buffer -- */

static int create_shm_file(size_t size) {
	static int counter = 0;
	char name[32];
	snprintf(name, sizeof(name), "/wlcd-%d-%d", (int)getpid(), counter++);
	int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		return -1;
	}
	shm_unlink(name);
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static struct wl_buffer *make_buffer(void) {
	int stride = WIDTH * 4;
	size_t size = (size_t)stride * HEIGHT;
	int fd = create_shm_file(size);
	if (fd < 0) {
		return NULL;
	}
	uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	for (int y = 0; y < HEIGHT; y++) {
		for (int x = 0; x < WIDTH; x++) {
			int checker = ((x / 32) + (y / 32)) % 2;
			pixels[y * WIDTH + x] = checker ? 0xFF3060A0 : 0xFF102838;
		}
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
		WIDTH, HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	munmap(pixels, size);
	close(fd);
	return buffer;
}

/* -- listeners -- */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
	}
}
static void registry_global_remove(void *data, struct wl_registry *reg,
		uint32_t name) {}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
	xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
		uint32_t serial) {
	xdg_surface_ack_configure(xdg_surface, serial);
	struct wl_buffer *buffer = make_buffer();
	if (buffer != NULL) {
		wl_surface_attach(surface, buffer, 0, 0);
		wl_surface_damage_buffer(surface, 0, 0, WIDTH, HEIGHT);
		wl_surface_commit(surface);
	}
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
		int32_t width, int32_t height, struct wl_array *states) {}
static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	running = 0;
}
static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
		int32_t width, int32_t height) {}
static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
		struct wl_array *capabilities) {}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to connect to WAYLAND_DISPLAY\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (compositor == NULL || shm == NULL || wm_base == NULL) {
		fprintf(stderr, "compositor is missing required globals\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);

	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xdg_surface =
		xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "wl-client-demo");
	wl_surface_commit(surface);

	while (running && wl_display_dispatch(display) != -1) {
	}

	wl_display_disconnect(display);
	return 0;
}
