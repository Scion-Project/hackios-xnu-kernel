/*
 * virtio-gpu 2D: a scanout for the display (sys/hakios.vgpu.h), on the virtio core
 * (sys/hakios.virtio.h). Without the core this is only stubs that say ENODEV.
 *
 * One resource (HAKIOS_VGPU_2D_RESOURCE) in B8G8R8X8, backed by one physically contiguous
 * buffer the kernel draws into, on scanout 0 at the size the device prefers. A frame reaches
 * the host with TRANSFER_TO_HOST_2D (the host copies the changed rectangle from guest memory
 * into its resource) and RESOURCE_FLUSH (it shows it). Commands go one at a time on the
 * control queue under a mutex, each waited for: a frame is two of them.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/hakios.vgpu.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>

#if __has_include(<sys/hakios.virtio.h>)
#include <sys/hakios.virtio.h>

/* virtio 1.2, 5.7 */
#define CMD_GET_DISPLAY_INFO            0x0100
#define CMD_RESOURCE_CREATE_2D          0x0101
#define CMD_SET_SCANOUT                 0x0103
#define CMD_RESOURCE_FLUSH              0x0104
#define CMD_TRANSFER_TO_HOST_2D         0x0105
#define CMD_RESOURCE_ATTACH_BACKING     0x0106
#define RESP_OK_NODATA                  0x1100
#define RESP_OK_DISPLAY_INFO            0x1101
#define FORMAT_B8G8R8X8_UNORM           2
#define MAX_SCANOUTS                    16

struct ctrl_hdr {
	uint32_t type, flags;
	uint64_t fence_id;
	uint32_t ctx_id;
	uint8_t ring_idx, padding[3];
};
struct rect {
	uint32_t x, y, width, height;
};
struct resp_display_info {
	struct ctrl_hdr hdr;
	struct {
		struct rect r;
		uint32_t enabled, flags;
	} pmodes[MAX_SCANOUTS];
};
struct resource_create_2d {
	struct ctrl_hdr hdr;
	uint32_t resource_id, format, width, height;
};
struct attach_backing {
	struct ctrl_hdr hdr;
	uint32_t resource_id, nr_entries;
	struct {
		uint64_t addr;
		uint32_t length, padding;
	} entry;                                /* one: the buffer is contiguous */
};
struct set_scanout {
	struct ctrl_hdr hdr;
	struct rect r;
	uint32_t scanout_id, resource_id;
};
struct transfer_to_host_2d {
	struct ctrl_hdr hdr;
	struct rect r;
	uint64_t offset;
	uint32_t resource_id, padding;
};
struct resource_flush {
	struct ctrl_hdr hdr;
	struct rect r;
	uint32_t resource_id, padding;
};

static LCK_GRP_DECLARE(vgpu_grp, "hakios_vgpu");
static LCK_MTX_DECLARE(vgpu_lock, &vgpu_grp);

static struct {
	int state;                              /* 0 not tried, 1 up, -1 no device */
	vio_dev_t *dev;
	vio_queue_t *ctrlq;
	vio_queue_t *cursorq;                  /* unused, but crosvm wants it set up */
	uint64_t features;
	uint8_t *cmd, *resp;                    /* device-visible bounce buffers */
	uint64_t cmd_pa, resp_pa;
	/* the scanout */
	bool shown;
	void *fb;
	uint64_t fb_pa;
	uint32_t width, height, stride;
} vg;

