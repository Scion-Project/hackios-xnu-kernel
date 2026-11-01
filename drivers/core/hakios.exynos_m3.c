/*
 * The Galaxy S9's big cores, four Exynos M3s (MPIDR 0x100-0x103, "CLUSTER1" in the PMU): what the PMU has to hold
 * before the firmware's CPU_ON can start one of them (m3_prep=N, the first N of three steps, each logged before it
 * is taken). psci_smp_start_cpu() asks hakios_psci_cpu_on_prepare() before every CPU_ON (patch 1508).
 *
 * Why. Samsung's kernel writes six CLUSTER1 fields of the PMU when it starts, before any other core is started: the
 * CPUCL1 entries of its "lpm init" sequence (pmucal_lpm_init, run by cal_if_init -> pmucal_system_init from the clock
 * driver's init, which runs in time_init, before smp_init). S-Boot leaves other values there (read on an S9+ by the
 * mainline port: +0x1088 = 0xC7150000, the five OPTION fields 1). The mainline S9+ port found that without the six
 * writes CPU_ON for CPU4-7 answers 0 and the cores never arrive, and that with them the M3s come up at boot
 * (starting them again after a CPU_OFF still fails there). Under Silicium and Carbon the same CPU_ON does not answer
 * at all (patch 1505's log, round 5b): Samsung's EL3 seems to wait for the core it starts (an A55's CPU_ON answers
 * 76 us after the call, and that core is in the kernel 9 us later). Nothing of Samsung's kernel runs before this one,
 * so nothing wrote them.
 *
 *   CPU_QCH_CLUSTER1_NONCPU_SYS_PWR_REG   PMU +0x1088  [31:24] = 0x8D
 *   MEMORY_CLUSTER1_CPU0_OPTION           PMU +0x2114  [1:0] = 2
 *   MEMORY_CLUSTER1_CPU1_OPTION           PMU +0x2180  [1:0] = 2
 *   MEMORY_CLUSTER1_CPU2_OPTION           PMU +0x21EC  [1:0] = 2
 *   MEMORY_CLUSTER1_CPU3_OPTION           PMU +0x2258  [1:0] = 2
 *   MEMORY_CLUSTER1_NONCPU_OPTION         PMU +0x2384  [1:0] = 2
 *
 * Read and logged, never written, to say what state the cluster is in:
 *   CLUSTER1_CPUn_CONFIGURATION / _STATUS     PMU +0x2000 / +0x2004 + 8n   [3:0], 0xF on (on the 9810 the big cluster
 *                                             is the PMU's first, at +0x2000; the A55s' cores follow at +0x2020)
 *   CLUSTER1_NONCPU_CONFIGURATION / _STATUS   PMU +0x2040 / +0x2044; CLUSTER0_NONCPU_STATUS +0x204C
 *   MEMORY_CLUSTER1_NONCPU_STATUS             PMU +0x2380 (the cluster's shared cache, bit 0)
 *   RESET_SEQUENCER_CONFIGURATION             PMU +0x0500; CPU_RESET_DISABLE_FROM_SOFTRESET +0x041C, _FROM_WDTRESET +0x0414
 *   cpucl1's and cpucl0's DVFS rates          ACPM DVFS, indexes 3 and 2 (ACPM_DVFS_CPUCL1 is 0x0B040003)
 *   vdd_cpucl1, vdd_cpucl0                    the S2MPS18's BUCK2 (B2CTRL 0x1F, B2OUT1 0x20) and BUCK3 (0x22, 0x23):
 *                                             300 mV + 6.25 mV a step, the enable in [7:6] of CTRL
 * None of these is in the big cluster's own power domain: the PMU and the APM are always on. CMU_CPUCL1 (0x1D100000),
 * which is in it, is not touched.
 *
 * Steps (m3_prep=N does the first N; 0, the default, does nothing here, and CPU_ON for an M3 is made as before):
 *   1  everything above read and logged, before the first CPU_ON of the boot (with cpus=2 bootcpu=2, core 3's)
 *   2  ... and the six fields written as Samsung's kernel writes them (only those bits), each read back, then read
 *      again. A field that does not read back stops it there: nothing more is written, and no M3 is started
 *   3  ... and CPU_ON for the M3s the kernel was given (cpus=3 bootcpu=2: A55s 2 and 3 and one M3), each after
 *      another reading of the state
 * Below 3 an M3 the kernel was given (cpus=3 or more) is refused, not started.
 *
 * Sources: Samsung's kernel for the S9 (lineage-24.0, android_kernel_samsung_exynos9810):
 * drivers/soc/samsung/cal-if/exynos9810/flexpmu_cal_system_exynos9810.h (pmucal_lpm_init: the six writes),
 * flexpmu_cal_cpu_exynos9810.h (the CLUSTERn_CPUm and NONCPU status registers), cal-if/pmucal_system.c and cal-if.c
 * (pmucal_system_init from cal_if_init), drivers/clk/samsung/clk-exynos9810.c (cal_if_init from the clock init),
 * drivers/soc/samsung/exynos-pmu.c (the CONFIGURATION/STATUS layout, the clusters' order on the 9810, +0x2380),
 * exynos-reboot.c (the reset registers), include/dt-bindings/clock/exynos9810.h (ACPM_DVFS_CPUCL1),
 * include/linux/mfd/samsung/s2mps18-private.h and the phone's device tree (BUCK2 is vdd_cpucl1, BUCK3 vdd_cpucl0).
 * The mainline Linux port to the S9+ (github.com/SheroAbi/galaxy-s9plus-mainline-linux, scripts/build/patch_tree.py,
 * "M3 cluster: vendor PMU defaults before PSCI CPU_ON", and docs/06-known-issues.md): what S-Boot leaves and what
 * happens without the writes. All GPL: read for these facts, not one line copied.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);

int hakios_psci_cpu_on_prepare(int cpu, uint32_t mpidr);

#define PMU_PA                  0x14060000UL
#define PMU_SIZE                0x3000

#define PMU_RESET_DIS_WDT       0x0414          /* CPU_RESET_DISABLE_FROM_WDTRESET */
#define PMU_RESET_DIS_SOFT      0x041C          /* CPU_RESET_DISABLE_FROM_SOFTRESET */
#define PMU_RESET_SEQ_CONFIG    0x0500          /* RESET_SEQUENCER_CONFIGURATION */
#define PMU_CL1_CPU_CONFIG(n)   (0x2000 + 8 * (n))
#define PMU_CL1_CPU_STATUS(n)   (0x2004 + 8 * (n))
#define PMU_CL0_CPU_STATUS(n)   (0x2024 + 8 * (n))
#define PMU_CL1_NONCPU_CONFIG   0x2040
#define PMU_CL1_NONCPU_STATUS   0x2044
#define PMU_CL0_NONCPU_STATUS   0x204C
#define PMU_CL1_L2_STATUS       0x2380          /* MEMORY_CLUSTER1_NONCPU_STATUS */

