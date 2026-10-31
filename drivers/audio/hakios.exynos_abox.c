/*
 * The Galaxy S9's speakers (starlte, Exynos 9810), the SoC's side: ABOX, Samsung's audio block, driven from the AP with
 * its own CPU off ("bypass", no firmware). What iOS plays goes memory -> RDMA0 -> SPUS (ABOX's stream bus, chain 0, no
 * ASRC) -> SIFS0 (the mixer) -> UAIF1, an I2S master, -> the two MAX98512 amplifiers. The amplifiers' I2C is another
 * driver's, reached only through the hook of sys/hakios.exynos_abox.h, which also has UAIF1's format and every register
 * fact used here (FACT or INFERRED, with the file it comes from). Only with boot-arg abox=N on this phone (the SoC's
 * registers at 0x10000000); every address below is the S9's.
 *
 * THE STEPS. abox=N makes the first N of eight and stops; 0 or none leaves ABOX alone. Each step says what it will touch
 * before it does, and every write to a register of the SoC is said with what it reads back. A step that does not find
 * what it needs ends the sequence; with N under 8 everything is put back at the end (the stream stopped, the pins idle,
 * the domain off again if this driver switched it on).
 *   1  look     the PMU's AUD domain, its pads' retention, ABOX's CPU power; ACPM's AUD DVFS rate. Nothing written, and
 *               nothing of the AUD block read (it may have no power)
 *   2  power    the AUD domain on (PMU 0x14064000 [3:0] = 0xF, then 0x14064004 [3:0] == 0xF) unless it is; the DTZPC
 *               restore, SMC 0x82000410 (1, 0x17C10204); ABOX's IP_INDEX must read "ABOX"; ABOX's CPU kept off (bypass)
 *   3  clocks   SYSREG MISC_CON = 1; CMU_AUD's dividers (ACLK 589.824 MHz, its APB 196.608, AUDIF 73.728, UAIF1's BCLK
 *               1.536) and UAIF1's mux, then PLL_AUD at 1179.648 MHz (M 45 P 1 S 0 K 24319), stable; the Q-channels of
 *               ABOX's "cpu" clock (abox_enable() turns it on before any firmware is there), UAIF1's BCLK and the
 *               SysMMU asked on if the hardware does not gate them itself
 *   4  pins     gpb1-0..3 to UAIF1 (function 2, pull 1), then the pads' retention released (PMU 0x4170 bit 28, 0x1424 bit 0)
 *   5  UAIF1    master, I2S, 2 x 16 bits (CTRL0 0x01000014, CTRL1 0x0984F000); then on for 50 ms with nothing routed to
 *               it: its status, and whether BCLK and LRCLK toggle at the pins (gpb1's DAT sampled)
 *   6  route    SPUS chain 0: RDMA0 in, no ASRC, out to SIFS0 (SPUS_CTRL0 [3:0] = 2); the mixer's format 16-bit stereo
 *               (SPUS_CTRL1 [4:0] = 9); UAIF1 from SIFS0 (ROUTE_CTRL0 [7:4] = 1); the mixer flushed (SPUS_CTRL2 bit 0);
 *               SPUS's sample banks read, and RDMA0's given 512 bytes if it has none (SBANK_RDMA0 SIZE, 0x17C50220)
 *   7  stream   ABOX's SysMMU checked to pass addresses through; a ring of 8 periods of 512 frames below 4 GB; RDMA0
 *               programmed (its burst length 8 if it reads 0); UAIF1 on, then RDMA0 on (Samsung's order); the amplifiers started; a tone (1 kHz, -20 dBFS,
 *               both channels) for abox_secs, through the speakers' guard; RDMA0's status every millisecond at first
 *               and every second after; then the amplifiers stopped, RDMA0 off, UAIF1 off
 *   8  sink     the same stream, kept running for good: /dev/hakios-audio's sink (sys/hakios.audio.h) into a ring, a
 *               thread of ours from there through the guard into each period RDMA0 has finished (silence when there is
 *               nothing), at the volume iOS sets (its "below", taken off digitally before the guard, as the POCO's
 *               amplifiers take it off in their own volume); the amplifiers started when iOS plays and stopped after
 *               3 s of silence or while it is muted
 *
 * Boot-args:
 *   abox=N             the first N of ABOX's eight steps (1 only looks, 7 plays a tone, 8 serves iOS); 0 or none: left alone
 *   abox_secs=S        how long step 7 plays its tone, 1 to 10 seconds (3)
 *   abox_route=1       UAIF1 from SIFS1 (SPUS OUT0 straight, SPUS_CTRL1 [18:16] = 0, ROUTE_CTRL0 [7:4] = 2) instead of the mixer
 *   abox_rdma_ctrl0=V  bits of RDMA0's CTRL0 besides ENABLE, to try a format there (none: as it reads after reset)
 *   abox_rdma_ctrl1=V  RDMA0's CTRL1 (none: as it reads after reset)
 *   abox_vol=V         RDMA0's VOL_FACTOR [23:0] (none: 0x800000 if it reads 0, else left)
 *   abox_bclk_on=1     ABOX never gates UAIF1's BCLK (QCHANNEL_DISABLE bit 3), as Samsung does for UAIF0 while it plays
 *   abox_gain=N        eighths of a dB of gain before the speakers' guard, 0 to 96 (0)
 *   abox_dvfs=KHZ      ACPM's AUD DVFS domain (index 9) set to this before step 3 (none: left as found, only read)
 *   abox_sbank=BYTES   RDMA0's sample bank in SPUS (step 6), a multiple of 16 up to 16368 (512, set only when it reads 0;
 *                      given, it is set whatever it reads; 0: left as read)
 *   abox_burst=N       RDMA0's CTRL0 BURST_LEN [22:19], 1 to 15 (8, set only when it reads 0; given, set anyway; 0: left)
 *
 * WHERE IT COMES FROM. The S9 kernel (github.com/ExyHyperBrick/android_kernel_samsung_exynos9810, lineage-24.0), GPL-2.0,
 * read for facts and not copied; which part of this file does the work of what there:
 *   step 2   cal-if/exynos9810/flexpmu_cal_local_exynos9810.h (aud_on), cal-if/pmucal_local.c pmucal_local_enable(),
 *            exynos-el3_mon.c exynos_pd_tz_restore(), linux/smc.h; the device tree's pd-aud (cal_id 0xB1380000: index 0,
 *            no PMIC write in cal-if.c cal_pd_control(), unlike the G3D's); abox.c abox_cpu_enable()/abox_cpu_power()
 *   step 3   abox.c abox_enable() (MISC_CON, the clocks) and abox_register_bclk_usage() (PLL 48 kHz family, AUDIF for
 *            uaif_max_div 512); abox_if.c abox_if_startup() and abox_uaif_hw_params() (BCLK = rate x channels x width);
 *            cal-if/ra.c ra_set_pll()/ra_pll_set_pmsk()/ra_set_div_rate()/ra_req_enable_qch(), pll_spec.c, and
 *            exynos9810/cmucal-*.c (the registers, the PLL table, BLK_AUD's dividers); clk-exynos9810.c (which clock
 *            each of the device tree's "pll", "cpu", "audif", "bus", "bclk", "bclk_gate" is)
 *   step 4   the device tree's aud-i2s1-bus/-idle; pinctrl-exynos.c (exynos9810 bank layout); abox.c abox_pad_retention()
 *   step 5   abox_if.c abox_uaif_set_fmt(), abox_uaif_hw_params(), abox_uaif_trigger(), abox_uaif_shutdown();
 *            exynos9810_madera.c (UAIF1's dai_fmt); abox.c probe (the SIFs' 48000/S16/2 defaults)
 *   step 6   abox.c: the DAPM muxes (spus_in/asrc/out, uaif1_spk, sifs1) and abox_flush_mixp(), abox_sif_format_put_ipc()
 *            (SPUS_CTRL1's MIXP_FORMAT, written by the AP), abox_try_to_asrc_off() (ASRC off when the rates and widths match)
 *   step 7   abox_rdma.c (hw_params, prepare, trigger, pointer, hw_free and its 40 ms disable barrier, the UAIF FIFO's
 *            0.6-1.2 ms); soc-pcm.c and the RDMA links' SND_SOC_DPCM_TRIGGER_POST_PRE (start: UAIF first, stop: RDMA
 *            first); drivers/iommu/exynos-iommu*.h (the SysMMU's CTRL values); include/sound/samsung/abox_ipc.h
 *   steps 6-7, the sample bank and the burst length: the Galaxy S10's kernel (github.com/ivanmeler/
 *            android_kernel_samsung_beyondlte, GPL-2.0, read for facts): abox_soc.h, abox_cmpnt_v20.c
 *            abox_cmpnt_adjust_sbank(), abox_rdma.c hw_params, Kconfig (ABOX version 0x10100 for the 9810)
 *
 * WITH THE FIRMWARE OFF, WHAT ANDROID DOES. Nothing that plays. abox_enable() switches the clocks on, puts back the
 * registers the AP owns (regcache_sync of the regmap: SYSTEM_CONFIG0, REMAP, DYN_CLOCK_OFF, QCHANNEL_DISABLE,
 * ROUTE_CTRL0-2, SPUS_CTRL0-3, the SIFS counts, SPUM_CTRL0-3, UAIFn CTRL0/1, DSIF, RDMA3's VOL_FACTOR) and sets the pins,
 * then abox_download_firmware() returns -EAGAIN while calliope_sram.bin/calliope_dram.bin are not loaded, and
 * abox_enable() returns before abox_cpu_power()/abox_cpu_enable(): calliope_state stays CALLIOPE_DISABLED. Every message
 * a stream sends (PCM_SET_BUFFER, PCM_PLTDAI_HW_PARAMS, _PREPARE, _TRIGGER, the mixer's format) then goes through
 * abox_process_ipc(), where abox_can_calliope_ipc() says "Invalid calliope state" and drops it. abox_uaif_set_fmt,
 * _hw_params and _trigger still write UAIF1 directly (snd_soc_component_update_bits on the regmap), and the DAPM muxes
 * still write SPUS_CTRL0/1 and ROUTE_CTRL0, so UAIF1 would clock out whatever SIFS0 holds; but nothing ever writes
 * RDMA0's BUF_*, STR_POINT or CTRL0, and abox_rdma_pointer() reads STATUS with PROGRESS 0 and says 0: the stream stalls.
 * So Android never runs RDMA without calliope, and its source cannot say whether RDMA0 runs that way; what it does say:
 *   - RDMA0's registers are plain registers on the AP's bus: the AP reads STATUS (abox_rdma_pointer) and CTRL0
 *     (abox_rdma_disable_barrier) directly, and writes RDMA3's VOL_FACTOR (0x1318) through the same regmap
 *   - what RDMA0 needs that calliope gives it: its BUF_*, STR_POINT and CTRL0/1, SPUS's sample bank (reset 0), the
 *     buffer as an IOVA (0x91000000, SysMMU on) and ABOX's GIC for its interrupts. Bypass gives the first two from the
 *     AP, a physical address with the SysMMU off for the third, and polls instead of the fourth
 *   - calliope's other duties for a plain stream (the mixer's rate, the ASRC, the DSM speaker protection) are either
 *     off here (the ASRC: try-to-asrc-off holds at 48 kHz 16-bit) or replaced (the guard for DSM)
 *
 * WHAT IS INFERRED, NOT READ. Samsung's ABOX firmware programs RDMA0; the kernel only names its registers.
 *   - RDMA0: BUF_STR = the ring's first byte, BUF_END = one past its last, BUF_OFFSET = a period's bytes, STR_POINT =
 *     BUF_STR, VOL_FACTOR 0x800000 as unity (Q1.23; any reading of 0x800000 is at most unity). Why: the registers' names,
 *     and that PCM_SET_BUFFER gives the firmware exactly an address, a period's bytes and a count; STATUS's period
 *     offset plus count in the period (abox_rdma_pointer) says the hardware keeps periods itself. The format (16-bit,
 *     stereo) is in no named field: CTRL0's other bits and CTRL1 are left as they read after reset unless abox_rdma_ctrl0/1
 *     say otherwise, and the bytes RDMA0 reads per second are measured and said (192000 is 48 kHz 16-bit stereo; 96000
 *     would say mono or 8-bit, 384000 32-bit or 4 channels). The ring sits at the start of 256 KB of zeroes (all that
 *     STATUS's 13-bit offset can point into, twice), so a different meaning of BUF_END reads silence, not memory of
 *     someone else's; a position outside the ring stops the stream.
 *   - The SIFS0 mixer's rate is told to the firmware (SET_MIXER_SAMPLE_RATE) and no register holds it: taken to follow
 *     the UAIF that pulls from it (48 kHz). RDMA0, the mixer and UAIF1 are all 48 kHz 16-bit stereo, so Samsung's own
 *     rule (abox_try_to_asrc_off) leaves the ASRC off.
 *   - The SysMMU passes addresses through when CTRL's bit 2 (on in Linux's CTRL_ENABLE 0x5 and CTRL_BLOCK 0x7, off in
 *     CTRL_DISABLE 0 and CTRL_BLOCK_DISABLE 3) is clear: checked before RDMA0 is given an address, and refused when set;
 *     when only bit 1 (blocked) is set, CTRL_DISABLE (0) is written. The ring is below 4 GB (RDMA's address registers are
 *     32 bits) and its physical address is what RDMA0 is given.
 *   - The DMA does not snoop the processors' caches: every period is cleaned to memory after it is written.
 *   - The DTZPC restore returns 0 when it succeeds (as hakios.mali.c assumes for the G3D's).
 * What a firmware mode would settle: run calliope once (abox_fw=1, a mode to add) and read RDMA0's registers back after
 * its PCM_PLTDAI_TRIGGER: CTRL0/CTRL1's format bits, what BUF_END and BUF_OFFSET hold, VOL_FACTOR's unity. Note that the
 * firmware sees memory through the SysMMU at IOVAs (its DRAM image at 0x80000000, RDMA buffers at 0x91000000 + 1 MB x n,
 * abox.h): that mode needs ABOX's SysMMU given page tables, where bypass needs it off. Steps 1-6 are the firmware mode's
 * too (abox_enable() does the same power, clocks, pins and UAIF/route writes around booting the CPU), and step 7 calls
 * RDMA0 only through `struct abox_dma` below, which that mode would give through IPC instead.
 *
 * WHAT IS NOT DONE. No PMIC is written (the AUD domain's power-on has no PMIC step in the CAL); ACPM is only read unless
 * abox_dvfs= asks. ABOX's GIC, its CPU, its SRAM and the WDMAs are not touched. No interrupt is used: RDMA0's status is
 * polled from a thread of ours every 2 ms. The amplifiers are not touched except through their driver's hook.
 *
 * THE SPEAKERS' GUARD (drivers/audio/hakios.guard.c) applies: on Android the S9's speakers are protected by Maxim's DSM
 * running on ABOX's CPU (the device tree's dsm.bin), which does not run here. Every period, silence too, goes through
 * it, reckoned with the amplifiers at Android's loudest (attenuation 0, the most careful), after abox_gain.
 *
 * Status: written; compiled; not yet run on the phone.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/errno.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.exynos_abox.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern uint64_t kvtophys(vm_offset_t va);                               /* pmap_paddr_t */
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);       /* osfmk/arm64/caches_asm.s */
extern kern_return_t thread_terminate(thread_t thread);
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

