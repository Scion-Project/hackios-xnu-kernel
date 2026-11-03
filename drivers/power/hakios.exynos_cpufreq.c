/*
 * The Galaxy S9's CPU frequency governor: the A55s' (cpucl0) and the M3s' (cpucl1) rates moved with the load, inside
 * the limits Samsung's own kernel keeps, and lowered when the TMU says the cores are hot. Only with cpufreq_gov=1; without
 * it the fixed rates of cpufreq_lit= and cpufreq_big= stay (drivers/core/hakios.exynos_acpm.c), as before.
 *
 * How a rate is set. A DVFS request to the APM's ACPM firmware names a domain (2 cpucl0, 3 cpucl1) and a rate in kHz,
 * nothing else (hakios_acpm_dvfs_set; Samsung's cal_dfs_set_rate -> exynos_acpm_set_rate sends the same three words).
 * The voltage is the APM's: it keeps a table of rate and voltage per level ("fvmap", the ASV voltages already in it) and
 * applies the voltage with the rate. Samsung's kernel never sends a voltage for the CPU clusters; it reads the table
 * once (cal-if/fvmap.c) and adds a margin to it only when its command line asks. Neither does this file: it reads the
 * table, logs it, asks only for rates that are in it, and reads each rate back.
 *
 * The levels. From the APM's table when it can be read (hakios_acpm_dvfs_table), else the device tree's (the dm
 * constraints' mif-perf tables of Samsung's tree and the phone's own, which list every level of each cluster):
 *   cpucl0  1950000 1794000 1690000 1456000 1248000 1053000 949000 832000 715000 598000 455000
 *   cpucl1  2886000 2860000 2704000 2652000 2496000 2314000 2106000 2002000 1924000 1794000 1690000 1586000 1469000
 *           1261000 1170000 1066000 962000 858000 741000 650000 598000
 *
 * The limits, as Samsung's kernel keeps them on this phone:
 *   cpucl0  min-freq 455000, max-freq 1950000 (Samsung's tree; the phone's own has no max-freq: the CAL's, the table's top)
 *   cpucl1  min-freq 650000; max-freq 2314000 and policy-max 1794000 in Samsung's tree, max-freq 1794000 in the phone's
 *           own; and by how many M3s are online (the hotplug governor's single/dual/triple/quad_freq, of which the
 *           driver takes the triple's for three): 1 or 2 online 2314000, 3 or 4 online 1794000
 * So stock Android on this phone never runs the M3s above 1794000 with all four online, and above 2314000 never at all
 * (2704000, the S9's advertised top, and 2886000 are in the table and nowhere allowed). The ceiling here: for the M3s
 * the online count's limit, and by default no more than 1794000 in any case (the phone's tree's max-freq;
 * cpufreq_gov_big_max= may raise it as far as the online count's limit, never further); for the A55s 1794000 by default
 * (cpufreq_gov_lit_max= up to 1950000). The floors: the M3s' min-freq; the A55s' 1053000 by default, not 455000, because
 * core 0, which runs the loader's UFS service, is an A55 the scheduler does not see: its load is not counted here.
 *
 * The heat, as Samsung's thermal zones treat it: BIG cooled from 83 C (big-control-temp, the power allocator, polled
 * every 50 ms once there) and its cores taken away at 96 (hotplug_out_threshold, back at 91); LITTLE's first frequency
 * trip at 76 (little-alert1); "hot" at 115 for both. The frequencies of each trip are in the bootloader's ECT, not in the
 * source, so this file has its own, in the same places: every second, a zone at its control point (83 BIG, 76 LITTLE)
 * or above loses one level of its cluster's ceiling, and gets one back once 5 degrees under it (the trips' hysteresis);
 * at 96 or above the cluster goes to its lowest level at once (the slowest at or above min-freq: never under what
 * Samsung's kernel uses). The temperatures are the TMU driver's, asked of it directly (hakios_exynos_tmu_zone_mc(): its
 * last reading of the BIG and LITTLE zones, with tmu=1 or more). They were read through kern.hakios.temp.big_mc and
 * little_mc (kernel_sysctlbyname) at first, which on the phone gave nothing ("temp big - lit - C" all through main-1102
 * while the TMU logged 90 C and more); the sysctl's answer is still logged once, beside the direct reading. Without
 * temperatures (no tmu=, or not read yet) the M3s stay at or below 1794000 whatever the online count. tmu=3 ends the run
 * at 110 C as before; this changes nothing there.
 *
 * The load. Every cpufreq_gov_ms (40 ms by default, 20 to 200) each core's idle time from the scheduler
 * (ml_cpu_idle_time, as cpuload= reads it); a cluster's load is its busiest core's share of the window it was not idle.
 * The rate wanted is the one at which that core would be 60% busy (the current rate times load / 60; gov_target=), rounded
 * up to a level. Up at once, and a core 90% busy or more (its real need unknown: it had no time left to show it) goes at
 * least to the cluster's touch-boost rate in one step (Android's interactive governor's hispeed_freq); down only after
 * five windows in a row want less (about 200 ms; gov_down=). It was 75% and three windows: on the phone the
 * M3s sat at 650 MHz with 20 to 40% load, and the display's presenter took 4.2 ms to copy a frame instead of 1.6 ms.
 *
 * The boosts. What the load cannot see coming, raised before the load shows it:
 *   touch  every finger event of the touch screen (hakios.s6sy761.c, its polling thread, hakios_exynos_cpufreq_touch())
 *          holds the M3s at 1469000 or more and the A55s at 1690000 or more until gov_boost_ms (300) after the last one;
 *   frames while the display's presenter is given frames (MiniIOMobileFramebuffer.cpp present(),
 *          hakios_exynos_cpufreq_frame()), a frame in the last 100 ms: the M3s at 1261000 or more (gov_frame_big=).
 * The caller only stamps the time, counts, and wakes this file's thread (thread_wakeup) when no boost of its kind was
 * running; it never asks the APM itself, never waits, never takes a lock. The thread, woken between its windows, raises
 * whichever cluster is under its boost at once, then goes back to waiting out the window; the load is measured over the
 * whole window as before. A boost is a floor only: it is clamped by everything that clamps the load's rate -- the
 * ceiling, the online count's limit, 1794000 without temperatures, the thermal ceiling, the lowest level at 96 C -- and
 * when it ends the rate comes down as the load's would (the down windows).
 * A core that has never been idle
 * since start is taken as not started (cores 0 and 1 are not the kernel's at all): it counts neither for the load nor as
 * online. Every change is read back from the APM (hakios_acpm_dvfs_get); a rate read back above the ceiling is logged
 * and the next level down asked for. The APM of the S9 answers FREQ_GET with 0 for both CPU domains (main-1102 and the
 * fixed-rate runs before it: "dvfs: the M3s (cpucl1): 0 kHz (0) -> asked 2106000 kHz (0) -> now 0 kHz (0)"; Samsung's
 * own cpufreq driver reads its rate from the CAL's cache, never from the APM). Taken as a rate, that 0 stopped the
 * governor dead: decide() skips a cluster whose rate is 0, so nothing after the first request was ever asked for, the
 * thermal ceilings included. A 0 is therefore "not said": the rate asked for is the rate, the APM's answer to the
 * request itself is logged (its four words, for the first changes), and the rate the cores really run at is measured
 * once a log line (measure_mhz(): a chain of dependent additions, one a cycle on both the A55 and the M3, timed on a
 * core of each cluster with interrupts off for a fraction of a millisecond).
 *
 * The log: at start the table, the limits and the rates found; then once a second (cpufreq_gov_log=SECONDS; 0: none)
 *   cpufreq_gov: 12 s: lit 1248 MHz load 31% (avg 22%, 2 online, 1794 max, measured 1251) | big 1586 MHz load 64% (avg
 *       58%, 4 online, 1794 max, measured 1590) | temp big 52.1 lit 48.0 C | 9 changes | boost: 41 touch, 58 frames, 2
 *       raised, touch on
 * (a rate is "asked" when the APM does not say it, "?" measured when no core of the cluster could be measured; the boost
 * part: the touch events and frames of that second, how many times a boost raised a cluster, which boosts hold now --
 * "touch", "frames", both, or "off" -- so boosts are said once a second at most, never one line each)
 * to the loader's log (loader_log_line), which printf does not reach.
 *
 * Status: the load governor run on the phone (ACPM takes the requests, the measured MHz match them, the temperatures are
 * read), with the touch and frame boosts, the 60% target and the five down windows (kernel PR 158): the S9's default,
 * cpufreq_gov=1.
 *
 * Boot-args:
 *   cpufreq_gov=1            run it (0, or none: the fixed rates of cpufreq_lit= and cpufreq_big=)
 *   cpufreq_gov_ms=<ms>      the load window (20 to 200; 40)
 *   cpufreq_gov_log=<s>      a summary line this often (0: none; 1)
 *   cpufreq_gov_lit_max=<kHz>, cpufreq_gov_big_max=<kHz>   the ceilings (at most 1950000 and the online count's limit)
 *   cpufreq_gov_lit_min=<kHz>, cpufreq_gov_big_min=<kHz>   the floors (at least 455000 and 650000)
 *   gov_boost_big=<kHz>      the M3s' floor while touched (1469000; 0: no touch boost for them)
 *   gov_boost_lit=<kHz>      the A55s' floor while touched (1690000; 0: none)
 *   gov_boost_ms=<ms>        how long the touch boost holds after the last finger event (300; 0: no touch boost)
 *   gov_frame_big=<kHz>      the M3s' floor while frames come (1261000; 0: no frame boost)
 *   gov_frame_ms=<ms>        a frame this recent keeps the frame boost on (100; 20 to 1000)
 *   gov_target=<percent>     the busiest core's share wanted at the new rate (60; 30 to 95)
 *   gov_down=<windows>       windows in a row that want less before the rate comes down (5; 1 to 50)
 *
 * Sources (Samsung's kernel for the S9, Scion-Project/android_kernel_samsung_exynos9810, lineage-24.0 at e5906667068e,
 * and Silicium's Resources/DTBs/starlte.dts, the phone's own tree): arch/arm64/boot/dts/exynos/
 * exynos9810-starlte_eur_open_26.dts (cpufreq { domain@0, domain@1 }: min-freq, max-freq, policy-max, the mif-perf
 * tables; hotplug_governor: single/dual/triple/quad_freq; thermal-zones BIG and LITTLE; BIG@10070000's hotplug
 * thresholds; acpm's fvmap_offset), drivers/cpufreq/exynos-acme.c (init_domain: max-freq over the CAL's; init_table: the
 * levels outside the limits left out; set_policy), drivers/soc/samsung/exynos-hotplug_governor.c (get_hpgov_maxfreq: the
 * limit per online count), drivers/soc/samsung/cal-if/cal-if.c (cal_dfs_set_rate), acpm_dvfs.c (exynos_acpm_set_rate:
 * domain, rate, FREQ_REQ), fvmap.c and fvmap.h (the table and its layout), exynos9810/acpm_dvfs_exynos9810.h (the
 * indexes), drivers/soc/samsung/acpm/acpm.c (plugins_init) and fw_header/ (the plugin and initdata layouts). All GPL:
 * read for these facts, not one line copied.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/errno.h>

extern void loader_log_line(const char *line);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
extern int kernel_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
/* the TMU driver's last reading of a zone (0 BIG, 1 LITTLE), millidegrees; -273000 before one or without the driver */
extern int32_t hakios_exynos_tmu_zone_mc(unsigned zone);
extern void *cpu_to_processor(int cpu);
extern void *thread_bind(void *processor);
extern int thread_block(void *continuation);
/* (kern/sched_prim.h, not included: its thread_block and thread_bind are declared above as this file has used them) */
extern wait_result_t assert_wait_deadline(event_t event, wait_interrupt_t interruptible, uint64_t deadline);
extern kern_return_t thread_wakeup_prim(event_t event, boolean_t one_thread, wait_result_t result);

