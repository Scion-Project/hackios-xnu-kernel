/*
 * The keys of the Galaxy S9 (starlte, Exynos 9810) -- power, volume up, volume down, Bixby -- as a source of
 * button records for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg exynos_keys=1: every address
 * below belongs to this phone.
 *
 * All four are pins of the SoC's ALIVE GPIO block (pinctrl@14050000 in Samsung's device tree), each pulled up
 * on the board and pulled to ground by its key, so a pin reads 0 while its key is held:
 *   volume up    gpa0 pin 3     DAT 0x14050024 bit 3
 *   volume down  gpa0 pin 4     DAT 0x14050024 bit 4
 *   Bixby        gpa0 pin 6     DAT 0x14050024 bit 6
 *   power        gpa2 pin 4     DAT 0x14050064 bit 4
 * A bank of that block is CON (+0x00, a nibble per pin: 0 input, 0xF its wakeup interrupt), DAT (+0x04, a bit
 * per pin), PUD (+0x08), DRV (+0x0C), banks 0x20 apart, gpa0 at 0x20 and gpa2 at 0x60. DAT says the pin's level
 * whether the pin is an input or an interrupt (Android's own driver reads it with the pins set to 0xF).
 * Sources: the phone's device tree (gpio_keys, gpa0/gpa2 under pinctrl@14050000, the key-* pin settings),
 * Linux's drivers/pinctrl/samsung/pinctrl-exynos.c (the ALIVE banks of the Exynos 9810) and Project Silicium's
 * starlte UEFI, which reads the same four bits the same way (KeypadDeviceLib.c) and sets nothing up first.
 *
 * THIS DRIVER ONLY READS. The firmware has left the pins as Android wants them; a pin found set to anything but
 * an input or its interrupt is not taken for a key, and said so.
 *
 * What a press is: a thread reads the two DAT registers 100 times a second, and a key has changed when two looks
 * in a row say so (Android debounces for 5 ms). A key that reads as held before it has ever been seen released
 * is not believed until it has been.
 *
 * The mapping: the iPhone X, whose user space runs here, has a side button and two volume keys. Power is the
 * side button (HAKIOS_BUTTON_LOCK); Bixby, where the iPhone has nothing, is the home button (HAKIOS_BUTTON_HOME),
 * which is a way home while there is no touch screen.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/* (every line to the loader's log as well, as the other drivers do: see hakios.buttons.c) */
extern void loader_log_line(const char *line);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

#define ALIVE_PA        0x14050000UL    /* pinctrl@14050000: the ALIVE GPIO block */
#define ALIVE_SIZE      0x1000
#define BANK_GPA0       0x020
#define BANK_GPA2       0x060
#define BANK_CON        0x00
#define BANK_DAT        0x04

#define POLL_MS         10
#define MINUTE_TICKS    (60 * 1000 / POLL_MS)

static struct key {
	const char *name;
	uint32_t bank, pin;
	uint32_t button;                /* HAKIOS_BUTTON_* */
	bool usable;                    /* the pin is an input or its interrupt */
	bool live;                      /* seen released: what it says is believed */
	bool held;                      /* as last reported */
	bool last;                      /* the previous look */
	uint32_t presses;
} keys[] = {
	{ "power", BANK_GPA2, 4, HAKIOS_BUTTON_LOCK },
	{ "volume-up", BANK_GPA0, 3, HAKIOS_BUTTON_VOLUME_UP },
	{ "volume-down", BANK_GPA0, 4, HAKIOS_BUTTON_VOLUME_DOWN },
	{ "bixby", BANK_GPA0, 6, HAKIOS_BUTTON_HOME },
};
#define NKEYS (sizeof(keys) / sizeof(keys[0]))

static vm_offset_t alive;

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(alive + off);
}

/* held: the pin at 0 */
static bool
key_held(const struct key *k, uint32_t gpa0, uint32_t gpa2)
{
	return (((k->bank == BANK_GPA0 ? gpa0 : gpa2) >> k->pin) & 1) == 0;
}

static void
keys_poll(void)
{
	uint32_t gpa0 = rd(BANK_GPA0 + BANK_DAT), gpa2 = rd(BANK_GPA2 + BANK_DAT);

	for (unsigned i = 0; i < NKEYS; i++) {
		struct key *k = &keys[i];
		bool now = key_held(k, gpa0, gpa2);

		if (!k->usable) {
			continue;
		}
		if (now != k->last) {
			k->last = now;          /* one look: wait for a second that agrees */
			continue;
		}
		if (!k->live) {
			if (!now) {
				k->live = true;
				say("exynos-keys: %s seen released: watching it\n", k->name);
			}
			continue;
		}
		if (now != k->held) {
			k->held = now;
			if (now) {
				k->presses++;
			}
			hakios_touch_post(now ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, k->button, 0, 0);
			say("exynos-keys: %s %s\n", k->name, now ? "down" : "up");
		}
	}
}

static void
keys_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t gpa0, gpa2;

	alive = ml_io_map(ALIVE_PA, ALIVE_SIZE);
	if (alive == 0) {
		say("exynos-keys: cannot map the ALIVE GPIO block at 0x%lx\n", ALIVE_PA);
		thread_terminate(current_thread());
		return;
	}
	gpa0 = rd(BANK_GPA0 + BANK_DAT);
	gpa2 = rd(BANK_GPA2 + BANK_DAT);
	for (unsigned i = 0; i < NKEYS; i++) {
		struct key *k = &keys[i];
		uint32_t con = (rd(k->bank + BANK_CON) >> (4 * k->pin)) & 0xf;

		k->usable = con == 0x0 || con == 0xf;
		k->last = key_held(k, gpa0, gpa2);
		k->live = k->usable && !k->last;
		say("exynos-keys: %s: bank 0x%x pin %u, CON %x (%s), %s\n", k->name, k->bank, k->pin, con,
		    con == 0 ? "input" : con == 0xf ? "interrupt" : "SOMETHING ELSE: not a key here, left alone",
		    k->last ? "reads as held: believed once it has been seen released" : "released");
	}
	say("exynos-keys: watching every %u ms: power is the side button, Bixby the home button\n", POLL_MS);
	for (unsigned tick = 1;; tick++) {
		keys_poll();
		if (tick % MINUTE_TICKS == 0) {
			say("exynos-keys: a minute: presses power %u, volume-up %u, volume-down %u, bixby %u\n",
			    keys[0].presses, keys[1].presses, keys[2].presses, keys[3].presses);
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
}

void
hakios_exynos_keys_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("exynos_keys", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	if (kernel_thread_start(keys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
