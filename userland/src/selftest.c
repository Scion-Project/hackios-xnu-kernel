/*
 * selftest: one plain-text report of how a first boot went, for a tester to read over the USB console.
 *
 * Sixty seconds after the boot (SpringBoard first, the radio and the panel settled) it writes REPORT: the kernel's
 * version and boot arguments, the cores online, the display (the frame buffer's surface; fps_log is among the arguments or not), the touch events seen (kern.hakios.touch_events: it counts only with s6sy761=1), the Wi-Fi interface, the
 * UFS errors, the panic reports on the volume, the battery and the temperatures. Every line is also printed on the
 * console, "selftest: " first, which is how it reaches a host on the USB console (the file is for a kept volume).
 * Nothing leaves the phone.
 *
 * Only the first boot: DONE is written after the report, and a later boot ends at once. On a /private/var kept in
 * memory nothing lasts, so every boot reports. A value the kernel does not offer reads "n/a". The job is
 * com.hakios.selftest; a panic reboots the phone before it can be counted, so the panic line counts the reports that
 * an earlier run left, not the one that just happened.
 */
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#define REPORT  "/private/var/scion-selftest.txt"
#define DONE    "/private/var/.scion-selftest-done"
#define PANICS  "/private/var/mobile/Library/Logs/CrashReporter"
#define AFTER   60                      /* seconds of uptime */

static FILE *out;

/* a line to the report and, prefixed, to the console, whose log the USB console streams to the host */
static void
put(const char *name, const char *value)
{
	char line[640];

	snprintf(line, sizeof(line), "%s: %s\n", name, value);
	fputs(line, out);
	printf("selftest: %s", line);
}

static void
show(const char *name, long long value, int ok)
{
	char digits[24] = "n/a";

	if (ok) {
		snprintf(digits, sizeof(digits), "%lld", value);
	}
	put(name, digits);
}

/* an integer sysctl of any width */
static int
number(const char *name, long long *value)
{
	union { int32_t i; int64_t q; char raw[8]; } v = { .q = 0 };
	size_t len = sizeof(v);

	if (sysctlbyname(name, &v, &len, NULL, 0) != 0) {
		return 0;
	}
	*value = len == sizeof(v.q) ? v.q : v.i;
	return 1;
}

static void
count(const char *name)
{
	long long v = 0;
	int ok = number(name, &v);

	show(name, v, ok);
}

static void
text(const char *name)
{
	char buf[512];
	size_t len = sizeof(buf) - 1;

	if (sysctlbyname(name, buf, &len, NULL, 0) != 0) {
		show(name, 0, 0);
		return;
	}
	buf[len] = '\0';
	put(name, buf);
}

static void
wifi(void)
{
	struct ifaddrs *list, *a;
	int found = 0;

	if (getifaddrs(&list) == 0) {
		for (a = list; a != NULL; a = a->ifa_next) {
			if (strcmp(a->ifa_name, "en0") == 0 && a->ifa_addr != NULL && a->ifa_addr->sa_family == AF_LINK) {
				put("wifi en0", (a->ifa_flags & IFF_UP) ?
				    ((a->ifa_flags & IFF_RUNNING) ? "up, running" : "up, not running") : "down");
				found = 1;
			}
		}
		freeifaddrs(list);
	}
	if (!found) {
		show("wifi en0", 0, 0);
	}
}

static void
panics(void)
{
	DIR *dir = opendir(PANICS);
	struct dirent *e;
	int n = 0;

	if (dir == NULL) {
		show("panic reports", 0, 0);
		return;
	}
	while ((e = readdir(dir)) != NULL) {
		size_t len = strlen(e->d_name);

		n += len > 6 && strcmp(e->d_name + len - 6, ".panic") == 0;
	}
	closedir(dir);
	show("panic reports", n, 1);
}

int
main(void)
{
	struct timeval boot, now;
	size_t len = sizeof(boot);
	struct stat st;

	if (stat(DONE, &st) == 0) {
		return 0;
	}
	if (sysctlbyname("kern.boottime", &boot, &len, NULL, 0) == 0 && gettimeofday(&now, NULL) == 0 &&
	    now.tv_sec - boot.tv_sec < AFTER) {
		sleep((unsigned)(AFTER - (now.tv_sec - boot.tv_sec)));
	}
	if ((out = fopen(REPORT, "w")) == NULL) {
		printf("selftest: %s: cannot write\n", REPORT);
		return 1;
	}
	printf("selftest: begins\n");
	text("kern.version");
	text("kern.bootargs");
	count("hw.ncpu");
	count("hw.activecpu");
	count("kern.hakios.fb_surface");
	count("kern.hakios.touch_events");
	wifi();
	count("kern.hakios.ufs_errors");
	count("kern.hakios.ufs_write_errors");
	count("kern.hakios.ufs_native_errors");
	panics();
	count("kern.hakios.battery_present");
	count("kern.hakios.battery_soc");
	count("kern.hakios.battery_charging");
	count("kern.hakios.battery_voltage_mv");
	count("kern.hakios.battery_temperature_c");
	count("kern.hakios.temp.max_mc");
	text("kern.hakios.temp.hottest");
	count("kern.hakios.thermal_pressure");
	printf("selftest: ends\n");
	fclose(out);
	close(creat(DONE, 0644));
	return 0;
}