#define LEVELS_MOST     32
#define TARGET_LOAD     60      /* percent: the busiest core's share wanted at the new rate (gov_target=) */
#define DOWN_WINDOWS    5       /* windows in a row that want less before the rate comes down (gov_down=) */
#define SATURATED       90      /* percent: a core this busy goes at least to its cluster's touch-boost rate at once */
#define NOT_READ_MC     (-273000)
#define HOT_CUT_MC      96000   /* BIG's hotplug_out_threshold: the cluster to its lowest level */
#define HYSTERESIS_MC   5000
#define NO_TEMP_BIG_MAX 1794000 /* without temperatures, the M3s no faster than this */
#define ANSWERS_LOGGED  8       /* changes whose APM answer is logged, per cluster */
#define SPIN_LOOPS      8192    /* measure_mhz(): 32 additions each, 262144 cycles (0.1 to 0.6 ms) */

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[320];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

static struct cluster {
	const char *name;               /* "lit", "big" */
	const char *zone;               /* the TMU's sysctl (logged once, beside the direct reading) */
	unsigned tmu_zone;              /* the TMU driver's zone: 0 BIG, 1 LITTLE */
	uint32_t index;                 /* the ACPM DVFS domain */
	uint32_t affinity;              /* MPIDR [15:8] of its cores */
	int32_t control_mc;             /* where Samsung starts to cool it */
	uint32_t dt_min, dt_max;        /* the device tree's limits (see the head) */
	const uint32_t *dt_levels;
	uint32_t n_dt_levels;
	uint32_t levels[LEVELS_MOST];   /* the fastest first */
	uint32_t n;
	uint32_t floor, ceiling;        /* kHz: boot-args within the device tree's limits */
	uint32_t thermal;               /* the thermal ceiling, a level index (0: none) */
	uint32_t cur;                   /* kHz, as read back, or as asked when the APM says 0 */
	bool asked;                     /* cur is the rate asked for: the APM's FREQ_GET said 0 */
	uint32_t measured;              /* MHz, measure_mhz()'s last (0: not measured) */
	uint32_t load, online, low_windows;
	uint32_t load_sum, windows;     /* this second's */
	uint32_t lowest;                /* the slowest level at or above the device tree's min-freq: as low as the heat takes it */
	uint32_t changes, errors;
	int32_t temp_mc;
	uint32_t touch_khz, frame_khz;  /* the boosts' floors (0: none), before the ceiling clamps them */
	uint32_t boost_floor;           /* the last boost_floor() (kHz; 0: no boost on) */
} clusters[2];

