/*
 * The kernel's side of virtio-gpu 3D (bsd/dev/redwood_vgpu3d.c): where its control queue
 * comes from, its memory, its log and its clock.
 *
 * Boot-arg gpu3d=1 (AVF with a GpuConfig: crosvm --gpu backend=gfxstream): bsd_init brings up
 * a gfxstream context and runs vgpu3d_selftest() before it goes on (about 0.2 s; synchronous,
 * so the result is in before process 1 runs; gpu3d=2 runs it in a thread instead). The Pixel's GPU draws a
 * picture, the guest reads it back and checks it, and the "GPU3D ..." lines on the console
 * report the result, the host's GL strings and timings. With a scanout, the picture is also
 * put on it (SET_SCANOUT of the gfxstream colour buffer, no copy through the guest).
 * gpu3d_size=WxH sets the picture's size (default 828x1792).
 *
 * The virtio-gpu device belongs to the display driver (2D, redwood_virtio_gpu). Until that
 * driver exports its control queue to this layer, gpu3d=1 drives the device on its own:
 * features, both queues (crosvm activates the device only with both), polled requests. That
 * is a bring-up mode for a kernel booted without the display driver, and the display driver
 * leaves the device alone when it sees gpu3d=1.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/redwood_virtio.h>
#include <sys/redwood_vgpu3d.h>

#define VGPU_CFG_NUM_SCANOUTS   0x08
#define VGPU_CFG_NUM_CAPSETS    0x0c
#define VGPU_TIMEOUT_NS         (20ull * NSEC_PER_SEC)

static struct {
	vio_dev_t       *dev;
	vio_queue_t     *ctrlq, *cursorq;
	lck_mtx_t       lock;
	uint8_t         *cmd, *resp;            /* one page each */
	uint64_t        cmd_pa, resp_pa;
	uint64_t        features;
	uint32_t        num_scanouts, num_capsets;
} vg;

static LCK_GRP_DECLARE(vgpu3d_lck_grp, "redwood-vgpu3d");
static struct vgpu3d vgpu3d_state;

static int
xnu_ctrl(__unused void *arg, const void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len)
{
	struct vio_sg sg[2];
	uint64_t deadline;
	uint32_t seq;
	int err = 0;

	if (cmd_len > PAGE_SIZE || resp_len > PAGE_SIZE) {
		return E2BIG;
	}
	lck_mtx_lock(&vg.lock);
	memcpy(vg.cmd, cmd, cmd_len);
	memset(vg.resp, 0, resp_len);
	sg[0] = (struct vio_sg){ vg.cmd_pa, cmd_len, false };
	sg[1] = (struct vio_sg){ vg.resp_pa, resp_len, true };
	seq = vio_intr_seq(vg.dev);
	if (vio_queue_add(vg.ctrlq, sg, 2, &vg) != 0) {
		err = EIO;
		goto out;
	}
	vio_queue_kick(vg.ctrlq);
	nanoseconds_to_deadline(VGPU_TIMEOUT_NS, &deadline);
	while (vio_queue_get(vg.ctrlq, NULL) == NULL) {
		if (mach_absolute_time() > deadline) {
			printf("vgpu3d: %s: a control request went unanswered\n", vio_name(vg.dev));
			err = ETIMEDOUT;
			goto out;
		}
		vio_intr_wait(vg.dev, seq, NSEC_PER_MSEC);
		seq = vio_intr_seq(vg.dev);
	}
	memcpy(resp, vg.resp, resp_len);
out:
	lck_mtx_unlock(&vg.lock);
	return err;
}

static void *
xnu_dma_alloc(__unused void *arg, size_t size, uint64_t *pa)
{
	return vio_dma_alloc(size, pa);
}

static void
xnu_log(__unused void *arg, const char *line)
{
	printf("%s\n", line);
}

static uint64_t
xnu_now_ns(__unused void *arg)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