#define STARLTE_SOC_BASE        0x10000000UL
#define STEPS                   8
#define ACPM_DVFS_AUD           9u              /* ACPM_DVFS_AUD 0x0B040009 (dt-bindings/clock/exynos9810.h): its index */

/* the ring RDMA0 reads: 8 periods of 512 frames (10.7 ms each), at the start of a zeroed area */
#define FRAME_BYTES             4u
#define PERIOD_FRAMES           512u
#define PERIOD_BYTES            (PERIOD_FRAMES * FRAME_BYTES)  /* 2048: a power of two, a multiple of STATUS's 16 bytes */
#define PERIODS                 8u
#define RING_BYTES              (PERIODS * PERIOD_BYTES)
#define AREA_BYTES              (256u * 1024u)
#define BYTES_PER_SECOND        (HAKIOS_ABOX_RATE * FRAME_BYTES)
#define DMA_STOP_US             40000u          /* abox.h ABOX_DMA_TIMEOUT_NS */
#define FIFO_FLUSH_US           1000u           /* abox_rdma_hw_free(): 600-1200 us between RDMA and UAIF stopping */
#define POLL_MS                 2u

/* /dev/hakios-audio's frames on their way to the feeder (step 8) */
#define SINK_FRAMES             16384u          /* 0.34 s; a power of two, the indices run on and wrap */
#define SILENCE                 8u              /* samples no larger than this are silence (as hakios.audio.c's) */
#define QUIET_STOP_US           3000000ull      /* the amplifiers stop after this much silence */

static const char *const step_names[STEPS + 1] = {
	"", "look", "power", "clocks", "pins", "UAIF1", "route", "stream", "sink",
};

static uint32_t steps, secs = 3, route_sifs1, bclk_on, gain, dvfs_khz;
static uint32_t sbank_bytes = 512, burst = 8;   /* Samsung's S10 values (see the header); set when the field reads 0 */
static bool sbank_given, burst_given;           /* given on the command line: set whatever the field reads */
static uint32_t rdma_ctrl0 = UINT32_MAX, rdma_ctrl1 = UINT32_MAX, vol = UINT32_MAX;
static uint32_t step;                   /* the step being made, for what is said */

/* ---- the blocks ---------------------------------------------------------------------------------------------------- */

struct block {
	const char *name;
	uint32_t pa, size;
	vm_offset_t va;
};

static struct block pmu = { "PMU", ABOX_PMU_PA, 0x5000, 0 };
static struct block cmu = { "CMU_AUD", ABOX_CMU_PA, 0x8000, 0 };
static struct block sysreg = { "SYSREG_AUD", ABOX_SYSREG_PA, 0x1000, 0 };
static struct block abox = { "ABOX", ABOX_SFR_PA, 0x2000, 0 };
static struct block gpio = { "GPIO_AUD", ABOX_GPIO_PA, 0x1000, 0 };
static struct block mmu = { "SYSMMU_ABOX", ABOX_SYSMMU_PA, 0x1000, 0 };

static inline uint32_t
rd(struct block *b, uint32_t off)
{
	return *(volatile uint32_t *)(b->va + off);
}

static inline void
wr(struct block *b, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(b->va + off) = v;
	__builtin_arm_dsb(0xf);
}

/* a register read, said */
static uint32_t
peek(struct block *b, const char *what, uint32_t off)
{
	uint32_t v = rd(b, off);

	say("abox: %u: %s %s (%#x) = %#x\n", step, b->name, what, b->pa + off, v);
	return v;
}

/* a register written and read back, both said (a difference in the bits of `mask` called out); the reading is returned */
static uint32_t
poke_checked(struct block *b, const char *what, uint32_t off, uint32_t value, uint32_t mask)
{
	uint32_t was = rd(b, off), back;

	wr(b, off, value);
	back = rd(b, off);
	say("abox: %u: %s %s (%#x): %#x -> wrote %#x, reads %#x%s\n", step, b->name, what, b->pa + off, was, value, back,
	    (back & mask) == (value & mask) ? "" : " (NOT what was written)");
	return back;
}

