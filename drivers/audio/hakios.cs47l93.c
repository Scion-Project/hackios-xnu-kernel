/*
 * The Galaxy S9's audio codec: a Cirrus Logic CS47L93 (Cirrus's Madera family, with the CS47L92), on SPI. The
 * headphones, the jack and the microphones go through it; the speakers do not (ABOX's UAIF1 drives the two MAX98512s
 * directly: hakios.exynos_abox.c). Only with boot-arg cs47l93=N on this phone (the SoC's registers at 0x10000000).
 *
 * WHERE IT IS (the S9's device tree, exynos9810-starlte_eur_open_26.dts, spi@10470000 and cs47l93@0):
 *   the bus      spi@10470000, "samsung,exynos-spi", a 64-byte FIFO, swap-mode; USI01 of PERIC0, whose mode word is
 *                usi@1041100c (usi_v2_mode "spi": SYSREG_PERIC0 +0x100C); its clocks USI01 of CMU_PERIC0 (0x10400000):
 *                GATE_CLK_PERIC0_USI01_USI +0x2010, RSTnSYNC_CLK_PERIC0_USI01_USI +0x205C, USI01_USI's IPCLK +0x20BC
 *                and PCLK +0x20C0, the Q-channel +0x3028, the divider DIV_CLK_PERIC0_USI01_USI +0x1808 [3:0] (under
 *                MUX_CLKCMU_PERIC0_IP_USER +0x0120); the pins CLK, MISO, MOSI gpp0-4..6 (spi2-bus: function 2, no pull)
 *                and nSS gpp0-7 (spi2-cs-func: function 2, pull-up), PERIC0's GPIO (0x10430000, bank gpp0 at +0)
 *   the codec    chip select 0, spi-max-frequency 25 MHz, samsung,spi-chip-select-mode 0 (the controller's nSS driven
 *                by hand), feedback delay 1; reset-gpios gpg1-0 (active high: low holds the codec in reset; codec-reset:
 *                function 1, no pull), PERIC0's GPIO bank gpg1 at +0xA0; its interrupt gpa2-2 (not used here)
 *   supplies     AVDD, DBVDD1, CPVDD1 from LDO33 of the S2MPS18 (vdd_pmic_codec_1p8, 1.8 V, always on); CPVDD2 and
 *                DCVDD from LDO34 (vdd_codec_1p2, 1.2 V, off until asked; enable ramp 5 ms). The S2MPS18's L33CTRL is
 *                0x6A and L34CTRL 0x6B (its PMIC block): [7:6] 3 on, [5:0] the voltage, LDO34's 0.7 V + 12.5 mV a step
 *                (S2MPS18_LDO_MIN1, _STEP1: 1.2 V is 40)
 *
 * THE SPI WORDS (Cirrus's madera-spi.c and cs47l92-tables.c: cs47l92_16bit_spi_regmap): a 32-bit register address,
 * 16 bits of padding, a 16-bit value, all big-endian; a read sets the address's top bit (regmap's SPI read flag, 0x80
 * on its first byte). Register 0x0000 (SOFTWARE_RESET) reads the device's ID, 0x6371 for the CS47L92/93 (the S9's
 * kernel's madera-core.c, CS47L92_SILICON_ID); 0x0001 the revision; IRQ1_RAW_STATUS_1 0x1880 bit 7 BOOT_DONE_STS1.
 *
 * THE ORDER (madera_dev_init in the S9's kernel): the reset held (the GPIO requested low), 20 ms, the core supplies on
 * (LDO33: already), DCVDD on (LDO34, then its 5 ms ramp), the reset let go and 1-2 ms, the ID read and checked before
 * anything is written, then BOOT_DONE polled (every 5 ms, 25 ms at most there) and acknowledged in IRQ1_STATUS_1 (0x1800).
 *
 * THE STEPS. cs47l93=N makes the first N; 0 or none leaves the codec alone (in reset, LDO34 off, as the boot leaves it).
 *   1  look    the bus's clocks and pins, the reset pin and both supplies (LDO33, LDO34) read; nothing written
 *   2  power   the SPI's clocks, mode and pins; the reset held 20 ms; LDO34 to 1.2 V and on (read back); 5 ms; the
 *              reset let go; the ID (0x0000) and the revision read; BOOT_DONE waited for and acknowledged
 *   3  jack    the jack detection on (JD1, JD2, the clamp in mode 8) and its state read
 *   4  clocks  MCLK1 (the SoC's CLKOUT0, 26 MHz) on; FLL1 from it to 49.152 MHz, locked; SYSCLK 98.304 MHz from FLL1;
 *              SAMPLE_RATE_1 48 kHz
 *   5  mic     MICVDD 3.0 V, MICBIAS1 1.8 V and its switch 1A; IN3L (the main microphone, digital) on
 *   6  AIF1    AIF1 a slave I2S of 2 x 16 bits at 48 kHz, sending IN3L on both slots; the headphones' mixers from AIF1RX1/2
 *              (the output itself stays off)
 *
 * Boot-args:
 *   cs47l93=N          the first N steps (1 looks, 2 powers the codec and reads its ID, 6 makes the microphone ready);
 *                      0 or none: left alone
 *   cs47l93_mic_vol=V  IN3L's digital volume, 0x80 0 dB, 0.5 dB a step, at most 0xBF (0x90, +8 dB)
 *   cs47l93_div=D      the SPI's IP clock divider, 0 to 15 (15: 400 MHz / 16 = 25 MHz, the bus at a quarter of it,
 *                      6.25 MHz); 255 leaves it as the boot has it
 *   cs47l93_hp=1       the headphones (THE HEADPHONES below): Cirrus's register patch at power-up, then, once the six
 *                      steps are done, the jack watched and the headphone output (OUT2) switched on while something is
 *                      plugged in, with iOS's sound taken from the speakers to it (ABOX, abox=8). 0 or none: the
 *                      output is never switched on (as before)
 *   cs47l93_hp_vol=V   OUT2's digital volume, 0x80 0 dB, 0.5 dB a step, at most 0x80 (0x68: -12 dB); iOS's own volume
 *                      scales the samples before it
 *
 * THE USB CONSOLE. "codec" says the state; "codec r REG [N]" reads N 16-bit registers from REG; "codec w REG VALUE"
 * writes one (the codec must have been powered: step 2); "codec power" makes steps 1-2 again, "codec mic" steps 1-6.
 *
 * THE HEADPHONES (cs47l93_hp=1; NOT tried with headphones yet: the owner's test is in hakios-starlte's AUDIO.md).
 * Cirrus's own order (the S9's kernel: cs47l92-tables.c cs47l92_patch(), madera.c madera_hp_ev() and madera_out_ev(),
 * cs47l92.c's DAPM for OUT2, mixer_paths.xml's route-headphone):
 *   power-up   after BOOT_DONE (step 2) the chip's register patch, as madera_dev_init() writes it before anything else:
 *              the 16-bit one (cs47l92_reva_16_patch) and the 32-bit one, the write sequencer's steps for the outputs
 *              (cs47l92_reva_32_patch, 0x3030-0x31D8: the 32-bit register map, a 32-bit value after the padding)
 *   the jack   once step 6 is done, a thread reads IRQ1_RAW_STATUS_7 every 100 ms (MICD_CLAMP_RISE 0: plugged in, as
 *              step 3) and takes a change after it has read the same three times (300 ms)
 *   plugged in ABOX first (hakios_abox_headphones(true): the amplifiers stopped, iOS's samples on UAIF0 into AIF1RX1/2,
 *              which step 6 has mixed into OUT2L/R at 0 dB); then OUT2: its digital volume (cs47l93_hp_vol) set muted,
 *              OUTPUT_ENABLES_1's OUT2L_ENA and OUT2R_ENA (the chip's write sequencer brings the output stage up), 6 ms
 *              for each output (madera_out_ev's out_up_delay for the CS47L92/93), OUTPUT_STATUS_1 read, then unmuted
 *              (OUT_VU taking both sides at once)
 *   pulled out OUT2 muted, its enables cleared, 1 ms for each output (out_down_delay); then ABOX given the sound back
 *              (the speakers' again)
 *   Every change is told to /dev/hakios-audio (hakios_audio_set_route()), whose HAKIOS_AUDIO_GET_ROUTE the plug-in in
 *   the audio server watches, to tell the HAL that the speaker's jack has changed.
 * Not done: the headphones' impedance (Cirrus's HPDET, which sets the output's gain for low-impedance ones), the
 * headset's microphone (IN1BL) and its buttons, the clamp madera's extcon puts on the output while a jack goes in and
 * out (a click is possible; the output is muted and off within 300 ms of the jack leaving).
 *
 * WHAT IS NOT DONE (yet): the other microphones (IN4L, the headset's IN1BL), the jack's microphone and impedance detection, Cirrus's register patch
 * for the chip's revision (cs47l92-tables.c). ABOX's side of the capture (UAIF0 -> WDMA1) is hakios.exynos_abox.c's
 * "abox rec". Nothing is ever written to the PMIC but LDO34's control register, and that only in step 2, read back
 * each time.
 *
 * Sources: the S9's kernel (github.com/ExyHyperBrick/android_kernel_samsung_exynos9810, lineage-24.0: the device tree,
 * drivers/mfd/madera-core.c, madera-spi.c, cs47l92-tables.c, include/linux/mfd/madera/registers.h,
 * include/linux/mfd/samsung/s2mps18-private.h, drivers/regulator/s2mps18.c, drivers/soc/samsung/cal-if/exynos9810/
 * cmucal-sfr.c and cmucal-node.c, drivers/pinctrl/samsung/pinctrl-exynos.c), GPL-2.0, read for facts and not copied.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/errno.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.cs47l93.h>
#include <sys/hakios.exynos_abox.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/hakios.exynos_spi.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_line(const char *line);
extern void delay(int usec);

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

#define STARLTE_SOC_BASE        0x10000000UL
#define STEPS                   6

/* the S2MPS18's PMIC block (HAKIOS_ACPM_PMIC_PMIC) */
#define L33CTRL                 0x6A    /* vdd_pmic_codec_1p8: AVDD, DBVDD1, CPVDD1 */
#define L34CTRL                 0x6B    /* vdd_codec_1p2: DCVDD, CPVDD2 */
#define LDO_ON                  0xC0    /* [7:6] 3 */
#define LDO_VSEL                0x3F
#define L34_1V2                 40      /* (1200000 - 700000) / 12500 */
#define L34_RAMP_US             5000    /* regulator-enable-ramp-delay */

