/*
 * hakios.exynos_tee.c: the Galaxy S9's secure OS (Kinibi) asked whether it is there, and at the next step started and
 * stopped there. Off unless asked, in nobody's default line; without tee_probe= this file does nothing at all.
 *
 *   tee_probe=1   one read-only question: the info fastcall, MC_FC_INFO (0xFF000002) with extended id 0 (MCI_VERSION)
 *   tee_probe=2   that, and if it answers "not initialised" the start, as far as the secure OS's own state:
 *                 MC_FC_INIT, the init values, one empty N-SIQ, then MC_FC_INFO and N_YIELD in turn until the state is
 *                 no longer 0, for at most TEE_POLL_S. Then nothing more
 *
 * Every call's four returned registers are logged (after TEE_LOG_CALLS of them, only a call whose answer differs from
 * that call's last). What is NOT here, on purpose: MC_FC_MEM_TRACE (Android's driver makes it first, for its log; nothing
 * in its source makes the init call depend on it), MC_FC_SWAP_CPU, any MCP command, any session, TA or driver, the
 * daemon, the storage proxy, RPMB, EFS. The secure interrupt (SPI 233) is not taken: the state is polled.
 *
 * Android's driver asks MC_FC_INFO only after its init call, so what it does before is what tee_probe=1 was for: on
 * phone 2 on 4 November the answer was x0=0xff000002 x1=0 x2=0 x3=0x10006 after 28 us. tee_probe=2 then, on the same
 * phone: INIT x1=0 x2=1 (LPAE), N_SIQ x1=0, state 0, ten N_YIELDs x1=0, state 2 with MCI 0x10006; the kernel ran on to
 * its 300 s limit. That is one boot of one phone, and nothing has asked the secure OS for anything since. A hang, an
 * SError or a halted secure OS stay possible, so:
 *   the core       the secure OS's fastcalls belong to the core it runs on, core 0 until it is told otherwise (Android's
 *                  worker is pinned there). On the S9 with cpus=8 that core starts as the boot loader's and is the
 *                  kernel's only after ufs=3's switch (patch 1513). The thread waits for it, moves to each core the
 *                  scheduler has and reads MPIDR_EL1 there, and stays on the one that reads 0x81000000 (the A55s' whole
 *                  MPIDR is what Android's table has). Every call checks it again. No such core within TEE_WAIT_S: no call
 *   the memory     the MCI block is a static buffer of this file: not from XNU's allocators, never moved, physically
 *                  contiguous (checked), cacheable like Android's (a mapping of memory outside XNU's RAM here is
 *                  write-combined, hakios_physmap_wc, which the secure OS's side could not be coherent with). It holds
 *                  nothing but this block, and a run's end restarts the SoC, so nothing else ever gets that memory
 *                  while the secure OS knows it
 *   a hang         another thread, bound to another core, panics if a call has not come back after TEE_HANG_S: with core
 *                  0 gone (the loader's core) nothing feeds the Exynos watchdog after that, so it restarts into TWRP.
 *                  The log is then in last_kmsg (esslog=1)
 *
 * The block (Android's order, offsets from its start; 2 x 16 KB here, 29912 bytes used):
 *   0       the queue to the secure OS: { write count, read count, size = 64 } and 64 notifications of 8 bytes
 *   524     the queue from it, the same
 *   1048    the MCP buffer, 160 bytes: 16 of flags { schedule, sleep request and state, timeout, reserved }, then the
 *           message, here the init values { flags, irq, time's offset, its length, sessions' offset, their length }
 *   1208    256 inter-world sessions of 112 bytes (only reserved here: nothing is ever opened)
 *   29880   the time: { wall seconds, wall nanoseconds, monotonic seconds, monotonic nanoseconds }, u64s; set before the init call
 *
 * The init call: x0 0xFF000001, x1 the block's physical address' low half, x2 (its high half << 16) | the queues' length,
 * x3 (the MCP buffer's offset << 16) | its length; back x1 the return code, x2 flags (bit 0 LPAE). The init values
 * follow it, as flags TIME | IWP, irq the S-SIQ's GIC number (SPI 233 + 32).
 *
 * Sources for the facts (Samsung's kernel, Scion-Project/android_kernel_samsung_exynos9810 at e5906667068e, GPL, read
 * for these facts, not one line copied): drivers/gud/gud-exynos9810/MobiCoreDriver: mci/mcifc.h, mci/mcinq.h,
 * mci/mcimcp.h, mci/mcitime.h, mci/mciiwp.h (the numbers and the layout), fastcall.c (the registers; the worker on core
 * 0), nq.c (the block's order, the sequence, the poll), main.c (what comes first), platform.h (the core's MPIDR);
 * arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts (the interrupt).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/hakios.input.h>
#include <sys/param.h>
#include <sys/systm.h>

extern void loader_log_line(const char *line);
extern int vsnprintf(char *, size_t, const char *, va_list) __printflike(3, 0);
extern void IOSleep(unsigned milliseconds);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
extern void *cpu_to_processor(int cpu);
extern void *thread_bind(void *processor);
extern int thread_block(void *continuation);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */

