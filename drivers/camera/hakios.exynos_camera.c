/*
 * The Galaxy S9's cameras (starlte, Exynos 9810), stage 1: the front camera's sensor, a Samsung S5K3H1 (Android's
 * camera 1, "fimc-is_sensor_3h1"), powered up the way Android powers it, its model ID read over I2C, and powered down
 * again. Only with boot-arg cam_s9=N on this phone; nothing is written at cam_s9=0 (the default). Every address and
 * number below belongs to this phone, from its device tree and the S9 kernel's camera driver (fimc-is2), read as data.
 *
 *   the sensor     0x35 (7 bits; 0x6A as Samsung's node name writes it) on hsi2c@108F0000, the second I2C of USI08
 *                  of PERIC1 ("USI08_I2C"), 400 kHz; its pins gpp5-2 (SCL) and gpp5-3 (SDA), function 3 ("on_i2c"),
 *                  function 0, inputs, when it is off ("off_i2c"). 16-bit register addresses, MIPI CCS's map: the
 *                  model ID at 0x0000 (16 bits), the revision at 0x0002, the frame count at 0x0005, mode_select at 0x0100
 *   reset          gpg2-5 of PERIC0's GPIO (0x104300C0), an output, high = running
 *   MCLK           CIS_CLK1 of CMU_TOP (0x1A240000): MUX_CLKCMU_CIS_CLK1 0x101C [0] (0: OSCCLK, 26 MHz), the divider
 *                  0x1818 [4:0], the gate 0x201C (bit 20 manual, 21 on), its Q-channel 0x3008; Android asks 26 MHz
 *                  (exynos9810_fimc_is_sensor_mclk_on), which is OSCCLK undivided. Out on gpc1-5 of PERIC1's GPIO
 *                  (0x10830080), function 2 ("fimc_is_mclk1_fn"); output low (function 1) when off
 *   the supplies   all four on the S2MPB02 (0x59 on hsi2c@10920000, USI10 of PERIC1: the flashlight's chip, see
 *                  hakios.s2mpb02_torch.c). An LDO's control register: bit 7 on, [6:0] the voltage, 600 mV + n steps
 *                  (12.5 mV for LDOs 1-5, 25 mV for 6-18):
 *                    VDDD_1.05V_VT   LDO5,  L5CTRL  0x22, 1.05 V = 36 (0x24)
 *                    VDDIO_1.8V_VT   LDO6,  L6CTRL  0x23, 1.8 V  = 48 (0x30)
 *                    VDDA_2.8V_VT    LDO13, L13CTRL 0x2A, 2.8 V  = 88 (0x58)
 *                    VDDAF_2.8V_VT   LDO16, L16CTRL 0x2D, 2.8 V  = 88 (0x58): the lens motor's, read and never switched here
 *   power-up       (the module file's, fimc-is-device-module-3h1.c, less the lens motor's supply): CIS_CLK1 on at 26
 *                  MHz; reset low; VDDA on; VDDIO on; VDDD at 1.05 V and on, 1 ms; the bus pins to I2C, 10 us; reset
 *                  high; MCLK out on its pin, 9 ms. Each LDO is given 1 ms (the regulator's enable time is 180 us).
 *   power-down     MCLK's pin to an output, low; reset low; the bus pins back to inputs; VDDD, VDDA, VDDIO off (the
 *                  module file's order); CIS_CLK1's gate, Q-channel, mux and divider as found.
 *
 *   cam_s9=1   a look, reading only (but for the S2MPB02's bus, set up as the flashlight's driver sets it up): the pins,
 *              CMU_TOP's CIS_CLK1, the two buses as found, the S2MPB02's ID and the four LDOs' registers
 *   cam_s9=2   ... and the sensor powered up, its model ID, revision, frame count and mode read, and powered down again
 *
 * Boot-arg cam_s9_ipclk=MHz: the two HSI2Cs' input clock (default 400, as the phone showed for the other buses on 1
 * November: a wrong guess on the high side only makes the bus slower).
 *
 * Limits, whatever asks. The S2MPB02 makes other supplies too (the flash LED, the rear camera's lens motor, the grip
 * sensor, the proximity LED): every write to it goes through one function that takes only the three LDO registers above,
 * only the values above (Android's voltages, on or off), and reads each back; a write that does not read back, or a byte
 * nobody acknowledges, and everything is powered down in the power-down's order and nothing more is done for the run.
 * An LDO found on is left as it is and left on at the end; one found on at another voltage stops the run before
 * anything is written (someone else has it). The flashlight's driver stays off in a run with cam_s9=N (the same bus,
 * one driver per bus).
 * At the run's end (loader_return(), a panic's way out too) whatever this switched on is switched off.
 *
 * Later stages (not written: devices/starlte/CAMERA.md has the plan): CSIS1's D-PHY and link, and its VC0 DMA writing
 * the sensor's RAW10 frames to memory, the ISP's firmware never started (fimc-is-hw-csi-v5_1.c: the WDMA is programmed by
 * the AP); then /dev/hakios-camera's frames for libhakios.media's capture device.
 *
 * Sources: the phone's device tree (exynos9810-starlte_eur_open_26.dts of the BSP: hsi2c@108F0000 and fimc-is-3h1@35,
 * fimc-is_sensor_3h1@6A, s2mpb02_pmic@59 and its regulators, the fimc_is_mclk1 pin states, hsi2c21-bus, usi@1081102C);
 * the S9 kernel's fimc-is-device-module-3h1.c, fimc-is-cis-3h1.c/.h, ischain/fimc-is-v6_0_0/setup-fimc-is-sensor.c,
 * s2mpb02-regulator.c, s2mpb02-private.h, cal-if/exynos9810/cmucal-sfr.c and cmucal-node.c, pinctrl-exynos.c
 * (android_kernel_samsung_exynos9810, lineage-24.0, e5906667; GPL: read for these facts, not copied).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_line(const char *line);

static bool leaving;

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
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

/* ---- the S2MPB02 ---- */

