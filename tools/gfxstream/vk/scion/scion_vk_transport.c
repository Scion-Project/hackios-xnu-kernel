/*
 * scion_vk_transport.h, on iOS (/dev/vgpu3d) and on the Mac (the emulator's gfxstream in this
 * process, for testing; tools/gfxstream/mk-vk.sh mac).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <TargetConditionals.h>

#include "scion_vk_transport.h"

#if TARGET_OS_IPHONE

/* ---- iOS: /dev/vgpu3d (drivers/gpu/hakios.vgpu3d_xnu.c), one virtio-gpu context and pipe an open */

static int
dev_write(void *arg, const void *buf, size_t n)
{
	int fd = (int)(intptr_t)arg;
	const char *p = buf;
	while (n) {
		ssize_t k = write(fd, p, n);
		if (k <= 0) {
			if (k < 0 && errno == EINTR) {
				continue;
			}
			return k < 0 ? errno : EIO;
		}
		p += k;
		n -= (size_t)k;
	}
	return 0;
}

static int
dev_read(void *arg, void *buf, size_t n)
{
	int fd = (int)(intptr_t)arg;
	char *p = buf;
	while (n) {
		ssize_t k = read(fd, p, n);
		if (k <= 0) {
			if (k < 0 && errno == EINTR) {
				continue;
			}
			return k < 0 ? errno : EIO;
		}
		p += k;
		n -= (size_t)k;
	}
	return 0;
}

static void
dev_close(void *arg)
{
	close((int)(intptr_t)arg);
}

int
scion_vk_transport_open(struct scion_vk_transport *t)
{
	int fd = open("/dev/vgpu3d", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		return errno;
	}
	t->arg = (void *)(intptr_t)fd;
	t->write = dev_write;
	t->read = dev_read;
	t->close = dev_close;
	t->flags_sent = 1;              /* vgpu3d_open sends "pipe:opengles" and the flags word */
	return 0;
}

int
scion_vk_transport_capset(uint32_t id, void *buf, size_t size)
{
	(void)id;
	(void)buf;
	(void)size;
	return ENOTSUP;                 /* the kernel reads the capsets but has no ioctl for them yet */
}

#else

/*
 * ---- macOS: the Android emulator's libgfxstream_backend.dylib (Android SDK, emulator/lib64),
 * driven through stream_renderer_* as crosvm drives it for a virtio-gpu guest: a context and a
 * PIPE_BUFFER resource (bind CUSTOM) with a 1 MiB iovec a stream, "pipe:opengles" written first,
 * then transfers of the byte stream. tools/gfxstream/host_test/host.c does the same for GL.
 *
 *   GFXSTREAM_LIB       the directory of libgfxstream_backend.dylib
 *   SCION_VK_FLAGS      the renderer flags (default EGL | surfaceless | GLES | VK: 0x39)
 *   SCION_VK_FEATURES   the renderer features (default "VirtioGpuNext:disabled": host memory is
 *                       not mapped into the guest; flush and invalidate carry the bytes)
 */
#include <dlfcn.h>
#include <pthread.h>

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
#define PARAM_RENDERER_FEATURES 11

static int (*sr_init)(struct sr_param *, uint64_t);
static int (*sr_context_create)(uint32_t, uint32_t, const char *, uint32_t);
static void (*sr_context_destroy)(uint32_t);
static int (*sr_resource_create)(struct sr_create *, struct sr_iovec *, uint32_t);
static void (*sr_resource_unref)(uint32_t);
static int (*sr_attach_iov)(int, struct sr_iovec *, int);
static void (*sr_ctx_attach)(int, int);
static int (*sr_write)(uint32_t, uint32_t, int, uint32_t, uint32_t, struct sr_box *, uint64_t, struct sr_iovec *, unsigned);
static int (*sr_read)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, struct sr_box *, uint64_t, struct sr_iovec *, int);
static void (*sr_get_cap_set)(uint32_t, uint32_t *, uint32_t *);
static void (*sr_fill_caps)(uint32_t, uint32_t, void *);

#define PIPE_SIZE (1u << 20)

struct pipe { uint32_t ctx, res; uint8_t *buf; };

static pthread_once_t host_once = PTHREAD_ONCE_INIT;
static int host_err = -1;
static pthread_mutex_t ids_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_ctx = 1, next_res = 0x10000;

static void fence_cb(void *cookie, struct sr_fence *f) { (void)cookie; (void)f; }

