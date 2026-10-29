/*
 * The touch screen of the Galaxy S9 (starlte, Exynos 9810): a Samsung S6SY761 at 0x48 on the I2C bus of USI03
 * (HSI2C at 0x104B0000), as a source of touch records for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg
 * s6sy761=1: every address below belongs to this phone.
 *
 * Nothing here powers the chip. Its two supplies (LDO35 1.8 V, LDO43 3.0 V of the S2MPS18) are "boot-on" in Samsung's
 * device tree, the boot loader leaves them on, and the WoA port's touch driver for the S9+ (same bus, address and
 * supplies) works without touching them. The PMIC is reached through the always-on processor (ACPM, over SPEEDY): not
 * from here. If the chip does not answer, that is the first thing to suspect (step 4 says so).
 *
 * The bus is this file's own: an Exynos HSI2C in its "auto" mode, polled. What the bring-up does before the first
 * transfer (the clock gates of USI03, the USI block set to I2C, the two pins to their I2C function) is what Linux's
 * drivers and the WoA port do; the UEFI does none of it (Silicium has no I2C for this SoC).
 *   HSI2C (USI03)      0x104B0000   CTL 0x00, FIFO_CTL 0x04, TRAILING 0x08, INT_EN 0x20, INT_STAT 0x24, FIFO_STAT 0x30,
 *                                   TX 0x34, RX 0x38, CONF 0x40, AUTO_CONF 0x44, TIMEOUT 0x48, TRANS_STATUS 0x50,
 *                                   FS1-3 0x60-0x68, ADDR 0x70, USI_CON 0xC4
 *   SYSREG_PERIC0      0x1041101C   USI03's mode: 4 = I2C
 *   CMU_PERIC0         0x10400000   gates +0x2018, +0x206C, +0x20DC, +0x20E0 (bit 20 manual, bit 21 on), Q-channel
 *                                   +0x3038 (bits 0-1); the IP clock's divider +0x1810 [3:0] (of 400 MHz) and mux +0x120
 *   PERIC0 GPIO        0x10430020   gpp1: pins 4 (SCL) and 5 (SDA), function 2
 *   the touch's IRQ    0x14050044   gpa1 pin 0 of the ALIVE block, low while the chip has events
 * The chip's protocol: write a command byte (and its data), or write it and read the answer. 0x55 boot status (0x20:
 * its application runs), 0x52 its id, 0x23 the panel (X and Y resolution, big-endian), 0x21 its firmware's check,
 * 0x10 sense on, 0x62 empty the event queue, 0x60 one event (8 bytes), 0x61 the rest. An event's byte 0: [1:0] kind
 * (0 coordinate, 1 status), [5:2] the finger (from 1), [7:6] 1 down, 2 moved, 3 up; X = b1 << 4 | b3 >> 4,
 * Y = b2 << 4 | b3 & 15; b7 [5:0] how many more are waiting.
 * Sources: the phone's device tree (hsi2c@104B0000, touchscreen@48, gpa1, the PERIC0 system register), Linux's
 * i2c-exynos5.c, usi_v2.c, the Exynos 9810 CMU tables and sec_ts (read for these facts, not copied), mainline Linux's
 * s6sy761.c, and the WoA port's HSI2C and touch drivers (github.com/ntdevlabs/exynos9810-woa), which run on the S9+.
 *
 * The bring-up is made in steps, each said in the log before it is made. Boot-arg s6sy761_steps=N stops after the
 * first N:
 *   1  map the blocks and read what they are set to          (nothing written)
 *   2  the bus's clocks on, USI03 to I2C, the pins to I2C    (the first writes: clock gates and the mode)
 *   3  the bus reset and set to 400 kHz
 *   4  ask the chip: boot status, id, panel, firmware check  (the first transfers)
 *   5  sense on, the event queue emptied
 *   6  watch it (the default)
 *
 * Boot-arg s6sy761_ipclk=MHz: the HSI2C's input clock, if step 1 says it is not the 200 MHz both other drivers assume.
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

/* ---- where things are ---- */
#define HSI2C_PA                0x104B0000UL
#define SYSREG_PERIC0_PA        0x10411000UL
#define USI03_SW_CONF           0x01C           /* in SYSREG_PERIC0 */
#define USI_MODE_I2C            0x4
#define CMU_PERIC0_PA           0x10400000UL
#define CMU_PERIC0_SIZE         0x4000
#define CMU_MUX_IP              0x0120          /* bit 4: the IP clock from the 400 MHz branch */
#define CMU_DIV_USI03           0x1810          /* [3:0]: divided by n + 1 */
#define CMU_GATE_MANUAL         (1u << 20)
#define CMU_GATE_ON             (1u << 21)
#define CMU_QCH_USI03           0x3038
static const uint32_t cmu_gates[] = { 0x2018, 0x206C, 0x20DC, 0x20E0 };
#define GPIO_PERIC0_PA          0x10430000UL
#define GPP1                    0x020           /* bank gpp1: CON +0, DAT +4, PUD +8 */
#define SCL_PIN                 4
#define SDA_PIN                 5
#define PIN_I2C                 0x2
#define ALIVE_PA                0x14050000UL
#define GPA1                    0x040
#define IRQ_PIN                 0