#define PMIC            0x59
#define PMIC_ID         0x00
#define LDO_ON          0x80
#define LDO_VSEL        0x7f

static const struct ldo {
	const char *name;
	uint8_t reg, vsel;
	bool switched;          /* by this driver (the lens motor's is only read) */
} ldos[] = {
	{ "VDDD_1.05V_VT (LDO5)", 0x22, 0x24, true },
	{ "VDDIO_1.8V_VT (LDO6)", 0x23, 0x30, true },
	{ "VDDA_2.8V_VT (LDO13)", 0x2A, 0x58, true },
	{ "VDDAF_2.8V_VT (LDO16)", 0x2D, 0x58, false },
};
#define VDDD            0
#define VDDIO           1
#define VDDA            2
#define NLDOS           (sizeof(ldos) / sizeof(ldos[0]))

static struct hakios_hsi2c pmic_bus = {
	.name = "cam-s2mpb02",
	.regs_pa = 0x10920000,                  /* hsi2c@10920000: USI10 of PERIC1 */
	.usi_mode_pa = 0x1081103C,              /* usi@1081103C: its mode word */
	.cmu_pa = 0x10800000, .cmu_size = 0x4000,       /* CMU_PERIC1 */
	.gates = { 0x2048, 0x20AC, 0x2114, 0x2118 }, .ngates = 4,
	.qch = 0x3050, .div = 0x1828, .mux = 0x0120,       /* DIV_CLK_PERIC1_USI10_USI; MUX_CLKCMU_PERIC1_IP_USER */
	.pins_pa = 0x10830040,                  /* gpp6: SCL pin 0, SDA pin 1, function 2 */
	.pins_mask = 0xffu, .pins_want = 0x22u,
	.ipclk_mhz = 400, .bus_hz = 100000,     /* the device tree gives this bus no speed: Linux's default */
};

/* ---- the sensor's bus ---- */

#define SENSOR          0x35

static struct hakios_hsi2c sensor_bus = {
	.name = "cam-3h1",
	.regs_pa = 0x108F0000,                  /* hsi2c@108F0000: USI08_I2C of PERIC1 (hsi2c21) */
	.usi_mode_pa = 0x1081102C,              /* usi@1081102C: USI08's mode word ("i2c" in the device tree) */
	.cmu_pa = 0x10800000, .cmu_size = 0x4000,       /* CMU_PERIC1 */
	/* GATE_CLK_PERIC1_USI_I2C, RSTNSYNC_CLK_PERIC1_USI08_I2C, USI08_I2C IPCLK, USI08_I2C PCLK */
	.gates = { 0x2050, 0x2098, 0x20EC, 0x20F0 }, .ngates = 4,
	.qch = 0x303C, .div = 0x1830, .mux = 0x0120,       /* QCH_CON_USI08_I2C_QCH; DIV_CLK_PERIC1_USI_I2C */
	.pins_pa = 0x10830020,                  /* gpp5: SCL pin 2, SDA pin 3, function 3 */
	.pins_mask = 0xff00u, .pins_want = 0x3300u,
	.ipclk_mhz = 400, .bus_hz = 400000,     /* clock-frequency = <400000> */
};

