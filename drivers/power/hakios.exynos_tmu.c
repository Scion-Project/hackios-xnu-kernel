/*
 * The Galaxy S9's temperatures: the Exynos 9810's TMU, read by the kernel itself, published, and a stop for a phone that
 * gets too hot. Nothing without boot-arg tmu=N, and then only its first N steps, each said in the log before it is taken.
 *
 * Why. Under Android the S9 is kept cool by Linux's thermal framework and Samsung's APM firmware (ACPM): they slow the
 * big cores from 83 degrees, take them away at 96, slow the GPU from 88. HakiOS has none of that. With bootcpu=4 cpus=8
 * the four M3s run at the loader's clock, the Mali-G72 at whatever mali=N set, and nothing would notice the heat. This
 * driver is the S9's counterpart of the POCO's drivers/power/hakios.thermal.c (whose sysctls it also fills, so that
 * whatever reads those on the POCO reads the same here) and battguard.c's way of ending a run.
 *
 * The hardware (hakios.exynos_tmu.h has every register, field and line of the BSP it comes from). Two TMU blocks:
 * TMU_TOP at 0x10070000 (the BIG zone: sensors 1, 2, 3, 4, 6; LITTLE: sensor 5) and TMU_SUB at 0x10074000 (G3D: sensor 1;
 * ISP: sensor 2). Each sensor gives a 9-bit code, all the time while the block's CORE_EN is set, in CURRENT_TEMP
 * (0x40/0x44/0x48). The codes are made degrees with each sensor's fuses, which the block shows as its TRIMINFO registers
 * (one per sensor: [8:0] the code at 25 degrees, [17:9] the code at 85; bit 23 of TRIMINFO0 says one- or two-point
 * calibration), as exynos9810_tmu_initialize() and code_to_temp_with_sensorinfo() do. Android itself never reads the codes
 * this way in normal use: with CONFIG_EXYNOS_ACPM_THERMAL (the S9's defconfig) the temperature comes from the APM firmware
 * over IPC (exynos9810_tmu_read -> exynos_acpm_tmu_set_read_temp). The register path used here is the BSP's own all_temp
 * sysfs file (all_temp_show), which reads the same CURRENT_TEMP registers with the same arithmetic. The APM's IPC is not
 * used: it needs Linux's TMU_IPC_INIT first, and its "balance" mode for BIG is the firmware's and not written down; this
 * driver takes the hottest sensor of each zone, which is never cooler than Android's figure.
 *
 * The clock. The TMU's bus clock is CMU_PERIS's BUSIF_TMU PCLK (gate +0x2014, Q-channel +0x3004 of 0x10020000;
 * cmucal-sfr.c:36, 1656, 1675, 7016-7018, 7070-7073). A gate forced shut (MANUAL set, CG_VAL clear) would make a read of
 * the TMU stop the bus, so step 1 reads the gate first and does not touch the TMU behind a closed one (tmu_ungate=1 opens
 * it, read back).
 *
 * THE STEPS (tmu=N does the first N):
 *   1  reads only, once at start and then every tmu_poll ms: the CMU gate; of each block CONTROL (CORE_EN, THERM_TRIP_EN,
 *      TRIP_MODE), CONTROL1, STATUS, AVG_CON, TRIM0, EMUL_CON; of each sensor of the four zones (and sensor 0) its TRIMINFO,
 *      its calibration, its code and temperature, its RISE7/RISE6 thresholds and interrupt enables as the boot left them
 *      (Android writes its "hot" trip, 115 degrees, as RISE7: a value there says the boot loader armed the block's own
 *      thermal trip); then a summary line every tmu_log seconds and at once when the hottest zone passes a multiple of 5
 *      from 60, or the point where Android would start to cool that zone
 *   2  publishes: sysctl kern.hakios.temp.* (big_mc, little_mc, g3d_mc, isp_mc, max_mc in millidegrees; hottest, sensors,
 *      reads, battery_c) and the POCO's kern.hakios.thermal_max_mc, thermal_hottest, thermal_sensors, thermal_reads, and
 *      hakios_thermal_max_mc() for other drivers; -273000 is "not read"
 *   3  acts: when a zone reads at least tmu_stop degrees (110 by default, 5 under Android's "hot" trip of 115, and never
 *      above it), or the battery (hakios_battery_temperature(): the MAX77705 driver's thermistor, -273 while not known) at least
 *      tmu_batt_stop (60), three reads in a row, it says so loudly and ends the run: the data volume saved, the drivers
 *      given back, and the kernel hands the phone to the loader (loader_return), which stops every core (the M3s among
 *      them) and restarts the SoC into recovery with the log kept in pstore; a restart powers the GPU off with the rest.
 *      With tmu_off=1 and no charger reported it powers the phone off instead: PS_HOLD low (PMU 0x14060000 +0x330C bit 8,
 *      as Samsung's sec_power_off() and exynos_power_off() do: include/linux/sec_ext.h:33, drivers/staging/samsung/
 *      sec_reboot.c:135, drivers/soc/samsung/exynos-reboot.c:83, 252), which loses the log (pstore is RAM), and if the
 *      phone still runs a second later (a charger holds it up; Samsung then reboots into charging), the loader's way.
 *      Samsung asks the APM to stop (exynos_acpm_reboot) before PS_HOLD; this does not, so tmu_off is the less sure way.
 * Off the steps: tmu_enable=1 starts a block whose CORE_EN the boot left clear, the way exynos9810_tmu_control() does
 * (VREF/SLOPE from TRIMINFO0/1, AVG_CON from TRIMINFO2, both COUNTER_VALUEs 0x028A, TRIM0 from TMU_TOP's TRIMINFO3/4/5,
 * then CORE_EN), every write read back, but WITHOUT THERM_TRIP_EN and without any threshold or interrupt: a hardware trip
 * armed against thresholds nobody wrote could reset the phone at once. A block already running is left alone.
 *
 * What it never does: write a PMIC, a threshold, an interrupt enable, the emulation register, or a clock rate. The writes
 * there are: tmu_ungate's CG_VAL, tmu_enable's TMU configuration, and tmu_off's PS_HOLD.
 *
 * Status: WRITTEN, NOT YET RUN on the phone, nor in QEMU (it has no such block: tmu= must not be given there). The kernel
 * builds with it in and out; the arithmetic is checked on the host (tools/exynos-tmu-test.sh) against the BSP's own
 * formulas and the dt's constants. Not known: whether S-Boot leaves the TMU running and its PCLK open (step 1 says), whether
 * the APM, which owns the TMU under Android, changes its registers while HakiOS runs, and the BIG zone's "balance" mode.
 *
 * Boot-args:
 *   tmu=<1-3>            the steps: 1 reads, 2 publishes, 3 acts (0, or none: nothing)
 *   tmu_poll=<ms>        how often to read (200 to 60000; 1000, the BIG zone's polling-delay)
 *   tmu_log=<s>          a summary this often (0: only on a rise; 60)
 *   tmu_stop=<C>         step 3's limit for the SoC's zones (40 to 115; 110)
 *   tmu_batt_stop=<C>    step 3's limit for the battery (0: not watched; 40 to 80, the BSP's temp_highlimit; 60)
 *   tmu_off=1            step 3 powers the phone off (PS_HOLD) when no charger is reported, instead of the loader's restart
 *   tmu_enable=1         start a block the boot left stopped (CORE_EN only; never the hardware trip)
 *   tmu_ungate=1         open the TMU's PCLK gate if the boot left it forced shut
 */