/* PERIC0's GPIO: bank gpg1 (the reset, pin 0) */
#define GPIO_PERIC0_PA          0x10430000UL
#define GPG1                    0xA0
#define RESET_PIN               0

/* the codec's registers (madera/registers.h) */
#define SOFTWARE_RESET          0x0000
#define HARDWARE_REVISION       0x0001
#define IRQ1_STATUS_1           0x1800
#define IRQ1_RAW_STATUS_1       0x1880
#define  BOOT_DONE              0x0080

static struct hakios_exynos_spi bus = {
	.name = "cs47l93",
	.regs_pa = 0x10470000,                  /* spi@10470000: USI01 of PERIC0 */
	.usi_mode_pa = 0x1041100C,              /* usi@1041100c: its mode word */
	.cmu_pa = 0x10400000, .cmu_size = 0x4000,       /* CMU_PERIC0 */
	.gates = { 0x2010, 0x205C, 0x20BC, 0x20C0 }, .ngates = 4,
	.qch = 0x3028, .div = 0x1808, .mux = 0x0120,
	.div_ratio = 15, .ipclk_mhz = 400,      /* 400 MHz / 16 = 25 MHz, SCLK 6.25 MHz (the codec takes up to 25) */
	.pins_pa = GPIO_PERIC0_PA,              /* bank gpp0: CLK, MISO, MOSI pins 4-6, nSS pin 7 */
	.pins_mask = 0xffffu << 16, .pins_want = 0x2222u << 16,
	.pud_mask = 0xffffu << 16, .pud_want = 0x3000u << 16,   /* nSS pulled up, the others not */
	.fifo_bytes = 64, .mode = 0, .fb_delay = 1,
};

static uint32_t steps, step;
static volatile uint32_t busy;
static bool powered;                    /* step 2 got the ID right */
static uint16_t id, revision;
static vm_offset_t gpio;

/* ---- the codec's registers over SPI ------------------------------------------------------------------------------- */

/* one transfer at a time: the boot's run, the USB console and the headphones' thread all use the bus */
static LCK_GRP_DECLARE(codec_lock_group, "hakios-cs47l93");
static LCK_MTX_DECLARE(spi_lock, &codec_lock_group);

static uint32_t
transfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	uint32_t error;

	lck_mtx_lock(&spi_lock);
	hakios_exynos_spi_select(&bus, true);
	error = hakios_exynos_spi_transfer(&bus, tx, rx, len);
	hakios_exynos_spi_select(&bus, false);
	lck_mtx_unlock(&spi_lock);
	return error;
}

static int
reg_read(uint32_t reg, uint16_t *value)
{
	uint8_t tx[8] = { (uint8_t)(0x80 | (reg >> 24)), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg, 0, 0, 0, 0 };
	uint8_t rx[8] = { 0 };

	if (transfer(tx, rx, sizeof(tx))) {
		return EIO;
	}
	*value = (uint16_t)(rx[6] << 8 | rx[7]);
	return 0;
}

static int
reg_write(uint32_t reg, uint16_t value)
{
	uint8_t tx[8] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg, 0, 0,
		          (uint8_t)(value >> 8), (uint8_t)value };

	return transfer(tx, NULL, sizeof(tx)) ? EIO : 0;
}

/* the 32-bit registers (0x3000 up: the write sequencer, the DSPs; cs47l92_32bit_spi_regmap): the same, a 32-bit value */
static int
reg_read32(uint32_t reg, uint32_t *value)
{
	uint8_t tx[10] = { (uint8_t)(0x80 | (reg >> 24)), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg };
	uint8_t rx[10] = { 0 };

	if (transfer(tx, rx, sizeof(tx))) {
		return EIO;
	}
	*value = (uint32_t)rx[6] << 24 | (uint32_t)rx[7] << 16 | (uint32_t)rx[8] << 8 | rx[9];
	return 0;
}

static int
reg_write32(uint32_t reg, uint32_t value)
{
	uint8_t tx[10] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg, 0, 0,
		           (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value };

	return transfer(tx, NULL, sizeof(tx)) ? EIO : 0;
}

/* ---- the reset pin and the supplies --------------------------------------------------------------------------------- */

static uint32_t
gpio_rd(uint32_t off)
{
	return *(volatile uint32_t *)(gpio + off);
}

static void
gpio_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(gpio + off) = v;
}

/* the reset pin an output at `high` (low: the codec held in reset), the level set before the direction */
static void
reset_pin(bool high)
{
	uint32_t dat = gpio_rd(GPG1 + 4);

	gpio_wr(GPG1 + 4, high ? (dat | 1u << RESET_PIN) : (dat & ~(1u << RESET_PIN)));
	gpio_wr(GPG1 + 8, gpio_rd(GPG1 + 8) & ~(0xfu << (4 * RESET_PIN)));                     /* no pull */
	gpio_wr(GPG1, (gpio_rd(GPG1) & ~(0xfu << (4 * RESET_PIN))) | 1u << (4 * RESET_PIN));    /* function 1, output */
}

