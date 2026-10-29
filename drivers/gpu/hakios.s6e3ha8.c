/*
 * The Galaxy S9's panel brightness (starlte, Exynos 9810): a Samsung S6E3HA8 AMOLED in MIPI command mode on DSIM0
 * (0x16080000), sent its brightness through the DSIM's command FIFO while the DECON keeps showing what the boot loader
 * set up. Only with boot-arg panel=1: every address below belongs to this phone.
 *
 * The panel has no DCS "set brightness" (0x51) in Samsung's tables. Its brightness is Samsung's own: a gamma table, the
 * AOR (the part of each frame the pixels are off) and ELVSS, behind a level-2 key and latched by F7 03. This driver
 * changes the AOR alone and leaves gamma as the boot loader left it:
 *   F0 5A 5A        the level-2 key on
 *   B1 hi lo        AOR, from 0x000C (brightest) to 0x0B76 (about 2 % of that)
 *   F7 03           take it
 *   F0 A5 A5        the key off
 * so the panel goes from the boot loader's level down. (The full range, 2 to 420 nits, needs the gamma that Android
 * works out from this panel's own measurements: Android's kernel prints it at boot, "gamma[lum]".) These registers are
 * the panel's working state: a restart clears them.
 *
 * A packet: its payload, four bytes a word, into PAYLOAD (0x5C), then its header into PKTHDR (0x58): the data type, and
 * the length (or the two bytes of a short one). 0x05 a DCS command without a parameter, 0x39 one with (Samsung's tables
 * send every longer one so). Before a header, the header FIFO must have room (FIFOCTRL 0x68, bit 11 full; bit 9 the
 * payload's); after it, its being sent shows as INTSRC (0x50) bit 28, written back to clear. The DSIM keeps command
 * packets out of the window the panel is reading the frame in (its TE protection), so the picture is not disturbed;
 * Android sends brightness at any moment, behind a lock.
 * Sources: the phone's device tree (dsim@0x16080000, the s6e3ha8 nodes), Linux's drivers/video/fbdev/exynos/dpu_9810
 * (dsim_drv.c, dsim_reg.c, regs-dsim.h) and panel/s6e3ha8 of the S9's kernel (GPL: read for these facts, not copied).
 *
 * kern.hakios.brightness: 0-255, written to set it (read: what it was set to, -1 never). Boot-arg panel_brightness=N
 * sets it once at start.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);
extern void loader_log_putc(char c);

SYSCTL_DECL(_kern_hakios);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

#define DSIM_PA         0x16080000UL
#define DSIM_INTSRC     0x50
#define  SFR_PH_EMPTY   (1u << 28)
#define DSIM_PKTHDR     0x58
#define DSIM_PAYLOAD    0x5C
#define DSIM_FIFOCTRL   0x68
#define  FULL_PL        (1u << 9)
#define  FULL_PH        (1u << 11)
#define  EMPTY_PH       (1u << 10)      /* (the header FIFO empty: Samsung takes that for "sent" as well) */

#define DCS_SHORT       0x05
#define DCS_LONG        0x39

#define AOR_BRIGHTEST   0x000C
#define AOR_DIMMEST     0x0B76

static vm_offset_t dsim;
static lck_mtx_t *panel_lock;
static int panel_level = -1;
static uint32_t panel_failures;

/* a moment `us` microseconds from now, by the clock: a loop bounded by it spins, where delay() would sleep a tick */
static uint64_t
deadline_us(uint64_t us)
{
	uint64_t interval;

	nanoseconds_to_absolutetime(us * NSEC_PER_USEC, &interval);
	return mach_absolute_time() + interval;
}

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(dsim + off);
}

static inline void
wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(dsim + off) = v;
}

