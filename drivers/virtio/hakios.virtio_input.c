/*
 * Virtio input devices as a source of touches and buttons (see sys/hakios.input.h).
 *
 * Under QEMU's "virt" machine a "virtio-tablet-device" stands for the touch screen (the pointer
 * with its button held is a finger) and a "virtio-keyboard-device" for the phone's buttons. They
 * sit on the machine's virtio-mmio bus, which must be the current one, not "legacy"
 * (-global virtio-mmio.force-legacy=false). Only with boot-arg virtio_input=1: the bus address
 * is QEMU's and means nothing on the phone.
 *
 * No interrupt is used; the event queues are looked at 125 times a second.
 *
 * The keyboard is also a way to tell the kernel something from the host while the system runs:
 * F12, a process name, Enter prints where every thread of that process is (ustacks_now(); the
 * host side is "qemu/touch.py stacks NAME"). F11 is five fingers on the screen for as long as it
 * is held, which is how the way out in hakios.input.c is tried ("qemu/touch.py key fivefingers 2000").
 *
 * Under crosvm (the Android Virtualization Framework) the touch screen is a virtio-input
 * multi-touch device on PCI, made by the Scion app (ABS_MT_SLOT, ABS_MT_TRACKING_ID,
 * ABS_MT_POSITION_X/Y, range 0..width/height of the framebuffer). With the virtio core
 * (sys/hakios.virtio.h) devices are found on either transport, the axes' ranges are read from
 * the device's configuration space instead of assuming QEMU's 0..0x7fff, and every contact
 * (up to ten) is its own finger. Without the core, the virtio-mmio code below is used as before.
 */
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#if __has_include(<sys/hakios.virtio.h>)
#include <sys/hakios.virtio.h>
#include <sys/hakios.scgpu.h>

extern bool hakios_scgpu_bell_attach(vio_dev_t *v);       /* bsd/dev/hakios.vgpu3d_xnu.c */
#define INPUT_VIO               1
#endif

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */

#define VIRTIO_BUS              0x0a000000UL    /* QEMU virt: 32 transports of 0x200 bytes */
#define VIRTIO_SLOTS            32
#define VIRTIO_SLOT_SIZE        0x200

#define VIRTIO_MAGIC            0x000           /* "virt" */
#define VIRTIO_VERSION          0x004
#define VIRTIO_DEVICE_ID        0x008
#define VIRTIO_DRIVER_FEATURES  0x020
#define VIRTIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_QUEUE_SEL        0x030
#define VIRTIO_QUEUE_NUM_MAX    0x034
#define VIRTIO_QUEUE_NUM        0x038
#define VIRTIO_QUEUE_READY      0x044
#define VIRTIO_QUEUE_NOTIFY     0x050
#define VIRTIO_STATUS           0x070
#define VIRTIO_QUEUE_DESC       0x080
#define VIRTIO_QUEUE_DRIVER     0x090
#define VIRTIO_QUEUE_DEVICE     0x0a0

#define VIRTIO_ID_INPUT         18
#define STATUS_ACKNOWLEDGE      1
#define STATUS_DRIVER           2
#define STATUS_DRIVER_OK        4
#define STATUS_FEATURES_OK      8
#define STATUS_FAILED           0x80

#define QUEUE_SIZE              64
#define DESC_F_WRITE            2
#define AVAIL_F_NO_INTERRUPT    1

struct vq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};
struct vq_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[QUEUE_SIZE];
};
struct vq_used {
	uint16_t flags;
	uint16_t idx;
	struct {
		uint32_t id;
		uint32_t len;
	} ring[QUEUE_SIZE];
};
struct input_event {
	uint16_t type;
	uint16_t code;
	uint32_t value;
};

/* everything the device reads and writes is in one page */
#define OFFSET_DESC             0x0000
#define OFFSET_AVAIL            0x0400
#define OFFSET_USED             0x0800
#define OFFSET_EVENTS           0x1000