static uint32_t
poke(struct block *b, const char *what, uint32_t off, uint32_t value)
{
	return poke_checked(b, what, off, value, 0xFFFFFFFFu);
}

/* the bits of `mask` set to `value`, the rest kept (and only those bits checked: others may be the hardware's) */
static uint32_t
poke_bits(struct block *b, const char *what, uint32_t off, uint32_t mask, uint32_t value)
{
	return poke_checked(b, what, off, (rd(b, off) & ~mask) | (value & mask), mask);
}

/* a moment `us` microseconds from now, by the clock: a loop bounded by it spins, where delay() would sleep a tick */
static uint64_t
deadline_us(uint64_t us)
{
	uint64_t interval;

	nanoseconds_to_absolutetime(us * NSEC_PER_USEC, &interval);
	return mach_absolute_time() + interval;
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

static void
spin_us(uint64_t us)
{
	for (uint64_t deadline = deadline_us(us); mach_absolute_time() < deadline;) {
	}
}

/* until (register & mask) == want, at most `us`: true if it came; *took the microseconds it took */
static bool
wait_bits(struct block *b, uint32_t off, uint32_t mask, uint32_t want, uint64_t us, uint64_t *took)
{
	uint64_t start = now_us(), deadline = deadline_us(us);
	bool came;

	while ((rd(b, off) & mask) != want && mach_absolute_time() < deadline) {
	}
	came = (rd(b, off) & mask) == want;
	*took = now_us() - start;
	return came;
}

static bool
map_blocks(void)
{
	struct block *all[] = { &pmu, &cmu, &sysreg, &abox, &gpio, &mmu };

	for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		all[i]->va = ml_io_map(all[i]->pa, all[i]->size);
		if (all[i]->va == 0) {
			say("abox: cannot map %s at %#x\n", all[i]->name, all[i]->pa);
			return false;
		}
	}
	return true;
}

/* one SMC to EL3 (through Carbon at EL2): x0 back */
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

/* ---- what has been done, to be put back ----------------------------------------------------------------------------- */

static bool on_at_boot, powered_here, pins_set, uaif_on, rdma_on;
static vm_offset_t area;                /* the ring and its zeroes */
static uint64_t area_pa;
static bool area_kept;                  /* RDMA0 did not say it stopped: the memory is never given back */

/* ---- the amplifiers' hook ------------------------------------------------------------------------------------------- */

static const struct hakios_abox_amp *volatile amp_hook;
static const struct hakios_abox_amp *amp_running;      /* the one started, which is the one stopped */
static bool amp_given_up, amp_none_said;

void
hakios_abox_set_amp(const struct hakios_abox_amp *amp)
{
	amp_hook = amp;
	say("abox: the amplifiers' driver: %s\n", amp != NULL && amp->name != NULL ? amp->name : "none");
}

static void
amps_start(void)
{
	const struct hakios_abox_amp *amp = amp_hook;
	int error;

	if (amp_running != NULL || amp_given_up) {
		return;
	}
	if (amp == NULL || amp->start == NULL) {
		/* (looked for again the next time: a driver may register later) */
		if (!amp_none_said) {
			say("abox: %u: no amplifiers' driver registered (hakios_abox_set_amp): UAIF1 runs and nothing is heard\n", step);
			amp_none_said = true;
		}
		return;
	}
	say("abox: %u: %s: start(%u, %u, %u), BCLK and LRCLK running\n", step, amp->name, HAKIOS_ABOX_RATE,
	    HAKIOS_ABOX_SLOT_BITS, HAKIOS_ABOX_CHANNELS);
	error = amp->start(HAKIOS_ABOX_RATE, HAKIOS_ABOX_SLOT_BITS, HAKIOS_ABOX_CHANNELS);
	if (error != 0) {
		say("abox: %u: %s did not start (%d): not asked again in this run\n", step, amp->name, error);
		amp_given_up = true;
		return;
	}
	amp_running = amp;
}

static void
amps_stop(void)
{
	if (amp_running != NULL) {
		say("abox: %u: %s: stop(), before UAIF1's clocks stop\n", step, amp_running->name);
		if (amp_running->stop != NULL) {
			amp_running->stop();
		}
		amp_running = NULL;
	}
}

/* ---- step 1: look -------------------------------------------------------------------------------------------------- */

static bool
step_look(void)
{
	uint32_t status, khz = 0;
	int error;

	say("abox: 1: reading the PMU's AUD domain, its pads, ABOX's CPU (nothing of the AUD block itself)\n");
	(void)peek(&pmu, "AUD_CONFIGURATION", PMU_AUD_CONFIGURATION);
	status = peek(&pmu, "AUD_STATUS", PMU_AUD_STATUS);
	(void)peek(&pmu, "GPIO_MODE_ABOX_SYS_PWR_REG", PMU_GPIO_MODE_ABOX);
	(void)peek(&pmu, "PAD_RETENTION_ABOX_OPTION", PMU_PAD_RETENTION_ABOX);
	(void)peek(&pmu, "ABOX_CPU_CONFIGURATION", PMU_ABOX_CPU_CONFIG);
	(void)peek(&pmu, "ABOX_CPU_STATUS", PMU_ABOX_CPU_STATUS);
	(void)peek(&pmu, "ABOX_CPU_OPTION", PMU_ABOX_CPU_OPTION);
	(void)peek(&pmu, "ABOX_CPU_STANDBY", PMU_ABOX_CPU_STANDBY);
	on_at_boot = (status & 0xF) == 0xF;
	say("abox: 1: the AUD domain is %s\n", on_at_boot ? "ON already (the boot left it so)" : (status & 0xF) == 0 ? "off" :
	    "neither on nor off");
	if (hakios_acpm_init(say)) {
		error = hakios_acpm_dvfs_get(ACPM_DVFS_AUD, &khz);
		say("abox: 1: ACPM's AUD DVFS domain (%u): %s %u kHz\n", ACPM_DVFS_AUD, error == 0 ? "at" : "not read, error", error == 0 ?
		    khz : (uint32_t)error);
	} else {
		say("abox: 1: no ACPM: the AUD DVFS domain's rate is not known\n");
	}
	return true;
}

/* ---- step 2: power ------------------------------------------------------------------------------------------------- */

static bool
step_power(void)
{
	uint64_t took, answer;
	uint32_t id;

	if (!on_at_boot) {
		say("abox: 2: about to switch the AUD domain on (PMU AUD_CONFIGURATION [3:0] = 0xF)\n");
		(void)poke_bits(&pmu, "AUD_CONFIGURATION", PMU_AUD_CONFIGURATION, 0xF, 0xF);
		powered_here = true;
		if (!wait_bits(&pmu, PMU_AUD_STATUS, 0xF, 0xF, 10000, &took)) {
			say("abox: 2: AUD_STATUS reads %#x after %llu us: the domain did not come up\n", rd(&pmu, PMU_AUD_STATUS), took);
			return false;
		}
		say("abox: 2: the AUD domain is on after %llu us\n", took);
	}
	say("abox: 2: about to make SMC %#x (1, %#x): the DTZPC restore Samsung's CAL makes once the AUD domain has power; "
	    "Carbon, at EL2, passes it on to EL3\n", ABOX_SMC_PD_ONOFF, ABOX_DTZPC_ARG);
	answer = smc(ABOX_SMC_PD_ONOFF, 1, ABOX_DTZPC_ARG, 0);
	say("abox: 2: SMC %#x returned %#llx\n", ABOX_SMC_PD_ONOFF, answer);
	if ((uint32_t)answer != 0) {
		if (!powered_here) {
			say("abox: 2: not 0, but the domain was on before us: what the boot set up is taken to stand\n");
		} else {
			say("abox: 2: not 0 (%s): the block's registers may not answer the AP; stopping here\n",
			    (uint32_t)answer == 0xffffffffu ? "SMCCC's NOT_SUPPORTED" : "taken as a refusal");
			return false;
		}
	}
	say("abox: 2: reading ABOX's IP_INDEX at %#x: should the run end here, the block does not answer the AP\n",
	    ABOX_SFR_PA + ABOX_IP_INDEX);
	id = peek(&abox, "IP_INDEX", ABOX_IP_INDEX);
	if (id != ABOX_IP_INDEX_VALUE) {
		say("abox: 2: IP_INDEX is not \"ABOX\" (%#x): stopping here\n", ABOX_IP_INDEX_VALUE);
		return false;
	}
	(void)peek(&abox, "VERSION", ABOX_VERSION);
	(void)peek(&abox, "SYSPOWER_CTRL", ABOX_SYSPOWER_CTRL);
	(void)peek(&abox, "SYSPOWER_STATUS", ABOX_SYSPOWER_STATUS);
	(void)peek(&abox, "SYSTEM_CONFIG0", ABOX_SYSTEM_CONFIG0);
	(void)peek(&abox, "REMAP_ADDR", ABOX_REMAP_ADDR);
	(void)peek(&abox, "DYN_CLOCK_OFF", ABOX_DYN_CLOCK_OFF);
	(void)peek(&abox, "QCHANNEL_DISABLE", ABOX_QCHANNEL_DISABLE);
	/* bypass: ABOX's CPU stays off, released last of all in abox_enable() and never here */
	if ((rd(&pmu, PMU_ABOX_CPU_OPTION) & PMU_ABOX_CPU_ENABLE) || (rd(&pmu, PMU_ABOX_CPU_CONFIG) & 1)) {
		say("abox: 2: ABOX's CPU is on: off again (abox.c's order: released first, then its power)\n");
		(void)poke_bits(&pmu, "ABOX_CPU_OPTION", PMU_ABOX_CPU_OPTION, PMU_ABOX_CPU_ENABLE, 0);
		(void)poke_bits(&pmu, "ABOX_CPU_CONFIGURATION", PMU_ABOX_CPU_CONFIG, 1, 0);
	} else {
		say("abox: 2: ABOX's CPU is off and stays so (bypass)\n");
	}
	(void)peek(&pmu, "ABOX_CPU_STATUS", PMU_ABOX_CPU_STATUS);
	return true;
}