/* one packet: true once the DSIM says its header has gone out */
static bool
dsim_send(const uint8_t *data, uint32_t len)
{
	uint64_t deadline = deadline_us(50000);

	while ((rd(DSIM_FIFOCTRL) & (FULL_PH | FULL_PL)) && mach_absolute_time() < deadline) {
	}
	if (rd(DSIM_FIFOCTRL) & (FULL_PH | FULL_PL)) {
		return false;
	}
	wr(DSIM_INTSRC, SFR_PH_EMPTY);
	if (len == 1) {
		wr(DSIM_PKTHDR, DCS_SHORT | (uint32_t)data[0] << 8);
	} else {
		for (uint32_t i = 0; i < len; i += 4) {
			uint32_t word = 0;

			for (uint32_t j = 0; j < 4 && i + j < len; j++) {
				word |= (uint32_t)data[i + j] << (8 * j);
			}
			wr(DSIM_PAYLOAD, word);
		}
		wr(DSIM_PKTHDR, DCS_LONG | (len & 0xff) << 8 | (len >> 8) << 16);
	}
	for (deadline = deadline_us(50000); !(rd(DSIM_INTSRC) & SFR_PH_EMPTY) && !(rd(DSIM_FIFOCTRL) & EMPTY_PH) &&
	    mach_absolute_time() < deadline;) {
	}
	bool sent = (rd(DSIM_INTSRC) & SFR_PH_EMPTY) || (rd(DSIM_FIFOCTRL) & EMPTY_PH);

	wr(DSIM_INTSRC, SFR_PH_EMPTY);
	return sent;
}

/* brightness 0-255 as the AOR: the light is the frame's lit part, made to follow the level's square (a dim end that is usable) */
static int
panel_set(int level)
{
	static const uint8_t key_on[] = { 0xF0, 0x5A, 0x5A }, take[] = { 0xF7, 0x03 }, key_off[] = { 0xF0, 0xA5, 0xA5 };
	uint32_t lit = (uint32_t)(level * level), aor = AOR_DIMMEST - (AOR_DIMMEST - AOR_BRIGHTEST) * lit / (255 * 255);
	uint8_t aor_packet[] = { 0xB1, (uint8_t)(aor >> 8), (uint8_t)aor };
	bool ok;

	if (dsim == 0 || level < 0 || level > 255) {
		return EINVAL;
	}
	lck_mtx_lock(panel_lock);
	ok = dsim_send(key_on, sizeof(key_on)) && dsim_send(aor_packet, sizeof(aor_packet)) && dsim_send(take, sizeof(take));
	ok = dsim_send(key_off, sizeof(key_off)) && ok;         /* (the key off whatever happened) */
	if (ok) {
		panel_level = level;
	} else {
		panel_failures++;
	}
	lck_mtx_unlock(panel_lock);
	if (!ok && panel_failures <= 3) {
		say("s6e3ha8: brightness %d (AOR 0x%x) not sent: the DSIM's FIFO did not take it (FIFOCTRL %x, INTSRC %x)\n",
		    level, aor, rd(DSIM_FIFOCTRL), rd(DSIM_INTSRC));
	}
	return ok ? 0 : EIO;
}

static int
panel_sysctl_brightness(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = panel_level, error;

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	return panel_set(value);
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    panel_sysctl_brightness, "I", "the panel's brightness, 0-255 (-1: never set); writing sets it (Galaxy S9, panel=1)");

/* for the framebuffer, when iOS asks (MiniIOMobileFramebuffer.cpp): 0-255; 0 if the panel took it */
int
hakios_panel_brightness(int level)
{
	return panel_set(level);
}

void
hakios_s6e3ha8_init(void)
{
	uint32_t enabled = 0, level = 0;

	if (!PE_parse_boot_argn("panel", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	panel_lock = lck_mtx_alloc_init(lck_grp_alloc_init("s6e3ha8", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	dsim = ml_io_map(DSIM_PA, 0x1000);
	if (dsim == 0) {
		say("s6e3ha8: cannot map the DSIM at 0x%lx\n", DSIM_PA);
		return;
	}
	say("s6e3ha8: DSIM0: FIFOCTRL %x, INTSRC %x; kern.hakios.brightness sets the panel's brightness\n",
	    rd(DSIM_FIFOCTRL), rd(DSIM_INTSRC));
	if (PE_parse_boot_argn("panel_brightness", &level, sizeof(level))) {
		int error = panel_set((int)level);

		say("s6e3ha8: brightness %u: %s\n", level, error == 0 ? "sent" : "NOT SENT");
	}
}
