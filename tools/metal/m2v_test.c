/*
 * SPDX-License-Identifier: MIT
 * m2v_test: metal2vulkan's runtime on a Vulkan driver of this Mac, through the C ABI the Metal plug-in uses (libmetal2vulkan.dylib,
 * tools/metal/bridge/NativeMetalObjects.h). Built and run by tools/metal/mk-metal2vulkan.sh mac.
 *
 *   m2v_test ICD CACHE METALLIB [PPM]
 *
 * ICD is the Vulkan driver by path (lavapipe: /opt/homebrew/lib/libvulkan_lvp.dylib; on the guest /redwood/lib/libhakios.vk.dylib),
 * CACHE a directory for the translated shaders (made if missing), METALLIB tools/metal/m2v_test.metal as Xcode's Metal compiler made it.
 * The same program is built for the guest (tools/metal/mk-metal2vulkan.sh ios; userland/rc.jobs.metalvk runs it).
 * What it runs, each checked against the CPU:
 *
 *   1. the bridge's own self-test (nmb_selftest): 1000 floats through a kernel, a quad drawn into an 8x8 target and read back, the
 *      same quad into 64x64 host pages imported without a copy (what an IOSurface's texture is);
 *   2. three compute kernels: a*k+b over 4096 floats; Collatz step counts of 1..4096 (loops, branches); a sum of 65536 numbers in
 *      groups of 256 (threadgroup memory, barriers, a device atomic);
 *   3. a triangle (red, green, blue corners) into a 64x64 RGBA8 render target, copied to a buffer by a blit and compared, pixel by
 *      pixel, with the coverage and colour the corners give (pixels within a pixel of an edge are not judged). PPM, if given, gets
 *      the picture.
 *
 * Exit status 0 when everything ran and every number is the CPU's.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "NativeMetalBridge.h"
#include "NativeMetalObjects.h"

#define SHARED 0ull                     /* MTLResourceStorageModeShared */
#define PRIVATE (2ull << 4)             /* MTLResourceStorageModePrivate */

static int failures;

static void
check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL  %s: %s\n", what, nmb_last_error());
		failures++;
	}
}

static NmbComputePipeline *
kernel(NmbDevice *device, NmbLibrary *library, const char *name)
{
	NmbFunction *function = nmb_function_new(library, name);
	NmbComputePipeline *pipeline;

	if (function == NULL) {
		printf("FAIL  function %s: %s\n", name, nmb_last_error());
		failures++;
		return NULL;
	}
	pipeline = nmb_compute_pipeline_new(device, function);
	if (pipeline == NULL) {
		printf("FAIL  compute pipeline %s: %s\n", name, nmb_last_error());
		failures++;
	}
	nmb_function_release(function);
	return pipeline;
}

/* commit, wait (10 s at most), and say whether it completed */
static int
run(NmbCommandBuffer *commands, const char *what)
{
	char error[256] = "";

	if (!nmb_command_buffer_commit(commands)) {
		printf("FAIL  %s: commit: %s\n", what, nmb_last_error());
		failures++;
		return 0;
	}
	if (nmb_command_buffer_wait(commands, 10000) != 0) {
		printf("FAIL  %s: no completion in 10 s\n", what);
		failures++;
		return 0;
	}
	if (nmb_command_buffer_status(commands) != 4) {
		nmb_command_buffer_error(commands, error, sizeof(error));
		printf("FAIL  %s: status %llu %s\n", what, (unsigned long long)nmb_command_buffer_status(commands), error);
		failures++;
		return 0;
	}
	return 1;
}

static void
dispatch(NmbQueue *queue, NmbComputePipeline *pipeline, NmbBuffer **buffers, unsigned count, const void *bytes, size_t length,
         unsigned bytesIndex, uint64_t threads, uint64_t group, const char *what)
{
	NmbCommandBuffer *commands = nmb_command_buffer_new(queue);
	NmbEncoder *encoder = commands ? nmb_compute_encoder_new(commands) : NULL;
	const uint64_t size[3] = { threads, 1, 1 }, per[3] = { group, 1, 1 };

	if (encoder == NULL) {
		check(0, what);
		return;
	}
	check(nmb_compute_set_pipeline(encoder, pipeline), what);
	for (unsigned i = 0; i < count; i++) {
		check(nmb_encoder_set_buffer(encoder, 0, buffers[i], 0, i), what);
	}
	if (bytes != NULL) {
		check(nmb_encoder_set_bytes(encoder, 0, bytes, length, bytesIndex), what);
	}
	check(nmb_compute_dispatch(encoder, size, per, true), what);
	check(nmb_encoder_end(encoder), what);
	run(commands, what);
	nmb_command_buffer_release(commands);
}

