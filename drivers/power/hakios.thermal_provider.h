/*
 * The SoC's temperature, whichever phone: the driver that reads a phone's sensors registers here
 * (hakios_thermal_register()), and drivers/power/hakios.thermal_provider.c publishes what it reads under the one set of
 * names, kern.hakios.thermal_max_mc, thermal_hottest, thermal_sensors and thermal_reads, and hands the hottest to other
 * drivers (hakios_thermal_max_mc()). The POCO's Qualcomm TSENS (hakios.thermal.c, thermal=1) and the Galaxy S9's
 * Exynos TMU (hakios.exynos_tmu.c, tmu=2) both register; the first to register keeps the names.
 */
#ifndef _HAKIOS_THERMAL_PROVIDER_H_
#define _HAKIOS_THERMAL_PROVIDER_H_

#define HAKIOS_THERMAL_NOT_READ_MC      (-273000)

/* What a driver publishes: its own variables, read each time a name is asked for. */
struct hakios_thermal_provider {
	const char *    name;           /* the driver: "thermal", "exynos_tmu" */
	const int *     max_mc;         /* the hottest valid sensor, millidegrees; HAKIOS_THERMAL_NOT_READ_MC: none read */
	const char *    hottest;        /* that sensor's name */
	const char *    sensors;        /* every sensor and its temperature */
	const int *     reads;          /* how many times the sensors were read */
};

/* Once, at the driver's start: 0, EBUSY when another driver registered first, EINVAL for a provider missing a field. */
int hakios_thermal_register(const struct hakios_thermal_provider *provider);

/* The provider registered, or NULL. */
const char *hakios_thermal_provider_name(void);

/* For other drivers (a GPU or modem that wants to back off): the hottest temperature, in millidegrees; -273000: not read. */
int hakios_thermal_max_mc(void);

#endif /* _HAKIOS_THERMAL_PROVIDER_H_ */