static bool
ldo_read(uint8_t reg, uint8_t *v)
{
	if (hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, reg, v) != 0) {
		say("cs47l93: the PMIC's register %02x could not be read\n", reg);
		return false;
	}
	return true;
}

/* LDO34, DCVDD, to 1.2 V and on: nothing else of the PMIC is ever written here, and every write is read back */
static bool
dcvdd_on(void)
{
	uint8_t v = 0;

	if (!ldo_read(L34CTRL, &v)) {
		return false;
	}
	if ((v & LDO_VSEL) != L34_1V2) {
		if (v & LDO_ON) {
			say("cs47l93: LDO34 reads %02x: on at another voltage than 1.2 V; left alone\n", v);
			return false;
		}
		if (hakios_acpm_pmic_update(HAKIOS_ACPM_PMIC_PMIC, L34CTRL, L34_1V2, LDO_VSEL) != 0 || !ldo_read(L34CTRL, &v) ||
		    (v & LDO_VSEL) != L34_1V2) {
			say("cs47l93: LDO34's voltage could not be set to 1.2 V (reads %02x)\n", v);
			return false;
		}
	}
	if ((v & LDO_ON) != LDO_ON) {
		if (hakios_acpm_pmic_update(HAKIOS_ACPM_PMIC_PMIC, L34CTRL, LDO_ON, LDO_ON) != 0 || !ldo_read(L34CTRL, &v) ||
		    (v & LDO_ON) != LDO_ON) {
			say("cs47l93: LDO34 could not be switched on (reads %02x)\n", v);
			return false;
		}
	}
	say("cs47l93: LDO34 (DCVDD, 1.2 V) reads %02x: on\n", v);
	return true;
}

/* ---- the steps ------------------------------------------------------------------------------------------------------ */

static bool
step_look(void)
{
	uint8_t l33 = 0, l34 = 0;

	if (!hakios_exynos_spi_map(&bus, say)) {
		return false;
	}
	gpio = ml_io_map(GPIO_PERIC0_PA, 0x1000);
	if (!gpio) {
		say("cs47l93: PERIC0's GPIO could not be mapped\n");
		return false;
	}
	say("cs47l93: the reset pin gpg1-0: CON %08x DAT %08x PUD %08x (CON [3:0] 1 output, DAT bit 0 low: in reset)\n",
	    gpio_rd(GPG1), gpio_rd(GPG1 + 4), gpio_rd(GPG1 + 8));
	if (!hakios_acpm_init(say)) {
		say("cs47l93: no ACPM: the codec's supplies cannot be read\n");
		return false;
	}
	if (!ldo_read(L33CTRL, &l33) || !ldo_read(L34CTRL, &l34)) {
		return false;
	}
	say("cs47l93: LDO33 (1.8 V: AVDD, DBVDD1, CPVDD1) reads %02x (%s); LDO34 (1.2 V: DCVDD, CPVDD2) reads %02x (%s, "
	    "voltage field %u: %u mV)\n", l33, (l33 & LDO_ON) == LDO_ON ? "on" : "NOT on", l34,
	    (l34 & LDO_ON) == LDO_ON ? "on" : "off", l34 & LDO_VSEL, 700 + (l34 & LDO_VSEL) * 25 / 2);
	if ((l33 & LDO_ON) != LDO_ON) {
		say("cs47l93: LDO33 is not on (the device tree has it always on): stopping\n");
		return false;
	}
	return true;
}

/* ---- Cirrus's register patch for the chip (cs47l93_hp=1) --------------------------------------------------------------- */

static uint32_t hp_enabled;             /* cs47l93_hp */
static uint32_t hp_vol = 0x68;          /* cs47l93_hp_vol: -12 dB */

/* the S9 kernel's cs47l92_reva_16_patch, in its order (0x120 twice, as there) */
static const uint16_t patch16[][2] = {
	{ 0x3A2, 0x2C29 }, { 0x3A3, 0x0E00 }, { 0x281, 0x0000 }, { 0x282, 0x0000 }, { 0x4EA, 0x0100 }, { 0x22B, 0x0000 },
	{ 0x4A0, 0x0080 }, { 0x4A1, 0x0000 }, { 0x4A2, 0x0000 }, { 0x180B, 0x033F }, { 0x190B, 0x033F }, { 0x442, 0x0304 },
	{ 0x34C, 0x0003 }, { 0x124, 0x0C49 }, { 0x120, 0x0345 }, { 0x120, 0x0305 }, { 0x4FA, 0x5064 }, { 0x1300, 0x050E },
	{ 0x1302, 0x0101 }, { 0x1380, 0x02E0 }, { 0x1381, 0xF942 }, { 0x1382, 0x04CE }, { 0x1383, 0xFF06 }, { 0x1390, 0x0304 },
	{ 0x1391, 0xF8FF }, { 0x1392, 0x04F3 }, { 0x1393, 0xFF00 }, { 0x13A0, 0x02E0 }, { 0x13A1, 0xF942 }, { 0x13A2, 0x04CE },
	{ 0x13A3, 0xFF06 }, { 0x13B0, 0x0304 }, { 0x13B1, 0xF8FF }, { 0x13B2, 0x04F3 }, { 0x13B3, 0xFF00 }, { 0x412, 0x0005 },
	{ 0x41A, 0x0005 }, { 0x422, 0x0005 },
};
/* ... and cs47l92_reva_32_patch: the write sequencer's steps for OUT1-3 (the address, then the 32-bit value) */
static const uint32_t patch32[][2] = {
	{ 0x3030, 0x04A00C01 }, { 0x3032, 0x0225F501 }, { 0x3044, 0x04A00C00 }, { 0x3046, 0x0225FF01 }, { 0x3080, 0x04A00C01 },
	{ 0x3082, 0x0226F501 }, { 0x3094, 0x04A00C00 }, { 0x3096, 0x0226FF01 }, { 0x30D1, 0x04A10C01 }, { 0x30D2, 0x0227F501 },
	{ 0x30E4, 0x04A10C00 }, { 0x30E6, 0x0227FF01 }, { 0x3120, 0x04A10C01 }, { 0x3122, 0x0228F501 }, { 0x3134, 0x04A10C00 },
	{ 0x3136, 0x0228FF01 }, { 0x3170, 0x04A20C01 }, { 0x3172, 0x022B0101 }, { 0x3174, 0x0229F501 }, { 0x3184, 0x04A20C00 },
	{ 0x3186, 0x022B0100 }, { 0x3188, 0x0229FF01 }, { 0x31C0, 0x04A20C01 }, { 0x31C2, 0x022B0001 }, { 0x31C4, 0x022AF501 },
	{ 0x31D4, 0x04A20C00 }, { 0x31D6, 0x022B0000 }, { 0x31D8, 0x022AFF01 },
};

