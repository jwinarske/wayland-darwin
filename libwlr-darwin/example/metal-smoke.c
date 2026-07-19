/*
 * Metal renderer smoke — HEADLESS, so CI actually runs it.
 *
 * 1: solid red rect into an IOSurface (add_rect).
 * 2: upload a green texture, composite it (texture_from_buffer + add_texture).
 * 3: composite a top-left-red texture with a 180 transform -> red lands
 *    bottom-right (transform correctness).
 * 4: read a texture back with wlr_texture_read_pixels (screencopy path).
 *
 * Exit 0 = PASS (or SKIP if no Metal device).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <drm_fourcc.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr-darwin.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#define SZ 64

static struct wlr_buffer *make_buffer(struct wlr_allocator *alloc) {
	uint64_t mods[] = { DRM_FORMAT_MOD_LINEAR };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888, .len = 1, .capacity = 1, .modifiers = mods,
	};
	return wlr_allocator_create_buffer(alloc, SZ, SZ, &format);
}

/* Fill a buffer: cb(x,y,px) writes BGRA into px. */
static bool fill(struct wlr_buffer *buffer,
		void (*cb)(int, int, uint8_t *)) {
	void *data; uint32_t f; size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &f, &stride)) {
		return false;
	}
	for (int y = 0; y < SZ; y++) {
		for (int x = 0; x < SZ; x++) {
			cb(x, y, (uint8_t *)data + y * stride + x * 4);
		}
	}
	wlr_buffer_end_data_ptr_access(buffer);
	return true;
}

static bool read_at(struct wlr_buffer *buffer, int x, int y, uint8_t out[4]) {
	void *data; uint32_t f; size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &f, &stride)) {
		return false;
	}
	uint8_t *px = (uint8_t *)data + y * stride + x * 4;
	for (int i = 0; i < 4; i++) out[i] = px[i];
	wlr_buffer_end_data_ptr_access(buffer);
	return true;
}

static void px_green(int x, int y, uint8_t *p)  { p[0]=0;   p[1]=255; p[2]=0;   p[3]=255; }
static void px_tl_red(int x, int y, uint8_t *p) { /* red in the top-left quadrant */
	int r = (x < SZ/2 && y < SZ/2);
	p[0]=0; p[1]=0; p[2]=r?255:0; p[3]=255;
}

int
main(void)
{
	wlr_log_init(WLR_ERROR, NULL);
	struct wlr_renderer *renderer = wlr_darwin_metal_renderer_create();
	if (renderer == NULL) {
		printf("metal smoke: SKIP (no Metal device on this runner)\n");
		return 0;
	}
	struct wlr_allocator *alloc = wlr_darwin_allocator_create();
	struct wlr_buffer *target = make_buffer(alloc);
	if (alloc == NULL || target == NULL) { fprintf(stderr, "FAIL: setup\n"); return 1; }

	int ok = 1;
	uint8_t p[4];

	/* 1: solid red rect */
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer, target, NULL);
	struct wlr_render_rect_options rect = {
		.box = {0,0,SZ,SZ}, .color = {.r=1,.g=0,.b=0,.a=1} };
	wlr_render_pass_add_rect(pass, &rect);
	wlr_render_pass_submit(pass);
	read_at(target, 0, 0, p);
	int red_ok = p[2]>200 && p[1]<60 && p[0]<60;
	printf("1 rect      BGRA=%u,%u,%u,%u %s\n", p[0],p[1],p[2],p[3], red_ok?"ok":"BAD");
	ok &= red_ok;

	/* 2: composite a green texture */
	struct wlr_buffer *green = make_buffer(alloc); fill(green, px_green);
	struct wlr_texture *tg = wlr_texture_from_buffer(renderer, green);
	pass = wlr_renderer_begin_buffer_pass(renderer, target, NULL);
	struct wlr_render_texture_options to = { .texture=tg, .dst_box={0,0,SZ,SZ} };
	wlr_render_pass_add_texture(pass, &to);
	wlr_render_pass_submit(pass);
	read_at(target, 0, 0, p);
	int green_ok = p[1]>200 && p[0]<60 && p[2]<60;
	printf("2 texture   BGRA=%u,%u,%u,%u %s\n", p[0],p[1],p[2],p[3], green_ok?"ok":"BAD");
	ok &= green_ok;

	/* 3: top-left-red texture, 180 transform -> red at bottom-right */
	struct wlr_buffer *tlr = make_buffer(alloc); fill(tlr, px_tl_red);
	struct wlr_texture *tt = wlr_texture_from_buffer(renderer, tlr);
	pass = wlr_renderer_begin_buffer_pass(renderer, target, NULL);
	struct wlr_render_texture_options tro = {
		.texture=tt, .dst_box={0,0,SZ,SZ}, .transform=WL_OUTPUT_TRANSFORM_180 };
	wlr_render_pass_add_texture(pass, &tro);
	wlr_render_pass_submit(pass);
	read_at(target, SZ-1, SZ-1, p);
	int br_red = p[2]>200;                 /* bottom-right should now be red */
	uint8_t p2[4]; read_at(target, 0, 0, p2);
	int tl_black = p2[2]<60;               /* top-left should be black */
	printf("3 transform BR=%u,%u,%u TL.r=%u %s\n", p[0],p[1],p[2], p2[2],
		(br_red&&tl_black)?"ok":"BAD");
	ok &= br_red && tl_black;

	/* 4: read a texture back (screencopy path) */
	uint8_t *rb = calloc(SZ*SZ, 4);
	struct wlr_texture_read_pixels_options rpo = {
		.data=rb, .format=DRM_FORMAT_XRGB8888, .stride=SZ*4 };
	int read_ok = wlr_texture_read_pixels(tg, &rpo) &&
		rb[1]>200 && rb[0]<60 && rb[2]<60; /* tg is green */
	printf("4 readpix   BGRA=%u,%u,%u,%u %s\n", rb[0],rb[1],rb[2],rb[3], read_ok?"ok":"BAD");
	ok &= read_ok;
	free(rb);

	printf(ok ? "metal smoke: PASS (rect, texture, transform, read_pixels)\n"
		  : "metal smoke: FAIL\n");
	return ok ? 0 : 1;
}