#define ACPM_DVFS_CPUCL0        2u
#define ACPM_DVFS_CPUCL1        3u
#define S2MPS18_PMIC            1u              /* the PMIC's block: ACPM's "type" */
#define S2MPS18_B2CTRL          0x1F
#define S2MPS18_B2OUT1          0x20
#define S2MPS18_B3CTRL          0x22
#define S2MPS18_B3OUT1          0x23

#define STEPS                   3

static const struct {
	const char *name;
	uint32_t offset, mask, value;
} m3_fields[] = {
	{ "CPU_QCH_CLUSTER1_NONCPU_SYS_PWR_REG", 0x1088, 0xFF000000u, 0x8D000000u },
	{ "MEMORY_CLUSTER1_CPU0_OPTION",         0x2114, 0x3u,        0x2u },
	{ "MEMORY_CLUSTER1_CPU1_OPTION",         0x2180, 0x3u,        0x2u },
	{ "MEMORY_CLUSTER1_CPU2_OPTION",         0x21EC, 0x3u,        0x2u },
	{ "MEMORY_CLUSTER1_CPU3_OPTION",         0x2258, 0x3u,        0x2u },
	{ "MEMORY_CLUSTER1_NONCPU_OPTION",       0x2384, 0x3u,        0x2u },
};
#define M3_FIELDS               (sizeof(m3_fields) / sizeof(m3_fields[0]))

