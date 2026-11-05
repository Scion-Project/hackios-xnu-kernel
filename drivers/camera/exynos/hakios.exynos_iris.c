#define HAKIOS_SOC_FIXED
/*
 * Iris camera on the Galaxy S9 (starlte, Exynos 9810): the Samsung S5K5F1 infrared sensor (fimc-is-secure@10) and
 * the S2MPB02's infrared LED channel. Powers the sensor from the S2MPS18's LDOs, streams one raw RAW8 frame of
 * 2400 x 2400 into a reserved buffer, and can send a grey picture of it on the USB console. The ISP's libraries
 * are not used. The sensor link is CSIS3 and the ISPPRE block is shared with the front camera (CSIS1), driven in
 * hakios.exynos_camera.c.
 *
 * Depends on: hakios.exynos_acpm.c (S2MPS18 LDOs), sys/hakios.exynos_hsi2c.h (I2C masters), sys/hakios.physmap.h,
 * sys/hakios.exynos9810_mem.h (frame buffer), hakios.dwc3.c (irshot), and the ISPPRE domain steps in
 * hakios.exynos_camera.c.
 *
 * Boot-args. Nothing is written unless iris_s9 or fp_s9 is nonzero.
 *   iris_s9=N          run the first N steps: 1 read the pins, CIS_CLK2, LDOs and the LED's ID; 2 power the sensor,
 *                      read its ID, version and OTP, power down; 3 set the LED registers and read them back; 4 take
 *                      one frame; 5 stay for the USB console's "irshot [N]". Refused with a nonzero cam_s9= or torch=,
 *                      which own the S2MPB02 bus and the ISPPRE domain (remove the S9's default cam_s9=7).
 *   iris_s9_exp=LINES  exposure in lines of 6.56 us (default 0x0339 = 825, at most 5075)
 *   iris_s9_gain=CODE  analog gain, 0x20 is 1x (default 0x20, range 0x20 to 0x200)
 *   iris_s9_ms=MS      longest a capture may stream (default 600, range 100 to 2000)
 *   iris_s9_smc=M      secure monitor calls around the run: 0 none, 1 PREPARE, 2 also holds ISPPRE, 3 also SYSREG_PROT
 *   iris_s9_ispdiv=D   CLKCMU_ISPPRE_BUS = 466 MHz / (D + 1) while the frame is taken (default 0, range 0 to 3)
 *   fp_s9=N            read-only probe of the rear fingerprint sensor's SPI1 (1); 2 also writes two registers and
 *                      restores them. Runs with or without iris_s9.
 *
 * Limits. The S2MPS18 is written only in the control registers of LDO39 to LDO41. The S2MPB02 is written only in
 * the seven LED registers listed below. A write that does not read back, or a missing acknowledgement, powers
 * everything down and writes nothing more in that run.
 *
 * Eye safety. The sensor streams only during one capture, for at most iris_s9_ms, and is stopped on every exit
 * path, including loader_return. Captures are at least 2 s apart and a run makes at most 40. The LED cannot be on
 * without the sensor streaming.
 *
 * Sources for the values: the phone's device tree (exynos9810-starlte_eur_open_26.dts of the BSP) and the S9
 * kernel's drivers: fimc-is-device-module-5f1.c, fimc-is-hw-csi-v5_1.c, leds-s2mpb02.c, s2mps18 driver and headers,
 * the 9810 clock and pinctrl tables (android_kernel_samsung_exynos9810, lineage-24.0, e5906667). GPL sources, read
 * for the facts only.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.exynos9810_mem.h>
#include <sys/hakios.input.h>
#include <sys/hakios.physmap.h>
#include <sys/systm.h>
#include <sys/hakios.exynos_soc.h>
#include <sys/hakios.xnu.h>

static bool leaving;

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[512];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	if (!leaving) {
		printf("%s", line);
	}
	return length;
}

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

/* a pause: slept when it may be, spun on the way out */
static void
pause_us(uint32_t us)
{
	if (leaving || us < 1000) {
		spin_us(us);
	} else {
		delay_for_interval(us, NSEC_PER_USEC);
	}
}

/* (reg & mask) == want within us microseconds, by the clock */
static bool
wait_for(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t want, uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	for (;;) {
		if ((rd(base, off) & mask) == want) {
			return true;
		}
		if (mach_absolute_time() > until) {
			return (rd(base, off) & mask) == want;
		}
		pause_us(leaving ? 10 : 1000);
	}
}

static void
cache_flush(vm_offset_t start, uint32_t bytes)
{
	for (vm_offset_t line = start; line < start + bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
	}
	__asm__ volatile ("dsb sy" : : : "memory");
}

/*
 * The sensor's bus: hsi2c@108D0000, USI07 of PERIC1 ("hsi2c19"), 400 kHz, 7-bit address 0x10. Its clock gates
 * (GATE_USI07_I2C), Q-channel and divider, and its pins (gpp4-6 SCL, gpp4-7 SDA, PERIC1 GPIO function 3 while this
 * driver holds them, function 0 when off) are taken from the device tree and the 9810 clock tables.
 * The sensor's registers are 16-bit addresses; see sensor_up() and the mode table.
 */
#define SENSOR          0x10            /* fimc-is-secure@10, reg = <16> */
#define PB02            0x59            /* the S2MPB02 */

static struct hakios_hsi2c sensor_bus = {
	.name = "iris-5f1",
	.regs_pa = EXYNOS_PERIC1_USI07_HSI2C1_PA,                  /* hsi2c@108D0000: USI07_I2C of PERIC1 (hsi2c19) */
	.usi_mode_pa = EXYNOS_PERIC1_USI07_MODE_PA,              /* usi@10811024: its mode word ("i2c") */
	.cmu_pa = EXYNOS_CMU_PERIC1_PA, .cmu_size = 0x4000,       /* CMU_PERIC1 */
	/* GATE_CLK_PERIC1_USI_I2C, RSTNSYNC_CLK_PERIC1_USI07_I2C, USI07_I2C IPCLK, USI07_I2C PCLK */
	.gates = { 0x2050, 0x2090, 0x20DC, 0x20E0 }, .ngates = 4,
	.qch = 0x3034, .div = 0x1830, .mux = 0x0120,       /* QCH_CON_USI07_I2C_QCH; DIV_CLK_PERIC1_USI_I2C */
	.pins_pa = EXYNOS_GPIO_PERIC1_PA,                  /* gpp4: SCL pin 6, SDA pin 7, function 3 */
	.pins_mask = 0xff000000u, .pins_want = 0x33000000u,
	.ipclk_mhz = 400, .bus_hz = 400000,     /* clock-frequency = <400000> */
};

static struct hakios_hsi2c pb02_bus = {
	.name = "iris-s2mpb02",
	.regs_pa = EXYNOS_PERIC1_USI10_HSI2C0_PA,                  /* hsi2c@10920000: USI10 of PERIC1 (as hakios.exynos_camera.c) */
	.usi_mode_pa = EXYNOS_PERIC1_USI10_MODE_PA,
	.cmu_pa = EXYNOS_CMU_PERIC1_PA, .cmu_size = 0x4000,
	.gates = { 0x2048, 0x20AC, 0x2114, 0x2118 }, .ngates = 4,
	.qch = 0x3050, .div = 0x1828, .mux = 0x0120,
	.pins_pa = EXYNOS_GPIO_PERIC1_PA + 0x40,                  /* gpp6: SCL pin 0, SDA pin 1, function 2 */
	.pins_mask = 0xffu, .pins_want = 0x22u,
	.ipclk_mhz = 400, .bus_hz = 100000,
};

/*
 * Pins. iris_en is gpg0-7 of PERIC0 (an output, high while the module is powered). The reset is gpg2-3 of PERIC0
 * (an output, high while the sensor runs). MCLK2 is gpc1-6 of PERIC1: function 2 ("fimc_is_mclk2_fn", drive 1)
 * while the clock runs, and function 1 (an output, low) when off. Pin values are from the device tree.
 */
#define GPIO_PERIC0_PA  EXYNOS_GPIO_PERIC0_PA
#define GPIO_PERIC1_PA  EXYNOS_GPIO_PERIC1_PA
#define GPG0            0x080           /* PERIC0: iris_en, pin 7 */
#define GPG2            0x0C0           /* PERIC0: the reset, pin 3 */
#define GPC1            0x080           /* PERIC1: MCLK2, pin 6 */
#define GPP4            0x000           /* PERIC1: the sensor's bus, pins 6 and 7 */
#define IRIS_EN_PIN     7
#define RESET_PIN       3
#define MCLK_PIN        6
#define PIN_DRV         0x10            /* CON, DAT, PUD, then DRV: 2 bits a pin (MCLK2's function state: drive 1) */

#define CMU_TOP_PA      EXYNOS_CMU_TOP_PA
#define CIS2_MUX        EXYNOS_IRIS_CIS2_MUX
#define CIS2_DIV        EXYNOS_IRIS_CIS2_DIV
#define CIS2_GATE       EXYNOS_IRIS_CIS2_GATE
#define CIS2_QCH        EXYNOS_IRIS_CIS2_QCH
#define GATE_MANUAL     (1u << 20)
#define GATE_ON         (1u << 21)

/*
 * CIS_CLK2 in CMU_TOP: the mux selects OSCCLK, 26 MHz, which is Android's rate undivided. The *_found values hold
 * each register as this run found it. The *_mine flags say this driver changed the register and must restore it.
 */
static vm_offset_t peric0, peric1, cmu_top;
static uint32_t gate_found, mux_found, div_found, qch_found, drv_found;
static bool clock_mine, drv_mine, powered, broken;

/*
 * The S2MPS18's LDOs, through ACPM (hakios.exynos_acpm.c, block PMIC). Control register: [7:6] enable (3 on),
 * [5:0] voltage. LDO39 VDDD_1.05V_IRIS at 0x70, 0.7 V + 12.5 mV a step (1.05 V = 0x1C). LDO40 VDDA_2.8V_IRIS at
 * 0x71, 1.8 V + 25 mV a step (2.8 V = 0x28). LDO41 VDDIO_1.8V_IRIS at 0x72, 0.7 V + 25 mV a step (1.8 V = 0x2C).
 */
#define SLDO_ON         0xC0            /* [7:6] = 3: on (normal mode) */
#define SLDO_VSEL       0x3F

static const struct sldo {
	const char *name;
	uint8_t reg, vsel;
} sldos[] = {
	{ "VDDIO_1.8V_IRIS (LDO41)", 0x72, 0x2C },
	{ "VDDA_2.8V_IRIS (LDO40)", 0x71, 0x28 },
	{ "VDDD_1.05V_IRIS (LDO39)", 0x70, 0x1C },
};
#define NSLDOS          (sizeof(sldos) / sizeof(sldos[0]))
static uint8_t sldo_found[NSLDOS];
static bool sldo_mine[NSLDOS];

static bool
sldo_read(uint32_t i, uint8_t *value)
{
	return hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, sldos[i].reg, value) == 0;
}

/* one of the three to Android's voltage, on or off; read back */
static bool
sldo_put(uint32_t i, bool on)
{
	uint8_t want = (uint8_t)((on ? SLDO_ON : 0) | sldos[i].vsel), back = 0;

	if (broken && on) {
		return false;
	}
	if (hakios_acpm_pmic_write(HAKIOS_ACPM_PMIC_PMIC, sldos[i].reg, want) != 0 || !sldo_read(i, &back) ||
	    back != want) {
		say("exynos_iris: S2MPS18 %#x (%s): wrote %#x, read back %#x: no more writing for this run\n", sldos[i].reg,
		    sldos[i].name, want, back);
		broken = true;
		return false;
	}
	return true;
}

