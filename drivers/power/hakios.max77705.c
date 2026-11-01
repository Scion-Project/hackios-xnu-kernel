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
 *
 * Boot-arg max77705_ipclk=MHz: the HSI2C's input clock, if step 1 says it is not the 200 MHz Android sets (the device
 * tree's default-clk); the bus's timing is worked out from it. (CMGP's mux, +0x1018 bit 0, picks the RCO oscillator at
 * 0 and CMGP's bus clock at 1; the divider is +0x1814 [3:0].)
 * The clocks forced on are the four of USI_CMGP03 in CMU_CMGP: GATE_CLK_USI_CMGP03 +0x2014, its IPCLK +0x20A8 and PCLK
 * +0x20AC, its RSTnSYNC +0x2074; its Q-channel +0x3050 (cal-if/exynos9810/cmucal-sfr.c of the S9's kernel).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern kern_return_t thread_terminate(thread_t thread);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);

/* (every line to the loader's log as well, as the other drivers do: see hakios.buttons.c) */
extern void loader_log_line(const char *line);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
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
	/* GATE_CLK_USI_CMGP03 (the divider's source), USI_CMGP03 IPCLK and PCLK, its RSTnSYNC: as the touch's bus */
	.gates = { 0x2014, 0x20A8, 0x20AC, 0x2074 }, .ngates = 4,
	.qch = 0x3050, .div = 0x1814, .mux = 0x1018,
	.pins_pa = 0,                           /* GPM14-0 and GPM15-0: the boot loader talks to the chip, they are set */
	.ipclk_mhz = 400, .bus_hz = 400000,       /* seen on the phone, 1 November: CMU divider 0 on the 400 MHz branch */
};

/* The bus is the battery's and the motor's (max77705_haptic=1): one transfer at a time, whoever asks. */
static lck_mtx_t *bus_lock;

static uint32_t
locked_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t len)
{
	uint32_t error;

	lck_mtx_lock(bus_lock);
	error = hakios_hsi2c_read_reg(&bus, addr, reg, buf, len);
	lck_mtx_unlock(bus_lock);
	return error;
}

static uint32_t
read16(uint8_t addr, uint8_t reg, uint16_t *value)
{
	uint8_t b[2];
	uint32_t error = locked_read(addr, reg, b, 2);

	if (error == 0) {
		*value = (uint16_t)(b[1] << 8 | b[0]);
	}
	return error;
}