#include <stdint.h>
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "hakios.exynos_tmu.h"
#include "hakios.thermal_provider.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern void loader_return(void);
extern void hakios_var_save_on_return(void);
extern void hakios_adreno_stop(void);
extern void hakios_wifi_stop(void);
extern void hakios_adsp_stop(void);
extern int hakios_battery_temperature(void);  /* drivers/power/hakios.battery.c; a stand-in says -273 */
extern int kernel_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
extern void delay(int usec);

SYSCTL_DECL(_kern_hakios);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[600];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

#define CMU_PERIS_PA            0x10020000u     /* cmucal-sfr.c:36 */
#define CMU_PERIS_TMU_GATE      0x2014u         /* :1656 CLK_CON_GAT_GOUT_BLK_PERIS_UID_BUSIF_TMU_IPCLKPORT_PCLK */
#define CMU_PERIS_TMU_QCH       0x3004u         /* :1675 QCH_CON_BUSIF_TMU_QCH */
#define GATE_MANUAL             (1u << 20)      /* :7017 */
#define GATE_CG_VAL             (1u << 21)      /* :7016 */
#define PMU_PA                  0x14060000u     /* flexpmu_cal_system_exynos9810.h:39 */
#define PMU_PS_HOLD_CONTROL     0x330Cu         /* sec_ext.h:33 */
#define PS_HOLD_HIGH            (1u << 8)       /* sec_reboot.c:135, exynos-reboot.c:252 */
#define NOT_READ_MC             (-273000)
#define STRIKES                 3

