/*
 * The Exynos 9810's always-on power processor (APM), asked through its ACPM firmware: the S2MPS18 PMIC's registers
 * (over SPEEDY, which the APM owns) and the clocks' DVFS rates. For the Galaxy S9's drivers (the GPU's supply and rate:
 * drivers/gpu/hakios.mali.c); sys/hakios.exynos_acpm.h says how to call it.
 *
 * The boot loader has started the APM with its firmware; nothing is to be done first (Linux's driver maps the two
 * regions, reads the firmware's table and masks the interrupts it polls, nothing more). The AP and the APM share:
 *   the mailbox   0x14100000: INTGR0 0x08 rings the APM (bit 16 + channel), INTCR1 0x20 clears and INTSR1 0x28 shows
 *                 its answer pending for us (bit = channel); INTMR1 0x24 masks that interrupt
 *   the SRAM      0x02039000, 0x30000 bytes: at +0x7F00 the firmware's "initdata" -- +0x08 where the channel table is,
 *                 +0x0C how many, +0x18 how many are the AP's, +0x60 two strings, its version and build time -- and
 *                 every place in it an offset from the SRAM's start
 *   a channel     0x48 bytes: +0x00 its id, +0x08 its owner, +0x0C its type (1: a queue), +0x10/+0x14/+0x18 where the
 *                 index the APM reads at, the index we write at and the queue of our requests are; +0x28/+0x2C/+0x30
 *                 the same for its answers (it writes at +0x2C's, we read at +0x28's); +0x34 the queue's length,
 *                 +0x38 its elements' size (16: four words), +0x44 whether the AP polls it
 * A request: four words written into our queue at our index, the index moved on, the doorbell rung; bits 21-16 of
 * its first word a number of ours (1 to 63) that its answer carries back, found between the APM's answer index and
 * ours, copied out, and our index moved on. 15 ms an attempt, six attempts, as Linux waits. Every access 32 bits.
 *   the PMIC (channel 2)   word 0 = type << 8 | register (type: 0 top, 1 PMIC, 2 RTC), word 1 = function (0 read,
 *                          1 write, 2 update) | value << 8 | mask << 16; the answer's word 1: the value [15:8], the
 *                          status [31:24] (0 good, 1 and 2 SPEEDY failed)
 *   DVFS (channel 0)       word 0 = the domain's index, word 1 = kHz, word 2 = 0 set, 1 get; a get's answer in word 1
 * Sources: the phone's device tree (acpm_ipc, the mailbox, the SRAM, s2mps18mfd's acpm-ipc-channel) and Linux's
 * drivers/soc/samsung/acpm (acpm_ipc.c, acpm_mfd.c, acpm_dvfs.c) and cal-if of the S9's kernel (GPL: read for these
 * facts, not copied).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/hakios.exynos_acpm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);

#define MBOX_PA         0x14100000UL
#define MBOX_INTGR0     0x08
#define MBOX_INTCR1     0x20
#define MBOX_INTMR1     0x24
#define MBOX_INTSR1     0x28
#define SRAM_PA         0x02039000UL
#define SRAM_SIZE       0x30000
#define INITDATA        0x7F00

#define ID_CHANNELS     0x08
#define ID_NCHANNELS    0x0C
#define ID_AP_MAX       0x18
#define ID_BUILD        0x60

#define CH_SIZE         0x48
#define CH_ID           0x00
#define CH_OWNER        0x08
#define CH_TYPE         0x0C
#define CH_IN_REAR      0x10    /* our requests: where the APM reads */
#define CH_IN_FRONT     0x14    /* ... where we write */
#define CH_IN_BASE      0x18
#define CH_OUT_REAR     0x28    /* its answers: where we read */
#define CH_OUT_FRONT    0x2C    /* ... where it writes */
#define CH_OUT_BASE     0x30
#define CH_QLEN         0x34
#define CH_ELEM         0x38
#define CH_AP_POLL      0x44

#define SEQ_SHIFT       16
#define SEQ_MASK        (0x3Fu << SEQ_SHIFT)
#define ATTEMPT_US      15000
#define ATTEMPTS        6
#define CHANNELS_MOST   16

static vm_offset_t mbox, sram;
static struct channel {
	bool usable;
	uint32_t in_rear, in_front, in_base, out_rear, out_front, out_base, len, elem;
	uint32_t seq;
} channels[CHANNELS_MOST];
static lck_mtx_t *acpm_lock;
static bool acpm_ready, acpm_tried;

/* a moment `us` microseconds from now, by the clock: a loop bounded by it spins, where delay() would sleep a tick */
static uint64_t
deadline_us(uint64_t us)
{
	uint64_t interval;

	nanoseconds_to_absolutetime(us * NSEC_PER_USEC, &interval);
	return mach_absolute_time() + interval;
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

static bool
printable(uint32_t off, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		uint8_t c = (uint8_t)(rd(sram, (off + i) & ~3u) >> (8 * ((off + i) & 3)));

		if (c == 0) {
			return i > 0;
		}
		if (c < 0x20 || c > 0x7e) {
			return false;
		}
	}
	return true;
}