static uint32_t
read8(uint8_t addr, uint8_t reg, uint8_t *value)
{
	return locked_read(addr, reg, value, 1);
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

/* ---- the motor: the MAX77705's haptic block, driven by a PWM of the SoC (max77705_haptic=1) ------------------------ */

/*
 * The S9's vibration motor is an LRA driven by the MAX77705's haptic block (0x66, the chip's "top"), its strength the
 * duty of a PWM from the SoC: channel 1 of pwm@10510000 at 20.48 kHz (period 48828 ns), which the block divides by 128
 * to drive the motor at 160 Hz; 50 % duty does not drive it, the further from 50 % the stronger, 70 % (34179 ns) as far
 * as Samsung goes. Set up once: the PWM's clock gate on (CMU_PERIC0 +0x2044, its Q-channel +0x3010), the PWM pin
 * (gpp3-1) to its function, the motor's boost (gpg1-7) on, channel 1 inverted and stopped, then in the chip
 * MAINCTRL1 (0x02) bit 7 (the bias) and MCONFIG (0x10) LRA (bit 7) with the divide-by-128 (bit 1), external PWM
 * (bit 5 clear), not enabled (bit 6). A buzz: the duty set, the channel started (auto-reload), MCONFIG bits 7 and 6
 * set; still: MCONFIG bits 7 and 6 clear, the duty back to 50 %, the channel stopped.
 *
 * THE CHIP IS THE PHONE'S CHARGER as well. Two registers are written and those only in the bits above (MAINCTRL1 0x80,
 * MCONFIG 0xC2), each change read back; one that does not read back ends the motor for the run.
 *
 * What iOS plays on the motor arrives as a waveform (the audio plug-in's actuator, /dev/hakios-haptic: samples of one
 * channel), as on the POCO (drivers/haptic/hakios.haptic.c, whose rules this follows): a write that is not silence keeps
 * the motor going for 30 ms; its loudness is the strength. A run of it is cut at a second. Facts from the phone's
 * device tree (/haptic, the PWM and its pins) and the S9 kernel's max77705_haptic.c, sec_haptic.c and pwm-samsung.c
 * (GPL: read, not copied).
 */
#define TOP                     0x66
#define MAINCTRL1               0x02
#define MCONFIG                 0x10
#define PWM_PA                  0x10510000UL
#define PWM_TCFG0               0x00
#define PWM_TCFG1               0x04
#define PWM_TCON                0x08
#define PWM_TCNTB1              0x18
#define PWM_TCMPB1              0x1C
#define TCON1_START             (1u << 8)
#define TCON1_UPDATE            (1u << 9)
#define TCON1_INVERT            (1u << 10)
#define TCON1_RELOAD            (1u << 11)
#define PWM_PERIOD_NS           48828u
#define PWM_MAX_DUTY_NS         34179u
#define OSC_HZ                  26000000u
#define CMU_PERIC0_PA           0x10400000UL
#define GPIO_PERIC0_PA          0x10430000UL
#define GPP3                    0x060           /* pin 1: the PWM's output (function 2) */
#define GPG1                    0x0A0           /* pin 7: the motor's boost, an output held high */
#define HAPTIC_ACTIVE_US        30000
#define HAPTIC_SILENT_BELOW     256
#define HAPTIC_MAX_MS           1000

static vm_offset_t pwm, peric0_gpio, peric0_cmu;
static uint32_t pwm_count;                      /* the period in the PWM's ticks */
static bool motor_broken, motor_on;
static volatile uint64_t heard_us;
static volatile uint32_t heard_peak;
static uint64_t motor_since_us;
static unsigned buzzes, capped;

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

/* a change of the haptic block's own bits, read back */
static bool
motor_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now = 0, back = 0, buf[2];
	bool ok;

	if (motor_broken || !((reg == MAINCTRL1 && (mask & ~0x80) == 0) || (reg == MCONFIG && (mask & ~0xC2) == 0))) {
		return false;
	}
	lck_mtx_lock(bus_lock);
	ok = hakios_hsi2c_read_reg(&bus, TOP, reg, &now, 1) == 0;
	if (ok) {
		buf[0] = reg;
		buf[1] = (uint8_t)((now & ~mask) | (value & mask));
		ok = hakios_hsi2c_message(&bus, TOP, false, buf, 2, true) == 0 &&
		    hakios_hsi2c_read_reg(&bus, TOP, reg, &back, 1) == 0 && back == buf[1];
	}
	lck_mtx_unlock(bus_lock);
	if (!ok) {
		say("max77705: haptic: register %#x did not take %#x (read %#x, then %#x): the motor stays still for this run\n",
		    reg, value & mask, now, back);
		motor_broken = true;
	}
	return ok;
}

static void
pwm_duty(uint32_t strength)                     /* 0 (50 %, still) to 10000 (70 %) */
{
	uint32_t duty_ns = PWM_PERIOD_NS / 2 + (PWM_MAX_DUTY_NS - PWM_PERIOD_NS / 2) * strength / 10000;
	uint32_t duty = (uint32_t)(((uint64_t)duty_ns * pwm_count + PWM_PERIOD_NS / 2) / PWM_PERIOD_NS);

	*(volatile uint32_t *)(pwm + PWM_TCNTB1) = pwm_count - 1;
	*(volatile uint32_t *)(pwm + PWM_TCMPB1) = pwm_count - duty - 1;
}

static void
motor_go(uint32_t strength)
{
	volatile uint32_t *tcon = (volatile uint32_t *)(pwm + PWM_TCON);

	pwm_duty(strength);
	if (!motor_on) {
		*tcon = (*tcon & ~TCON1_START) | TCON1_UPDATE | TCON1_INVERT;
		*tcon = (*tcon & ~TCON1_UPDATE) | TCON1_START | TCON1_RELOAD;
		if (!motor_bits(MCONFIG, 0xC0, 0xC0)) {
			*tcon &= ~(TCON1_START | TCON1_RELOAD);
			return;
		}
		motor_on = true;
		motor_since_us = now_us();
		buzzes++;
	}
}

static void
motor_still(void)
{
	volatile uint32_t *tcon = (volatile uint32_t *)(pwm + PWM_TCON);

	if (!motor_on) {
		return;
	}
	(void)motor_bits(MCONFIG, 0xC0, 0x00);
	pwm_duty(0);
	*tcon &= ~(TCON1_START | TCON1_RELOAD);
	motor_on = false;
}