/* ---- the HSI2C ---- */
#define CTL                     0x00
#define  CTL_MASTER             (1u << 3)
#define  CTL_RXCHON             (1u << 6)
#define  CTL_TXCHON             (1u << 7)
#define  CTL_SW_RST             (1u << 31)
#define FIFO_CTL                0x04
#define TRAILING                0x08
#define INT_EN                  0x20
#define INT_STAT                0x24
#define  INT_DONE               (1u << 7)
#define  INT_ERRORS             (0xfu << 8)     /* abort, no ack, no device, timeout */
#define FIFO_STAT               0x30
#define  TX_FULL                (1u << 7)
#define  TX_EMPTY               (1u << 8)
#define  RX_EMPTY               (1u << 24)
#define TX_DATA                 0x34
#define RX_DATA                 0x38
#define CONF                    0x40
#define  CONF_AUTO              (1u << 31)
#define AUTO_CONF               0x44
#define  AUTO_READ              (1u << 16)
#define  AUTO_STOP              (1u << 17)
#define  AUTO_RUN               (1u << 31)
#define TIMEOUT                 0x48
#define TRANS_STATUS            0x50
#define  MASTER_BUSY            (1u << 17)
#define TIMING_FS1              0x60
#define TIMING_FS2              0x64
#define TIMING_FS3              0x68
#define ADDR                    0x70
#define USI_CON                 0xC4

#define BUS_HZ                  400000
#define CHIP                    0x48

/* ---- the chip ---- */
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
#define EVENTS_MOST             31
#define FINGERS                 10

#define POLL_MS                 4
#define MINUTE_TICKS            (60 * 1000 / POLL_MS)

static vm_offset_t i2c, sysreg, cmu, gpio, alive;
static uint32_t ipclk_mhz = 200;
static uint32_t res_x = 4096, res_y = 4096;
static struct {
	uint64_t transfers, errors, events, downs;
	uint32_t last_error;
} stats;
static bool finger_down[FINGERS];

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

/* ---- the bus ---- */

/*
 * 400 kHz from the input clock, the way both other drivers work it out: the divider for 15 clocks of the input per
 * bit, then the high time and the start's hold as masks of n bits (n at most 7).
 */