static const uint32_t lit_dt[] = {
	1950000, 1794000, 1690000, 1456000, 1248000, 1053000, 949000, 832000, 715000, 598000, 455000,
};
static const uint32_t big_dt[] = {
	2886000, 2860000, 2704000, 2652000, 2496000, 2314000, 2106000, 2002000, 1924000, 1794000, 1690000, 1586000, 1469000,
	1261000, 1170000, 1066000, 962000, 858000, 741000, 650000, 598000,
};

static uint32_t window_ms = 40, log_seconds = 1, target_load = TARGET_LOAD, down_windows = DOWN_WINDOWS;

/*
 * The boosts (see the head). The stamps and counts are written by the touch screen's thread and the display's presenter,
 * read by this file's thread: plain 64- and 32-bit atomics, no lock. boost_event is what the thread waits on.
 */
static uint32_t boost_ms = 300, frame_ms = 100;
static uint64_t boost_span, frame_span;         /* the same in mach_absolute_time units */
static uint64_t touch_last, frame_last;         /* the last finger event's and frame's time (0: none yet) */
static uint32_t touch_count, frame_count;       /* this second's, for the summary */
static uint32_t boost_raises;                   /* this second's: times a boost raised a cluster's rate */
static bool gov_running;
static int boost_event;

static bool
boost_on(uint64_t last, uint64_t span, uint64_t now)
{
	return last != 0 && span != 0 && now - last <= span;
}

