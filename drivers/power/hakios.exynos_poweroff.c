/*
 * The Galaxy S9 powered off (Exynos 9810): PS_HOLD let go, for the TMU's stop (tmu_off=1, hakios.exynos_tmu.c) and, with
 * boot-arg halt_off=N, for a halt ("shut down" in iOS: host_reboot with HOST_REBOOT_HALT, halt_all_cpus(FALSE), patch 1510).
 *
 * Why. No platform driver of this port powers a machine off: a halt hands the phone back to the loader (loader_return),
 * and the rails stay up. The S9's panel, an S6E3HA8 in MIPI command mode, keeps its own copy of the last frame and goes on
 * showing it from that memory with nothing sending it anything: the phone looks frozen on the last spinner frame of the
 * shutdown, and stays on until the battery is flat or the loader's own restart comes.
 *
 * PS_HOLD. The PMIC (S2MPS18) keeps the phone powered while the SoC holds its PWRHOLD line high, and the SoC holds it so
 * while the PMU's PS_HOLD_CONTROL (0x14060000 +0x330C) has bit 8 set. Clearing that bit is how Samsung's kernel ends a
 * power-off (sec_power_off(), drivers/staging/samsung/sec_reboot.c:135; exynos_power_off(), drivers/soc/samsung/
 * exynos-reboot.c:252; the offset in include/linux/sec_ext.h:33). Everything is lost with it, this log too (pstore is RAM).
 * With a charger attached, or the power key still held, the PMIC keeps the phone up anyway: Samsung then restarts into its
 * charging screen instead (INFORM2 = SEC_POWER_OFF, sec_reboot.c:120); here the run is handed to the loader, as before.
 *
 * A halt, with halt_off=N (only on the Galaxy S9: every address here belongs to it; 0, or none: nothing, the halt as it was):
 *   1  the data volume saved (hakios_var_save_on_return(), as every orderly end of a run does), the panel blanked
 *      (hakios_s6e3ha8_blank(): DCS display-off, 0x28, and sleep-in, 0x10, through DSIM0's command FIFO, as Samsung's
 *      display-off and exit sequences send them), then PS_HOLD low, unless the battery driver reports a charger
 *   2  the same, and before PS_HOLD the APM asked to stop (hakios_acpm_stop(): Samsung's acpm_enter_wfi(), which
 *      exynos_acpm_reboot() sends before every PS_HOLD and every restart; drivers/soc/samsung/acpm/acpm.c:250-292)
 * In both, the phone still running a second after PS_HOLD (a charger the battery driver did not see, the power key held),
 * or a charger reported: the loader's way, as without halt_off -- with the panel already dark, so a phone that the loader
 * then leaves on at least does not look frozen.
 *
 * Status: WRITTEN, NOT YET RUN on the phone, nor in QEMU (it has no such PMU: halt_off= must not be given there). Not known:
 * whether PS_HOLD alone powers the S9 off with the APM still running (Samsung never does it without exynos_acpm_reboot()
 * first, halt_off=2; tmu_off=1 does, and has not been seen either), and what the APM answers to the stop request.
 *
 * Boot-args:
 *   halt_off=<0-2>   what a halt does: 0 the loader's way (the default), 1 panel off and PS_HOLD, 2 also the APM stopped first
 */
#include <stdint.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/hakios.exynos_acpm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);
extern void hakios_var_save_on_return(void);
extern bool hakios_s6e3ha8_blank(int (*say)(const char *, ...));   /* drivers/gpu/hakios.s6e3ha8.c; a stand-in says no */
extern int kernel_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
extern void delay(int usec);

bool hakios_exynos_power_off(const char *who, int (*say_as)(const char *, ...));
void hakios_exynos_halt_off(boolean_t reboot);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

#define PMU_PA                  0x14060000u     /* flexpmu_cal_system_exynos9810.h:39 */
#define PMU_PS_HOLD_CONTROL     0x330Cu         /* sec_ext.h:33 */
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
