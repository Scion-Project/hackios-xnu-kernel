/*
 * SPDX-License-Identifier: MIT
 * The Metal-on-Vulkan plug-in (userland/src/metalvk/: HakiosMetalVK.mm and AdrenoMetal's frontend) on this Mac: the same sources built
 * for macOS, its device made the way Metal makes it (HakiosMetalDevice -initWithAcceleratorPort:), on a Vulkan driver of the Mac, and
 * used through the Metal API itself: a library from a metallib, compute pipelines, a command queue, compute, render and blit encoders,
 * a completion handler, and a texture over an IOSurface. Every number is checked against the CPU. tools/metal/metalvk-plugin-test.sh
 * builds and runs it.
 *
 * What it shows and what it does not: the frontend and the bridge do what the Metal API asks, on a real Vulkan driver, from the plug-in's
 * own entry. macOS's Metal is not iOS 16.5's (the frontend's device class is a subclass of macOS's _MTLDevice here, and Metal's own
 * discovery, MTLAddDevice, is not run), so it does not show that iOS's Metal takes the device: that is metal_test's to say on the guest.
 *
 * The binary must be called metal_test: the plug-in shows its device to that program only at metal=1, which is what a Mac's
 * kern.bootargs gives.
 *
 *   usage: metal_test ICD METALLIB
 */
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

/* libmetal2vulkan.dylib's count of its copies of no-copy buffers the driver could not import (tools/metal/m2v/0002) */
extern void nmb_host_copies(uint64_t out[4]);

static double
now_ms(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6;
}


static void
check(BOOL ok, NSString *what)
{
	printf("%s  %s\n", ok ? "ok  " : "FAIL", what.UTF8String);
	failures += !ok;
}

static BOOL
completed(id<MTLCommandBuffer> commands, NSString *what)
{
	[commands commit];
	[commands waitUntilCompleted];
	if (commands.status != MTLCommandBufferStatusCompleted) {
		check(NO, [NSString stringWithFormat:@"%@: status %ld, %@", what, (long)commands.status, commands.error]);
		return NO;
	}
	return YES;
}

static double
edge(const double a[2], const double b[2], const double p[2])
{
	return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]);
}

/* pixels (rows of ROW bytes, R, G, B at red, green, blue) against the red-green-blue triangle of corners; the wrong ones */
static unsigned
judge(const uint8_t *pixels, int width, int height, size_t row, int red, int blue, const float corners[3][2], unsigned *inside)
{
	double v[3][2];
	unsigned wrong = 0;

	for (int i = 0; i < 3; i++) {
		v[i][0] = (corners[i][0] + 1.0) * 0.5 * width;
		v[i][1] = (1.0 - corners[i][1]) * 0.5 * height;
	}
	const double area = edge(v[0], v[1], v[2]);
	*inside = 0;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const double c[2] = { x + 0.5, y + 0.5 };
			const double w[3] = { edge(v[1], v[2], c) / area, edge(v[2], v[0], c) / area, edge(v[0], v[1], c) / area };
			const uint8_t *p = pixels + y * row + x * 4;
			const uint8_t rgb[3] = { p[red], p[1], p[blue] };
			double nearest = 1e9;

			for (int i = 0; i < 3; i++) {
				const double *a = v[(i + 1) % 3], *b = v[(i + 2) % 3];
				nearest = fmin(nearest, fabs(edge(a, b, c)) / hypot(b[0] - a[0], b[1] - a[1]));
			}
			if (nearest < 1.0) {
				continue;
			}
			if (w[0] >= 0 && w[1] >= 0 && w[2] >= 0) {
				(*inside)++;
				for (int i = 0; i < 3; i++) {
					if (fabs(rgb[i] - w[i] * 255.0) > 2.5) {
						wrong++;
						break;
					}
				}
			} else if (rgb[0] || rgb[1] || rgb[2]) {
				wrong++;
			}
		}
	}
	return wrong;
}