/* ---- pins and the clock ---- */

#define GPIO_PERIC0_PA  0x10430000UL
#define GPIO_PERIC1_PA  0x10830000UL
#define GPG2            0x0C0           /* PERIC0: the reset, pin 5 */
#define GPC1            0x080           /* PERIC1: MCLK1, pin 5 */
#define GPP5            0x020           /* PERIC1: the sensor's bus, pins 2 and 3 */
#define RESET_PIN       5
#define MCLK_PIN        5

#define CMU_TOP_PA      0x1A240000UL
#define CIS1_MUX        0x101C          /* [0]: 0 OSCCLK, 1 DIV_PLL_SHARED2_DIV2 */
#define CIS1_DIV        0x1818          /* [4:0]: the ratio less one */
#define CIS1_GATE       0x201C          /* bit 20 manual, bit 21 on */
#define CIS1_QCH        0x3008          /* DMYQCH_CON_DFTMUX_TOP_QCH_CIS_CLK1: [0] enable, [1] clock request */
#define GATE_MANUAL     (1u << 20)
#define GATE_ON         (1u << 21)

static vm_offset_t peric0, peric1, cmu_top;
static uint8_t found[NLDOS];            /* each LDO's register as found */
static bool mine[NLDOS];                /* switched on by this run */
static bool broken;                     /* a write did not read back, or the bus failed: no more writing */
static bool powered;                    /* the sensor's power-up was begun */
static uint32_t gate_found, mux_found, div_found, qch_found;
static bool clock_mine;

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

/* by the clock, never sleeping: also on the run's way out */
static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static uint32_t
pmic_get(uint8_t reg, uint8_t *value)
{
	return hakios_hsi2c_read_reg(&pmic_bus, PMIC, reg, value, 1);
}

/* The only way anything reaches the S2MPB02: one of the three LDOs, Android's voltage, on or off; read back. */
static bool
ldo_put(uint32_t i, bool on)
{
	uint8_t value, buf[2], back = 0;

	if (broken || i >= NLDOS || !ldos[i].switched) {
		return false;
	}
	value = (uint8_t)((on ? LDO_ON : 0) | ldos[i].vsel);
	buf[0] = ldos[i].reg;
	buf[1] = value;
	if (hakios_hsi2c_message(&pmic_bus, PMIC, false, buf, 2, true) != 0 || pmic_get(ldos[i].reg, &back) != 0 ||
	    back != value) {
		say("exynos-camera: S2MPB02 %#x (%s): wrote %#x, read back %#x (bus %#x): no more writing for this run\n",
		    ldos[i].reg, ldos[i].name, value, back, pmic_bus.last_error);
		broken = true;
		return false;
	}
	return true;
}

/* an LDO to Android's voltage and on, unless it was found on (then it is left as it is) */
static bool
ldo_on(uint32_t i)
{
	if (found[i] & LDO_ON) {
		say("exynos-camera: 2: %s was on already (%#x): left as it is\n", ldos[i].name, found[i]);
		return true;
	}
	if ((found[i] & LDO_VSEL) != ldos[i].vsel && !ldo_put(i, false)) {
		return false;
	}
	mine[i] = true;
	if (!ldo_put(i, true)) {
		return false;
	}
	say("exynos-camera: 2: %s on (%#x)\n", ldos[i].name, LDO_ON | ldos[i].vsel);
	return true;
}

static void
ldo_off(uint32_t i)
{
	if (mine[i]) {
		bool was_broken = broken;

		broken = false;         /* switching off is always tried */
		if (ldo_put(i, false)) {
			mine[i] = false;
		}
		broken = broken || was_broken;
	}
}

