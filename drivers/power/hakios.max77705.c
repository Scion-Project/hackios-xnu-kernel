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
 * an ADC thermistor's reading into it): it is the thermistor's, channel 4 of the CMGP block's ADC (0x14230000, 12 bits),
 * read as Android does (five conversions, the highest and the lowest dropped) and turned into degrees by the phone's own
 * table (battery,temp_table_adc and _data in its device tree). The ADC: CON1 0x00 (bit 2 reset, bit 1 out of reset,
 * bit 0 start), CON2 0x04 ([6:4] conversion time, [3:0] the channel), DAT 0x08 [11:0], INT_EN 0x10, INT_ST 0x14 (bit 0
 * done, 1 clears it); its clock gates CMU_CMGP +0x2018 and +0x201C, its Q-channels +0x3008 and +0x300C.
 * Sources: the phone's device tree (hsi2c@14360000, max77705 at 0x66, the fuel gauge's settings: fg_resistor 2, capacity
 * 2967 mAh; adc@14230000, the temperature table), Linux's drivers/battery_v2/max77705_fuelgauge.c, _charger.c, sec_adc.c,
 * drivers/mfd/max77705.c and drivers/iio/adc/exynos_adc.c of the S9's kernel (GPL: read for these facts, not copied).
 *
 * The bring-up is made in steps, each said in the log before it is made. Boot-arg max77705_steps=N stops after the
 * first N:
 *   1  map the bus's blocks and read what they are set to   (nothing written)
 *   2  the bus's clocks on and USI03 to I2C, the ADC's on    (the first writes)
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
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);

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

#define ADC_PA          0x14230000UL
#define ADC_CON1        0x00
#define ADC_CON2        0x04
#define ADC_DAT         0x08
#define ADC_INT_EN      0x10
#define ADC_INT_ST      0x14
#define ADC_CHANNEL     4               /* the battery's thermistor (sec-battery's adc-temp) */
static const uint32_t adc_gates[] = { 0x2018, 0x201C }, adc_qch[] = { 0x3008, 0x300C };

/* the device tree's battery,temp_table_adc and _data: the ADC's reading, and tenths of a degree */
static const uint16_t temp_adc[] = { 275, 327, 385, 451, 535, 626, 737, 864, 1013, 1210, 1361, 1554, 1769, 2007, 2239,
	                             2476, 2702, 2924, 3116, 3294, 3446, 3564, 3676 };
static const int16_t temp_data[] = { 900, 850, 800, 750, 700, 650, 600, 550, 500, 450, 400, 350, 300, 250, 200, 150, 100,
	                             50, 0, -50, -100, -150, -200 };
static vm_offset_t adc;
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

static inline uint32_t
adc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(adc + off);
}

static inline void
adc_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(adc + off) = v;
}

/* the ADC's clocks on (in CMU_CMGP, which the bus has mapped), reset, its conversion time set */
static void
adc_init(void)
{
	adc = ml_io_map(ADC_PA, 0x1000);
	if (!adc || !bus.cmu) {
		adc = 0;
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		*(volatile uint32_t *)(bus.cmu + adc_gates[i]) |= (1u << 20) | (1u << 21);
		*(volatile uint32_t *)(bus.cmu + adc_qch[i]) |= 3;
	}
	adc_wr(ADC_CON1, 1u << 2);
	adc_wr(ADC_CON1, 1u << 1);
	adc_wr(ADC_CON2, 6u << 4);
	adc_wr(ADC_INT_EN, 1);
}

/* one conversion of the thermistor's channel; -1 if it does not finish */
static int
adc_convert(void)
{
	adc_wr(ADC_CON2, (adc_rd(ADC_CON2) & ~0xfu) | ADC_CHANNEL);
	adc_wr(ADC_CON1, adc_rd(ADC_CON1) | 1);
	for (int waited = 0; waited < 10000; waited++) {
		if (adc_rd(ADC_INT_ST) & 1) {
			int value = (int)(adc_rd(ADC_DAT) & 0xfff);

			adc_wr(ADC_INT_ST, 1);
			return value;
		}
		delay(1);
	}
	return -1;
}

/* the battery's temperature in hundredths of a degree, or 2500 (25 C) when the ADC does not answer */
static int32_t
battery_temperature(int *raw_out)
{
	int sample[5], low = 0, high = 0, sum = 0, raw;
	unsigned n = sizeof(temp_adc) / sizeof(temp_adc[0]);

	*raw_out = -1;
	if (!adc) {
		return 2500;
	}
	for (int i = 0; i < 5; i++) {
		if ((sample[i] = adc_convert()) < 0) {
			return 2500;
		}
		low = (i == 0 || sample[i] < sample[low]) ? i : low;
		high = (i == 0 || sample[i] > sample[high]) ? i : high;
	}
	for (int i = 0; i < 5; i++) {
		sum += (i == low || i == high) ? 0 : sample[i];
	}
	raw = sum / (low == high ? 4 : 3);
	*raw_out = raw;
	if (raw <= temp_adc[0]) {
		return temp_data[0] * 10;
	}
	if (raw >= temp_adc[n - 1]) {
		return temp_data[n - 1] * 10;
	}
	for (unsigned i = 1; i < n; i++) {
		if (raw <= temp_adc[i]) {
			/* between two points of the table, on the line through them (tenths of a degree, to hundredths) */
			return (temp_data[i - 1] * 10) + (temp_data[i] - temp_data[i - 1]) * 10 * (raw - temp_adc[i - 1]) /
			       (temp_adc[i] - temp_adc[i - 1]);
		}
	}
	return 2500;
}

static int last_adc = -1;              /* the thermistor's last reading, for the log */

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
	b->temperature_hundredths = battery_temperature(&last_adc);    /* (the thermistor's: see the top of this file) */
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
	say("max77705: %s: %d.%02d%%, %d mV, %d mA, %d.%d C (ADC %d), %s, %s, %d of %d mAh (full %d), %d cycles\n", when,
	    b->percent_hundredths / 100, b->percent_hundredths % 100, b->voltage_uv / 1000, b->current_ua / 1000,
	    b->temperature_hundredths / 100, (b->temperature_hundredths < 0 ? -b->temperature_hundredths : b->temperature_hundredths) % 100 / 10,
	    last_adc,
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
	say("max77705: step 2: the clocks of CMGP's USI03 on, USI03 to I2C; the ADC's clocks on, the ADC reset\n");
	hakios_hsi2c_power(&bus);
	adc_init();
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