static void
host_init(void)
{
	const char *dir = getenv("GFXSTREAM_LIB"), *home = getenv("HOME");
	char path[1024];
	if (dir) {
		snprintf(path, sizeof(path), "%s/libgfxstream_backend.dylib", dir);
	} else {
		snprintf(path, sizeof(path), "%s/Library/Android/sdk/emulator/lib64/libgfxstream_backend.dylib",
		    home ? home : "");
	}
	void *h = dlopen(path, RTLD_NOW);
	if (h == NULL) {
		fprintf(stderr, "scion-vk: %s\n", dlerror());
		host_err = ENOENT;
		return;
	}
	sr_init = dlsym(h, "stream_renderer_init");
	sr_context_create = dlsym(h, "stream_renderer_context_create");
	sr_context_destroy = dlsym(h, "stream_renderer_context_destroy");
	sr_resource_create = dlsym(h, "stream_renderer_resource_create");
	sr_resource_unref = dlsym(h, "stream_renderer_resource_unref");
	sr_attach_iov = dlsym(h, "stream_renderer_resource_attach_iov");
	sr_ctx_attach = dlsym(h, "stream_renderer_ctx_attach_resource");
	sr_write = dlsym(h, "stream_renderer_transfer_write_iov");
	sr_read = dlsym(h, "stream_renderer_transfer_read_iov");
	sr_get_cap_set = dlsym(h, "stream_renderer_get_cap_set");
	sr_fill_caps = dlsym(h, "stream_renderer_fill_caps");
	const char *f = getenv("SCION_VK_FLAGS");
	const char *features = getenv("SCION_VK_FEATURES");
	uint32_t flags = f ? (uint32_t)strtoul(f, NULL, 0) : (1u << 0) | (1u << 3) | (1u << 4) | (1u << 5);
	if (features == NULL) {
		features = "VirtioGpuNext:disabled";
	}
	struct sr_param params[] = {
		{ PARAM_USER_DATA, 0 },
		{ PARAM_RENDERER_FLAGS, flags },
		{ PARAM_FENCE_CALLBACK, (uint64_t)(uintptr_t)fence_cb },
		{ PARAM_WIN0_WIDTH, 64 },
		{ PARAM_WIN0_HEIGHT, 64 },
		{ PARAM_RENDERER_FEATURES, (uint64_t)(uintptr_t)features },
	};
	int r = sr_init(params, *features ? 6 : 5);
	if (r) {
		fprintf(stderr, "scion-vk: stream_renderer_init (flags 0x%x, features \"%s\"): %d\n", flags, features, r);
		host_err = EIO;
		return;
	}
	host_err = 0;
}

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
			return EIO;
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
			return EIO;
		}
		memcpy(d, p->buf, k);
		d += k;
		n -= k;
	}
	return 0;
}

static void
pipe_close(void *arg)
{
	struct pipe *p = arg;
	sr_resource_unref(p->res);
	sr_context_destroy(p->ctx);
	free(p->buf);
	free(p);
}

int
scion_vk_transport_open(struct scion_vk_transport *t)
{
	pthread_once(&host_once, host_init);
	if (host_err) {
		return host_err;
	}
	struct pipe *p = calloc(1, sizeof(*p));
	if (p == NULL || (p->buf = calloc(1, PIPE_SIZE)) == NULL) {
		free(p);
		return ENOMEM;
	}
	pthread_mutex_lock(&ids_lock);
	p->ctx = next_ctx++;
	p->res = next_res++;
	pthread_mutex_unlock(&ids_lock);
	static const char name[] = "scion-vk";
	if (sr_context_create(p->ctx, sizeof(name) - 1, name, 3 /* gfxstream-vulkan */)) {
		free(p->buf);
		free(p);
		return EIO;
	}
	struct sr_create c = { p->res, 0 /* PIPE_BUFFER */, 64 /* R8 */, 1u << 17 /* CUSTOM */, PIPE_SIZE, 1, 1, 1, 0, 0, 0 };
	struct sr_iovec iov = { p->buf, PIPE_SIZE };
	if (sr_resource_create(&c, NULL, 0) || sr_attach_iov((int)p->res, &iov, 1)) {
		sr_context_destroy(p->ctx);
		free(p->buf);
		free(p);
		return EIO;
	}
	sr_ctx_attach((int)p->ctx, (int)p->res);
	static const char service[] = "pipe:opengles";
	memcpy(p->buf, service, sizeof(service));
	if (xfer(p, 1, sizeof(service))) {
		pipe_close(p);
		return EIO;
	}
	t->arg = p;
	t->write = pipe_write;
	t->read = pipe_read;
	t->close = pipe_close;
	t->flags_sent = 0;
	return 0;
}

int
scion_vk_transport_capset(uint32_t id, void *buf, size_t size)
{
	pthread_once(&host_once, host_init);
	if (host_err || sr_get_cap_set == NULL || sr_fill_caps == NULL) {
		return ENOTSUP;
	}
	uint32_t version = 0, max = 0;
	sr_get_cap_set(id, &version, &max);
	if (max == 0) {
		return ENOTSUP;
	}
	uint8_t *caps = calloc(1, max);
	if (caps == NULL) {
		return ENOMEM;
	}
	sr_fill_caps(id, version, caps);
	memset(buf, 0, size);
	memcpy(buf, caps, max < size ? max : size);
	free(caps);
	return 0;
}

#endif