#define MC_FC_INIT              0xFF000001ull
#define MC_FC_INFO              0xFF000002ull
#define MC_SMC_N_YIELD          3ull
#define MC_SMC_N_SIQ            4ull
#define EXT_MCI_VERSION         0ull
#define STATE_NOT_INITIALIZED   0
#define STATE_INITIALIZED       2
#define STATE_HALT              3
#define CORE0_MPIDR             0x81000000ull
#define TEE_WAIT_S              240     /* how long the core may take to be the kernel's */
#define TEE_SETTLE_S            10      /* after it is: the boot's busy start is over */
#define TEE_HANG_S              30      /* one call not back after this: a panic, and the watchdog */
#define TEE_POLL_S              30      /* how long the state may take */
#define TEE_YIELDS              10      /* N_YIELDs between two looks at the state, as Android's */
#define TEE_LOG_CALLS           80

#define MCI_BYTES               (32u * 1024)
#define MCI_QUEUE_ELEMENTS      64u
#define MCI_QUEUE_RX            524u
#define MCI_QUEUES_LEN          1048u
#define MCI_MCP                 1048u
#define MCI_MCP_LEN             160u
#define MCI_INIT_VALUES         (MCI_MCP + 16)
#define MCI_SESSIONS            1208u
#define MCI_SESSIONS_LEN        (256u * 112)
#define MCI_TIME                29880u
#define MCI_TIME_LEN            32u
#define IV_FLAG_TIME            (1u << 1)
#define IV_FLAG_IWP             (1u << 2)
#define SSIQ_GIC_NUMBER         (233u + 32)
_Static_assert(MCI_SESSIONS == MCI_MCP + MCI_MCP_LEN && MCI_TIME == MCI_SESSIONS + MCI_SESSIONS_LEN &&
    MCI_TIME + MCI_TIME_LEN <= MCI_BYTES, "the MCI block's layout");

static uint8_t mci[MCI_BYTES] __attribute__((aligned(16384)));

static volatile uint32_t tee_finished;
static volatile uint64_t call_began;            /* mach_absolute_time of the call under way, 0: none */
static const char * volatile call_name;
static uint32_t calls_logged;

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[256];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	printf("%s", line);
	return length;
}

static inline void
put32(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(void *)(mci + offset) = value;
}

static inline uint32_t
get32(uint32_t offset)
{
	return *(volatile uint32_t *)(void *)(mci + offset);
}

static inline void
put64(uint32_t offset, uint64_t value)
{
	*(volatile uint64_t *)(void *)(mci + offset) = value;
}

static void
bind_to(int cpu)
{
	(void)thread_bind(cpu < 0 ? NULL : cpu_to_processor(cpu));
	(void)thread_block(NULL);             /* over to it now */
}

