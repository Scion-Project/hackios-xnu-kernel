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

/* colour-buffer resources: a tight backing each, as the kernel keeps them */
struct res { uint32_t id, w, h; uint8_t *mem; };
static struct res resources[64];
static uint32_t next_res_id = 0x30000;

static struct res *
res_of(uint32_t id)
{
	for (int i = 0; i < 64; i++) {
		if (resources[i].id == id && id) {
			return &resources[i];
		}
	}
	return NULL;
}

static int
res_create(void *arg, uint32_t w, uint32_t h, uint32_t *out)
{
	struct pipe *p = arg;
	struct res *r = res_of(0);
	for (int i = 0; r == NULL && i < 64; i++) {
		if (resources[i].id == 0) {
			r = &resources[i];
		}
	}
	if (r == NULL) {
		return 28;
	}
	r->id = next_res_id++;
	r->w = w;
	r->h = h;
	r->mem = calloc(w * h, 4);
	struct sr_create c = { r->id, 2 /* TEXTURE_2D */, 1 /* B8G8R8A8 */, (1u << 1) | (1u << 3), w, h, 1, 1, 0, 0, 0 };
	struct sr_iovec iov = { r->mem, (size_t)w * h * 4 };
	if (sr_resource_create(&c, NULL, 0) || sr_attach_iov((int)r->id, &iov, 1)) {
		return 5;
	}
	sr_ctx_attach((int)p->ctx, (int)r->id);
	*out = r->id;
	return 0;
}

static int
res_unref(void *arg, uint32_t id)
{
	(void)arg;
	struct res *r = res_of(id);
	if (r) {
		free(r->mem);
		memset(r, 0, sizeof(*r));
	}
	return 0;
}

static int
res_xfer(struct pipe *p, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *user, uint32_t stride,
    int upload)
{
	struct res *r = res_of(id);
	if (r == NULL) {
		return 2;
	}
	size_t pitch = (size_t)r->w * 4;
	uint64_t off = (uint64_t)y * pitch + (uint64_t)x * 4;
	struct sr_box box = { x, y, 0, w, h, 1 };
	struct sr_iovec iov = { r->mem, (size_t)r->w * r->h * 4 };
	if (upload) {
		for (uint32_t i = 0; i < h; i++) {
			memcpy(r->mem + off + i * pitch, user + (size_t)i * stride, (size_t)w * 4);
		}
		return sr_write(r->id, p->ctx, 0, 0, 0, &box, off, &iov, 1) ? 5 : 0;
	}
	if (sr_read(r->id, p->ctx, 0, 0, 0, &box, off, &iov, 1)) {
		return 5;
	}
	for (uint32_t i = 0; i < h; i++) {
		memcpy(user + (size_t)i * stride, r->mem + off + i * pitch, (size_t)w * 4);
	}
	return 0;
}

static int
res_upload(void *arg, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *src, uint32_t stride)
{
	return res_xfer(arg, id, x, y, w, h, (uint8_t *)src, stride, 1);
}

static int
res_download(void *arg, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, void *dst, uint32_t stride)
{
	return res_xfer(arg, id, x, y, w, h, dst, stride, 0);
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
	t->res_create = res_create;
	t->res_unref = res_unref;
	t->res_upload = res_upload;
	t->res_download = res_download;
	return 0;
}