/* a supply already on is left on, and not switched off at power-down */
static bool
sldo_on(uint32_t i)
{
	if (sldo_found[i] & SLDO_ON) {
		say("exynos_iris: 2: %s was on already (%#x): left as it is\n", sldos[i].name, sldo_found[i]);
		return true;
	}
	sldo_mine[i] = true;
	if (!sldo_put(i, true)) {
		return false;
	}
	say("exynos_iris: 2: %s on (%#x)\n", sldos[i].name, SLDO_ON | sldos[i].vsel);
	spin_us(200);
	return true;
}

static void
sldo_off(uint32_t i)
{
	if (sldo_mine[i]) {
		bool was = broken;

		broken = false;         /* switching off is always tried */
		if (sldo_put(i, false)) {
			sldo_mine[i] = false;
		}
		broken = broken || was;
	}
}

/*
 * The infrared LED is the S2MPB02's second flash channel (FLED2), on the pb02 bus at 0x59. This driver does not
 * light it. In Android's set-up FLED2 is in infrared mode and pulses on a strobe the AP does not drive: once per
 * frame while the sensor streams, with IRON set long after IRD. The values below are the kernel's own, from
 * leds-s2mpb02.c (CONFIG_LEDS_IRIS_IRLED_SUPPORT) and the active discharge its probe adds (bit 1 of FLED_IRD2,
 * "to prevent faint light"). Samsung's iris HAL sets other values per shot through V4L2 controls, and those are
 * not in the GPL sources.
 * Only these seven registers are written, with these values. Nothing else of the LED is written: no torch mode,
 * no other current or pulse. FLED_TIME2 bit 0 is the IR max timer that s2mpb02_ir_led_max_time() sets.
 */
static const struct led_reg {
	uint8_t reg, value, mask;       /* mask: the bits written (the rest as found) */
	const char *name;
} led_regs[] = {
	{ 0x13, 0x38, 0xff, "FLED_CTRL2 (IR mode: remote off, power-LED, GPIO polarity)" },
	{ 0x16, 0xAF, 0xff, "FLED_CUR2 (1050 mA)" },
	{ 0x18, 0x19, 0xff, "FLED_IRON1" },
	{ 0x19, 0x0B, 0xff, "FLED_IRON2" },
	{ 0x1A, 0x00, 0xff, "FLED_IRD1" },
	{ 0x1B, 0x2E, 0xff, "FLED_IRD2 (and the active discharge)" },
	{ 0x17, 0x01, 0x01, "FLED_TIME2 bit 0 (the IR max timer on)" },
};
#define NLEDREGS        (sizeof(led_regs) / sizeof(led_regs[0]))
static bool pb02_ready, led_ready;

static uint32_t
pb02_get(uint8_t reg, uint8_t *value)
{
	return hakios_hsi2c_read_reg(&pb02_bus, PB02, reg, value, 1);
}

/* the seven registers to the kernel's values, each read back; true when all of them read as they should */
static bool
led_setup(const char *step)
{
	uint8_t v = 0, want, buf[2];

	led_ready = false;
	if (broken || !pb02_ready) {
		return false;
	}
	for (uint32_t i = 0; i < NLEDREGS; i++) {
		const struct led_reg *r = &led_regs[i];

		if (pb02_get(r->reg, &v) != 0) {
			say("exynos_iris: %s: S2MPB02 %#x (%s) cannot be read (%#x)\n", step, r->reg, r->name, pb02_bus.last_error);
			broken = true;
			return false;
		}
		want = (uint8_t)((v & ~r->mask) | (r->value & r->mask));
		if (v != want) {
			buf[0] = r->reg;
			buf[1] = want;
			if (hakios_hsi2c_message(&pb02_bus, PB02, false, buf, 2, true) != 0 || pb02_get(r->reg, &v) != 0 ||
			    v != want) {
				say("exynos_iris: %s: S2MPB02 %#x (%s): wrote %#x, read back %#x: the sensor will not be started this "
				    "run\n", step, r->reg, r->name, want, v);
				broken = true;
				return false;
			}
		}
	}
	led_ready = true;
	return true;
}