/* a finger event (hakios.s6sy761.c): the stamp, and the thread woken when the touch boost was not already on */
void
hakios_exynos_cpufreq_touch(void)
{
	uint64_t now, before;

	if (!__atomic_load_n(&gov_running, __ATOMIC_ACQUIRE) || boost_span == 0) {
		return;
	}
	now = mach_absolute_time();
	before = __atomic_exchange_n(&touch_last, now, __ATOMIC_RELEASE);
	__atomic_fetch_add(&touch_count, 1, __ATOMIC_RELAXED);
	if (!boost_on(before, boost_span, now)) {
		(void)thread_wakeup_prim((event_t)&boost_event, FALSE, THREAD_AWAKENED);
	}
}

/* a frame given to the display (MiniIOMobileFramebuffer.cpp present()): the same, for the frame boost */
void
hakios_exynos_cpufreq_frame(void)
{
	uint64_t now, before;

	if (!__atomic_load_n(&gov_running, __ATOMIC_ACQUIRE) || frame_span == 0) {
		return;
	}
	now = mach_absolute_time();
	before = __atomic_exchange_n(&frame_last, now, __ATOMIC_RELEASE);
	__atomic_fetch_add(&frame_count, 1, __ATOMIC_RELAXED);
	if (!boost_on(before, frame_span, now)) {
		(void)thread_wakeup_prim((event_t)&boost_event, FALSE, THREAD_AWAKENED);
	}
}

/*
 * kern.hakios.gov_frame_big: the M3s' floor while frames come (gov_frame_big=), changed while the system runs (kHz; 0:
 * no frame boost), for trying one rate against another in one run: the USB console's "sysctl kern.hakios.gov_frame_big=N".
 */
SYSCTL_DECL(_kern_hakios);
static int
frame_big_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int khz = (int)clusters[1].frame_khz, error;

	error = sysctl_io_number(req, khz, sizeof(khz), &khz, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	if (khz < 0 || khz > 3000000) {
		return EINVAL;
	}
	if (khz != 0 && frame_span == 0) {
		frame_ms = 100;
		nanoseconds_to_absolutetime((uint64_t)frame_ms * NSEC_PER_MSEC, &frame_span);
	}
	clusters[1].frame_khz = (uint32_t)khz;
	say("cpufreq_gov: the M3s' frame boost now %d kHz (kern.hakios.gov_frame_big)\n", khz);
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, gov_frame_big, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    frame_big_sysctl, "I", "the M3s' floor while frames come, kHz (gov_frame_big=; 0: none)");

static uint64_t idle_before[MAX_CPUS];
static bool ever_idle[MAX_CPUS];

static bool
ever_idle_cpu(unsigned int i)
{
	return i < MAX_CPUS && ever_idle[i];
}

/* the M3s' limit for this many online (exynos-hotplug_governor.c get_hpgov_maxfreq with the device tree's values) */
static uint32_t
big_online_limit(uint32_t online)
{
	return online <= 2 ? 2314000 : 1794000;
}

/* the slowest level at or above khz (the fastest when none is) */
static uint32_t
level_at_least(const struct cluster *c, uint64_t khz)
{
	for (uint32_t l = c->n; l-- > 0;) {
		if (c->levels[l] >= khz) {
			return l;
		}
	}
	return 0;
}

/* the fastest level at or below khz (the slowest when none is) */
static uint32_t
level_at_most(const struct cluster *c, uint32_t khz)
{
	for (uint32_t l = 0; l < c->n; l++) {
		if (c->levels[l] <= khz) {
			return l;
		}
	}
	return c->n - 1;
}

static uint32_t
ceiling_now(struct cluster *c)
{
	uint32_t max = c->ceiling;

	if (c == &clusters[1]) {
		max = MIN(max, big_online_limit(c->online));
		if (c->temp_mc == NOT_READ_MC) {
			max = MIN(max, NO_TEMP_BIG_MAX);
		}
	}
	if (c->thermal > 0) {
		max = MIN(max, c->levels[MIN(c->thermal, c->n - 1)]);
	}
	return c->levels[level_at_most(c, max)];
}

/* the floor the boosts on now give the cluster, a level, clamped by the ceiling now (0: no boost on) */
static uint32_t
boost_floor(struct cluster *c, uint64_t now)
{
	uint32_t khz = 0;

	if (boost_on(__atomic_load_n(&touch_last, __ATOMIC_ACQUIRE), boost_span, now)) {
		khz = MAX(khz, c->touch_khz);
	}
	if (boost_on(__atomic_load_n(&frame_last, __ATOMIC_ACQUIRE), frame_span, now)) {
		khz = MAX(khz, c->frame_khz);
	}
	if (khz == 0) {
		return 0;
	}
	return MIN(c->levels[level_at_least(c, khz)], ceiling_now(c));
}