/* a pin: an output (CON nibble 1) at the level given */
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
clock_on(void)
{
	gate_found = rd(cmu_top, CIS1_GATE);
	mux_found = rd(cmu_top, CIS1_MUX);
	div_found = rd(cmu_top, CIS1_DIV);
	qch_found = rd(cmu_top, CIS1_QCH);
	if ((mux_found & 1) != 0 || (div_found & 0x1f) != 0) {
		/* the mux on OSCCLK and undivided: 26 MHz, what Android sets; written only when it is not so already */
		wr(cmu_top, CIS1_MUX, mux_found & ~1u);
		wr(cmu_top, CIS1_DIV, div_found & ~0x1fu);
	}
	wr(cmu_top, CIS1_QCH, qch_found | 3);
	wr(cmu_top, CIS1_GATE, gate_found | GATE_MANUAL | GATE_ON);
	clock_mine = true;
	say("exynos-camera: 2: CIS_CLK1: mux %x -> %x, divider %x -> %x, gate %x -> %x (26 MHz from OSCCLK)\n", mux_found,
	    rd(cmu_top, CIS1_MUX), div_found, rd(cmu_top, CIS1_DIV), gate_found, rd(cmu_top, CIS1_GATE));
}

/* The power-down, in the module file's order; only what this run switched on. */
static void
power_down(void)
{
	if (!powered) {
		return;
	}
	pin_out(peric1 + GPC1, MCLK_PIN, false);
	pin_out(peric0 + GPG2, RESET_PIN, false);
	spin_us(1000);
	pin_function(peric1 + GPP5, 2, 0);
	pin_function(peric1 + GPP5, 3, 0);
	ldo_off(VDDD);
	ldo_off(VDDA);
	ldo_off(VDDIO);
	if (clock_mine) {
		wr(cmu_top, CIS1_GATE, gate_found);
		wr(cmu_top, CIS1_QCH, qch_found);
		wr(cmu_top, CIS1_DIV, div_found);
		wr(cmu_top, CIS1_MUX, mux_found);
		clock_mine = false;
	}
	powered = false;
	say("exynos-camera: powered down: MCLK and reset low, the bus pins inputs, LDOs %s; CIS_CLK1's gate %x again\n",
	    (mine[VDDD] || mine[VDDA] || mine[VDDIO]) ? "NOT ALL OFF (a write failed)" : "off as found", gate_found);
}

/* On the way back to the loader (a panic's too): the sensor powered down, if this run powered it up. */
void
hakios_exynos_camera_quiesce(void)
{
	if (!powered || peric0 == 0 || peric1 == 0) {
		return;
	}
	leaving = true;
	say("exynos-camera: at the run's end: the front camera powered down\n");
	power_down();
}