static void
led_look(const char *step)
{
	uint8_t v[10] = { 0 }, id = 0;

	(void)pb02_get(0x00, &id);
	for (uint32_t i = 0; i < 10; i++) {
		(void)pb02_get((uint8_t)(0x12 + i), &v[i]);
	}
	say("exynos_iris: %s: S2MPB02 ID %#x; FLED CTRL1 %#x CTRL2 %#x CUR1 %#x TIME1 %#x CUR2 %#x TIME2 %#x IRON1 %#x IRON2 %#x "
	    "IRD1 %#x IRD2 %#x\n", step, id, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
}

/*
 * The sensor's registers are 16-bit addresses, written with an 8- or 16-bit value. Used here: model ID 0x0000,
 * version 0x0002 (0xA1 is the final module), mode_select 0x0100 (1 streams), coarse integration 0x0202 (lines),
 * analog gain 0x0204 (0x20 is 1x), frame length 0x0340 (lines).
 */
static bool sensor_broken;

static uint32_t
sensor_get(uint16_t reg, uint8_t *buf, uint32_t len)
{
	uint8_t address[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
	uint32_t error = hakios_hsi2c_message(&sensor_bus, SENSOR, false, address, 2, false);

	return error ? error : hakios_hsi2c_message(&sensor_bus, SENSOR, true, buf, len, true);
}

static uint32_t
sensor_get16(uint16_t reg)
{
	uint8_t b[2] = { 0xff, 0xff };

	(void)sensor_get(reg, b, 2);
	return ((uint32_t)b[0] << 8) | b[1];
}

static bool
sensor_put(uint16_t reg, uint16_t value, uint32_t bytes)
{
	uint8_t buf[4] = { (uint8_t)(reg >> 8), (uint8_t)reg, 0, 0 };

	if (bytes == 2) {
		buf[2] = (uint8_t)(value >> 8);
		buf[3] = (uint8_t)value;
	} else {
		buf[2] = (uint8_t)value;
	}
	if (hakios_hsi2c_message(&sensor_bus, SENSOR, false, buf, 2 + bytes, true) != 0) {
		say("exynos_iris: the sensor did not take %#06x = %#x (%x)\n", reg, value, sensor_bus.last_error);
		sensor_broken = true;
		return false;
	}
	return true;
}

/* sensor_5f1_init: a reset (0x6010), 5 ms, then the clock registers; 0xFCFC is written before each group */
static const uint16_t init_reset[][2] = { { 0xFCFC, 0x4000 }, { 0x6010, 0x0001 } };
static const uint16_t init_clock[][2] = { { 0xFCFC, 0x4000 }, { 0x6214, 0x7971 }, { 0x6218, 0x7150 } };

/*
 * setfile_vision_5f1_fps_30_26MHz from S5K5F1S_EVT0_Ver_0.5_20170710 (the S9 kernel's module file), as 16-bit
 * writes: 2400 x 2400 RAW8, two lanes, 30 frames a second. Its line length 0x0342 = 0x0A38 pixel clocks of 4 x
 * 99.67 MHz, 6.56 us. Its frame length 0x0340 = 0x13D7 lines, 33.3 ms. Its exposure 0x0202 = 0x0339 lines, 5.4 ms.
 * The capture rewrites frame length, exposure and gain after the table (sensor_5f1_s_duration, s_again).
 */
static const uint16_t mode_2400[][2] = {
	{ 0xF43E, 0x24CE }, { 0xF440, 0x402F }, { 0x354C, 0x0004 }, { 0x3544, 0x110B }, { 0x3540, 0x110B },
	{ 0x3082, 0x0100 }, { 0x3168, 0x00A0 }, { 0x31A6, 0x0100 }, { 0xF470, 0x0000 }, { 0xF43A, 0x0010 },
	{ 0x3572, 0x0012 }, { 0xF420, 0x0013 }, { 0xF422, 0x0000 }, { 0xF424, 0x000B }, { 0xF426, 0x000E },
	{ 0xF488, 0x0008 }, { 0x3534, 0x0708 }, { 0x320E, 0x0000 }, { 0x3304, 0x0094 }, { 0xF4BA, 0x0008 },
	{ 0x319E, 0x0001 }, { 0x3078, 0x0340 }, { 0x31C6, 0x0000 }, { 0x6028, 0x2000 }, { 0x602A, 0x0E1E },
	{ 0x6F12, 0x8110 }, { 0x0306, 0x008A }, { 0x030E, 0x009D }, { 0x3560, 0x0050 }, { 0x0342, 0x0A38 },
	{ 0x0340, 0x13D7 }, { 0x0202, 0x0339 }, { 0xB134, 0x0180 }, { 0x0344, 0x0008 }, { 0x0346, 0x0008 },
	{ 0x0348, 0x0967 }, { 0x034A, 0x0967 }, { 0x034C, 0x0960 }, { 0x034E, 0x0960 }, { 0x0112, 0x0A08 },
	{ 0xF1A2, 0x0200 }, { 0xF1A8, 0x09B0 }, { 0xF1AA, 0x09B0 }, { 0x30BE, 0x0100 }, { 0x6028, 0x2000 },
	{ 0x602A, 0x0F10 }, { 0x6F12, 0x0001 }, { 0x602A, 0x0F32 }, { 0x6F12, 0x0001 }, { 0x602A, 0x0F1E },
	{ 0x6F12, 0x0487 }, { 0x6F12, 0x0725 },
};
#define MODE_ENTRIES    (sizeof(mode_2400) / sizeof(mode_2400[0]))
#define MODE_LINES      0x13D7
#define MODE_MARGIN     4
#define FRAME_W         2400
#define FRAME_H         2400
#define LINE_BYTES      FRAME_W         /* RAW8 in packet mode: a byte a pixel */
#define FRAME_BYTES     (LINE_BYTES * FRAME_H)

static uint32_t iris_exp = 0x0339, iris_gain = 0x20, iris_ms = 600, iris_ispdiv;
static bool streaming;

static void
pin_out(vm_offset_t bank, uint32_t number, bool high)
{
	uint32_t data = rd(bank, 4);

	wr(bank, 4, high ? data | (1u << number) : data & ~(1u << number));
	wr(bank, 0, (rd(bank, 0) & ~(0xfu << (4 * number))) | (1u << (4 * number)));
}

static void
pin_function(vm_offset_t bank, uint32_t number, uint32_t function)
{
	wr(bank, 0, (rd(bank, 0) & ~(0xfu << (4 * number))) | (function << (4 * number)));
}

static void
look_pins(const char *when)
{
	say("exynos_iris: %s: iris_en gpg0-7 CON %x DAT %u; reset gpg2-3 CON %x DAT %u; MCLK2 gpc1-6 CON %x DAT %u DRV %u; "
	    "bus gpp4-6/7 CON %x %x\n", when,
	    (rd(peric0 + GPG0, 0) >> (4 * IRIS_EN_PIN)) & 0xf, (rd(peric0 + GPG0, 4) >> IRIS_EN_PIN) & 1,
	    (rd(peric0 + GPG2, 0) >> (4 * RESET_PIN)) & 0xf, (rd(peric0 + GPG2, 4) >> RESET_PIN) & 1,
	    (rd(peric1 + GPC1, 0) >> (4 * MCLK_PIN)) & 0xf, (rd(peric1 + GPC1, 4) >> MCLK_PIN) & 1,
	    (rd(peric1 + GPC1, PIN_DRV) >> (2 * MCLK_PIN)) & 3,
	    (rd(peric1 + GPP4, 0) >> 24) & 0xf, (rd(peric1 + GPP4, 0) >> 28) & 0xf);
}

static void
clock_on(void)
{
	gate_found = rd(cmu_top, CIS2_GATE);
	mux_found = rd(cmu_top, CIS2_MUX);
	div_found = rd(cmu_top, CIS2_DIV);
	qch_found = rd(cmu_top, CIS2_QCH);
	if ((mux_found & 1) != 0 || (div_found & 0x1f) != 0) {
		wr(cmu_top, CIS2_MUX, mux_found & ~1u);
		wr(cmu_top, CIS2_DIV, div_found & ~0x1fu);
	}
	wr(cmu_top, CIS2_QCH, qch_found | 3);
	wr(cmu_top, CIS2_GATE, gate_found | GATE_MANUAL | GATE_ON);
	clock_mine = true;
	say("exynos_iris: 2: CIS_CLK2: mux %x -> %x, divider %x -> %x, gate %x -> %x (26 MHz from OSCCLK)\n", mux_found,
	    rd(cmu_top, CIS2_MUX), div_found, rd(cmu_top, CIS2_DIV), gate_found, rd(cmu_top, CIS2_GATE));
}

static void stream_stop(void);

/*
 * Power-down, in the module file's order. The stream stops first. Then MCLK2 goes low for 200 us, reset goes low,
 * the bus pins become inputs, the LDOs go off in reverse order, iris_en goes low, and CIS_CLK2's gate, Q-channel,
 * divider and mux are put back as found. Only what this run switched on is undone.
 */
static void
power_down(void)
{
	if (!powered) {
		return;
	}
	stream_stop();
	pin_out(peric1 + GPC1, MCLK_PIN, false);
	spin_us(200);
	pin_out(peric0 + GPG2, RESET_PIN, false);
	pin_function(peric1 + GPP4, 6, 0);
	pin_function(peric1 + GPP4, 7, 0);
	for (uint32_t i = NSLDOS; i-- > 0;) {
		sldo_off(i);
	}
	pin_out(peric0 + GPG0, IRIS_EN_PIN, false);
	if (drv_mine) {
		wr(peric1 + GPC1, PIN_DRV, drv_found);
		drv_mine = false;
	}
	if (clock_mine) {
		wr(cmu_top, CIS2_GATE, gate_found);
		wr(cmu_top, CIS2_QCH, qch_found);
		wr(cmu_top, CIS2_DIV, div_found);
		wr(cmu_top, CIS2_MUX, mux_found);
		clock_mine = false;
	}
	powered = false;
	say("exynos_iris: powered down: MCLK2, reset and iris_en low, the bus pins inputs, the LDOs %s; CIS_CLK2 as found\n",
	    (sldo_mine[0] || sldo_mine[1] || sldo_mine[2]) ? "NOT ALL OFF (a write failed)" : "off as found");
}

/*
 * Power-up, in the module file's order (fimc-is-device-module-5f1.c, secure scenario): CIS_CLK2 on at 26 MHz, reset
 * low, iris_en high, VDDIO then VDDA then VDDD at 1.05 V (200 us each), 1 ms, the bus pins to I2C (10 us), reset
 * high, MCLK2 out on its pin, 2 ms. Then the sensor's init. Returns false after powering down again.
 */
static bool
sensor_up(void)
{
	uint8_t id[2] = { 0, 0 }, version = 0;

	for (uint32_t i = 0; i < NSLDOS; i++) {
		if (!sldo_read(i, &sldo_found[i])) {
			say("exynos_iris: 2: %s cannot be read through ACPM\n", sldos[i].name);
			return false;
		}
		if ((sldo_found[i] & SLDO_ON) && (sldo_found[i] & SLDO_VSEL) != sldos[i].vsel) {
			say("exynos_iris: 2: %s is on at another voltage (%#x): someone else has it; nothing written\n",
			    sldos[i].name, sldo_found[i]);
			return false;
		}
	}
	say("exynos_iris: 2: power-up: CIS_CLK2, reset low, iris_en high, VDDIO, VDDA, VDDD 1.05 V, 1 ms, the bus, reset "
	    "high, MCLK2 out, 2 ms\n");
	powered = true;
	clock_on();
	pin_out(peric0 + GPG2, RESET_PIN, false);
	pin_out(peric0 + GPG0, IRIS_EN_PIN, true);
	for (uint32_t i = 0; i < NSLDOS; i++) {
		if (!sldo_on(i)) {
			power_down();
			return false;
		}
	}
	pause_us(1000);
	hakios_hsi2c_power(&sensor_bus);
	hakios_hsi2c_reset(&sensor_bus);
	spin_us(10);
	pin_out(peric0 + GPG2, RESET_PIN, true);
	drv_found = rd(peric1 + GPC1, PIN_DRV);
	drv_mine = true;
	wr(peric1 + GPC1, PIN_DRV, (drv_found & ~(3u << (2 * MCLK_PIN))) | (1u << (2 * MCLK_PIN)));
	pin_function(peric1 + GPC1, MCLK_PIN, 2);
	pause_us(2000);
	look_pins("2");

	sensor_broken = false;
	for (uint32_t i = 0; i < 2; i++) {
		if (!sensor_put(init_reset[i][0], init_reset[i][1], 2)) {
			say("exynos_iris: 2: the sensor does not answer at %#x\n", SENSOR);
			power_down();
			return false;
		}
	}
	pause_us(5000);
	for (uint32_t i = 0; i < 3; i++) {
		if (!sensor_put(init_clock[i][0], init_clock[i][1], 2)) {
			power_down();
			return false;
		}
	}
	(void)sensor_get(0x0000, id, 2);
	(void)sensor_get(0x0002, &version, 1);
	say("exynos_iris: 2: the sensor: model ID %02x%02x, version %#x (0xA1: the final module)\n", id[0], id[1], version);
	return true;
}

/*
 * The OTP test result and date, read as sensor_5f1_init reads them. The sensor streams during the read: page_info
 * at 0x0A12 (page 62's) names the page, which is read at 0x0A04-0x0A09.
 */
static void
sensor_otp(void)
{
	uint8_t page = 0xff, t[6] = { 0 };
	uint16_t select;

	if (!sensor_put(0x0100, 0x01, 1)) {
		return;
	}
	pause_us(4000);
	if (sensor_put(0x0A02, 0x3E00, 2) && sensor_put(0x0A00, 0x0100, 2)) {
		spin_us(360);
		(void)sensor_get(0x0A12, &page, 1);
		select = page == 0x03 ? 0x3F00 : 0x3E00;
		if ((page == 0x00 || page == 0x01 || page == 0x03) && sensor_put(0x0A02, select, 2) &&
		    sensor_put(0x0A00, 0x0100, 2)) {
			spin_us(360);
			for (uint32_t i = 0; i < 6; i++) {
				(void)sensor_get((uint16_t)(0x0A04 + i), &t[i], 1);
			}
		}
		(void)sensor_put(0x0A00, 0x0000, 2);
	}
	(void)sensor_put(0x0100, 0x00, 1);
	pause_us(40000);
	say("exynos_iris: 2: the OTP: page_info %#x; test (focus, macro, pan) %u %u %u (1 1 1: passed); made 20%02x-%02x by "
	    "%#x\n", page, t[0], t[1], t[2], t[3], t[4], t[5]);
}

/*
 * The link is CSIS3 (0x16260000, the same block as CSIS1; register layout from fimc-is-hw-csi-v5_1.h), its D-PHY
 * (dphy_m0s4s2_csis3, registers at 0x16190000), the DMA common block at 0x162F0000 and VC0's WDMA at 0x162E0000.
 * The ISPPRE power domain, CMU_TOP's CLKCMU_ISPPRE_BUS and the CMU_ISPPRE Q-channels are used as in
 * hakios.exynos_camera.c, and only while this driver holds them. The SysMMU is not touched: it is taken as off,
 * pass-through.
 *
 * Link settings: CMN_CTRL two lanes, interleave mode 0 (channel 0 only). Channel 0 is VC 0, RAW8 (data type 0x2A),
 * dual pixel mode, 2400 x 2400. RAW8 is packet storage: one byte a pixel, 2400 bytes a line (DMA0_FMT bit 4, set by
 * csi_hw_s_config_dma).
 * D-PHY at 2041 Mb/s a lane: CLKSETTLECTL 1; per lane HS_MODE_SEL 0, SKEWCAL_EN 1, SKEW_DLYSEL 2 (2 to 3 Gb/s),
 * HSSETTLECTL 10 (fimc_is_hw_find_settle(2041), csi_hw_s_phy_config). The D-PHY isolation is PMU 0x14060730 bit 0,
 * shared with CSIS1. Its reset is bit 3 of SYSREG_ISPPRE 0x16210500.
 *
 * Frame buffer: 16 MB below 4 GB, outside XNU's RAM, from sys/hakios.exynos9810_mem.h (0xD8000000; the front
 * camera uses the same range, and the two are never used together). The raw frame takes the first 8 MB, filled
 * with 0xA5 before the capture. The RGB picture follows it and is at most 6 MB.
 *
 * Sources: fimc-is-hw-csi-v5_1.c and .h (csi_hw_s_config, csi_hw_s_config_dma, csi_hw_s_phy_config),
 * fimc-is-hw-settle-10nm-lpp.h and fimc-is-hw-chain.c (fimc_is_hw_find_settle), fimc-is-hw.h (HW_FORMAT_RAW8).
 */

#define PMU_PA                  EXYNOS_PMU_PA
#define PMU_SIZE                0x5000
#define PMU_DPHY_ISO            EXYNOS_IRIS_PMU_DPHY_ISO          /* bit 0: the M0S4S2's isolation bypassed (CSIS1's and CSIS3's) */
#define PMU_DPU_STATUS          EXYNOS_IRIS_PMU_DPU_STATUS
#define PMU_ISPPRE_CONFIG       EXYNOS_IRIS_PMU_ISPPRE_CONFIG
#define PMU_ISPPRE_STATUS       EXYNOS_IRIS_PMU_ISPPRE_STATUS
#define PMU_ISPPRE_MEMORY       EXYNOS_IRIS_PMU_ISPPRE_MEMORY
#define PD_ON                   0xfu
#define PD_WAIT_US              10000u
#define ISO_BYPASS              1u
#define DTZPC_SMC               0x82000410u
#define DTZPC_SAVE              0u
#define DTZPC_RESTORE           1u
#define DTZPC_ISPPRE            EXYNOS_IRIS_DTZPC_ISPPRE

#define CMU_ISPPRE_PA           EXYNOS_CMU_ISPPRE_PA
#define ISPPRE_BUS_USER         0x0100
static const uint32_t qchs[] = { 0x3020, 0x3024, 0x3028, 0x302C, 0x3038, 0x305C };     /* CSIS0-3, PDP DMA, SysMMU */
#define NQCHS                   (sizeof(qchs) / sizeof(qchs[0]))
#define QCH_CSIS3               EXYNOS_IRIS_QCH_CSIS3
#define QCH_REQ                 (1u << 1)
#define TOP_ISPPRE_MUX          EXYNOS_IRIS_TOP_ISPPRE_MUX
#define TOP_ISPPRE_DIV          EXYNOS_IRIS_TOP_ISPPRE_DIV
#define TOP_ISPPRE_GATE         EXYNOS_IRIS_TOP_ISPPRE_GATE
#define TOP_BUSY                (1u << 16)
#define ISPPRE_MUX_SHARED1_DIV4 1u

#define SYSREG_ISPPRE_PA        EXYNOS_SYSREG_ISPPRE_PA
#define DPHY_RESET              0x0500
#define DPHY_RESET_CSIS3        (1u << 3)

#define CSIS3_PA                EXYNOS_CSIS3_PA
#define CSIS_VERSION            0x000
#define CSIS_CMN_CTRL           0x004
#define CSIS_INT_MSK0           0x010
#define CSIS_INT_SRC0           0x014
#define CSIS_INT_MSK1           0x018
#define CSIS_INT_SRC1           0x01C
#define CSIS_ISP_CONFIG(n)      (0x040 + 0x10 * (n))
#define CSIS_ISP_RESOL(n)       (0x044 + 0x10 * (n))
#define CSIS_FRM_CNT_CH0        0x100
#define CSIS_PHY_STATUS         0x700
#define CSIS_PHY_CMN_CTRL       0x704
#define CMN_CSI_EN              (1u << 0)
#define CMN_SW_RESET            (1u << 1)
#define CMN_LANES_SHIFT         8
#define CMN_INTERLEAVE_SHIFT    10
#define CMN_UPDATE_SHADOW       (0xfu << 16)
#define CMN_QCHANNEL_EN         (1u << 20)
#define CMN_PHY_SEL             (1u << 21)
#define PHY_ENABLE_CLK          (1u << 0)
#define PHY_ENABLE_DAT_SHIFT    1
#define CSIS_IRQ_MASK0          0x011FFF1Fu
#define CSIS_IRQ_MASK1          0x0000FFF0u
#define HW_FORMAT_RAW8          0x2A
#define HW_FORMAT_UNKNOWN       0x3F

#define CSIS3_PHY_PA            EXYNOS_CSIS3_PHY_PA
#define DPHY_DCTRL_SC_02        0x1084          /* [7:0] CLKSETTLECTL */
#define DPHY_ACTRL_SC_06        0x1018
#define DPHY_LANE(n)            (0x400u * (n))
#define DPHY_ACTRL_SD_08        0x143C          /* [6:5] SKEW_DLYSEL */
#define DPHY_DCTRL_SD_05        0x14AC          /* bit 2 HS_MODE_SEL */
#define DPHY_DCTRL_SD_06        0x14B0          /* [7:0] HSSETTLECTL */
#define DPHY_DCTRL_SD_13        0x14E0          /* bit 0 SKEWCAL_EN */
#define SETTLE_2041             10
#define SKEW_DLYSEL_2G          2

#define CSIS3_WDMA_PA           EXYNOS_CSIS3_WDMA_PA
#define DMA_CTRL                0x000
#define DMA_FMT                 0x004
#define DMA_ADDR(n)             (0x010 + 4 * (n))
#define DMA_ACT_CTRL            0x030
#define DMA_FCNTSEQ_STAT        0x054
#define DMA_ON                  (1u << 0)
#define DMA_UPDT_PTR_EN         (1u << 1)
#define DMA_FRAMEPTR_MASK       (7u << 2)
#define DMA_STG_PACKET          (1u << 4)
#define VCMN                    0x400
#define VCMN_DATA_CTRL          0x00
#define VCMN_INT_MASK           0x04
#define VCMN_INT_SRC            0x08
#define VCMN_CMN_CTRL           0x14
#define VCMN_ERR_CODE           0x18
#define VCMN_OTF_FORMAT         0x1C
#define VCMN_IRQ_MASK           0x00F11F00u
#define DMA_FRM_START0          (1u << 4)
#define DMA_FRM_END0            (1u << 8)
#define DMA_ERROR               (1u << 12)

#define CSIS_DMA_PA             EXYNOS_CSIS_DMA_PA
#define CDMA_ARB_PRI            0x00
#define CDMA_CTRL               0x08
#define CDMA_SRAM_SPLIT         0x0C
#define CDMA_MATRIX             0x10

#define MEM_PA                  EXYNOS9810_MEM_IRIS_PA
#define MEM_BYTES               ((uint32_t)EXYNOS9810_MEM_SIZE(IRIS))
#define RAW_SLOT                (8u << 20)
#define RGB_OFF                 RAW_SLOT
#define RGB_MAX                 (MEM_BYTES - RAW_SLOT)
#define CANARY                  0xA5
_Static_assert(FRAME_BYTES <= RAW_SLOT, "exynos_iris: a frame is more than its slot");
_Static_assert((FRAME_W / 2) * (FRAME_H / 2) * 3 <= (16u << 20) - RAW_SLOT, "exynos_iris: the picture is more than its place");

static vm_offset_t pmu, cmu_isppre, sysreg_isppre, csis, dphy, wdma, cdma, buffer;
static uint32_t iso_found, dphy_reset_found, user_found, top_gate_found, top_mux_found, top_div_found;
static uint32_t isppre_qch_found[NQCHS], cdma_found[4];
static bool domain_up, domain_mine, clocks_mine, top_gate_mine, top_rate_mine, phy_mine, link_on, dma_on, cdma_mine;
static bool buffer_busy;
static bool isp_held;           /* iris_s9_smc >= 2: the domain stays up from before PREPARE to after UNPREPARE */

/* One SMC to EL3, through Carbon at EL2. Returns x0. The same helper as in hakios.exynos_camera.c. */
static uint64_t
smc(uint64_t function, uint64_t first, uint64_t second, uint64_t third)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = first;
	register uint64_t x2 __asm__("x2") = second;
	register uint64_t x3 __asm__("x3") = third;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static void
stream_stop(void)
{
	if (!streaming) {
		return;
	}
	(void)sensor_put(0x0100, 0x00, 1);
	streaming = false;
	spin_us(40000);         /* one frame (sensor_5f1_stream_off) */
	say("exynos_iris: the sensor told to stop streaming (0x0100 = 0)\n");
}

static void
dma_stop(void)
{
	if (wdma == 0 || !domain_up || !dma_on) {
		return;
	}
	wr(wdma, DMA_CTRL, rd(wdma, DMA_CTRL) & ~(DMA_ON | DMA_UPDT_PTR_EN));
	dma_on = false;
	if (!wait_for(wdma, DMA_ACT_CTRL, DMA_ON, 0, 100000)) {
		/* ACT_CTRL takes DMA_CTRL's ON only at a frame start. On the phone no frame start came after the sensor
		 * stopped (ACT_CTRL read 0xff11 after a whole frame), so the link is stopped next and nothing more reaches
		 * the WDMA. */
		say("exynos_iris: the WDMA's active bit is still set (ACT_CTRL %#x): it changes at the next frame start, and "
		    "the link is stopped now\n", rd(wdma, DMA_ACT_CTRL));
	}
}

static void
link_stop(void)
{
	if (!link_on || !domain_up) {
		return;
	}
	wr(csis, CSIS_PHY_CMN_CTRL, rd(csis, CSIS_PHY_CMN_CTRL) & ~(PHY_ENABLE_CLK | (0xfu << PHY_ENABLE_DAT_SHIFT)));
	wr(csis, CSIS_CMN_CTRL, (rd(csis, CSIS_CMN_CTRL) & ~CMN_CSI_EN) | CMN_QCHANNEL_EN);
	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) | CMN_SW_RESET);
	(void)wait_for(csis, CSIS_CMN_CTRL, CMN_SW_RESET, 0, 100);
	wr(csis, CSIS_INT_MSK0, 0);
	wr(csis, CSIS_INT_MSK1, 0);
	link_on = false;
}