/* both patches written, as madera_dev_init() writes them right after the boot; what reads back otherwise is said */
static void
chip_patch(void)
{
	uint32_t differ = 0;

	for (unsigned i = 0; i < sizeof(patch16) / sizeof(patch16[0]); i++) {
		uint16_t back = 0;

		if (reg_write(patch16[i][0], patch16[i][1]) != 0 || reg_read(patch16[i][0], &back) != 0 || back != patch16[i][1]) {
			say("cs47l93: the patch: %04x <- %04x reads %04x\n", patch16[i][0], patch16[i][1], back);
			differ++;
		}
	}
	for (unsigned i = 0; i < sizeof(patch32) / sizeof(patch32[0]); i++) {
		uint32_t back = 0;

		if (reg_write32(patch32[i][0], patch32[i][1]) != 0 || reg_read32(patch32[i][0], &back) != 0 || back != patch32[i][1]) {
			say("cs47l93: the patch: %04x <- %08x reads %08x\n", patch32[i][0], patch32[i][1], back);
			differ++;
		}
	}
	say("cs47l93: Cirrus's register patch written (%u 16-bit, %u 32-bit registers), %u not read back as written (some "
	    "read differently by design: the list above)\n", (unsigned)(sizeof(patch16) / sizeof(patch16[0])),
	    (unsigned)(sizeof(patch32) / sizeof(patch32[0])), differ);
}

static bool
step_power(void)
{
	uint16_t raw = 0;
	uint64_t start, now;

	hakios_exynos_spi_power(&bus);
	hakios_exynos_spi_reset(&bus);
	hakios_exynos_spi_map(&bus, say);       /* says what the clocks and pins read now */
	reset_pin(false);
	say("cs47l93: the reset held (gpg1-0 low: CON %08x DAT %08x); 20 ms\n", gpio_rd(GPG1), gpio_rd(GPG1 + 4));
	delay(20000);
	if (!dcvdd_on()) {
		return false;
	}
	delay(L34_RAMP_US);
	reset_pin(true);
	delay(2000);
	if (reg_read(SOFTWARE_RESET, &id) != 0 || reg_read(HARDWARE_REVISION, &revision) != 0) {
		say("cs47l93: the ID could not be read over SPI\n");
		hakios_exynos_spi_report(&bus, say);
		return false;
	}
	say("cs47l93: ID %04x (%s), revision %04x\n", id, id == HAKIOS_CS47L93_ID ? "a CS47L92/93" : "NOT a CS47L92/93", revision);
	hakios_exynos_spi_report(&bus, say);
	if (id != HAKIOS_CS47L93_ID) {
		return false;
	}
	start = mach_absolute_time();
	for (int i = 0; i < 20; i++) {
		if (reg_read(IRQ1_RAW_STATUS_1, &raw) == 0 && (raw & BOOT_DONE)) {
			break;
		}
		delay(5000);
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &now);
	if (!(raw & BOOT_DONE)) {
		say("cs47l93: BOOT_DONE not seen in 100 ms (IRQ1_RAW_STATUS_1 %04x)\n", raw);
		return false;
	}
	(void)reg_write(IRQ1_STATUS_1, BOOT_DONE);      /* BOOT_DONE is unmasked after a reset: acknowledged */
	say("cs47l93: booted (IRQ1_RAW_STATUS_1 %04x) within %llu us\n", raw, now / 1000);
	powered = true;
	if (hp_enabled) {
		chip_patch();           /* (only with the headphones: what the microphone was verified with stays as it was) */
	}
	return true;
}

/* ---- step 3: the jack ---------------------------------------------------------------------------------------------- */

/*
 * The S9's jack detection (cirrus,accdet acc@1: cirrus,jd-use-jd2, cirrus,micd-clamp-mode 8): both JD inputs on and
 * debounced, the clamp in mode 8 (JD1 low and JD2 low); then a jack is in when IRQ1_RAW_STATUS_7's MICD_CLAMP_RISE reads
 * 0 (extcon-madera.c, madera_jds_get_status / madera_extcon_probe). Nothing here drives an output.
 */
#define MICD_CLAMP_CONTROL      0x02C6  /* MICD_CLAMP_MODE [3:0] */
#define JACK_DETECT_ANALOGUE    0x02D3  /* JD1_ENA 0, JD2_ENA 1 */
#define INTERRUPT_DEBOUNCE_7    0x1A06  /* JD1_DB 0, JD2_DB 2 */
#define IRQ1_RAW_STATUS_7       0x1886  /* JD1_RISE 0, JD1_FALL 1, MICD_CLAMP_RISE 4, MICD_CLAMP_FALL 5 */
#define  CLAMP_RISE             0x0010

static bool jack_in;

static int
update(uint32_t reg, uint16_t mask, uint16_t value)
{
	uint16_t v = 0;

	if (reg_read(reg, &v) != 0) {
		return EIO;
	}
	return reg_write(reg, (uint16_t)((v & ~mask) | (value & mask)));
}

/* a register written and read back, said (a difference in the bits of `mask` called out) */
static bool
set(const char *what, uint32_t reg, uint16_t mask, uint16_t value)
{
	uint16_t was = 0, back = 0;

	(void)reg_read(reg, &was);
	if (update(reg, mask, value) != 0 || reg_read(reg, &back) != 0) {
		say("cs47l93: %u: %s (%04x): the bus failed\n", step, what, reg);
		return false;
	}
	say("cs47l93: %u: %s (%04x): %04x -> %04x%s\n", step, what, reg, was, back,
	    (back & mask) == (value & mask) ? "" : " (NOT what was written)");
	return (back & mask) == (value & mask);
}

static bool
step_jack(void)
{
	uint16_t raw = 0;

	if (!set("MICD_CLAMP_CONTROL (mode 8: JD1 low, JD2 low)", MICD_CLAMP_CONTROL, 0x000F, 0x8) ||
	    !set("INTERRUPT_DEBOUNCE_7 (JD1, JD2 debounced)", INTERRUPT_DEBOUNCE_7, 0x0005, 0x0005) ||
	    !set("JACK_DETECT_ANALOGUE (JD1, JD2 on)", JACK_DETECT_ANALOGUE, 0x0003, 0x0003)) {
		return false;
	}
	delay(100000);
	if (reg_read(IRQ1_RAW_STATUS_7, &raw) != 0) {
		return false;
	}
	jack_in = !(raw & CLAMP_RISE);
	say("cs47l93: 3: the jack: IRQ1_RAW_STATUS_7 %04x: %s (MICD_CLAMP_RISE %u, JD1_RISE %u)\n", raw,
	    jack_in ? "something is plugged in" : "nothing plugged in", !!(raw & CLAMP_RISE), raw & 1);
	return true;
}

/* ---- step 4: the clocks -------------------------------------------------------------------------------------------- */

/*
 * The S9's (sound node "samsung,star-madera": cirrus,sysclk <1 4 98304000>, cirrus,fll1-refclk <1 0 26000000>; the
 * machine driver star_madera.c): SYSCLK from FLL1 at 98.304 MHz, FLL1 from MCLK1 at 26 MHz, MCLK1 being the SoC's
 * CLKOUT0 (the node's "xclkout", OSC_AUD: VCLK_CLKOUT0, cal-if clkout_exynos9810.c), the PMU's 0x0A00: SEL [13:8] 1
 * for the TCXO, and bit 0 a disable (ra_enable_clkout() writes 0 to enable). SYSCLK's register: SRC 4 (FLL1), FREQ 4
 * (98.304 MHz), then ENA; SAMPLE_RATE_1 3 (48 kHz, madera_sr_vals).
 *
 * FLL1 is a Cirrus "FLLHJ" on the CS47L92/93; madera.c's madera_fllhj_apply/_enable say how: the FLL makes half of
 * SYSCLK (49.152 MHz); the reference divided by 2^REFDIV to 13 MHz or under; at 13 MHz (above 1.152 MHz) the "high"
 * gains 0x21F0, a lock threshold of 8, FBDIV 1, and with a fractional ratio the 300 MHz fast oscillator (FAST_SRC 3,
 * the output reckoned six times over) and HP 3; N.THETA/LAMBDA from the gcd. The order: HOLD; N (CONTROL_2, CTRL_UPD
 * clear), THETA, LAMBDA, FBDIV, REFDIV, the gains, HP and PHASEDET, the lock threshold, the fast source; the reference
 * source (CS47L92: CONTROL_1 [15:12]); ENA; LOCKDET; CTRL_UPD; HOLD off; then FLL1_LOCK_STS1 (IRQ1_RAW_STATUS_2 bit 8).
 */