/* a register of the sensor: its 16-bit address written, a repeated start, len bytes read */
static uint32_t
sensor_get(uint16_t reg, uint8_t *buf, uint32_t len)
{
	uint8_t address[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
	uint32_t error = hakios_hsi2c_message(&sensor_bus, SENSOR, false, address, 2, false);

	return error ? error : hakios_hsi2c_message(&sensor_bus, SENSOR, true, buf, len, true);
}

static void
look_pins(const char *when)
{
	say("exynos-camera: %s: reset gpg2-5 CON %x DAT %u; MCLK1 gpc1-5 CON %x DAT %u; bus gpp5-2/3 CON %x %x\n", when,
	    (rd(peric0 + GPG2, 0) >> (4 * RESET_PIN)) & 0xf, (rd(peric0 + GPG2, 4) >> RESET_PIN) & 1,
	    (rd(peric1 + GPC1, 0) >> (4 * MCLK_PIN)) & 0xf, (rd(peric1 + GPC1, 4) >> MCLK_PIN) & 1,
	    (rd(peric1 + GPP5, 0) >> 8) & 0xf, (rd(peric1 + GPP5, 0) >> 12) & 0xf);
}

static void
camera_thread(void *arg, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg;
	uint8_t id = 0, model[2] = { 0 }, revision = 0, frames = 0, mode = 0;
	uint32_t error;

	say("exynos-camera: 1: map the pins' GPIO blocks, CMU_TOP and the two buses (USI10 of PERIC1 for the S2MPB02, "
	    "USI08_I2C for the sensor)\n");
	peric0 = ml_io_map(GPIO_PERIC0_PA, 0x1000);
	peric1 = ml_io_map(GPIO_PERIC1_PA, 0x1000);
	cmu_top = ml_io_map(CMU_TOP_PA, 0x4000);
	if (peric0 == 0 || peric1 == 0 || cmu_top == 0 || !hakios_hsi2c_map(&pmic_bus, say) ||
	    !hakios_hsi2c_map(&sensor_bus, say)) {
		say("exynos-camera: 1: a block could not be mapped\n");
		goto out;
	}
	look_pins("1");
	say("exynos-camera: 1: CIS_CLK1: mux %x, divider %x, gate %x, Q-channel %x\n", rd(cmu_top, CIS1_MUX),
	    rd(cmu_top, CIS1_DIV), rd(cmu_top, CIS1_GATE), rd(cmu_top, CIS1_QCH));
	hakios_hsi2c_power(&pmic_bus);
	hakios_hsi2c_reset(&pmic_bus);
	if (pmic_get(PMIC_ID, &id) != 0) {
		say("exynos-camera: 1: the S2MPB02 does not answer at 0x%x (%x)\n", PMIC, pmic_bus.last_error);
		goto out;
	}
	say("exynos-camera: 1: S2MPB02 ID %#x\n", id);
	for (uint32_t i = 0; i < NLDOS; i++) {
		if (pmic_get(ldos[i].reg, &found[i]) != 0) {
			say("exynos-camera: 1: %s cannot be read (%x)\n", ldos[i].name, pmic_bus.last_error);
			goto out;
		}
		say("exynos-camera: 1: %s: %#x (%s, voltage code %#x, Android's %#x)\n", ldos[i].name, found[i],
		    (found[i] & LDO_ON) ? "on" : "off", found[i] & LDO_VSEL, ldos[i].vsel);
	}
	if (steps < 2) {
		goto out;
	}

	for (uint32_t i = 0; i < NLDOS; i++) {
		if (ldos[i].switched && (found[i] & LDO_ON) && (found[i] & LDO_VSEL) != ldos[i].vsel) {
			say("exynos-camera: 2: %s is on at another voltage: someone else has it; nothing is written\n", ldos[i].name);
			goto out;
		}
	}
	say("exynos-camera: 2: the front camera's power-up (Android's order, less the lens motor's supply)\n");
	powered = true;
	clock_on();
	pin_out(peric0 + GPG2, RESET_PIN, false);
	if (!ldo_on(VDDA) || !ldo_on(VDDIO) || !ldo_on(VDDD)) {
		goto down;
	}
	spin_us(1000);
	hakios_hsi2c_power(&sensor_bus);        /* "on_i2c": the pins to function 3 (and the bus's clocks) */
	hakios_hsi2c_reset(&sensor_bus);
	spin_us(10);
	pin_out(peric0 + GPG2, RESET_PIN, true);
	pin_function(peric1 + GPC1, MCLK_PIN, 2);
	delay_for_interval(9, NSEC_PER_MSEC);
	look_pins("2");

	error = sensor_get(0x0000, model, 2);
	if (error != 0) {
		say("exynos-camera: 2: the sensor does not answer at 0x%x (%x)\n", SENSOR, error);
		goto down;
	}
	(void)sensor_get(0x0002, &revision, 1);
	(void)sensor_get(0x0005, &frames, 1);
	(void)sensor_get(0x0100, &mode, 1);
	say("exynos-camera: 2: THE SENSOR ANSWERS: model ID %#06x, revision %#x, frame count %u, mode_select %#x "
	    "(0: standby)\n", (model[0] << 8) | model[1], revision, frames, mode);
down:
	power_down();
out:
	thread_terminate(current_thread());
}

void
hakios_exynos_camera_init(void)
{
	uint32_t steps = 0, ipclk = 400;
	thread_t thread;

	if (!PE_parse_boot_argn("cam_s9", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != 0x10000000) {
		return;                 /* not the Exynos 9810 */
	}
	PE_parse_boot_argn("cam_s9_ipclk", &ipclk, sizeof(ipclk));
	if (ipclk < 26 || ipclk > 400) {
		ipclk = 400;
	}
	pmic_bus.ipclk_mhz = sensor_bus.ipclk_mhz = ipclk;
	if (steps > 2) {
		steps = 2;
	}
	if (kernel_thread_start(camera_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