/* Linux input event numbers, which is what virtio input speaks */
#define EV_SYN                  0
#define EV_KEY                  1
#define EV_ABS                  3
#define ABS_X                   0
#define ABS_Y                   1
#define ABS_RANGE               0x7fff          /* QEMU's tablet: 0..0x7fff across the screen */
#define BTN_LEFT                0x110
#define BTN_TOUCH               0x14a
#define ABS_MT_SLOT             0x2f
#define ABS_MT_POSITION_X       0x35
#define ABS_MT_POSITION_Y       0x36
#define ABS_MT_TRACKING_ID      0x39
#define MT_SLOTS                10              /* fingers told apart (the HID side takes ten) */
#define EV_MSC                  4
#define MSC_TIMESTAMP           5               /* microseconds of the sender's clock, wrapping */

/* an axis' range, 0..65535 across the display after scaling */
struct axis {
	int32_t min, max;
};

struct input_device {
	vm_offset_t             regs;
	struct vq_avail *       avail;
	struct vq_used *        used;
	struct input_event *    events;
	uint16_t                seen;           /* how far the used ring has been read */
	/* the pointer */
	uint32_t                x, y;
	bool                    pressed, touching, moved;
	/* multi-touch: one finger per slot */
	struct axis             ax, ay, mtx, mty;
	bool                    mt;             /* the device reports ABS_MT_*: ABS_X/Y are ignored */
	int                     slot;
	struct {
		int32_t         id;             /* tracking id, -1 = no contact */
		uint32_t        x, y;
		bool            down, moved;    /* as last posted, and changed since */
	} contact[MT_SLOTS];
	uint64_t                reports;        /* EV_SYN reports handled */
	/*
	 * The sender's time of the report being read (MSC_TIMESTAMP, the Scion app sends one), and
	 * how it maps onto mach_absolute_time: samples read in a burst then keep their real spacing.
	 */
	bool                    have_ts, ts_synced;
	uint32_t                ts_us, base_us;
	uint64_t                base_abs, last_abs;
#ifdef INPUT_VIO
	vio_dev_t *             vdev;
	vio_queue_t *           vq;
	vio_queue_t *           statusq;        /* (unused, but crosvm refuses to start a device without it) */
	uint64_t                events_pa;
#endif
};

#define MAX_DEVICES     4
static struct input_device devices[MAX_DEVICES];
static int device_count;
#ifndef INPUT_VIO
static thread_call_t poll_call;
#endif

static inline uint32_t
reg_read(struct input_device *d, uint32_t reg)
{
	return *(volatile uint32_t *)(d->regs + reg);
}

static inline void
reg_write(struct input_device *d, uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(d->regs + reg) = value;
}

static inline void
reg_write64(struct input_device *d, uint32_t reg, uint64_t value)
{
	reg_write(d, reg, (uint32_t)value);
	reg_write(d, reg + 4, (uint32_t)(value >> 32));
}

extern void ustacks_now(const char *name);

#define KEY_ENTER               28
#define KEY_LEFTSHIFT           42
#define KEY_RIGHTSHIFT          54
#define KEY_F11                 87
#define KEY_F12                 88

/* F12 NAME Enter: the keys in between spell a process name. Returns true if the key was taken. */
static bool
command_key(uint16_t key, uint32_t value)
{
	static const char plain[] =
	    "\0\0001234567890-=\0\0qwertyuiop[]\0\0asdfghjkl;'`\0\\zxcvbnm,./\0\0\0 ";
	static char name[32];
	static unsigned length;
	static bool active, shift;

	if (key == KEY_F12) {
		if (value == 1) {
			active = true;
			length = 0;
		}
		return true;
	}
	if (!active) {
		return false;
	}
	if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) {
		shift = value != 0;
	} else if (value != 1) {
		/* a release or a repeat */
	} else if (key == KEY_ENTER) {
		active = false;
		name[length] = 0;
		if (length) {
			printf("virtio-input: stacks of \"%s\" asked for\n", name);
			ustacks_now(name);
		}
	} else if (key < sizeof(plain) - 1 && plain[key] != 0 && length < sizeof(name) - 1) {
		char c = plain[key];

		if (shift && c >= 'a' && c <= 'z') {
			c -= 'a' - 'A';
		} else if (shift && c == '-') {
			c = '_';
		}
		name[length++] = c;
	}
	return true;
}