#define PMU_PA                  0x14060000UL
#define PMU_CLKOUT0             0x0A00
#define SYSTEM_CLOCK_1          0x0101
#define SAMPLE_RATE_1           0x0102
#define FLL1                    0x0170  /* CONTROL_n at +n (1-6), the gains +8, CONTROL_10 +0xA, _11 +0xB, DIGITAL_TEST_1 +0xD */
#define IRQ1_RAW_STATUS_2       0x1881
#define  FLL1_LOCK              0x0100
#define MCLK1_HZ                26000000u
#define SYSCLK_HZ               98304000u

static vm_offset_t pmu;
static bool clocked;

static uint32_t
gcd32(uint32_t a, uint32_t b)
{
	while (b != 0) {
		uint32_t t = a % b;

		a = b;
		b = t;
	}
	return a;
}

static bool
fll1_start(uint32_t fin, uint32_t sysclk)
{
	uint32_t refdiv, fref, fout = sysclk / 2, lockdet, gains, fbdiv, hp, fast = 0, min_n, max_n, ratio, g, num, lambda, n,
	    theta;
	bool frac;
	uint16_t raw = 0;

	for (refdiv = 0; refdiv < 3 && fin / (1u << refdiv) > 13000000u; refdiv++) {
	}
	fref = fin / (1u << refdiv);
	frac = (fout % fref) != 0;
	if (fref < 192000u) {
		lockdet = 2;
		gains = 0x23F0;
		fbdiv = frac ? 256 : 4;
	} else if (fref < 1152000u) {
		lockdet = 8;
		gains = 0x22F2;
		fbdiv = 1;
	} else {
		lockdet = 8;
		gains = 0x21F0;
		fbdiv = 1;
		if (frac) {
			fast = 3;
			fout *= 6;
		}
	}
	hp = frac ? 3 : 0;
	min_n = frac ? 4 : 1;
	max_n = frac ? 255 : 1023;
	ratio = fout / fref;
	while (ratio / fbdiv < min_n && fbdiv > 1) {
		fbdiv /= 2;
	}
	while (frac && ratio / fbdiv > max_n && fbdiv < 512) {
		fbdiv *= 2;
	}
	g = gcd32(fout, fbdiv * fref);
	num = fout / g;
	lambda = fbdiv * fref / g;
	n = num / lambda;
	theta = num % lambda;
	say("cs47l93: 4: FLL1 from %u Hz to %u Hz (SYSCLK %u): REFDIV %u (%u Hz), %s, FBDIV %u, N %u THETA %u LAMBDA %u, "
	    "gains %04x, lock threshold %u, HP %u, fast oscillator %u\n", fin, sysclk / 2, sysclk, refdiv, fref,
	    frac ? "fractional" : "integer", fbdiv, n, theta, lambda, gains, lockdet, hp, fast);
	if (n < min_n || n > max_n || lambda > 0xFFFF || theta > 0xFFFF) {
		say("cs47l93: 4: N %u is out of the %u-%u range (or THETA/LAMBDA too large): FLL1 not started\n", n, min_n, max_n);
		return false;
	}
	if (!set("FLL1_CONTROL_1 (HOLD)", FLL1 + 1, 0x0004, 0x0004) ||
	    !set("FLL1_CONTROL_2 (N, CTRL_UPD clear)", FLL1 + 2, 0xFFFF, (uint16_t)n) ||
	    !set("FLL1_CONTROL_3 (THETA)", FLL1 + 3, 0xFFFF, (uint16_t)theta) ||
	    !set("FLL1_CONTROL_4 (LAMBDA)", FLL1 + 4, 0xFFFF, (uint16_t)lambda) ||
	    !set("FLL1_CONTROL_5 (FB_DIV)", FLL1 + 5, 0x03FF, (uint16_t)fbdiv) ||
	    !set("FLL1_CONTROL_6 (REFCLK_DIV)", FLL1 + 6, 0x00C0, (uint16_t)(refdiv << 6)) ||
	    !set("FLL1_GAIN", FLL1 + 8, 0xFFFF, (uint16_t)gains) ||
	    !set("FLL1_CONTROL_10 (HP, PHASEDET_ENA)", FLL1 + 0xA, 0xC000 | 0x1000, (uint16_t)(hp << 14 | 0x1000)) ||
	    !set("FLL1_CONTROL_11 (LOCKDET_THR)", FLL1 + 0xB, 0x001E, (uint16_t)(lockdet << 1)) ||
	    !set("FLL1_DIGITAL_TEST_1 (SYNC_EFS off, VCO fast source)", FLL1 + 0xD, 0x0103, (uint16_t)fast) ||
	    !set("FLL1_CONTROL_1 (REFCLK_SRC MCLK1)", FLL1 + 1, 0xF000, 0) ||
	    !set("FLL1_CONTROL_1 (ENA)", FLL1 + 1, 0x0001, 0x0001) ||
	    !set("FLL1_CONTROL_11 (LOCKDET)", FLL1 + 0xB, 0x0001, 0x0001)) {
		return false;
	}
	(void)update(FLL1 + 2, 0x8000, 0x8000);         /* CTRL_UPD: the values above taken */
	(void)update(FLL1 + 1, 0x0004, 0);              /* HOLD off: the FLL locks */
	for (int i = 0; i < 40; i++) {
		if (reg_read(IRQ1_RAW_STATUS_2, &raw) == 0 && (raw & FLL1_LOCK)) {
			say("cs47l93: 4: FLL1 locked (IRQ1_RAW_STATUS_2 %04x) after about %d ms\n", raw, i);
			return true;
		}
		delay(1000);
	}
	say("cs47l93: 4: FLL1 has NOT locked in 40 ms (IRQ1_RAW_STATUS_2 %04x)\n", raw);
	return false;
}

static bool
step_clocks(void)
{
	uint32_t was;

	pmu = ml_io_map(PMU_PA, 0x1000);
	if (!pmu) {
		say("cs47l93: 4: the PMU could not be mapped\n");
		return false;
	}
	was = *(volatile uint32_t *)(pmu + PMU_CLKOUT0);
	*(volatile uint32_t *)(pmu + PMU_CLKOUT0) = (was & ~(0x3Fu << 8 | 1u)) | 1u << 8;
	say("cs47l93: 4: the PMU's CLKOUT0 (%#lx, MCLK1: the 26 MHz TCXO): %08x -> %08x (SEL [13:8] 1, bit 0 clear: on)\n",
	    PMU_PA + PMU_CLKOUT0, was, *(volatile uint32_t *)(pmu + PMU_CLKOUT0));
	delay(1000);
	if (!fll1_start(MCLK1_HZ, SYSCLK_HZ)) {
		return false;
	}
	if (!set("SAMPLE_RATE_1 (48 kHz)", SAMPLE_RATE_1, 0x001F, 0x03) ||
	    !set("SYSTEM_CLOCK_1 (FLL1, 98.304 MHz)", SYSTEM_CLOCK_1, 0x870F, 4u << 8 | 4u) ||
	    !set("SYSTEM_CLOCK_1 (SYSCLK_ENA)", SYSTEM_CLOCK_1, 0x0040, 0x0040)) {
		return false;
	}
	clocked = true;
	return true;
}

