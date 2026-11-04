/*
 * hakios.exynos_tee.c: tee_probe=1 on the Galaxy S9: one question to the secure OS (Kinibi), asked once, read only.
 *
 * Nothing starts the TEE under HakiOS, and it has never been asked whether it is there. This asks: the info fastcall,
 * MC_FC_INFO (0xFF000002) with extended id 0 (MCI_VERSION) and x2 = x3 = 0, and logs the four registers that come back.
 * It makes no other call: not the init call, not the trace call, no notification, no yield, no core switch, no session,
 * and nothing of the storage. Without tee_probe=1 this file does nothing at all and is in nobody's default line.
 *
 * Android's driver asks this only after its init call, so what the call does before that is not known; a hang or an
 * SError is possible. So:
 *   the core       the secure OS's fastcalls belong to the core it runs on, core 0 until it is told otherwise. On the S9
 *                  with cpus=8 that core starts as the boot loader's, and is the kernel's only after ufs=3's switch (patch
 *                  1513). The thread waits for it, moves to each core the scheduler has and reads MPIDR_EL1 there, and
 *                  asks only on the one that reads 0x81000000 (the A55s' whole MPIDR is what the table has). Never
 *                  there within TEE_WAIT_S: no call, and the log says so.
 *   a hang         another thread, bound to another core, waits TEE_HANG_S for the answer and then panics: with that core
 *                  gone nothing feeds the Exynos watchdog (the loader's core is the one not coming back), so the
 *                  watchdog restarts into TWRP. The log is then in last_kmsg (esslog=1).
 *
 * Sources for the facts (Samsung's kernel, Scion-Project/android_kernel_samsung_exynos9810 at e5906667068e, GPL, read
 * for these facts, not one line copied): drivers/gud/gud-exynos9810/MobiCoreDriver: mci/mcifc.h (the call's number, the
 * extended id, the states), fastcall.c (the registers and the call's answer: x1 the return code, x2 the state, x3 the
 * extended value; the worker pinned to core 0), platform.h (the core's MPIDR).
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

#define MC_FC_INFO              0xFF000002ull
#define MCI_VERSION             0ull
#define CORE0_MPIDR             0x81000000ull
#define TEE_WAIT_S              240     /* how long the core may take to be the kernel's */
#define TEE_SETTLE_S            10      /* after it is: the boot's busy start is over */
#define TEE_HANG_S              30      /* the answer not back after this: a panic, and the watchdog */

enum { TEE_WAITING, TEE_ASKED, TEE_ANSWERED, TEE_NO_CALL };
static volatile uint32_t tee_state = TEE_WAITING;

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

/* The one call: x0 to x3 go through Carbon (EL2) to EL3 and x0 to x3 come back. */
static void
info_call(uint64_t answer[4])
{
	register uint64_t x0 __asm__("x0") = MC_FC_INFO;
	register uint64_t x1 __asm__("x1") = MCI_VERSION;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	answer[0] = x0;
	answer[1] = x1;
	answer[2] = x2;
	answer[3] = x3;
}

static void
watch_thread(void *arg, __unused wait_result_t wr)
{
	bind_to((int)(intptr_t)arg);
	while (__atomic_load_n(&tee_state, __ATOMIC_ACQUIRE) == TEE_WAITING) {
		IOSleep(100);
	}
	for (unsigned int ms = 0; ms < TEE_HANG_S * 1000; ms += 100) {
		if (__atomic_load_n(&tee_state, __ATOMIC_ACQUIRE) != TEE_ASKED) {
			return;
		}
		IOSleep(100);
	}
	say("tee_probe: no answer after %d s: the core has not come back; a panic, then the watchdog\n", TEE_HANG_S);
	IOSleep(500);
	panic("tee_probe: MC_FC_INFO has not returned after %d s", TEE_HANG_S);
}

static void
tee_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t mpidr, answer[4], began, took, ns;
	boolean_t enabled;
	thread_t thread;
	int core0 = -1, other = -1;

	for (unsigned int s = 0; s < TEE_WAIT_S && core0 < 0; s += 2) {
		IOSleep(2000);
		core0 = find_core0(&other);
	}
	if (core0 < 0 || other < 0) {
		say("tee_probe: no core reads MPIDR %#llx after %d s (another core: %d): NO CALL MADE\n", CORE0_MPIDR, TEE_WAIT_S, other);
		__atomic_store_n(&tee_state, TEE_NO_CALL, __ATOMIC_RELEASE);
		return;
	}
	IOSleep(TEE_SETTLE_S * 1000);
	if (kernel_thread_start(watch_thread, (void *)(intptr_t)other, &thread) != KERN_SUCCESS) {
		say("tee_probe: the watching thread could not be started: NO CALL MADE\n");
		__atomic_store_n(&tee_state, TEE_NO_CALL, __ATOMIC_RELEASE);
		return;
	}
	thread_deallocate(thread);
	say("tee_probe: core 0 is the kernel's cpu %d; a hang is watched from cpu %d, for %d s\n", core0, other, TEE_HANG_S);
	bind_to(core0);
	mpidr = __builtin_arm_rsr64("MPIDR_EL1");
	if (mpidr != CORE0_MPIDR) {
		bind_to(-1);
		say("tee_probe: on cpu %d MPIDR reads %#llx, not %#llx: NO CALL MADE\n", core0, mpidr, CORE0_MPIDR);
		__atomic_store_n(&tee_state, TEE_NO_CALL, __ATOMIC_RELEASE);
		return;
	}
	say("tee_probe: MPIDR %#llx: about to call: smc x0=%#llx x1=%#llx x2=0 x3=0\n", mpidr, MC_FC_INFO, MCI_VERSION);
	IOSleep(200);
	/* (the sleep may not move it: the thread is bound) */
	enabled = ml_set_interrupts_enabled(FALSE);
	__atomic_store_n(&tee_state, TEE_ASKED, __ATOMIC_RELEASE);
	__asm__ volatile ("dsb sy" ::: "memory");
	began = mach_absolute_time();
	info_call(answer);
	took = mach_absolute_time() - began;
	__atomic_store_n(&tee_state, TEE_ANSWERED, __ATOMIC_RELEASE);
	mpidr = __builtin_arm_rsr64("MPIDR_EL1");
	(void)ml_set_interrupts_enabled(enabled);
	bind_to(-1);
	absolutetime_to_nanoseconds(took, &ns);
	say("tee_probe: answer on MPIDR %#llx after %llu us: x0=%#llx x1=%#llx x2=%#llx x3=%#llx\n", mpidr, ns / 1000, answer[0],
	    answer[1], answer[2], answer[3]);
	say("tee_probe: done: one call made; the driver's reading: x1 the return code (0 ok), x2 the state (0 not initialised, "
	    "1 bad init, 2 initialised, 3 halted), x3 the extended value (MCI's version)\n");
}

void
hakios_exynos_tee_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("tee_probe", &on, sizeof(on)) || on != 1) {
		return;
	}
	say("tee_probe=1: one read-only MC_FC_INFO, once core 0 is the kernel's\n");
	if (kernel_thread_start(tee_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		say("tee_probe: the thread could not be started: NO CALL MADE\n");
	}
}