static void
compute(NmbDevice *device, NmbLibrary *library, NmbQueue *queue)
{
	enum { N = 4096, M = 65536 };
	NmbComputePipeline *axpb = kernel(device, library, "axpb"), *collatz = kernel(device, library, "collatz"),
	    *sum = kernel(device, library, "group_sum");
	float a[N], b[N], k = 3.0f;
	static uint32_t values[M];
	uint64_t expected = 0;
	unsigned wrong = 0;

	if (axpb != NULL) {
		for (unsigned i = 0; i < N; i++) {
			a[i] = (float)(i % 1000);
			b[i] = (float)(N - i);
		}
		NmbBuffer *buffers[3] = { nmb_buffer_new(device, sizeof(a), SHARED, a), nmb_buffer_new(device, sizeof(b), SHARED, b),
		    nmb_buffer_new(device, sizeof(a), SHARED, NULL) };
		dispatch(queue, axpb, buffers, 3, &k, sizeof(k), 3, N, 64, "axpb");
		const float *out = nmb_buffer_contents(buffers[2]);
		for (unsigned i = 0; i < N; i++) {
			wrong += out[i] != a[i] * k + b[i];
		}
		printf("%s  axpb: %u floats, %u wrong (out[1234] = %.1f, the CPU says %.1f)\n", wrong ? "FAIL" : "ok  ", N, wrong,
		    out[1234], a[1234] * k + b[1234]);
		failures += wrong != 0;
		for (unsigned i = 0; i < 3; i++) {
			nmb_buffer_release(buffers[i]);
		}
		nmb_compute_pipeline_release(axpb);
	}
	if (collatz != NULL) {
		NmbBuffer *steps = nmb_buffer_new(device, N * 4, SHARED, NULL);
		unsigned longest = 0, at = 0;

		dispatch(queue, collatz, &steps, 1, NULL, 0, 0, N, 64, "collatz");
		const uint32_t *out = nmb_buffer_contents(steps);
		wrong = 0;
		for (unsigned i = 0; i < N; i++) {
			uint32_t n = i + 1, s = 0;
			while (n != 1 && s < 1000) {
				n = (n & 1) ? 3 * n + 1 : n >> 1;
				s++;
			}
			wrong += out[i] != s;
			if (out[i] > longest) {
				longest = out[i];
				at = i + 1;
			}
		}
		printf("%s  collatz: %u numbers, %u wrong (the longest: %u steps from %u)\n", wrong ? "FAIL" : "ok  ", N, wrong, longest, at);
		failures += wrong != 0;
		nmb_buffer_release(steps);
		nmb_compute_pipeline_release(collatz);
	}
	if (sum != NULL) {
		for (unsigned i = 0; i < M; i++) {
			values[i] = (i * 2654435761u) >> 20;
			expected += values[i];
		}
		NmbBuffer *buffers[3] = { nmb_buffer_new(device, sizeof(values), SHARED, values), nmb_buffer_new(device, 4, SHARED, NULL),
		    nmb_buffer_new(device, (M / 256) * 4, SHARED, NULL) };
		dispatch(queue, sum, buffers, 3, NULL, 0, 0, M, 256, "group_sum");
		const uint32_t *total = nmb_buffer_contents(buffers[1]), *partial = nmb_buffer_contents(buffers[2]);
		wrong = 0;
		for (unsigned g = 0; g < M / 256; g++) {
			uint32_t s = 0;
			for (unsigned i = 0; i < 256; i++) {
				s += values[g * 256 + i];
			}
			wrong += partial[g] != s;
		}
		printf("%s  group_sum: %u numbers in %u groups, total %u (the CPU: %u), %u group sums wrong\n",
		    wrong || *total != (uint32_t)expected ? "FAIL" : "ok  ", M, M / 256, *total, (uint32_t)expected, wrong);
		failures += wrong != 0 || *total != (uint32_t)expected;
		for (unsigned i = 0; i < 3; i++) {
			nmb_buffer_release(buffers[i]);
		}
		nmb_compute_pipeline_release(sum);
	}
}

