/*
 * C <-> Metal boundary (all Metal/Objective-C lives in metal.m).
 *
 * The renderer (renderer.c, pure C, implements the wlroots renderer/pass
 * interfaces) drives Metal through these calls. Render targets are IOSurfaces
 * (the same surfaces the allocator hands out), so rendering is into the buffer
 * the compositor will present — no copy.
 */
#ifndef WLR_DARWIN_METAL_H
#define WLR_DARWIN_METAL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct darwin_metal darwin_metal;
typedef struct darwin_metal_pass darwin_metal_pass;

/* Create the Metal device + command queue + pipelines. NULL if no GPU/Metal. */
darwin_metal *darwin_metal_create(void);
void darwin_metal_destroy(darwin_metal *metal);

/* Begin a render pass into an IOSurface (BGRA) of the given size. */
darwin_metal_pass *darwin_metal_begin(darwin_metal *metal, void *iosurface_ref,
	uint32_t width, uint32_t height);

/*
 * Draw a solid-colour rect. box is in target pixels, top-left origin; colour is
 * straight (non-premultiplied) RGBA in [0,1]. blend != 0 = alpha-over, 0 = replace.
 */
void darwin_metal_pass_rect(darwin_metal_pass *pass, int x, int y, int w, int h,
	float r, float g, float b, float a, int blend);

/*
 * End encoding, commit, and wait for completion. If out_gpu_ns is non-NULL it
 * receives the GPU execution time in nanoseconds (render-timer telemetry).
 */
bool darwin_metal_pass_submit(darwin_metal_pass *pass, int64_t *out_gpu_ns);

/* -- textures (client surfaces) -- */

typedef struct darwin_metal_texture darwin_metal_texture;

/* Create an MTLTexture (BGRA) and upload `data` (LINEAR, `stride` bytes/row). */
darwin_metal_texture *darwin_metal_texture_create(darwin_metal *metal,
	uint32_t width, uint32_t height, uint32_t drm_format,
	const void *data, uint32_t stride);
/* Wrap an existing IOSurface as a sampleable texture (zero-copy). */
darwin_metal_texture *darwin_metal_texture_from_iosurface(darwin_metal *metal,
	void *iosurface_ref, uint32_t width, uint32_t height);
/* Re-upload the whole texture. */
bool darwin_metal_texture_update(darwin_metal_texture *tex, const void *data,
	uint32_t stride);
/* Re-upload just a sub-region (damage). `data` points at the full buffer. */
bool darwin_metal_texture_update_region(darwin_metal_texture *tex,
	const void *data, uint32_t stride, uint32_t x, uint32_t y,
	uint32_t w, uint32_t h);
/* Read a region of the texture back into CPU memory (BGRA). */
bool darwin_metal_texture_read(darwin_metal_texture *tex, void *dst,
	uint32_t stride, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void darwin_metal_texture_destroy(darwin_metal_texture *tex);

/*
 * Sample `tex` onto a quad. Destination box in target pixels (top-left origin).
 * uv holds the four source texture coordinates for the destination corners in
 * the order TL, TR, BL, BR (each u then v) — the caller bakes any
 * wl_output_transform into them. `alpha` scales opacity; `nearest` selects
 * nearest vs bilinear filtering.
 */
void darwin_metal_pass_texture(darwin_metal_pass *pass, darwin_metal_texture *tex,
	int dx, int dy, int dw, int dh, const float uv[8],
	float alpha, int nearest, int blend);

#endif