static vm_offset_t block_va[2], cmu_va;
static const uint32_t block_pa[2] = { TMU_TOP_PA, TMU_SUB_PA };
static const char *const block_name[2] = { "TMU_TOP", "TMU_SUB" };
static struct tmu_cal cal[2][8];
static uint32_t steps, poll_ms = 1000, log_seconds = 60, stop_celsius = 110, batt_stop_celsius = 60, power_off;
static uint64_t last_log, reads;
static int strikes, last_step;
static bool stopping, warned_cools[TMU_NZONES];
static thread_call_t call;

/* step 2's values (sysctl) */
static int temp_zone_mc[TMU_NZONES] = { NOT_READ_MC, NOT_READ_MC, NOT_READ_MC, NOT_READ_MC };
static int temp_max_mc = NOT_READ_MC, temp_reads, temp_battery_c = -273;
static char temp_hottest[16] = "", temp_sensors[400] = "";

SYSCTL_NODE(_kern_hakios, OID_AUTO, temp, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "the Exynos 9810's TMU (drivers/power/hakios.exynos_tmu.c, tmu=2)");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, big_mc, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_zone_mc[0], 0, "the big cores' hottest sensor, millidegrees");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, little_mc, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_zone_mc[1], 0, "the little cores' sensor, millidegrees");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, g3d_mc, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_zone_mc[2], 0, "the GPU's sensor, millidegrees");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, isp_mc, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_zone_mc[3], 0, "the camera ISP's sensor, millidegrees");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, max_mc, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_max_mc, 0, "the hottest zone, millidegrees (-273000: none read)");
SYSCTL_STRING(_kern_hakios_temp, OID_AUTO, hottest, CTLFLAG_RD | CTLFLAG_LOCKED, temp_hottest, 0, "the hottest zone's name");
SYSCTL_STRING(_kern_hakios_temp, OID_AUTO, sensors, CTLFLAG_RD | CTLFLAG_LOCKED, temp_sensors, 0,
    "every sensor in degrees: \"big1=41.0 big2=40.5 ... battery=31\"");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, reads, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_reads, 0, "how many times the sensors were read");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, battery_c, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_battery_c, 0,
    "the battery's temperature as the battery driver says it (-273: not known)");

/* the POCO's names, kern.hakios.thermal_* and hakios_thermal_max_mc() (hakios.thermal_provider.c), read these at step 2 */
static const struct hakios_thermal_provider provider = {
	.name = "exynos_tmu",
	.max_mc = &temp_max_mc,
	.hottest = temp_hottest,
	.sensors = temp_sensors,
	.reads = &temp_reads,
};

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
text_of(char *out, size_t size, int32_t mc)
{
	int32_t tenth = mc / 100, whole = tenth / 10, frac = tenth % 10;

	snprintf(out, size, "%s%d.%d", (tenth < 0 && whole == 0) ? "-" : "", whole, frac < 0 ? -frac : frac);
}

static uint32_t
code_of(unsigned block, unsigned sensor)
{
	return (rd(block_va[block], tmu_temp_reg(sensor)) >> tmu_temp_shift(sensor)) & 0x1ffu;
}

