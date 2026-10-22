/*
 * Virtio input devices as a source of touches and buttons (see sys/redwood_input.h).
 *
 * Under QEMU's "virt" machine a "virtio-tablet-device" stands for the touch screen (the pointer
 * with its button held is a finger) and a "virtio-keyboard-device" for the phone's buttons. Any
 * transport of sys/redwood_virtio.h: on virtio-mmio (-device virtio-tablet-device, with
 * -global virtio-mmio.force-legacy=false) only with boot-arg virtio_input=1, which turns that bus
 * on; on virtio-pci (-device virtio-tablet-pci, crosvm's input devices) always, unless
 * virtio_input=0.
 *
 * Each device has a thread (vio_service_start) that empties its event queue after every
 * interrupt, and 125 times a second when there is no interrupt to wait for.
 *
 * The keyboard is also a way to tell the kernel something from the host while the system runs:
 * F12, a process name, Enter prints where every thread of that process is (ustacks_now(); the
 * host side is "qemu/touch.py stacks NAME"). F11 is five fingers on the screen for as long as it
 * is held, which is how the way out in redwood_input.c is tried ("qemu/touch.py key fivefingers 2000").
 */
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

#include <sys/redwood_virtio.h>

#define QUEUE_SIZE              64

struct input_event {
	uint16_t type;
	uint16_t code;
	uint32_t value;
};

/* Linux input event numbers, which is what virtio input speaks */
#define EV_SYN                  0
#define EV_KEY                  1
#define EV_ABS                  3
#define ABS_X                   0
#define ABS_Y                   1
#define ABS_RANGE               0x7fff          /* QEMU's tablet: 0..0x7fff across the screen */
#define BTN_LEFT                0x110
#define BTN_TOUCH               0x14a

struct input_device {
	vio_dev_t *             dev;
	vio_queue_t *           queue;
	struct input_event *    events;         /* QUEUE_SIZE of them, the device writes into */
	uint64_t                events_pa;
	/* the pointer */
	uint32_t                x, y;
	bool                    pressed, touching, moved;
};

#define MAX_DEVICES     4
static struct input_device devices[MAX_DEVICES];
static int device_count;

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
		return REDWOOD_BUTTON_LOCK;
	case 115: /* KEY_VOLUMEUP */
	case 103: /* KEY_UP */
		return REDWOOD_BUTTON_VOLUME_UP;
	case 114: /* KEY_VOLUMEDOWN */
	case 108: /* KEY_DOWN */
		return REDWOOD_BUTTON_VOLUME_DOWN;
	case 102: /* KEY_HOME */
	case 35:  /* KEY_H */
		return REDWOOD_BUTTON_HOME;
	}
	return 0;
}

static void
input_event(struct input_device *d, const struct input_event *e)
{
	uint32_t button;

	switch (e->type) {
	case EV_ABS:
		if (e->code == ABS_X) {
			d->x = e->value * 65535 / ABS_RANGE;
			d->moved = true;
		} else if (e->code == ABS_Y) {
			d->y = e->value * 65535 / ABS_RANGE;
			d->moved = true;
		}
		break;
	case EV_KEY:
		if (e->code == BTN_LEFT || e->code == BTN_TOUCH) {
			d->pressed = e->value != 0;
		} else if (command_key(e->code, e->value)) {
			break;
		} else if (e->code == KEY_F11 && e->value < 2) {
			/* five more fingers, for as long as the key is held: the way out of redwood_input.c */
			for (uint32_t i = 0; i < 5; i++) {
				redwood_touch_post(e->value ? REDWOOD_TOUCH_DOWN : REDWOOD_TOUCH_UP, 5 + i, 13107 + 9830 * i, 32768);
			}
		} else if ((button = button_for_key(e->code)) != 0 && e->value < 2) {   /* 2 = key repeat */
			redwood_touch_post(e->value ? REDWOOD_BUTTON_DOWN : REDWOOD_BUTTON_UP, button, 0, 0);
		}
		break;
	case EV_SYN:
		if (d->pressed && !d->touching) {
			redwood_touch_post(REDWOOD_TOUCH_DOWN, 0, d->x, d->y);
		} else if (d->pressed && d->moved) {
			redwood_touch_post(REDWOOD_TOUCH_MOVE, 0, d->x, d->y);
		} else if (!d->pressed && d->touching) {
			redwood_touch_post(REDWOOD_TOUCH_UP, 0, d->x, d->y);
		}
		d->touching = d->pressed;
		d->moved = false;
		break;
	}
}

/* Give event buffer i (back) to the device. */
static void
input_give(struct input_device *d, unsigned i)
{
	struct vio_sg sg = { d->events_pa + i * sizeof(struct input_event), sizeof(struct input_event), true };

	(void)vio_queue_add(d->queue, &sg, 1, &d->events[i]);
}

/* The device's thread: after an interrupt, or every 8 ms without one. */
static void
input_service(void *arg, __unused uint8_t isr)
{
	struct input_device *d = arg;
	struct input_event *e;
	bool returned = false;

	while ((e = vio_queue_get(d->queue, NULL)) != NULL) {
		input_event(d, e);
		input_give(d, (unsigned)(e - d->events));
		returned = true;
	}
	if (returned) {
		vio_queue_kick(d->queue);
	}
}

static bool
input_attach(struct input_device *d, vio_dev_t *dev)
{
	int error;

	d->dev = dev;
	if ((error = vio_init(dev, 0, NULL)) != 0) {
		printf("virtio-input: %s: features refused (%d)\n", vio_name(dev), error);
		return false;
	}
	/* queue 0 brings the events: hand the device all the buffers */
	if ((error = vio_queue_setup(dev, 0, QUEUE_SIZE, &d->queue)) != 0) {
		printf("virtio-input: %s: no event queue (%d)\n", vio_name(dev), error);
		vio_fail(dev);
		return false;
	}
	if ((d->events = vio_dma_alloc(QUEUE_SIZE * sizeof(struct input_event), &d->events_pa)) == NULL) {
		vio_fail(dev);
		return false;
	}
	for (unsigned i = 0; i < vio_queue_size(d->queue); i++) {
		input_give(d, i);
	}
	vio_driver_ok(dev);
	vio_queue_kick(d->queue);
	return true;
}

void
redwood_virtio_input_init(void)
{
	uint32_t enabled = 1;
	vio_dev_t *dev;

	if (PE_parse_boot_argn("virtio_input", &enabled, sizeof(enabled)) && !enabled) {
		return;
	}
	for (unsigned nth = 0; device_count < MAX_DEVICES && (dev = vio_find(VIRTIO_ID_INPUT, nth)) != NULL; nth++) {
		struct input_device *d = &devices[device_count];

		if (!input_attach(d, dev)) {
			continue;
		}
		device_count++;
		if (vio_service_start(dev, input_service, d, 8 * NSEC_PER_MSEC) != 0) {
			printf("virtio-input: %s: no thread\n", vio_name(dev));
			continue;
		}
		printf("virtio-input: device on %s%s\n", vio_name(dev), vio_intr_active(dev) ? ", interrupts" : ", polled");
	}
	if (device_count == 0) {
		printf("virtio-input: no input devices\n");
	}
}