/* ---- step 3: clocks ------------------------------------------------------------------------------------------------ */

/* a divider (or a mux) of CMU_AUD set, then its BUSY waited for */
static bool
cmu_ratio(const char *what, uint32_t off, uint32_t mask, uint32_t value)
{
	uint64_t took;

	(void)poke_bits(&cmu, what, off, mask, value);
	if (!wait_bits(&cmu, off, CMU_DIV_BUSY, 0, 1000, &took)) {
		say("abox: 3: %s still busy after %llu us\n", what, took);
		return false;
	}
	return (rd(&cmu, off) & mask) == value;
}

/* PLL_AUD's rate as its registers say, in Hz (0 while it is off: OSCCLK's 26 MHz goes through then, if MUX_SEL is clear) */
static uint64_t
pll_hz(void)
{
	uint32_t con0 = rd(&cmu, CMU_PLL_CON0), m = (con0 >> 16) & 0x3FF, p = (con0 >> 8) & 0x3F, s = con0 & 7;
	int64_t k = (int16_t)(rd(&cmu, CMU_PLL_CON3) & 0xFFFF);

	if (!(con0 & PLL_MUX_SEL)) {
		return 26000000;
	}
	if (!(con0 & PLL_ENABLE) || p == 0) {
		return 0;
	}
	return (uint64_t)((26000000ll * (((int64_t)m << 16) + k)) / ((int64_t)p << s) >> 16);
}

/* a clock's Q-channel asked on, as clk_enable() does: CLOCK_REQ, if the hardware does not gate it by itself (ENABLE 0) */
static void
qch_on(const char *what, uint32_t off)
{
	uint32_t v = rd(&cmu, off);

	if (v & QCH_ENABLE) {
		say("abox: 3: CMU_AUD %s (%#x) = %#x: gated by the hardware by itself, left so\n", what, ABOX_CMU_PA + off, v);
	} else if (!(v & QCH_CLOCK_REQ)) {
		(void)poke_bits(&cmu, what, off, QCH_CLOCK_REQ, QCH_CLOCK_REQ);
	} else {
		say("abox: 3: CMU_AUD %s (%#x) = %#x: its clock already asked on\n", what, ABOX_CMU_PA + off, v);
	}
}

static bool
step_clocks(void)
{
	uint32_t con0, want;
	uint64_t took, hz;
	int error;

	if (dvfs_khz != 0) {
		say("abox: 3: abox_dvfs=%u: asking ACPM to set the AUD DVFS domain (%u) to %u kHz\n", dvfs_khz, ACPM_DVFS_AUD, dvfs_khz);
		error = hakios_acpm_dvfs_set(ACPM_DVFS_AUD, dvfs_khz);
		say("abox: 3: ACPM answered %d\n", error);
	}
	say("abox: 3: about to write SYSREG_AUD MISC_CON and CMU_AUD (dividers, UAIF1's mux, PLL_AUD, two Q-channels)\n");
	(void)peek(&cmu, "PLL_CON0", CMU_PLL_CON0);
	(void)peek(&cmu, "PLL_CON3", CMU_PLL_CON3);
	(void)peek(&cmu, "MUX_CLK_AUD_CPU", CMU_MUX_CPU);
	(void)peek(&cmu, "DIV_CLK_AUD_PLL", CMU_DIV_PLL);
	(void)peek(&cmu, "CONTROLLER_OPTION", CMU_CONTROLLER_OPTION);
	(void)peek(&cmu, "HCHGEN_CLKMUX", CMU_HCHGEN_CLKMUX);
	(void)peek(&cmu, "QCH ABOX_DUMMY (cpu)", CMU_QCH_DUMMY);
	(void)peek(&cmu, "QCH ABOX_ACLK", CMU_QCH_ACLK);
	(void)peek(&cmu, "QCH GPIO_AUD", CMU_QCH_GPIO);
	(void)peek(&cmu, "GATE BCLK_UAIF1", CMU_GATE_BCLK_UAIF1);
	(void)peek(&cmu, "GATE RSTNSYNC_UAIF1", CMU_GATE_RSTN_UAIF1);
	(void)peek(&sysreg, "BUS_COMPONENT_DRCG_EN", SYSREG_DRCG_EN);
	(void)peek(&sysreg, "SNPS_EMA", SYSREG_SNPS_EMA);
	(void)poke(&sysreg, "MISC_CON", SYSREG_MISC_CON, 1);

	/* the dividers first, while PLL_AUD may still be off: when it comes up, nothing downstream runs faster than meant */
	if (!cmu_ratio("DIV_CLK_AUD_BUS", CMU_DIV_BUS, 0x7, AUD_DIV_BUS) ||
	    !cmu_ratio("DIV_CLK_AUD_BUSP", CMU_DIV_BUSP, 0x3, AUD_DIV_BUSP) ||
	    !cmu_ratio("DIV_CLK_AUD_AUDIF", CMU_DIV_AUDIF, 0x1FF, AUD_DIV_AUDIF) ||
	    !cmu_ratio("DIV_CLK_AUD_UAIF1", CMU_DIV_UAIF1, 0x1FF, AUD_DIV_UAIF1) ||
	    !cmu_ratio("MUX_CLK_AUD_UAIF1", CMU_MUX_UAIF1, 0x1, 0)) {
		say("abox: 3: a divider or the mux did not take its value\n");
		return false;
	}

	con0 = rd(&cmu, CMU_PLL_CON0);
	want = (PLL_AUD_M << 16) | (PLL_AUD_P << 8) | PLL_AUD_S;
	if ((con0 & (PLL_ENABLE | PLL_STABLE | PLL_MUX_SEL)) == (PLL_ENABLE | PLL_STABLE | PLL_MUX_SEL) &&
	    (con0 & 0x03FF3F07) == want && (rd(&cmu, CMU_PLL_CON3) & 0xFFFF) == PLL_AUD_K) {
		say("abox: 3: PLL_AUD already runs at M %u P %u S %u K %u: left so\n", PLL_AUD_M, PLL_AUD_P, PLL_AUD_S, PLL_AUD_K);
	} else {
		/* ra_set_pll(): off (the mux to OSCCLK first), lock time, K, then M/P/S with ENABLE and the mux, until STABLE */
		if (con0 & PLL_MUX_SEL) {
			(void)poke_bits(&cmu, "PLL_CON0 (mux to OSCCLK)", CMU_PLL_CON0, PLL_MUX_SEL, 0);
			if (!wait_bits(&cmu, CMU_PLL_CON0, PLL_MUX_BUSY, 0, 1000, &took)) {
				say("abox: 3: PLL_AUD's mux still busy after %llu us\n", took);
				return false;
			}
		}
		(void)poke_bits(&cmu, "PLL_CON0 (off)", CMU_PLL_CON0, PLL_ENABLE, 0);
		(void)poke_bits(&cmu, "PLL_LOCKTIME", CMU_PLL_LOCKTIME, 0xFFFFF, PLL_AUD_P * PLL_AUD_LOCK);
		(void)poke_bits(&cmu, "PLL_CON3 (K)", CMU_PLL_CON3, 0xFFFF, PLL_AUD_K);
		(void)poke_bits(&cmu, "PLL_CON0 (M P S, on)", CMU_PLL_CON0, 0x03FF3F07 | PLL_MUX_SEL | PLL_ENABLE,
		    want | PLL_MUX_SEL | PLL_ENABLE);
		if (!wait_bits(&cmu, CMU_PLL_CON0, PLL_STABLE, PLL_STABLE, 5000, &took)) {
			say("abox: 3: PLL_AUD not stable after %llu us (PLL_CON0 %#x)\n", took, rd(&cmu, CMU_PLL_CON0));
			return false;
		}
		say("abox: 3: PLL_AUD stable after %llu us\n", took);
	}
	qch_on("QCH ABOX_DUMMY (abox's cpu clock, which abox_enable() turns on with no firmware yet)", CMU_QCH_DUMMY);
	qch_on("QCH ABOX_BCLK1 (UAIF1's bclk_gate)", CMU_QCH_BCLK1);
	qch_on("QCH SYSMMU_AUD (the SysMMU's aclk)", CMU_QCH_SYSMMU);

	hz = pll_hz();
	say("abox: 3: PLL_AUD %llu Hz; so ACLK %llu, its APB %llu, AUDIF %llu, UAIF1's BCLK %llu (want 1179648040, 589824020, "
	    "196608006, 73728002, 1536000)\n", hz, hz / ((rd(&cmu, CMU_DIV_BUS) & 7) + 1),
	    hz / ((rd(&cmu, CMU_DIV_BUS) & 7) + 1) / ((rd(&cmu, CMU_DIV_BUSP) & 3) + 1), hz / ((rd(&cmu, CMU_DIV_AUDIF) & 0x1FF) + 1),
	    hz / ((rd(&cmu, CMU_DIV_AUDIF) & 0x1FF) + 1) / ((rd(&cmu, CMU_DIV_UAIF1) & 0x1FF) + 1));
	if (hz < 1179000000 || hz > 1180300000) {
		say("abox: 3: PLL_AUD is not at the 48 kHz family's rate: stopping here\n");
		return false;
	}
	return true;
}

/* ---- step 4: pins -------------------------------------------------------------------------------------------------- */