static void
phy_off(void)
{
	if (!phy_mine) {
		return;
	}
	if (domain_up && (dphy_reset_found & DPHY_RESET_CSIS3) == 0) {
		wr(sysreg_isppre, DPHY_RESET, rd(sysreg_isppre, DPHY_RESET) & ~DPHY_RESET_CSIS3);
	}
	if ((iso_found & ISO_BYPASS) == 0) {
		wr(pmu, PMU_DPHY_ISO, rd(pmu, PMU_DPHY_ISO) & ~ISO_BYPASS);
	}
	phy_mine = false;
}

/* everything of the ISPPRE block this run switched on, off again; as isp_down() in hakios.exynos_camera.c, for CSIS3 */
static void
isp_down(void)
{
	uint64_t answer;
	uint32_t was;

	dma_stop();
	link_stop();
	phy_off();
	if (isp_held) {
		return;
	}
	if (cdma_mine && domain_up) {
		wr(cdma, CDMA_ARB_PRI, cdma_found[0]);
		wr(cdma, CDMA_CTRL, cdma_found[1]);
		wr(cdma, CDMA_SRAM_SPLIT, cdma_found[2]);
		wr(cdma, CDMA_MATRIX, cdma_found[3]);
		cdma_mine = false;
	}
	if (clocks_mine && !domain_mine && domain_up) {
		for (uint32_t i = 0; i < NQCHS; i++) {
			wr(cmu_isppre, qchs[i], isppre_qch_found[i]);
		}
		wr(cmu_isppre, ISPPRE_BUS_USER, user_found);
	}
	clocks_mine = false;
	if (domain_mine) {
		was = rd(pmu, PMU_ISPPRE_MEMORY);
		wr(pmu, PMU_ISPPRE_MEMORY, (was & ~3u) | 2u);
		answer = smc(DTZPC_SMC, DTZPC_SAVE, DTZPC_ISPPRE, 0);
		if ((uint32_t)answer != 0) {
			say("exynos_iris: the DTZPC save returned %#llx: the ISPPRE domain is left ON\n", answer);
		} else {
			domain_up = false;
			wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) & ~PD_ON);
			if (!wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, 0, PD_WAIT_US)) {
				say("exynos_iris: the domain did not go off (status %#x): on again, its protection restored, left on\n",
				    rd(pmu, PMU_ISPPRE_STATUS));
				wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) | PD_ON);
				if (wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, PD_ON, PD_WAIT_US)) {
					(void)smc(DTZPC_SMC, DTZPC_RESTORE, DTZPC_ISPPRE, 0);
					domain_up = true;
				}
			}
		}
		domain_mine = false;
		say("exynos_iris: ISPPRE: configuration %#x, status %#x\n", rd(pmu, PMU_ISPPRE_CONFIG),
		    rd(pmu, PMU_ISPPRE_STATUS));
	}
	if (top_rate_mine) {
		wr(cmu_top, TOP_ISPPRE_DIV, top_div_found);
		(void)wait_for(cmu_top, TOP_ISPPRE_DIV, TOP_BUSY, 0, 1000);
		wr(cmu_top, TOP_ISPPRE_MUX, top_mux_found);
		(void)wait_for(cmu_top, TOP_ISPPRE_MUX, TOP_BUSY, 0, 1000);
		top_rate_mine = false;
	}
	if (top_gate_mine) {
		wr(cmu_top, TOP_ISPPRE_GATE, top_gate_found);
		top_gate_mine = false;
	}
}