/* From the audio server's I/O thread: never waits. */
static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		bytes = bytes > (user_ssize_t)sizeof(samples) ? (user_ssize_t)sizeof(samples) : bytes & ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		if (peak > HAPTIC_SILENT_BELOW) {
			heard_us = now_us();
			heard_peak = peak;
		}
	}
	return error;
}

static bool haptic_node_made;

static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw haptic_cdevsw = {
	.d_open = haptic_open, .d_close = haptic_open, .d_read = eno_rdwrt, .d_write = haptic_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static void
haptic_thread(__unused void *arg, __unused wait_result_t result)
{
	for (unsigned tick = 1;; tick++) {
		uint64_t now = now_us();
		bool wanted = heard_us != 0 && now - heard_us < HAPTIC_ACTIVE_US;

		if (wanted && motor_on && now - motor_since_us >= HAPTIC_MAX_MS * 1000ull) {
			capped++;
			motor_still();
			heard_us = 0;           /* (until iOS plays anew) */
		} else if (wanted && !motor_broken) {
			/* the strength from iOS's loudness, with a floor a tap can be felt at */
			uint32_t strength = heard_peak * 10000u / 32768u;

			motor_go(strength < 3000 ? 3000 : strength);
		} else {
			motor_still();
		}
		if (tick % (60 * 500) == 0) {
			say("max77705: haptic: a minute: %u buzzes, %u cut at %u ms\n", buzzes, capped, HAPTIC_MAX_MS);
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
}

/* the PWM, its pins and the haptic block set up; /dev/hakios-haptic made. In the battery's thread, after the bus. */
static void
haptic_start(void)
{
	uint32_t tcfg0, tcfg1, prescale, divider, tick_hz;
	thread_t thread;
	int major;

	pwm = ml_io_map(PWM_PA, 0x1000);
	peric0_gpio = ml_io_map(GPIO_PERIC0_PA, 0x1000);
	peric0_cmu = ml_io_map(CMU_PERIC0_PA, 0x4000);
	if (!pwm || !peric0_gpio || !peric0_cmu) {
		say("max77705: haptic: the PWM or its pins could not be mapped\n");
		return;
	}
	*(volatile uint32_t *)(peric0_cmu + 0x2044) |= (1u << 20) | (1u << 21);
	*(volatile uint32_t *)(peric0_cmu + 0x3010) |= 3u;
	{
		volatile uint32_t *con = (volatile uint32_t *)(peric0_gpio + GPP3);

		*con = (*con & ~(0xfu << 4)) | (2u << 4);                       /* gpp3-1: TOUT1 */
		con = (volatile uint32_t *)(peric0_gpio + GPG1);
		*(volatile uint32_t *)(peric0_gpio + GPG1 + 4) |= 1u << 7;      /* gpg1-7 high ... */
		*con = (*con & ~(0xfu << 28)) | (1u << 28);                     /* ... an output */
	}
	/* channel 1's clock as it is set (timers 0 and 1 share the prescaler: left alone), its own divider 1 */
	tcfg0 = *(volatile uint32_t *)(pwm + PWM_TCFG0);
	*(volatile uint32_t *)(pwm + PWM_TCFG1) &= ~(0xfu << 4);
	tcfg1 = *(volatile uint32_t *)(pwm + PWM_TCFG1);
	prescale = (tcfg0 & 0xff) + 1;
	divider = 1u << ((tcfg1 >> 4) & 0x7);
	tick_hz = OSC_HZ / prescale / divider;
	pwm_count = (uint32_t)(((uint64_t)PWM_PERIOD_NS * tick_hz + 500000000ull) / 1000000000ull);
	if (pwm_count < 16) {
		say("max77705: haptic: the PWM's tick (%u Hz) is too slow for its period: no motor\n", tick_hz);
		return;
	}
	pwm_duty(0);
	*(volatile uint32_t *)(pwm + PWM_TCON) = (*(volatile uint32_t *)(pwm + PWM_TCON) &
	    ~(TCON1_START | TCON1_RELOAD)) | TCON1_UPDATE | TCON1_INVERT;
	*(volatile uint32_t *)(pwm + PWM_TCON) &= ~TCON1_UPDATE;
	if (!motor_bits(MAINCTRL1, 0x80, 0x80) || !motor_bits(MCONFIG, 0xC2, 0x82)) {
		return;
	}
	if (!haptic_node_made) {
		say("max77705: haptic: /dev/hakios-haptic was not made at start\n");
		return;
	}
	(void)major;
	say("max77705: haptic: the motor for iOS (/dev/hakios-haptic): PWM %u ticks of %u Hz a period, up to %u ms a run\n",
	    pwm_count, tick_hz, HAPTIC_MAX_MS);
	if (kernel_thread_start(haptic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
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
	{
		/* the bus's pins: GPM14-0 (SCL) and GPM15-0 (SDA) of CMGP's pin controller (0x14220000; banks at +0x180/+0x1A0,
		 * CON +0, DAT +4, PUD +8): function 2, no pull (the device tree's hsi2c40-bus) */
		vm_offset_t g = ml_io_map(0x14220000, 0x1000);

		if (g != 0) {
			for (uint32_t b = 0x180; b <= 0x1A0; b += 0x20) {
				volatile uint32_t *con = (volatile uint32_t *)(g + b), *pud = (volatile uint32_t *)(g + b + 8);
				uint32_t c0 = *con, p0 = *pud;

				*con = (c0 & ~0xfu) | 2;
				*pud = p0 & ~0xfu;
				say("max77705: pins: bank +%#x CON %x -> %x, PUD %x -> %x, DAT %x\n", b, c0, *con, p0, *pud,
				    *(volatile uint32_t *)(g + b + 4));
			}
		}
	}
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
	/* (a first answer missed is not the end: step 5 goes on asking, and reports once one comes) */
	if (read8(PMIC, 0x01, &rev) != 0) {
		say("max77705: no answer at 0x%x (%x)\n", PMIC, bus.last_error);
	} else {
		say("max77705: version %u, revision %u\n", rev >> 3, rev & 7);
	}
	if (!battery_read(&b)) {
		say("max77705: the gauge or the charger did not answer (%x)\n", bus.last_error);
	} else {
		say_state("now", &b);
	}
	{
		/*
		 * max77705_usbsw=1: the USB-C switch's D+/D- to the AP's USB (Samsung's com_to_usb_ap: the MUIC at 0x25, opcode
		 * COMMAND_CONTROL1_WRITE 0x06 with COM_USB 0x09, written to OPCODE_WRITE 0x21, then OPCODE_WRITE_END 0x41 = 0).
		 */
		uint32_t usbsw = 0;

		if (PE_parse_boot_argn("max77705_usbsw", &usbsw, sizeof(usbsw)) && usbsw != 0) {
			uint8_t w1[3] = { 0x21, 0x06, 0x09 }, w2[2] = { 0x41, 0x00 };
			uint32_t e1 = hakios_hsi2c_message(&bus, 0x25, false, w1, 3, true);
			uint32_t e2 = e1 ? e1 : hakios_hsi2c_message(&bus, 0x25, false, w2, 2, true);

			say("max77705: usb switch: CONTROL1 = 0x09 (D+/D- to the AP's USB): %s (%x)\n", e2 ? "FAILED" : "written", e2);
		}
	}
	if (++step >= steps) {
		goto out;
	}
	{
		uint32_t haptic = 0;

		if (PE_parse_boot_argn("max77705_haptic", &haptic, sizeof(haptic)) && haptic != 0) {
			haptic_start();
		}
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
	PE_parse_boot_argn("max77705_ipclk", &bus.ipclk_mhz, sizeof(bus.ipclk_mhz));
	if (steps == 0 || bus.ipclk_mhz < 26 || bus.ipclk_mhz > 400) {
		return;
	}
	bus_lock = lck_mtx_alloc_init(lck_grp_alloc_init("max77705", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	{
		/* /dev/hakios-haptic made now, before user space starts: the audio server opens it once, at its first sound,
		 * and keeps "not there" for good if it was not there yet (seen on the phone: the motor came up later, in the
		 * battery's thread). Writes before the motor is ready are only listened to. */
		uint32_t haptic = 0;

		if (PE_parse_boot_argn("max77705_haptic", &haptic, sizeof(haptic)) && haptic != 0) {
			int major = cdevsw_add(-1, &haptic_cdevsw);

			haptic_node_made = major >= 0 &&
			    devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") != NULL;
		}
	}
	if (kernel_thread_start(battery_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