static bool
step_pins(void)
{
	say("abox: 4: about to set gpb1-0..3 of GPIO_AUD to UAIF1 (function 2, pull 1, as the device tree's aud-i2s1-bus), then "
	    "release the pads' retention (PMU)\n");
	(void)peek(&gpio, "gpb1 DRV", GPIO_GPB1 + GPIO_DRV);
	(void)peek(&gpio, "gpb1 CONPDN", GPIO_GPB1 + GPIO_CONPDN);
	(void)peek(&gpio, "gpb1 PUDPDN", GPIO_GPB1 + GPIO_PUDPDN);
	(void)poke_bits(&gpio, "gpb1 CON", GPIO_GPB1 + GPIO_CON, UAIF1_PINS_MASK, UAIF1_PINS_FUNC);
	(void)poke_bits(&gpio, "gpb1 PUD", GPIO_GPB1 + GPIO_PUD, UAIF1_PINS_MASK, UAIF1_PINS_PUD);
	pins_set = true;
	if ((rd(&gpio, GPIO_GPB1 + GPIO_CON) & UAIF1_PINS_MASK) != UAIF1_PINS_FUNC) {
		say("abox: 4: the pins did not take function 2\n");
		return false;
	}
	/* abox_pad_retention(false): OPTION's bit 28, then GPIO_MODE's bit 0 */
	(void)poke_bits(&pmu, "PAD_RETENTION_ABOX_OPTION", PMU_PAD_RETENTION_ABOX, PMU_PAD_RELEASE, PMU_PAD_RELEASE);
	(void)poke_bits(&pmu, "GPIO_MODE_ABOX_SYS_PWR_REG", PMU_GPIO_MODE_ABOX, 1, 1);
	(void)peek(&gpio, "gpb1 DAT", GPIO_GPB1 + GPIO_DAT);
	return true;
}

/* ---- step 5: UAIF1 ------------------------------------------------------------------------------------------------- */

static uint32_t
uaif1_ctrl1(void)
{
	return ABOX_FORMAT(HAKIOS_ABOX_SLOT_BITS, HAKIOS_ABOX_CHANNELS) << UAIF_FORMAT_SHIFT | UAIF_BCLK_POLARITY |
	       (HAKIOS_ABOX_CHANNELS - 1) << UAIF_SLOT_MAX_SHIFT | (HAKIOS_ABOX_SLOT_BITS - 1) << UAIF_SBIT_MAX_SHIFT;
}

/*
 * Whether UAIF1's pins move: gpb1's DAT read as fast as it reads, the changes of each of its four pins counted. (That
 * DAT shows a pin's level while the pin is given to UAIF1 is an assumption: if every count is 0 here and RDMA0 still
 * moves, it is that.) At 1.536 MHz BCLK changes more often than the bus can look; LRCLK, 48 kHz, should change about
 * twice every 21 us looked at.
 */
static void
pins_toggle(const char *when)
{
	uint32_t last = rd(&gpio, GPIO_GPB1 + GPIO_DAT) & 0xF, changes[4] = { 0 };
	uint64_t start = now_us(), took;

	for (int i = 0; i < 4000; i++) {
		uint32_t now = rd(&gpio, GPIO_GPB1 + GPIO_DAT) & 0xF, diff = now ^ last;

		for (int pin = 0; pin < 4; pin++) {
			changes[pin] += (diff >> pin) & 1;
		}
		last = now;
	}
	took = now_us() - start;
	say("abox: %u: gpb1-0..3 %s: %u, %u, %u, %u changes in 4000 reads over %llu us\n", step, when, changes[0], changes[1],
	    changes[2], changes[3], took);
}

static void
uaif1_enable(bool on)
{
	(void)poke_bits(&abox, on ? "UAIF1_CTRL0 (SPK_ENABLE)" : "UAIF1_CTRL0 (SPK_ENABLE off)", ABOX_UAIF_CTRL0(1),
	    UAIF_SPK_ENABLE, on ? UAIF_SPK_ENABLE : 0);
	uaif_on = on;
}

static bool
step_uaif(void)
{
	uint32_t ctrl1 = uaif1_ctrl1();

	say("abox: 5: about to make UAIF1 the master of an I2S of 2 x %u bits (CTRL1 %#x, Samsung's NB_NF: BCLK_POLARITY 1, "
	    "WS_POLAR 0)\n", HAKIOS_ABOX_SLOT_BITS, ctrl1);
	(void)peek(&abox, "UAIF1_STATUS", ABOX_UAIF_STATUS(1));
	(void)poke_bits(&abox, "UAIF1_CTRL0", ABOX_UAIF_CTRL0(1), UAIF_SPK_ENABLE | UAIF_MODE_MASTER, UAIF_MODE_MASTER);
	if (poke(&abox, "UAIF1_CTRL1", ABOX_UAIF_CTRL1(1), ctrl1) != ctrl1) {
		return false;
	}
	if (bclk_on) {
		(void)poke_bits(&abox, "QCHANNEL_DISABLE (BCLK_UAIF1)", ABOX_QCHANNEL_DISABLE, QCHANNEL_BCLK_UAIF1, QCHANNEL_BCLK_UAIF1);
	}
	/* a moment of UAIF1 alone, nothing routed to it, no amplifier on: do its clocks come out? */
	pins_toggle("with UAIF1 off");
	uaif1_enable(true);
	spin_us(50000);
	pins_toggle("with UAIF1 on");
	(void)peek(&abox, "UAIF1_STATUS", ABOX_UAIF_STATUS(1));
	uaif1_enable(false);
	return true;
}

/* ---- step 6: route ------------------------------------------------------------------------------------------------- */

/*
 * SPUS's sample banks (sys/hakios.exynos_abox.h): all read and said; RDMA0's given abox_sbank's bytes when its SIZE reads
 * 0 (or whatever it reads, when abox_sbank= was given), its STR kept, as Samsung's S10 driver does. A write that does not
 * hold is said and the run goes on: the stream (step 7) is what says whether RDMA0 reads.
 */
static void
sample_banks(void)
{
	uint32_t v[8], asrc, mixp, want;

	for (uint32_t n = 0; n < 8; n++) {
		v[n] = rd(&abox, ABOX_SPUS_SBANK_RDMA(n));
	}
	asrc = rd(&abox, ABOX_SPUS_SBANK_ASRC0);
	mixp = rd(&abox, ABOX_SPUS_SBANK_MIXP);
	say("abox: 6: SPUS's sample banks (%#x on): RDMA0-7 %#x %#x %#x %#x %#x %#x %#x %#x, ASRC0 %#x, MIXP %#x\n",
	    ABOX_SFR_PA + ABOX_SPUS_SBANK_RDMA(0), v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], asrc, mixp);
	say("abox: 6: RDMA0's bank: %u bytes at %#x; the mixer's: %u bytes at %#x\n", SBANK_BYTES(v[0]), v[0] & SBANK_STR_MASK,
	    SBANK_BYTES(mixp), mixp & SBANK_STR_MASK);
	if (sbank_bytes == 0 || (!sbank_given && SBANK_BYTES(v[0]) != 0)) {
		say("abox: 6: RDMA0's sample bank left as it reads\n");
		return;
	}
	want = (sbank_bytes / 16) << SBANK_SIZE_SHIFT;
	say("abox: 6: about to give RDMA0 a sample bank of %u bytes (SBANK_RDMA0 SIZE [29:20]; Samsung's S10 driver sets it, "
	    "the 9810's AP never does)\n", sbank_bytes);
	if ((poke_bits(&abox, "SPUS_SBANK_RDMA0 (SIZE)", ABOX_SPUS_SBANK_RDMA(0), SBANK_SIZE_MASK, want) & SBANK_SIZE_MASK) !=
	    want) {
		say("abox: 6: the bank's size did not hold (not the AP's to write?): going on; step 7 says whether RDMA0 reads\n");
	}
}

static bool
step_route(void)
{
	uint32_t format = ABOX_FORMAT(HAKIOS_ABOX_SLOT_BITS, HAKIOS_ABOX_CHANNELS);

	say("abox: 6: about to route RDMA0 -> SPUS chain 0 (no ASRC) -> %s -> UAIF1\n", route_sifs1 ? "SIFS1" : "SIFS0, the mixer");
	(void)peek(&abox, "SPUS_CTRL3", ABOX_SPUS_CTRL3);
	if (route_sifs1) {
		(void)poke_bits(&abox, "SPUS_CTRL0 (chain 0)", ABOX_SPUS_CTRL0, SPUS_CHAIN_MASK, SPUS_OUT_SIFS1);
		(void)poke_bits(&abox, "SPUS_CTRL1 (SIFS1 from OUT0)", ABOX_SPUS_CTRL1, SPUS_SIFS1_SEL_MASK, 0);
		(void)poke_bits(&abox, "ROUTE_CTRL0 (UAIF1 SPK)", ABOX_ROUTE_CTRL0, 0xFu << ROUTE_UAIF1_SHIFT,
		    ROUTE_SIFS1 << ROUTE_UAIF1_SHIFT);
	} else {
		(void)poke_bits(&abox, "SPUS_CTRL0 (chain 0)", ABOX_SPUS_CTRL0, SPUS_CHAIN_MASK, SPUS_OUT_SIFS0);
		(void)poke_bits(&abox, "SPUS_CTRL1 (MIXP_FORMAT)", ABOX_SPUS_CTRL1, SPUS_MIXP_FORMAT_MASK, format);
		(void)poke_bits(&abox, "ROUTE_CTRL0 (UAIF1 SPK)", ABOX_ROUTE_CTRL0, 0xFu << ROUTE_UAIF1_SHIFT,
		    ROUTE_SIFS0 << ROUTE_UAIF1_SHIFT);
		(void)poke_bits(&abox, "SPUS_CTRL2 (MIXP_FLUSH)", ABOX_SPUS_CTRL2, 1, 1);
	}
	sample_banks();
	return (rd(&abox, ABOX_SPUS_CTRL0) & SPUS_CHAIN_MASK) == (route_sifs1 ? SPUS_OUT_SIFS1 : SPUS_OUT_SIFS0) &&
	       ((rd(&abox, ABOX_ROUTE_CTRL0) >> ROUTE_UAIF1_SHIFT) & 0xF) == (route_sifs1 ? ROUTE_SIFS1 : ROUTE_SIFS0);
}