static bool
isp_up(void)
{
	uint64_t answer;

	if (isp_held) {
		return true;
	}

	if (pmu == 0) {
		pmu = ml_io_map(PMU_PA, PMU_SIZE);
		cmu_isppre = ml_io_map(CMU_ISPPRE_PA, 0x8000);
	}
	if (pmu == 0 || cmu_isppre == 0) {
		say("exynos_iris: 4: the PMU or CMU_ISPPRE could not be mapped\n");
		return false;
	}
	iso_found = rd(pmu, PMU_DPHY_ISO);
	if ((rd(pmu, PMU_DPU_STATUS) & PD_ON) != PD_ON) {
		say("exynos_iris: 4: the parent domain (DPU) is off: ISPPRE is not switched on under it\n");
		return false;
	}
	if ((rd(pmu, PMU_ISPPRE_STATUS) & PD_ON) == PD_ON) {
		say("exynos_iris: 4: ISPPRE is on already: used as it is, and left on\n");
	} else {
		domain_mine = true;
		wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) | PD_ON);
		if (!wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, PD_ON, PD_WAIT_US)) {
			say("exynos_iris: 4: the domain did not come on (status %#x)\n", rd(pmu, PMU_ISPPRE_STATUS));
			wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) & ~PD_ON);
			(void)wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, 0, PD_WAIT_US);
			domain_mine = false;
			return false;
		}
		answer = smc(DTZPC_SMC, DTZPC_RESTORE, DTZPC_ISPPRE, 0);
		say("exynos_iris: 4: ISPPRE on (status %#x); the DTZPC restore returned %#llx\n", rd(pmu, PMU_ISPPRE_STATUS),
		    answer);
		if ((uint32_t)answer != 0) {
			wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) & ~PD_ON);
			(void)wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, 0, PD_WAIT_US);
			domain_mine = false;
			return false;
		}
	}
	domain_up = true;
	if (sysreg_isppre == 0) {
		sysreg_isppre = ml_io_map(SYSREG_ISPPRE_PA, 0x1000);
		csis = ml_io_map(CSIS3_PA, 0x1000);
		dphy = ml_io_map(CSIS3_PHY_PA, 0x3000);
		wdma = ml_io_map(CSIS3_WDMA_PA, 0x1000);
		cdma = ml_io_map(CSIS_DMA_PA, 0x1000);
	}
	if (sysreg_isppre == 0 || csis == 0 || dphy == 0 || wdma == 0 || cdma == 0) {
		say("exynos_iris: 4: a block could not be mapped\n");
		return false;
	}
	top_gate_found = rd(cmu_top, TOP_ISPPRE_GATE);
	if ((top_gate_found & (GATE_MANUAL | GATE_ON)) != (GATE_MANUAL | GATE_ON)) {
		wr(cmu_top, TOP_ISPPRE_GATE, top_gate_found | GATE_MANUAL | GATE_ON);
		top_gate_mine = true;
	}
	top_mux_found = rd(cmu_top, TOP_ISPPRE_MUX);
	top_div_found = rd(cmu_top, TOP_ISPPRE_DIV);
	if ((top_mux_found & 3u) != ISPPRE_MUX_SHARED1_DIV4 || (top_div_found & 0xfu) != iris_ispdiv) {
		top_rate_mine = true;
		wr(cmu_top, TOP_ISPPRE_MUX, (top_mux_found & ~3u) | ISPPRE_MUX_SHARED1_DIV4);
		(void)wait_for(cmu_top, TOP_ISPPRE_MUX, TOP_BUSY, 0, 1000);
		wr(cmu_top, TOP_ISPPRE_DIV, (top_div_found & ~0xfu) | iris_ispdiv);
		(void)wait_for(cmu_top, TOP_ISPPRE_DIV, TOP_BUSY, 0, 1000);
	}
	user_found = rd(cmu_isppre, ISPPRE_BUS_USER);
	for (uint32_t i = 0; i < NQCHS; i++) {
		isppre_qch_found[i] = rd(cmu_isppre, qchs[i]);
	}
	clocks_mine = true;
	if ((user_found & (1u << 4)) == 0) {
		wr(cmu_isppre, ISPPRE_BUS_USER, user_found | (1u << 4));
		(void)wait_for(cmu_isppre, ISPPRE_BUS_USER, 1u << 7, 0, 1000);
	}
	for (uint32_t i = 0; i < NQCHS; i++) {
		wr(cmu_isppre, qchs[i], isppre_qch_found[i] | QCH_REQ);
	}
	dphy_reset_found = rd(sysreg_isppre, DPHY_RESET);
	say("exynos_iris: 4: CLKCMU_ISPPRE_BUS mux %#x, divider %#x (466 MHz / %u); Q-channel CSIS3 %#x; CSIS3 version %#x, "
	    "CMN_CTRL %#x; the PHYs' resets %#x, isolation %#x; VC0's WDMA CTRL %#x, ACT_CTRL %#x\n",
	    rd(cmu_top, TOP_ISPPRE_MUX), rd(cmu_top, TOP_ISPPRE_DIV), iris_ispdiv + 1, rd(cmu_isppre, QCH_CSIS3),
	    rd(csis, CSIS_VERSION), rd(csis, CSIS_CMN_CTRL), dphy_reset_found, iso_found, rd(wdma, DMA_CTRL),
	    rd(wdma, DMA_ACT_CTRL));
	if (rd(csis, CSIS_VERSION) == 0 || rd(csis, CSIS_VERSION) == 0xffffffffu) {
		say("exynos_iris: 4: CSIS3's version reads %#x: is its clock there?\n", rd(csis, CSIS_VERSION));
	}
	return true;
}

/* the D-PHY's isolation off and its reset pulsed; the settings for 2041 Mb/s a lane, with deskew on */
static void
phy_setup(void)
{
	phy_mine = true;
	wr(pmu, PMU_DPHY_ISO, rd(pmu, PMU_DPHY_ISO) | ISO_BYPASS);
	wr(sysreg_isppre, DPHY_RESET, rd(sysreg_isppre, DPHY_RESET) & ~DPHY_RESET_CSIS3);
	wr(sysreg_isppre, DPHY_RESET, rd(sysreg_isppre, DPHY_RESET) | DPHY_RESET_CSIS3);
	wr(dphy, DPHY_ACTRL_SC_06, 2);
	wr(dphy, DPHY_DCTRL_SC_02, (rd(dphy, DPHY_DCTRL_SC_02) & ~0xffu) | 1u);
	for (uint32_t lane = 0; lane < 2; lane++) {
		uint32_t o = DPHY_LANE(lane);

		wr(dphy, DPHY_DCTRL_SD_13 + o, rd(dphy, DPHY_DCTRL_SD_13 + o) | 1u);
		wr(dphy, DPHY_ACTRL_SD_08 + o, (rd(dphy, DPHY_ACTRL_SD_08 + o) & ~(3u << 5)) | (SKEW_DLYSEL_2G << 5));
		wr(dphy, DPHY_DCTRL_SD_05 + o, rd(dphy, DPHY_DCTRL_SD_05 + o) & ~(1u << 2));
		wr(dphy, DPHY_DCTRL_SD_06 + o, (rd(dphy, DPHY_DCTRL_SD_06 + o) & ~0xffu) | SETTLE_2041);
	}
	say("exynos_iris: 4: the D-PHY (CSIS3): isolation %#x, resets %#x; lane 0: SD_08 %#x SD_05 %#x SD_06 %#x SD_13 %#x\n",
	    rd(pmu, PMU_DPHY_ISO), rd(sysreg_isppre, DPHY_RESET), rd(dphy, DPHY_ACTRL_SD_08), rd(dphy, DPHY_DCTRL_SD_05),
	    rd(dphy, DPHY_DCTRL_SD_06), rd(dphy, DPHY_DCTRL_SD_13));
}

/* CSIS3 reset and set up for 2400 x 2400 RAW8 on VC0; its WDMA for packet storage onto the buffer */
static bool
link_setup(void)
{
	uint32_t v;

	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) | CMN_SW_RESET);
	if (!wait_for(csis, CSIS_CMN_CTRL, CMN_SW_RESET, 0, 100)) {
		say("exynos_iris: 4: CSIS3's reset does not finish (CMN_CTRL %#x)\n", rd(csis, CSIS_CMN_CTRL));
		return false;
	}
	link_on = true;
	wr(csis, CSIS_INT_MSK0, CSIS_IRQ_MASK0);
	wr(csis, CSIS_INT_MSK1, CSIS_IRQ_MASK1);
	wr(csis, CSIS_INT_SRC0, rd(csis, CSIS_INT_SRC0));
	wr(csis, CSIS_INT_SRC1, rd(csis, CSIS_INT_SRC1));
	v = rd(csis, CSIS_CMN_CTRL) & ~((3u << CMN_LANES_SHIFT) | (3u << CMN_INTERLEAVE_SHIFT));
	wr(csis, CSIS_CMN_CTRL, v | (1u << CMN_LANES_SHIFT));
	wr(csis, CSIS_PHY_CMN_CTRL, (rd(csis, CSIS_PHY_CMN_CTRL) & ~(0xfu << PHY_ENABLE_DAT_SHIFT)) |
	    (3u << PHY_ENABLE_DAT_SHIFT));
	wr(csis, CSIS_ISP_CONFIG(0), (rd(csis, CSIS_ISP_CONFIG(0)) & ~0x33ffu) | (1u << 12) | (HW_FORMAT_RAW8 << 2) | 0);
	wr(csis, CSIS_ISP_RESOL(0), ((uint32_t)FRAME_H << 16) | FRAME_W);
	for (uint32_t ch = 1; ch < 4; ch++) {
		wr(csis, CSIS_ISP_CONFIG(ch), (rd(csis, CSIS_ISP_CONFIG(ch)) & ~0x33ffu) | (1u << 12) |
		    (HW_FORMAT_UNKNOWN << 2) | ch);
		wr(csis, CSIS_ISP_RESOL(ch), 0);
	}
	wr(wdma, DMA_FMT, (rd(wdma, DMA_FMT) & ~0x3fu) | DMA_STG_PACKET);
	wr(wdma, VCMN + VCMN_DATA_CTRL, rd(wdma, VCMN + VCMN_DATA_CTRL) & ~1u);
	wr(wdma, VCMN + VCMN_INT_MASK, VCMN_IRQ_MASK);
	wr(wdma, VCMN + VCMN_INT_SRC, rd(wdma, VCMN + VCMN_INT_SRC));
	if (rd(wdma, VCMN + VCMN_CMN_CTRL) & 1u) {
		wr(wdma, VCMN + VCMN_CMN_CTRL, rd(wdma, VCMN + VCMN_CMN_CTRL) & ~1u);   /* a stale abort request */
	}
	if (!cdma_mine) {
		cdma_found[0] = rd(cdma, CDMA_ARB_PRI);
		cdma_found[1] = rd(cdma, CDMA_CTRL);
		cdma_found[2] = rd(cdma, CDMA_SRAM_SPLIT);
		cdma_found[3] = rd(cdma, CDMA_MATRIX);
		cdma_mine = true;
	}
	wr(cdma, CDMA_ARB_PRI, (cdma_found[0] & ~0xfu) | (1u << 2) | 2u);
	wr(cdma, CDMA_CTRL, cdma_found[1] | (1u << 1));
	wr(cdma, CDMA_SRAM_SPLIT, (cdma_found[2] & ~0x03ff03ffu) | (0x140u << 16) | 0x140u);
	wr(cdma, CDMA_MATRIX, cdma_found[3] & ~0x1fu);
	say("exynos_iris: 4: CSIS3 set up: CMN_CTRL %#x, PHY_CMN_CTRL %#x, CH0 %#x %#x; WDMA FMT %#x\n",
	    rd(csis, CSIS_CMN_CTRL), rd(csis, CSIS_PHY_CMN_CTRL), rd(csis, CSIS_ISP_CONFIG(0)), rd(csis, CSIS_ISP_RESOL(0)),
	    rd(wdma, DMA_FMT));
	return true;
}

