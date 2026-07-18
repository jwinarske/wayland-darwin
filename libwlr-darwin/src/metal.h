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
 * straight (non-premultiplied) RGBA in [0,1]. blend != 0 = alpha-over.
 */
void darwin_metal_pass_rect(darwin_metal_pass *pass, int x, int y, int w, int h,
	float r, float g, float b, float a, int blend);

/* End encoding, commit, and wait for completion. */
bool darwin_metal_pass_submit(darwin_metal_pass *pass);

/* -- textures (client surfaces) -- */

typedef struct darwin_metal_texture darwin_metal_texture;

/* Create an MTLTexture (BGRA) and upload `data` (LINEAR, `stride` bytes/row). */
darwin_metal_texture *darwin_metal_texture_create(darwin_metal *metal,
	uint32_t width, uint32_t height, uint32_t drm_format,
	const void *data, uint32_t stride);
/* Re-upload the whole texture (damage-aware upload is a later optimization). */
bool darwin_metal_texture_update(darwin_metal_texture *tex, const void *data,
	uint32_t stride);
void darwin_metal_texture_destroy(darwin_metal_texture *tex);

/*
 * Sample `tex` onto a quad. Destination box in target pixels (top-left origin);
 * source box normalized [0,1] in texture space. `alpha` scales opacity;
 * `nearest` picks nearest vs bilinear filtering.
 */
void darwin_metal_pass_texture(darwin_metal_pass *pass, darwin_metal_texture *tex,
	int dx, int dy, int dw, int dh,
	float sx, float sy, float sw, float sh,
	float alpha, int nearest);

#endif