/* ---- step 5: the main microphone ----------------------------------------------------------------------------------- */

/*
 * The main microphone is a digital one on IN3L (the device tree's routing: "DMIC1" <- "MICBIAS1A", "IN3L" <- "DMIC1";
 * mixer_paths.xml's dev-main-mic: IN3L, "DMIC1 Switch"; "IN3 OSR" 3.072 MHz, "IN3L HPF Switch" 1). Its supply: MICVDD
 * (the node's micvdd, 3.0 V: LDO2_VSEL 0x24 in madera's ranges, 0.9 V + 25 mV to 0x14 then 1.5 V + 100 mV; the charge
 * pump CPMIC_ENA; 3 ms), MICBIAS1 (1.8 V: LVL (1.8 - 1.5) / 0.1 = 3, cirrus,ext-cap), MICBIAS1A (its switch, active
 * discharge). IN3 on the CS47L92/93 is digital only; its DMIC clock's supply is cirrus,dmic-ref's third cell, 1
 * (MICBIAS1): IN3_DMIC_SUP [12:11]. IN3L's digital volume: 0.5 dB a step, 0x80 0 dB (Android's main-mic paths use
 * 129 to 154, 0.5 to 13 dB up); here cs47l93_mic_vol, 0x90 (+8 dB) by default.
 */
#define MIC_CHARGE_PUMP_1       0x0200  /* CPMIC_ENA 0, CPMIC_BYPASS 1, CPMIC_DISCH 2 */
#define LDO2_CONTROL_1          0x0213  /* LDO2_VSEL [10:5] */
#define MIC_BIAS_CTRL_1         0x0218  /* MICB1_ENA 0, BYPASS 1, DISCH 2, RATE 3, LVL [8:5], EXT_CAP 15 */
#define MIC_BIAS_CTRL_5         0x021C  /* MICB1A_ENA 0, MICB1A_DISCH 1, MICB1B 4-5, MICB1C 8-9 */
#define INPUT_ENABLES           0x0300  /* IN3L_ENA 5 */
#define INPUT_ENABLES_STATUS    0x0301
#define IN3L_CONTROL            0x0320  /* IN3L_HPF 15, IN3_DMIC_SUP [12:11] */
#define ADC_DIGITAL_VOLUME_3L   0x0321  /* IN_VU 9, IN3L_MUTE 8, IN3L_DIG_VOL [7:0] */
#define DMIC3L_CONTROL          0x0322  /* IN3_OSR [10:8]: 5 is 3.072 MHz */

static uint32_t mic_vol = 0x90;
static bool mic_on;

static bool
step_mic(void)
{
	uint16_t status = 0;

	if (!set("LDO2_CONTROL_1 (MICVDD 3.0 V)", LDO2_CONTROL_1, 0x07E0, 0x24 << 5) ||
	    !set("MIC_CHARGE_PUMP_1 (CPMIC_ENA, no bypass)", MIC_CHARGE_PUMP_1, 0x0003, 0x0001)) {
		return false;
	}
	delay(3000);
	if (!set("MIC_BIAS_CTRL_1 (MICBIAS1 1.8 V, external cap, on)", MIC_BIAS_CTRL_1, 0x8000 | 0x01E0 | 0x000B,
	    0x8000 | 3u << 5 | 0x0001) ||
	    !set("MIC_BIAS_CTRL_5 (MICBIAS1A on, active discharge)", MIC_BIAS_CTRL_5, 0x0003, 0x0003)) {
		return false;
	}
	delay(5000);
	if (!set("IN3L_CONTROL (HPF, DMIC clock from MICBIAS1)", IN3L_CONTROL, 0x8000 | 0x1800, 0x8000 | 1u << 11) ||
	    !set("DMIC3L_CONTROL (OSR 3.072 MHz)", DMIC3L_CONTROL, 0x0700, 5u << 8) ||
	    update(ADC_DIGITAL_VOLUME_3L, 0x03FF, (uint16_t)(0x0200 | (mic_vol & 0xFF))) != 0 ||      /* IN_VU reads 0 */
	    !set("ADC_DIGITAL_VOLUME_3L (unmuted; IN_VU taken)", ADC_DIGITAL_VOLUME_3L, 0x01FF, (uint16_t)(mic_vol & 0xFF)) ||
	    !set("INPUT_ENABLES (IN3L)", INPUT_ENABLES, 0x0020, 0x0020)) {
		return false;
	}
	delay(20000);
	(void)reg_read(INPUT_ENABLES_STATUS, &status);
	say("cs47l93: 5: INPUT_ENABLES_STATUS %04x (IN3L's bit 5 %s)\n", status, (status & 0x20) ? "on" : "NOT on");
	mic_on = true;
	return true;
}

/* ---- step 6: AIF1, ABOX's UAIF0 -------------------------------------------------------------------------------------- */

/*
 * AIF1's four pins are the codec's GPIO5-8 (pinctrl-cs47l92.c: the "aif1" group, pins 4-7), GPIOs after a reset (FN 1,
 * pulled down: 0x2001 read on the phone) until given their alternate function (FN 0; the device tree's cs47l93-gpio-
 * defaults: aif1, bias-bus-hold): without that ABOX records zeroes (phone 1, 3 November).
 * AIF1 is the codec's side of UAIF0: I2S, the codec a slave of both clocks (CBS_CFS), 48 kHz, 2 x 16 bits
 * (madera_set_fmt, madera_hw_params: FMT 2 is I2S, the rate from SAMPLE_RATE_1 (AIF1_RATE 0), RX_BCPF the BCLKs a frame,
 * 32; FRAME_CTRL_1/2 WL [13:8] and SLOT_LEN [7:0] 16). What it sends (mixer_paths.xml's route-main-mic): AIF1TX1 and
 * AIF1TX2 from IN3L (source 0x14, madera_mixer_values), each at 0 dB (Input 1 Volume [7:1] 0x40). What it takes goes to
 * the headphones' mixers (route-headphone: HPOUT2L from AIF1RX1, HPOUT2R from AIF1RX2, source 0x20 and 0x21, 0 dB); the
 * headphone output itself (OUT2) is NOT switched on here.
 */
#define GPIO1_CTRL_1            0x1700  /* GPIOn at + 2 (n - 1): FN [9:0] (0 the pin's alternate function, 1 a GPIO), PD 13,
					 * PU 14, DIR 15 */
#define AIF1_BCLK_CTRL          0x0500  /* AIF1_BCLK_MSTR 5 */
#define AIF1_RATE_CTRL          0x0503  /* AIF1_RATE [15:11] */
#define AIF1_FORMAT             0x0504  /* AIF1_FMT [2:0] */
#define AIF1_RX_BCLK_RATE       0x0506  /* AIF1_RX_BCPF [12:0] */
#define AIF1_FRAME_CTRL_1       0x0507  /* TX: WL [13:8], SLOT_LEN [7:0] */
#define AIF1_FRAME_CTRL_2       0x0508  /* RX */
#define AIF1_TX_ENABLES         0x0519
#define AIF1_RX_ENABLES         0x051A
#define OUT2LMIX_INPUT_1_SOURCE 0x0690
#define OUT2RMIX_INPUT_1_SOURCE 0x0698
#define AIF1TX1MIX_INPUT_1_SOURCE 0x0700
#define AIF1TX2MIX_INPUT_1_SOURCE 0x0708
#define SRC_IN3L                0x14
#define SRC_AIF1RX1             0x20
#define SRC_AIF1RX2             0x21
#define MIX_0DB                 0x80    /* 0x40 << 1 */

static bool aif_on;

