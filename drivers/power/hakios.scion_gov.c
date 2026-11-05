/*
 * The scion CPU governor: the control law, apart from any kernel or SoC (hakios.exynos_cpufreq.c measures and acts on it,
 * gov_policy=3; tools/gov-replay runs this file on a host).
 *
 *   rate    the level at which the busiest core would be 60% busy. Up at once; down after 300 ms, to the most any of those
 *           windows wanted. Never under the floor the input events ask for, never over the ceiling.
 *   cores   the fastest rates are for few big cores only, as in stock's hotplug governor. When the busiest core is heavy at
 *           the all-core limit, the rest idle and the little cores not heavy, for change_ms, the others are parked and the
 *           ceiling rises. They come back when the two cannot hold the load, when the rate no longer needs it, or at
 *           unpark_at_mc, read every window: a burst has its heat to spend and sustained load stays out. The caller lowers the rate before it brings a core back and never raises it before one is parked.
 *   heat    a temperature that is not read, not plausible or not recent means the safe rate and every core, at once.
 *
 * It never asks for a rate outside the APM's table or above the SoC's limits, and sends no voltage. The TMU driver's stop and
 * the caller's thermal ceiling (a level a second from the control point, the lowest at 96 C) stand above everything here.
 */
#include <sys/hakios.scion_gov.h>

#define TARGET_PCT      60
#define DOWN_MS         300
#define TEMP_TOO_OLD_MS 3000    /* three of the TMU's 1 s polls missed */
#define TEMP_LOW_MC     1000
#define TEMP_HIGH_MC    125000

bool
scion_temp_ok(int32_t mc, uint64_t age_ms)
{
	return mc >= TEMP_LOW_MC && mc <= TEMP_HIGH_MC && age_ms <= TEMP_TOO_OLD_MS;
}

/* the slowest level at or above khz; the fastest when there is none */
static uint32_t
level_up(const uint32_t *levels, uint32_t n, uint64_t khz)
{
	for (uint32_t l = n; l-- > 0;) {
		if (levels[l] >= khz) {
			return levels[l];
		}
	}
	return levels[0];
}

uint32_t
scion_rate(struct scion_rate *s, const uint32_t *levels, uint32_t n, uint32_t cur, uint32_t load, uint32_t floor,
    uint32_t ceiling, uint32_t window_ms)
{
	uint32_t want = level_up(levels, n, (uint64_t)cur * load / TARGET_PCT);

	want = want > floor ? want : floor;
	want = want < ceiling ? want : ceiling;
	if (want >= cur || cur > ceiling) {
		s->low_ms = s->peak = 0;
		return want;
	}
	s->peak = want > s->peak ? want : s->peak;
	s->low_ms += window_ms;
	if (s->low_ms < DOWN_MS) {
		return cur;
	}
	want = s->peak;
	s->low_ms = s->peak = 0;
	return want;
}

uint32_t
scion_limit(const struct scion_soc *soc, uint32_t cores)
{
	return cores <= soc->big_limit[0].cores ? soc->big_limit[0].khz : soc->big_limit[1].khz;
}

uint32_t
scion_cores(struct scion_park *s, const struct scion_soc *soc, uint32_t cores, const uint32_t *load, uint32_t lit_load,
    uint32_t cur, int32_t mc, uint32_t window_ms)
{
	uint32_t few = soc->big_limit[0].cores, all = soc->big_limit[1].cores, top = soc->big_limit[1].khz;

	if (cores <= few) {
		s->asked_ms = 0;
		if (mc >= soc->unpark_at_mc || cur <= top || (load[few - 1] >= soc->heavy_pct && lit_load >= soc->lit_heavy_pct)) {
			return all;
		}
		return few;
	}
	if (mc < soc->park_under_mc && cur >= top && load[0] >= soc->heavy_pct && load[few] <= soc->idle_pct && lit_load < soc->lit_heavy_pct) {
		s->asked_ms += window_ms;
		if (s->asked_ms >= soc->change_ms) {
			s->asked_ms = 0;
			return few;
		}
	} else {
		s->asked_ms = 0;
	}
	return all;
}
