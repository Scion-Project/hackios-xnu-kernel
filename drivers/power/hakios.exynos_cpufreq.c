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
 * Samsung's kernel uses). The temperatures are the TMU driver's
 * (kern.hakios.temp.big_mc and little_mc: tmu=2 or more). Without them (no tmu=, or not read yet) the M3s stay at or
 * below 1794000 whatever the online count. tmu=3 ends the run at 110 C as before; this changes nothing there.
 *
 * The load. Every cpufreq_gov_ms (40 ms by default, 20 to 200) each core's idle time from the scheduler
 * (ml_cpu_idle_time, as cpuload= reads it); a cluster's load is its busiest core's share of the window it was not idle.
 * The rate wanted is the one at which that core would be 75% busy (the current rate times load / 75), rounded up to a
 * level. Up at once; down only after three windows in a row want less (about 120 ms). A core that has never been idle
 * since start is taken as not started (cores 0 and 1 are not the kernel's at all): it counts neither for the load nor as
 * online. Every change is read back from the APM (hakios_acpm_dvfs_get); a rate read back above the ceiling is logged
 * and the next level down asked for.
 *
 * The log: at start the table, the limits and the rates found; then once a second (cpufreq_gov_log=SECONDS; 0: none)
 *   cpufreq_gov: 12 s: lit 1248 MHz load 31% (avg 22%, 1794 max) | big 1586 MHz load 64% (avg 58%, 4 online, 1794 max)
 *       | temp big 52.1 lit 48.0 C | 9 changes
 * to the loader's log (loader_log_line), which printf does not reach.
 *
 * Status: WRITTEN, NOT YET RUN on the phone. It builds; the level arithmetic is plain. Not known until the first run:
 * whether the APM's table is where Samsung's kernel finds it (its absence only means the device tree's levels are used),
 * and how the APM answers a request it would refuse (it is never sent one outside its table).
 *
 * Boot-args:
 *   cpufreq_gov=1            run it (0, or none: the fixed rates of cpufreq_lit= and cpufreq_big=)
 *   cpufreq_gov_ms=<ms>      the load window (20 to 200; 40)
 *   cpufreq_gov_log=<s>      a summary line this often (0: none; 1)
 *   cpufreq_gov_lit_max=<kHz>, cpufreq_gov_big_max=<kHz>   the ceilings (at most 1950000 and the online count's limit)
 *   cpufreq_gov_lit_min=<kHz>, cpufreq_gov_big_min=<kHz>   the floors (at least 455000 and 650000)
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

extern void loader_log_line(const char *line);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
extern int kernel_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);

#define LEVELS_MOST     32
#define TARGET_LOAD     75      /* percent: the busiest core's share wanted at the new rate */
#define DOWN_WINDOWS    3       /* windows in a row that want less before the rate comes down */
#define NOT_READ_MC     (-273000)
#define HOT_CUT_MC      96000   /* BIG's hotplug_out_threshold: the cluster to its lowest level */
#define HYSTERESIS_MC   5000
#define NO_TEMP_BIG_MAX 1794000 /* without temperatures, the M3s no faster than this */

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
	const char *zone;               /* the TMU's sysctl */
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
	uint32_t cur;                   /* kHz, as read back */
	uint32_t load, online, low_windows;
	uint32_t load_sum, windows;     /* this second's */
	uint32_t lowest;                /* the slowest level at or above the device tree's min-freq: as low as the heat takes it */
	uint32_t changes, errors;
	int32_t temp_mc;
} clusters[2];

static const uint32_t lit_dt[] = {
	1950000, 1794000, 1690000, 1456000, 1248000, 1053000, 949000, 832000, 715000, 598000, 455000,
};
static const uint32_t big_dt[] = {
	2886000, 2860000, 2704000, 2652000, 2496000, 2314000, 2106000, 2002000, 1924000, 1794000, 1690000, 1586000, 1469000,
	1261000, 1170000, 1066000, 962000, 858000, 741000, 650000, 598000,
};

static uint32_t window_ms = 40, log_seconds = 1;
static uint64_t idle_before[MAX_CPUS];
static bool ever_idle[MAX_CPUS];

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

static void
set_rate(struct cluster *c, uint32_t khz, uint32_t max)
{
	uint32_t got = 0;
	int e1, e2;

	e1 = hakios_acpm_dvfs_set(c->index, khz);
	e2 = hakios_acpm_dvfs_get(c->index, &got);
	if (e1 != 0 || e2 != 0) {
		if (c->errors++ < 5) {
			say("cpufreq_gov: %s: asked %u kHz: set %d, get %d (%u kHz)\n", c->name, khz, e1, e2, got);
		}
		if (e2 == 0) {
			c->cur = got;
		}
		return;
	}
	c->changes++;
	c->cur = got;
	if (got > max) {
		uint32_t lower = level_at_most(c, khz);

		lower = lower + 1 < c->n ? lower + 1 : lower;
		say("cpufreq_gov: %s: asked %u kHz, the APM says %u, above %u: asking %u\n", c->name, khz, got, max,
		    c->levels[lower]);
		(void)hakios_acpm_dvfs_set(c->index, c->levels[lower]);
		if (hakios_acpm_dvfs_get(c->index, &got) == 0) {
			c->cur = got;
		}
	}
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
decide(struct cluster *c)
{
	uint32_t max = ceiling_now(c), want, target;

	c->load_sum += c->load;
	c->windows++;
	if (c->online == 0 || c->cur == 0) {
		return;
	}
	want = c->levels[level_at_least(c, (uint64_t)c->cur * c->load / TARGET_LOAD)];
	target = MAX(want, c->floor);
	target = MIN(target, max);
	if (target < c->cur && c->cur <= max) {
		if (++c->low_windows < DOWN_WINDOWS) {
			return;
		}
	}
	c->low_windows = 0;
	if (target != c->cur) {
		set_rate(c, target, max);
	}
}

static int32_t
read_temp(const char *name)
{
	int value = NOT_READ_MC;
	size_t size = sizeof(value);

	if (kernel_sysctlbyname(name, &value, &size, NULL, 0) != 0) {
		return NOT_READ_MC;
	}
	return value;
}

/* once a second: each zone's temperature moves its cluster's thermal ceiling by a level at most */
static void
cool(struct cluster *c)
{
	uint32_t before = c->thermal;

	c->temp_mc = read_temp(c->zone);
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
	char tb[16], tl[16];

	temp_text(tb, sizeof(tb), b->temp_mc);
	temp_text(tl, sizeof(tl), l->temp_mc);
	say("cpufreq_gov: %llu s: lit %u MHz load %u%% (avg %u%%, %u max) | big %u MHz load %u%% (avg %u%%, %u online, %u max) "
	    "| temp big %s lit %s C | %u changes\n", seconds, l->cur / 1000, l->load, l->windows ? l->load_sum / l->windows : 0,
	    ceiling_now(l) / 1000, b->cur / 1000, b->load, b->windows ? b->load_sum / b->windows : 0, b->online,
	    ceiling_now(b) / 1000, tb, tl, l->changes + b->changes);
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
		uint64_t now, ns;

		delay_for_interval(window_ms, NSEC_PER_MSEC);
		now = mach_absolute_time();
		measure(now - then > 0 ? now - then : 1);
		then = now;
		if (now - last_second >= second) {
			last_second = now;
			cool(&clusters[0]);
			cool(&clusters[1]);
		}
		decide(&clusters[0]);
		decide(&clusters[1]);
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
	if (!hakios_acpm_init(say)) {
		say("cpufreq_gov: no ACPM: not started (and the fixed rates not set either)\n");
		return true;
	}
	clusters[0] = (struct cluster){ .name = "lit", .zone = "kern.hakios.temp.little_mc", .index = 2, .affinity = 0,
		                        .control_mc = 76000, .dt_min = 455000, .dt_max = 1950000, .dt_levels = lit_dt,
		                        .n_dt_levels = sizeof(lit_dt) / sizeof(lit_dt[0]) };
	clusters[1] = (struct cluster){ .name = "big", .zone = "kern.hakios.temp.big_mc", .index = 3, .affinity = 1,
		                        .control_mc = 83000, .dt_min = 650000, .dt_max = 2314000, .dt_levels = big_dt,
		                        .n_dt_levels = sizeof(big_dt) / sizeof(big_dt[0]) };
	cluster_setup(&clusters[0], lit_max, lit_min, 1794000, 1053000);
	cluster_setup(&clusters[1], big_max, big_min, 1794000, 650000);
	for (int i = 0; i < 2; i++) {
		struct cluster *c = &clusters[i];
		uint32_t found = 0;
		int e = hakios_acpm_dvfs_get(c->index, &found);

		c->cur = e == 0 ? found : 0;
		say("cpufreq_gov: %s (DVFS %u): %u to %u kHz, now %u kHz (%d)%s\n", c->name, c->index, c->floor, c->ceiling,
		    c->cur, e, i == 1 ? "; and by M3s online: 1-2 2314000, 3-4 1794000; 1794000 without temperatures" : "");
		if (c->cur == 0) {
			c->cur = c->levels[level_at_most(c, c->ceiling)];
			set_rate(c, c->cur, c->ceiling);
		}
	}
	say("cpufreq_gov: every %u ms, the busiest core %u%% busy wanted, down after %u windows; cooling from %d C (big), %d C "
	    "(lit), the lowest level from %d C; a line every %u s\n", window_ms, TARGET_LOAD, DOWN_WINDOWS,
	    clusters[1].control_mc / 1000, clusters[0].control_mc / 1000, HOT_CUT_MC / 1000, log_seconds);
	if (kernel_thread_start(gov_thread, NULL, &thread) != KERN_SUCCESS) {
		say("cpufreq_gov: no thread: the rates stay as they are\n");
		return true;
	}
	thread_deallocate(thread);
	return true;
}
