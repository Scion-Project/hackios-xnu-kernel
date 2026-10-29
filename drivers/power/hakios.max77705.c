/*
 * The Galaxy S9's battery (starlte, Exynos 9810): its fuel gauge and charger, both blocks of a Maxim MAX77705 on the I2C
 * of USI03 of the CMGP block (HSI2C at 0x14360000), read every few seconds and handed to iOS's power source through
 * hakios_battery_report() (drivers/power/hakios.battery.c). Only with boot-arg max77705=1: every address below belongs
 * to this phone.
 *
 * THIS DRIVER ONLY READS THE CHIP. The MAX77705 is the phone's charger: a wrong write there changes how the battery is
 * charged. Nothing is written to it, not even the latches its interrupt handler would clear on Android (the interrupt
 * registers are not read at all: reading them clears them). What is written is the bus's own: the clock gates of
 * CMGP's USI03, its mode word, the controller (drivers/core/hakios.exynos_hsi2c.c).
 *
 *   the fuel gauge, 0x36: 16-bit registers, little-endian
 *     0x00 status (bit 3: no battery), 0x05 RepCap and 0x35 FullCapRep (1 mAh), 0x06 RepSOC (1/256 %),
 *     0x09 VCELL (78.125 uV), 0x0A current (signed, 1.5625 uV over the 5 mOhm sense resistor: 312.5 uA, more than 0
 *     while charging), 0x17 cycles (1/100)
 *   the charger, 0x69: 0xB2 CHG_INT_OK (bit 6 the USB input, bit 5 wireless), 0xB4 DTLS_01 ([3:0]: 0-2 charging,
 *     3-4 full, 5-7 not charging, 8 and up off)
 *   the chip, 0x66: 0x01 its revision (bits 2:0) and version (7:3)
 * The gauge learns the battery on its own and keeps that while the battery is connected (it is not removable); no model
 * is loaded by the host on Android either. The charge iOS sees is RepSOC; Android's own percentage is the same scaled to
 * a learnt maximum, so near full the two may differ by some percent. The temperature is not the gauge's (Android writes
 * an ADC thermistor's reading into it): until that ADC is read here, the battery is said to be at 25 C.
 * Sources: the phone's device tree (hsi2c@14360000, max77705 at 0x66, the fuel gauge's settings: fg_resistor 2, capacity
 * 2967 mAh), Linux's drivers/battery_v2/max77705_fuelgauge.c and _charger.c and drivers/mfd/max77705.c of the S9's kernel
 * (GPL: read for these facts, not copied).
 *
 * The bring-up is made in steps, each said in the log before it is made. Boot-arg max77705_steps=N stops after the
 * first N:
 *   1  map the bus's blocks and read what they are set to   (nothing written)
 *   2  the bus's clocks on and USI03 to I2C                  (the first writes)
 *   3  the bus reset and set to 400 kHz
 *   4  read the chip's revision, the gauge and the charger once
 *   5  report every 5 seconds (the default)
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern kern_return_t thread_terminate(thread_t thread);

/* (every line to the loader's log as well, as the other drivers do: see hakios.buttons.c) */
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

#define PMIC            0x66
#define CHARGER         0x69
#define GAUGE           0x36

#define FG_STATUS       0x00
#define FG_REPCAP       0x05
#define FG_REPSOC       0x06
#define FG_VCELL        0x09
#define FG_CURRENT      0x0A
#define FG_CYCLES       0x17
#define FG_FULLCAPREP   0x35
#define CHG_INT_OK      0xB2
#define CHG_DTLS_01     0xB4

#define DESIGN_MAH      2967            /* the device tree's fuelgauge,capacity */
#define POLL_MS         5000

static struct hakios_hsi2c bus = {
	.name = "max77705",
	.regs_pa = 0x14360000,                  /* hsi2c@14360000: USI03 of CMGP */
	.usi_mode_pa = 0x14212030,              /* usi@14212030: its mode word */
	.cmu_pa = 0x14200000, .cmu_size = 0x4000,       /* CMU_CMGP */
	.gates = { 0x20A8, 0x20AC }, .ngates = 2,
	.qch = 0x3050, .div = 0x1814, .mux = 0x1018,
	.pins_pa = 0,                           /* GPM14-0 and GPM15-0: the boot loader talks to the chip, they are set */
	.ipclk_mhz = 200, .bus_hz = 400000,
};

static uint32_t
read16(uint8_t addr, uint8_t reg, uint16_t *value)
{
	uint8_t b[2];
	uint32_t error = hakios_hsi2c_read_reg(&bus, addr, reg, b, 2);

	if (error == 0) {
		*value = (uint16_t)(b[1] << 8 | b[0]);
	}
	return error;
}

static uint32_t
read8(uint8_t addr, uint8_t reg, uint8_t *value)
{
	return hakios_hsi2c_read_reg(&bus, addr, reg, value, 1);
}

