/*
 * The Galaxy S9's real-time clock: the RTC block of its S2MPS18 PMIC, read once at start-up so that the kernel's calendar
 * starts at the phone's own time instead of the time the image was built (epoch=). READ ONLY: nothing is written to
 * the PMIC. Only with boot-arg rtc_pmic=1, the S9's default (devices/starlte/configs/starlte.config): its read seen
 * working on the phone, 2 November.
 *
 * The way. The S2MPS18 is on SPEEDY, which the APM owns: the AP reaches its registers only by asking the APM through
 * ACPM (drivers/core/hakios.exynos_acpm.c, channel 2, the same path the GPU's and the touch screen's supplies take, and
 * Samsung's kernel's own s2mps18 driver: every one of its reads and writes is an ACPM request). The RTC is the PMIC's
 * block 2 ("type" 2; Samsung's kernel calls it the i2c address 0x02 of the PMIC). Its registers, as Samsung's kernel
 * has them for the S9 (CONFIG_RTC_DRV_S2MP not set, so the layout with milliseconds):
 *   0x00 CTRL     bit 0 BCD (0: binary), bit 1 24-hour mode; Linux sets 0x02 (binary, 24 h) when it finds the block reset
 *   0x01 WTSR_SMPL                0x02 UPDATE (RUDR bit 0, the latch of the counters into the registers read here)
 *   0x03 CAPSEL   its top four bits are all set once Linux has set the clock up: "CTRL bit 1 and CAPSEL[7:4] = 0xF, or
 *                 the RTC was reset" is Linux's own test
 *   0x04 MSEC     0x05 SEC [6:0]  0x06 MIN [6:0]  0x07 HOUR [4:0] (bit 6: PM, said, not used in 24-hour mode)
 *   0x08 WEEK (one bit a day)     0x09 DAY [4:0]  0x0A MONTH [3:0] (1-12)  0x0B YEAR [6:0] (since 2000)
 * all binary.
 *
 * What is read is what was last latched. Linux latches (writes RUDR to UPDATE) before each read; that is a write, and
 * this driver does not write. S-Boot reads the clock on every boot (its log: "PMIC_RTC_CTRL = 0x02", "RTC TIME:
 * 2018-01-01 14:54:18(0x02)PM" at 1.26 s of its timer), so the registers hold the time of S-Boot's read, a little
 * after the timer started. When the kernel's uptime at this read is between 2 s and 10 minutes (the timer counted
 * from S-Boot's start, not from an earlier boot) that uptime less one second is added; otherwise nothing is.
 * Either way the log says both, so the first run shows how far off it is.
 *
 * Which wins. The kernel's calendar starts at the clock's time when the clock is valid -- its CTRL and CAPSEL as Linux
 * leaves them, every field in range, a year 2024 to 2099 -- and not earlier than epoch= (the time the image was made:
 * a clock behind it is wrong). Otherwise at epoch= (or the loader's boot-epoch), as before. On the S9's runs so far
 * (runs/auto-210855, the phone's last_kmsg) the clock said 2018-01-01: reset and never set, because nothing sets it
 * from the network; then epoch= stays. Setting the clock (from iOS's time) would be a write, and is not here.
 *
 * Boot-args:
 *   rtc_pmic=1   read the clock at start-up (the kernel's first ask for the time of day), log it, use it when valid
 * Log (loader log), with rtc_pmic=1:
 *   rtc: S2MPS18 RTC (ACPM, block 2), read only
 *   rtc: CTRL 0x02 UPDATE 0x.. CAPSEL 0xf. MSEC 0x.. SEC 0x.. MIN 0x.. HOUR 0x.. WEEK 0x.. DAY 0x.. MONTH 0x.. YEAR 0x..
 *   rtc: the clock says 2026-11-02 08:49:03 (1793609343); uptime 9 s: +8 s since S-Boot read it: 1793609351
 *   rtc: using the clock: 2026-11-02 08:49:11 UTC (epoch= 1793600000 was 9351 s behind)
 * or "rtc: not used (<why>); the calendar starts at epoch= ..." or "rtc: ACPM not there: not read".
 *
 * Sources (facts only, nothing copied; GPL-2.0): Samsung's kernel for the S9 (lineage-24.0, android_kernel_samsung_exynos9810):
 * include/linux/mfd/samsung/rtc-s2mp.h (the register layout without CONFIG_RTC_DRV_S2MP, the CTRL bits),
 * drivers/rtc/rtc-s2mps18.c (s2m_data_to_tm: the fields' masks, the year since 2000, the month 1-12; s2m_rtc_init_reg:
 * the reset test; s2m_rtc_update: RUDR), drivers/mfd/s2mps18_core.c (I2C_ADDR_RTC 0x02, every access an ACPM request),
 * arch/arm64/configs/exynos9810-starlte_defconfig (CONFIG_RTC_DRV_S2MPS18=y, no CONFIG_RTC_DRV_S2MP); S-Boot's and
 * Linux's log on the phone (runs/auto-210855/last_kmsg).
 *
 * Status: compiled; NOT RUN on the phone. The ACPM path is the one the GPU and touch drivers have run on the phone; the
 * RTC block's registers have not been read by this kernel before.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_acpm.h>

extern void loader_log_line(const char *line);

#define RTC_CTRL        0x00
#define RTC_UPDATE      0x02
#define RTC_CAPSEL      0x03
#define RTC_MSEC        0x04
#define RTC_SEC         0x05
#define RTC_MIN         0x06
#define RTC_HOUR        0x07
#define RTC_WEEK        0x08
#define RTC_DAY         0x09
#define RTC_MONTH       0x0A
#define RTC_YEAR        0x0B
#define RTC_REGS        12              /* 0x00 to 0x0B */

