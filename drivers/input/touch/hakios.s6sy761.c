#define HAKIOS_SOC_FIXED
/*
 * Touch input for the Galaxy S9 (starlte, Exynos 9810): a Samsung S6SY761 at 0x48 on the I2C bus of USI03
 * (HSI2C at 0x104B0000). Its records reach /dev/hakios-touch (sys/hakios.input.h). Enabled by boot-arg s6sy761=1;
 * every address in this file belongs to this phone.
 *
 * The chip's supplies are boot-on in the device tree and the boot loader leaves them on; nothing here turns them
 * on at first. The PMIC is reached through ACPM over SPEEDY, and only if the chip does not answer (power_cycle()).
 *
 * The bus is an Exynos HSI2C in auto mode, polled (drivers/soc/exynos/hakios.exynos_hsi2c.c). The clock gates,
 * the USI block's I2C mode and the two pins' I2C function are set up before the first transfer, as Linux's drivers
 * and the WoA port do. Silicium has no I2C for this SoC, so the UEFI does none of it.
 *
 * Sources for these facts: the phone's device tree (hsi2c@104B0000, touchscreen@48, gpa1, the PERIC0 system
 * register); Linux's i2c-exynos5.c and usi_v2.c; the Exynos 9810 CMU tables; sec_ts (read for these facts, not
 * copied); mainline Linux's s6sy761.c; and the WoA port's HSI2C and touch drivers (github.com/ntdevlabs/exynos9810-woa),
 * which run on the S9+ with the same bus, address and supplies.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.exynos_soc.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/hakios.log.h>
#include <sys/hakios.xnu.h>


/* (every line also goes to the loader's log, as in the other drivers: see drivers/input/hakios.buttons.c) */

/* ---- where things are ---- */
#define ALIVE_PA                0x14050000UL
#define GPA1                    0x040
#define IRQ_PIN                 0
#define CHIP                    0x48

/*
 * Addresses:
 *   HSI2C (USI03)      0x104B0000
 *   SYSREG_PERIC0      0x1041101C   USI03's mode: 4 = I2C
 *   CMU_PERIC0         0x10400000   gates +0x2018, +0x206C, +0x20DC, +0x20E0 (bit 20 manual, bit 21 on), Q-channel
 *                                   +0x3038 (bits 0-1); the IP clock's divider +0x1810 [3:0] (of 400 MHz) and mux +0x120
 *   PERIC0 GPIO        0x10430020   gpp1: pins 4 (SCL) and 5 (SDA), function 2
 *   the touch's IRQ    0x14050044   gpa1 pin 0 of the ALIVE block, low while the chip has events
 */
static struct hakios_hsi2c bus = {
	.name = "s6sy761",
	.regs_pa = EXYNOS_PERIC0_USI03_HSI2C_PA,                  /* hsi2c@104B0000: USI03 of PERIC0 */
	.usi_mode_pa = EXYNOS_PERIC0_USI03_MODE_PA,              /* SYSREG_PERIC0: USI03's mode */
	.cmu_pa = EXYNOS_CMU_PERIC0_PA, .cmu_size = 0x4000,       /* CMU_PERIC0 */
	.gates = { 0x2018, 0x206C, 0x20DC, 0x20E0 }, .ngates = 4,
	.qch = 0x3038, .div = 0x1810, .mux = 0x0120,
	.pins_pa = EXYNOS_GPIO_PERIC0_PA + 0x20,                  /* PERIC0 GPIO, bank gpp1: SCL pin 4, SDA pin 5 */
	.pins_mask = 0xffu << 16, .pins_want = 0x22u << 16,
	.ipclk_mhz = 400, .bus_hz = 400000,       /* CMU divider 0 on the 400 MHz branch, as seen on the phone */
};

/* ---- the chip ---- */

/*
 * The chip's protocol: a command byte, with data if any, is written; or it is written and the answer is read.
 *   0x55 boot status (BOOT_APPLICATION: its application runs)
 *   0x52 id
 *   0x23 panel size: X and Y resolution, big-endian
 *   0x21 firmware check
 *   0x10 sense on
 *   0x62 empty the event queue
 *   0x60 one event (8 bytes); 0x61 the rest of the queue
 * An event's byte 0: [1:0] kind (0 coordinate, 1 status), [5:2] the finger (from 1), [7:6] 1 down, 2 moved, 3 up.
 * X = b1 << 4 | b3 >> 4, Y = b2 << 4 | b3 & 15. Byte 7 [5:0]: how many more events are waiting.
 */