/* a core the scheduler has: it has been idle at least once (one not yet the kernel's has not) */
static bool
core_is_ours(int cpu)
{
	uint64_t idle = 0;

	return cpu_to_processor(cpu) != NULL && ml_cpu_idle_time(cpu, &idle) && idle != 0;
}

/* The kernel's number of the core that reads CORE0_MPIDR, -1 while there is none. *other: another core of the kernel's. */
static int
find_core0(int *other)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	int found = -1;

	*other = -1;
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		int cpu = (int)topology->cpus[i].cpu_id;
		uint64_t mpidr;

		if (!core_is_ours(cpu)) {
			continue;
		}
		bind_to(cpu);
		mpidr = __builtin_arm_rsr64("MPIDR_EL1");
		bind_to(-1);
		if (mpidr == CORE0_MPIDR) {
			found = cpu;
		} else if (*other < 0) {
			*other = cpu;
		}
	}
	return found;
}

/* The SMC itself: x0 to x3 go through Carbon (EL2) to EL3 and x0 to x3 come back. */
static void
smc4(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t answer[4])
{
	register uint64_t x0 __asm__("x0") = a0;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	answer[0] = x0;
	answer[1] = x1;
	answer[2] = x2;
	answer[3] = x3;
}

/* One call on core 0, watched and logged. False, and no call, off core 0. */
static bool
call(const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t answer[4], uint64_t last[4])
{
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1"), began, took, ns;
	boolean_t enabled;

	if (mpidr != CORE0_MPIDR) {
		say("tee_probe: %s: MPIDR reads %#llx, not %#llx: NO CALL MADE\n", name, mpidr, CORE0_MPIDR);
		return false;
	}
	enabled = ml_set_interrupts_enabled(FALSE);
	__asm__ volatile ("dsb sy" ::: "memory");
	began = mach_absolute_time();
	call_name = name;
	__atomic_store_n(&call_began, began, __ATOMIC_RELEASE);
	__asm__ volatile ("dsb sy" ::: "memory");
	smc4(a0, a1, a2, a3, answer);
	took = mach_absolute_time() - began;
	__atomic_store_n(&call_began, 0, __ATOMIC_RELEASE);
	(void)ml_set_interrupts_enabled(enabled);
	absolutetime_to_nanoseconds(took, &ns);
	if (calls_logged < TEE_LOG_CALLS || last == NULL || memcmp(answer, last, 4 * sizeof(uint64_t)) != 0) {
		calls_logged++;
		say("tee_probe: %s(x1=%#llx x2=%#llx x3=%#llx): %llu us: x0=%#llx x1=%#llx x2=%#llx x3=%#llx\n", name, a1, a2, a3,
		    ns / 1000, answer[0], answer[1], answer[2], answer[3]);
	}
	if (last != NULL) {
		memcpy(last, answer, 4 * sizeof(uint64_t));
	}
	return true;
}

static void
watch_thread(void *arg, __unused wait_result_t wr)
{
	uint64_t limit, began;

	nanoseconds_to_absolutetime((uint64_t)TEE_HANG_S * NSEC_PER_SEC, &limit);
	bind_to((int)(intptr_t)arg);
	while (!__atomic_load_n(&tee_finished, __ATOMIC_ACQUIRE)) {
		began = __atomic_load_n(&call_began, __ATOMIC_ACQUIRE);
		if (began != 0 && mach_absolute_time() - began > limit) {
			say("tee_probe: %s has not come back after %d s: the core is gone; a panic, then the watchdog\n", call_name, TEE_HANG_S);
			IOSleep(500);
			panic("tee_probe: %s has not returned after %d s", call_name, TEE_HANG_S);
		}
		IOSleep(200);
	}
}

/* what Android's driver reads back when the secure OS has halted (nq.c): the same ids, read only, logged */
static void
halted(void)
{
	static const uint32_t ids[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 19, 20, 21, 22 };
	uint64_t answer[4];

	for (unsigned int i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		if (!call("INFO, extended", MC_FC_INFO, ids[i], 0, 0, answer, NULL)) {
			return;
		}
	}
}