static bool
buffer_get(void)
{
	if (buffer != 0) {
		return !buffer_busy;
	}
	buffer = hakios_physmap_wc(MEM_PA, MEM_BYTES);
	if (buffer == 0) {
		say("exynos_iris: the buffer (%u bytes at %#llx, physical) cannot be mapped\n", MEM_BYTES, MEM_PA);
		return false;
	}
	say("exynos_iris: the buffer: %u bytes at %#llx (physical, below 4 GB, outside XNU's RAM)\n", MEM_BYTES, MEM_PA);
	return true;
}

static void
dma_log(const char *step)
{
	say("exynos_iris: %s: WDMA CTRL %#x, ACT_CTRL %#x, FCNTSEQ %#x; INT_SRC %#x, ERR_CODE %#x; CSIS3 INT_SRC0 %#x "
	    "INT_SRC1 %#x FRM_CNT_CH0 %u PHY_STATUS %#x\n", step, rd(wdma, DMA_CTRL), rd(wdma, DMA_ACT_CTRL),
	    rd(wdma, DMA_FCNTSEQ_STAT), rd(wdma, VCMN + VCMN_INT_SRC), rd(wdma, VCMN + VCMN_ERR_CODE),
	    rd(csis, CSIS_INT_SRC0), rd(csis, CSIS_INT_SRC1), rd(csis, CSIS_FRM_CNT_CH0), rd(csis, CSIS_PHY_STATUS));
}

/* how much of the frame changed, and the middle 256 x 256's mean and spread */
static void
frame_report(void)
{
	const uint8_t *b = (const uint8_t *)buffer;
	uint32_t changed = 0, last = 0, lo = 255, hi = 0;
	uint64_t sum = 0;

	cache_flush(buffer, FRAME_BYTES);
	for (uint32_t i = 0; i < FRAME_BYTES; i += 8) {
		uint64_t w = *(const volatile uint64_t *)(buffer + i);

		if (w != 0xA5A5A5A5A5A5A5A5ULL) {
			changed += 8;
			last = i + 8;
		}
	}
	for (uint32_t y = FRAME_H / 2 - 128; y < FRAME_H / 2 + 128; y++) {
		for (uint32_t x = FRAME_W / 2 - 128; x < FRAME_W / 2 + 128; x++) {
			uint8_t p = b[y * LINE_BYTES + x];

			sum += p;
			lo = p < lo ? p : lo;
			hi = p > hi ? p : hi;
		}
	}
	say("exynos_iris: 4: the frame: about %u of %u bytes changed, the last near %u (line %u); the middle 256 x 256: "
	    "mean %llu, %u to %u; first bytes %02x %02x %02x %02x %02x %02x %02x %02x\n", changed, FRAME_BYTES, last,
	    last / LINE_BYTES, sum / (256 * 256), lo, hi, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
}

/*
 * One capture: the mode and exposure, the LED's registers, the link, then streaming until the second VC0 frame end,
 * for at most iris_s9_ms. The exposure is kept within 2 to (frame length - MODE_MARGIN) lines.
 */
static bool
capture(void)
{
	uint64_t until, began;
	uint32_t exp = iris_exp, src;
	bool got = false;

	if (!buffer_get()) {
		return false;
	}
	if (!led_setup("4")) {
		say("exynos_iris: 4: the LED's registers are not the kernel's: no capture\n");
		return false;
	}
	exp = exp < 2 ? 2 : exp;
	exp = exp > MODE_LINES - MODE_MARGIN ? MODE_LINES - MODE_MARGIN : exp;
	say("exynos_iris: 4: the mode: 2400 x 2400 RAW8, %u writes; frame length %#x; exposure %u lines (%u us), analog gain "
	    "%#x\n", (unsigned)MODE_ENTRIES, MODE_LINES, exp, exp * 656 / 100, iris_gain);
	for (uint32_t i = 0; i < MODE_ENTRIES; i++) {
		if (!sensor_put(mode_2400[i][0], mode_2400[i][1], 2)) {
			return false;
		}
	}
	if (!sensor_put(0x0340, MODE_LINES, 2) || !sensor_put(0x0202, (uint16_t)exp, 2) ||
	    !sensor_put(0x0204, (uint16_t)iris_gain, 2)) {
		return false;
	}
	say("exynos_iris: 4: read back: output %u x %u, frame %u lines of %u clocks, exposure %u, gain %#x\n",
	    sensor_get16(0x034C), sensor_get16(0x034E), sensor_get16(0x0340), sensor_get16(0x0342), sensor_get16(0x0202),
	    sensor_get16(0x0204));
	if (!isp_up()) {
		return false;
	}
	phy_setup();
	if (!link_setup()) {
		return false;
	}
	memset((void *)buffer, CANARY, RAW_SLOT);
	cache_flush(buffer, RAW_SLOT);
	if (*(const volatile uint64_t *)buffer != 0xA5A5A5A5A5A5A5A5ULL) {
		say("exynos_iris: 4: the buffer does not hold what was written into it (reads %#llx): no capture\n",
		    *(const volatile uint64_t *)buffer);
		return false;
	}
	wr(wdma, DMA_CTRL, rd(wdma, DMA_CTRL) & ~DMA_FRAMEPTR_MASK);
	for (uint32_t i = 0; i < 8; i++) {
		wr(wdma, DMA_ADDR(i), (uint32_t)MEM_PA);
	}
	wr(wdma, VCMN + VCMN_INT_SRC, rd(wdma, VCMN + VCMN_INT_SRC));
	dma_on = true;
	wr(wdma, DMA_CTRL, rd(wdma, DMA_CTRL) | DMA_ON | DMA_UPDT_PTR_EN);
	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) | CMN_UPDATE_SHADOW);
	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) & ~CMN_PHY_SEL);
	wr(csis, CSIS_PHY_CMN_CTRL, rd(csis, CSIS_PHY_CMN_CTRL) | PHY_ENABLE_CLK);
	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) & ~CMN_QCHANNEL_EN);
	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) | CMN_CSI_EN);

	say("exynos_iris: 4: STREAM ON (0x0100 = 1) for %u ms at the most: the LED pulses with each frame\n", iris_ms);
	began = mach_absolute_time();
	nanoseconds_to_absolutetime((uint64_t)iris_ms * NSEC_PER_MSEC, &until);
	until += began;
	streaming = true;
	if (!sensor_put(0x0100, 0x01, 1)) {
		return false;
	}
	/* The first frame end may belong to a frame begun before the WDMA was ready, so the second one is taken. */
	for (uint32_t ends = 0; mach_absolute_time() < until;) {
		src = rd(wdma, VCMN + VCMN_INT_SRC);
		if (src & (DMA_FRM_START0 | DMA_ERROR)) {
			wr(wdma, VCMN + VCMN_INT_SRC, src & (DMA_FRM_START0 | DMA_ERROR));
		}
		if (src & DMA_FRM_END0) {
			wr(wdma, VCMN + VCMN_INT_SRC, DMA_FRM_END0);
			if (++ends == 2) {
				got = true;
				break;
			}
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	stream_stop();
	dma_log("4");
	dma_stop();
	{
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		say("exynos_iris: 4: streamed %llu ms; %s\n", ns / NSEC_PER_MSEC, got ? "a whole frame" : "NO FRAME END seen");
	}
	frame_report();
	return got;
}

extern int hakios_usb_picture(uintptr_t rgb, uint32_t width, uint32_t height, const char *what);   /* hakios.dwc3.c */

/*
 * Grey picture of every Nth pixel of the raw frame. The 1st and 99th percentiles of a sample set the black and
 * white points, which are stretched to 0 and 255.
 */
static void
picture(uint32_t n, uint32_t *wp, uint32_t *hp)
{
	const uint8_t *raw = (const uint8_t *)buffer;
	uint8_t *rgb = (uint8_t *)(buffer + RGB_OFF);
	uint32_t w = FRAME_W / n, h = FRAME_H / n, hist[256] = { 0 }, total = 0, acc = 0, lo = 0, hi = 255;

	for (uint32_t y = 0; y < h; y += 4) {
		for (uint32_t x = 0; x < w; x += 4) {
			hist[raw[(y * n) * LINE_BYTES + x * n]]++;
			total++;
		}
	}
	for (uint32_t v = 0; v < 256; v++) {
		acc += hist[v];
		if (acc <= total / 100) {
			lo = v;
		}
		if (acc <= total - total / 100) {
			hi = v;
		}
	}
	hi = hi <= lo ? lo + 1 : hi;
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			int32_t p = raw[(y * n) * LINE_BYTES + x * n];
			uint32_t o = (y * w + x) * 3;

			p = (p - (int32_t)lo) * 255 / (int32_t)(hi - lo);
			p = p < 0 ? 0 : (p > 255 ? 255 : p);
			rgb[o] = rgb[o + 1] = rgb[o + 2] = (uint8_t)p;
		}
	}
	cache_flush(buffer + RGB_OFF, w * h * 3);
	say("exynos_iris: the picture: %u x %u, every %uth pixel, grey %u..%u stretched to 0..255\n", w, h, n, lo, hi);
	*wp = w;
	*hp = h;
}

/* The LED's budget: at most 40 captures a run, at least 2 s apart. */
#define SHOTS_MAX       40
#define SHOT_GAP_MS     2000

/*
 * Secure monitor calls, iris_s9_smc=M (an experiment; 0 makes no calls). On this phone gpg0-7 (iris_en) and
 * gpp4-6/7 (the sensor bus) ignore writes from the AP while other pins in those banks take them, and the sensor
 * does not answer. Something above the kernel probably holds those pins, most likely EL3 for the iris camera's
 * secure scenario.
 * Samsung's kernel makes MC_SECURE_CAMERA_PREPARE (0x83000043) before it powers the module and UNPREPARE
 * (0x83000044) after (fimc_is_sensor_s_input in fimc-is-device-sensor_v2.c). SYSREG_PROT (0x82002132: 1 on, 0 off)
 * is called there only where NOT_SEPERATED_SYSREG is defined. That is the v5_2_0 config, not the 9810's v6_0_0, so
 * M = 3 tries it anyway.
 * Order: SMCCC_VERSION and PSCI_VERSION, logged (PSCI_VERSION is answered by Carbon, which passes other calls on
 * to EL3: smc/smc_handler.c). Pin tries. [M >= 2: ISPPRE up and held for the run.] [M = 3: SYSREG_PROT(1).]
 * PREPARE. Pin tries again. Then the steps. At the end, after the sensor is off: UNPREPARE if PREPARE returned 0,
 * [SYSREG_PROT(0)], [ISPPRE down]. Every return value is logged. The calls take no address. The secure_camera
 * region (0xE1900000, 30 MB, outside both the 0x880000000 RAM bank and the loader's extra_ram ranges) is not
 * touched, and the iris buffer ends before it.
 *
 * Observed on one phone and one kernel build. Without PREPARE the sensor does not answer at 0x10. With M = 1,
 * PREPARE returns 0 and the sensor does answer: 0x0000 reads 0x5061 (the sources do not say which model ID is
 * right), the version is 0xA1, the OTP test reads 1 1 1, and UNPREPARE returns 0. So the call reaches EL3 and is
 * what lets the sensor answer. The pin read-back does not change: gpg0-7 and gpp4-6/7 ignore writes before and
 * after PREPARE, while the bus works.
 * The frame does not reach memory at the default exposure. The WDMA reports a frame end and CSIS3 counts frames,
 * with ERR_CODE 0, yet every byte of the buffer still holds 0xA5. The check flushes the cache first with dc civac.
 * The front camera's frames do reach this same range. At 2000 lines with gain 0x100, and at 5075 lines with gain
 * 0x200, no frame end is seen in 600 ms. The cause is not known, and neither is where the bytes go. PREPARE moving
 * the CSIS3 path into the secure world would explain it, but nothing here shows that. M = 2 and 3 have not been
 * run. No irshot has produced a picture, and whether the LED pulsed has not been observed.
 */