/* step 1's look at what the boot left, once */
static void
describe(void)
{
	char one[16], two[16];

	for (unsigned b = 0; b < 2; b++) {
		vm_offset_t va = block_va[b];
		uint32_t ctrl = rd(va, TMU_CONTROL), t0 = rd(va, TMU_TRIMINFO(0));

		say("tmu: 1: %s %#x: CONTROL %08x (CORE_EN %u, THERM_TRIP_EN %u, TRIP_MODE %u, VREF %u, SLOPE %u), CONTROL1 %08x "
		    "(NUM_PROBE %u, LPI %u), STATUS %08x, AVG_CON %08x, COUNTER %08x/%08x, TRIM0 %08x, EMUL_CON %08x; %s-point calibration\n",
		    block_name[b], block_pa[b], ctrl, ctrl & TMU_CTRL_CORE_EN ? 1 : 0, ctrl & TMU_CTRL_TRIP_EN ? 1 : 0,
		    TMU_CTRL_TRIP_MODE(ctrl), (ctrl >> TMU_CTRL_VREF_SHIFT) & 0x1f, (ctrl >> TMU_CTRL_SLOPE_SHIFT) & 0xf,
		    rd(va, TMU_CONTROL1), TMU_CON1_NUM_PROBE(rd(va, TMU_CONTROL1)), rd(va, TMU_CONTROL1) & TMU_CON1_LPI ? 1 : 0,
		    rd(va, TMU_STATUS), rd(va, TMU_AVG_CON), rd(va, TMU_COUNTER_VALUE0), rd(va, TMU_COUNTER_VALUE1), rd(va, TMU_TRIM0),
		    rd(va, TMU_EMUL_CON), TMU_TRIM_CAL_TYPE(t0) ? "two" : "one");
		if (rd(va, TMU_EMUL_CON) & TMU_EMUL_ENABLE) {
			say("tmu: 1: %s: EMULATION IS ON: its readings are a made-up temperature, not the chip's\n", block_name[b]);
		}
		for (unsigned s = 0; s < 8; s++) {
			uint16_t used = 0;

			for (unsigned z = 0; z < TMU_NZONES; z++) {
				if (tmu_zones[z].block == b && (tmu_zones[z].sensors & (1u << s))) {
					used = 1;
				}
			}
			if (!used && s != 0) {
				continue;
			}
			uint32_t trim = rd(va, TMU_TRIMINFO(s)), code = code_of(b, s), thd = rd(va, tmu_thd_reg(s));
			uint32_t r7 = (thd >> 16) & 0x1ff, r6 = thd & 0x1ff;
			const struct tmu_cal *c = &cal[b][s];

			text_of(one, sizeof(one), tmu_code_to_mc(c, code));
			text_of(two, sizeof(two), tmu_code_to_mc(c, r7));
			say("tmu: 1: %s sensor %u%s: TRIMINFO %08x (25 C = %u, 85 C = %u%s), code %u = %s C; RISE7 %u (%s C) RISE6 %u, "
			    "INTEN %08x\n", block_name[b], s, used ? "" : " (in no zone)", trim, c->error1, c->error2,
			    c->fell_back == 1 ? ", fuse empty: the dt's 55" : c->fell_back == 2 ? ", second point unusable: one-point" : "",
			    code, one, r7, r7 ? two : "-", r6, rd(va, tmu_inten_reg(s)));
		}
	}
}

static bool
read_int(const char *name, int *value)
{
	size_t size = sizeof(*value);

	return kernel_sysctlbyname(name, value, &size, NULL, 0) == 0 && size == sizeof(*value);
}

static void
power_off_or_return(void)
{
	int charger = 1;

	if (power_off && read_int("kern.hakios.battery_charger_present", &charger) && !charger) {
		vm_offset_t pmu = ml_io_map(PMU_PA + (PMU_PS_HOLD_CONTROL & ~0xfffu), 0x1000);

		if (pmu != 0) {
			uint32_t off = PMU_PS_HOLD_CONTROL & 0xfffu, v = rd(pmu, off);

			say("tmu: 3: PS_HOLD_CONTROL %08x: PS_HOLD low, the phone off (this log is lost with it)\n", v);
			wr(pmu, off, v & ~PS_HOLD_HIGH);
			delay(1000 * 1000);
			say("tmu: 3: still running a second after PS_HOLD (%08x): the loader's way instead\n", rd(pmu, off));
		}
	} else if (power_off) {
		say("tmu: 3: tmu_off=1, but a charger is attached or not known (%d): the loader's way\n", charger);
	}
	hakios_var_save_on_return();
	hakios_adreno_stop();
	hakios_wifi_stop();
	hakios_adsp_stop();
	loader_return();
}

