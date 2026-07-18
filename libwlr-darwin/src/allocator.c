/*
 * IOSurface allocator (D2 phase 1).
 *
 * Produces wlr_buffers backed by IOSurface memory. The compositor's renderer
 * (pixman) draws directly into the IOSurface via begin/end_data_ptr_access
 * (mapped onto IOSurfaceLock/Unlock), and the backend presents the same
 * IOSurface to a CALayer with no copy (see output.c / cocoa.m).
 *
 * Caps: DATA_PTR. Foreign DATA_PTR buffers (e.g. the plain shm allocator)
 * still work via the backend's copy fallback, so autocreate keeps functioning.
 */
#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>

#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/util/log.h>

#include "wlr-darwin.h"
#include "darwin.h"

struct wlr_darwin_allocator {
	struct wlr_allocator base;
};

struct wlr_darwin_buffer {
	struct wlr_buffer base;
	darwin_iosurface *surface;
	uint32_t drm_format;
	size_t stride;
	bool locked_write;
};

static const struct wlr_buffer_impl buffer_impl;

static struct wlr_darwin_buffer *darwin_buffer_from_buffer(
		struct wlr_buffer *wlr_buffer) {
	assert(wlr_buffer->impl == &buffer_impl);
	struct wlr_darwin_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	return buffer;
}

static void buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct wlr_darwin_buffer *buffer = darwin_buffer_from_buffer(wlr_buffer);
	darwin_iosurface_destroy(buffer->surface);
	free(buffer);
}

static bool buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct wlr_darwin_buffer *buffer = darwin_buffer_from_buffer(wlr_buffer);
	buffer->locked_write = flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE;
	void *base = darwin_iosurface_lock(buffer->surface, buffer->locked_write);
	if (base == NULL) {
		return false;
	}
	*data = base;
	*format = buffer->drm_format;
	*stride = buffer->stride;
	return true;
}

static void buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	struct wlr_darwin_buffer *buffer = darwin_buffer_from_buffer(wlr_buffer);
	darwin_iosurface_unlock(buffer->surface, buffer->locked_write);
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.begin_data_ptr_access = buffer_begin_data_ptr_access,
	.end_data_ptr_access = buffer_end_data_ptr_access,
	// get_dmabuf / get_shm intentionally unimplemented: DATA_PTR buffer.
};

darwin_iosurface *darwin_buffer_get_iosurface(struct wlr_buffer *wlr_buffer) {
	if (wlr_buffer->impl != &buffer_impl) {
		return NULL;
	}
	return darwin_buffer_from_buffer(wlr_buffer)->surface;
}

static struct wlr_buffer *allocator_create_buffer(
		struct wlr_allocator *wlr_alloc, int width, int height,
		const struct wlr_drm_format *format) {
	struct wlr_darwin_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		return NULL;
	}

	uint32_t stride = 0;
	buffer->surface = darwin_iosurface_create((uint32_t)width,
		(uint32_t)height, format->format, &stride);
	if (buffer->surface == NULL) {
		wlr_log(WLR_ERROR, "Unsupported IOSurface format 0x%"PRIx32,
			format->format);
		free(buffer);
		return NULL;
	}
	buffer->drm_format = format->format;
	buffer->stride = stride;

	wlr_buffer_init(&buffer->base, &buffer_impl, width, height);
	return &buffer->base;
}

static void allocator_destroy(struct wlr_allocator *wlr_alloc) {
	struct wlr_darwin_allocator *alloc =
		wl_container_of(wlr_alloc, alloc, base);
	free(alloc);
}

static const struct wlr_allocator_interface allocator_impl = {
	.create_buffer = allocator_create_buffer,
	.destroy = allocator_destroy,
};

struct wlr_allocator *wlr_darwin_allocator_create(void) {
	struct wlr_darwin_allocator *alloc = calloc(1, sizeof(*alloc));
	if (alloc == NULL) {
		return NULL;
	}
	wlr_allocator_init(&alloc->base, &allocator_impl, WLR_BUFFER_CAP_DATA_PTR);
	return &alloc->base;
}

bool wlr_buffer_is_darwin(const struct wlr_buffer *buffer) {
	return buffer->impl == &buffer_impl;
}