static void set_rate(struct cluster *c, uint32_t khz, uint32_t max);

/* the clusters under their boost floor raised to it now (between windows; nothing measured) */
static void
boost(uint64_t now)
{
	for (int i = 0; i < 2; i++) {
		struct cluster *c = &clusters[i];
		uint32_t floor = boost_floor(c, now);

		c->boost_floor = floor;
		if (c->cur != 0 && c->online != 0 && floor > c->cur) {
			set_rate(c, floor, ceiling_now(c));
			c->low_windows = 0;
			__atomic_fetch_add(&boost_raises, 1, __ATOMIC_RELAXED);
		}
	}
}

/* a boost wanted that is not applied yet: the thread does not wait out its window then */
static bool
boost_due(uint64_t now)
{
	for (int i = 0; i < 2; i++) {
		struct cluster *c = &clusters[i];

		if (c->cur != 0 && c->online != 0 && boost_floor(c, now) > c->cur) {
			return true;
		}
	}
	return false;
}

/*
 * One DVFS request, as hakios_acpm_dvfs_set() sends it (domain, kHz, FREQ_REQ 0, 0), with the APM's answer kept: its four
 * words come back in place of the request's.
 */
static int
dvfs_request(uint32_t index, uint32_t khz, uint32_t answer[4])
{
	answer[0] = index;
	answer[1] = khz;
	answer[2] = 0;
	answer[3] = 0;
	return hakios_acpm_request(HAKIOS_ACPM_CH_DVFS, answer);
}

/* the rate the APM says (FREQ_GET); 0 is "not said": the rate asked for stands */
static void
read_back(struct cluster *c, uint32_t asked)
{
	uint32_t got = 0;
	int e = hakios_acpm_dvfs_get(c->index, &got);

	if (e == 0 && got != 0) {
		c->cur = got;
		c->asked = false;
		return;
	}
	if (!c->asked) {
		say("cpufreq_gov: %s: the APM answers FREQ_GET with %u kHz (%d): the rate asked for, %u kHz, is taken as the "
		    "rate (and measured once a log line)\n", c->name, got, e, asked);
	}
	c->cur = asked;
	c->asked = true;
}

static void
set_rate(struct cluster *c, uint32_t khz, uint32_t max)
{
	uint32_t answer[4];
	int e = dvfs_request(c->index, khz, answer);

	if (e != 0) {
		if (c->errors++ < 5) {
			say("cpufreq_gov: %s: asked %u kHz: the APM did not answer (%d); the rate stays %u kHz\n", c->name, khz, e,
			    c->cur);
		}
		return;
	}
	if (c->changes < ANSWERS_LOGGED) {
		say("cpufreq_gov: %s: %u -> %u kHz asked; the APM answered %08x %08x %08x %08x\n", c->name, c->cur, khz,
		    answer[0], answer[1], answer[2], answer[3]);
	}
	c->changes++;
	read_back(c, khz);
	if (!c->asked && c->cur > max) {
		uint32_t lower = level_at_most(c, khz);

		lower = lower + 1 < c->n ? lower + 1 : lower;
		say("cpufreq_gov: %s: asked %u kHz, the APM says %u, above %u: asking %u\n", c->name, khz, c->cur, max,
		    c->levels[lower]);
		if (dvfs_request(c->index, c->levels[lower], answer) == 0) {
			read_back(c, c->levels[lower]);
		}
	}
}

/*
 * The rate a core of the cluster really runs at, in MHz: this thread moved to the first of its cores the scheduler has
 * (one that has been idle: cores 0 and 1 are not the kernel's), and a chain of 32 dependent additions run SPIN_LOOPS
 * times with interrupts off -- one addition a cycle on both the A55 and the M3, the loop's own subtraction and branch
 * beside them -- timed with mach_absolute_time. 0 when there is no such core. Back on any core afterwards.
 */
static bool ever_idle_cpu(unsigned int i);

static uint32_t
measure_mhz(const struct cluster *c)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t loops = SPIN_LOOPS, x = 0, began, took, ns;
	boolean_t enabled;
	int cpu = -1;

	for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
		if (((topology->cpus[i].phys_id >> 8) & 0xff) == c->affinity && ever_idle_cpu(i)) {
			cpu = (int)topology->cpus[i].cpu_id;
			break;
		}
	}
	if (cpu < 0 || cpu_to_processor(cpu) == NULL) {
		return 0;
	}
	(void)thread_bind(cpu_to_processor(cpu));
	(void)thread_block(NULL);             /* over to it now */
	enabled = ml_set_interrupts_enabled(FALSE);
	began = mach_absolute_time();
	__asm__ volatile (
                "1:\n"
                ".rept 32\n"
                "add %0, %0, #1\n"
                ".endr\n"
                "subs %1, %1, #1\n"
                "b.ne 1b\n"
                : "+r" (x), "+r" (loops) : : "cc");
	took = mach_absolute_time() - began;
	(void)ml_set_interrupts_enabled(enabled);
	(void)thread_bind(NULL);
	absolutetime_to_nanoseconds(took, &ns);
	return ns != 0 && x == 32ull * SPIN_LOOPS ? (uint32_t)(32ull * SPIN_LOOPS * 1000 / ns) : 0;
}