static void
look(void)
{
	int32_t zone_mc[TMU_NZONES], hottest = INT32_MIN;
	unsigned hot = 0, good = 0;
	char list[sizeof(temp_sensors)], one[16], item[32];

	list[0] = 0;
	for (unsigned z = 0; z < TMU_NZONES; z++) {
		const struct tmu_zone *zone = &tmu_zones[z];

		zone_mc[z] = INT32_MIN;
		for (unsigned s = 0; s < 8; s++) {
			if (!(zone->sensors & (1u << s))) {
				continue;
			}
			uint32_t code = code_of(zone->block, s);
			int32_t mc = tmu_code_to_mc(&cal[zone->block][s], code);

			if (!tmu_reading_ok(code, mc)) {
				snprintf(item, sizeof(item), "%s%u=? ", zone->name, s);
			} else {
				text_of(one, sizeof(one), mc);
				snprintf(item, sizeof(item), "%s%u=%s ", zone->name, s, one);
				good++;
				if (mc > zone_mc[z]) {
					zone_mc[z] = mc;
				}
			}
			strlcat(list, item, sizeof(list));
		}
		if (zone_mc[z] != INT32_MIN && zone_mc[z] > hottest) {
			hottest = zone_mc[z];
			hot = z;
		}
	}

	int battery = hakios_battery_temperature();     /* the battery driver's last reading; -273 before one (or without it) */
	bool battery_known = battery > -40 && battery < 100;

	if (battery_known) {
		snprintf(item, sizeof(item), "battery=%d", battery);
		strlcat(list, item, sizeof(list));
	}
	reads++;
	if (steps >= 2) {
		for (unsigned z = 0; z < TMU_NZONES; z++) {
			temp_zone_mc[z] = zone_mc[z] == INT32_MIN ? NOT_READ_MC : zone_mc[z];
		}
		temp_reads = (int)reads;
		temp_battery_c = battery_known ? battery : -273;
		strlcpy(temp_sensors, list, sizeof(temp_sensors));
		if (good != 0) {
			temp_max_mc = hottest;
			strlcpy(temp_hottest, tmu_zones[hot].name, sizeof(temp_hottest));
		}
	}
	if (good == 0) {
		if (reads == 1 || reads % 60 == 0) {
			say("tmu: no sensor gives a reading (%s): is the block running? (step 1's CORE_EN; tmu_enable=1)\n", list);
		}
		strikes = 0;
		return;
	}

	uint64_t now = mach_absolute_time(), ns;
	int step = hottest >= 60000 ? hottest / 5000 : 0;       /* a step each 5 degrees from 60 */

	absolutetime_to_nanoseconds(now - last_log, &ns);
	text_of(one, sizeof(one), hottest);
	if (step > last_step || last_log == 0 || (log_seconds != 0 && ns / NSEC_PER_SEC >= log_seconds)) {
		say("tmu: %u sensors read; hottest %s at %s C; %s\n", good, tmu_zones[hot].name, one, list);
		last_log = now;
	}
	last_step = step;
	for (unsigned z = 0; z < TMU_NZONES; z++) {
		if (zone_mc[z] != INT32_MIN && zone_mc[z] >= tmu_zones[z].android_cools_mc && !warned_cools[z]) {
			warned_cools[z] = true;
			text_of(one, sizeof(one), zone_mc[z]);
			say("tmu: WARNING: %s at %s C: Android would be cooling it from %d C; nothing here does%s\n", tmu_zones[z].name, one,
			    tmu_zones[z].android_cools_mc / 1000, steps >= 3 ? "" : " (and without tmu=3 nothing stops the run)");
		} else if (zone_mc[z] != INT32_MIN && zone_mc[z] < tmu_zones[z].android_cools_mc - 5000) {
			warned_cools[z] = false;
		}
	}

	if (steps < 3) {
		return;
	}
	bool soc_hot = hottest >= (int32_t)stop_celsius * 1000;
	bool batt_hot = batt_stop_celsius != 0 && battery_known && battery >= (int)batt_stop_celsius;

	strikes = (soc_hot || batt_hot) ? strikes + 1 : 0;
	if (strikes > 0 && strikes < STRIKES) {
		say("tmu: 3: %s %s C (limit %u), battery %d C (limit %u): %d of %d reads in a row\n", tmu_zones[hot].name, one, stop_celsius,
		    battery, batt_stop_celsius, strikes, STRIKES);
	}
	if (strikes >= STRIKES && !stopping) {
		stopping = true;
		say("\n--- tmu: TOO HOT: %s reads %s C (tmu_stop=%u, Android's hot trip %d), battery %d C (tmu_batt_stop=%u), %d reads "
		    "in a row ---\n--- tmu: %s ---\n--- tmu: sensors: %s ---\n", tmu_zones[hot].name, one, stop_celsius,
		    TMU_ANDROID_HOT_MC / 1000, battery, batt_stop_celsius, STRIKES,
		    power_off ? "powering the phone off (tmu_off=1)" : "ending the run: back to the loader, which restarts into recovery",
		    list);
		power_off_or_return();
	}
}