#define CTRL_BCD        0x01
#define CTRL_24H        0x02

static int rtc_say(const char *format, ...) __printflike(1, 2);

static int
rtc_say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

/* seconds since 1970 of a UTC date (year 1970 and on) */
static uint64_t
rtc_days_secs(uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t min, uint32_t sec)
{
	static const uint16_t before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	uint64_t days = (uint64_t)(year - 1970) * 365 + (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400 +
	    before[month - 1] + (day - 1);
	bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;

	if (month > 2 && leap) {
		days++;
	}
	return days * 86400 + (uint64_t)hour * 3600 + (uint64_t)min * 60 + sec;
}

/* the date of seconds since 1970, for the log */
static void
rtc_date(uint64_t t, uint32_t *y, uint32_t *mo, uint32_t *d, uint32_t *h, uint32_t *mi, uint32_t *s)
{
	uint64_t days = t / 86400, rest = t % 86400;
	uint32_t year = 1970;

	for (;;) {
		uint32_t len = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 366 : 365;

		if (days < len) {
			break;
		}
		days -= len;
		year++;
	}
	*y = year;
	*mo = 1;
	for (; *mo < 12; (*mo)++) {
		uint64_t next = rtc_days_secs(year, *mo + 1, 1, 0, 0, 0) - rtc_days_secs(year, *mo, 1, 0, 0, 0);

		if (days < next / 86400) {
			break;
		}
		days -= next / 86400;
	}
	*d = (uint32_t)days + 1;
	*h = (uint32_t)(rest / 3600);
	*mi = (uint32_t)(rest / 60 % 60);
	*s = (uint32_t)(rest % 60);
}

/*
 * The kernel's time of day at start-up (GenericARMPE.cpp, findBootEpoch): `epoch` is what it has otherwise (epoch=, or
 * the loader's boot-epoch; 0: none). The answer is the clock's time when rtc_pmic=1 and it is valid, else `epoch`.
 */
uint64_t hakios_rtc_boot_time(uint64_t epoch);

uint64_t
hakios_rtc_boot_time(uint64_t epoch)
{
	uint32_t on = 0, year, month, day, hour, min, sec, y, mo, d, h, mi, s;
	uint8_t r[RTC_REGS];
	uint64_t clock, now, up_ns;
	const char *why = NULL;

	if (!PE_parse_boot_argn("rtc_pmic", &on, sizeof(on)) || on == 0) {
		return epoch;
	}
	rtc_say("rtc: S2MPS18 RTC (ACPM, block 2), read only\n");
	if (!hakios_acpm_init(rtc_say)) {
		rtc_say("rtc: ACPM not there: not read; the calendar starts at epoch= %llu\n", epoch);
		return epoch;
	}
	for (uint32_t i = 0; i < RTC_REGS; i++) {
		int error = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_RTC, i, &r[i]);

		if (error != 0) {
			rtc_say("rtc: reading register 0x%02x failed (%d): not used; the calendar starts at epoch= %llu\n", i, error,
			    epoch);
			return epoch;
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &up_ns);
	rtc_say("rtc: CTRL 0x%02x UPDATE 0x%02x CAPSEL 0x%02x MSEC 0x%02x SEC 0x%02x MIN 0x%02x HOUR 0x%02x WEEK 0x%02x "
	    "DAY 0x%02x MONTH 0x%02x YEAR 0x%02x\n", r[RTC_CTRL], r[RTC_UPDATE], r[RTC_CAPSEL], r[RTC_MSEC], r[RTC_SEC],
	    r[RTC_MIN], r[RTC_HOUR], r[RTC_WEEK], r[RTC_DAY], r[RTC_MONTH], r[RTC_YEAR]);

	sec = r[RTC_SEC] & 0x7f;
	min = r[RTC_MIN] & 0x7f;
	hour = r[RTC_HOUR] & 0x1f;
	day = r[RTC_DAY] & 0x1f;
	month = r[RTC_MONTH] & 0x0f;
	year = 2000 + (r[RTC_YEAR] & 0x7f);
	if ((r[RTC_CTRL] & (CTRL_BCD | CTRL_24H)) != CTRL_24H) {
		why = "CTRL is not binary, 24-hour: not set up";
	} else if ((r[RTC_CAPSEL] & 0xf0) != 0xf0) {
		why = "CAPSEL[7:4] is not 0xF: the clock was reset";
	} else if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || month < 1 || month > 12) {
		why = "a field out of range";
	}
	if (why != NULL) {
		rtc_say("rtc: not used (%s); the calendar starts at epoch= %llu\n", why, epoch);
		return epoch;
	}
	clock = rtc_days_secs(year, month, day, hour, min, sec);
	now = clock;
	if (up_ns >= 2 * NSEC_PER_SEC && up_ns <= 600 * NSEC_PER_SEC) {
		now += up_ns / NSEC_PER_SEC - 1;
	}
	rtc_say("rtc: the clock says %u-%02u-%02u %02u:%02u:%02u (%llu); uptime %llu s: +%llu s since S-Boot read it: %llu\n",
	    year, month, day, hour, min, sec, clock, up_ns / NSEC_PER_SEC, now - clock, now);
	if (year < 2024 || year > 2099) {
		why = "the year is not 2024-2099: the clock was never set";
	} else if (epoch != 0 && now < epoch) {
		why = "it is behind epoch=, the time the image was made";
	}
	if (why != NULL) {
		rtc_say("rtc: not used (%s); the calendar starts at epoch= %llu\n", why, epoch);
		return epoch;
	}
	rtc_date(now, &y, &mo, &d, &h, &mi, &s);
	rtc_say("rtc: using the clock: %u-%02u-%02u %02u:%02u:%02u UTC (epoch= %llu was %llu s behind)\n", y, mo, d, h, mi, s,
	    epoch, epoch != 0 ? now - epoch : 0);
	return now;
}
