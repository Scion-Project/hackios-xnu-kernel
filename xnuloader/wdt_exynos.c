/* The Exynos cluster watchdog (platform_starlte's exynos_wdt). */

/* Armed from the loader's start to the end of the run and fed by this core  */
/* while it watches a kernel on another: when the whole SoC hangs, this core */
/* with it, the watchdog resets the SoC. S-Boot then finds INFORM3 cleared   */
/* and starts BOOT, not TWRP: a run that ends normally restarts with the     */
/* PMU's SWRESET instead (platform_restart), which keeps INFORM3.            */

/*
 * The facts are the mainline Linux port to the S9+'s, which turns every hang into a restart into TWRP this way
 * (github.com/SheroAbi/galaxy-s9plus-mainline-linux: its device tree, src/mainline-dts/exynos9810-star2lte.dts, GPL-2.0
 * OR BSD-3-Clause -- the clock-gates node's watchdog writes and the watchdog@10060000 node; docs/04-drivers.md and
 * 06-known-issues.md), and Samsung's kernel's (drivers/watchdog/s3c2410_wdt.c, and the phone's device tree's
 * watchdog_cl0 and watchdog_cl1; GPL, read for the facts):
 *   the watchdogs  cluster 0's at 0x10050000 ("samsung,exynos8-wdt": its PMU bit is 24), cluster 1's at 0x10060000
 *                  ("samsung,exynos7-wdt": bit 23; SPI 465). Both count the 26 MHz oscillator. WTCON +0x0 (bit 0 the
 *                  reset, bit 2 the interrupt, [4:3] the divider 16 << n, bit 5 on, [15:8] the prescaler less 1),
 *                  WTDAT +0x4 (the count reloaded), WTCNT +0x8 (the count; written to feed it), WTCLRINT +0xC. S-Boot
 *                  and the kernel before leave both running; the S9+ port stops cluster 0's and arms cluster 1's
 *   the PMU        (0x14060000) MASK_WDT_RESET_REQUEST +0x40C: with a watchdog's bit set its expiry resets nothing. S-Boot
 *                  leaves it all set (0xFFFFFFFF): on the S9+, arming the watchdog alone changed nothing until bit 23 was
 *                  cleared. DISABLE_WDT +0x408: with the bit set the PMU stops the watchdog by itself; Samsung's and
 *                  mainline's drivers clear it when they arm one. RST_STAT +0x404: why the SoC last reset (bit 23:
 *                  cluster 1's watchdog)
 *   the timeout    prescaler 255, divider 128 (WTCON 0xFF39: on, the reset, no interrupt): 26 MHz / 256 / 128 = 793.457
 *                  Hz in 16 bits of count, 82 s at most; 30 s is about 23805
 *   afterwards     S-Boot reads INFORM3 = 0x12345674 as "recovery" (and INFORM2 = 0x12345678: a restart, not a power-off).
 *                  The S9+ port writes them first thing and has its watchdog's resets land in TWRP; on this S9 they do
 *                  not: after every reset by this watchdog (S-Boot: "WDT_RST2 (0x800000)") its log reads "INF2: 0 /
 *                  INF3: 0", what was written here gone, and BOOT is started, the run again (only keys held send it to
 *                  TWRP). After the PMU's software reset (SWRESET, "SW_RST (0x20000000)") they are kept. So INFORM2/3 are
 *                  written here (recovery_after_reset()) before the watchdog is armed, and again right before the
 *                  loader's own restart, which is SWRESET (platform_restart()); the watchdog is the last resort, for a
 *                  hang of this core too, and that lands in BOOT. recovery=0: not written, and a restart is BOOT again
 * UEFI's page tables (devices/starlte/uefi, MemoryMapLib) do not map the watchdogs: their pages are mapped through the
 * CPU protocol's SetMemoryAttributes as device memory, and touched only if the processor then translates them (AT
 * S1E1R; Carbon at EL2 has no stage 2) to themselves as device memory.
 * Boot-arg:
 *   wdt=0  the Galaxy S9's hardware watchdog is not armed (a restart still goes to recovery: INFORM3 is written)
 */
#define EXYNOS_WTCON            0x0
#define EXYNOS_WTDAT            0x4
#define EXYNOS_WTCNT            0x8
#define EXYNOS_WTCLRINT         0xC
#define EXYNOS_WTCON_ARMED      0xFF39u         /* prescaler 255, divider 128, on, the reset; no interrupt */
#define EXYNOS_WTCON_RUN        0x21u           /* on (bit 5) and the reset (bit 0) */
#define EXYNOS_WDT_MILLIHZ      793457ull       /* 26 MHz / 256 / 128, in thousandths of a hertz */
#define EXYNOS_WDT_MS           30000u          /* while the loader runs; wdt_set_ms() says how long around the kernel */
#define EXYNOS_PMU_RST_STAT     0x404
#define EXYNOS_PMU_WDT_DISABLE  0x408
#define EXYNOS_PMU_WDT_MASK     0x40C
#define EXYNOS_PMU_INFORM2      0x808
#define EXYNOS_PMU_INFORM3      0x80C