/* twice the signed area of (a, b, p): which side of the edge a-b the point p is on */
static double
edge(const double a[2], const double b[2], const double p[2])
{
	return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]);
}

static void
triangle(NmbDevice *device, NmbLibrary *library, NmbQueue *queue, const char *ppm)
{
	enum { W = 64, H = 64, ROW = W * 4 };
	const float corners[3][2] = { { -0.8f, -0.8f }, { 0.8f, -0.6f }, { 0.0f, 0.8f } };
	NmbFunction *vertex = nmb_function_new(library, "triangle_vertex"), *fragment = nmb_function_new(library, "triangle_fragment");
	struct NmbRenderPipelineDescriptor p;
	struct NmbTextureDescriptor t = { 2, 70, W, H, 1, 1, 1, 1, 4 | 1, PRIVATE };   /* 2D, RGBA8Unorm, render target and read */
	struct NmbRenderPassDescriptor pass;
	unsigned inside = 0, outside = 0, wrong = 0, edges = 0;

	if (vertex == NULL || fragment == NULL) {
		check(0, "triangle functions");
		return;
	}
	memset(&p, 0, sizeof(p));
	p.vertex = vertex;
	p.fragment = fragment;
	p.colors[0] = (struct NmbColorAttachment){ .format = 70, .write_mask = 0xf, .source_rgb = 1, .source_alpha = 1 };
	p.samples = 1;
	p.topology = 3;                 /* MTLPrimitiveTopologyClassTriangle */
	p.rasterization = 1;
	NmbRenderPipeline *pipeline = nmb_render_pipeline_new(device, &p);
	NmbTexture *target = nmb_texture_new(device, &t);
	NmbBuffer *readback = nmb_buffer_new(device, ROW * H, SHARED, NULL);
	NmbCommandBuffer *commands = nmb_command_buffer_new(queue);

	check(pipeline != NULL, "render pipeline");
	check(target != NULL, "render target");
	if (pipeline == NULL || target == NULL || readback == NULL || commands == NULL) {
		return;
	}
	memset(&pass, 0, sizeof(pass));
	pass.colors[0] = (struct NmbPassAttachment){ .texture = target, .load = 2, .store = 1, .clear = { 0, 0, 0, 1 } };
	pass.width = W;
	pass.height = H;
	pass.array_length = 1;
	pass.samples = 1;
	NmbEncoder *render = nmb_render_encoder_new(commands, &pass);
	check(render != NULL, "render encoder");
	if (render == NULL) {
		return;
	}
	check(nmb_render_set_pipeline(render, pipeline), "set render pipeline");
	check(nmb_encoder_set_bytes(render, 1, corners, sizeof(corners), 0), "set corners");
	check(nmb_render_draw(render, 3, 0, 3, 1, 0), "draw");
	check(nmb_encoder_end(render), "end render");
	NmbEncoder *blit = nmb_blit_encoder_new(commands);
	const uint64_t origin[3] = { 0, 0, 0 }, size[3] = { W, H, 1 };
	check(blit != NULL && nmb_blit_texture_to_buffer(blit, target, 0, 0, origin, size, readback, 0, ROW, ROW * H), "blit");
	check(blit != NULL && nmb_encoder_end(blit), "end blit");
	if (!run(commands, "triangle")) {
		return;
	}

	/* the prediction: Metal's normalised coordinates, y up, onto pixels whose rows go down; colour by barycentric weights */
	const uint8_t *pixels = nmb_buffer_contents(readback);
	double v[3][2];
	for (int i = 0; i < 3; i++) {
		v[i][0] = (corners[i][0] + 1.0) * 0.5 * W;
		v[i][1] = (1.0 - corners[i][1]) * 0.5 * H;
	}
	const double area = edge(v[0], v[1], v[2]);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			const double c[2] = { x + 0.5, y + 0.5 };
			double w[3] = { edge(v[1], v[2], c) / area, edge(v[2], v[0], c) / area, edge(v[0], v[1], c) / area };
			const uint8_t *px = pixels + y * ROW + x * 4;
			double nearest = 1e9;

			for (int i = 0; i < 3; i++) {
				const double *a = v[(i + 1) % 3], *b = v[(i + 2) % 3];
				nearest = fmin(nearest, fabs(edge(a, b, c)) / hypot(b[0] - a[0], b[1] - a[1]));
			}
			if (nearest < 1.0) {
				edges++;
				continue;
			}
			if (w[0] >= 0 && w[1] >= 0 && w[2] >= 0) {
				inside++;
				for (int i = 0; i < 3; i++) {
					if (fabs(px[i] - w[i] * 255.0) > 2.5) {
						wrong++;
						if (wrong <= 3) {
							printf("      pixel %d,%d is %u,%u,%u; the corners give %.0f,%.0f,%.0f\n", x, y, px[0], px[1], px[2],
							    w[0] * 255, w[1] * 255, w[2] * 255);
						}
						break;
					}
				}
			} else {
				outside++;
				if (px[0] || px[1] || px[2] || px[3] != 255) {
					wrong++;
					if (wrong <= 3) {
						printf("      pixel %d,%d outside the triangle is %u,%u,%u,%u\n", x, y, px[0], px[1], px[2], px[3]);
					}
				}
			}
		}
	}
	printf("%s  triangle: 64x64 RGBA8, %u pixels inside and %u outside judged (%u on edges not), %u wrong\n",
	    wrong || inside == 0 ? "FAIL" : "ok  ", inside, outside, edges, wrong);
	failures += wrong != 0 || inside == 0;
	if (ppm != NULL) {
		FILE *f = fopen(ppm, "wb");

		if (f != NULL) {
			fprintf(f, "P6 %d %d 255\n", W, H);
			for (int i = 0; i < W * H; i++) {
				fwrite(pixels + i * 4, 1, 3, f);
			}
			fclose(f);
		}
	}
	nmb_buffer_release(readback);
	nmb_texture_release(target);
	nmb_render_pipeline_release(pipeline);
	nmb_command_buffer_release(commands);
	nmb_function_release(vertex);
	nmb_function_release(fragment);
}

