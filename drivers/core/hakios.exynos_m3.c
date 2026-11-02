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
 * Also here: cohtest=N, a check of the clusters' cache coherency once the M3s run (at the end of this file).
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
extern void hakios_esslog_start(void);
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <mach/mach_time.h>
#include <os/atomic_private.h>
#include <sys/param.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
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
	hakios_esslog_start();          /* (esslog=1) the log kept for TWRP from here: a fault on the new core is caught */
	say("m3: 3: CPU_ON for cpu %d (MPIDR 0x%x) next\n", cpu, mpidr);
	return 0;
}

/*
 * cohtest=N: is the big cluster in the same coherency domain as the A55s once it is started? (off by default)
 *
 * Why. With the M3s up the screen shows tearing, picture errors and text that is blank for a while and then comes back,
 * with no fault and no GPU error; with the two A55s alone it does not. The picture is drawn on one core (Mesa's
 * software side in backboardd, wherever the scheduler puts it), copied by another, scaled onto the panel by the
 * fb_threads presenters and read by the GPU: if a write made on one cluster is not seen, or not cleaned to memory, from
 * the other, this is what it would look like.
 *
 * What the source says (Samsung's kernel for the S9; no line of it here): nothing in it turns the big cluster's snooping
 * on. There is no CCI or DSU driver and no snoop register for the 9810: the cluster's power-up is the APM firmware's
 * (cal-if/exynos9810/flexpmu_cal_cpu_exynos9810.h: cluster1_on is one write of MAILBOX_APM2AP bit 24; the CPU entries
 * only unmask PMU interrupts), the core's own start is EL3's PSCI CPU_ON (the dt's psci node, cpu_on 0xC4000003), and
 * exynos-powermode.c only asks for cluster power-down through cal_cluster_disable. The 8895's pmucal (cal-if/exynos8895)
 * shows what that firmware does there: clocks, Q-channels and CLUSTER1_CPUSEQUENCER_OPTION, nothing called snoop. So
 * on a Samsung boot the cluster joins the coherent interconnect inside EL3/APM, and Linux runs both clusters as one
 * Outer Shareable domain (XNU's own normal memory is Outer Shareable too, proc_reg.h SH_OUTER_MEMORY). Whether that
 * happened here, after CPU_ON on a boot whose PMU fields were written by m3_prep rather than by Samsung's kernel, is
 * what this measures. That this kernel does not fall over (its locks, its run queues and TLB maintenance are shared
 * between the clusters) argues for coherency; a fault in the paths that only the picture uses, cache maintenance by VA
 * and memory written outside the caches, would not crash anything.
 *
 * How. One thread is bound to each processor; each says its MPIDR, MIDR, CTR_EL0 (the line sizes), CLIDR, SCTLR (the
 * caches on) and TCR. Then one A55 (MPIDR Aff1 0) and one M3 (Aff1 1) are given these in turn. Every command and answer
 * goes through a Device-memory (uncached) alias of the test's pages, so a failure of coherency cannot hide or hang the
 * test itself, and "memory has" is read through the same alias. Each step has a time limit; nothing waits for ever.
 *   0  the method: the A55 writes cached, cleans by VA to the point of coherency itself; memory has it
 *   1  ping-pong: the two take turns 100000 times on a cached line; each round a counter and 256 bytes of data
 *      (four lines) written with a store-release, read with a load-acquire. Stale data under a new counter, or a counter
 *      that is not seen in 500 ms, is incoherence
 *   2  the M3 writes cached, nothing more; the A55 reads cached (the snoop the other way: is a dirty line seen?)
 *   3  the same, the other way round
 *   4  the M3 writes cached; the A55 cleans by VA (DC CVAC, broadcast); does memory have it? (a picture drawn on an M3,
 *      made ready for the GPU or the display from an A55)
 *   5  the same, the other way round
 *   6  the M3 writes cached and cleans by VA itself; does memory have it?
 *   7  memory changed under the M3's cached copy (a GPU's write); the A55 invalidates by VA (DC IVAC, broadcast); does
 *      the M3 read the new data? (7c, the control: the M3 invalidates itself)
 * Each step is said with the number of 64-byte lines (or rounds) that were wrong, then a verdict line.
 *
 * Boot-args: cohtest=N runs it N times (1-10; 0 or none: never), the first cohtest_delay seconds after start-up (30,
 * when the screen is drawing), then every 30 s. It uses 16 KB and two processors for a second or so.
 *
 * Expected on a coherent phone (the numbers are examples; with bootcpu=2 the A55s are MPIDR 0x2 and 0x3):
 *   cohtest: cohtest=1: the clusters' coherency is measured 30 s after start-up
 *   cohtest: 16 KB at 0x..., cached at 0x... and uncached (Device) at 0x...
 *   cohtest: run 1 of 1: 6 processors
 *   cohtest: cpu 2 (ran on 2): MPIDR 0x100 MIDR 0x... CTR 0x... (D line 64, I line 64) CLIDR 0x... SCTLR 0x... (C 1) TCR 0x...
 *            (one such line a processor: the line sizes and SCTLR.C of an M3 next to an A55's)
 *   cohtest: A55 cpu 0 (MPIDR 0x2) and M3 cpu 2 (MPIDR 0x100); 128 lines of 64 bytes a step
 *   cohtest: 0 the method (A55 writes, cleans; memory has it?): 0 of 128 lines wrong: ok
 *   cohtest: 1 ping-pong A55 <-> M3: 100000 of 100000 rounds, 0 stale (first at count 0), 0 stuck (seen 0 waiting
 *            for 0), 900 ns a round trip: ok
 *   cohtest: 2 M3 writes cached; A55 reads cached: 0 of 128 lines wrong: ok
 *   ... 3, 4, 5, 6, 7 and 7c the same
 *   cohtest: verdict: COHERENT: all 9 checks passed (A55 cpu 0, M3 cpu 2)
 * and when not, FAIL on the steps that failed and "cohtest: verdict: NOT COHERENT: n of 9 checks failed". Reading it:
 * 1, 2 or 3 failing is the clusters not snooping each other at all (the kernel itself would be in trouble soon); 4 or
 * 5 alone, cleaning by VA not broadcast to the other cluster; 6 alone, the M3's clean not reaching memory; 7 alone
 * (7c passing), invalidation by VA not broadcast: a picture the GPU wrote read stale on an M3. Any of 4-7 fits the
 * screen's symptoms without any crash.
 */

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern kern_return_t kernel_thread_start(thread_continue_t continuation, void *parameter, thread_t *new_thread);
extern void thread_deallocate(thread_t thread);
extern void *cpu_to_processor(int cpu);
extern void *thread_bind(void *processor);
extern int thread_block(void *continuation);
extern int cpu_number(void);
extern unsigned int real_ncpus;
extern void delay(int usec);