static bool
step_aif(void)
{
	for (uint32_t pin = 4; pin <= 7; pin++) {
		/* GPIO5-8 to AIF1 (function 0), bus hold (pull-up and pull-down both); the reset leaves them GPIOs (0x2001) */
		if (!set("GPIOn_CTRL_1 (AIF1's pin)", GPIO1_CTRL_1 + 2 * pin, 0x63FF, 0x6000)) {
			return false;
		}
	}
	if (!set("AIF1_BCLK_CTRL (a slave)", AIF1_BCLK_CTRL, 0x0020, 0) ||
	    !set("AIF1_RATE_CTRL (SAMPLE_RATE_1)", AIF1_RATE_CTRL, 0xF800, 0) ||
	    !set("AIF1_FORMAT (I2S)", AIF1_FORMAT, 0x0007, 2) ||
	    !set("AIF1_RX_BCLK_RATE (32 BCLKs a frame)", AIF1_RX_BCLK_RATE, 0x1FFF, 32) ||
	    !set("AIF1_FRAME_CTRL_1 (TX 16 bits in 16)", AIF1_FRAME_CTRL_1, 0x3FFF, 16u << 8 | 16) ||
	    !set("AIF1_FRAME_CTRL_2 (RX 16 bits in 16)", AIF1_FRAME_CTRL_2, 0x3FFF, 16u << 8 | 16) ||
	    !set("AIF1TX1MIX source 1 (IN3L)", AIF1TX1MIX_INPUT_1_SOURCE, 0x00FF, SRC_IN3L) ||
	    !set("AIF1TX1MIX volume 1 (0 dB)", AIF1TX1MIX_INPUT_1_SOURCE + 1, 0x00FE, MIX_0DB) ||
	    !set("AIF1TX2MIX source 1 (IN3L)", AIF1TX2MIX_INPUT_1_SOURCE, 0x00FF, SRC_IN3L) ||
	    !set("AIF1TX2MIX volume 1 (0 dB)", AIF1TX2MIX_INPUT_1_SOURCE + 1, 0x00FE, MIX_0DB) ||
	    !set("OUT2LMIX source 1 (AIF1RX1)", OUT2LMIX_INPUT_1_SOURCE, 0x00FF, SRC_AIF1RX1) ||
	    !set("OUT2LMIX volume 1 (0 dB)", OUT2LMIX_INPUT_1_SOURCE + 1, 0x00FE, MIX_0DB) ||
	    !set("OUT2RMIX source 1 (AIF1RX2)", OUT2RMIX_INPUT_1_SOURCE, 0x00FF, SRC_AIF1RX2) ||
	    !set("OUT2RMIX volume 1 (0 dB)", OUT2RMIX_INPUT_1_SOURCE + 1, 0x00FE, MIX_0DB) ||
	    !set("AIF1_TX_ENABLES (TX1, TX2)", AIF1_TX_ENABLES, 0x0003, 0x0003) ||
	    !set("AIF1_RX_ENABLES (RX1, RX2)", AIF1_RX_ENABLES, 0x0003, 0x0003)) {
		return false;
	}
	aif_on = true;
	say("cs47l93: 6: AIF1 sends IN3L (the main microphone) on both slots once UAIF0 clocks it: \"abox rec S\" records it\n");
	return true;
}

/* ---- the headphones (cs47l93_hp=1; THE HEADPHONES in the header) ------------------------------------------------------ */

#define OUTPUT_ENABLES_1        0x0400  /* OUT2L_ENA 3, OUT2R_ENA 2 */
#define  OUT2_ENA               0x000C
#define OUTPUT_STATUS_1         0x0401
#define DAC_DIGITAL_VOLUME_2L   0x0419  /* OUT_VU 9, OUT2L_MUTE 8, OUT2L_VOL [7:0] */
#define DAC_DIGITAL_VOLUME_2R   0x041D
#define  OUT_VU                 0x0200
#define  OUT_MUTE               0x0100
#define OUT_UP_MS               6       /* madera_out_ev(): each output of a CS47L92/93 */
#define OUT_DOWN_MS             1

static bool hp_out;                     /* OUT2 is on */
static bool hp_watching;                /* the jack's thread runs */
static volatile bool hp_stop;           /* the run ends: the thread stops */

/* OUT2 up (muted, enabled, 6 ms each, unmuted) or down (muted, disabled, 1 ms each), Cirrus's order */
static bool
hp_power(bool on)
{
	uint16_t status = 0;

	if (on) {
		if (!set("DAC_DIGITAL_VOLUME_2L (muted, the volume)", DAC_DIGITAL_VOLUME_2L, 0x01FF, (uint16_t)(OUT_MUTE | hp_vol)) ||
		    update(DAC_DIGITAL_VOLUME_2R, 0x03FF, (uint16_t)(OUT_VU | OUT_MUTE | hp_vol)) != 0 ||
		    !set("OUTPUT_ENABLES_1 (OUT2L, OUT2R)", OUTPUT_ENABLES_1, OUT2_ENA, OUT2_ENA)) {
			(void)update(OUTPUT_ENABLES_1, OUT2_ENA, 0);
			return false;
		}
		delay(2 * OUT_UP_MS * 1000);
		(void)reg_read(OUTPUT_STATUS_1, &status);
		(void)update(DAC_DIGITAL_VOLUME_2L, OUT_MUTE, 0);
		(void)update(DAC_DIGITAL_VOLUME_2R, OUT_VU | OUT_MUTE, OUT_VU);
		hp_out = true;
		say("cs47l93: the headphones: OUT2 on (OUTPUT_STATUS_1 %04x), unmuted at %d.%d dB\n", status,
		    ((int)hp_vol - 0x80) / 2, (hp_vol & 1) ? 5 : 0);
	} else {
		(void)update(DAC_DIGITAL_VOLUME_2L, OUT_MUTE, OUT_MUTE);
		(void)update(DAC_DIGITAL_VOLUME_2R, OUT_VU | OUT_MUTE, OUT_VU | OUT_MUTE);
		(void)update(OUTPUT_ENABLES_1, OUT2_ENA, 0);
		delay(2 * OUT_DOWN_MS * 1000);
		(void)reg_read(OUTPUT_STATUS_1, &status);
		hp_out = false;
		say("cs47l93: the headphones: OUT2 muted and off (OUTPUT_STATUS_1 %04x)\n", status);
	}
	return true;
}

/* the jack's new state: ABOX before OUT2 going in, OUT2 before ABOX going out */
static bool
hp_jack(bool in)
{
	bool routed = false;

	say("cs47l93: the jack: %s\n", in ? "something plugged in" : "pulled out");
	if (in) {
		routed = hakios_abox_headphones(true);
		if (!routed) {
			say("cs47l93: the headphones: ABOX did not take iOS's sound to UAIF0 (abox=8 not running?): OUT2 stays off\n");
		} else if (!hp_power(true)) {
			(void)hakios_abox_headphones(false);
			routed = false;
		}
	} else {
		if (hp_out) {
			(void)hp_power(false);
		}
		(void)hakios_abox_headphones(false);
	}
	hakios_audio_set_route(in, routed);
	return routed;
}

static void
hp_thread(__unused void *arg, __unused wait_result_t result)
{
	bool state = jack_in, seen = jack_in, routed = false;
	unsigned same = 0, tick = 0;

	say("cs47l93: the headphones: the jack watched (%s now)\n", jack_in ? "something plugged in" : "nothing plugged in");
	if (jack_in) {
		routed = hp_jack(true);
	} else {
		hakios_audio_set_route(false, false);
	}
	while (!hp_stop) {
		uint16_t raw = 0;
		bool in;

		delay_for_interval(100, NSEC_PER_MSEC);
		if (!powered || busy || reg_read(IRQ1_RAW_STATUS_7, &raw) != 0) {
			continue;               /* (the console re-making the codec: not now) */
		}
		in = !(raw & CLAMP_RISE);
		same = in == seen ? same + 1 : 0;
		seen = in;
		if (in != state && same >= 2) {
			state = in;
			jack_in = in;
			routed = hp_jack(in);
		} else if (state && !routed && ++tick % 20 == 0) {
			routed = hp_jack(true);         /* plugged in before iOS's stream ran: tried again every 2 s */
		}
	}
	thread_terminate(current_thread());
}