/* The device, standalone (see above). 0 or an errno. */
static int
vgpu3d_attach_standalone(void)
{
	uint64_t want = (1ull << VIRTIO_GPU_F_VIRGL) | (1ull << VIRTIO_GPU_F_CONTEXT_INIT) |
	    (1ull << VIRTIO_GPU_F_RESOURCE_BLOB) | (1ull << VIRTIO_GPU_F_EDID);
	int err;

	vg.dev = vio_find(VIRTIO_ID_GPU, 0);
	if (vg.dev == NULL) {
		printf("vgpu3d: no virtio-gpu device (does the VM have a GpuConfig?)\n");
		return ENXIO;
	}
	if ((err = vio_init(vg.dev, want, &vg.features)) != 0 ||
	    (err = vio_queue_setup(vg.dev, 0, 0, &vg.ctrlq)) != 0 ||
	    (err = vio_queue_setup(vg.dev, 1, 0, &vg.cursorq)) != 0) {
		printf("vgpu3d: %s: setup failed (%d)\n", vio_name(vg.dev), err);
		vio_fail(vg.dev);
		return err;
	}
	vg.cmd = vio_dma_alloc(PAGE_SIZE, &vg.cmd_pa);
	vg.resp = vio_dma_alloc(PAGE_SIZE, &vg.resp_pa);
	if (vg.cmd == NULL || vg.resp == NULL) {
		vio_fail(vg.dev);
		return ENOMEM;
	}
	(void)vio_intr_enable(vg.dev);  /* polled when there is no line */
	vio_driver_ok(vg.dev);
	vg.num_scanouts = vio_cfg_read32(vg.dev, VGPU_CFG_NUM_SCANOUTS);
	vg.num_capsets = vio_cfg_read32(vg.dev, VGPU_CFG_NUM_CAPSETS);
	printf("vgpu3d: %s: features 0x%llx, %u scanouts, %u capsets\n", vio_name(vg.dev),
	    vg.features, vg.num_scanouts, vg.num_capsets);
	return 0;
}

static void
vgpu3d_thread(__unused void *param, __unused wait_result_t wr)
{
	struct vgpu3d *g = &vgpu3d_state;
	static struct vgpu3d_selftest t;
	char size[24] = "";
	uint32_t w = 828, h = 1792;
	uint64_t pa;

	if (PE_parse_boot_argn("gpu3d_size", size, sizeof(size))) {
		char *x = strchr(size, 'x');
		if (x != NULL) {
			*x = 0;
			w = (uint32_t)strtoul(size, NULL, 10);
			h = (uint32_t)strtoul(x + 1, NULL, 10);
		}
	}
	if (vgpu3d_attach_standalone() != 0) {
		printf("GPU3D-DONE fail\n");
		return;
	}
	g->ops = (struct vgpu3d_ops){ NULL, xnu_ctrl, xnu_dma_alloc, xnu_log, xnu_now_ns };
	if (vgpu3d_query(g, vg.features, vg.num_capsets) ||
	    vgpu3d_open(g, vgpu3d_has_capset(g, VIRTIO_GPU_CAPSET_GFXSTREAM_GLES) ?
	    VIRTIO_GPU_CAPSET_GFXSTREAM_GLES : 0, 1 << 20)) {
		printf("GPU3D-DONE fail\n");
		return;
	}
	t.width = w;
	t.height = h;
	t.pixels = vio_dma_alloc((size_t)w * h * 4, &pa);
	if (t.pixels == NULL) {
		printf("vgpu3d: no memory for a %ux%u picture\n", w, h);
		printf("GPU3D-DONE fail\n");
		return;
	}
	if (vg.num_scanouts) {
		t.scanout_res = vgpu3d_create_scanout_resource(g, w, h);
	}
	uint64_t t0 = xnu_now_ns(NULL);
	int err = vgpu3d_selftest(g, &t);
	printf("GPU3D kernel selftest_ms=%llu\n", (xnu_now_ns(NULL) - t0) / 1000000);
	if (!err && t.scanout_res) {
		err = vgpu3d_present(g, 0, t.scanout_res, w, h);
		printf("GPU3D %s on scanout 0\n", err ? "present failed" : "presented");
	}
	printf("GPU3D-DONE %s\n", !err && t.checks_passed == t.checks_total ? "pass" : "fail");
}

/* From bsd_init, after devfs: boot-arg gpu3d=1 runs the bring-up here, gpu3d=2 in a thread. */
void
redwood_vgpu3d_init(void)
{
	uint32_t on = 0;
	thread_t th;

	if (!PE_parse_boot_argn("gpu3d", &on, sizeof(on)) || !on) {
		return;
	}
	lck_mtx_init(&vg.lock, &vgpu3d_lck_grp, LCK_ATTR_NULL);
	if (on == 1) {
		vgpu3d_thread(NULL, THREAD_AWAKENED);
	} else if (kernel_thread_start(vgpu3d_thread, NULL, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
	}
}