#define COH_PAGES       4
#define COH_SIZE        (COH_PAGES * 4096)
#define COH_CTL         0x0000          /* the commands: Device alias only */
#define COH_PING        0x1000          /* the ping-pong line and its data */
#define COH_BUF         0x2000          /* 8 KB for the other steps */
#define COH_BUF_SIZE    0x2000
#define COH_LINES       (COH_BUF_SIZE / 64)
#define COH_WORDS       32              /* the ping-pong's data: 256 bytes, four lines */
#define COH_ROUNDS      100000u
#define COH_MAXCPU      8

enum { OP_NONE, OP_ID, OP_PING, OP_WRITE, OP_WRITE_CLEAN, OP_CLEAN, OP_INVAL, OP_READ, OP_DEV_WRITE, OP_DEV_COUNT, OP_EXIT };

struct coh_ctl {                        /* one per processor, 256 bytes apart, in the Device alias */
	uint32_t seq, op, arg, done;
	uint64_t r[8];
};

static uint32_t coh_runs, coh_delay = 30, coh_run;
static vm_offset_t coh_va;              /* cached */
static volatile uint8_t *coh_dev;       /* the same pages, Device memory */
static thread_call_t coh_call;

static inline volatile struct coh_ctl *
coh_ctl(int cpu)
{
	return (volatile struct coh_ctl *)(coh_dev + COH_CTL + 256 * (unsigned)cpu);
}

static inline volatile uint32_t *
coh_abort(void)
{
	return (volatile uint32_t *)(coh_dev + COH_CTL + 256 * COH_MAXCPU);
}

static inline uint64_t
coh_pattern(uint64_t v, uint32_t i)
{
	return (v * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)i << 3) ^ 0x5a5a;
}

static inline uint32_t
coh_line(void)
{
	return 4u << ((__builtin_arm_rsr64("CTR_EL0") >> 16) & 0xf);   /* DminLine on this core */
}