static void
copy_string(uint32_t off, char *out, uint32_t len)
{
	uint32_t i;

	for (i = 0; i + 1 < len; i++) {
		char c = (char)(rd(sram, (off + i) & ~3u) >> (8 * ((off + i) & 3)));

		if (c == 0) {
			break;
		}
		out[i] = c;
	}
	out[i] = 0;
}

bool
hakios_acpm_init(int (*say)(const char *, ...))
{
	uint32_t table, count, ap_max;
	char version[26], built[26];

	if (acpm_tried) {
		return acpm_ready;
	}
	acpm_tried = true;
	acpm_lock = lck_mtx_alloc_init(lck_grp_alloc_init("hakios-acpm", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	mbox = ml_io_map(MBOX_PA, 0x1000);
	sram = ml_io_map(SRAM_PA, SRAM_SIZE);
	if (mbox == 0 || sram == 0) {
		say("acpm: cannot map the mailbox (%#lx) or the APM's SRAM (%#lx)\n", MBOX_PA, SRAM_PA);
		return false;
	}
	if (!printable(INITDATA + ID_BUILD, 25)) {
		say("acpm: no firmware's table at SRAM +%#x (its version string is not text): not used\n", INITDATA);
		return false;
	}
	copy_string(INITDATA + ID_BUILD, version, sizeof(version));
	copy_string(INITDATA + ID_BUILD + 25, built, sizeof(built));
	table = rd(sram, INITDATA + ID_CHANNELS);
	count = rd(sram, INITDATA + ID_NCHANNELS);
	ap_max = rd(sram, INITDATA + ID_AP_MAX);
	say("acpm: firmware \"%s\" built \"%s\"; %u channels at SRAM +%#x, %u the AP's; mailbox INTMR1 %#x, INTSR1 %#x\n",
	    version, built, count, table, ap_max, rd(mbox, MBOX_INTMR1), rd(mbox, MBOX_INTSR1));
	if (ap_max > count || count > 64 || table >= SRAM_SIZE || table + count * CH_SIZE > SRAM_SIZE) {
		say("acpm: the table does not fit in the SRAM: not used\n");
		return false;
	}
	for (uint32_t i = 0; i < ap_max; i++) {
		uint32_t at = table + i * CH_SIZE, id = rd(sram, at + CH_ID);
		struct channel *c;

		say("acpm: channel %u: owner %u, type %u, queue of %u x %u bytes, %s\n", id, rd(sram, at + CH_OWNER),
		    rd(sram, at + CH_TYPE), rd(sram, at + CH_QLEN), rd(sram, at + CH_ELEM),
		    rd(sram, at + CH_AP_POLL) ? "polled" : "by interrupt");
		if (id >= CHANNELS_MOST || rd(sram, at + CH_TYPE) != 1 || rd(sram, at + CH_ELEM) != 16 ||
		    rd(sram, at + CH_QLEN) == 0 || rd(sram, at + CH_QLEN) > 64) {
			continue;
		}
		c = &channels[id];
		c->in_rear = rd(sram, at + CH_IN_REAR);
		c->in_front = rd(sram, at + CH_IN_FRONT);
		c->in_base = rd(sram, at + CH_IN_BASE);
		c->out_rear = rd(sram, at + CH_OUT_REAR);
		c->out_front = rd(sram, at + CH_OUT_FRONT);
		c->out_base = rd(sram, at + CH_OUT_BASE);
		c->len = rd(sram, at + CH_QLEN);
		c->elem = rd(sram, at + CH_ELEM);
		if (c->in_rear >= SRAM_SIZE || c->in_front >= SRAM_SIZE || c->out_rear >= SRAM_SIZE ||
		    c->out_front >= SRAM_SIZE || c->in_base + c->len * c->elem > SRAM_SIZE ||
		    c->out_base + c->len * c->elem > SRAM_SIZE) {
			continue;
		}
		/* answers nobody read (a kernel before this one, or none): out of the way, our numbers start at 1 */
		wr(sram, c->out_rear, rd(sram, c->out_front));
		wr(mbox, MBOX_INTCR1, 1u << id);
		c->usable = true;
	}
	acpm_ready = channels[HAKIOS_ACPM_CH_PMIC].usable || channels[HAKIOS_ACPM_CH_DVFS].usable;
	say("acpm: the PMIC's channel %s, DVFS's %s\n", channels[HAKIOS_ACPM_CH_PMIC].usable ? "usable" : "NOT usable",
	    channels[HAKIOS_ACPM_CH_DVFS].usable ? "usable" : "NOT usable");
	return acpm_ready;
}

/* the answer with our number, if it has come: copied out, taken off the queue */
static bool
take_answer(struct channel *c, uint32_t id, uint32_t seq, uint32_t cmd[4])
{
	uint32_t rear = rd(sram, c->out_rear), front = rd(sram, c->out_front);

	if (rear >= c->len || front >= c->len) {
		return false;
	}
	for (uint32_t i = rear; i != front; i = (i + 1) % c->len) {
		vm_offset_t slot = sram + c->out_base + i * c->elem;

		if (((rd(slot, 0) & SEQ_MASK) >> SEQ_SHIFT) != seq) {
			continue;
		}
		for (int w = 0; w < 4; w++) {
			cmd[w] = rd(slot, 4u * w);
		}
		if (i != rear) {
			/* (an earlier answer not ours where ours was: kept, at the place ours leaves) */
			vm_offset_t first = sram + c->out_base + rear * c->elem;

			for (int w = 0; w < 4; w++) {
				wr(slot, 4u * w, rd(first, 4u * w));
			}
		}
		rear = (rear + 1) % c->len;
		wr(sram, c->out_rear, rear);
		if (rear == rd(sram, c->out_front)) {
			wr(mbox, MBOX_INTCR1, 1u << id);
		}
		return true;
	}
	return false;
}

int
hakios_acpm_request(uint32_t id, uint32_t cmd[4])
{
	struct channel *c;
	uint32_t front, next, seq;
	vm_offset_t slot;
	uint64_t deadline;
	int error = ETIMEDOUT;

	if (!acpm_ready || id >= CHANNELS_MOST || !channels[id].usable) {
		return ENXIO;
	}
	c = &channels[id];
	lck_mtx_lock(acpm_lock);
	front = rd(sram, c->in_front);
	if (front >= c->len) {
		lck_mtx_unlock(acpm_lock);
		return EIO;
	}
	next = (front + 1) % c->len;
	for (deadline = deadline_us(ATTEMPT_US); rd(sram, c->in_rear) == next && mach_absolute_time() < deadline;) {
		/* the queue is full: the APM has not read our last requests yet */
	}
	if (rd(sram, c->in_rear) == next) {
		lck_mtx_unlock(acpm_lock);
		return EBUSY;
	}
	c->seq = c->seq % 63 + 1;
	seq = c->seq;
	cmd[0] = (cmd[0] & ~SEQ_MASK) | seq << SEQ_SHIFT;
	slot = sram + c->in_base + front * c->elem;
	for (int w = 0; w < 4; w++) {
		wr(slot, 4u * w, cmd[w]);
	}
	__builtin_arm_dsb(0xf);     /* DSB SY */
	wr(sram, c->in_front, next);
	__builtin_arm_dsb(0xf);     /* DSB SY */
	wr(mbox, MBOX_INTGR0, 1u << (16 + id));
	for (deadline = deadline_us(ATTEMPTS * ATTEMPT_US); mach_absolute_time() < deadline;) {
		if (take_answer(c, id, seq, cmd)) {
			error = 0;
			break;
		}
	}
	lck_mtx_unlock(acpm_lock);
	return error;
}

int
hakios_acpm_pmic_read(uint32_t type, uint32_t reg, uint8_t *value)
{
	uint32_t cmd[4] = { (type & 0xf) << 8 | (reg & 0xff), 0, 0, 0 };
	int error = hakios_acpm_request(HAKIOS_ACPM_CH_PMIC, cmd);

	if (error == 0 && (cmd[1] >> 24) != 0) {
		error = EIO;
	}
	if (error == 0) {
		*value = (uint8_t)(cmd[1] >> 8);
	}
	return error;
}

int
hakios_acpm_pmic_update(uint32_t type, uint32_t reg, uint8_t value, uint8_t mask)
{
	uint32_t cmd[4] = { (type & 0xf) << 8 | (reg & 0xff), 2u | (uint32_t)value << 8 | (uint32_t)mask << 16, 0, 0 };
	int error = hakios_acpm_request(HAKIOS_ACPM_CH_PMIC, cmd);

	return error == 0 && (cmd[1] >> 24) != 0 ? EIO : error;
}

int
hakios_acpm_dvfs_get(uint32_t index, uint32_t *khz)
{
	uint32_t cmd[4] = { index, 0, 1, 0 };
	int error = hakios_acpm_request(HAKIOS_ACPM_CH_DVFS, cmd);

	if (error == 0) {
		*khz = cmd[1];
	}
	return error;
}

int
hakios_acpm_dvfs_set(uint32_t index, uint32_t khz)
{
	uint32_t cmd[4] = { index, khz, 0, 0 };

	return hakios_acpm_request(HAKIOS_ACPM_CH_DVFS, cmd);
}