/* tee_probe=2: on core 0, the secure OS said state 0. The block, the init call, the init values, an N-SIQ, the poll. */
static void
start(void)
{
	uint64_t base = kvtophys((vm_offset_t)mci), answer[4], last_info[4] = { 0 }, last_yield[4] = { 0 };
	uint64_t deadline, span, yields = 0, looks = 0;
	clock_sec_t seconds;
	clock_nsec_t nanos;

	for (uint32_t offset = 16384; offset < MCI_BYTES; offset += 16384) {
		if (kvtophys((vm_offset_t)mci + offset) != base + offset) {
			say("tee_probe: the block is not contiguous at +%u (%#llx from %#llx): NO INIT\n", offset,
			    kvtophys((vm_offset_t)mci + offset), base);
			return;
		}
	}
	if (base == 0 || (base & 0x3fff) != 0 || (base >> 48) != 0) {
		say("tee_probe: the block at %#llx will not do: NO INIT\n", base);
		return;
	}
	put32(8, MCI_QUEUE_ELEMENTS);
	put32(MCI_QUEUE_RX + 8, MCI_QUEUE_ELEMENTS);
	clock_get_calendar_nanotime(&seconds, &nanos);
	put64(MCI_TIME, (uint64_t)seconds);
	put64(MCI_TIME + 8, (uint64_t)nanos);
	__asm__ volatile ("dsb sy" ::: "memory");
	say("tee_probe: the block at %#llx, %u bytes, kernel %p\n", base, MCI_BYTES, mci);

	if (!call("INIT", MC_FC_INIT, base & 0xffffffffull, ((base >> 32) << 16) | MCI_QUEUES_LEN,
	    ((uint64_t)MCI_MCP << 16) | MCI_MCP_LEN, answer, NULL)) {
		return;
	}
	if (answer[1] != 0) {
		say("tee_probe: INIT returned code %#llx: stopping\n", answer[1]);
		return;
	}
	put32(MCI_INIT_VALUES, IV_FLAG_TIME | IV_FLAG_IWP);
	put32(MCI_INIT_VALUES + 4, SSIQ_GIC_NUMBER);
	put32(MCI_INIT_VALUES + 8, MCI_TIME);
	put32(MCI_INIT_VALUES + 12, MCI_TIME_LEN);
	put32(MCI_INIT_VALUES + 16, MCI_SESSIONS);
	put32(MCI_INIT_VALUES + 20, MCI_SESSIONS_LEN);
	__asm__ volatile ("dsb sy" ::: "memory");

	if (!call("N_SIQ", MC_SMC_N_SIQ, 0, 0, 0, answer, NULL)) {
		return;
	}
	if (answer[1] != 0) {
		say("tee_probe: N_SIQ returned code %#llx: stopping\n", answer[1]);
		return;
	}

	nanoseconds_to_absolutetime((uint64_t)TEE_POLL_S * NSEC_PER_SEC, &span);
	deadline = mach_absolute_time() + span;
	for (;;) {
		uint64_t state;

		if (!call("INFO", MC_FC_INFO, EXT_MCI_VERSION, 0, 0, answer, last_info)) {
			return;
		}
		looks++;
		state = answer[2] & 0xffffffffull;
		if (answer[1] != 0) {
			say("tee_probe: INFO returned code %#llx: stopping\n", answer[1]);
			break;
		}
		if (state == STATE_INITIALIZED) {
			say("tee_probe: state 2 (initialised) after %llu looks and %llu yields; MCI %#llx\n", looks, yields, answer[3]);
			break;
		}
		if (state != STATE_NOT_INITIALIZED) {
			say("tee_probe: state %llu after %llu looks and %llu yields\n", state, looks, yields);
			if (state == STATE_HALT) {
				halted();
			}
			break;
		}
		if (mach_absolute_time() > deadline) {
			say("tee_probe: still state 0 after %d s, %llu looks and %llu yields: stopping\n", TEE_POLL_S, looks, yields);
			break;
		}
		for (int i = 0; i < TEE_YIELDS; i++) {
			if (!call("N_YIELD", MC_SMC_N_YIELD, 0, 0, 0, answer, last_yield)) {
				return;
			}
			yields++;
			if (answer[1] != 0) {
				say("tee_probe: N_YIELD returned code %#llx: stopping\n", answer[1]);
				goto out;
			}
		}
		IOSleep(1);
	}
out:
	__asm__ volatile ("dsb sy" ::: "memory");
	say("tee_probe: the block now: to it write %u read %u size %u; from it write %u read %u size %u; MCP flags %#x %#x %d %#x; "
	    "message %#x %#x %#x %#x %#x %#x\n", get32(0), get32(4), get32(8), get32(MCI_QUEUE_RX), get32(MCI_QUEUE_RX + 4),
	    get32(MCI_QUEUE_RX + 8), get32(MCI_MCP), get32(MCI_MCP + 4), (int)get32(MCI_MCP + 8), get32(MCI_MCP + 12),
	    get32(MCI_INIT_VALUES), get32(MCI_INIT_VALUES + 4), get32(MCI_INIT_VALUES + 8), get32(MCI_INIT_VALUES + 12),
	    get32(MCI_INIT_VALUES + 16), get32(MCI_INIT_VALUES + 20));
}