static int exynos_wdt_on;               /* armed, and fed by wdt_pet() */
static int exynos_wdt_paused;           /* stopped while a kernel runs on this core; wdt_set_ms() arms it again */
static int starts_with(const char *s, const char *prefix);

static inline uint32_t
exynos_rd(uint64_t at)
{
	return *(volatile uint32_t *)(uintptr_t)at;
}

static inline void
exynos_wr(uint64_t at, uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)at = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* 1: the watchdog is this block's to feed (armed, or stopped for a while), not the hypervisor's */
static int
exynos_wdt_owns(void)
{
	return exynos_wdt_on || exynos_wdt_paused;
}

/* the page at `pa` as the processor translates it now: 1 if to itself, as device memory */
static int
exynos_page_ok(uint64_t pa)
{
	uint64_t par;

	__asm__ volatile ("at s1e1r, %0\nisb" : : "r" (pa) : "memory");
	par = READ_SYSREG(PAR_EL1);
	/* F (bit 0) clear; the address [47:12]; the attributes [63:56] of device memory, 0b0000xx00 */
	return !(par & 1) && (par & 0x0000FFFFFFFFF000ull) == (pa & ~0xFFFull) && (par >> 60) == 0;
}

static int
exynos_page_map(uint64_t pa)
{
	static const efi_guid cpu_guid = EFI_CPU_ARCH_GUID;
	efi_cpu_arch *cpu;
	efi_status s;

	if (exynos_page_ok(pa)) {
		return 1;
	}
	if (EFI_ERROR(BS->locate_protocol(&cpu_guid, NULL, (void **)&cpu)) || cpu->set_memory_attributes == NULL) {
		print("watchdog: %x is not mapped, and there is no CPU protocol to map it\n", pa);
		return 0;
	}
	s = cpu->set_memory_attributes(cpu, pa & ~0xFFFull, 0x1000, EFI_MEMORY_UC);
	print("watchdog: %x mapped as device memory by the firmware: status %x, %s\n", pa, (uint64_t)s,
	    exynos_page_ok(pa) ? "it translates" : "IT STILL DOES NOT TRANSLATE");
	return exynos_page_ok(pa);
}

static uint32_t
exynos_wdt_counts(uint64_t ms)
{
	uint64_t n = ms * EXYNOS_WDT_MILLIHZ / 1000000;

	return n == 0 ? 1 : n > 0xFFFF ? 0xFFFF : (uint32_t)n;
}

/* fed: its count back to the full time */
static void
exynos_wdt_feed(void)
{
	if (exynos_wdt_on) {
		exynos_wr(PLAT->exynos_wdt + EXYNOS_WTCNT, exynos_wdt_count);
	}
}

/* the time to the reset, from now; a watchdog stopped for a kernel on this core is armed again */
static void
exynos_wdt_set_ms(uint64_t ms)
{
	uint64_t wdt = PLAT->exynos_wdt;

	exynos_wdt_count = exynos_wdt_counts(ms);
	exynos_wr(wdt + EXYNOS_WTDAT, exynos_wdt_count);
	exynos_wr(wdt + EXYNOS_WTCNT, exynos_wdt_count);
	if (exynos_wdt_paused) {
		exynos_wr(wdt + EXYNOS_WTCON, EXYNOS_WTCON_ARMED);
		exynos_wdt_paused = 0;
		exynos_wdt_on = 1;
		print("watchdog: armed again, %d s\n", ms / 1000);
	}
}

/* stopped, for as long as nothing can feed it (a kernel on this core) */
static void
exynos_wdt_pause(const char *why)
{
	if (!exynos_wdt_on) {
		return;
	}
	exynos_wr(PLAT->exynos_wdt + EXYNOS_WTCON, exynos_rd(PLAT->exynos_wdt + EXYNOS_WTCON) & ~EXYNOS_WTCON_RUN);
	exynos_wdt_on = 0;
	exynos_wdt_paused = 1;
	print("watchdog: stopped: %s\n", why);
}

/*
 * From efi_main(), once the payload's command line is known: INFORM2/3 for recovery (unless recovery=0), then the
 * watchdog armed for EXYNOS_WDT_MS (unless wdt=0), the other cluster's stopped, and its reset unmasked in the PMU.
 */