#define CMD_SENSE_ON            0x10
#define CMD_FW_CHECK            0x21
#define CMD_PANEL_INFO          0x23
#define CMD_READ_ID             0x52
#define CMD_BOOT_STATUS         0x55
#define CMD_ONE_EVENT           0x60
#define CMD_ALL_EVENTS          0x61
#define CMD_CLEAR_EVENTS        0x62
#define BOOT_APPLICATION        0x20
#define EVENT_SIZE              8
#define EVENTS_MOST             31      /* most events read in one batch; more than this empties the queue */
#define FINGERS                 10

#define POLL_MS                 4       /* the IRQ pin is polled this often */
#define MINUTE_TICKS            (60 * 1000 / POLL_MS)

static vm_offset_t alive;
static uint32_t res_x = 4096, res_y = 4096;
static uint32_t ldo_cycles = 1;       /* s6sy761_ldo=0: never switch the chip's supplies */
static struct {
	uint64_t events, downs;
} stats;
static bool finger_down[FINGERS];

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

/* write a command byte and its data to the chip */
static uint32_t
chip_write(uint8_t cmd, const uint8_t *data, uint32_t len)
{
	uint8_t buf[8] = { cmd };

	if (len > sizeof(buf) - 1) {
		return 1;
	}
	memcpy(buf + 1, data, len);
	return hakios_hsi2c_message(&bus, CHIP, false, buf, 1 + len, true);
}

/*
 * write a command byte, then read its answer. Tried twice: the first transfer after a reset may be lost.
 * The two transfers are separate messages, with a STOP between them (stop-after-trans in the device tree).
 */
static uint32_t
chip_read(uint8_t cmd, uint8_t *answer, uint32_t len)
{
	uint32_t error = 0;

	for (int attempt = 0; attempt < 2; attempt++) {
		error = hakios_hsi2c_message(&bus, CHIP, false, &cmd, 1, true);
		if (error == 0) {
			error = hakios_hsi2c_message(&bus, CHIP, true, answer, len, true);
		}
		if (error == 0) {
			break;
		}
	}
	return error;
}

/* ---- events ---- */

extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);

/* s6sy761_trace=N: say the first N finger events, with their time (ms, from the 24 MHz counter), to measure the rate */
static uint32_t trace_left;

/* kern.hakios.touch_events: finger events read from the chip since boot; the first-boot report (userland/src/selftest.c) prints it */
static uint32_t touch_events;
SYSCTL_DECL(_kern_hakios);
SYSCTL_UINT(_kern_hakios, OID_AUTO, touch_events, CTLFLAG_RD | CTLFLAG_LOCKED, &touch_events, 0,
    "finger events read from the S6SY761");

/* tell the CPU governor how many fingers are down: its touch boost is held while any is (gov_policy=2) */
static void
fingers_changed(void)
{
	uint32_t down = 0;

	for (uint32_t f = 0; f < FINGERS; f++) {
		down += finger_down[f] ? 1 : 0;
	}
	hakios_exynos_cpufreq_fingers(down);
}

/* lift every finger that is down: for when the chip's event stream was lost or reset */
static void
all_up(void)
{
	for (uint32_t f = 0; f < FINGERS; f++) {
		if (finger_down[f]) {
			finger_down[f] = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, f, 0, 0);
		}
	}
	fingers_changed();
}

static void
handle_event(const uint8_t *e)
{
	uint32_t kind = e[0] & 3;

	if (kind == 0) {
		uint32_t id = (e[0] >> 2) & 0xf, state = e[0] >> 6;
		uint32_t x = (uint32_t)e[1] << 4 | e[3] >> 4, y = (uint32_t)e[2] << 4 | (e[3] & 0xf);
		uint32_t nx, ny;

		if (id == 0 || id > FINGERS || state == 0) {
			return;
		}
		id--;
		/* the CPU governor's touch boost (cpufreq_gov=1): a stamp, and at the first event its thread is woken; no wait */
		hakios_exynos_cpufreq_touch();
		touch_events++;
		if (trace_left > 0) {
			trace_left--;
			say("s6sy761: t %llu ms: finger %u %s at %u,%u\n", (unsigned long long)(mach_absolute_time() / 24000), id,
			    state == 3 ? "up" : (finger_down[id] ? "moved" : "down"), x, y);
		}
		nx = x >= res_x - 1 ? 65535 : x * 65535 / (res_x - 1);
		ny = y >= res_y - 1 ? 65535 : y * 65535 / (res_y - 1);
		stats.events++;
		if (state == 3) {
			if (finger_down[id]) {
				finger_down[id] = false;
				fingers_changed();
				hakios_touch_post(HAKIOS_TOUCH_UP, id, nx, ny);
			}
		} else if (!finger_down[id]) {
			finger_down[id] = true;
			fingers_changed();
			stats.downs++;
			hakios_touch_post(HAKIOS_TOUCH_DOWN, id, nx, ny);
		} else {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, id, nx, ny);
		}
	} else if (kind == 1) {
		uint32_t type = (e[0] >> 2) & 0xf, sid = e[1];

		if ((type == 2 || type == 7) && sid == 0 && e[2] == BOOT_APPLICATION) {
			/* the chip restarted itself (its watchdog); sensing is off again */
			say("s6sy761: the chip restarted itself: sense on again\n");
			all_up();
			(void)chip_write(CMD_SENSE_ON, NULL, 0);
		} else if (type == 1 && sid == 1) {
			say("s6sy761: the chip's event queue was full: emptied, every finger up\n");
			all_up();
			(void)chip_write(CMD_CLEAR_EVENTS, NULL, 0);
		}
	}
}