static void
drawTriangle(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLRenderPipelineState> state, id<MTLTexture> target,
             id<MTLBuffer> readback, const float corners[3][2])
{
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = target;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
	id<MTLCommandBuffer> commands = [queue commandBuffer];
	id<MTLRenderCommandEncoder> render = [commands renderCommandEncoderWithDescriptor:pass];
	[render setRenderPipelineState:state];
	[render setVertexBytes:corners length:sizeof(float) * 6 atIndex:0];
	[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[render endEncoding];
	if (readback != nil) {
		id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
		[blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
		    sourceSize:MTLSizeMake(target.width, target.height, 1) toBuffer:readback destinationOffset:0
		    destinationBytesPerRow:target.width * 4 destinationBytesPerImage:target.width * 4 * target.height];
		[blit endEncoding];
	}
	completed(commands, [NSString stringWithFormat:@"triangle into %@", target.iosurface ? @"an IOSurface" : @"a texture"]);
	(void)device;
}

/*
 * METALVK_BENCH=1: what a frame costs on a full-screen surface of the iPhone X (1125 x 2436 BGRA8, 10.96 MB), the drawable's case:
 * each frame clears it to a colour of its own and draws the triangle, commits and waits; the same frames into a Private texture of
 * the same size are the frame without any copy. Where the driver imports the surface's pages, nothing is copied; where it does not,
 * the runtime copies the surface in where it changed (here it does not: only the GPU writes it) and the GPU's frame out
 * (tools/metal/m2v/0002-copied-host-memory.patch). The difference of the two times is what the copies cost a frame.
 */
static void
bench(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLRenderPipelineState> bgra, const float corners[3][2])
{
	enum { W = 1125, H = 2436, FRAMES = 30 };
	IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
		(id)kIOSurfaceWidth: @(W), (id)kIOSurfaceHeight: @(H), (id)kIOSurfaceBytesPerElement: @4,
		(id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') });
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
	d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	d.storageMode = MTLStorageModeShared;
	id<MTLTexture> onSurface = surface ? [device newTextureWithDescriptor:d iosurface:surface plane:0] : nil;
	d.storageMode = MTLStorageModePrivate;
	id<MTLTexture> private = [device newTextureWithDescriptor:d];
	if (onSurface == nil || private == nil) {
		check(NO, @"bench: the full-screen textures");
		return;
	}
	double ms[2];
	uint64_t before[4], after[4];
	for (int k = 0; k < 2; k++) {
		id<MTLTexture> target = k == 0 ? onSurface : private;
		if (k == 0) {
			nmb_host_copies(before);
		}
		double began = 0;
		for (int i = -2; i < FRAMES; i++) {             /* two frames first, not timed: pipelines made, memory placed */
			if (i == 0) {
				began = now_ms();
			}
			MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
			pass.colorAttachments[0].texture = target;
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			pass.colorAttachments[0].clearColor = MTLClearColorMake((i & 7) / 7.0, 0.5, 1.0 - (i & 3) / 3.0, 1);
			id<MTLCommandBuffer> commands = [queue commandBuffer];
			id<MTLRenderCommandEncoder> render = [commands renderCommandEncoderWithDescriptor:pass];
			[render setRenderPipelineState:bgra];
			[render setVertexBytes:corners length:sizeof(float) * 6 atIndex:0];
			[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[render endEncoding];
			if (!completed(commands, @"bench frame")) {
				return;
			}
		}
		ms[k] = (now_ms() - began) / FRAMES;
		if (k == 0) {
			nmb_host_copies(after);
		}
	}
	/* the last frame's clear colour, in the surface's own memory, at its top-left corner (outside the triangle) */
	IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
	const uint8_t *p = IOSurfaceGetBaseAddress(surface);
	const int last = FRAMES - 1;
	const int b = (int)lround((1.0 - (last & 3) / 3.0) * 255), g = 128, r = (int)lround((last & 7) / 7.0 * 255);
	BOOL right = abs(p[0] - b) <= 1 && abs(p[1] - g) <= 1 && abs(p[2] - r) <= 1;
	printf("      surface's corner after the last frame: %u,%u,%u (BGR; the clear colour %d,%d,%d)\n", p[0], p[1], p[2], b, g, r);
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	check(right, @"bench: the last frame is in the IOSurface's memory");
	const double in = (after[0] - before[0]) / (double)FRAMES / 1e6, out = (after[1] - before[1]) / (double)FRAMES / 1e6;
	printf("      bench: 1125x2436 BGRA8, %d frames: %.2f ms a frame into the IOSurface, %.2f ms into a Private texture: "
	    "%.2f ms of copies a frame;\n      the runtime's copies a frame: %.2f MB in (%.2f ms), %.2f MB out (%.2f ms)\n",
	    FRAMES, ms[0], ms[1], ms[0] - ms[1], in, (after[2] - before[2]) / (double)FRAMES / 1e6, out,
	    (after[3] - before[3]) / (double)FRAMES / 1e6);
	CFRelease(surface);
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	@autoreleasepool {
		if (argc < 3) {
			fprintf(stderr, "usage: %s ICD METALLIB\n", argv[0]);
			return 2;
		}
		setenv("HAKIOS_VK_ICD", argv[1], 1);
		Class cls = objc_getClass("HakiosMetalDevice");
		check(cls != Nil && class_getSuperclass(cls) == objc_getClass("_MTLDevice"),
		    @"the plug-in registered HakiosMetalDevice, a subclass of _MTLDevice");
		if (cls == Nil) {
			return 1;
		}
		/* as Metal calls it: [[cls alloc] initWithAcceleratorPort:], the allocated object handed over and the result owned */
		typedef id (*Init)(__attribute__((ns_consumed)) id, SEL, unsigned int) __attribute__((ns_returns_retained));
		id<MTLDevice> device = ((Init)objc_msgSend)([cls alloc], sel_registerName("initWithAcceleratorPort:"), 7);
		check(device != nil && [(id)device conformsToProtocol:@protocol(MTLDevice)] &&
		    class_getSuperclass(object_getClass(device)) == objc_getClass("_MTLDevice"),
		    [NSString stringWithFormat:@"-initWithAcceleratorPort: gives the frontend's device (%s, on _MTLDevice): %@",
		    object_getClassName(device), device.name]);
		if (device == nil) {
			return 1;
		}
		check(device.maxBufferLength > 0 && device.hasUnifiedMemory, [NSString stringWithFormat:@"maxBufferLength %llu, unified memory",
		    (unsigned long long)device.maxBufferLength]);

		NSError *error = nil;
		id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[2])] error:&error];
		check(library != nil, [NSString stringWithFormat:@"newLibraryWithURL: %@ (%@)", [library.functionNames componentsJoinedByString:@" "], error]);
		if (library == nil) {
			return 1;
		}
		id<MTLCommandQueue> queue = [device newCommandQueue];

		/* compute: a*k+b over 4096 floats, with a completion handler */
		enum { N = 4096 };
		float a[N], b[N], k = 3.0f;
		for (int i = 0; i < N; i++) {
			a[i] = (float)(i % 1000);
			b[i] = (float)(N - i);
		}
		id<MTLComputePipelineState> axpb = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"axpb"] error:&error];
		check(axpb != nil, [NSString stringWithFormat:@"newComputePipelineStateWithFunction: axpb (%@)", error]);
		id<MTLBuffer> ab = [device newBufferWithBytes:a length:sizeof(a) options:MTLResourceStorageModeShared];
		id<MTLBuffer> bb = [device newBufferWithBytes:b length:sizeof(b) options:MTLResourceStorageModeShared];
		id<MTLBuffer> out = [device newBufferWithLength:sizeof(a) options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> commands = [queue commandBuffer];
		__block BOOL handled = NO;
		[commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
			handled = done.status == MTLCommandBufferStatusCompleted;
		}];
		id<MTLComputeCommandEncoder> compute = [commands computeCommandEncoder];
		[compute setComputePipelineState:axpb];
		[compute setBuffer:ab offset:0 atIndex:0];
		[compute setBuffer:bb offset:0 atIndex:1];
		[compute setBuffer:out offset:0 atIndex:2];
		[compute setBytes:&k length:sizeof(k) atIndex:3];
		[compute dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
		[compute endEncoding];
		if (completed(commands, @"axpb")) {
			unsigned wrong = 0;
			const float *o = out.contents;
			for (int i = 0; i < N; i++) {
				wrong += o[i] != a[i] * k + b[i];
			}
			check(wrong == 0, [NSString stringWithFormat:@"axpb through the Metal API: %u of %d floats wrong", wrong, N]);
			check(handled, @"the completion handler ran, with the status completed");
		}

		/* threadgroups: a sum of 65536 numbers in groups of 256, with a device atomic */
		enum { M = 65536 };
		uint32_t *values = malloc(M * 4), expected = 0;
		for (unsigned i = 0; i < M; i++) {
			values[i] = (i * 2654435761u) >> 20;
			expected += values[i];
		}
		id<MTLComputePipelineState> sum = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"group_sum"] error:&error];
		id<MTLBuffer> vb = [device newBufferWithBytes:values length:M * 4 options:MTLResourceStorageModeShared];
		id<MTLBuffer> total = [device newBufferWithLength:4 options:MTLResourceStorageModeShared];
		id<MTLBuffer> partial = [device newBufferWithLength:(M / 256) * 4 options:MTLResourceStorageModeShared];
		commands = [queue commandBuffer];
		compute = [commands computeCommandEncoder];
		[compute setComputePipelineState:sum];
		[compute setBuffer:vb offset:0 atIndex:0];
		[compute setBuffer:total offset:0 atIndex:1];
		[compute setBuffer:partial offset:0 atIndex:2];
		[compute dispatchThreadgroups:MTLSizeMake(M / 256, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
		[compute endEncoding];
		if (completed(commands, @"group_sum")) {
			check(*(uint32_t *)total.contents == expected, [NSString stringWithFormat:@"group_sum through the Metal API: %u (the CPU: %u)",
			    *(uint32_t *)total.contents, expected]);
		}
		free(values);

		/* render: the triangle into a texture, read back by a blit */
		const float corners[3][2] = { { -0.8f, -0.8f }, { 0.8f, -0.6f }, { 0.0f, 0.8f } };
		MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
		d.vertexFunction = [library newFunctionWithName:@"triangle_vertex"];
		d.fragmentFunction = [library newFunctionWithName:@"triangle_fragment"];
		d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
		id<MTLRenderPipelineState> rgba = [device newRenderPipelineStateWithDescriptor:d error:&error];
		check(rgba != nil, [NSString stringWithFormat:@"newRenderPipelineStateWithDescriptor: RGBA8 (%@)", error]);
		MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
		t.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		t.storageMode = MTLStorageModePrivate;
		id<MTLTexture> target = [device newTextureWithDescriptor:t];
		id<MTLBuffer> readback = [device newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
		if (rgba != nil && target != nil) {
			unsigned inside, wrong;
			drawTriangle(device, queue, rgba, target, readback, corners);
			wrong = judge(readback.contents, 64, 64, 64 * 4, 0, 2, corners, &inside);
			check(wrong == 0 && inside > 0, [NSString stringWithFormat:@"the triangle through the Metal API: %u pixels inside judged, %u wrong",
			    inside, wrong]);
		}

		/* an IOSurface's texture (what a CAMetalLayer's drawable is): the triangle rendered into the surface's own pages */
		IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
			(id)kIOSurfaceWidth: @128, (id)kIOSurfaceHeight: @96, (id)kIOSurfaceBytesPerElement: @4,
			(id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') });
		d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		id<MTLRenderPipelineState> bgra = [device newRenderPipelineStateWithDescriptor:d error:&error];
		MTLTextureDescriptor *s = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:128 height:96 mipmapped:NO];
		s.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		s.storageMode = MTLStorageModeShared;
		id<MTLTexture> onSurface = surface ? [device newTextureWithDescriptor:s iosurface:surface plane:0] : nil;
		check(onSurface != nil && onSurface.iosurface == surface, [NSString stringWithFormat:@"newTextureWithDescriptor:iosurface:plane: "
		    "a 128x96 BGRA8 texture over the surface (%zu bytes a row)", surface ? IOSurfaceGetBytesPerRow(surface) : 0]);
		if (bgra != nil && onSurface != nil) {
			unsigned inside, wrong;
			drawTriangle(device, queue, bgra, onSurface, nil, corners);
			IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
			wrong = judge(IOSurfaceGetBaseAddress(surface), 128, 96, IOSurfaceGetBytesPerRow(surface), 2, 0, corners, &inside);
			IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
			check(wrong == 0 && inside > 0, [NSString stringWithFormat:@"the triangle read from the IOSurface's own memory: %u pixels inside "
			    "judged, %u wrong", inside, wrong]);
		}
		onSurface = nil;
		if (surface != NULL) {
			CFRelease(surface);
		}

		/* a Shared texture's CPU copies, replaceRegion: and getBytes:, which go through staging buffers where the driver copies
		   nothing to or from an image on the host (gfxstream's) */
		MTLTextureDescriptor *c = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
		c.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		c.storageMode = MTLStorageModeShared;
		id<MTLTexture> sharedTexture = [device newTextureWithDescriptor:c];
		uint8_t *pattern = malloc(64 * 64 * 4), *got = calloc(1, 64 * 64 * 4);
		for (int i = 0; i < 64 * 64 * 4; i++) {
			pattern[i] = (uint8_t)(i * 7 + 3);
		}
		if (sharedTexture != nil && rgba != nil) {
			[sharedTexture replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:pattern bytesPerRow:64 * 4];
			id<MTLBuffer> copied = [device newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
			commands = [queue commandBuffer];
			id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
			[blit copyFromTexture:sharedTexture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
			    sourceSize:MTLSizeMake(64, 64, 1) toBuffer:copied destinationOffset:0 destinationBytesPerRow:64 * 4
			    destinationBytesPerImage:64 * 64 * 4];
			[blit endEncoding];
			if (completed(commands, @"a blit of the replaced texture")) {
				check(memcmp(copied.contents, pattern, 64 * 64 * 4) == 0,
				    @"replaceRegion: into a Shared texture, the GPU's copy of it out is the CPU's bytes");
			}
			unsigned inside, wrong;
			drawTriangle(device, queue, rgba, sharedTexture, nil, corners);
			[sharedTexture getBytes:got bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
			wrong = judge(got, 64, 64, 64 * 4, 0, 2, corners, &inside);
			check(wrong == 0 && inside > 0, [NSString stringWithFormat:@"getBytes: of the triangle rendered into a Shared texture: "
			    "%u pixels inside judged, %u wrong", inside, wrong]);
		}
		free(pattern);
		free(got);
		uint64_t copies[4];
		nmb_host_copies(copies);
		if (copies[0] || copies[1]) {
			printf("      no-copy buffers copied, the driver importing none: %llu bytes in (%.2f ms), %llu out (%.2f ms)\n",
			    (unsigned long long)copies[0], copies[2] / 1e6, (unsigned long long)copies[1], copies[3] / 1e6);
		}
		if (getenv("METALVK_BENCH") != NULL && bgra != nil) {
			bench(device, queue, bgra, corners);
		}
		printf("%s\n", failures ? "metalvk_plugin_test: FAILED" : "metalvk_plugin_test: the plug-in did everything asked, every number the CPU's");
	}
	return failures != 0;
}
