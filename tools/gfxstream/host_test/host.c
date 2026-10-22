/*
 * A gfxstream host on the Mac, for testing src/gfx without a VM: the Android emulator's
 * libgfxstream_backend.dylib (Android SDK, emulator/lib64) driven through its stream_renderer_*
 * API exactly as crosvm drives it for a virtio-gpu guest. One context, one PIPE_BUFFER resource
 * with a 1 MiB iovec, "pipe:opengles", then transfers: the byte stream of a guest.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

struct sr_iovec { void *base; size_t len; };
struct sr_box { uint32_t x, y, z, w, h, d; };
struct sr_create { uint32_t handle, target, format, bind, width, height, depth, array_size, last_level,
	nr_samples, flags; };
struct sr_param { uint64_t key, value; };
struct sr_fence { uint32_t flags; uint64_t fence_id; uint32_t ctx_id; uint8_t ring_idx; };

#define PARAM_USER_DATA         1
#define PARAM_RENDERER_FLAGS    2
#define PARAM_FENCE_CALLBACK    3
#define PARAM_WIN0_WIDTH        4
#define PARAM_WIN0_HEIGHT       5

static int (*sr_init)(struct sr_param *, uint64_t);
static int (*sr_context_create)(uint32_t, uint32_t, const char *, uint32_t);
static int (*sr_resource_create)(struct sr_create *, struct sr_iovec *, uint32_t);
static int (*sr_attach_iov)(int, struct sr_iovec *, int);
static void (*sr_ctx_attach)(int, int);
static int (*sr_write)(uint32_t, uint32_t, int, uint32_t, uint32_t, struct sr_box *, uint64_t, struct sr_iovec *, unsigned);
static int (*sr_read)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, struct sr_box *, uint64_t, struct sr_iovec *, int);

#define PIPE_SIZE (1u << 20)

struct pipe { uint32_t ctx, res; uint8_t *buf; };

static void fence_cb(void *cookie, struct sr_fence *f) { (void)cookie; (void)f; }

static int
xfer(struct pipe *p, int to_host, uint32_t n)
{
	struct sr_box box = { 0, 0, 0, n, 1, 1 };
	struct sr_iovec iov = { p->buf, PIPE_SIZE };
	return to_host ? sr_write(p->res, p->ctx, 0, 0, 0, &box, 0, &iov, 1)
	               : sr_read(p->res, p->ctx, 0, 0, 0, &box, 0, &iov, 1);
}

static int
pipe_write(void *arg, const void *buf, size_t n)
{
	struct pipe *p = arg;
	const uint8_t *s = buf;
	while (n) {
		uint32_t k = n > PIPE_SIZE ? PIPE_SIZE : (uint32_t)n;
		memcpy(p->buf, s, k);
		if (xfer(p, 1, k)) {
			return 5;
		}
		s += k;
		n -= k;
	}
	return 0;
}

static int
pipe_read(void *arg, void *buf, size_t n)
{
	struct pipe *p = arg;
	uint8_t *d = buf;
	while (n) {
		uint32_t k = n > PIPE_SIZE ? PIPE_SIZE : (uint32_t)n;
		if (xfer(p, 0, k)) {
			return 5;
		}
		memcpy(d, p->buf, k);
		d += k;
		n -= k;
	}
	return 0;
}

int
host_init(uint32_t flags)
{
	const char *dir = getenv("GFXSTREAM_LIB");
	char path[1024];
	snprintf(path, sizeof(path), "%s/libgfxstream_backend.dylib",
	    dir ? dir : "/Users/dylan/Library/Android/sdk/emulator/lib64");
	void *h = dlopen(path, RTLD_NOW);
	if (h == NULL) {
		fprintf(stderr, "host: %s\n", dlerror());
		return -1;
	}
	sr_init = dlsym(h, "stream_renderer_init");
	sr_context_create = dlsym(h, "stream_renderer_context_create");
	sr_resource_create = dlsym(h, "stream_renderer_resource_create");
	sr_attach_iov = dlsym(h, "stream_renderer_resource_attach_iov");
	sr_ctx_attach = dlsym(h, "stream_renderer_ctx_attach_resource");
	sr_write = dlsym(h, "stream_renderer_transfer_write_iov");
	sr_read = dlsym(h, "stream_renderer_transfer_read_iov");
	struct sr_param params[] = {
		{ PARAM_USER_DATA, 0 },
		{ PARAM_RENDERER_FLAGS, flags },
		{ PARAM_FENCE_CALLBACK, (uint64_t)(uintptr_t)fence_cb },
		{ PARAM_WIN0_WIDTH, 64 },
		{ PARAM_WIN0_HEIGHT, 64 },
	};
	int r = sr_init(params, 5);
	if (r) {
		fprintf(stderr, "host: stream_renderer_init: %d\n", r);
	}
	return r;
}

int
host_open(struct gfx_transport *t)
{
	static uint32_t next_ctx = 1, next_res = 0x10000;
	struct pipe *p = calloc(1, sizeof(*p));
	p->ctx = next_ctx++;
	p->res = next_res++;
	p->buf = calloc(1, PIPE_SIZE);
	if (sr_context_create(p->ctx, 4, "test", 8 /* gfxstream-gles */)) {
		return -1;
	}
	struct sr_create c = { p->res, 0 /* PIPE_BUFFER */, 64 /* R8 */, 1u << 17 /* CUSTOM */, PIPE_SIZE, 1, 1, 1, 0, 0, 0 };
	struct sr_iovec iov = { p->buf, PIPE_SIZE };
	if (sr_resource_create(&c, NULL, 0) || sr_attach_iov((int)p->res, &iov, 1)) {
		return -1;
	}
	sr_ctx_attach((int)p->ctx, (int)p->res);
	static const char service[] = "pipe:opengles";
	memcpy(p->buf, service, sizeof(service));
	if (xfer(p, 1, sizeof(service))) {
		return -1;
	}
	t->arg = p;
	t->write = pipe_write;
	t->read = pipe_read;
	t->close = NULL;
	return 0;
}