/*
 * Read the queue: one event, then the rest that its byte 7 says are waiting.
 * A lost read can be a finger's "up" (seen on the phone: about 3 failed transfers a minute), so each read is
 * tried once more; chip_read() has already retried inside.
 */
static void
read_events(void)
{
	uint8_t one[EVENT_SIZE], rest[EVENT_SIZE * EVENTS_MOST];
	uint32_t left;

	if ((chip_read(CMD_ONE_EVENT, one, sizeof(one)) != 0 && chip_read(CMD_ONE_EVENT, one, sizeof(one)) != 0) || one[0] == 0) {
		return;
	}
	handle_event(one);
	left = one[7] & 0x3f;
	if (left == 0) {
		return;
	}
	if (left > EVENTS_MOST) {
		say("s6sy761: %u events waiting, more than %u: the queue emptied, every finger up\n", left, EVENTS_MOST);
		all_up();
		(void)chip_write(CMD_CLEAR_EVENTS, NULL, 0);
		return;
	}
	if (chip_read(CMD_ALL_EVENTS, rest, left * EVENT_SIZE) != 0 && chip_read(CMD_ALL_EVENTS, rest, left * EVENT_SIZE) != 0) {
		return;
	}
	for (uint32_t i = 0; i < left; i++) {
		handle_event(rest + i * EVENT_SIZE);
	}
}

static bool
irq_low(void)
{
	return ((rd(alive, GPA1 + 4) >> IRQ_PIN) & 1) == 0;
}

/* ---- the chip's supplies, switched off and on ---- */

/*
 * LDO35 (tsp_io, 1.8 V) and LDO43 (tsp_avdd, 3.0 V) of the S2MPS18, as on the S9 and S9+ (the phone's device tree).
 * Their control registers are 0x6C and 0x74; bits 7:6 are the on mode (0xC0).
 *
 * Why this exists: the mainline Linux port to the S9+ (github.com/SheroAbi/galaxy-s9plus-mainline-linux, s9p-acpm.c;
 * GPL, read for these facts) finds the controller dead after the hand-over from the previous boot stage, until it
 * sees a power-on reset.
 */
#define LDO35_CTRL              0x6c    /* tsp_io */
#define LDO43_CTRL              0x74    /* tsp_avdd */
#define LDO_ON_MODE             0xc0
#define LDO_OFF_MS              2000
#define LDO_AVDD_TO_DVDD_MS     4       /* sec_ts_power(): off, AVDD, then 4 ms, then DVDD */
#define LDO_DVDD_TO_AVDD_MS     1       /* ... on, DVDD, then 1 ms, then AVDD */

/*
 * Set one of the two supplies, through ACPM (drivers/soc/exynos/hakios.exynos_acpm.c). Only those bits of only those
 * two registers are written, and each is read back.
 */
static bool
ldo_set(uint8_t reg, bool on)
{
	uint8_t v = 0;

	if (reg != LDO35_CTRL && reg != LDO43_CTRL) {
		return false;           /* nothing else of the PMIC, ever */
	}
	if (hakios_acpm_pmic_update(HAKIOS_ACPM_PMIC_PMIC, reg, on ? LDO_ON_MODE : 0, LDO_ON_MODE) != 0 ||
	    hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, reg, &v) != 0) {
		say("s6sy761: the PMIC's register %02x could not be %s\n", reg, on ? "switched on" : "switched off");
		return false;
	}
	if (((v & LDO_ON_MODE) == LDO_ON_MODE) != on) {
		say("s6sy761: the PMIC's register %02x reads %02x after it was switched %s\n", reg, v, on ? "on" : "off");
		return false;
	}
	return true;
}