static vm_offset_t pmu;
static uint32_t m3_steps;
static bool m3_read_args, m3_looked, m3_written;

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

static uint32_t
pmu_rd(uint32_t offset)
{
	return *(volatile uint32_t *)(pmu + offset);
}

static void
pmu_wr(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(pmu + offset) = value;
	__builtin_arm_dsb(0xF);         /* DSB SY */
}

/* an S2MPS18 buck's OUT1 code, in mV */
static uint32_t
buck_mv(uint8_t code)
{
	return 300 + (uint32_t)code * 25 / 4;
}

/* step 1: what says the big cluster's state, read only */
static void
m3_report(int cpu, uint32_t mpidr, const char *when)
{
	uint32_t khz1 = 0, khz0 = 0;
	uint8_t b2ctrl = 0, b2out = 0, b3ctrl = 0, b3out = 0;
	int e1, e0, eb2c, eb2o, eb3c, eb3o;

	say("m3: cpu %d (MPIDR 0x%x), %s: CLUSTER1 cores' CONFIGURATION/STATUS %x/%x %x/%x %x/%x %x/%x, NONCPU %x/%x, "
	    "shared cache %x; CLUSTER0 cores' STATUS %x %x %x %x, NONCPU %x\n", cpu, mpidr, when,
	    pmu_rd(PMU_CL1_CPU_CONFIG(0)), pmu_rd(PMU_CL1_CPU_STATUS(0)), pmu_rd(PMU_CL1_CPU_CONFIG(1)),
	    pmu_rd(PMU_CL1_CPU_STATUS(1)), pmu_rd(PMU_CL1_CPU_CONFIG(2)), pmu_rd(PMU_CL1_CPU_STATUS(2)),
	    pmu_rd(PMU_CL1_CPU_CONFIG(3)), pmu_rd(PMU_CL1_CPU_STATUS(3)), pmu_rd(PMU_CL1_NONCPU_CONFIG),
	    pmu_rd(PMU_CL1_NONCPU_STATUS), pmu_rd(PMU_CL1_L2_STATUS), pmu_rd(PMU_CL0_CPU_STATUS(0)),
	    pmu_rd(PMU_CL0_CPU_STATUS(1)), pmu_rd(PMU_CL0_CPU_STATUS(2)), pmu_rd(PMU_CL0_CPU_STATUS(3)),
	    pmu_rd(PMU_CL0_NONCPU_STATUS));
	say("m3: RESET_SEQUENCER_CONFIGURATION %x, CPU_RESET_DISABLE_FROM_SOFTRESET %x, _FROM_WDTRESET %x\n",
	    pmu_rd(PMU_RESET_SEQ_CONFIG), pmu_rd(PMU_RESET_DIS_SOFT), pmu_rd(PMU_RESET_DIS_WDT));
	for (unsigned i = 0; i < M3_FIELDS; i++) {
		uint32_t v = pmu_rd(m3_fields[i].offset);

		say("m3: %s (PMU +0x%x) = %08x: %s (Samsung's kernel: %08x under %08x)\n", m3_fields[i].name,
		    m3_fields[i].offset, v, (v & m3_fields[i].mask) == m3_fields[i].value ? "as Samsung's kernel leaves it" :
		    "NOT as Samsung's kernel leaves it", m3_fields[i].value, m3_fields[i].mask);
	}
	if (!hakios_acpm_init(say)) {
		say("m3: ACPM is not there: the DVFS rates and the bucks are not read\n");
		return;
	}
	e1 = hakios_acpm_dvfs_get(ACPM_DVFS_CPUCL1, &khz1);
	e0 = hakios_acpm_dvfs_get(ACPM_DVFS_CPUCL0, &khz0);
	eb2c = hakios_acpm_pmic_read(S2MPS18_PMIC, S2MPS18_B2CTRL, &b2ctrl);
	eb2o = hakios_acpm_pmic_read(S2MPS18_PMIC, S2MPS18_B2OUT1, &b2out);
	eb3c = hakios_acpm_pmic_read(S2MPS18_PMIC, S2MPS18_B3CTRL, &b3ctrl);
	eb3o = hakios_acpm_pmic_read(S2MPS18_PMIC, S2MPS18_B3OUT1, &b3out);
	say("m3: DVFS cpucl1 %u kHz (error %d), cpucl0 %u kHz (error %d); vdd_cpucl1 BUCK2 CTRL %02x OUT1 %02x = %u mV "
	    "(errors %d %d), vdd_cpucl0 BUCK3 CTRL %02x OUT1 %02x = %u mV (errors %d %d)\n", khz1, e1, khz0, e0,
	    b2ctrl, b2out, buck_mv(b2out), eb2c, eb2o, b3ctrl, b3out, buck_mv(b3out), eb3c, eb3o);
}