/* the cluster's busiest core's load over the window, and how many of its cores run */
static void
measure(uint64_t span)
{
	const ml_topology_info_t *topology = ml_get_topology_info();

	clusters[0].load = clusters[1].load = 0;
	clusters[0].online = clusters[1].online = 0;
	for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
		uint32_t aff = (topology->cpus[i].phys_id >> 8) & 0xff;
		uint64_t idle = 0, busy;
		struct cluster *c;

		if (!ml_cpu_idle_time((int)i, &idle)) {
			continue;
		}
		busy = span > idle - idle_before[i] ? span - (idle - idle_before[i]) : 0;
		idle_before[i] = idle;
		if (idle != 0) {
			ever_idle[i] = true;
		}
		c = aff == clusters[1].affinity ? &clusters[1] : (aff == clusters[0].affinity ? &clusters[0] : NULL);
		if (c == NULL || !ever_idle[i]) {
			continue;
		}
		c->online++;
		c->load = MAX(c->load, (uint32_t)(100 * busy / span));
	}
}

static void
decide(struct cluster *c, uint64_t now)
{
	uint32_t max = ceiling_now(c), want, target;

	c->load_sum += c->load;
	c->windows++;
	c->boost_floor = boost_floor(c, now);
	if (c->online == 0 || c->cur == 0) {
		return;
	}
	want = c->levels[level_at_least(c, (uint64_t)c->cur * c->load / target_load)];
	if (c->load >= SATURATED && c->touch_khz != 0) {
		/* no time left over to say how much more it needs: the touch-boost rate at once, then the load's again */
		want = MAX(want, c->levels[level_at_least(c, c->touch_khz)]);
	}
	target = MAX(want, c->floor);
	target = MAX(target, c->boost_floor);
	target = MIN(target, max);
	if (target < c->cur && c->cur <= max) {
		if (++c->low_windows < down_windows) {
			return;
		}
	}
	c->low_windows = 0;
	if (target != c->cur) {
		set_rate(c, target, max);
	}
}

/* the zone's temperature, from the TMU driver itself; the first time also what its sysctl says, for the log */
static int32_t
read_temp(struct cluster *c)
{
	static bool compared[2];
	int32_t mc = hakios_exynos_tmu_zone_mc(c->tmu_zone);

	if (!compared[c->tmu_zone] && mc != NOT_READ_MC) {
		int value = NOT_READ_MC;
		size_t size = sizeof(value);
		int e = kernel_sysctlbyname(c->zone, &value, &size, NULL, 0);

		compared[c->tmu_zone] = true;
		say("cpufreq_gov: %s: the TMU driver reads %d mC; its sysctl %s says %d (error %d)\n", c->name, mc, c->zone, value,
		    e);
	}
	return mc;
}

/* once a second: each zone's temperature moves its cluster's thermal ceiling by a level at most */
static void
cool(struct cluster *c)
{
	uint32_t before = c->thermal;

	c->temp_mc = read_temp(c);
	if (c->temp_mc == NOT_READ_MC) {
		c->thermal = 0;
	} else if (c->temp_mc >= HOT_CUT_MC) {
		c->thermal = c->lowest;
	} else if (c->temp_mc >= c->control_mc) {
		/* one level under where it runs now (or under the last thermal ceiling) */
		uint32_t from = c->thermal ? c->thermal : level_at_most(c, MIN(c->cur, ceiling_now(c)));

		c->thermal = MIN(from + 1, c->lowest);
	} else if (c->thermal > 0 && c->temp_mc < c->control_mc - HYSTERESIS_MC) {
		c->thermal = c->thermal > 1 && c->levels[c->thermal - 1] < c->ceiling ? c->thermal - 1 : 0;
	}
	if (c->thermal != before) {
		say("cpufreq_gov: %s at %d.%d C: its ceiling %s %u kHz\n", c->name, c->temp_mc / 1000,
		    (c->temp_mc < 0 ? -c->temp_mc : c->temp_mc) % 1000 / 100, c->thermal ? "now" : "back to",
		    c->thermal ? c->levels[c->thermal] : c->ceiling);
	}
}

static void
temp_text(char *out, size_t size, int32_t mc)
{
	if (mc == NOT_READ_MC) {
		snprintf(out, size, "-");
	} else {
		snprintf(out, size, "%d.%d", mc / 1000, (mc < 0 ? -mc : mc) % 1000 / 100);
	}
}