static void
run(void)
{
	bool (*const fn[STEPS])(void) = { step_look, step_power, step_jack, step_clocks, step_mic, step_aif };

	say("cs47l93: the audio codec, %u step%s of %u\n", steps, steps == 1 ? "" : "s", STEPS);
	for (step = 1; step <= steps && step <= STEPS; step++) {
		if (!fn[step - 1]()) {
			say("cs47l93: %u: failed; nothing more is done\n", step);
			return;
		}
	}
	say("cs47l93: the first %u step%s done\n", step - 1, step == 2 ? "" : "s");
	if (hp_enabled && step > STEPS && !hp_watching) {
		thread_t thread;

		hp_watching = true;
		if (kernel_thread_start(hp_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			hp_watching = false;
		}
	}
}

static void
codec_thread(void *arg, __unused wait_result_t result)
{
	if (arg != NULL) {
		delay(2000000);         /* at boot: 2 s, for the rest of the kernel (ACPM's users among them) to come up */
	}
	run();
	busy = 0;
	thread_terminate(current_thread());
}

void
hakios_cs47l93_quiesce(void)
{
	hp_stop = true;
	if (gpio && powered) {
		reset_pin(false);       /* the codec held in reset on the way back to the loader: OUT2 off with it */
		powered = clocked = mic_on = aif_on = false;
	}
	if (pmu) {
		*(volatile uint32_t *)(pmu + PMU_CLKOUT0) |= 1u;        /* MCLK1 off again */
	}
}

/* ---- the USB console ------------------------------------------------------------------------------------------------ */

static char *
word(char **p)
{
	char *w = *p;

	while (*w == ' ') {
		w++;
	}
	*p = w;
	while (**p != 0 && **p != ' ') {
		(*p)++;
	}
	if (**p != 0) {
		*(*p)++ = 0;
	}
	return w;
}

static bool
number(const char *w, uint32_t *v)
{
	char *end = NULL;
	unsigned long long n;

	if (*w == 0) {
		return false;
	}
	n = strtoul(w, &end, 0);
	if (end == NULL || *end != 0 || n > UINT32_MAX) {
		return false;
	}
	*v = (uint32_t)n;
	return true;
}

void
hakios_cs47l93_console(const char *arg)
{
	char line[128], *p = line, *verb;
	thread_t thread;

	strlcpy(line, arg != NULL ? arg : "", sizeof(line));
	verb = word(&p);
	if (steps == 0) {
		say("codec> not started at boot (cs47l93=N, 1 at least)\n");
		return;
	}
	if (*verb == 0) {
		uint16_t raw = 0, ins = 0, sysclk = 0, fll = 0;

		if (powered && !busy) {
			(void)reg_read(IRQ1_RAW_STATUS_7, &raw);
			(void)reg_read(INPUT_ENABLES_STATUS, &ins);
			(void)reg_read(SYSTEM_CLOCK_1, &sysclk);
			(void)reg_read(IRQ1_RAW_STATUS_2, &fll);
		}
		say("codec> step %u of %u; %s; ID %04x revision %04x; the jack %s (IRQ1_RAW_STATUS_7 %04x); SYSTEM_CLOCK_1 %04x, "
		    "FLL1 %s; inputs %04x; the microphone %s, AIF1 %s\n", step > steps ? steps : step, steps,
		    powered ? "powered" : "not powered", id, revision, !(raw & CLAMP_RISE) ? "in" : "out", raw, sysclk,
		    (fll & FLL1_LOCK) ? "locked" : "not locked", ins, mic_on ? "on" : "off", aif_on ? "on" : "off");
		hakios_exynos_spi_report(&bus, say);
	} else if (strcmp(verb, "power") == 0 || strcmp(verb, "mic") == 0) {
		if (!OSCompareAndSwap(0, 1, &busy)) {
			say("codec> a run is going on\n");
			return;
		}
		steps = strcmp(verb, "mic") == 0 ? STEPS : steps < 2 ? 2 : steps;
		clocked = mic_on = aif_on = false;
		powered = hp_out = false;
		if (kernel_thread_start(codec_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			busy = 0;
		}
	} else if (strcmp(verb, "r") == 0 || strcmp(verb, "w") == 0) {
		uint32_t reg = 0, v = 1;
		bool have = number(word(&p), &reg), second = number(word(&p), &v);

		if (!powered || busy) {
			say("codec> the codec is not powered (or a run is going on)\n");
			return;
		}
		if (!have || reg > 0x3fff) {
			say("codec> %s REG ...: a 16-bit register's address\n", verb);
			return;
		}
		if (*verb == 'w') {
			uint16_t back = 0;

			if (!second || v > 0xffff) {
				say("codec> w REG VALUE\n");
				return;
			}
			int error = reg_write(reg, (uint16_t)v);
			(void)reg_read(reg, &back);
			say("codec> %04x <- %04x (%s), reads %04x\n", reg, v, error ? "failed" : "written", back);
			return;
		}
		v = v < 1 ? 1 : v > 64 ? 64 : v;
		for (uint32_t i = 0; i < v; i += 8) {
			char row[80];

			row[0] = 0;
			for (uint32_t j = 0; j < 8 && i + j < v; j++) {
				uint16_t value = 0;

				if (reg_read(reg + i + j, &value) != 0) {
					strlcat(row, " ????", sizeof(row));
				} else {
					snprintf(row + strlen(row), sizeof(row) - strlen(row), " %04x", value);
				}
			}
			say("codec> %04x:%s\n", reg + i, row);
		}
	} else {
		say("codec> codec [power | mic | r REG [N] | w REG VALUE]\n");
	}
}

__startup_func
static void
cs47l93_init(void)
{
	thread_t thread;
	uint32_t div;

	if (!PE_parse_boot_argn("cs47l93", &steps, sizeof(steps)) || steps == 0) {
		steps = 0;
		return;
	}
	if (pe_arm_get_soc_base_phys() != STARLTE_SOC_BASE) {
		steps = 0;
		return;                 /* not the Exynos 9810's */
	}
	if (PE_parse_boot_argn("cs47l93_div", &div, sizeof(div)) && (div <= 15 || div == 255)) {
		bus.div_ratio = div;
	}
	if (PE_parse_boot_argn("cs47l93_mic_vol", &div, sizeof(div)) && div <= 0xBF) {
		mic_vol = div;          /* IN3L's digital volume: 0x80 is 0 dB, 0.5 dB a step; at most +31.5 dB */
	}
	PE_parse_boot_argn("cs47l93_hp", &hp_enabled, sizeof(hp_enabled));
	if (PE_parse_boot_argn("cs47l93_hp_vol", &div, sizeof(div)) && div <= 0x80) {
		hp_vol = div;           /* OUT2's digital volume: 0x80 is 0 dB, 0.5 dB a step; never above 0 dB here */
	}
	if (hp_enabled) {
		hakios_audio_set_route(false, false);   /* a jack is known here: the plug-in's speaker gets one (its state follows) */
	}
	busy = 1;
	if (kernel_thread_start(codec_thread, (void *)1, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		busy = 0;
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, cs47l93_init);