/* Which of the phone's buttons a key stands for; 0 = none. */
static uint32_t
button_for_key(uint16_t key)
{
	switch (key) {
	case 116: /* KEY_POWER */
	case 38:  /* KEY_L */
		return HAKIOS_BUTTON_LOCK;
	case 115: /* KEY_VOLUMEUP */
	case 103: /* KEY_UP */
		return HAKIOS_BUTTON_VOLUME_UP;
	case 114: /* KEY_VOLUMEDOWN */
	case 108: /* KEY_DOWN */
		return HAKIOS_BUTTON_VOLUME_DOWN;
	case 102: /* KEY_HOME */
	case 35:  /* KEY_H */
		return HAKIOS_BUTTON_HOME;
	}
	return 0;
}

static uint32_t
scale(const struct axis *a, int32_t v)
{
	int64_t span = (int64_t)a->max - a->min;

	if (span <= 0) {
		return 0;
	}
	if (v < a->min) {
		v = a->min;
	} else if (v > a->max) {
		v = a->max;
	}
	return (uint32_t)(((int64_t)v - a->min) * 65535 / span);
}

/*
 * When the report being finished was made, in mach_absolute_time, from its MSC_TIMESTAMP; 0
 * (= now) without one. The sender's clock is mapped by the earliest-arriving report seen: a
 * report that would map into the future moves the mapping to now, so it tracks the shortest
 * delivery delay. A mapping more than a second off starts again. Never earlier than the previous.
 */
static uint64_t
report_time(struct input_device *d)
{
	uint64_t now = mach_absolute_time(), t, delta, limit;

	if (!d->have_ts) {
		return 0;
	}
	d->have_ts = false;
	nanoseconds_to_absolutetime((uint64_t)(uint32_t)(d->ts_us - d->base_us) * NSEC_PER_USEC, &delta);
	nanoseconds_to_absolutetime(NSEC_PER_SEC, &limit);
	t = d->base_abs + delta;
	if (!d->ts_synced || t > now || now - t > limit) {
		d->ts_synced = true;
		d->base_us = d->ts_us;
		d->base_abs = now;
		t = now;
	}
	if (t < d->last_abs) {
		t = d->last_abs;
	}
	d->last_abs = t;
	return t;
}

/* A whole report has come (EV_SYN): post what changed for every contact. */
static void
mt_report(struct input_device *d)
{
	uint64_t when = report_time(d);

	for (int i = 0; i < MT_SLOTS; i++) {
		bool down = d->contact[i].id >= 0;

		if (down && !d->contact[i].down) {
			hakios_touch_post_at(HAKIOS_TOUCH_DOWN, (uint32_t)i, d->contact[i].x, d->contact[i].y, when);
		} else if (down && d->contact[i].moved) {
			hakios_touch_post_at(HAKIOS_TOUCH_MOVE, (uint32_t)i, d->contact[i].x, d->contact[i].y, when);
		} else if (!down && d->contact[i].down) {
			hakios_touch_post_at(HAKIOS_TOUCH_UP, (uint32_t)i, d->contact[i].x, d->contact[i].y, when);
		}
		d->contact[i].down = down;
		d->contact[i].moved = false;
	}
}