#define SMCCC_VERSION           0x80000000u
#define PSCI_VERSION            0x84000000u
#define SECCAM_SYSREG_PROT      0x82002132u     /* MC_SECURE_CAMERA_SYSREG_PROT (1 on, 0 off) */
#define SECCAM_PREPARE          0x83000043u     /* MC_SECURE_CAMERA_PREPARE */
#define SECCAM_UNPREPARE        0x83000044u     /* MC_SECURE_CAMERA_UNPREPARE */

static uint32_t iris_smc;
static bool prepared, sysreg_protected;

/* one field of a pin's register written, read back, put back as found: true if it took the write */
static bool
pin_try(vm_offset_t bank, uint32_t off, uint32_t shift, uint32_t value, const char *what)
{
	uint32_t found = rd(bank, off), want = (found & ~(0xfu << shift)) | (value << shift), got;

	if (want == found) {
		want = found & ~(0xfu << shift);        /* already so: try 0 instead */
	}
	wr(bank, off, want);
	got = rd(bank, off);
	wr(bank, off, found);
	say("exynos_iris: try: %s: found %#x, wrote %#x, read back %#x: %s; put back, %#x\n", what, found, want, got,
	    got == want ? "TAKEN" : "IGNORED", rd(bank, off));
	return got == want;
}

static void
pins_try(const char *when)
{
	bool con, pud, scl, sda;

	con = pin_try(peric0 + GPG0, 0, 4 * IRIS_EN_PIN, 1, "iris_en gpg0-7 CON (an output, DAT low)");
	pud = pin_try(peric0 + GPG0, 8, 4 * IRIS_EN_PIN, 1, "iris_en gpg0-7 PUD (pull-down)");
	scl = pin_try(peric1 + GPP4, 0, 4 * 6, 3, "the bus gpp4-6 CON (function 3, I2C)");
	sda = pin_try(peric1 + GPP4, 0, 4 * 7, 3, "the bus gpp4-7 CON (function 3, I2C)");
	say("exynos_iris: smc: %s: the pins are %s (iris_en CON %s, PUD %s; gpp4-6 %s, gpp4-7 %s)\n", when,
	    (con && pud && scl && sda) ? "UNLOCKED" : ((con || pud || scl || sda) ? "partly locked" : "LOCKED"),
	    con ? "takes" : "ignores", pud ? "takes" : "ignores", scl ? "takes" : "ignores", sda ? "takes" : "ignores");
}

static void
smc_begin(void)
{
	uint64_t answer;

	say("exynos_iris: smc: iris_s9_smc=%u: %s\n", iris_smc, iris_smc == 1 ? "PREPARE" :
	    (iris_smc == 2 ? "ISPPRE up, PREPARE" : "ISPPRE up, SYSREG_PROT(1), PREPARE"));
	answer = smc(SMCCC_VERSION, 0, 0, 0);
	say("exynos_iris: smc: SMCCC_VERSION (0x80000000) returned %#llx\n", answer);
	answer = smc(PSCI_VERSION, 0, 0, 0);
	say("exynos_iris: smc: PSCI_VERSION (0x84000000) returned %#llx (%llu.%llu)\n", answer, (answer >> 16) & 0xffff,
	    answer & 0xffff);
	pins_try("before PREPARE");
	if (iris_smc >= 2) {
		if (!isp_up()) {
			say("exynos_iris: smc: the ISPPRE domain did not come up: no PREPARE\n");
			isp_down();
			return;
		}
		isp_held = true;
		say("exynos_iris: smc: the ISPPRE domain up and held\n");
	}
	if (iris_smc >= 3) {
		answer = smc(SECCAM_SYSREG_PROT, 1, 0, 0);
		say("exynos_iris: smc: SYSREG_PROT(1) (0x82002132) returned %#llx\n", answer);
		if ((uint32_t)answer != 0) {
			return;
		}
		sysreg_protected = true;
	}
	answer = smc(SECCAM_PREPARE, 0, 0, 0);
	say("exynos_iris: smc: PREPARE (0x83000043) returned %#llx\n", answer);
	prepared = (uint32_t)answer == 0;
	pins_try("after PREPARE");
}

/* after the sensor is powered down: what smc_begin did, undone (also from loader_return: no sleeping) */
static void
smc_end(void)
{
	uint64_t answer;

	if (prepared) {
		prepared = false;
		answer = smc(SECCAM_UNPREPARE, 0, 0, 0);
		say("exynos_iris: smc: UNPREPARE (0x83000044) returned %#llx\n", answer);
	}
	if (sysreg_protected) {
		sysreg_protected = false;
		answer = smc(SECCAM_SYSREG_PROT, 0, 0, 0);
		say("exynos_iris: smc: SYSREG_PROT(0) returned %#llx\n", answer);
	}
	if (isp_held) {
		isp_held = false;
		isp_down();
	}
}

/*
 * fp_s9=N: a probe of the rear fingerprint sensor's bus, SPI1 (spi@10450000, USI00 of PERIC0, mode word
 * usi@10411004 "spi"). The device tree marks it "secure-mode", so Samsung's TA drives it from the secure world.
 * The question is whether the secure world also holds the block and its pins (gpp0-0..3 of PERIC0: CLK, MOSI,
 * MISO, CS).
 * N = 1 reads: USI00's gates in CMU_PERIC0 (a gate found manually off is opened for the reads and closed again),
 * gpp0's CON, DAT and PUD, SPI1's CH_CFG, MODE_CFG and STATUS, and last the mode word.
 * N = 2 also writes, reads back and restores MODE_CFG's TX_RDY_LVL [8:5] (only when both channels are off) and
 * gpp0-3's pull. Nothing is powered and no SMC is made. It runs after the iris steps, or alone.
 * On the phone, the read of SPI1's CH_CFG panics the kernel: a synchronous external abort (ESR 0x96000410), with
 * USI00's gates open and gpp0 reading 0. An earlier order aborted the same way on the mode word at SYSREG_PERIC0
 * (ESR 0x96000610), after the CMU reads. The SPI read and the mode-word read are each preceded by a log line, so
 * the log shows where an abort happened. Neither read succeeded, so the N = 2 writes and the sensor were never
 * reached.
 */

#define CMU_PERIC0_PA           EXYNOS_CMU_PERIC0_PA
#define SYSREG_PERIC0_PA        EXYNOS_SYSREG_PERIC0_PA
#define USI00_SW_CONF           0x1004          /* usi@10411004: 1 UART, 2 SPI, 4 I2C */
#define FP_SPI_PA               EXYNOS_PERIC0_USI00_SPI_PA    /* spi@10450000, SPI1 */
#define SPI_CH_CFG              0x00
#define SPI_MODE_CFG            0x08
#define SPI_STATUS              0x14
#define GPP0                    0x000           /* PERIC0: gpp0-0..3, the SPI's CLK, MOSI, MISO, CS */
#define GATE_AUTO               (1u << 28)

static const struct { uint32_t off; const char *name; } fp_gates[] = {
	{ 0x20B0, "PCLK" }, { 0x20AC, "IPCLK" }, { 0x200C, "GATE_CLK" },
};
#define NFPGATES                (sizeof(fp_gates) / sizeof(fp_gates[0]))

static uint32_t fp_level;
static volatile uint32_t iris_settled;

static void
fp_probe(void)
{
	vm_offset_t cmu, sysreg, spi, gpio;
	uint32_t found[NFPGATES], ch, mode, status;
	bool opened[NFPGATES] = { false }, mode_ok = false, pud_ok;

	say("exynos_iris: fp: fp_s9=%u: the rear fingerprint sensor's SPI1 (spi@10450000, USI00, \"secure-mode\")\n", fp_level);
	cmu = ml_io_map(CMU_PERIC0_PA, 0x4000);
	sysreg = ml_io_map(SYSREG_PERIC0_PA, 0x2000);
	spi = ml_io_map(FP_SPI_PA, 0x1000);
	gpio = peric0 != 0 ? peric0 : ml_io_map(GPIO_PERIC0_PA, 0x1000);
	if (cmu == 0 || sysreg == 0 || spi == 0 || gpio == 0) {
		say("exynos_iris: fp: a block could not be mapped\n");
		return;
	}
	if (peric0 == 0) {
		peric0 = gpio;
	}
	for (uint32_t i = 0; i < NFPGATES; i++) {
		found[i] = rd(cmu, fp_gates[i].off);
	}
	say("exynos_iris: fp: 1: CMU_PERIC0 USI00 PCLK %#x, IPCLK %#x, GATE_CLK %#x, divider %#x, Q-channel %#x; gpp0 CON %#x "
	    "DAT %#x PUD %#x\n", found[0], found[1], found[2], rd(cmu, 0x1804), rd(cmu, 0x3020), rd(gpio, GPP0),
	    rd(gpio, GPP0 + 4), rd(gpio, GPP0 + 8));
	for (uint32_t i = 0; i < NFPGATES; i++) {
		if ((found[i] & GATE_MANUAL) && !(found[i] & GATE_ON) && !(found[i] & GATE_AUTO)) {
			wr(cmu, fp_gates[i].off, found[i] | GATE_ON);
			opened[i] = true;
			say("exynos_iris: fp: 1: USI00's %s was off: opened for the reads (%#x)\n", fp_gates[i].name,
			    rd(cmu, fp_gates[i].off));
		}
	}
	/* The SPI registers are read before SYSREG_PERIC0's 0x10411004 (USI00's mode word), which aborted a read on the
	 * phone, so that the log shows what the SPI registers did first. */
	say("exynos_iris: fp: 1: reading SPI1's CH_CFG, MODE_CFG, STATUS (a panic right after this line: SPI1 is not the "
	    "normal world's)\n");
	ch = rd(spi, SPI_CH_CFG);
	mode = rd(spi, SPI_MODE_CFG);
	status = rd(spi, SPI_STATUS);
	say("exynos_iris: fp: 1: SPI1 CH_CFG %#x (TX %s, RX %s), MODE_CFG %#x, STATUS %#x\n", ch, (ch & 1) ? "on" : "off",
	    (ch & 2) ? "on" : "off", mode, status);
	if (fp_level >= 2) {
		if ((ch & 3) == 0) {
			mode_ok = pin_try(spi, SPI_MODE_CFG, 5, 1, "fp: SPI1 MODE_CFG TX_RDY_LVL");
		} else {
			say("exynos_iris: fp: 2: a channel is on: MODE_CFG not tried\n");
		}
		pud_ok = pin_try(gpio, GPP0 + 8, 4 * 3, 1, "fp: gpp0-3 PUD (the CS's pull)");
		say("exynos_iris: fp: 2: SPI1's MODE_CFG %s, gpp0-3's pull %s: the bus is %s\n",
		    (ch & 3) ? "not tried" : (mode_ok ? "takes writes" : "IGNORES writes"),
		    pud_ok ? "takes writes" : "IGNORES writes",
		    (mode_ok && pud_ok) ? "the normal world's (writes stick)" :
		    ((mode_ok || pud_ok) ? "partly held" : "HELD (the secure world's, most likely)"));
	}
	for (uint32_t i = 0; i < NFPGATES; i++) {
		if (opened[i]) {
			wr(cmu, fp_gates[i].off, found[i]);
		}
	}
	say("exynos_iris: fp: 3: reading USI00's mode word, SYSREG_PERIC0 0x10411004 (it aborted on 3 November: a panic "
	    "after this line is that again)\n");
	say("exynos_iris: fp: 3: USI00's mode word %#x (2: SPI)\n", rd(sysreg, USI00_SW_CONF));
	say("exynos_iris: fp: done\n");
}