static void
tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	look();
	clock_interval_to_deadline(poll_ms, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}

/* tmu_enable=1: exynos9810_tmu_control()'s configuration and CORE_EN, never TRIP_EN, thresholds or interrupts */
static void
start_block(unsigned b)
{
	vm_offset_t va = block_va[b], top = block_va[TMU_BLOCK_TOP];
	uint32_t ctrl = rd(va, TMU_CONTROL), v, want;

	if (ctrl & TMU_CTRL_CORE_EN) {
		say("tmu: enable: %s already runs (CONTROL %08x): left alone\n", block_name[b], ctrl);
		return;
	}
	say("tmu: enable: %s is stopped (CONTROL %08x): configuring it as Android does, without the hardware trip\n", block_name[b], ctrl);
	want = ctrl & ~((0x1fu << TMU_CTRL_VREF_SHIFT) | (0xfu << TMU_CTRL_SLOPE_SHIFT) | TMU_CTRL_TRIP_EN);
	want |= TMU_TRIM_FIELD(rd(va, TMU_TRIMINFO(0))) << TMU_CTRL_VREF_SHIFT;
	want |= TMU_TRIM_FIELD4(rd(va, TMU_TRIMINFO(1))) << TMU_CTRL_SLOPE_SHIFT;
	wr(va, TMU_CONTROL, want);
	v = rd(va, TMU_CONTROL1);
	wr(va, TMU_CONTROL1, v & ~TMU_CON1_LPI);
	uint32_t mode = TMU_TRIM_AVG_MODE(rd(va, TMU_TRIMINFO(2)));
	v = rd(va, TMU_AVG_CON) & ~(TMU_AVG_MODE_MASK | TMU_AVG_DEM);
	wr(va, TMU_AVG_CON, mode ? (v | mode | TMU_AVG_DEM) : v);
	wr(va, TMU_COUNTER_VALUE0, (rd(va, TMU_COUNTER_VALUE0) & ~0xffffu) | TMU_SENSE_VALUE);
	wr(va, TMU_COUNTER_VALUE1, (rd(va, TMU_COUNTER_VALUE1) & ~0xffff0000u) | (TMU_SENSE_VALUE << 16));
	v = rd(va, TMU_TRIM0) & ~((0xfu << 20) | (0xfu << 12) | (0xfu << 8));
	v |= TMU_TRIM_FIELD4(rd(top, TMU_TRIMINFO(3))) << 20;
	v |= TMU_TRIM_FIELD4(rd(top, TMU_TRIMINFO(4))) << 12;
	v |= TMU_TRIM_FIELD4(rd(top, TMU_TRIMINFO(5))) << 8;
	wr(va, TMU_TRIM0, v);
	wr(va, TMU_CONTROL, rd(va, TMU_CONTROL) | TMU_CTRL_CORE_EN);
	delay(10 * 1000);
	say("tmu: enable: %s now CONTROL %08x (wanted %08x), CONTROL1 %08x, AVG_CON %08x, COUNTER %08x/%08x, TRIM0 %08x (wanted %08x)\n",
	    block_name[b], rd(va, TMU_CONTROL), want | TMU_CTRL_CORE_EN, rd(va, TMU_CONTROL1), rd(va, TMU_AVG_CON),
	    rd(va, TMU_COUNTER_VALUE0), rd(va, TMU_COUNTER_VALUE1), rd(va, TMU_TRIM0), v);
}