/*
 * Off for LDO_OFF_MS, then on, in Android's order (sec_ts.c sec_ts_power()): LDO43 off, 4 ms, LDO35 off; then
 * LDO35 on, 1 ms, LDO43 on. The I/O supply goes off only once the analog one has: never the chip's analog side
 * powered with its I/O side not. Both come back on whatever happened above: never leave the touch unpowered.
 */
static bool
power_cycle(void)
{
	uint8_t l35 = 0, l43 = 0;
	bool ok;

	if (!hakios_acpm_init(say)) {
		say("s6sy761: no ACPM: its supplies cannot be switched\n");
		return false;
	}
	(void)hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, LDO35_CTRL, &l35);
	(void)hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, LDO43_CTRL, &l43);
	say("s6sy761: its supplies: LDO35 %02x, LDO43 %02x; off for %u ms, then on\n", l35, l43, LDO_OFF_MS);
	ok = ldo_set(LDO43_CTRL, false);
	if (ok) {
		delay_for_interval(LDO_AVDD_TO_DVDD_MS, NSEC_PER_MSEC);
		ok = ldo_set(LDO35_CTRL, false);
	}
	delay_for_interval(LDO_OFF_MS, NSEC_PER_MSEC);
	ok = ldo_set(LDO35_CTRL, true) && ok;
	delay_for_interval(LDO_DVDD_TO_AVDD_MS, NSEC_PER_MSEC);
	ok = ldo_set(LDO43_CTRL, true) && ok;
	return ok;
}

/* ---- the steps ---- */

/*
 * The bring-up is made in steps, each said in the log before it is made. Boot-arg s6sy761_steps=N stops after the
 * first N:
 *   1  map the blocks and read what they are set to          (nothing written)
 *   2  the bus's clocks on, USI03 to I2C, the pins to I2C    (the first writes: clock gates and the mode)
 *   3  the bus reset and set to 400 kHz
 *   4  ask the chip: boot status, id, panel, firmware check  (the first transfers)
 *   5  sense on, the event queue emptied
 *   6  watch it (the default)
 */
