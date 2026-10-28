/*
 * The flashlight under AVF (patch 1402): the phone's own torch, switched by the Scion app.
 *
 * iOS's flashlight (Control Center's, and the camera's torch) is a TorchLevel that the media library
 * in mediaserverd writes to /dev/hakios-torch (userland/src/camera_device.c): a 32-bit number, 0 to
 * 1000, thousandths of the brightest, 0 off. On the POCO the kernel drives the LED itself
 * (bsd/dev/hakios.torch.c, torch=3). Under AVF the app lends the camera region (SCCAM, sccam.addr=,
 * patch 1400) and is already watching its header; the level goes into a word of the guest's part of
 * it, SCCAM_TORCH, and the app turns the phone's torch on and off by it (Android's CameraManager, or
 * the flash of its own capture while iOS has the back camera). avfboot then also says the camera has
 * a flash (the device tree's "flash": MobileGestalt's camera-flash, without which there is no
 * flashlight anywhere in iOS).
 *
 * Nothing is made with torch=N (the POCO's driver has the node) or without sccam.addr=.
 */
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);

#define SCCAM_MAGIC     0x4d434353u     /* "SCCM" (patch 1400) */
#define SCCAM_TORCH     0x68            /* the guest's: the torch's level, 0 to 1000 (reserved[0] there) */

static volatile uint8_t *header;
static uint32_t level, changes;

static int
torch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value;
	int error;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(value)) {
		if ((error = uiomove((caddr_t)&value, sizeof(value), uio)) != 0) {
			return error;
		}
		value = value > 1000 ? 1000 : value;
		if ((value != 0) != (level != 0) && changes++ < 20) {
			printf("torch: iOS asks for the light %s (%u of 1000): the Scion app switches the phone's\n",
			    value ? "on" : "off", value);
		}
		level = value;
		__atomic_store_n((uint32_t *)(uintptr_t)(header + SCCAM_TORCH), value, __ATOMIC_RELEASE);
	}
	return 0;
}

static int
torch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value = level;

	return uiomove((caddr_t)&value, (int)MIN(sizeof(value), (size_t)uio_resid(uio)), uio);
}

static int
torch_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw sccam_torch_cdevsw = {
	.d_open = torch_open, .d_close = torch_open, .d_read = torch_read, .d_write = torch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

void
hakios_sccam_torch_init(void)
{
	uint64_t pa = 0;
	uint32_t torch = 0;
	int major;

	if (PE_parse_boot_argn("torch", &torch, sizeof(torch)) && torch != 0) {
		return;
	}
	if (!PE_parse_boot_argn("sccam.addr", &pa, sizeof(pa)) || pa == 0 || (pa & PAGE_MASK) != 0) {
		return;
	}
	header = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)pa, PAGE_SIZE, VM_WIMG_DEFAULT);
	if (header == NULL || *(volatile uint32_t *)header != SCCAM_MAGIC) {
		printf("torch: no camera region of the app's at %#llx: no flashlight\n", pa);
		header = NULL;
		return;
	}
	__atomic_store_n((uint32_t *)(uintptr_t)(header + SCCAM_TORCH), 0, __ATOMIC_RELEASE);
	major = cdevsw_add(-1, &sccam_torch_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-torch") == NULL) {
		printf("torch: /dev/hakios-torch cannot be made\n");
		return;
	}
	printf("torch: /dev/hakios-torch: the phone's torch, through the Scion app (SCCAM)\n");
}