static void
bus_timing(void)
{
	uint32_t div = ipclk_mhz * 1000000 / (BUS_HZ * 15);
	uint32_t n = (9 * ipclk_mhz) / ((div + 1) * 10);

	if (n > 7) {
		n = 7;
	}
	uint32_t scl_h = (0xFFFFFFFFu >> n << n) & 0xff;
	uint32_t start_hd = (0xFFFFFFFFu >> (n ? n - 1 : 0) << (n ? n - 1 : 0)) & 0xff;

	wr(i2c, TIMING_FS3, (rd(i2c, TIMING_FS3) & ~(0xffu << 16)) | (div & 0xff) << 16);
	wr(i2c, TIMING_FS2, (rd(i2c, TIMING_FS2) & ~0xffu) | scl_h);
	wr(i2c, TIMING_FS1, (rd(i2c, TIMING_FS1) & ~(0xffu << 16)) | start_hd << 16);
}

static void
bus_reset(void)
{
	wr(i2c, USI_CON, 0);
	wr(i2c, INT_STAT, rd(i2c, INT_STAT));
	wr(i2c, CTL, rd(i2c, CTL) | CTL_SW_RST);
	delay(10);
	wr(i2c, CTL, rd(i2c, CTL) & ~CTL_SW_RST);
	bus_timing();
	wr(i2c, CTL, CTL_MASTER);
	wr(i2c, TRAILING, 0xffffff);
	wr(i2c, TIMEOUT, rd(i2c, TIMEOUT) & ~(1u << 31));
	wr(i2c, CONF, rd(i2c, CONF) | CONF_AUTO);
}

/*
 * One message, with a STOP after it (the bus's device tree asks for that: stop-after-trans). 0 or the INT_STAT error
 * bits, or 1 for a time-out of ours. A failed transfer leaves the bus reset.
 */
static uint32_t
bus_message(bool reading, uint8_t *buf, uint32_t len)
{
	uint32_t trigger = len < 8 ? len : 8, done = 0, status = 0;
	int waited;

	stats.transfers++;
	wr(i2c, INT_STAT, rd(i2c, INT_STAT));
	wr(i2c, FIFO_CTL, 0x3 | trigger << 4 | trigger << 16);
	wr(i2c, CTL, CTL_MASTER | (reading ? CTL_RXCHON : CTL_TXCHON));
	wr(i2c, ADDR, CHIP << 10);
	wr(i2c, INT_EN, INT_DONE | INT_ERRORS);
	wr(i2c, AUTO_CONF, len | (reading ? AUTO_READ : 0) | AUTO_STOP);
	wr(i2c, AUTO_CONF, rd(i2c, AUTO_CONF) | AUTO_RUN);

	/* a second at most, in steps of a microsecond and some */
	for (waited = 0; waited < 1000000; waited++) {
		uint32_t fifo = rd(i2c, FIFO_STAT);

		status = rd(i2c, INT_STAT);
		if (status & INT_ERRORS) {
			break;
		}
		if (reading) {
			while (done < len && !(fifo & RX_EMPTY)) {
				buf[done++] = (uint8_t)rd(i2c, RX_DATA);
				fifo = rd(i2c, FIFO_STAT);
			}
			if (done == len) {
				break;
			}
		} else {
			while (done < len && !(fifo & TX_FULL)) {
				wr(i2c, TX_DATA, buf[done++]);
				fifo = rd(i2c, FIFO_STAT);
			}
			if (done == len && (status & INT_DONE) && (fifo & TX_EMPTY)) {
				break;
			}
		}
		delay(1);
	}
	/* the STOP gone out: the master no longer busy (the chip may stretch the clock) */
	for (int idle = 0; idle < 100000 && (rd(i2c, TRANS_STATUS) & MASTER_BUSY); idle++) {
		delay(1);
	}
	if ((status & INT_ERRORS) || waited >= 1000000) {
		stats.errors++;
		stats.last_error = (status & INT_ERRORS) ? status : 1;
		bus_reset();
		return stats.last_error;
	}
	return 0;
}