/* step 2: the six fields, as Samsung's kernel writes them before it starts any other core */
static bool
m3_write(void)
{
	for (unsigned i = 0; i < M3_FIELDS; i++) {
		uint32_t before = pmu_rd(m3_fields[i].offset), want, after;

		want = (before & ~m3_fields[i].mask) | m3_fields[i].value;
		say("m3: 2: %s (PMU +0x%x): %08x -> %08x\n", m3_fields[i].name, m3_fields[i].offset, before, want);
		if (want != before) {
			pmu_wr(m3_fields[i].offset, want);
		}
		after = pmu_rd(m3_fields[i].offset);
		if ((after & m3_fields[i].mask) != m3_fields[i].value) {
			say("m3: 2: %s reads %08x after the write: NOT TAKEN; nothing more is written, and no M3 is started\n",
			    m3_fields[i].name, after);
			return false;
		}
	}
	say("m3: 2: the six CLUSTER1 fields hold Samsung's values\n");
	return true;
}

/*
 * Before every CPU_ON (psci_smp_start_cpu(), patch 1508): 0, go ahead; anything else, that core is not to be started.
 * With m3_prep=0 it does nothing and says go ahead. Otherwise the first call, whichever core it is for, reads (and with
 * m3_prep=2 or more writes) the big cluster's fields, so that steps 1 and 2 run with the A55s alone (cpus=2 bootcpu=2:
 * the call for core 3) and change nothing else about the boot; and a call for an M3 (MPIDR 0x100-0x103) is refused
 * unless m3_prep=3 and the six fields took.
 */
int
hakios_psci_cpu_on_prepare(int cpu, uint32_t mpidr)
{
	bool m3 = (mpidr & ~0x3u) == 0x100;

	if (!m3_read_args) {
		m3_read_args = true;
		if (!PE_parse_boot_argn("m3_prep", &m3_steps, sizeof(m3_steps))) {
			m3_steps = 0;
		}
		if (m3_steps > STEPS) {
			m3_steps = STEPS;
		}
	}
	if (m3_steps == 0) {
		return 0;
	}
	if (pmu == 0) {
		pmu = ml_io_map(PMU_PA, PMU_SIZE);
		if (pmu == 0) {
			say("m3: cannot map the PMU (%#lx)%s\n", PMU_PA, m3 ? ": this M3 is not started" : "");
			return m3 ? -1 : 0;
		}
	}
	if (!m3_looked) {
		m3_looked = true;
		say("m3: m3_prep=%u: 1: the big cluster's state, before CPU_ON for cpu %d (MPIDR 0x%x)\n", m3_steps, cpu, mpidr);
		m3_report(cpu, mpidr, "as found");
		if (m3_steps >= 2) {
			say("m3: 2: writing the six CLUSTER1 fields of Samsung's start-up sequence (pmucal_lpm_init), each read back\n");
			if (m3_write()) {
				m3_written = true;
				m3_report(cpu, mpidr, "after the writes");
			}
		}
	} else if (m3) {
		m3_report(cpu, mpidr, "before its CPU_ON");
	}
	if (!m3) {
		return 0;
	}
	if (m3_steps < 3 || !m3_written) {
		say("m3: cpu %d (MPIDR 0x%x) is an M3 and is not started: %s\n", cpu, mpidr, m3_steps < 3 ?
		    "m3_prep=3 starts the M3s (steps 1 and 2 are for cpus=2 bootcpu=2)" : "the six fields did not take");
		return -1;
	}
	say("m3: 3: CPU_ON for cpu %d (MPIDR 0x%x) next\n", cpu, mpidr);
	return 0;
}
