/*
 * gfx's transport on iOS: /dev/vgpu3d, the kernel's virtio-gpu 3D layer (hackios-xnu-kernel
 * bsd/dev/redwood_vgpu3d_xnu.c). Every open is a virtio-gpu context with its own gfxstream pipe
 * to a render thread on the host; write and read carry the stream, the ioctls colour-buffer
 * resources.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "gfx.h"
#include "gfx_dev.h"

struct vgpu3d_ioc_create {
	uint32_t width, height, format, bind;
	uint32_t res;
};
struct vgpu3d_ioc_xfer {
	uint32_t res, x, y, w, h;
	uint32_t stride;
	uint64_t addr;
};
#define VGPU3D_IOC_CREATE       _IOWR('G', 1, struct vgpu3d_ioc_create)
#define VGPU3D_IOC_UNREF        _IOW('G', 2, uint32_t)
#define VGPU3D_IOC_UPLOAD       _IOW('G', 3, struct vgpu3d_ioc_xfer)
#define VGPU3D_IOC_DOWNLOAD     _IOW('G', 4, struct vgpu3d_ioc_xfer)

static int
dev_write(void *arg, const void *buf, size_t n)
{
	int fd = (int)(intptr_t)arg;
	const char *p = buf;
	while (n) {
		ssize_t k = write(fd, p, n);
		if (k <= 0) {
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

static int
dev_res_create(void *arg, uint32_t width, uint32_t height, uint32_t *res)
{
	struct vgpu3d_ioc_create c = { width, height, 0, 0, 0 };
	if (ioctl((int)(intptr_t)arg, VGPU3D_IOC_CREATE, &c) != 0) {
		return errno;
	}
	*res = c.res;
	return 0;
}

static int
dev_res_unref(void *arg, uint32_t res)
{
	return ioctl((int)(intptr_t)arg, VGPU3D_IOC_UNREF, &res) ? errno : 0;
}

static int
dev_res_upload(void *arg, uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *src,
    uint32_t stride)
{
	struct vgpu3d_ioc_xfer t = { res, x, y, w, h, stride, (uint64_t)(uintptr_t)src };
	return ioctl((int)(intptr_t)arg, VGPU3D_IOC_UPLOAD, &t) ? errno : 0;
}

static int
dev_res_download(void *arg, uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h, void *dst,
    uint32_t stride)
{
	struct vgpu3d_ioc_xfer t = { res, x, y, w, h, stride, (uint64_t)(uintptr_t)dst };
	return ioctl((int)(intptr_t)arg, VGPU3D_IOC_DOWNLOAD, &t) ? errno : 0;
}

int
gfx_dev_open(struct gfx_transport *t)
{
	int fd = open("/dev/vgpu3d", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		return errno;
	}
	t->arg = (void *)(intptr_t)fd;
	t->write = dev_write;
	t->read = dev_read;
	t->close = dev_close;
	t->res_create = dev_res_create;
	t->res_unref = dev_res_unref;
	t->res_upload = dev_res_upload;
	t->res_download = dev_res_download;
	t->flags_sent = 1;              /* the kernel's vgpu3d_open sends it */
	return 0;
}