/* Under vgpu_lock. */
static int
vgpu_setup(void)
{
	vio_dev_t *d;
	uint64_t want = VIRTIO_BIT(VIRTIO_GPU_F_VIRGL) | VIRTIO_BIT(VIRTIO_GPU_F_CONTEXT_INIT);

	if (vg.state) {
		return vg.state > 0 ? 0 : ENODEV;
	}
	vg.state = -1;
	if ((d = vio_find(VIRTIO_ID_GPU, 0)) == NULL) {
		return ENODEV;
	}
	/*
	 * Both queues: crosvm activates virtio-gpu only once the cursor queue (1) is set up as
	 * well ("pcivirtio-gpu activate failed: expected 2 queues, got 1"); nothing is put on it.
	 */
	if (vio_init(d, want, &vg.features) != 0 || vio_queue_setup(d, 0, 0, &vg.ctrlq) != 0 ||
	    vio_queue_setup(d, 1, 0, &vg.cursorq) != 0) {
		printf("virtio-gpu: %s: refused\n", vio_name(d));
		vio_fail(d);
		return ENODEV;
	}
	vg.cmd = vio_dma_alloc(HAKIOS_VGPU_CTRL_MAX, &vg.cmd_pa);
	vg.resp = vio_dma_alloc(HAKIOS_VGPU_RESP_MAX, &vg.resp_pa);
	if (vg.cmd == NULL || vg.resp == NULL) {
		vio_fail(d);
		return ENOMEM;
	}
	vio_queue_intr(vg.ctrlq, true);
	int intr = vio_intr_enable(d);          /* (or polled) */
	vio_driver_ok(d);
	printf("virtio-gpu: %s: %s\n", vio_name(d), intr == 0 && vio_intr_active(d) ? "interrupts" :
	    intr == ENOTSUP ? "polled (virtio_poll=1)" : "polled: no interrupt line");
	vg.dev = d;
	vg.state = 1;
	printf("virtio-gpu: %s, features 0x%llx%s%s\n", vio_name(d), vg.features,
	    vg.features & VIRTIO_BIT(VIRTIO_GPU_F_VIRGL) ? " virgl" : "",
	    vg.features & VIRTIO_BIT(VIRTIO_GPU_F_CONTEXT_INIT) ? " context-init" : "");
	return 0;
}

/* One command, waited for. Under vgpu_lock, device up. */
static int
vgpu_submit(const void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len)
{
	struct vio_sg sg[2] = {
		{ vg.cmd_pa, cmd_len, false },
		{ vg.resp_pa, resp_len, true },
	};
	uint32_t seq, len;
	void *done;

	if (cmd_len < sizeof(struct ctrl_hdr) || cmd_len > HAKIOS_VGPU_CTRL_MAX ||
	    resp_len < sizeof(struct ctrl_hdr) || resp_len > HAKIOS_VGPU_RESP_MAX) {
		return EINVAL;
	}
	memcpy(vg.cmd, cmd, cmd_len);
	bzero(vg.resp, resp_len);
	seq = vio_intr_seq(vg.dev);
	if (vio_queue_add(vg.ctrlq, sg, 2, (void *)1) != 0) {
		return EIO;
	}
	vio_queue_kick(vg.ctrlq);
	/*
	 * Most commands are answered in tens of microseconds, and 3D ones (a gfxstream pipe read
	 * waits for the host's render thread) in a few milliseconds. Interrupts may not come
	 * (polled), and a sleep is 10 ms at least: spin for the first 4 ms, then sleep a millisecond
	 * at a time. A read of the pipe may wait for long GPU work: give up after 20 s, not 2.
	 */
	uint64_t now = mach_absolute_time(), spin_end, give_up;
	nanoseconds_to_absolutetime(4 * NSEC_PER_MSEC, &spin_end);
	nanoseconds_to_absolutetime(20 * NSEC_PER_SEC, &give_up);
	spin_end += now;
	give_up += now;
	while ((done = vio_queue_get(vg.ctrlq, &len)) == NULL) {
		now = mach_absolute_time();
		if (now > give_up) {
			printf("virtio-gpu: command 0x%x not answered\n", ((const struct ctrl_hdr *)cmd)->type);
			return ETIMEDOUT;
		}
		if (now < spin_end) {
			__asm__ volatile ("yield");
			continue;
		}
		vio_intr_wait(vg.dev, seq, NSEC_PER_MSEC);
		seq = vio_intr_seq(vg.dev);
	}
	vio_intr_isr(vg.dev);
	memcpy(resp, vg.resp, resp_len);
	return 0;
}

/* A command whose answer is just "OK". */
static int
vgpu_simple(void *cmd, uint32_t len, uint32_t type)
{
	struct ctrl_hdr resp;

	bzero(cmd, sizeof(struct ctrl_hdr));
	((struct ctrl_hdr *)cmd)->type = type;
	if (vgpu_submit(cmd, len, &resp, sizeof(resp)) != 0) {
		return EIO;
	}
	if (resp.type != RESP_OK_NODATA) {
		printf("virtio-gpu: command 0x%x: answer 0x%x\n", type, resp.type);
		return EIO;
	}
	return 0;
}

int
hakios_vgpu_ctrl(const void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len)
{
	int error;

	lck_mtx_lock(&vgpu_lock);
	error = vgpu_setup();
	if (error == 0) {
		error = vgpu_submit(cmd, cmd_len, resp, resp_len);
	}
	lck_mtx_unlock(&vgpu_lock);
	return error;
}

uint64_t
hakios_vgpu_features(void)
{
	return vg.state > 0 ? vg.features : 0;
}