int
main(int argc, char **argv)
{
	struct NmbSelftest self;
	char name[256] = "", icd[512] = "";
	FILE *f;
	long length;
	void *bytes;

	if (argc < 4) {
		fprintf(stderr, "usage: %s ICD CACHE METALLIB [PPM]\n", argv[0]);
		return 2;
	}
	mkdir(argv[2], 0700);           /* the shader cache: the runtime wants it to exist */
	/* 1. the bridge's self-test, on its own device */
	uint32_t stage = nmb_selftest(argv[1], argv[2], NULL, 0, NULL, &self);
	if (stage != 0) {
		printf("FAIL  self-test: step 0x%x: %s\n", stage, self.message);
		failures++;
	} else {
		printf("%s  self-test: compute %u of 1000 wrong, render %u of 64 wrong, import %u of 4096 wrong\n",
		    self.compute_bad || self.render_bad || self.import_bad ? "FAIL" : "ok  ", self.compute_bad, self.render_bad, self.import_bad);
		failures += self.compute_bad || self.render_bad || self.import_bad;
	}

	/* 2 and 3, on the device the plug-in would make */
	NmbDevice *device = nmb_device_open(argv[1], argv[2]);
	if (device == NULL) {
		printf("FAIL  device: %s\n", nmb_last_error());
		return 1;
	}
	nmb_device_name(device, name, sizeof(name));
	nmb_icd_describe(icd, sizeof(icd));
	printf("      device: %s; driver %s; runtime %s\n", name, icd, nmb_runtime_build());
	if ((f = fopen(argv[3], "rb")) == NULL || fseek(f, 0, SEEK_END) != 0 || (length = ftell(f)) <= 0) {
		fprintf(stderr, "%s: cannot read\n", argv[3]);
		return 2;
	}
	bytes = malloc((size_t)length);
	rewind(f);
	if (fread(bytes, 1, (size_t)length, f) != (size_t)length) {
		return 2;
	}
	fclose(f);
	NmbLibrary *library = nmb_library_new(device, bytes, (size_t)length, false);
	if (library == NULL) {
		printf("FAIL  library %s: %s\n", argv[3], nmb_last_error());
		return 1;
	}
	printf("      library: %u functions from %s (%ld bytes)\n", nmb_library_function_count(library), argv[3], length);
	NmbQueue *queue = nmb_queue_new(device);
	compute(device, library, queue);
	triangle(device, library, queue, argc > 4 ? argv[4] : NULL);
	nmb_queue_release(queue);
	nmb_library_release(library);
	nmb_device_release(device);
	free(bytes);
	printf("%s\n", failures ? "m2v_test: FAILED" : "m2v_test: everything ran and every number is the CPU's");
	return failures != 0;
}