/* the gauge and the charger as one state; false if a read failed */
static bool
battery_read(struct hakios_battery_state *b)
{
	uint16_t status, soc, vcell, cap, full, cycles;
	int16_t current;
	uint8_t ok, dtls;

	if (read16(GAUGE, FG_STATUS, &status) || read16(GAUGE, FG_REPSOC, &soc) || read16(GAUGE, FG_VCELL, &vcell) ||
	    read16(GAUGE, FG_CURRENT, (uint16_t *)&current) || read16(GAUGE, FG_REPCAP, &cap) ||
	    read16(GAUGE, FG_FULLCAPREP, &full) || read16(GAUGE, FG_CYCLES, &cycles) ||
	    read8(CHARGER, CHG_INT_OK, &ok) || read8(CHARGER, CHG_DTLS_01, &dtls)) {
		return false;
	}
	memset(b, 0, sizeof(*b));
	b->size = sizeof(*b);
	b->percent_hundredths = (int32_t)((uint32_t)soc * 100 / 256);
	if (b->percent_hundredths > 10000) {
		b->percent_hundredths = 10000;
	}
	b->voltage_uv = (int32_t)((uint64_t)vcell * 78125 / 1000);
	b->current_ua = (int32_t)current * 3125 / 10;
	b->temperature_hundredths = 2500;               /* (not the gauge's: see the top of this file) */
	b->present = !(status & (1u << 3));
	b->external = (ok & (3u << 5)) != 0;           /* the USB input or the wireless one */
	switch (dtls & 0xf) {
	case 0: case 1: case 2:
		b->status = 1;                          /* charging */
		break;
	case 3: case 4:
		b->status = 4;                          /* full */
		break;
	default:
		b->status = b->external ? 3 : 2;        /* not charging, discharging */
		break;
	}
	b->charge_uah = (int32_t)cap * 1000;
	b->full_uah = (int32_t)full * 1000;
	b->design_uah = DESIGN_MAH * 1000;
	b->cycles = cycles / 100;
	b->usb_type = b->external ? 2 : 0;
	return true;
}

static void
say_state(const char *when, const struct hakios_battery_state *b)
{
	say("max77705: %s: %d.%02d%%, %d mV, %d mA, %s, %s, %d of %d mAh (full %d), %d cycles\n", when,
	    b->percent_hundredths / 100, b->percent_hundredths % 100, b->voltage_uv / 1000, b->current_ua / 1000,
	    b->present ? "a battery" : "NO BATTERY", b->status == 1 ? "charging" : b->status == 4 ? "full" :
	    b->status == 3 ? "on a charger, not charging" : "discharging", b->charge_uah / 1000, b->design_uah / 1000,
	    b->full_uah / 1000, b->cycles);
}

static void
battery_thread(void *arg, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg, step = 0, failed = 0;
	struct hakios_battery_state b;
	uint8_t rev;

	say("max77705: step 1: map the HSI2C, the CMGP clocks and USI03's mode word\n");
	if (!hakios_hsi2c_map(&bus, say) || ++step >= steps) {
		goto out;
	}
	say("max77705: step 2: the clocks of CMGP's USI03 on, USI03 to I2C\n");
	hakios_hsi2c_power(&bus);
	if (++step >= steps) {
		goto out;
	}
	say("max77705: step 3: the bus reset, at %u kHz from %u MHz\n", bus.bus_hz / 1000, bus.ipclk_mhz);
	hakios_hsi2c_reset(&bus);
	if (++step >= steps) {
		goto out;
	}
	say("max77705: step 4: the chip, the gauge, the charger\n");
	if (read8(PMIC, 0x01, &rev) != 0) {
		say("max77705: no answer at 0x%x (%x)\n", PMIC, bus.last_error);
		goto out;
	}
	say("max77705: version %u, revision %u\n", rev >> 3, rev & 7);
	if (!battery_read(&b)) {
		say("max77705: the gauge or the charger did not answer (%x)\n", bus.last_error);
		goto out;
	}
	say_state("now", &b);
	if (++step >= steps) {
		goto out;
	}
	say("max77705: step 5: reporting every %u s\n", POLL_MS / 1000);
	for (unsigned tick = 0;; tick++) {
		if (battery_read(&b)) {
			failed = 0;
			hakios_battery_report(&b);
			if (tick % 60 == 0) {
				say_state("every 5 minutes", &b);
			}
		} else if (++failed == 10) {
			say("max77705: 10 reads in a row have failed (%x); still trying\n", bus.last_error);
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
out:
	if (steps < 5) {
		say("max77705: stopping here (max77705_steps=%u)\n", steps);
	}
	thread_terminate(current_thread());
}

void
hakios_max77705_init(void)
{
	uint32_t enabled = 0, steps = 5;
	thread_t thread;

	if (!PE_parse_boot_argn("max77705", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	PE_parse_boot_argn("max77705_steps", &steps, sizeof(steps));
	if (steps == 0) {
		return;
	}
	if (kernel_thread_start(battery_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