static void
input_event(struct input_device *d, const struct input_event *e)
{
	uint32_t button;

	switch (e->type) {
	case EV_ABS:
		if (e->code == ABS_MT_SLOT) {
			d->slot = (int32_t)e->value >= 0 && (int32_t)e->value < MT_SLOTS ? (int)e->value : 0;
			d->mt = true;
		} else if (e->code == ABS_MT_TRACKING_ID) {
			d->contact[d->slot].id = (int32_t)e->value;
			d->mt = true;
		} else if (e->code == ABS_MT_POSITION_X) {
			d->contact[d->slot].x = scale(&d->mtx, (int32_t)e->value);
			d->contact[d->slot].moved = true;
			d->mt = true;
		} else if (e->code == ABS_MT_POSITION_Y) {
			d->contact[d->slot].y = scale(&d->mty, (int32_t)e->value);
			d->contact[d->slot].moved = true;
			d->mt = true;
		} else if (e->code == ABS_X) {
			d->x = scale(&d->ax, (int32_t)e->value);
			d->moved = true;
		} else if (e->code == ABS_Y) {
			d->y = scale(&d->ay, (int32_t)e->value);
			d->moved = true;
		}
		break;
	case EV_KEY:
		if (d->mt && e->code == BTN_TOUCH) {
			/* contacts say it themselves */
		} else if (e->code == BTN_LEFT || e->code == BTN_TOUCH) {
			d->pressed = e->value != 0;
		} else if (command_key(e->code, e->value)) {
			break;
		} else if (e->code == KEY_F11 && e->value < 2) {
			/* five more fingers, for as long as the key is held: the way out of hakios.input.c */
			for (uint32_t i = 0; i < 5; i++) {
				hakios_touch_post(e->value ? HAKIOS_TOUCH_DOWN : HAKIOS_TOUCH_UP, 5 + i, 13107 + 9830 * i, 32768);
			}
		} else if ((button = button_for_key(e->code)) != 0 && e->value < 2) {   /* 2 = key repeat */
			hakios_touch_post(e->value ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, button, 0, 0);
		}
		break;
	case EV_MSC:
		if (e->code == MSC_TIMESTAMP) {
			d->ts_us = e->value;
			d->have_ts = true;
		}
		break;
	case EV_SYN:
		d->reports++;
		if (d->mt) {
			mt_report(d);
			break;
		}
		d->have_ts = false;
		if (d->pressed && !d->touching) {
			hakios_touch_post(HAKIOS_TOUCH_DOWN, 0, d->x, d->y);
		} else if (d->pressed && d->moved) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, 0, d->x, d->y);
		} else if (!d->pressed && d->touching) {
			hakios_touch_post(HAKIOS_TOUCH_UP, 0, d->x, d->y);
		}
		d->touching = d->pressed;
		d->moved = false;
		break;
	}
}

/* QEMU's tablet range until the device says otherwise; no contacts */
static void
input_defaults(struct input_device *d)
{
	d->ax = d->ay = d->mtx = d->mty = (struct axis){ 0, ABS_RANGE };
	for (int i = 0; i < MT_SLOTS; i++) {
		d->contact[i].id = -1;
	}
}