int
hakios_vgpu_display(void **fb, uint32_t *width, uint32_t *height, uint32_t *stride)
{
	static struct resp_display_info info;   /* (under the lock) */
	struct ctrl_hdr get = { .type = CMD_GET_DISPLAY_INFO };
	struct resource_create_2d create;
	struct attach_backing attach;
	struct set_scanout scanout;
	uint32_t w = 0, h = 0;
	int error;

	lck_mtx_lock(&vgpu_lock);
	if ((error = vgpu_setup()) != 0 || vg.shown) {
		goto out;
	}
	error = EIO;
	if (vgpu_submit(&get, sizeof(get), &info, sizeof(info)) != 0 || info.hdr.type != RESP_OK_DISPLAY_INFO) {
		goto out;
	}
	if (info.pmodes[0].enabled) {
		w = info.pmodes[0].r.width;
		h = info.pmodes[0].r.height;
	}
	if (!PE_parse_boot_argn("vgpu_width", &w, sizeof(w)) && w == 0) {
		w = 1125;
	}
	if (!PE_parse_boot_argn("vgpu_height", &h, sizeof(h)) && h == 0) {
		h = 2436;
	}
	vg.width = w;
	vg.height = h;
	vg.stride = w * 4;
	vg.fb = vio_dma_alloc((size_t)vg.stride * h, &vg.fb_pa);
	if (vg.fb == NULL) {
		error = ENOMEM;
		goto out;
	}

	create.resource_id = HAKIOS_VGPU_2D_RESOURCE;
	create.format = FORMAT_B8G8R8X8_UNORM;
	create.width = w;
	create.height = h;
	attach.resource_id = HAKIOS_VGPU_2D_RESOURCE;
	attach.nr_entries = 1;
	attach.entry.addr = vg.fb_pa;
	attach.entry.length = vg.stride * h;
	attach.entry.padding = 0;
	scanout.r = (struct rect){ 0, 0, w, h };
	scanout.scanout_id = 0;
	scanout.resource_id = HAKIOS_VGPU_2D_RESOURCE;
	if (vgpu_simple(&create, sizeof(create), CMD_RESOURCE_CREATE_2D) != 0 ||
	    vgpu_simple(&attach, sizeof(attach), CMD_RESOURCE_ATTACH_BACKING) != 0 ||
	    vgpu_simple(&scanout, sizeof(scanout), CMD_SET_SCANOUT) != 0) {
		goto out;
	}
	vg.shown = true;
	printf("virtio-gpu: scanout 0 is %ux%u at 0x%llx\n", w, h, vg.fb_pa);
	error = 0;
out:
	if (vg.shown) {
		*fb = vg.fb;
		*width = vg.width;
		*height = vg.height;
		*stride = vg.stride;
		error = 0;
	}
	lck_mtx_unlock(&vgpu_lock);
	return error;
}

int
hakios_vgpu_flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	struct transfer_to_host_2d transfer;
	struct resource_flush flush;
	int error = ENODEV;

	lck_mtx_lock(&vgpu_lock);
	if (vg.shown) {
		transfer.r = (struct rect){ x, y, width, height };
		transfer.offset = (uint64_t)y * vg.stride + (uint64_t)x * 4;
		transfer.resource_id = HAKIOS_VGPU_2D_RESOURCE;
		transfer.padding = 0;
		flush.r = transfer.r;
		flush.resource_id = HAKIOS_VGPU_2D_RESOURCE;
		flush.padding = 0;
		error = vgpu_simple(&transfer, sizeof(transfer), CMD_TRANSFER_TO_HOST_2D);
		if (error == 0) {
			error = vgpu_simple(&flush, sizeof(flush), CMD_RESOURCE_FLUSH);
		}
	}
	lck_mtx_unlock(&vgpu_lock);
	return error;
}

#else /* no virtio core in this kernel */

int
hakios_vgpu_display(__unused void **fb, __unused uint32_t *width, __unused uint32_t *height, __unused uint32_t *stride)
{
	return ENODEV;
}

int
hakios_vgpu_flush(__unused uint32_t x, __unused uint32_t y, __unused uint32_t width, __unused uint32_t height)
{
	return ENODEV;
}

int
hakios_vgpu_ctrl(__unused const void *cmd, __unused uint32_t cmd_len, __unused void *resp, __unused uint32_t resp_len)
{
	return ENODEV;
}

uint64_t
hakios_vgpu_features(void)
{
	return 0;
}

#endif
