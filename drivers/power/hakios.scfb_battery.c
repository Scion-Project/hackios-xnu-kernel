/*
 * The battery under AVF (patch 1403): the phone's own, as Android sees it, for iOS's battery gauge.
 *
 * The Scion app writes Android's battery state (BatteryManager: the level, charging, the charger,
 * voltage, current, temperature, charge counter, cycles) into a block of the shared framebuffer's
 * header page (SCFB, scfb.addr=, patch 1200) at SCFB_BATTERY, under a sequence count that is odd
 * while it writes, every time Android says it changed and every 10 s besides. A thread here looks
 * every 2 s and hands a new state to hakios_battery_report() (bsd/dev/hakios.battery.c), which makes
 * and updates iOS's power source (iokit/Kernel/hakios.battery.cpp): the status bar's battery, the
 * lock screen's "Charging", Settings > Battery.
 *
 * Nothing is done with battery=1 (the POCO's charger cache has the battery then) or without scfb.addr=.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define SCFB_MAGIC      0x42464353u     /* "SCFB" (patch 1200) */
#define SCFB_BATTERY    0x100           /* the battery block in the header page */
#define BATTERY_MAGIC   0x54544142u     /* "BATT": the app has written it */

/* At SCFB_BATTERY; the app's (avf/ScfbBattery.kt). The numbers are struct hakios_battery_state's. */
struct scfb_battery {
	uint32_t magic;
	uint32_t seq;                           /* odd while the app writes */
	int32_t percent_hundredths, voltage_uv, current_ua, temperature_hundredths;
	int32_t present, external, status;      /* status: Linux's POWER_SUPPLY_STATUS_* */
	int32_t charge_uah, full_uah, design_uah, cycles;
	int32_t usb_type, usb_voltage_uv, usb_current_ua;
};

static volatile struct scfb_battery *block;

/* A whole state, not one the app is half way through: false if none could be had. */
static bool
snapshot(struct scfb_battery *out)
{
	for (int tries = 0; tries < 4; tries++) {
		uint32_t before = __atomic_load_n((uint32_t *)(uintptr_t)&block->seq, __ATOMIC_ACQUIRE);
		if (before & 1) {
			delay_for_interval(1, NSEC_PER_MSEC);
			continue;
		}
		/* (word by word: the page is mapped as a device's, where wider or unaligned loads fault) */
		for (size_t i = 0; i < sizeof(*out) / 4; i++) {
			((uint32_t *)out)[i] = ((volatile const uint32_t *)block)[i];
		}
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n((uint32_t *)(uintptr_t)&block->seq, __ATOMIC_ACQUIRE) == before) {
			out->seq = before;
			return out->magic == BATTERY_MAGIC;
		}
	}
	return false;
}

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t seen = 1;              /* (odd: nothing seen yet) */
	unsigned said = 0;

	for (;;) {
		struct scfb_battery b;

		if (snapshot(&b) && b.seq != seen) {
			struct hakios_battery_state s = {
				.size = sizeof(s),
				.percent_hundredths = b.percent_hundredths,
				.voltage_uv = b.voltage_uv,
				.current_ua = b.current_ua,
				.temperature_hundredths = b.temperature_hundredths,
				.present = b.present,
				.external = b.external,
				.status = b.status,
				.charge_uah = b.charge_uah,
				.full_uah = b.full_uah,
				.design_uah = b.design_uah,
				.cycles = b.cycles,
				.usb_type = b.usb_type,
				.usb_voltage_uv = b.usb_voltage_uv,
				.usb_current_ua = b.usb_current_ua,
			};
			if (said++ == 0) {
				printf("battery: the phone's, from the Scion app: %d.%02d%%, %s\n", b.percent_hundredths / 100,
				    b.percent_hundredths % 100, b.external ? (b.status == 1 ? "charging" : "on a charger") : "on battery");
			}
			seen = b.seq;
			hakios_battery_report(&s);
		}
		delay_for_interval(2000, NSEC_PER_MSEC);
	}
}

void
hakios_scfb_battery_init(void)
{
	uint64_t pa = 0;
	uint32_t battery = 0;
	vm_offset_t page;
	thread_t thread;

	if (PE_parse_boot_argn("battery", &battery, sizeof(battery)) && battery != 0) {
		return;
	}
	if (!PE_parse_boot_argn("scfb.addr", &pa, sizeof(pa)) || pa == 0 || (pa & PAGE_MASK) != 0) {
		return;
	}
	/* (uncached, as the display's driver maps this page: what the app wrote is always what is read) */
	page = ml_io_map((vm_offset_t)pa, PAGE_SIZE);
	if (page == 0 || *(volatile uint32_t *)page != SCFB_MAGIC) {
		printf("battery: no shared framebuffer header at %#llx: no battery for iOS\n", pa);
		return;
	}
	block = (volatile struct scfb_battery *)(page + SCFB_BATTERY);
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