/* ---- RDMA0: how the stream's memory is read ------------------------------------------------------------------------ */

/*
 * The four things the stream asks of RDMA0. Here the AP does them (bypass). A firmware mode gives the same through
 * calliope's IPC: prepare = PCM_SET_BUFFER + PCM_PLTDAI_HW_PARAMS + PCM_PLTDAI_PREPARE, start/stop = PCM_PLTDAI_TRIGGER
 * 1/0 (stop then waiting for CTRL0's ENABLE to read 0, as abox_rdma_hw_free() does), position = STATUS as here.
 */
struct abox_dma {
	const char *name;
	bool (*prepare)(uint64_t pa, uint32_t ring_bytes, uint32_t period_bytes);
	bool (*start)(void);
	bool (*position)(uint32_t *bytes, uint32_t *status);    /* false: STATUS says it is not running */
	bool (*stop)(void);
};

#define R0(reg)                 (ABOX_RDMA(0) + (reg))

static bool
bypass_prepare(uint64_t pa, uint32_t ring_bytes, uint32_t period_bytes)
{
	uint32_t ctrl0, v;

	say("abox: %u: about to program RDMA0 (%#x): the ring at %#llx, %u bytes, periods of %u\n", step, ABOX_SFR_PA + R0(0), pa,
	    ring_bytes, period_bytes);
	for (uint32_t off = 0; off <= RDMA_STATUS; off += 4) {
		say("abox: %u: RDMA0 +%#04x = %#x\n", step, off, rd(&abox, R0(off)));
	}
	ctrl0 = rd(&abox, R0(RDMA_CTRL0));
	if (ctrl0 & RDMA_ENABLE) {
		(void)poke(&abox, "RDMA0_CTRL0 (off first)", R0(RDMA_CTRL0), ctrl0 & ~RDMA_ENABLE);
	}
	(void)poke(&abox, "RDMA0_BUF_STR", R0(RDMA_BUF_STR), (uint32_t)pa);
	(void)poke(&abox, "RDMA0_BUF_END", R0(RDMA_BUF_END), (uint32_t)(pa + ring_bytes));
	(void)poke(&abox, "RDMA0_BUF_OFFSET", R0(RDMA_BUF_OFFSET), period_bytes);
	(void)poke(&abox, "RDMA0_STR_POINT", R0(RDMA_STR_POINT), (uint32_t)pa);
	v = rd(&abox, R0(RDMA_VOL_FACTOR));
	if (vol != UINT32_MAX) {
		(void)poke_bits(&abox, "RDMA0_VOL_FACTOR (abox_vol)", R0(RDMA_VOL_FACTOR), RDMA_VOL_MASK, vol);
	} else if ((v & RDMA_VOL_MASK) == 0) {
		(void)poke_bits(&abox, "RDMA0_VOL_FACTOR (0 read: unity, inferred)", R0(RDMA_VOL_FACTOR), RDMA_VOL_MASK, 0x800000);
	}
	ctrl0 = rd(&abox, R0(RDMA_CTRL0));
	say("abox: %u: RDMA0 CTRL0 BURST_LEN [22:19] reads %u\n", step, (ctrl0 & RDMA_BURST_MASK) >> RDMA_BURST_SHIFT);
	if (burst != 0 && (burst_given || (ctrl0 & RDMA_BURST_MASK) == 0)) {
		(void)poke_bits(&abox, "RDMA0_CTRL0 (BURST_LEN, as Samsung's S10 driver sets it; ENABLE kept 0)", R0(RDMA_CTRL0),
		    RDMA_BURST_MASK | RDMA_ENABLE, burst << RDMA_BURST_SHIFT);
	}
	if (rdma_ctrl1 != UINT32_MAX) {
		(void)poke(&abox, "RDMA0_CTRL1 (abox_rdma_ctrl1)", R0(RDMA_CTRL1), rdma_ctrl1);
	}
	if (rdma_ctrl0 != UINT32_MAX) {
		(void)poke(&abox, "RDMA0_CTRL0 (abox_rdma_ctrl0, off)", R0(RDMA_CTRL0), rdma_ctrl0 & ~RDMA_ENABLE);
	}
	return rd(&abox, R0(RDMA_BUF_STR)) == (uint32_t)pa && rd(&abox, R0(RDMA_BUF_END)) == (uint32_t)(pa + ring_bytes);
}

static bool
bypass_start(void)
{
	(void)poke_bits(&abox, "RDMA0_CTRL0 (ENABLE)", R0(RDMA_CTRL0), RDMA_ENABLE, RDMA_ENABLE);
	rdma_on = true;
	return true;
}

static bool
bypass_position(uint32_t *bytes, uint32_t *status)
{
	uint32_t s = rd(&abox, R0(RDMA_STATUS));

	*status = s;
	*bytes = RDMA_RBUF_OFFSET(s) + RDMA_RBUF_CNT(s);
	return (s & RDMA_PROGRESS) != 0;
}

static bool
bypass_stop(void)
{
	uint64_t took;
	bool stopped;

	(void)poke_bits(&abox, "RDMA0_CTRL0 (ENABLE off)", R0(RDMA_CTRL0), RDMA_ENABLE, 0);
	stopped = wait_bits(&abox, R0(RDMA_CTRL0), RDMA_ENABLE, 0, DMA_STOP_US, &took);
	say("abox: %u: RDMA0 %s after %llu us (STATUS %#x)\n", step, stopped ? "stopped" : "has NOT said it stopped", took,
	    rd(&abox, R0(RDMA_STATUS)));
	rdma_on = !stopped;
	return stopped;
}

static const struct abox_dma bypass_dma = {
	.name = "RDMA0 programmed by the AP (bypass)",
	.prepare = bypass_prepare, .start = bypass_start, .position = bypass_position, .stop = bypass_stop,
};
static const struct abox_dma *dma = &bypass_dma;

/* ---- the stream (steps 7 and 8) ------------------------------------------------------------------------------------ */

/* ABOX's SysMMU must pass the ring's physical address through untranslated */
static bool
sysmmu_passes(void)
{
	uint32_t ctrl = peek(&mmu, "MMU_CTRL", MMU_CTRL);

	(void)peek(&mmu, "MMU_CFG", MMU_CFG);
	(void)peek(&mmu, "MMU_STATUS", MMU_STATUS);
	(void)peek(&mmu, "MMU_VERSION", MMU_VERSION);
	if (ctrl & MMU_CTRL_TRANSLATE) {
		say("abox: %u: ABOX's SysMMU translates (CTRL bit 2): RDMA0 would read through tables nobody here made; no stream\n",
		    step);
		return false;
	}
	if (ctrl & MMU_CTRL_BLOCK) {
		say("abox: %u: ABOX's SysMMU is blocked and not translating: CTRL_DISABLE (0), so that it lets ABOX through\n", step);
		ctrl = poke(&mmu, "MMU_CTRL", MMU_CTRL, MMU_CTRL_DISABLE);
		if (ctrl & (MMU_CTRL_TRANSLATE | MMU_CTRL_BLOCK)) {
			return false;
		}
	}
	say("abox: %u: ABOX's SysMMU does not translate: RDMA0's addresses are physical (CTRL %#x)\n", step, ctrl);
	return true;
}