static void
tee_thread(void *arg, __unused wait_result_t wr)
{
	uint32_t on = (uint32_t)(uintptr_t)arg;
	uint64_t answer[4];
	thread_t thread;
	int core0 = -1, other = -1;

	for (unsigned int s = 0; s < TEE_WAIT_S && core0 < 0; s += 2) {
		IOSleep(2000);
		core0 = find_core0(&other);
	}
	if (core0 < 0 || other < 0) {
		say("tee_probe: no core reads MPIDR %#llx after %d s (another core: %d): NO CALL MADE\n", CORE0_MPIDR, TEE_WAIT_S, other);
		return;
	}
	IOSleep(TEE_SETTLE_S * 1000);
	if (kernel_thread_start(watch_thread, (void *)(intptr_t)other, &thread) != KERN_SUCCESS) {
		say("tee_probe: the watching thread could not be started: NO CALL MADE\n");
		return;
	}
	thread_deallocate(thread);
	say("tee_probe: core 0 is the kernel's cpu %d; a call is watched from cpu %d, for %d s\n", core0, other, TEE_HANG_S);
	bind_to(core0);
	if (call("INFO", MC_FC_INFO, EXT_MCI_VERSION, 0, 0, answer, NULL) && on == 2) {
		if (answer[1] == 0 && (answer[2] & 0xffffffffull) == STATE_NOT_INITIALIZED) {
			start();
		} else {
			say("tee_probe: code %#llx, state %#llx, not (0, 0): NO INIT\n", answer[1], answer[2]);
		}
	}
	bind_to(-1);
	__atomic_store_n(&tee_finished, 1, __ATOMIC_RELEASE);
	say("tee_probe: done (%u): the driver's reading: x1 the return code (0 ok), x2 the state (0 not initialised, 1 bad init, "
	    "2 initialised, 3 halted), x3 the extended value (MCI's version); no more calls\n", on);
}

void
hakios_exynos_tee_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("tee_probe", &on, sizeof(on)) || (on != 1 && on != 2)) {
		return;
	}
	say("tee_probe=%u: %s, once core 0 is the kernel's\n", on, on == 1 ? "one read-only MC_FC_INFO" : "MC_FC_INFO, then the start");
	if (kernel_thread_start(tee_thread, (void *)(uintptr_t)on, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		say("tee_probe: the thread could not be started: NO CALL MADE\n");
	}
}
