#define HAKIOS_SOC_FIXED
/*
 * Power off for the Galaxy S9: releases PS_HOLD, for the TMU's stop (tmu_off=1) and, with halt_off=N, for a halt.
 *
 * Without this a halt hands the phone back to the loader with the rails up, and the panel (S6E3HA8, MIPI command mode)
 * keeps showing its last frame from its own memory, so the phone looks frozen until the battery is flat.
 *
 * The PMIC (S2MPS18) keeps the phone on while the SoC holds PWRHOLD high, which it does while bit 8 of the PMU's
 * PS_HOLD_CONTROL is set. Clearing that bit is how Samsung's kernel ends a power-off (exynos-reboot.c: exynos_power_off(),
 * sec_reboot.c: sec_power_off(); the offset from sec_ext.h). With a charger attached or the power key held the PMIC keeps
 * the phone up anyway; Samsung then restarts into its charging screen, and here the run returns to the loader.
 *
 * halt_off=
 *   0  a halt returns to the loader (default)
 *   1  save the data volume, blank the panel (DCS display-off 0x28, sleep-in 0x10), release PS_HOLD unless a charger is reported
 *   2  the same, and stop the APM first (Samsung's acpm_enter_wfi(), sent before every PS_HOLD by exynos_acpm_reboot())
 * If the phone is still running a second after PS_HOLD, the run returns to the loader with the panel dark.
 *
 * Not yet run on a phone. Unknown: whether PS_HOLD alone powers off with the APM running (Samsung never does that), and
 * what the APM answers to the stop request. QEMU has no such PMU: never give halt_off= there.
 */
#include <stdint.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/hakios.exynos_soc.h>
#include <sys/hakios.log.h>
#include <sys/hakios.xnu.h>
extern void hakios_var_save_on_return(void);
extern bool hakios_s6e3ha8_blank(int (*say)(const char *, ...));   /* drivers/display/panel/hakios.s6e3ha8.c; a stand-in says no */

bool hakios_exynos_power_off(const char *who, int (*say_as)(const char *, ...));
void hakios_exynos_halt_off(boolean_t reboot);

#define PMU_PA                  ((uint32_t)EXYNOS_PMU_PA)     /* flexpmu_cal_system_exynos9810.h:39 */
#define PMU_PS_HOLD_CONTROL     EXYNOS_POWEROFF_PMU_PS_HOLD_CONTROL         /* sec_ext.h:33 */
#define PS_HOLD_HIGH            (1u << 8)       /* sec_reboot.c:135, exynos-reboot.c:252 */

static bool
read_int(const char *name, int *value)
{
	size_t size = sizeof(*value);

	return kernel_sysctlbyname(name, value, &size, NULL, 0) == 0 && size == sizeof(*value);
}

/*
 * PS_HOLD low, when the battery driver says no charger is attached (not known counts as attached): returns only if the
 * phone is still running a second later, or was not powered off. `who` begins each line said, in the caller's `say`.
 */
bool
hakios_exynos_power_off(const char *who, int (*say_as)(const char *, ...))
{
	int charger = 1;

	if (!read_int("kern.hakios.battery_charger_present", &charger) || charger) {
		say_as("%s: a charger is attached or not known (%d): the loader's way\n", who, charger);
		return false;
	}
	vm_offset_t pmu = ml_io_map(PMU_PA + (PMU_PS_HOLD_CONTROL & ~0xfffu), 0x1000);

	if (pmu == 0) {
		say_as("%s: cannot map the PMU at %#x: the loader's way\n", who, PMU_PA);
		return false;
	}
	volatile uint32_t *ps_hold = (volatile uint32_t *)(pmu + (PMU_PS_HOLD_CONTROL & 0xfffu));
	uint32_t v = *ps_hold;

	say_as("%s: PS_HOLD_CONTROL %08x: PS_HOLD low, the phone off (this log is lost with it)\n", who, v);
	*ps_hold = v & ~PS_HOLD_HIGH;
	delay(1000 * 1000);
	say_as("%s: still running a second after PS_HOLD (%08x): the loader's way instead\n", who, *ps_hold);
	return false;
}

/* halt_all_cpus(): with halt_off=N, a halt is the phone powered off; returns when it is not (the loader's way follows) */
void
hakios_exynos_halt_off(boolean_t reboot)
{
	uint32_t level = 0;

	if (reboot || !PE_parse_boot_argn("halt_off", &level, sizeof(level)) || level == 0) {
		return;
	}
	say("\n--- halt_off=%u: the panel blanked, %sthen the phone powered off (PS_HOLD) ---\n", level,
	    level >= 2 ? "the APM stopped, " : "");
	hakios_var_save_on_return();
	say("halt_off: 1: the panel %s\n", hakios_s6e3ha8_blank(say) ? "is dark (display off, sleep-in)" :
	    "NOT blanked: it may go on showing its last frame");
	if (level >= 2) {
		int error = hakios_acpm_init(say) ? hakios_acpm_stop() : ENXIO;

		say("halt_off: 2: the APM asked to stop (ACPM channel %d, STOP): %s (%d); PS_HOLD whatever it answered, as "
		    "Samsung's exynos_power_off() does\n", HAKIOS_ACPM_CH_ACPM, error == 0 ? "answered" :
		    error == ENXIO ? "no ACPM, or no such channel" : "no answer", error);
	}
	(void)hakios_exynos_power_off("halt_off: 3", say);
}