static void
summary(uint64_t seconds)
{
	struct cluster *l = &clusters[0], *b = &clusters[1];
	char tb[16], tl[16], ml[16], mb[16];
	uint32_t touches, frames, raises;
	uint64_t now;
	bool touched, framed;

	l->measured = measure_mhz(l);
	b->measured = measure_mhz(b);
	temp_text(tb, sizeof(tb), b->temp_mc);
	temp_text(tl, sizeof(tl), l->temp_mc);
	snprintf(ml, sizeof(ml), l->measured ? "%u" : "?", l->measured);
	snprintf(mb, sizeof(mb), b->measured ? "%u" : "?", b->measured);
	now = mach_absolute_time();
	touched = boost_on(__atomic_load_n(&touch_last, __ATOMIC_ACQUIRE), boost_span, now);
	framed = boost_on(__atomic_load_n(&frame_last, __ATOMIC_ACQUIRE), frame_span, now);
	touches = __atomic_exchange_n(&touch_count, 0, __ATOMIC_RELAXED);
	frames = __atomic_exchange_n(&frame_count, 0, __ATOMIC_RELAXED);
	raises = __atomic_exchange_n(&boost_raises, 0, __ATOMIC_RELAXED);
	say("cpufreq_gov: %llu s: lit %u MHz%s load %u%% (avg %u%%, %u online, %u max, measured %s) | big %u MHz%s load %u%% "
	    "(avg %u%%, %u online, %u max, measured %s) | temp big %s lit %s C | %u changes | boost: %u touch, %u frames, %u "
	    "raised, %s\n", seconds, l->cur / 1000,
	    l->asked ? " asked" : "", l->load, l->windows ? l->load_sum / l->windows : 0, l->online, ceiling_now(l) / 1000, ml,
	    b->cur / 1000, b->asked ? " asked" : "", b->load, b->windows ? b->load_sum / b->windows : 0, b->online,
	    ceiling_now(b) / 1000, mb, tb, tl, l->changes + b->changes, touches, frames, raises,
	    touched && framed ? "touch and frames on" : touched ? "touch on" : framed ? "frames on" : "off");
	l->load_sum = l->windows = b->load_sum = b->windows = 0;
}

static void
gov_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint64_t then = mach_absolute_time(), start = then, last_second = then, last_log = then, second, log_every;

	nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
	log_every = second * log_seconds;
	measure(1);     /* the idle times so far, and which cores have been idle: the first window starts here */
	for (;;) {
		uint64_t now, ns, window, deadline;
		bool tried = false;

		/*
		 * The window waited out, but woken by a boost (hakios_exynos_cpufreq_touch/frame) to raise a cluster at once. The
		 * wait is asserted before the check, so a wakeup between the two is not lost: it ends the wait at once.
		 */
		nanoseconds_to_absolutetime((uint64_t)window_ms * NSEC_PER_MSEC, &window);
		deadline = then + window;
		while ((now = mach_absolute_time()) < deadline) {
			(void)assert_wait_deadline((event_t)&boost_event, THREAD_UNINT, deadline);
			if (!tried && boost_due(now)) {
				(void)thread_wakeup_prim((event_t)&boost_event, FALSE, THREAD_AWAKENED);
			}
			(void)thread_block(NULL);
			now = mach_absolute_time();
			if (boost_due(now)) {
				/* (not woken by itself again this window: a request the APM refused waits for the next) */
				tried = true;
				boost(now);
			}
		}
		measure(now - then > 0 ? now - then : 1);
		then = now;
		if (now - last_second >= second) {
			last_second = now;
			cool(&clusters[0]);
			cool(&clusters[1]);
		}
		decide(&clusters[0], now);
		decide(&clusters[1], now);
		if (log_seconds != 0 && now - last_log >= log_every) {
			last_log = now;
			absolutetime_to_nanoseconds(now - start, &ns);
			summary(ns / NSEC_PER_SEC);
		}
	}
}

static void
cluster_setup(struct cluster *c, uint32_t asked_max, uint32_t asked_min, uint32_t max_default, uint32_t floor_default)
{
	uint32_t uv[LEVELS_MOST];
	char line[256];
	int at;

	c->n = hakios_acpm_dvfs_table(c->index, c->levels, uv, LEVELS_MOST, say);
	if (c->n > 0) {
		at = snprintf(line, sizeof(line), "cpufreq_gov: %s: the APM's table (DVFS %u), kHz/uV:", c->name, c->index);
		for (uint32_t l = 0; l < c->n && at < (int)sizeof(line) - 20; l++) {
			at += snprintf(line + at, sizeof(line) - at, " %u/%u", c->levels[l], uv[l]);
		}
		say("%s\n", line);
	} else {
		for (uint32_t l = 0; l < c->n_dt_levels; l++) {
			c->levels[l] = c->dt_levels[l];
		}
		c->n = c->n_dt_levels;
		say("cpufreq_gov: %s: the APM's table not read: the device tree's %u levels, %u to %u kHz\n", c->name, c->n,
		    c->levels[c->n - 1], c->levels[0]);
	}
	c->ceiling = MIN(asked_max ? asked_max : max_default, c->dt_max);
	c->floor = MAX(asked_min ? asked_min : floor_default, c->dt_min);
	c->ceiling = c->levels[level_at_most(c, c->ceiling)];
	c->lowest = level_at_least(c, c->dt_min);
	c->floor = MIN(c->levels[level_at_least(c, c->floor)], c->ceiling);
	c->temp_mc = NOT_READ_MC;
}