static void
coh_dc(int op, vm_offset_t va, size_t len)
{
	uint32_t line = coh_line();

	__asm__ volatile ("dsb sy" ::: "memory");
	for (vm_offset_t a = va & ~(vm_offset_t)(line - 1); a < va + len; a += line) {
		if (op == OP_CLEAN) {
			__asm__ volatile ("dc cvac, %0" :: "r"(a) : "memory");
		} else {
			__asm__ volatile ("dc ivac, %0" :: "r"(a) : "memory");
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline bool
coh_late(uint64_t start, uint64_t ms)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	return ns > ms * NSEC_PER_MSEC;
}

/* the ping-pong: role 0 writes the odd counts, role 1 the even ones */
static void
coh_ping(int role, volatile struct coh_ctl *c)
{
	uint64_t *seq = (uint64_t *)(coh_va + COH_PING);
	volatile uint64_t *data = (volatile uint64_t *)(coh_va + COH_PING + 64);
	uint64_t next = role == 0 ? 1 : 2, stale = 0, stuck = 0, first = 0, start = mach_absolute_time();
	uint32_t rounds = 0;

	while (next <= 2ull * COH_ROUNDS && *coh_abort() == 0) {
		uint64_t wait = mach_absolute_time(), now;
		bool late = false;

		while ((now = os_atomic_load(seq, acquire)) != next - 1) {
			if (*coh_abort() != 0) {
				break;
			}
			if (coh_late(wait, 500)) {
				late = true;
				break;
			}
		}
		if (now != next - 1) {
			if (!late) {
				break;          /* the other one gave up */
			}
			stuck++;
			*coh_abort() = 1;
			__asm__ volatile ("dsb sy" ::: "memory");
			c->r[3] = now;
			c->r[4] = next - 1;
			break;
		}
		for (uint32_t i = 0; i < COH_WORDS; i++) {
			if (data[i] != coh_pattern(next - 1, i)) {
				if (stale++ == 0) {
					first = next - 1;
				}
				break;
			}
		}
		for (uint32_t i = 0; i < COH_WORDS; i++) {
			data[i] = coh_pattern(next, i);
		}
		os_atomic_store(seq, next, release);
		rounds++;
		next += 2;
	}
	c->r[0] = rounds;
	c->r[1] = stale;
	c->r[2] = stuck;
	c->r[5] = first;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &c->r[6]);
}

static void
coh_worker(void *arg, __unused wait_result_t wr)
{
	int cpu = (int)(uintptr_t)arg;
	volatile struct coh_ctl *c = coh_ctl(cpu);
	uint32_t last = 0;
	uint64_t idle = mach_absolute_time();

	(void)thread_bind(cpu_to_processor(cpu));
	(void)thread_block(NULL);
	for (;;) {
		uint32_t seq = c->seq;
		uint32_t op, tag;
		uint64_t wrong = 0;

		if (seq == last) {
			if (coh_late(idle, 20000)) {
				return;         /* the controller is gone: nothing more for this processor */
			}
			delay(50);      /* a sleep, not a spin: the others (and the controller) may run here */
			continue;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		last = seq;
		op = c->op;
		tag = c->arg;
		switch (op) {
		case OP_ID:
			c->r[0] = __builtin_arm_rsr64("MPIDR_EL1");
			c->r[1] = __builtin_arm_rsr64("MIDR_EL1");
			c->r[2] = __builtin_arm_rsr64("CTR_EL0");
			c->r[3] = __builtin_arm_rsr64("CLIDR_EL1");
			c->r[4] = __builtin_arm_rsr64("SCTLR_EL1");
			c->r[5] = __builtin_arm_rsr64("TCR_EL1");
			c->r[6] = (uint64_t)cpu_number();
			break;
		case OP_PING:
			coh_ping((int)tag, c);
			break;
		case OP_WRITE:
		case OP_WRITE_CLEAN:
			for (uint32_t i = 0; i < COH_BUF_SIZE / 8; i++) {
				((volatile uint64_t *)(coh_va + COH_BUF))[i] = coh_pattern(tag, i);
			}
			__asm__ volatile ("dsb sy" ::: "memory");
			if (op == OP_WRITE_CLEAN) {
				coh_dc(OP_CLEAN, coh_va + COH_BUF, COH_BUF_SIZE);
			}
			break;
		case OP_CLEAN:
		case OP_INVAL:
			coh_dc((int)op, coh_va + COH_BUF, COH_BUF_SIZE);
			break;
		case OP_READ:
		case OP_DEV_COUNT:
			for (uint32_t l = 0; l < COH_LINES; l++) {
				for (uint32_t w = 0; w < 8; w++) {
					uint32_t i = l * 8 + w;
					uint64_t v = op == OP_READ ? ((volatile uint64_t *)(coh_va + COH_BUF))[i] :
					    ((volatile uint64_t *)(coh_dev + COH_BUF))[i];

					if (v != coh_pattern(tag, i)) {
						wrong++;
						break;
					}
				}
			}
			c->r[0] = wrong;
			break;
		case OP_DEV_WRITE:
			for (uint32_t i = 0; i < COH_BUF_SIZE / 8; i++) {
				((volatile uint64_t *)(coh_dev + COH_BUF))[i] = coh_pattern(tag, i);
			}
			__asm__ volatile ("dsb sy" ::: "memory");
			break;
		default:
			break;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		c->done = seq;
		__asm__ volatile ("dsb sy" ::: "memory");
		idle = mach_absolute_time();
		if (op == OP_EXIT) {
			return;
		}
	}
}

/* a command to one processor's thread, and its answer within ms; false: none came */
static bool
coh_send(int cpu, uint32_t op, uint32_t arg)
{
	volatile struct coh_ctl *c = coh_ctl(cpu);
	uint32_t seq = c->seq + 1;

	c->op = op;
	c->arg = arg;
	__asm__ volatile ("dsb sy" ::: "memory");
	c->seq = seq;
	__asm__ volatile ("dsb sy" ::: "memory");
	return true;
}

static bool
coh_wait(int cpu, uint64_t ms)
{
	volatile struct coh_ctl *c = coh_ctl(cpu);
	uint64_t start = mach_absolute_time();

	while (c->done != c->seq) {
		if (coh_late(start, ms)) {
			return false;
		}
		delay(50);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	return true;
}

static bool
coh_do(int cpu, uint32_t op, uint32_t arg)
{
	(void)coh_send(cpu, op, arg);
	if (!coh_wait(cpu, 1000)) {
		say("cohtest: cpu %d did not answer command %u in 1 s\n", cpu, op);
		return false;
	}
	return true;
}

/* one step: memory or a cache set up by the others, then the count of wrong lines from `reader` */
static int
coh_count(const char *what, int reader, uint32_t op, uint32_t tag, uint32_t *failed, uint32_t *checks)
{
	uint64_t wrong;

	if (!coh_do(reader, op, tag)) {
		return -1;
	}
	wrong = coh_ctl(reader)->r[0];
	(*checks)++;
	if (wrong != 0) {
		(*failed)++;
	}
	say("cohtest: %s: %llu of %u lines wrong: %s\n", what, wrong, COH_LINES, wrong ? "FAIL" : "ok");
	return wrong != 0;
}

static void
coh_main(__unused void *arg, __unused wait_result_t wr)
{
	int ncpu = (int)MIN(real_ncpus, COH_MAXCPU), a = -1, b = -1;
	bool alive[COH_MAXCPU] = { false };
	uint32_t failed = 0, checks = 0, tag = (coh_run + 1) << 8;

	say("cohtest: run %u of %u: %d processors\n", coh_run + 1, coh_runs, ncpu);
	for (int i = 0; i < ncpu; i++) {
		volatile struct coh_ctl *c = coh_ctl(i);

		c->seq = c->done = 0;
		c->op = OP_NONE;
	}
	*coh_abort() = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
	for (int i = 0; i < ncpu; i++) {
		thread_t t;

		if (cpu_to_processor(i) == NULL || kernel_thread_start(coh_worker, (void *)(uintptr_t)i, &t) != KERN_SUCCESS) {
			say("cohtest: cpu %d: no thread\n", i);
			continue;
		}
		thread_deallocate(t);
	}
	for (int i = 0; i < ncpu; i++) {
		volatile struct coh_ctl *c = coh_ctl(i);
		uint64_t mpidr;

		(void)coh_send(i, OP_ID, 0);
		if (!coh_wait(i, 1000)) {
			say("cohtest: cpu %d: no answer (not running?)\n", i);
			continue;
		}
		alive[i] = true;
		mpidr = c->r[0] & 0xffffff;
		say("cohtest: cpu %d (ran on %llu): MPIDR 0x%llx MIDR 0x%llx CTR 0x%llx (D line %u, I line %u) CLIDR 0x%llx "
		    "SCTLR 0x%llx (C %llu) TCR 0x%llx\n", i, c->r[6], mpidr, c->r[1], c->r[2], 4u << ((c->r[2] >> 16) & 0xf),
		    4u << (c->r[2] & 0xf), c->r[3], c->r[4], (c->r[4] >> 2) & 1, c->r[5]);
		if (((mpidr >> 8) & 0xff) == 0 && a < 0) {
			a = i;
		} else if (((mpidr >> 8) & 0xff) == 1 && b < 0) {
			b = i;
		}
	}
	if (a < 0 || b < 0) {
		say("cohtest: no %s running: nothing to compare (m3_prep=3 and cpus= with an M3)\n", a < 0 ? "A55" : "M3");
		goto out;
	}
	say("cohtest: A55 cpu %d (MPIDR 0x%llx) and M3 cpu %d (MPIDR 0x%llx); %u lines of 64 bytes a step\n", a,
	    coh_ctl(a)->r[0] & 0xffffff, b, coh_ctl(b)->r[0] & 0xffffff, COH_LINES);
	for (int i = 0; i < ncpu; i++) {
		if (alive[i] && i != a && i != b) {
			(void)coh_send(i, OP_EXIT, 0);
			alive[i] = false;
		}
	}

	/* 0: the method */
	if (!coh_do(a, OP_WRITE_CLEAN, tag + 0) || coh_count("0 the method (A55 writes, cleans; memory has it?)", a, OP_DEV_COUNT,
	    tag + 0, &failed, &checks) < 0) {
		goto out;
	}

	/* 1: the ping-pong, on a line nobody has cached yet */
	{
		uint64_t *seq = (uint64_t *)(coh_va + COH_PING);
		uint64_t *data = (uint64_t *)(coh_va + COH_PING + 64);
		volatile struct coh_ctl *ca = coh_ctl(a), *cb = coh_ctl(b);
		bool ok;

		*seq = 0;
		for (uint32_t i = 0; i < COH_WORDS; i++) {
			data[i] = coh_pattern(0, i);
		}
		FlushPoC_DcacheRegion(coh_va + COH_PING, 4096);
		(void)coh_send(b, OP_PING, 1);
		(void)coh_send(a, OP_PING, 0);
		ok = coh_wait(a, 5000) && coh_wait(b, 5000);
		checks++;
		if (!ok) {
			*coh_abort() = 1;
			failed++;
			say("cohtest: 1 ping-pong: no end in 5 s: FAIL\n");
		} else {
			uint64_t rounds = ca->r[0] + cb->r[0], stale = ca->r[1] + cb->r[1], stuck = ca->r[2] + cb->r[2];
			uint64_t ns = MAX(ca->r[6], cb->r[6]);
			bool bad = stale != 0 || stuck != 0 || rounds != 2ull * COH_ROUNDS;

			failed += bad;
			say("cohtest: 1 ping-pong A55 <-> M3: %llu of %u rounds, %llu stale (first at count %llu), %llu stuck "
			    "(seen %llu waiting for %llu), %llu ns a round trip: %s\n", rounds / 2, COH_ROUNDS, stale,
			    ca->r[1] ? ca->r[5] : cb->r[5], stuck, ca->r[2] ? ca->r[3] : cb->r[3], ca->r[2] ? ca->r[4] : cb->r[4],
			    rounds ? ns * 2 / rounds : 0, bad ? "FAIL" : "ok");
		}
	}

	/* 2, 3: a dirty line in one cluster, read cached from the other */
	if (!coh_do(b, OP_WRITE, tag + 2) || coh_count("2 M3 writes cached; A55 reads cached", a, OP_READ, tag + 2, &failed,
	    &checks) < 0) {
		goto out;
	}
	if (!coh_do(a, OP_WRITE, tag + 3) || coh_count("3 A55 writes cached; M3 reads cached", b, OP_READ, tag + 3, &failed,
	    &checks) < 0) {
		goto out;
	}
	/* 4, 5: written in one, cleaned by VA from the other: in memory? */
	if (!coh_do(b, OP_WRITE, tag + 4) || !coh_do(a, OP_CLEAN, 0) ||
	    coh_count("4 M3 writes; A55 cleans by VA; memory has it?", a, OP_DEV_COUNT, tag + 4, &failed, &checks) < 0) {
		goto out;
	}
	if (!coh_do(a, OP_WRITE, tag + 5) || !coh_do(b, OP_CLEAN, 0) ||
	    coh_count("5 A55 writes; M3 cleans by VA; memory has it?", a, OP_DEV_COUNT, tag + 5, &failed, &checks) < 0) {
		goto out;
	}
	/* 6: written and cleaned in the M3 */
	if (!coh_do(b, OP_WRITE_CLEAN, tag + 6) ||
	    coh_count("6 M3 writes, cleans by VA itself; memory has it?", a, OP_DEV_COUNT, tag + 6, &failed, &checks) < 0) {
		goto out;
	}
	/* 7: memory changed under the M3's cached copy; the A55 invalidates by VA (7), the M3 itself (7c) */
	for (int own = 0; own < 2; own++) {
		uint32_t old = tag + 0x70 + (uint32_t)own * 2, now = old + 1;

		if (!coh_do(a, OP_CLEAN, 0) || !coh_do(a, OP_INVAL, 0) || !coh_do(b, OP_CLEAN, 0) || !coh_do(b, OP_INVAL, 0) ||
		    !coh_do(a, OP_DEV_WRITE, old) || !coh_do(b, OP_INVAL, 0) ||
		    !coh_do(b, OP_READ, old)) {
			goto out;
		}
		if (coh_ctl(b)->r[0] != 0) {
			say("cohtest: 7%s: (setting up) the M3 did not read what memory had even after its own invalidate: %llu lines "
			    "(not counted; 7c says more)\n", own ? "c" : "", coh_ctl(b)->r[0]);
		}
		if (!coh_do(a, OP_DEV_WRITE, now) || !coh_do(own ? b : a, OP_INVAL, 0) ||
		    coh_count(own ? "7c memory changed; M3 invalidates by VA itself; M3 reads new data? (control)" :
		    "7 memory changed under the M3's copy; A55 invalidates by VA; M3 reads new data?", b, OP_READ, now, &failed,
		    &checks) < 0) {
			goto out;
		}
	}
	if (failed == 0) {
		say("cohtest: verdict: COHERENT: all %u checks passed (A55 cpu %d, M3 cpu %d)\n", checks, a, b);
	} else {
		say("cohtest: verdict: NOT COHERENT: %u of %u checks failed (A55 cpu %d, M3 cpu %d)\n", failed, checks, a, b);
	}
out:
	for (int i = 0; i < ncpu; i++) {
		if (alive[i]) {
			(void)coh_send(i, OP_EXIT, 0);
		}
	}
	if (++coh_run < coh_runs) {
		uint64_t deadline;

		clock_interval_to_deadline(30, NSEC_PER_SEC, &deadline);
		thread_call_enter_delayed(coh_call, deadline);
	}
}

static void
coh_tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	thread_t t;

	if (coh_va == 0) {
		vm_offset_t va = 0;

		if (kmem_alloc_contig(kernel_map, &va, COH_SIZE, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			say("cohtest: no memory; off\n");
			return;
		}
		FlushPoC_DcacheRegion(va, COH_SIZE);
		coh_dev = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)kvtophys(va), COH_SIZE, VM_WIMG_IO);
		if (coh_dev == NULL) {
			say("cohtest: no uncached alias of its pages; off\n");
			return;
		}
		coh_va = va;
		say("cohtest: 16 KB at %#llx, cached at %#lx and uncached (Device) at %p\n", kvtophys(va), va, coh_dev);
	}
	if (kernel_thread_start(coh_main, NULL, &t) == KERN_SUCCESS) {
		thread_deallocate(t);
	}
}

__startup_func
static void
coh_init(void)
{
	uint64_t deadline;

	if (!PE_parse_boot_argn("cohtest", &coh_runs, sizeof(coh_runs)) || coh_runs == 0) {
		return;
	}
	coh_runs = MIN(coh_runs, 10u);
	(void)PE_parse_boot_argn("cohtest_delay", &coh_delay, sizeof(coh_delay));
	coh_call = thread_call_allocate(coh_tick, NULL);
	say("cohtest: cohtest=%u: the clusters' coherency is measured %u s after start-up\n", coh_runs, coh_delay);
	clock_interval_to_deadline(coh_delay, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(coh_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, coh_init);
