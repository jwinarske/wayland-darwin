/*
 * metal.m — all Metal / Objective-C for the renderer (D7 containment).
 *
 * ARC does not manage Objective-C pointers stored in malloc'd C structs, so the
 * device/pass state lives in ObjC objects handed across the C boundary as
 * opaque, CFBridgingRetain'd handles (the same pattern as cocoa.m).
 */
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>

#include <stdlib.h>

#include "metal.h"

/* Solid-colour pipeline. Vertices are NDC; colour is a fragment uniform. */
static const char *kShaderSource =
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct VOut { float4 pos [[position]]; };\n"
	"vertex VOut solid_vs(uint vid [[vertex_id]],\n"
	"                     constant float2 *verts [[buffer(0)]]) {\n"
	"    VOut o; o.pos = float4(verts[vid], 0.0, 1.0); return o;\n"
	"}\n"
	"fragment float4 solid_fs(constant float4 &color [[buffer(0)]]) {\n"
	"    return color;\n"
	"}\n"
	"struct TVOut { float4 pos [[position]]; float2 uv; };\n"
	"vertex TVOut tex_vs(uint vid [[vertex_id]],\n"
	"                    constant float4 *v [[buffer(0)]]) {\n"
	"    TVOut o; o.pos = float4(v[vid].xy, 0.0, 1.0); o.uv = v[vid].zw;\n"
	"    return o;\n"
	"}\n"
	"fragment float4 tex_fs(TVOut in [[stage_in]],\n"
	"                       texture2d<float> tex [[texture(0)]],\n"
	"                       sampler smp [[sampler(0)]],\n"
	"                       constant float &alpha [[buffer(0)]]) {\n"
	"    return tex.sample(smp, in.uv) * alpha;\n"
	"}\n";

@interface DarwinMetal : NSObject {
@public
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	id<MTLRenderPipelineState> solid;
	id<MTLRenderPipelineState> textured;
	id<MTLSamplerState> samplerLinear;
	id<MTLSamplerState> samplerNearest;
}
@end
@implementation DarwinMetal
@end

@interface DarwinMetalTexture : NSObject {
@public
	id<MTLTexture> texture;
	uint32_t width, height;
}
@end
@implementation DarwinMetalTexture
@end

/* sRGB straight-alpha over blending, shared by both pipelines. */
static void configure_blend(MTLRenderPipelineColorAttachmentDescriptor *att) {
	att.pixelFormat = MTLPixelFormatBGRA8Unorm;
	att.blendingEnabled = YES;
	att.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
	att.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	att.sourceAlphaBlendFactor = MTLBlendFactorOne;
	att.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
}

@interface DarwinMetalPass : NSObject {
@public
	DarwinMetal *metal;
	id<MTLTexture> target;
	id<MTLCommandBuffer> cmd;
	id<MTLRenderCommandEncoder> enc;
	uint32_t width, height;
}
@end
@implementation DarwinMetalPass
@end

darwin_metal *darwin_metal_create(void) {
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (device == nil) {
		return NULL;
	}

	NSError *err = nil;
	id<MTLLibrary> lib = [device newLibraryWithSource:@(kShaderSource)
		options:nil error:&err];
	if (lib == nil) {
		NSLog(@"libwlr-darwin: Metal shader compile failed: %@", err);
		return NULL;
	}

	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"solid_vs"];
	pd.fragmentFunction = [lib newFunctionWithName:@"solid_fs"];
	configure_blend(pd.colorAttachments[0]);
	id<MTLRenderPipelineState> solid =
		[device newRenderPipelineStateWithDescriptor:pd error:&err];
	if (solid == nil) {
		NSLog(@"libwlr-darwin: Metal solid pipeline failed: %@", err);
		return NULL;
	}

	MTLRenderPipelineDescriptor *tpd = [MTLRenderPipelineDescriptor new];
	tpd.vertexFunction = [lib newFunctionWithName:@"tex_vs"];
	tpd.fragmentFunction = [lib newFunctionWithName:@"tex_fs"];
	configure_blend(tpd.colorAttachments[0]);
	id<MTLRenderPipelineState> textured =
		[device newRenderPipelineStateWithDescriptor:tpd error:&err];
	if (textured == nil) {
		NSLog(@"libwlr-darwin: Metal textured pipeline failed: %@", err);
		return NULL;
	}

	MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
	sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
	id<MTLSamplerState> samplerLinear = [device newSamplerStateWithDescriptor:sd];
	sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
	id<MTLSamplerState> samplerNearest = [device newSamplerStateWithDescriptor:sd];

	DarwinMetal *m = [DarwinMetal new];
	m->device = device;
	m->queue = [device newCommandQueue];
	m->solid = solid;
	m->textured = textured;
	m->samplerLinear = samplerLinear;
	m->samplerNearest = samplerNearest;
	return (darwin_metal *)CFBridgingRetain(m);
}

void darwin_metal_destroy(darwin_metal *handle) {
	if (handle == NULL) {
		return;
	}
	DarwinMetal *m = (DarwinMetal *)CFBridgingRelease(handle);
	(void)m; /* ARC releases device/queue/pipeline */
}