static void
exynos_wdt_start(const char *cmdline)
{
	uint64_t pmu = PLAT->recovery_pmu, wdt = PLAT->exynos_wdt, other = PLAT->exynos_wdt_other;
	uint32_t bit = 1u << PLAT->exynos_wdt_pmu_bit, rst;
	int wanted = 1;

	if (wdt == 0 || pmu == 0) {
		return;
	}
	for (const char *c = cmdline; c != NULL && *c; c++) {
		if (c != cmdline && c[-1] != ' ') {
			continue;
		}
		if (starts_with(c, "recovery=0") && (c[10] == 0 || c[10] == ' ')) {
			reset_to_recovery = 0;
		}
		if (starts_with(c, "wdt=0") && (c[5] == 0 || c[5] == ' ')) {
			wanted = 0;
		}
	}
	/* first: a restart from here on goes to recovery, the watchdog's included */
	recovery_after_reset();
	rst = exynos_rd(pmu + EXYNOS_PMU_RST_STAT);
	print("watchdog: INFORM2 %x, INFORM3 %x: %s; the last reset: RST_STAT %x%s\n",
	    (uint64_t)exynos_rd(pmu + EXYNOS_PMU_INFORM2), (uint64_t)exynos_rd(pmu + EXYNOS_PMU_INFORM3),
	    reset_to_recovery ? "a restart goes to recovery" : "recovery=0, a restart comes back here", (uint64_t)rst,
	    (rst & bit) ? " (this watchdog's: S-Boot found INFORM2/3 cleared by it, and started BOOT unless keys were held)" :
	    (rst & 0x20000000u) ? " (a software reset)" : "");
	print("watchdog: PMU DISABLE_WDT %x, MASK_WDT_RESET_REQUEST %x as found\n",
	    (uint64_t)exynos_rd(pmu + EXYNOS_PMU_WDT_DISABLE), (uint64_t)exynos_rd(pmu + EXYNOS_PMU_WDT_MASK));
	if (!wanted) {
		print("watchdog: wdt=0: not armed (the boot's watchdogs left as they are, their resets masked)\n");
		return;
	}
	if (!exynos_page_map(wdt) || !exynos_page_map(other)) {
		print("watchdog: its registers cannot be reached: NOT ARMED\n");
		return;
	}
	print("watchdog: %x WTCON %x WTDAT %x WTCNT %x, the other cluster's (%x) WTCON %x, as found\n", wdt,
	    (uint64_t)exynos_rd(wdt + EXYNOS_WTCON), (uint64_t)exynos_rd(wdt + EXYNOS_WTDAT),
	    (uint64_t)exynos_rd(wdt + EXYNOS_WTCNT), other, (uint64_t)exynos_rd(other + EXYNOS_WTCON));
	exynos_wr(other + EXYNOS_WTCON, 0);
	exynos_wdt_count = exynos_wdt_counts(EXYNOS_WDT_MS);
	exynos_wr(wdt + EXYNOS_WTCON, exynos_rd(wdt + EXYNOS_WTCON) & ~EXYNOS_WTCON_RUN);
	exynos_wr(wdt + EXYNOS_WTDAT, exynos_wdt_count);
	exynos_wr(wdt + EXYNOS_WTCNT, exynos_wdt_count);
	exynos_wr(wdt + EXYNOS_WTCLRINT, 1);
	exynos_wr(wdt + EXYNOS_WTCON, EXYNOS_WTCON_ARMED);
	/* then its reset unmasked, and the PMU kept from stopping it */
	exynos_wr(pmu + EXYNOS_PMU_WDT_DISABLE, exynos_rd(pmu + EXYNOS_PMU_WDT_DISABLE) & ~bit);
	exynos_wr(pmu + EXYNOS_PMU_WDT_MASK, exynos_rd(pmu + EXYNOS_PMU_WDT_MASK) & ~bit);
	exynos_wdt_on = 1;
	wdt_armed = 1;
	print("watchdog: armed, %d s (WTCON %x, WTDAT %x); DISABLE_WDT %x, MASK_WDT_RESET_REQUEST %x; the other's WTCON %x\n",
	    (uint64_t)(EXYNOS_WDT_MS / 1000), (uint64_t)exynos_rd(wdt + EXYNOS_WTCON), (uint64_t)exynos_rd(wdt + EXYNOS_WTDAT),
	    (uint64_t)exynos_rd(pmu + EXYNOS_PMU_WDT_DISABLE), (uint64_t)exynos_rd(pmu + EXYNOS_PMU_WDT_MASK),
	    (uint64_t)exynos_rd(other + EXYNOS_WTCON));
}