/* the chip: a command and its data */
static uint32_t
chip_write(uint8_t cmd, const uint8_t *data, uint32_t len)
{
	uint8_t buf[8] = { cmd };

	if (len > sizeof(buf) - 1) {
		return 1;
	}
	memcpy(buf + 1, data, len);
	return bus_message(false, buf, 1 + len);
}

/* ... a command and its answer (a second try after a failure: the first transfer after a reset may be lost) */
static uint32_t
chip_read(uint8_t cmd, uint8_t *answer, uint32_t len)
{
	uint32_t error = 0;

	for (int attempt = 0; attempt < 2; attempt++) {
		error = bus_message(false, &cmd, 1);
		if (error == 0) {
			error = bus_message(true, answer, len);
		}
		if (error == 0) {
			break;
		}
	}
	return error;
}

/* ---- events ---- */

static void
all_up(void)
{
	for (uint32_t f = 0; f < FINGERS; f++) {
		if (finger_down[f]) {
			finger_down[f] = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, f, 0, 0);
		}
	}
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
		nx = x >= res_x - 1 ? 65535 : x * 65535 / (res_x - 1);
		ny = y >= res_y - 1 ? 65535 : y * 65535 / (res_y - 1);
		stats.events++;
		if (state == 3) {
			if (finger_down[id]) {
				finger_down[id] = false;
				hakios_touch_post(HAKIOS_TOUCH_UP, id, nx, ny);
			}
		} else if (!finger_down[id]) {
			finger_down[id] = true;
			stats.downs++;
			hakios_touch_post(HAKIOS_TOUCH_DOWN, id, nx, ny);
		} else {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, id, nx, ny);
		}
	} else if (kind == 1) {
		uint32_t type = (e[0] >> 2) & 0xf, sid = e[1];

		if ((type == 2 || type == 7) && sid == 0 && e[2] == BOOT_APPLICATION) {
			/* the chip started again by itself (its watchdog): sensing is off again */
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

/* everything the chip has: one event, then the rest it says are waiting */
static void
read_events(void)
{
	uint8_t one[EVENT_SIZE], rest[EVENT_SIZE * EVENTS_MOST];
	uint32_t left;

	if (chip_read(CMD_ONE_EVENT, one, sizeof(one)) != 0 || one[0] == 0) {
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
	if (chip_read(CMD_ALL_EVENTS, rest, left * EVENT_SIZE) != 0) {
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

/* ---- the steps ---- */

static void
touch_thread(void *arg, __unused wait_result_t wr_)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg, step = 0, error;
	uint8_t answer[16];

	say("s6sy761: step 1: map the HSI2C, the PERIC0 clocks and system register, the pins\n");
	i2c = ml_io_map(HSI2C_PA, 0x1000);
	sysreg = ml_io_map(SYSREG_PERIC0_PA, 0x1000);
	cmu = ml_io_map(CMU_PERIC0_PA, CMU_PERIC0_SIZE);
	gpio = ml_io_map(GPIO_PERIC0_PA, 0x1000);
	alive = ml_io_map(ALIVE_PA, 0x1000);
	if (!i2c || !sysreg || !cmu || !gpio || !alive) {
		say("s6sy761: a block could not be mapped\n");
		goto out;
	}
	{
		uint32_t mux = rd(cmu, CMU_MUX_IP), div = rd(cmu, CMU_DIV_USI03);

		say("s6sy761: USI03 mode %x (4 is I2C), CMU mux %x, divider %x (the input clock: 400 MHz / %u = %u MHz if the "
		    "mux takes the 400 MHz branch; assumed %u), gates %x %x %x %x, Q-channel %x\n",
		    rd(sysreg, USI03_SW_CONF), mux, div, (div & 0xf) + 1, 400 / ((div & 0xf) + 1), ipclk_mhz,
		    rd(cmu, cmu_gates[0]), rd(cmu, cmu_gates[1]), rd(cmu, cmu_gates[2]), rd(cmu, cmu_gates[3]),
		    rd(cmu, CMU_QCH_USI03));
		say("s6sy761: gpp1 CON %x (pins 4, 5: function 2 is I2C), the IRQ pin: CON %x, %s\n", rd(gpio, GPP1),
		    rd(alive, GPA1) & 0xf, irq_low() ? "low (events waiting)" : "high");
	}
	if (++step >= steps) {
		goto out;
	}

	say("s6sy761: step 2: the clocks of USI03 on, USI03 to I2C, its pins to I2C\n");
	for (unsigned i = 0; i < sizeof(cmu_gates) / sizeof(cmu_gates[0]); i++) {
		wr(cmu, cmu_gates[i], rd(cmu, cmu_gates[i]) | CMU_GATE_MANUAL | CMU_GATE_ON);
	}
	wr(cmu, CMU_QCH_USI03, rd(cmu, CMU_QCH_USI03) | 3);
	wr(sysreg, USI03_SW_CONF, USI_MODE_I2C);
	{
		uint32_t con = rd(gpio, GPP1), want = PIN_I2C << (4 * SCL_PIN) | PIN_I2C << (4 * SDA_PIN);
		uint32_t mask = 0xfu << (4 * SCL_PIN) | 0xfu << (4 * SDA_PIN);

		if ((con & mask) != want) {
			wr(gpio, GPP1, (con & ~mask) | want);
		}
	}
	say("s6sy761: now: USI03 mode %x, gpp1 CON %x\n", rd(sysreg, USI03_SW_CONF), rd(gpio, GPP1));
	if (++step >= steps) {
		goto out;
	}

	say("s6sy761: step 3: the bus reset, at %u kHz from %u MHz\n", BUS_HZ / 1000, ipclk_mhz);
	bus_reset();
	say("s6sy761: CTL %x CONF %x FS1 %x FS2 %x FS3 %x TRANS_STATUS %x\n", rd(i2c, CTL), rd(i2c, CONF),
	    rd(i2c, TIMING_FS1), rd(i2c, TIMING_FS2), rd(i2c, TIMING_FS3), rd(i2c, TRANS_STATUS));
	if (++step >= steps) {
		goto out;
	}

	say("s6sy761: step 4: ask the chip at 0x%x\n", CHIP);
	error = chip_read(CMD_BOOT_STATUS, answer, 1);
	if (error != 0) {
		say("s6sy761: no answer (INT_STAT %x): the bus is not working, or the chip has no power (its supplies are "
		    "the PMIC's LDO35 and LDO43, which this port does not switch)\n", error);
		goto out;
	}
	say("s6sy761: boot status %x (%s)\n", answer[0], answer[0] == BOOT_APPLICATION ? "its firmware runs" :
	    answer[0] == 0x10 ? "IN ITS BOOT LOADER: no firmware running" : "unknown");
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
	for (unsigned tick = 1;; tick++) {
		for (int rounds = 0; rounds < 4 && irq_low(); rounds++) {
			read_events();
		}
		if (tick % MINUTE_TICKS == 0) {
			say("s6sy761: a minute: %llu touches, %llu events, %llu transfers, %llu failed (last %x)\n", stats.downs,
			    stats.events, stats.transfers, stats.errors, stats.last_error);
		}
		if (stats.errors >= 1000 && stats.errors * 2 > stats.transfers) {
			say("s6sy761: more than half of %llu transfers have failed: giving up\n", stats.transfers);
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

void
hakios_s6sy761_init(void)
{
	uint32_t enabled = 0, steps = 6;
	thread_t thread;

	if (!PE_parse_boot_argn("s6sy761", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	PE_parse_boot_argn("s6sy761_steps", &steps, sizeof(steps));
	PE_parse_boot_argn("s6sy761_ipclk", &ipclk_mhz, sizeof(ipclk_mhz));
	if (steps == 0 || ipclk_mhz < 26 || ipclk_mhz > 400) {
		return;
	}
	if (kernel_thread_start(touch_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