static void
fp_thread(__unused void *arg, __unused wait_result_t result)
{
	while (!__atomic_load_n(&iris_settled, __ATOMIC_ACQUIRE) && !leaving) {
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	if (!leaving) {
		fp_probe();
	}
	thread_terminate(current_thread());
}

static uint32_t shots_taken;
static uint64_t shot_last;
static volatile uint32_t shot_ready, shot_asked, shot_n;
static bool shots_enabled, shots_closed;

/* steps 2 to 4 once: the sensor up, its frame, all down again; true with a frame in the buffer */
static bool
take(bool with_frame)
{
	bool ok;
	uint64_t gap;

	if (shots_taken >= SHOTS_MAX) {
		say("exynos_iris: %u captures this run: no more (the LED's budget)\n", SHOTS_MAX);
		return false;
	}
	nanoseconds_to_absolutetime((uint64_t)SHOT_GAP_MS * NSEC_PER_MSEC, &gap);
	while (shot_last != 0 && mach_absolute_time() < shot_last + gap) {
		delay_for_interval(50, NSEC_PER_MSEC);
	}
	if (!sensor_up()) {
		return false;
	}
	sensor_otp();
	ok = true;
	if (with_frame) {
		shots_taken++;
		ok = capture();
		shot_last = mach_absolute_time();
	}
	isp_down();
	power_down();
	return ok;
}

const char *hakios_exynos_iris_shot(uint32_t n);

/* asked by the USB console's "irshot [N]" (hakios.dwc3.c), from its thread call: never waits. NULL: taken */
const char *
hakios_exynos_iris_shot(uint32_t n)
{
	uint32_t ready = 1;

	if (shots_closed) {
		return "the run is ending";
	}
	if (n < 2 || n > 8) {
		return "N is a number from 2 to 8 (irshot 2: 1200 x 1200, every second pixel)";
	}
	if (!shots_enabled) {
		return "not in this run: boot-arg iris_s9=5 (and a kernel with the S9's iris driver, CONFIG_HAKIOS_EXYNOS_IRIS)";
	}
	if (!__atomic_compare_exchange_n(&shot_ready, &ready, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return "the iris camera is busy (the boot's run or the last irshot is not finished, or a step failed for good)";
	}
	shot_n = n;
	__atomic_store_n(&shot_asked, 1, __ATOMIC_RELEASE);
	return NULL;
}

/*
 * The run, in the boot-arg's order. Each step is logged before it is taken.
 *   1  read the pins, CIS_CLK2, the three LDOs through ACPM, and the S2MPB02's ID and FLED registers (0x00, 0x12-0x1B)
 *   2  power the sensor up, read its ID, version and OTP, power it down
 *   3  set the LED's seven registers to the kernel's values and read them back. Nothing strobes.
 *   4  take one frame: the ISPPRE domain and clocks, CSIS3 and its D-PHY, VC0's WDMA on the buffer; the sensor up and
 *      streaming until the second frame end, for at most iris_s9_ms; streaming off and everything down; then the
 *      link's counters, how much of the buffer changed, and the mean and spread of the middle 256 x 256
 *   5  stay after step 4 for the USB console's "irshot [N]" (hakios.dwc3.c, usb_gadget=5). Each request repeats
 *      steps 2 to 4 and sends the frame as a grey picture of every Nth pixel (N 2 to 8, default 2). tools/usb-shot.py
 *      --cam fetches it.
 */
static void
iris_thread(void *arg, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg, w, h;
	uint8_t v;

	say("exynos_iris: iris_s9=%u: the S9's iris camera (S5K5F1 on CSIS3) and its IR LED; exposure %u lines, gain %#x, "
	    "%u ms a capture at most\n", steps, iris_exp, iris_gain, iris_ms);
	peric0 = ml_io_map(GPIO_PERIC0_PA, 0x1000);
	peric1 = ml_io_map(GPIO_PERIC1_PA, 0x1000);
	cmu_top = ml_io_map(CMU_TOP_PA, 0x4000);
	if (peric0 == 0 || peric1 == 0 || cmu_top == 0 || !hakios_hsi2c_map(&sensor_bus, say) ||
	    !hakios_hsi2c_map(&pb02_bus, say)) {
		say("exynos_iris: a block could not be mapped\n");
		goto out;
	}
	say("exynos_iris: 1: look: CIS_CLK2 mux %#x divider %#x gate %#x Q-channel %#x\n", rd(cmu_top, CIS2_MUX),
	    rd(cmu_top, CIS2_DIV), rd(cmu_top, CIS2_GATE), rd(cmu_top, CIS2_QCH));
	look_pins("1");
	if (!hakios_acpm_init(say)) {
		say("exynos_iris: 1: ACPM is not there: the supplies cannot be reached\n");
		goto out;
	}
	for (uint32_t i = 0; i < NSLDOS; i++) {
		v = 0;
		(void)sldo_read(i, &v);
		say("exynos_iris: 1: %s: %#x (on: [7:6] = 3; Android's voltage code %#x)\n", sldos[i].name, v, sldos[i].vsel);
	}
	hakios_hsi2c_power(&pb02_bus);
	hakios_hsi2c_reset(&pb02_bus);
	pb02_ready = true;
	led_look("1");
	if (iris_smc != 0) {
		smc_begin();
	}
	if (steps < 2) {
		goto out;
	}
	say("exynos_iris: 2: the sensor up, its ID, down again\n");
	if (!take(false) || steps < 3) {
		goto out;
	}
	say("exynos_iris: 3: the LED's seven registers to the kernel's values, read back\n");
	if (!led_setup("3")) {
		goto out;
	}
	led_look("3");
	if (steps < 4) {
		goto out;
	}
	say("exynos_iris: 4: one frame\n");
	if (!take(true) || steps < 5) {
		goto out;
	}
	shots_enabled = true;
	__atomic_store_n(&iris_settled, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&shot_ready, 1, __ATOMIC_RELEASE);
	say("exynos_iris: 5: ready for the USB console's irshot [N]\n");
	while (!leaving) {
		if (__atomic_load_n(&shot_asked, __ATOMIC_ACQUIRE)) {
			__atomic_store_n(&shot_asked, 0, __ATOMIC_RELEASE);
			if (take(true)) {
				picture(shot_n, &w, &h);
				if (hakios_usb_picture((uintptr_t)(buffer + RGB_OFF), w, h, "the iris camera (infrared, grey)") != 0) {
					say("usb: irshot failed: the USB link did not take the picture\n");
				}
				/* the picture's memory stays as it is until the link has sent it; the next capture waits 2 s */
			} else {
				say("usb: irshot failed: the capture failed (the log says why)\n");
			}
			if (!broken && !buffer_busy) {
				__atomic_store_n(&shot_ready, 1, __ATOMIC_RELEASE);
			}
		}
		delay_for_interval(20, NSEC_PER_MSEC);
	}
out:
	isp_down();
	power_down();
	smc_end();
	say("exynos_iris: the run's steps are done\n");
	__atomic_store_n(&iris_settled, 1, __ATOMIC_RELEASE);
	thread_terminate(current_thread());
}

/* loader_return(): whatever is up comes down, the sensor first (no sleeping) */
void
hakios_exynos_iris_quiesce(void)
{
	shots_closed = true;
	if (!powered && !domain_mine && !phy_mine && !link_on && !dma_on && !prepared && !sysreg_protected && !isp_held) {
		return;
	}
	leaving = true;
	stream_stop();
	isp_down();
	power_down();
	smc_end();
	say("exynos_iris: quiesced\n");
}

void
hakios_exynos_iris_init(void)
{
	uint32_t steps = 0, other = 0;
	thread_t thread;

	(void)PE_parse_boot_argn("iris_s9", &steps, sizeof(steps));
	(void)PE_parse_boot_argn("fp_s9", &fp_level, sizeof(fp_level));
	fp_level = fp_level > 2 ? 2 : fp_level;
	if (steps == 0 && fp_level == 0) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != EXYNOS_SOC_BASE_PA) {
		say("exynos_iris: not an Exynos 9810 (the SoC's registers are not at 0x10000000): nothing done\n");
		return;
	}
	if (fp_level != 0) {
		if (steps == 0) {
			iris_settled = 1;
		}
		if (kernel_thread_start(fp_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			say("exynos_iris: fp: the thread could not be started\n");
		}
		if (steps == 0) {
			return;
		}
	}
	if ((PE_parse_boot_argn("cam_s9", &other, sizeof(other)) && other != 0) ||
	    (PE_parse_boot_argn("torch", &other, sizeof(other)) && other != 0)) {
		say("exynos_iris: iris_s9=%u is not run with cam_s9= or torch= (they own the S2MPB02's bus and the ISPPRE "
		    "domain): take them out (cam_s9=)\n", steps);
		__atomic_store_n(&iris_settled, 1, __ATOMIC_RELEASE);
		return;
	}
	PE_parse_boot_argn("iris_s9_exp", &iris_exp, sizeof(iris_exp));
	PE_parse_boot_argn("iris_s9_gain", &iris_gain, sizeof(iris_gain));
	PE_parse_boot_argn("iris_s9_ms", &iris_ms, sizeof(iris_ms));
	PE_parse_boot_argn("iris_s9_ispdiv", &iris_ispdiv, sizeof(iris_ispdiv));
	PE_parse_boot_argn("iris_s9_smc", &iris_smc, sizeof(iris_smc));
	iris_smc = iris_smc > 3 ? 3 : iris_smc;
	iris_gain = iris_gain < 0x20 ? 0x20 : (iris_gain > 0x200 ? 0x200 : iris_gain);
	iris_ms = iris_ms < 100 ? 100 : (iris_ms > 2000 ? 2000 : iris_ms);
	iris_ispdiv = iris_ispdiv > 3 ? 3 : iris_ispdiv;
	if (kernel_thread_start(iris_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		say("exynos_iris: the thread could not be started\n");
		__atomic_store_n(&iris_settled, 1, __ATOMIC_RELEASE);
	}
}