__startup_func
static void
exynos_tmu_init(void)
{
	uint32_t value = 0, enable = 0, ungate = 0;

	if (!PE_parse_boot_argn("tmu", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	steps = steps > 3 ? 3 : steps;
	if (PE_parse_boot_argn("tmu_poll", &value, sizeof(value)) && value >= 200 && value <= 60000) {
		poll_ms = value;
	}
	if (PE_parse_boot_argn("tmu_log", &value, sizeof(value)) && value <= 86400) {
		log_seconds = value;
	}
	if (PE_parse_boot_argn("tmu_stop", &value, sizeof(value)) && value >= 40 && value <= TMU_ANDROID_HOT_MC / 1000) {
		stop_celsius = value;
	}
	if (PE_parse_boot_argn("tmu_batt_stop", &value, sizeof(value)) && (value == 0 || (value >= 40 && value <= 80))) {
		batt_stop_celsius = value;
	}
	(void)PE_parse_boot_argn("tmu_off", &power_off, sizeof(power_off));
	(void)PE_parse_boot_argn("tmu_enable", &enable, sizeof(enable));
	(void)PE_parse_boot_argn("tmu_ungate", &ungate, sizeof(ungate));
	say("tmu: steps 1-%u (1 reads, 2 publishes kern.hakios.temp, 3 acts); every %u ms%s\n", steps, poll_ms,
	    steps >= 3 ? "" : "; NOTHING STOPS A HOT RUN below tmu=3");
	if (steps >= 3) {
		say("tmu: 3: the run ends at %u C on any zone (Android's hot trip is %d) or %u C on the battery%s, %d reads in a row; %s\n",
		    stop_celsius, TMU_ANDROID_HOT_MC / 1000, batt_stop_celsius, batt_stop_celsius ? "" : " (off)", STRIKES,
		    power_off ? "then the phone is powered off (PS_HOLD) when no charger is attached" : "then the loader restarts the phone");
	}

	say("tmu: 1: reading CMU_PERIS's TMU gate (%#x +%#x) and Q-channel (+%#x)\n", CMU_PERIS_PA, CMU_PERIS_TMU_GATE, CMU_PERIS_TMU_QCH);
	cmu_va = ml_io_map(CMU_PERIS_PA, 0x4000);
	if (cmu_va == 0) {
		say("tmu: could not map CMU_PERIS; off\n");
		return;
	}
	uint32_t gate = rd(cmu_va, CMU_PERIS_TMU_GATE);
	bool closed = (gate & GATE_MANUAL) && !(gate & GATE_CG_VAL);

	say("tmu: 1: gate %08x (MANUAL %u, CG_VAL %u)%s, Q-channel %08x (ENABLE %u, CLOCK_REQ %u)\n", gate, gate & GATE_MANUAL ? 1 : 0,
	    gate & GATE_CG_VAL ? 1 : 0, closed ? ": CLOSED" : ": open", rd(cmu_va, CMU_PERIS_TMU_QCH), rd(cmu_va, CMU_PERIS_TMU_QCH) & 1,
	    (rd(cmu_va, CMU_PERIS_TMU_QCH) >> 1) & 1);
	if (closed && ungate) {
		say("tmu: 1: tmu_ungate=1: CG_VAL set\n");
		wr(cmu_va, CMU_PERIS_TMU_GATE, gate | GATE_CG_VAL);
		closed = !(rd(cmu_va, CMU_PERIS_TMU_GATE) & GATE_CG_VAL);
		say("tmu: 1: gate now %08x\n", rd(cmu_va, CMU_PERIS_TMU_GATE));
	}
	if (closed) {
		say("tmu: the TMU's clock is shut: its registers are not touched (tmu_ungate=1 opens it); off\n");
		return;
	}
	for (unsigned b = 0; b < 2; b++) {
		block_va[b] = ml_io_map(block_pa[b], TMU_SIZE);
		if (block_va[b] == 0) {
			say("tmu: could not map %s at %#x; off\n", block_name[b], block_pa[b]);
			return;
		}
	}
	for (unsigned b = 0; b < 2; b++) {
		uint32_t t0 = rd(block_va[b], TMU_TRIMINFO(0));

		for (unsigned s = 0; s < 8; s++) {
			cal[b][s] = tmu_cal_of(rd(block_va[b], TMU_TRIMINFO(s)), t0);
		}
	}
	describe();
	if (enable) {
		start_block(TMU_BLOCK_TOP);
		start_block(TMU_BLOCK_SUB);
	}
	if (steps >= 2) {
		say("tmu: 2: publishing kern.hakios.temp.* and kern.hakios.thermal_*\n");
		if (hakios_thermal_register(&provider) != 0) {
			say("tmu: 2: kern.hakios.thermal_* are %s's already (both drivers started); kern.hakios.temp.* only\n",
			    hakios_thermal_provider_name());
		}
	}
	call = thread_call_allocate(tick, NULL);
	tick(NULL, NULL);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, exynos_tmu_init);