static bool
area_alloc(void)
{
	/* below 4 GB: RDMA0's address registers are 32 bits (max_pnum is a page number, inclusive) */
	ppnum_t max_pnum = (ppnum_t)((0x100000000ull >> PAGE_SHIFT) - 1);

	if (area != 0) {
		return true;
	}
	if (kmem_alloc_contig(kernel_map, &area, AREA_BYTES, PAGE_MASK, max_pnum, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("abox: %u: no %u KB of contiguous memory below 4 GB\n", step, AREA_BYTES / 1024);
		area = 0;
		return false;
	}
	area_pa = kvtophys(area);
	if (area_pa == 0 || area_pa + AREA_BYTES > 0x100000000ull || (area_pa & 0xF) != 0) {
		say("abox: %u: the memory is at %#llx, not below 4 GB: not used\n", step, area_pa);
		kmem_free(kernel_map, area, AREA_BYTES);
		area = 0;
		return false;
	}
	bzero((void *)area, AREA_BYTES);
	FlushPoC_DcacheRegion(area, AREA_BYTES);
	say("abox: %u: the ring: %u bytes at %#llx (kernel %#lx), then zeroes to %u KB\n", step, RING_BYTES, area_pa, area,
	    AREA_BYTES / 1024);
	return true;
}

static void
area_free(void)
{
	if (area != 0 && !rdma_on && !area_kept) {
		kmem_free(kernel_map, area, AREA_BYTES);
		area = 0;
	}
}

/* what iOS writes (step 8): the sink puts frames into the ring, the feeder takes them */
static int16_t sink_ring[SINK_FRAMES * 2];
static volatile uint32_t sink_head, sink_tail;
static volatile uint64_t last_sound_us;
static volatile uint32_t sink_peak;
static uint64_t sink_frames, sink_lost, fed_frames, short_periods;

/* From the writer's thread, under hakios.audio.c's lock: never waits */
static uint32_t
abox_sink(const int16_t *frames, uint32_t count)
{
	uint32_t head = sink_head, room = SINK_FRAMES - (head - sink_tail), taken = count < room ? count : room, peak = 0;

	for (uint32_t i = 0; i < taken; i++) {
		int32_t l = frames[2 * i], r = frames[2 * i + 1];
		uint32_t al = (uint32_t)(l < 0 ? -l : l), ar = (uint32_t)(r < 0 ? -r : r);

		sink_ring[2 * ((head + i) % SINK_FRAMES)] = (int16_t)l;
		sink_ring[2 * ((head + i) % SINK_FRAMES) + 1] = (int16_t)r;
		peak = al > peak ? al : peak;
		peak = ar > peak ? ar : peak;
	}
	OSMemoryBarrier();
	sink_head = head + taken;
	sink_frames += taken;
	sink_lost += count - taken;
	if (peak > SILENCE) {
		last_sound_us = now_us();
		if (peak > sink_peak) {
			sink_peak = peak;
		}
	}
	return taken;
}

/* a 1 kHz sine at -20 dBFS, 48 samples a cycle: a quarter of it */
static const int16_t quarter[13] = { 0, 428, 848, 1254, 1638, 1995, 2317, 2600, 2838, 3028, 3165, 3249, 3277 };
static uint32_t tone_phase;

static int16_t
tone_sample(void)
{
	uint32_t k = tone_phase++ % 48;

	return k <= 12 ? quarter[k] : k <= 24 ? quarter[24 - k] : k <= 36 ? (int16_t)-quarter[k - 24] : (int16_t)-quarter[48 - k];
}

/*
 * iOS's volume (step 8): `below' eighths of a dB under the loudest (sys/hakios.audio.h), taken off the samples before the
 * guard. On the POCO the amplifiers take it off (hakios.speaker.c's volume); the MAX98512s' hook has no volume, so here it
 * is digital: 10^(-below/160) in Q16, from a table of whole dBs, one of eighths, and a tenth for every 20 dB.
 */
static volatile uint32_t sink_below;
static const uint16_t db_whole[20] = { 65535, 58409, 52057, 46396, 41350, 36854, 32846, 29274, 26090, 23253, 20724, 18471,
	                               16462, 14672, 13076, 11654, 10387, 9257, 8250, 7353 };
static const uint16_t db_eighth[8] = { 65535, 64600, 63677, 62767, 61870, 60986, 60115, 59256 };

static uint32_t
below_gain(uint32_t below)
{
	uint32_t g = (uint32_t)db_whole[(below % 160) / 8] * db_eighth[below % 8] >> 16;

	for (uint32_t tens = below / 160; tens != 0 && g != 0; tens--) {
		g /= 10;
	}
	return below == 0 ? 65536 : g;
}

enum source { SILENT, TONE, SINK };
static int16_t period_buf[PERIOD_FRAMES * 2];

/* one period of the ring: from `source`, through the guard, cleaned to memory */
static void
fill_period(uint32_t p, enum source source)
{
	uint32_t n = 0;

	if (source == TONE) {
		for (uint32_t i = 0; i < PERIOD_FRAMES; i++) {
			period_buf[2 * i] = period_buf[2 * i + 1] = tone_sample();
		}
		n = PERIOD_FRAMES;
	} else if (source == SINK) {
		uint32_t tail = sink_tail, have = sink_head - tail;

		OSMemoryBarrier();
		n = have < PERIOD_FRAMES ? have : PERIOD_FRAMES;
		for (uint32_t i = 0; i < n; i++) {
			period_buf[2 * i] = sink_ring[2 * ((tail + i) % SINK_FRAMES)];
			period_buf[2 * i + 1] = sink_ring[2 * ((tail + i) % SINK_FRAMES) + 1];
		}
		OSMemoryBarrier();
		sink_tail = tail + n;
		fed_frames += n;
		if (n != 0 && n < PERIOD_FRAMES) {
			short_periods++;
		}
		if (sink_below != 0) {
			uint32_t g = below_gain(sink_below);

			for (uint32_t i = 0; i < 2 * n; i++) {
				period_buf[i] = (int16_t)(((int32_t)period_buf[i] * (int32_t)g) >> 16);
			}
		}
	}
	bzero(period_buf + 2 * n, (PERIOD_FRAMES - n) * FRAME_BYTES);
	hakios_guard_process(period_buf, PERIOD_FRAMES);       /* silence too: its filter and its averages go on */
	if (source == SINK && n != 0 && hakios_audio_recording_fed()) {
		hakios_audio_record_fed(period_buf, n);
	}
	memcpy((void *)(area + p * PERIOD_BYTES), period_buf, PERIOD_BYTES);
	FlushPoC_DcacheRegion(area + p * PERIOD_BYTES, PERIOD_BYTES);
}

/* tenths of a dB as text */
static const char *
db_text(char *text, int tenths)
{
	snprintf(text, 16, "%s%d.%d", tenths < 0 ? "-" : "", (tenths < 0 ? -tenths : tenths) / 10, (tenths < 0 ? -tenths : tenths) % 10);
	return text;
}

static void
guard_say(void)
{
	struct hakios_guard_report r;
	char t[2][16];

	hakios_guard_take_report(&r);
	if (r.blocks == 0) {
		return;
	}
	say("abox: %u: the guard: peaks in %u, out %u (of 32768); least gain %s dB; peaks limited in %u frames (by %s dB at most)\n",
	    step, r.peak_in, r.peak_out, db_text(t[0], hakios_guard_decibels(r.least_gain * r.least_gain >> 30)), r.limited_frames,
	    db_text(t[1], hakios_guard_decibels((uint64_t)r.least_peak_gain * r.least_peak_gain >> 2)));
}

static uint32_t clock_writes[3];        /* PLL_CON0, DIV AUDIF, DIV UAIF1 as left by step 3 */

static bool
stream_start(void)
{
	uint32_t s[10], bytes;

	if (!sysmmu_passes() || !area_alloc()) {
		return false;
	}
	hakios_guard_configure(true, gain);
	hakios_guard_attenuation(0);            /* the amplifiers taken at Android's loudest: the most careful */
	hakios_guard_monitor(0);
	for (uint32_t p = 0; p < PERIODS; p++) {
		fill_period(p, SILENT);
	}
	if (!dma->prepare(area_pa, RING_BYTES, PERIOD_BYTES)) {
		say("abox: %u: RDMA0 did not take the ring\n", step);
		return false;
	}
	clock_writes[0] = rd(&cmu, CMU_PLL_CON0);
	clock_writes[1] = rd(&cmu, CMU_DIV_AUDIF);
	clock_writes[2] = rd(&cmu, CMU_DIV_UAIF1);
	say("abox: %u: about to start: UAIF1 first, then RDMA0 (%s), Samsung's order for these links\n", step, dma->name);
	(void)peek(&abox, "UAIF1_STATUS", ABOX_UAIF_STATUS(1));
	uaif1_enable(true);
	if (!dma->start()) {
		return false;
	}
	for (int i = 0; i < 10; i++) {
		spin_us(1000);
		(void)dma->position(&bytes, &s[i]);
	}
	say("abox: %u: RDMA0 STATUS each ms: %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x\n", step, s[0], s[1], s[2], s[3], s[4], s[5],
	    s[6], s[7], s[8], s[9]);
	(void)peek(&abox, "RDMA0_CTRL0", R0(RDMA_CTRL0));
	(void)peek(&abox, "UAIF1_STATUS", ABOX_UAIF_STATUS(1));
	pins_toggle("with the stream on");
	return true;
}

static void
stream_stop(void)
{
	say("abox: %u: stopping: the amplifiers, then RDMA0, then UAIF1 (Samsung's order)\n", step);
	amps_stop();
	if (rdma_on && !dma->stop()) {
		area_kept = true;
		say("abox: %u: RDMA0 may still read the ring: its memory is never given back in this run\n", step);
	}
	spin_us(FIFO_FLUSH_US);
	if (uaif_on) {
		uaif1_enable(false);
	}
	(void)peek(&abox, "UAIF1_STATUS", ABOX_UAIF_STATUS(1));
	area_free();
}

/*
 * The feeder: RDMA0's position every 2 ms, and each period it has finished refilled from `source` (step 7: the tone
 * until `tone_until_us`, silence after; step 8: what iOS wrote). Step 7 returns at `until_us`; step 8 never returns.
 */
static bool
stream_run(uint64_t tone_until_us, uint64_t until_us)
{
	uint64_t start = now_us(), last_report = start, last_volume = 0, clock_from = 0;
	uint64_t read_bytes = 0, read_at_report = 0, refilled = 0, outside = 0;
	uint32_t next_fill = 0, last_pos = 0, reports = 0;
	bool moved = false, by_clock = false, muted = false, said_outside = false;

	for (;;) {
		uint64_t now = now_us();
		uint32_t pos, status, hw;

		if (until_us != 0 && now >= until_us) {
			return true;
		}
		if (!(rd(&abox, R0(RDMA_CTRL0)) & RDMA_ENABLE)) {
			say("abox: %u: RDMA0's ENABLE reads 0 of itself (CTRL0 %#x, STATUS %#x): the stream stops; whether RDMA0 still "
			    "reads is not known, so its memory is never given back\n", step, rd(&abox, R0(RDMA_CTRL0)),
			    rd(&abox, R0(RDMA_STATUS)));
			rdma_on = false;
			area_kept = true;
			return false;
		}
		(void)dma->position(&pos, &status);
		if (pos >= RING_BYTES) {
			outside++;
			if (!said_outside) {
				say("abox: %u: RDMA0's position %#x (STATUS %#x) is outside the ring of %#x bytes: it reads the zeroes after\n",
				    step, pos, status, RING_BYTES);
				said_outside = true;
			}
			if (pos >= AREA_BYTES || outside > 25) {
				say("abox: %u: still outside after %llu looks: stopping the stream (the ring's end is not where RDMA0 takes it)\n",
				    step, outside);
				return false;
			}
		} else {
			outside = 0;
			if (pos != last_pos) {
				read_bytes += (pos + RING_BYTES - last_pos) % RING_BYTES;
				moved = true;
			}
			last_pos = pos;
		}
		if (!moved && !by_clock && now - start > 200000) {
			say("abox: %u: RDMA0's STATUS has not moved in 200 ms (%#x): the ring is refilled by the clock from here on, at "
			    "%u bytes a second (an assumption, so that a DMA that reads without saying so still gets sound)\n", step, status,
			    BYTES_PER_SECOND);
			by_clock = true;
			clock_from = now;
		}
		hw = by_clock ? (uint32_t)(((now - clock_from) * BYTES_PER_SECOND / 1000000) % RING_BYTES) :
		    pos < RING_BYTES ? pos : next_fill * PERIOD_BYTES;
		while (next_fill != hw / PERIOD_BYTES) {
			fill_period(next_fill, steps < 8 ? (now < tone_until_us ? TONE : SILENT) : SINK);
			next_fill = (next_fill + 1) % PERIODS;
			refilled++;
		}

		/* step 8: the amplifiers while iOS plays, and not while it is muted */
		if (steps >= 8) {
			uint64_t sounded = last_sound_us, since = sounded > now ? 0 : now - sounded;    /* (the sink's clock may be ahead) */

			if (now - last_volume >= 100000) {
				uint32_t below, generation;
				bool m = false;

				last_volume = now;
				if (hakios_audio_volume(&below, &m, &generation)) {
					if (m != muted) {
						muted = m;
						say("abox: 8: iOS %s\n", muted ? "mutes: the amplifiers stay off" : "unmutes");
					}
					if (below != sink_below) {
						sink_below = below;
						say("abox: 8: iOS's volume: %u.%03u dB under the loudest, taken off the samples (x %u/65536)\n",
						    below / 8, below % 8 * 125, below_gain(below));
					}
				}
			}
			if (amp_running == NULL && !amp_given_up && !muted && sounded != 0 && since < 100000) {
				if (amp_hook != NULL) {
					say("abox: 8: iOS plays: starting the amplifiers\n");
				}
				sink_peak = 0;
				amps_start();
			} else if (amp_running != NULL && (muted || since > QUIET_STOP_US)) {
				say("abox: 8: %s: stopping the amplifiers; what iOS wrote peaked at %u of 32768\n",
				    muted ? "muted" : "three seconds of silence", sink_peak);
				guard_say();
				amps_stop();
			}
		}

		/* what is going on: every second in step 7; in step 8 every 5 s while the amplifiers are on, and the first 4 */
		if (now - last_report >= (steps < 8 ? 1000000u : 5000000u) && (steps < 8 || amp_running != NULL || reports < 4)) {
			uint64_t rate = (read_bytes - read_at_report) * 1000000 / (now - last_report);

			say("abox: %u: %llu.%01llu s: STATUS %#x (progress %u, period at %#x, %u into it); %llu bytes a second read "
			    "(%u is 48 kHz 16-bit stereo)%s; %llu periods refilled; CTRL0 %#x; UAIF1_STATUS %#x\n", step,
			    (now - start) / 1000000, (now - start) / 100000 % 10, status, (status & RDMA_PROGRESS) ? 1u : 0u,
			    RDMA_RBUF_OFFSET(status),
			    RDMA_RBUF_CNT(status), rate, BYTES_PER_SECOND, by_clock ? " (refilled by the clock)" : "", refilled,
			    rd(&abox, R0(RDMA_CTRL0)), rd(&abox, ABOX_UAIF_STATUS(1)));
			if (steps >= 8) {
				say("abox: 8: from iOS %llu frames (%llu lost), %llu fed, %llu periods short of frames\n", sink_frames, sink_lost,
				    fed_frames, short_periods);
				guard_say();
			}
			if (rd(&cmu, CMU_PLL_CON0) != clock_writes[0] || rd(&cmu, CMU_DIV_AUDIF) != clock_writes[1] ||
			    rd(&cmu, CMU_DIV_UAIF1) != clock_writes[2]) {
				say("abox: %u: CMU_AUD changed under us: PLL_CON0 %#x, DIV AUDIF %#x, DIV UAIF1 %#x (were %#x, %#x, %#x)\n", step,
				    rd(&cmu, CMU_PLL_CON0), rd(&cmu, CMU_DIV_AUDIF), rd(&cmu, CMU_DIV_UAIF1), clock_writes[0], clock_writes[1],
				    clock_writes[2]);
			}
			last_report = now;
			read_at_report = read_bytes;
			reports++;
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
}

/* step 7: the stream with a tone for abox_secs */
static bool
step_stream(void)
{
	uint64_t t;
	bool ok;

	if (!stream_start()) {
		return false;
	}
	/* the ring holds silence now; the amplifiers come up on it, and the tone begins with the next period refilled */
	amps_start();
	t = now_us();
	say("abox: 7: the tone for %u s (1 kHz, -20 dBFS, both channels), through the speakers' guard\n", secs);
	ok = stream_run(t + secs * 1000000ull, t + secs * 1000000ull + 200000);
	guard_say();
	return ok;
}

/* step 8: the stream for iOS, until the run ends */
static void
step_sink(void)
{
	if (!stream_start()) {
		return;
	}
	hakios_audio_set_sink("ABOX: RDMA0 -> UAIF1 -> the speakers", abox_sink);
	say("abox: 8: /dev/hakios-audio plays through ABOX; the amplifiers start when iOS plays\n");
	(void)stream_run(0, 0);
	/* (only on a failure of RDMA0's: what iOS writes is then dropped) */
	hakios_audio_set_sink("nothing (ABOX's stream stopped)", NULL);
	stream_stop();
}

/* ---- the whole ----------------------------------------------------------------------------------------------------- */

/* what was done, undone: the stream stopped, the pins idle, the domain off if it was off before */
static void
put_back(void)
{
	uint64_t took;

	say("abox: putting back what was done\n");
	if (uaif_on || rdma_on || amp_running != NULL) {
		stream_stop();
	}
	if (pins_set) {
		(void)poke_bits(&gpio, "gpb1 CON (idle)", GPIO_GPB1 + GPIO_CON, UAIF1_PINS_MASK, UAIF1_PINS_IDLE);
		/* abox_pad_retention(true): GPIO_MODE's bit 0, so that the pads are held through the power-down */
		(void)poke_bits(&pmu, "GPIO_MODE_ABOX_SYS_PWR_REG", PMU_GPIO_MODE_ABOX, 1, 1);
	}
	if (powered_here && !rdma_on) {
		(void)poke_bits(&pmu, "AUD_CONFIGURATION (off)", PMU_AUD_CONFIGURATION, 0xF, 0);
		say("abox: the AUD domain %s after %llu us\n", wait_bits(&pmu, PMU_AUD_STATUS, 0xF, 0, 10000, &took) ? "is off" :
		    "has NOT gone off", took);
	}
}

static void
abox_thread(__unused void *arg, __unused wait_result_t result)
{
	bool (*const fn[STEPS])(void) = { step_look, step_power, step_clocks, step_pins, step_uaif, step_route, step_stream, NULL };

	say("abox: ABOX's speaker path, %u step%s of 8, bypass (ABOX's CPU off, no firmware)\n", steps, steps == 1 ? "" : "s");
	if (!map_blocks()) {
		goto out;
	}
	for (step = 1; step <= steps && step <= STEPS; step++) {
		say("abox: %u: %s\n", step, step_names[step]);
		if (step == 8) {
			step_sink();            /* returns only when the stream could not start or has failed */
			say("abox: 8: the stream has stopped\n");
			put_back();
			goto out;
		}
		if (!fn[step - 1]()) {
			say("abox: %u: failed; nothing more is done\n", step);
			break;
		}
	}
	if (step > steps) {
		step = steps;
		say("abox: the first %u step%s done\n", steps, steps == 1 ? "" : "s");
	}
	put_back();
out:
	thread_terminate(current_thread());
}

void
hakios_exynos_abox_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("abox", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != STARLTE_SOC_BASE) {
		return;                 /* not the Exynos 9810 */
	}
	steps = steps > STEPS ? STEPS : steps;
	PE_parse_boot_argn("abox_secs", &secs, sizeof(secs));
	secs = secs < 1 ? 1 : secs > 10 ? 10 : secs;
	PE_parse_boot_argn("abox_route", &route_sifs1, sizeof(route_sifs1));
	PE_parse_boot_argn("abox_rdma_ctrl0", &rdma_ctrl0, sizeof(rdma_ctrl0));
	PE_parse_boot_argn("abox_rdma_ctrl1", &rdma_ctrl1, sizeof(rdma_ctrl1));
	PE_parse_boot_argn("abox_vol", &vol, sizeof(vol));
	PE_parse_boot_argn("abox_bclk_on", &bclk_on, sizeof(bclk_on));
	PE_parse_boot_argn("abox_gain", &gain, sizeof(gain));
	PE_parse_boot_argn("abox_dvfs", &dvfs_khz, sizeof(dvfs_khz));
	gain = gain > HAKIOS_GUARD_MAX_GAIN ? HAKIOS_GUARD_MAX_GAIN : gain;
	sbank_given = PE_parse_boot_argn("abox_sbank", &sbank_bytes, sizeof(sbank_bytes));
	sbank_bytes = sbank_bytes > 0x3FF * 16 ? 0x3FF * 16 : sbank_bytes & ~15u;
	burst_given = PE_parse_boot_argn("abox_burst", &burst, sizeof(burst));
	burst = burst > 15 ? 15 : burst;
	if (kernel_thread_start(abox_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