darwin_metal_pass *darwin_metal_begin(darwin_metal *handle, void *iosurface_ref,
		uint32_t width, uint32_t height) {
	DarwinMetal *m = (__bridge DarwinMetal *)handle;
	IOSurfaceRef surface = (IOSurfaceRef)iosurface_ref;

	MTLTextureDescriptor *td = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		width:width height:height mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModeShared;
	id<MTLTexture> target = [m->device newTextureWithDescriptor:td
		iosurface:surface plane:0];
	if (target == nil) {
		NSLog(@"libwlr-darwin: newTextureWithDescriptor:iosurface: failed");
		return NULL;
	}

	MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
	rpd.colorAttachments[0].texture = target;
	rpd.colorAttachments[0].loadAction = MTLLoadActionLoad;
	rpd.colorAttachments[0].storeAction = MTLStoreActionStore;

	id<MTLCommandBuffer> cmd = [m->queue commandBuffer];
	id<MTLRenderCommandEncoder> enc =
		[cmd renderCommandEncoderWithDescriptor:rpd];
	[enc setRenderPipelineState:m->solid];

	DarwinMetalPass *p = [DarwinMetalPass new];
	p->metal = m;
	p->target = target;
	p->cmd = cmd;
	p->enc = enc;
	p->width = width;
	p->height = height;
	return (darwin_metal_pass *)CFBridgingRetain(p);
}

void darwin_metal_pass_rect(darwin_metal_pass *handle, int x, int y, int w, int h,
		float r, float g, float b, float a, int blend) {
	DarwinMetalPass *p = (__bridge DarwinMetalPass *)handle;
	float W = (float)p->width, H = (float)p->height;

	/* Pixel box (top-left origin) -> NDC (y up), as a triangle strip. */
	float x0 = 2.0f * (float)x / W - 1.0f;
	float x1 = 2.0f * (float)(x + w) / W - 1.0f;
	float y0 = 1.0f - 2.0f * (float)y / H;
	float y1 = 1.0f - 2.0f * (float)(y + h) / H;
	float verts[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
	float color[4] = { r, g, b, a };

	(void)blend; /* single alpha-over pipeline for now */
	[p->enc setRenderPipelineState:p->metal->solid];
	[p->enc setVertexBytes:verts length:sizeof(verts) atIndex:0];
	[p->enc setFragmentBytes:color length:sizeof(color) atIndex:0];
	[p->enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0
		vertexCount:4];
}

bool darwin_metal_pass_submit(darwin_metal_pass *handle) {
	DarwinMetalPass *p = (DarwinMetalPass *)CFBridgingRelease(handle);
	[p->enc endEncoding];
	[p->cmd commit];
	[p->cmd waitUntilCompleted];
	return p->cmd.status == MTLCommandBufferStatusCompleted;
}

/* -- textures -- */

darwin_metal_texture *darwin_metal_texture_create(darwin_metal *handle,
		uint32_t width, uint32_t height, uint32_t drm_format,
		const void *data, uint32_t stride) {
	DarwinMetal *m = (__bridge DarwinMetal *)handle;
	(void)drm_format; /* BGRA only for now */

	MTLTextureDescriptor *td = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		width:width height:height mipmapped:NO];
	td.usage = MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModeShared;
	id<MTLTexture> texture = [m->device newTextureWithDescriptor:td];
	if (texture == nil) {
		return NULL;
	}
	[texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
		mipmapLevel:0 withBytes:data bytesPerRow:stride];

	DarwinMetalTexture *t = [DarwinMetalTexture new];
	t->texture = texture;
	t->width = width;
	t->height = height;
	return (darwin_metal_texture *)CFBridgingRetain(t);
}

bool darwin_metal_texture_update(darwin_metal_texture *handle, const void *data,
		uint32_t stride) {
	DarwinMetalTexture *t = (__bridge DarwinMetalTexture *)handle;
	[t->texture replaceRegion:MTLRegionMake2D(0, 0, t->width, t->height)
		mipmapLevel:0 withBytes:data bytesPerRow:stride];
	return true;
}

void darwin_metal_texture_destroy(darwin_metal_texture *handle) {
	if (handle == NULL) {
		return;
	}
	DarwinMetalTexture *t = (DarwinMetalTexture *)CFBridgingRelease(handle);
	(void)t; /* ARC releases the MTLTexture */
}

void darwin_metal_pass_texture(darwin_metal_pass *handle,
		darwin_metal_texture *thandle, int dx, int dy, int dw, int dh,
		float sx, float sy, float sw, float sh, float alpha, int nearest) {
	DarwinMetalPass *p = (__bridge DarwinMetalPass *)handle;
	DarwinMetalTexture *t = (__bridge DarwinMetalTexture *)thandle;
	float W = (float)p->width, H = (float)p->height;

	float x0 = 2.0f * (float)dx / W - 1.0f;
	float x1 = 2.0f * (float)(dx + dw) / W - 1.0f;
	float y0 = 1.0f - 2.0f * (float)dy / H;
	float y1 = 1.0f - 2.0f * (float)(dy + dh) / H;
	float u0 = sx, u1 = sx + sw, v0 = sy, v1 = sy + sh;
	/* Each vertex is (ndc_x, ndc_y, u, v); triangle strip. */
	float verts[16] = {
		x0, y0, u0, v0,  x1, y0, u1, v0,
		x0, y1, u0, v1,  x1, y1, u1, v1,
	};

	[p->enc setRenderPipelineState:p->metal->textured];
	[p->enc setVertexBytes:verts length:sizeof(verts) atIndex:0];
	[p->enc setFragmentTexture:t->texture atIndex:0];
	[p->enc setFragmentSamplerState:(nearest ? p->metal->samplerNearest
					: p->metal->samplerLinear) atIndex:0];
	[p->enc setFragmentBytes:&alpha length:sizeof(alpha) atIndex:0];
	[p->enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0
		vertexCount:4];
}