bool
hakios_exynos_cpufreq_gov_start(void)
{
	uint32_t on = 0, value = 0, lit_max = 0, lit_min = 0, big_max = 0, big_min = 0;
	uint32_t touch_big = 1469000, touch_lit = 1690000, frame_big = 1261000;
	thread_t thread;

	if (!PE_parse_boot_argn("cpufreq_gov", &on, sizeof(on)) || on == 0) {
		return false;
	}
	if (PE_parse_boot_argn("cpufreq_gov_ms", &value, sizeof(value)) && value >= 20 && value <= 200) {
		window_ms = value;
	}
	value = 1;
	if (PE_parse_boot_argn("cpufreq_gov_log", &value, sizeof(value)) && value <= 3600) {
		log_seconds = value;
	}
	(void)PE_parse_boot_argn("cpufreq_gov_lit_max", &lit_max, sizeof(lit_max));
	(void)PE_parse_boot_argn("cpufreq_gov_lit_min", &lit_min, sizeof(lit_min));
	(void)PE_parse_boot_argn("cpufreq_gov_big_max", &big_max, sizeof(big_max));
	(void)PE_parse_boot_argn("cpufreq_gov_big_min", &big_min, sizeof(big_min));
	(void)PE_parse_boot_argn("gov_boost_big", &touch_big, sizeof(touch_big));
	(void)PE_parse_boot_argn("gov_boost_lit", &touch_lit, sizeof(touch_lit));
	(void)PE_parse_boot_argn("gov_frame_big", &frame_big, sizeof(frame_big));
	if (PE_parse_boot_argn("gov_boost_ms", &value, sizeof(value)) && value <= 5000) {
		boost_ms = value;
	}
	if (PE_parse_boot_argn("gov_frame_ms", &value, sizeof(value)) && value >= 20 && value <= 1000) {
		frame_ms = value;
	}
	if (PE_parse_boot_argn("gov_target", &value, sizeof(value)) && value >= 30 && value <= 95) {
		target_load = value;
	}
	if (PE_parse_boot_argn("gov_down", &value, sizeof(value)) && value >= 1 && value <= 50) {
		down_windows = value;
	}
	if (boost_ms == 0) {
		touch_big = touch_lit = 0;
	}
	if (touch_big == 0 && touch_lit == 0) {
		boost_ms = 0;
	}
	if (frame_big == 0) {
		frame_ms = 0;
	}
	nanoseconds_to_absolutetime((uint64_t)boost_ms * NSEC_PER_MSEC, &boost_span);
	nanoseconds_to_absolutetime((uint64_t)frame_ms * NSEC_PER_MSEC, &frame_span);
	if (!hakios_acpm_init(say)) {
		say("cpufreq_gov: no ACPM: not started (and the fixed rates not set either)\n");
		return true;
	}
	clusters[0] = (struct cluster){ .name = "lit", .zone = "kern.hakios.temp.little_mc", .tmu_zone = 1, .index = 2, .affinity = 0,
		                        .control_mc = 76000, .dt_min = 455000, .dt_max = 1950000, .dt_levels = lit_dt,
		                        .n_dt_levels = sizeof(lit_dt) / sizeof(lit_dt[0]) };
	clusters[1] = (struct cluster){ .name = "big", .zone = "kern.hakios.temp.big_mc", .tmu_zone = 0, .index = 3, .affinity = 1,
		                        .control_mc = 83000, .dt_min = 650000, .dt_max = 2314000, .dt_levels = big_dt,
		                        .n_dt_levels = sizeof(big_dt) / sizeof(big_dt[0]) };
	cluster_setup(&clusters[0], lit_max, lit_min, 1794000, 1053000);
	cluster_setup(&clusters[1], big_max, big_min, 1794000, 650000);
	clusters[0].touch_khz = touch_lit;
	clusters[1].touch_khz = touch_big;
	clusters[1].frame_khz = frame_big;
	for (int i = 0; i < 2; i++) {
		struct cluster *c = &clusters[i];
		uint32_t found = 0;
		int e = hakios_acpm_dvfs_get(c->index, &found);

		c->cur = e == 0 ? found : 0;
		say("cpufreq_gov: %s (DVFS %u): %u to %u kHz, now %u kHz (%d)%s\n", c->name, c->index, c->floor, c->ceiling,
		    c->cur, e, i == 1 ? "; and by M3s online: 1-2 2314000, 3-4 1794000; 1794000 without temperatures" : "");
		if (c->cur == 0) {
			/* (not said: the ceiling is asked for, and from then on the rate is the one asked for) */
			set_rate(c, c->levels[level_at_most(c, c->ceiling)], c->ceiling);
		}
	}
	say("cpufreq_gov: every %u ms, the busiest core %u%% busy wanted, down after %u windows; cooling from %d C (big), %d C "
	    "(lit), the lowest level from %d C; a line every %u s\n", window_ms, target_load, down_windows,
	    clusters[1].control_mc / 1000, clusters[0].control_mc / 1000, HOT_CUT_MC / 1000, log_seconds);
	say("cpufreq_gov: boosts: touch big %u lit %u kHz for %u ms after the last finger event; frames big %u kHz while one "
	    "came in the last %u ms; %u%% busy or more: the touch-boost rate at once; all within the ceilings above\n",
	    boost_ms ? touch_big : 0, boost_ms ? touch_lit : 0, boost_ms, frame_ms ? frame_big : 0, frame_ms, SATURATED);
	if (kernel_thread_start(gov_thread, NULL, &thread) != KERN_SUCCESS) {
		say("cpufreq_gov: no thread: the rates stay as they are\n");
		return true;
	}
	thread_deallocate(thread);
	__atomic_store_n(&gov_running, true, __ATOMIC_RELEASE);
	return true;
}
