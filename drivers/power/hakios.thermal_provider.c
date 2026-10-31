/*
 * The SoC's temperature, whichever phone (hakios.thermal_provider.h): the one owner of kern.hakios.thermal_* and
 * hakios_thermal_max_mc(). A phone's temperature driver registers its variables (hakios_thermal_register()); the names
 * read them each time they are asked for. Until one registers, and in a kernel with no such driver, they read as the
 * POCO's driver left them before its first read: -273000, "", "", 0.
 *
 * Why it is here. The POCO's hakios.thermal.c (THERMAL) and the S9's hakios.exynos_tmu.c (EXYNOS_TMU) each defined these
 * names and the function themselves, so a kernel could not have both drivers in; the panel's brightness was untangled
 * the same way (drivers/gpu/hakios.backlight.cpp, hakios_panel_register()). Nothing is said in the log here, so that a
 * phone's log is what it was.
 */
#include <stdint.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "hakios.thermal_provider.h"

SYSCTL_DECL(_kern_hakios);

static const struct hakios_thermal_provider *provider;

/* once, at a driver's start (STARTUP, one thread): the first keeps the names */
int
hakios_thermal_register(const struct hakios_thermal_provider *p)
{
	if (p == NULL || p->name == NULL || p->max_mc == NULL || p->hottest == NULL || p->sensors == NULL || p->reads == NULL) {
		return EINVAL;
	}
	if (provider != NULL) {
		return EBUSY;
	}
	provider = p;
	return 0;
}

const char *
hakios_thermal_provider_name(void)
{
	return provider != NULL ? provider->name : NULL;
}

int
hakios_thermal_max_mc(void)
{
	return provider != NULL ? *provider->max_mc : HAKIOS_THERMAL_NOT_READ_MC;
}

/* arg2: which name. Read only: the same int or string the driver's own SYSCTL_INT/SYSCTL_STRING gave. */
enum { MAX_MC, HOTTEST, SENSORS, READS };

static int
thermal_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1)
	switch (arg2) {
	case MAX_MC:
		return sysctl_handle_int(oidp, NULL, hakios_thermal_max_mc(), req);
	case READS:
		return sysctl_handle_int(oidp, NULL, provider != NULL ? *provider->reads : 0, req);
	case HOTTEST:
		return sysctl_handle_string(oidp, __DECONST(char *, provider != NULL ? provider->hottest : ""), 0, req);
	case SENSORS:
		return sysctl_handle_string(oidp, __DECONST(char *, provider != NULL ? provider->sensors : ""), 0, req);
	}
	return EINVAL;
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, thermal_max_mc, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, MAX_MC, thermal_sysctl, "I",
    "the hottest valid temperature sensor of the SoC, in millidegrees (-273000: none read)");
SYSCTL_PROC(_kern_hakios, OID_AUTO, thermal_hottest, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, HOTTEST, thermal_sysctl, "A",
    "the name of that sensor");
SYSCTL_PROC(_kern_hakios, OID_AUTO, thermal_sensors, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, SENSORS, thermal_sysctl, "A",
    "every sensor and its temperature in degrees: \"cpu-0-0=41.2 gpuss-0=38.0 ...\"");
SYSCTL_PROC(_kern_hakios, OID_AUTO, thermal_reads, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, READS, thermal_sysctl, "I",
    "how many times the sensors were read");