static void
touch_thread(void *arg, __unused wait_result_t wr_)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg, step = 0, error;
	uint8_t answer[16];

	say("s6sy761: step 1: map the HSI2C, the PERIC0 clocks and system register, the pins\n");
	alive = ml_io_map(ALIVE_PA, 0x1000);
	if (!alive || !hakios_hsi2c_map(&bus, say)) {
		goto out;
	}
	say("s6sy761: the IRQ pin: CON %x, %s\n", rd(alive, GPA1) & 0xf, irq_low() ? "low (events waiting)" : "high");
	if (++step >= steps) {
		goto out;
	}

	say("s6sy761: step 2: the clocks of USI03 on, USI03 to I2C, its pins to I2C\n");
	hakios_hsi2c_power(&bus);
	say("s6sy761: now: USI03 mode %x, gpp1 CON %x\n", *(volatile uint32_t *)bus.usi, *(volatile uint32_t *)bus.pins);
	if (++step >= steps) {
		goto out;
	}

	say("s6sy761: step 3: the bus reset, at %u kHz from %u MHz\n", bus.bus_hz / 1000, bus.ipclk_mhz);
	hakios_hsi2c_reset(&bus);
	if (++step >= steps) {
		goto out;
	}

	say("s6sy761: step 4: ask the chip at 0x%x\n", CHIP);
	error = chip_read(CMD_BOOT_STATUS, answer, 1);
	/* no answer: cycle the supplies, up to three times; after each, ask every 500 ms for 12 s */
	for (uint32_t cycle = 1; error != 0 && ldo_cycles != 0 && cycle <= 3; cycle++) {
		say("s6sy761: no answer (INT_STAT %x): its supplies switched off and on, cycle %u of 3\n", error, cycle);
		if (!power_cycle()) {
			break;
		}
		for (uint32_t probe = 0; probe < 24 && error != 0; probe++) {
			delay_for_interval(500, NSEC_PER_MSEC);
			error = chip_read(CMD_BOOT_STATUS, answer, 1);
		}
		if (error == 0) {
			say("s6sy761: the chip answers after %u cycle(s) of its supplies\n", cycle);
		}
	}
	if (error != 0) {
		say("s6sy761: no answer (INT_STAT %x): the bus is not working, or the chip has no power (its supplies are "
		    "the PMIC's LDO35 and LDO43%s)\n", error, ldo_cycles ? ", cycled without effect" : "; s6sy761_ldo=0 left them");
		goto out;
	}
	say("s6sy761: boot status %x (%s)\n", answer[0], answer[0] == BOOT_APPLICATION ? "its firmware runs" :
	    answer[0] == 0x10 ? "IN ITS BOOT LOADER: no firmware running" : "unknown");
	if (answer[0] != BOOT_APPLICATION) {
		say("s6sy761: not its application: no sensing asked of it (its firmware would have to be written first)\n");
		goto out;
	}
	if (chip_read(CMD_READ_ID, answer, 3) == 0) {
		say("s6sy761: id %02x %02x %02x\n", answer[0], answer[1], answer[2]);
	}
	if (chip_read(CMD_FW_CHECK, answer, 1) == 0) {
		say("s6sy761: firmware check %x (%s)\n", answer[0], (answer[0] & 0x80) ? "good" : "BAD");
	}
	if (chip_read(CMD_PANEL_INFO, answer, 11) == 0) {
		uint32_t x = (uint32_t)answer[0] << 8 | answer[1], y = (uint32_t)answer[2] << 8 | answer[3];

		say("s6sy761: panel: %u x %u, %u TX x %u RX channels\n", x, y, answer[8], answer[9]);
		if (x > 1 && y > 1) {
			res_x = x, res_y = y;
		}
	}
	if (++step >= steps) {
		goto out;
	}

	say("s6sy761: step 5: sense on, the event queue emptied\n");
	if ((error = chip_write(CMD_SENSE_ON, NULL, 0)) != 0) {
		say("s6sy761: sense on failed (%x)\n", error);
		goto out;
	}
	delay_for_interval(10, NSEC_PER_MSEC);
	(void)chip_write(CMD_CLEAR_EVENTS, NULL, 0);
	if (++step >= steps) {
		goto out;
	}

	say("s6sy761: step 6: watching the IRQ pin every %u ms; the coordinates are 0-%u x 0-%u\n", POLL_MS, res_x - 1,
	    res_y - 1);
	/* up to four rounds of reads per tick while the pin is low */
	for (unsigned tick = 1;; tick++) {
		for (int rounds = 0; rounds < 4 && irq_low(); rounds++) {
			read_events();
		}
		if (tick % MINUTE_TICKS == 0) {
			say("s6sy761: a minute: %llu touches, %llu events, %llu transfers, %llu failed (last %x)\n", stats.downs,
			    stats.events, bus.transfers, bus.errors, bus.last_error);
		}
		/* give up when more than half of at least 1000 transfers have failed */
		if (bus.errors >= 1000 && bus.errors * 2 > bus.transfers) {
			say("s6sy761: more than half of %llu transfers have failed: giving up\n", bus.transfers);
			all_up();               /* (no finger left down for good) */
			break;
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
out:
	if (step < 6 && steps < 6) {
		say("s6sy761: stopping here (s6sy761_steps=%u)\n", steps);
	}
	thread_terminate(current_thread());
}

/*
 * Boot args: s6sy761=1 enables the driver. s6sy761_steps=N (default 6, see touch_thread()). s6sy761_ldo=0 never
 * switches the supplies. s6sy761_ipclk=MHz sets the HSI2C's input clock (default 400; the other drivers assume
 * 200). s6sy761_trace=N, s6sy761_pri=N: see above.
 */
void
hakios_s6sy761_init(void)
{
	uint32_t enabled = 0, steps = 6;
	thread_t thread;

	if (!PE_parse_boot_argn("s6sy761", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	PE_parse_boot_argn("s6sy761_steps", &steps, sizeof(steps));
	PE_parse_boot_argn("s6sy761_ldo", &ldo_cycles, sizeof(ldo_cycles));
	PE_parse_boot_argn("s6sy761_ipclk", &bus.ipclk_mhz, sizeof(bus.ipclk_mhz));
	if (steps == 0 || bus.ipclk_mhz < 26 || bus.ipclk_mhz > 400) {
		return;
	}
	PE_parse_boot_argn("s6sy761_trace", &trace_left, sizeof(trace_left));
	{
		/*
		 * s6sy761_pri=N: the polling thread's priority. Default 92 (BASEPRI_PREEMPT, as fb_pri, the display's
		 * presenter). At the kernel's default it lost the processors to the window server under load on two cores,
		 * and a finger's moves came in bursts (seen on the phone: scrolling sticky, long presses by mistake).
		 */
		uint32_t pri = 92;

		PE_parse_boot_argn("s6sy761_pri", &pri, sizeof(pri));
		if (kernel_thread_start_priority((thread_continue_t)touch_thread, (void *)(uintptr_t)steps, (integer_t)pri,
		    &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}