#ifndef INPUT_VIO
static void
input_poll(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	for (int i = 0; i < device_count; i++) {
		struct input_device *d = &devices[i];
		bool returned = false;

		while (d->seen != *(volatile uint16_t *)&d->used->idx) {
			/* the ring entry and the event must be read after the index that announced them (arm64 does not order those loads by itself) */
			__builtin_arm_dmb(0x9);         /* ISHLD */
			uint32_t id = d->used->ring[d->seen++ % QUEUE_SIZE].id % QUEUE_SIZE;

			input_event(d, &d->events[id]);
			/* give the buffer back */
			d->avail->ring[d->avail->idx % QUEUE_SIZE] = (uint16_t)id;
			__builtin_arm_dmb(0xb);         /* ISH */
			d->avail->idx++;
			returned = true;
		}
		if (returned) {
			__builtin_arm_dmb(0xb);
			reg_write(d, VIRTIO_QUEUE_NOTIFY, 0);
		}
	}
	clock_interval_to_deadline(8, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(poll_call, deadline);
}

static bool
input_attach(struct input_device *d, int slot)
{
	vm_offset_t page = 0;
	struct vq_desc *desc;
	uint64_t phys;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	phys = kvtophys(page);
	desc = (struct vq_desc *)(page + OFFSET_DESC);
	d->avail = (struct vq_avail *)(page + OFFSET_AVAIL);
	d->used = (struct vq_used *)(page + OFFSET_USED);
	d->events = (struct input_event *)(page + OFFSET_EVENTS);

	reg_write(d, VIRTIO_STATUS, 0);         /* reset */
	reg_write(d, VIRTIO_STATUS, STATUS_ACKNOWLEDGE);
	reg_write(d, VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
	reg_write(d, VIRTIO_DRIVER_FEATURES_SEL, 0);
	reg_write(d, VIRTIO_DRIVER_FEATURES, 0);
	reg_write(d, VIRTIO_DRIVER_FEATURES_SEL, 1);
	reg_write(d, VIRTIO_DRIVER_FEATURES, 1);        /* bit 32: VIRTIO_F_VERSION_1 */
	reg_write(d, VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
	if (!(reg_read(d, VIRTIO_STATUS) & STATUS_FEATURES_OK)) {
		printf("virtio-input: slot %d: features refused\n", slot);
		/* the device never got this page: give it back and tell the device the driver gave up, so the failing slot costs nothing */
		reg_write(d, VIRTIO_STATUS, STATUS_FAILED);
		kmem_free(kernel_map, page, PAGE_SIZE);
		return false;
	}

	/* queue 0 brings the events: hand the device all the buffers */
	reg_write(d, VIRTIO_QUEUE_SEL, 0);
	if (reg_read(d, VIRTIO_QUEUE_NUM_MAX) < QUEUE_SIZE) {
		printf("virtio-input: slot %d: event queue of %u\n", slot, reg_read(d, VIRTIO_QUEUE_NUM_MAX));
		/* same as above: the queue was never given to the device */
		reg_write(d, VIRTIO_STATUS, STATUS_FAILED);
		kmem_free(kernel_map, page, PAGE_SIZE);
		return false;
	}
	for (int i = 0; i < QUEUE_SIZE; i++) {
		desc[i].addr = phys + OFFSET_EVENTS + (uint64_t)i * sizeof(struct input_event);
		desc[i].len = sizeof(struct input_event);
		desc[i].flags = DESC_F_WRITE;
		d->avail->ring[i] = (uint16_t)i;
	}
	d->avail->flags = AVAIL_F_NO_INTERRUPT;
	d->avail->idx = QUEUE_SIZE;
	reg_write(d, VIRTIO_QUEUE_NUM, QUEUE_SIZE);
	reg_write64(d, VIRTIO_QUEUE_DESC, phys + OFFSET_DESC);
	reg_write64(d, VIRTIO_QUEUE_DRIVER, phys + OFFSET_AVAIL);
	reg_write64(d, VIRTIO_QUEUE_DEVICE, phys + OFFSET_USED);
	__builtin_arm_dmb(0xb);
	reg_write(d, VIRTIO_QUEUE_READY, 1);
	reg_write(d, VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
	reg_write(d, VIRTIO_QUEUE_NOTIFY, 0);
	return true;
}
#endif /* !INPUT_VIO */

#ifdef INPUT_VIO
/* virtio-input configuration space (virtio 1.2, 5.8.4) */
#define CFG_SELECT              0
#define CFG_SUBSEL              1
#define CFG_SIZE                2
#define CFG_UNION               8
#define CFG_ID_NAME             0x01
#define CFG_ABS_INFO            0x12
#define VIO_EVENTS              64

static bool
vio_axis(vio_dev_t *v, uint8_t code, struct axis *a)
{
	vio_cfg_write8(v, CFG_SELECT, CFG_ABS_INFO);
	vio_cfg_write8(v, CFG_SUBSEL, code);
	if (vio_cfg_read8(v, CFG_SIZE) < 8) {
		return false;
	}
	a->min = (int32_t)vio_cfg_read32(v, CFG_UNION);
	a->max = (int32_t)vio_cfg_read32(v, CFG_UNION + 4);
	return a->max > a->min;
}

/* Every interrupt, and every 8 ms anyway: handle what came, give the buffers back. */
static void
vio_input_service(void *arg, __unused uint8_t isr)
{
	struct input_device *d = arg;
	uint32_t len;
	void *cookie;
	bool returned = false;

	while ((cookie = vio_queue_get(d->vq, &len)) != NULL) {
		uintptr_t i = (uintptr_t)cookie - 1;
		struct vio_sg sg = { d->events_pa + i * sizeof(struct input_event), sizeof(struct input_event), true };

		if (len >= sizeof(struct input_event)) {
			input_event(d, &d->events[i]);
		}
		vio_queue_add(d->vq, &sg, 1, cookie);
		returned = true;
	}
	if (returned) {
		vio_queue_kick(d->vq);
	}
}

static bool
vio_input_attach(struct input_device *d, vio_dev_t *v)
{
	char name[64];
	uint64_t features;
	uint8_t size;

	if (vio_init(v, 0, &features) != 0) {
		printf("virtio-input: %s: refused\n", vio_name(v));
		return false;
	}
	vio_cfg_write8(v, CFG_SELECT, CFG_ID_NAME);
	vio_cfg_write8(v, CFG_SUBSEL, 0);
	size = vio_cfg_read8(v, CFG_SIZE);
	size = size < sizeof(name) - 1 ? size : sizeof(name) - 1;
	vio_cfg_read(v, CFG_UNION, name, size);
	name[size] = 0;
	/* SCGPU's doorbell, not a touch screen: the SCGPU code takes it (bsd/dev/hakios.vgpu3d_xnu.c) */
	if (strcmp(name, SCGPU_BELL_NAME) == 0) {
		hakios_scgpu_bell_attach(v);
		return false;
	}
	input_defaults(d);
	vio_axis(v, ABS_X, &d->ax);
	vio_axis(v, ABS_Y, &d->ay);
	d->mt = vio_axis(v, ABS_MT_POSITION_X, &d->mtx) && vio_axis(v, ABS_MT_POSITION_Y, &d->mty);
	if (vio_queue_setup(v, 0, VIO_EVENTS, &d->vq) != 0) {
		printf("virtio-input: %s: no event queue\n", vio_name(v));
		vio_fail(v);
		return false;
	}
	/*
	 * The status queue (LED state and the like, guest to device) is never used here, but it must
	 * exist: crosvm's virtio-input does not activate with one queue ("expected 2 queues, got 1"),
	 * and then no event ever comes. QEMU does not mind.
	 */
	if (vio_queue_setup(v, 1, 8, &d->statusq) != 0) {
		printf("virtio-input: %s: no status queue\n", vio_name(v));
	}
	d->events = vio_dma_alloc(VIO_EVENTS * sizeof(struct input_event), &d->events_pa);
	if (d->events == NULL) {
		vio_fail(v);
		return false;
	}
	for (uintptr_t i = 0; i < vio_queue_size(d->vq); i++) {
		struct vio_sg sg = { d->events_pa + i * sizeof(struct input_event), sizeof(struct input_event), true };

		vio_queue_add(d->vq, &sg, 1, (void *)(i + 1));
	}
	d->vdev = v;
	vio_intr_enable(v);                     /* (polled if there is no line) */
	vio_driver_ok(v);
	vio_queue_kick(d->vq);
	printf("virtio-input: %s \"%s\": %s x %d..%d y %d..%d\n", vio_name(v), name, d->mt ? "multi-touch" : "pointer",
	    d->mt ? d->mtx.min : d->ax.min, d->mt ? d->mtx.max : d->ax.max, d->mt ? d->mty.min : d->ay.min,
	    d->mt ? d->mty.max : d->ay.max);
	vio_service_start(v, vio_input_service, d, 8 * NSEC_PER_MSEC);
	return true;
}

/* Every virtio input device, on PCI (crosvm) or virtio-mmio (QEMU, with its boot-arg). */
void
hakios_virtio_input_init(void)
{
	vio_dev_t *v;

	for (unsigned n = 0; device_count < MAX_DEVICES && (v = vio_find(VIRTIO_ID_INPUT, n)) != NULL; n++) {
		if (vio_input_attach(&devices[device_count], v)) {
			device_count++;
		}
	}
	if (device_count == 0) {
		printf("virtio-input: no input devices\n");
	}
}

#else /* !INPUT_VIO: QEMU's virtio-mmio bus only */
void
hakios_virtio_input_init(void)
{
	uint32_t enabled = 0;
	vm_offset_t bus;

	if (!PE_parse_boot_argn("virtio_input", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	bus = ml_io_map(VIRTIO_BUS, VIRTIO_SLOTS * VIRTIO_SLOT_SIZE);
	for (int slot = 0; slot < VIRTIO_SLOTS && device_count < MAX_DEVICES; slot++) {
		struct input_device *d = &devices[device_count];

		d->regs = bus + (vm_offset_t)slot * VIRTIO_SLOT_SIZE;
		if (reg_read(d, VIRTIO_MAGIC) != 0x74726976 || reg_read(d, VIRTIO_DEVICE_ID) != VIRTIO_ID_INPUT) {
			continue;
		}
		if (reg_read(d, VIRTIO_VERSION) != 2) {
			printf("virtio-input: slot %d is a legacy transport; start QEMU with -global virtio-mmio.force-legacy=false\n", slot);
			continue;
		}
		input_defaults(d);
		if (input_attach(d, slot)) {
			printf("virtio-input: device in slot %d\n", slot);
			device_count++;
		}
	}
	if (device_count) {
		poll_call = thread_call_allocate(input_poll, NULL);
		input_poll(NULL, NULL);
	} else {
		printf("virtio-input: no input devices\n");
	}
}
#endif /* INPUT_VIO */
