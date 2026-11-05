#define HAKIOS_SOC_FIXED
/*
 * hakios.exynos_tee.c: probes the Galaxy S9's secure OS (Kinibi) from XNU, one step per boot argument.
 *
 * Depends on the SMC path to EL3, the UFS partition reader (hakios_ufs_read_partition and hakios_partition_file)
 * for the TA files, and loader_log_line for the log. Nothing runs unless tee_probe is set to 1 through 4:
 *
 *   tee_probe=1   one read-only MC_FC_INFO (0xFF000002), extended id 0 (MCI_VERSION)
 *   tee_probe=2   that, then the start: MC_FC_INIT, the init values, one empty N-SIQ, then MC_FC_INFO and
 *                 N_YIELD in turn until the state is no longer 0, for at most TEE_POLL_S
 *   tee_probe=3   that, then one MCP round: GET_VERSION, then open and close the TA on the list below, loaded
 *                 from the phone's VENDOR partition
 *   tee_probe=4   that, then one keymaster test on TA ...3e: configure, generate or reuse an HMAC key (the
 *                 blob is kept in KM_TEST_BLOB), and HMAC a fixed test value twice
 *
 * Limits: no RPMB access. The only file written is the test blob at KM_TEST_BLOB under /private/var. Nothing
 * is written to EFS or any other partition. No TA attestation, and nothing from keymaster's key installers.
 * No TA other than those on the list is loaded.
 *
 * tee_probe=3 and 4 were written from the sources below and built. They have not been run on a device.
 *
 * Each call's four returned registers are logged. After TEE_LOG_CALLS calls, only a call whose answer differs
 * from the previous answer of that call is logged.
 *
 * Not used: MC_FC_MEM_TRACE (Android's driver makes it first, for its log; the init call does not depend on it),
 * MC_FC_SWAP_CPU, the Rich OS daemon and its services, the storage proxy, RPMB, EFS, and any TA not on the list.
 * The secure interrupt (SPI 233) is not taken; everything is polled.
 *
 * tee_probe=1 exists because Android's driver calls MC_FC_INFO only after its init call. On one device, observed:
 * MC_FC_INFO before init returned x0=0xff000002 x1=0 x2=0 x3=0x10006 after 28 us. With tee_probe=2 on the same
 * device: INIT x1=0 x2=1 (LPAE), N_SIQ x1=0, state 0, ten N_YIELDs x1=0, then state 2 with MCI 0x10006.
 * A hang, an SError or a halted secure OS are still possible, so:
 *   the core       the secure OS's fastcalls belong to the core it runs on: core 0, until told otherwise. Android's
 *                  worker is pinned there. On the S9 with cpus=8, that core starts as the boot loader's and becomes
 *                  the kernel's after the ufs=3 switch (patch 1513). The thread waits for it, moves to each core the
 *                  scheduler has, reads MPIDR_EL1 there, and stays on the one that reads 0x81000000 (the A55s' whole
 *                  MPIDR is what Android's table has). Every call checks it again. Without such a core within
 *                  TEE_WAIT_S, no call is made.
 *   the memory     the MCI block is a static buffer of this file: not from XNU's allocators, never moved, physically
 *                  contiguous (checked), cacheable like Android's. A mapping of memory outside XNU's RAM is
 *                  write-combined (hakios_physmap_wc), which the secure OS's side could not be coherent with. The block
 *                  holds nothing else, and a run's end restarts the SoC, so nothing else gets that memory while the
 *                  secure OS knows it.
 *   a hang         a watcher thread on another core panics if a call has not returned after TEE_HANG_S. With core 0
 *                  gone (the loader's core), nothing feeds the Exynos watchdog, so the device restarts into TWRP.
 *                  The log is then in last_kmsg (esslog=1).
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
 * The MCP round (tee_probe=3). Numbers from mci/mcimcp.h; behaviour from mcp.c, nq.c, scheduler.c and mmu.c.
 *   a command   written over the init values, 144 bytes at most. Then a notification (session 0, payload 0) goes
 *               into the queue to the secure OS, an N-SIQ is sent, and the secure OS gets time until a notification
 *               for session 0 comes back in the other queue. The answer is in the same place: { command | 1 << 31, result, ... }
 *   its time    while MCP's flags say "not idle": N_YIELD, and every fifth time an N-SIQ, as Android's scheduler does.
 *               While "idle": a 1 ms sleep and an N-SIQ, because the interrupt that would say it wants time is not taken here
 *   a buffer    one 4 KB page table of at most 256 64-bit entries (WSM_L2; the init call's x2 bit 0, LPAE, says 64-bit,
 *               and the phone answered 1): 4 KB pages, normal cacheable inner-shareable, non-secure, execute-never,
 *               read-only for the TA's own image. A table covers 1 MB; a larger TA is refused
 *   open        the TA's file in such a buffer, a TCI in another, the login word "GPA1" with an all-zero (public) identity
 *   not done    any TA's command, and MC_FC_SWAP_CPU. The secure OS's halted state is read and logged, and the driver
 *               stops, as a boot with no TEE would. It does not panic
 * The list of what may be loaded is tee_allowed[]; each file is also checked against its own MCLF header.
 *
 * tee_probe=4, keymaster's client side (facts from disassembling the phone's libkeymaster_helper_vendor.so, for
 * interoperating with the owner's own phone; no code copied):
 *   the session  TA opened from its bytes; a 16-byte TCI, two 0x9008-byte buffers mapped into it (MCP's MAP); the TCI is
 *                { in's secure address, out's secure address, 0, 0 }, u32s. A request: notify the session, wait for its notification
 *   a buffer     { length, u64 } then DER. In: KM_INDATA, out: KM_OUTDATA
 *   KM_INDATA    SEQUENCE { ver 2, km_ver 20, cmd, pid, [0] int0, [1] long0, [2] bin0, [3] bin1, [4] key, [8] SET OF param }, each [n]
 *                EXPLICIT OPTIONAL; a param is SEQUENCE { tag, [0] i, [1] b }
 *   KM_OUTDATA   SEQUENCE { ver, cmd, pid, err, [0] int0, [1] long0, [2] bin0, [3] bin1, [4] SET OF param, [5] log }
 *   cmds         2 generate, 8 begin, 9 update, 10 finish, 15 configure. Tags and values as Android's keymaster
 *   configure    takes the OS version and patch level. The real HAL also sends the file /vendor/tima_measurement_info,
 *                on user builds only. The TA then goes on to an ICCC trustboot initialisation that writes a flag through
 *                Iccc_phys_write. Without the file it logs "Cannot get tima measurement file" and goes on (seen in the
 *                TA's code; not observed on a phone). So this code does not send it
 *
 * Sources for the facts: Samsung's kernel, Scion-Project/android_kernel_samsung_exynos9810 at e5906667068e (GPL),
 * read for these facts; no code copied. drivers/gud/gud-exynos9810/MobiCoreDriver: mci/mcifc.h, mci/mcinq.h,
 * mci/mcimcp.h, mci/mcitime.h, mci/mciiwp.h, mci/mcloadformat.h (the numbers and the layout); fastcall.c (the
 * registers; the worker on core 0); nq.c (the block's order, the sequence, the poll); main.c (what comes first);
 * platform.h (the core's MPIDR); mcp.c, scheduler.c, mmu.c. Interrupt: arch/arm64/boot/dts/exynos/
 * exynos9810-starlte_eur_open_26.dts.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/OSByteOrder.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/hakios.input.h>
#include <sys/param.h>
#include <sys/fcntl.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/hakios.exynos_soc.h>
#include <sys/hakios.xnu.h>
extern int vsnprintf(char *, size_t, const char *, va_list) __printflike(3, 0);
extern void IOSleep(unsigned milliseconds);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
extern void *cpu_to_processor(int cpu);
extern void *thread_bind(void *processor);
extern int thread_block(void *continuation);
extern void *IOMallocAligned(size_t size, size_t alignment);
extern void IOFreeAligned(void *address, size_t size);
extern vfs_context_t vfs_context_kernel(void);

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
#define SSIQ_GIC_NUMBER         EXYNOS_TEE_SSIQ_GIC_NUMBER
_Static_assert(MCI_SESSIONS == MCI_MCP + MCI_MCP_LEN && MCI_TIME == MCI_SESSIONS + MCI_SESSIONS_LEN &&
    MCI_TIME + MCI_TIME_LEN <= MCI_BYTES, "the MCI block's layout");

/* MCP (tee_probe=3) */
#define MCI_FLAGS_SCHEDULE      MCI_MCP                 /* MCP's flags: 0 idle, 1 not */
#define MCI_MESSAGE             MCI_INIT_VALUES         /* a command and its answer, in place of the init values */
#define MCI_MESSAGE_LEN         (MCI_MCP_LEN - 16)
#define NQ_HEADER               12u                     /* { write count, read count, size } */
#define NQ_ELEMENT              8u                      /* { session, payload } */
#define SID_MCP                 0u
#define MCP_RESPONSE            (1u << 31)
#define MCP_OPEN_SESSION        1u
#define MCP_CLOSE_SESSION       3u
#define MCP_GET_VERSION         9u
#define MCP_MAP                 4u
#define WSM_L2                  2u
#define GP_LOGIN_MAGIC          0x47504131u             /* "GPA1" */
#define MCP_WAIT_S              30                      /* an answer not there after this: stop asking */
#define SCHEDULE_NSIQ_EVERY     5                       /* N_YIELDs to one N-SIQ, as Android's */
#define PTE_LEN                 4096u
#define L2_ENTRIES              256u
#define WSM_MAX                 (L2_ENTRIES * PTE_LEN)
#define PTE_RW                  (((uint64_t)1 << 54) /* XN */ | (1u << 10) /* AF */ | (3u << 8) /* inner shareable */ | \
	                        (1u << 6) /* any level */ | (1u << 5) /* NS */ | (1u << 3) | (1u << 2) /* cacheable */ | 3u /* page */)
#define PTE_RO                  (PTE_RW | (1u << 7))
#define MCLF_SERVICE_SYSTEM     3u
/* The TAs this file may load: unmodified, from the phone's own firmware, nothing else. */
static const struct {
	const char *path;
	uint8_t uuid[16];
	uint32_t tci_length;
	const char *what;
} tee_allowed[] = {
#define TA_BIO 0
	{ "/app/mcRegistry/ffffffff000000000000000000000060.tlbin",
	  { 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x60 }, PTE_LEN, "the bio submodule, opened and closed" },
#define TA_KEYMASTER 1
	{ "/app/mcRegistry/ffffffff00000000000000000000003e.tlbin",
	  { 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x3e }, 16, "Samsung's keymaster" },
};
#define KM_BUFFER               0x9008u                 /* the helper's shared_mem_get_size */
#define KM_DER_MAX              (KM_BUFFER - 8)
#define KM_BLOB_MAX             4096u
#define KM_TEST_BLOB            "/private/var/hakios-tee-km-test.blob"
#define KM_CMD_GENERATE         2
#define KM_CMD_BEGIN            8
#define KM_CMD_UPDATE           9
#define KM_CMD_FINISH           10
#define KM_CMD_CONFIGURE        15
#define TAG_PURPOSE             0x20000001u
#define TAG_ALGORITHM           0x10000002u
#define TAG_KEY_SIZE            0x30000003u
#define TAG_DIGEST              0x20000005u
#define TAG_MIN_MAC_LENGTH      0x30000008u
#define TAG_NO_AUTH_REQUIRED    0x700001F7u
#define TAG_CREATION_DATETIME   0x600002BDu
#define TAG_OS_VERSION          0x300002C1u
#define TAG_OS_PATCHLEVEL       0x300002C2u
#define TAG_MAC_LENGTH          0x300003EBu
#define KM_ALGORITHM_HMAC       128
#define KM_DIGEST_SHA256        4
#define KM_PURPOSE_SIGN         2
/* The phone's firmware values, G960FXXUHFVG4: Android 10, July 2022 */
#define KM_OS_VERSION           100000
#define KM_OS_PATCHLEVEL        202207

static uint8_t mci[MCI_BYTES] __attribute__((aligned(16384)));
static uint64_t table_data[PTE_LEN / sizeof(uint64_t)] __attribute__((aligned(4096)));
static uint64_t table_tci[PTE_LEN / sizeof(uint64_t)] __attribute__((aligned(4096)));
static uint64_t table_in[PTE_LEN / sizeof(uint64_t)] __attribute__((aligned(4096)));
static uint64_t table_out[PTE_LEN / sizeof(uint64_t)] __attribute__((aligned(4096)));
static uint8_t tci[PTE_LEN] __attribute__((aligned(4096)));
static uint8_t km_in[KM_BUFFER] __attribute__((aligned(4096)));
static uint8_t km_out[KM_BUFFER] __attribute__((aligned(4096)));

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
	(void)thread_block(NULL);             /* lets the bind take effect */
}

/* A core the scheduler knows, once it has been idle. A core not yet used by the kernel has not been. */
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

/* The SMC itself: x0 to x3 go through Carbon (EL2) to EL3, and x0 to x3 come back. */
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

/* Starts the secure OS from state 0, on core 0: the block, INIT, the init values, an N-SIQ, then polls the state. */
static bool
start(void)
{
	uint64_t base = kvtophys((vm_offset_t)mci), answer[4], last_info[4] = { 0 }, last_yield[4] = { 0 };
	uint64_t deadline, span, yields = 0, looks = 0;
	bool initialised = false;
	clock_sec_t seconds;
	clock_nsec_t nanos;

	for (uint32_t offset = 16384; offset < MCI_BYTES; offset += 16384) {
		if (kvtophys((vm_offset_t)mci + offset) != base + offset) {
			say("tee_probe: the block is not contiguous at +%u (%#llx from %#llx): NO INIT\n", offset,
			    kvtophys((vm_offset_t)mci + offset), base);
			return false;
		}
	}
	if (base == 0 || (base & 0x3fff) != 0 || (base >> 48) != 0) {
		say("tee_probe: the block at %#llx will not do: NO INIT\n", base);
		return false;
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
		return false;
	}
	if (answer[1] != 0) {
		say("tee_probe: INIT returned code %#llx: stopping\n", answer[1]);
		return false;
	}
	put32(MCI_INIT_VALUES, IV_FLAG_TIME | IV_FLAG_IWP);
	put32(MCI_INIT_VALUES + 4, SSIQ_GIC_NUMBER);
	put32(MCI_INIT_VALUES + 8, MCI_TIME);
	put32(MCI_INIT_VALUES + 12, MCI_TIME_LEN);
	put32(MCI_INIT_VALUES + 16, MCI_SESSIONS);
	put32(MCI_INIT_VALUES + 20, MCI_SESSIONS_LEN);
	__asm__ volatile ("dsb sy" ::: "memory");

	if (!call("N_SIQ", MC_SMC_N_SIQ, 0, 0, 0, answer, NULL)) {
		return false;
	}
	if (answer[1] != 0) {
		say("tee_probe: N_SIQ returned code %#llx: stopping\n", answer[1]);
		return false;
	}

	nanoseconds_to_absolutetime((uint64_t)TEE_POLL_S * NSEC_PER_SEC, &span);
	deadline = mach_absolute_time() + span;
	for (;;) {
		uint64_t state;

		if (!call("INFO", MC_FC_INFO, EXT_MCI_VERSION, 0, 0, answer, last_info)) {
			return false;
		}
		looks++;
		state = answer[2] & 0xffffffffull;
		if (answer[1] != 0) {
			say("tee_probe: INFO returned code %#llx: stopping\n", answer[1]);
			break;
		}
		if (state == STATE_INITIALIZED) {
			say("tee_probe: state 2 (initialised) after %llu looks and %llu yields; MCI %#llx\n", looks, yields, answer[3]);
			initialised = true;
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
				return false;
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
	return initialised;
}

static void
time_now(void)
{
	clock_sec_t seconds;
	clock_nsec_t nanos;
	uint64_t ns;

	clock_get_calendar_nanotime(&seconds, &nanos);
	put64(MCI_TIME, (uint64_t)seconds);
	put64(MCI_TIME + 8, (uint64_t)nanos);
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	put64(MCI_TIME + 16, ns / NSEC_PER_SEC);
	put64(MCI_TIME + 24, ns % NSEC_PER_SEC);
}

/* The secure OS gets time until a notification for `session` has come back: its payload in *payload. False: none came. */
static bool
wait_notification(uint32_t session, uint32_t *payload)
{
	static uint64_t last_yield[4], last_siq[4];
	uint64_t answer[4], deadline, span;
	uint32_t calls = 0;
	int slice = SCHEDULE_NSIQ_EVERY;

	nanoseconds_to_absolutetime((uint64_t)MCP_WAIT_S * NSEC_PER_SEC, &span);
	deadline = mach_absolute_time() + span;
	for (;;) {
		bool idle, yield, got = false;

		__asm__ volatile ("dsb sy" ::: "memory");
		while (get32(MCI_QUEUE_RX) != get32(MCI_QUEUE_RX + 4)) {
			uint32_t read = get32(MCI_QUEUE_RX + 4);
			uint32_t at = MCI_QUEUE_RX + NQ_HEADER + (read % MCI_QUEUE_ELEMENTS) * NQ_ELEMENT;
			uint32_t from = get32(at), value = get32(at + 4);

			__asm__ volatile ("dmb sy" ::: "memory");
			put32(MCI_QUEUE_RX + 4, read + 1);
			__asm__ volatile ("dsb sy" ::: "memory");
			say("tee_probe: notification from session %#x, payload %#x\n", from, value);
			if (from == session && !got) {
				*payload = value;
				got = true;
			}
		}
		if (got) {
			return true;
		}
		if (mach_absolute_time() > deadline) {
			say("tee_probe: no notification for session %#x after %d s, %u calls\n", session, MCP_WAIT_S, calls);
			return false;
		}
		time_now();
		idle = get32(MCI_FLAGS_SCHEDULE) == 0;
		yield = !idle && slice-- > 0;
		if (idle) {
			IOSleep(1);
		}
		if (yield) {
			if (!call("N_YIELD", MC_SMC_N_YIELD, 0, 0, 0, answer, last_yield)) {
				return false;
			}
		} else {
			slice = SCHEDULE_NSIQ_EVERY;
			if (!call("N_SIQ", MC_SMC_N_SIQ, 0, 0, 0, answer, last_siq)) {
				return false;
			}
		}
		calls++;
		if (answer[1] != 0) {
			say("tee_probe: the secure OS's scheduling call returned code %#llx: stopping\n", answer[1]);
			return false;
		}
	}
}

/* Queue a notification to the secure OS, as Android's: the element, a barrier, the counter. */
static bool
notify(uint32_t session, uint32_t payload)
{
	uint32_t write = get32(0);
	uint32_t at = NQ_HEADER + (write % MCI_QUEUE_ELEMENTS) * NQ_ELEMENT;

	if (write - get32(4) >= MCI_QUEUE_ELEMENTS) {
		say("tee_probe: the queue to the secure OS is full\n");
		return false;
	}
	put32(at, session);
	put32(at + 4, payload);
	__asm__ volatile ("dmb sy" ::: "memory");
	put32(0, write + 1);
	__asm__ volatile ("dsb sy" ::: "memory");
	return true;
}

/* A command is at MCI_MESSAGE. The answer's result in *result. False: nothing usable came back. */
static bool
mcp_command(const char *name, uint32_t id, uint32_t *result)
{
	uint64_t answer[4];
	uint32_t payload = 0;

	put32(MCI_MESSAGE, id);
	if (!notify(SID_MCP, 0) || !call(name, MC_SMC_N_SIQ, 0, 0, 0, answer, NULL) || answer[1] != 0 ||
	    !wait_notification(SID_MCP, &payload)) {
		say("tee_probe: %s: no answer\n", name);
		return false;
	}
	if (get32(MCI_MESSAGE) != (id | MCP_RESPONSE)) {
		say("tee_probe: %s: the answer's id is %#x, not %#x\n", name, get32(MCI_MESSAGE), id | MCP_RESPONSE);
		return false;
	}
	*result = get32(MCI_MESSAGE + 4);
	say("tee_probe: %s: MCP result %u\n", name, *result);
	return true;
}

static void
message_clear(void)
{
	for (uint32_t i = 0; i < MCI_MESSAGE_LEN; i += 4) {
		put32(MCI_MESSAGE + i, 0);
	}
}

/* One table for a buffer in wired kernel memory; its physical address, 0 if it will not do. */
static uint64_t
wsm_table(uint64_t *table, const void *buffer, uint32_t length, uint64_t flags)
{
	vm_offset_t va = (vm_offset_t)buffer & ~(vm_offset_t)(PTE_LEN - 1);
	uint32_t offset = (uint32_t)((vm_offset_t)buffer & (PTE_LEN - 1));
	uint32_t pages = (offset + length + PTE_LEN - 1) / PTE_LEN;

	if (length == 0 || pages > L2_ENTRIES) {
		return 0;
	}
	bzero(table, PTE_LEN);
	for (uint32_t i = 0; i < pages; i++) {
		uint64_t pa = kvtophys(va + (vm_offset_t)i * PTE_LEN);

		if (pa == 0 || (pa & (PTE_LEN - 1)) != 0) {
			return 0;
		}
		table[i] = pa | flags;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	return kvtophys((vm_offset_t)table);
}

/* The file from the phone's own firmware, checked against the list; a wired copy in *copy, *length. */
static bool
ta_read(unsigned int which, uint8_t **copy, uint32_t *length)
{
	static uint8_t probe[4096];
	const uint8_t *data = NULL;
	uint64_t size = 0, got = 0;
	int waited;

	for (waited = 0; waited < 120 && hakios_ufs_read_partition("VENDOR", 0, probe, sizeof(probe), &got) != 0; waited++) {
		IOSleep(500);
	}
	if (hakios_partition_file("VENDOR", "/vendor", tee_allowed[which].path, WSM_MAX, &data, &size) != 0 || data == NULL) {
		say("tee_probe: %s is not on the phone's VENDOR partition (or is larger than %u): no load\n", tee_allowed[which].path, WSM_MAX);
		return false;
	}
	/* an MCLF header: "MCLF", version, flags, memory type, service type, instances, uuid */
	if (size < 0x28 + 16 || memcmp(data, "MCLF", 4) != 0 || memcmp(data + 0x18, tee_allowed[which].uuid, 16) != 0 ||
	    *(const uint32_t *)(const void *)(data + 0x10) != MCLF_SERVICE_SYSTEM) {
		say("tee_probe: %s: its header is not what the list says: no load\n", tee_allowed[which].path);
		return false;
	}
	*copy = IOMallocAligned((size_t)size, PTE_LEN);
	if (*copy == NULL) {
		say("tee_probe: no memory for %llu bytes of TA\n", size);
		return false;
	}
	memcpy(*copy, data, (size_t)size);
	*length = (uint32_t)size;
	return true;
}

/* Open a session to a TA of the list. *ta, *ta_length: its copy, for session_close. 0: no session. */
static uint32_t
session_open(unsigned int which, uint8_t **ta, uint32_t *ta_length)
{
	uint32_t result = 0, session;
	uint64_t ta_table, tci_table;

	if (!ta_read(which, ta, ta_length)) {
		return 0;
	}
	ta_table = wsm_table(table_data, *ta, *ta_length, PTE_RO);
	tci_table = wsm_table(table_tci, tci, tee_allowed[which].tci_length, PTE_RW);
	if (ta_table == 0 || tci_table == 0) {
		say("tee_probe: the buffers' tables will not do (%#llx, %#llx): no load\n", ta_table, tci_table);
		IOFreeAligned(*ta, *ta_length);
		return 0;
	}
	say("tee_probe: %s, %u bytes, version word %#x, %s\n", tee_allowed[which].path, *ta_length,
	    *(const uint32_t *)(const void *)(*ta + 4), tee_allowed[which].what);
	message_clear();
	/* { id, uuid 4..19, -, tci's table 24, data's table 32, tci's offset 40, length 44, type 48, data's type 52, offset 56, length 60, login 64.., -  } */
	for (uint32_t i = 0; i < 16; i += 4) {
		put32(MCI_MESSAGE + 4 + i, *(const uint32_t *)(const void *)(*ta + 0x18 + i));
	}
	put64(MCI_MESSAGE + 24, tci_table);
	put64(MCI_MESSAGE + 32, ta_table);
	put32(MCI_MESSAGE + 44, tee_allowed[which].tci_length);
	put32(MCI_MESSAGE + 48, WSM_L2);
	put32(MCI_MESSAGE + 52, WSM_L2);
	put32(MCI_MESSAGE + 60, *ta_length);
	put32(MCI_MESSAGE + 64, GP_LOGIN_MAGIC);
	if (!mcp_command("OPEN_SESSION", MCP_OPEN_SESSION, &result)) {
		/* the secure OS may hold the buffers now: they are not freed; the run ends in a restart */
		return 0;
	}
	session = get32(MCI_MESSAGE + 8);
	say("tee_probe: OPEN_SESSION: result %u, session %#x\n", result, session);
	if (result != 0 || session == 0) {
		IOFreeAligned(*ta, *ta_length);
		return 0;
	}
	return session;
}

static void
session_close(uint32_t session, uint8_t *ta, uint32_t ta_length)
{
	uint32_t result = 0;

	for (int tries = 0; tries < 10; tries++) {
		message_clear();
		put32(MCI_MESSAGE + 4, session);
		if (!mcp_command("CLOSE_SESSION", MCP_CLOSE_SESSION, &result)) {
			return;
		}
		if (result != 28) {     /* MC_MCP_RET_ERR_CLOSE_TASK_FAILED: try again */
			break;
		}
		IOSleep(10);
	}
	say("tee_probe: CLOSE_SESSION: result %u\n", result);
	if (result == 0) {
		IOFreeAligned(ta, ta_length);
	}
}

/* Runs with the secure OS initialised, on core 0: GET_VERSION, then one TA opened and closed. */
static bool
mcp_round(void)
{
	uint8_t *ta = NULL;
	uint32_t ta_length = 0, result = 0, session, payload;
	char product[65];

	time_now();
	message_clear();
	if (!mcp_command("GET_VERSION", MCP_GET_VERSION, &result)) {
		return false;
	}
	for (uint32_t i = 0; i < 64; i++) {
		product[i] = (char)mci[MCI_MESSAGE + 8 + i];
	}
	product[64] = 0;
	say("tee_probe: product \"%s\", MCI %#x\n", product, get32(MCI_MESSAGE + 8 + 64));
	if (result != 0) {
		return false;
	}
	session = session_open(TA_BIO, &ta, &ta_length);
	if (session == 0) {
		return false;
	}
	/* no command to it: time for it to start, and anything it says (a TA's exit comes as a payload) */
	if (wait_notification(session, &payload)) {
		say("tee_probe: the TA's session said %#x\n", payload);
	}
	session_close(session, ta, ta_length);
	return true;
}

struct der {
	uint8_t *p;
	uint32_t n;
	bool bad;
};

static void
der_raw(struct der *d, const void *data, uint32_t length)
{
	if (d->bad || length > KM_DER_MAX - d->n) {
		d->bad = true;
		return;
	}
	memmove(d->p + d->n, data, length);
	d->n += length;
}

/* What has been written since `start` becomes the contents of a `tag`. */
static void
der_wrap(struct der *d, uint32_t start, uint8_t tag)
{
	uint8_t header[5];
	uint32_t length = d->n - start, h = 0;

	if (d->bad) {
		return;
	}
	header[h++] = tag;
	if (length < 0x80) {
		header[h++] = (uint8_t)length;
	} else if (length < 0x100) {
		header[h++] = 0x81;
		header[h++] = (uint8_t)length;
	} else {
		header[h++] = 0x82;
		header[h++] = (uint8_t)(length >> 8);
		header[h++] = (uint8_t)length;
	}
	if (h > KM_DER_MAX - d->n) {
		d->bad = true;
		return;
	}
	memmove(d->p + start + h, d->p + start, length);
	memcpy(d->p + start, header, h);
	d->n += h;
}

static void
der_integer(struct der *d, uint64_t value)
{
	uint8_t bytes[9];
	int first = 0;

	bytes[0] = 0;
	for (int i = 0; i < 8; i++) {
		bytes[8 - i] = (uint8_t)(value >> (8 * i));
	}
	while (first < 8 && bytes[first] == 0 && (bytes[first + 1] & 0x80) == 0) {
		first++;
	}
	uint32_t start = d->n;
	der_raw(d, bytes + first, (uint32_t)(9 - first));
	der_wrap(d, start, 0x02);
}

static void
der_explicit_integer(struct der *d, uint8_t number, uint64_t value)
{
	uint32_t start = d->n;

	der_integer(d, value);
	der_wrap(d, start, 0xA0 | number);
}

static void
der_explicit_octets(struct der *d, uint8_t number, const uint8_t *data, uint32_t length)
{
	uint32_t start = d->n, inner;

	inner = d->n;
	der_raw(d, data, length);
	der_wrap(d, inner, 0x04);
	der_wrap(d, start, 0xA0 | number);
}

static void
der_param(struct der *d, uint32_t tag, uint64_t value)
{
	uint32_t start = d->n;

	der_integer(d, tag);
	der_explicit_integer(d, 0, value);
	der_wrap(d, start, 0x30);
}

/* A TLV at *at of limit: its tag, contents and length; *at past it. */
static bool
der_next(const uint8_t *base, uint32_t limit, uint32_t *at, uint8_t *tag, uint32_t *contents, uint32_t *length)
{
	uint32_t i = *at, l;

	if (i + 2 > limit) {
		return false;
	}
	*tag = base[i++];
	l = base[i++];
	if (l & 0x80) {
		uint32_t count = l & 0x7f;

		if (count == 0 || count > 3 || i + count > limit) {
			return false;
		}
		l = 0;
		while (count-- > 0) {
			l = (l << 8) | base[i++];
		}
	}
	if (l > limit - i) {
		return false;
	}
	*contents = i;
	*length = l;
	*at = i + l;
	return true;
}

static int64_t
der_read_integer(const uint8_t *base, uint32_t contents, uint32_t length)
{
	uint64_t v = (length > 0 && (base[contents] & 0x80)) ? ~0ull : 0;

	for (uint32_t i = 0; i < length && i < 9; i++) {
		v = (v << 8) | base[contents + i];
	}
	return (int64_t)v;
}

struct km_answer {
	int64_t err;
	bool has_long0;
	uint64_t long0;
	const uint8_t *bin0;
	uint32_t bin0_length;
	const uint8_t *bin1;
	uint32_t bin1_length;
};

/* Parses the answer by context tag. Log lines in field [5] are printed. */
static bool
km_parse(uint32_t length, struct km_answer *a)
{
	const uint8_t *base = km_out + 8;
	uint32_t at = 0, seq, seq_length, end, i = 0;
	uint8_t tag;

	bzero(a, sizeof(*a));
	a->err = -1000;
	if (!der_next(base, length, &at, &tag, &seq, &seq_length) || tag != 0x30) {
		say("tee_probe: keymaster: the answer is not a SEQUENCE\n");
		return false;
	}
	at = seq;
	end = seq + seq_length;
	while (at < end) {
		uint32_t c, l, inner, c2, l2;
		uint8_t t2;

		if (!der_next(base, end, &at, &tag, &c, &l)) {
			say("tee_probe: keymaster: the answer stops at %u of %u\n", at, end);
			return false;
		}
		if (tag == 0x02) {
			if (i == 3) {
				a->err = der_read_integer(base, c, l);
			}
			i++;
			continue;
		}
		if (tag == 0x85 || tag == 0xA5) {
			/* the log: octet strings */
			inner = c;
			while (inner < c + l && der_next(base, c + l, &inner, &t2, &c2, &l2)) {
				if (t2 == 0x04) {
					say("tee_probe: keymaster's log: %.*s\n", (int)MIN(l2, 200u), (const char *)base + c2);
				}
			}
			continue;
		}
		inner = c;
		if (!der_next(base, c + l, &inner, &t2, &c2, &l2)) {
			continue;
		}
		if (tag == 0xA1 && t2 == 0x02) {
			a->has_long0 = true;
			a->long0 = (uint64_t)der_read_integer(base, c2, l2);
		} else if (tag == 0xA2 && t2 == 0x04) {
			a->bin0 = base + c2;
			a->bin0_length = l2;
		} else if (tag == 0xA3 && t2 == 0x04) {
			a->bin1 = base + c2;
			a->bin1_length = l2;
		}
	}
	return true;
}

/* One request: the DER is in km_in + 8, its length d->n. False: no answer at all. a->err: the TA's answer. */
static bool
km_call(uint32_t session, const char *name, const struct der *d, struct km_answer *a)
{
	uint64_t answer[4], out_length;
	uint32_t payload = 0;

	if (d->bad) {
		say("tee_probe: keymaster %s: the request does not fit\n", name);
		return false;
	}
	*(volatile uint64_t *)(void *)km_in = d->n;
	*(volatile uint64_t *)(void *)km_out = 0;
	time_now();
	__asm__ volatile ("dsb sy" ::: "memory");
	if (!notify(session, 0) || !call(name, MC_SMC_N_SIQ, 0, 0, 0, answer, NULL) || answer[1] != 0 ||
	    !wait_notification(session, &payload)) {
		say("tee_probe: keymaster %s: no answer\n", name);
		return false;
	}
	if (payload != 0) {
		say("tee_probe: keymaster %s: the TA has exited, code %#x\n", name, payload);
		return false;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	out_length = *(volatile uint64_t *)(void *)km_out;
	if (out_length == 0 || out_length > KM_DER_MAX) {
		say("tee_probe: keymaster %s: the answer's length is %llu\n", name, out_length);
		return false;
	}
	if (!km_parse((uint32_t)out_length, a)) {
		return false;
	}
	say("tee_probe: keymaster %s: err %lld, %llu bytes back; long0 %s, bin0 %u bytes, bin1 %u bytes\n", name, a->err, out_length,
	    a->has_long0 ? "yes" : "no", a->bin0_length, a->bin1_length);
	return true;
}

static uint32_t
km_request(struct der *d, uint32_t cmd)
{
	d->p = km_in + 8;
	d->n = 0;
	d->bad = false;
	der_integer(d, 2);
	der_integer(d, 20);
	der_integer(d, cmd);
	der_integer(d, 1);
	return 0;
}

static void
km_finish_request(struct der *d, uint32_t start)
{
	der_wrap(d, start, 0x30);
}

static bool
km_map(uint32_t session, uint8_t *buffer, uint64_t *table, uint32_t *secure)
{
	uint32_t result = 0;
	uint64_t pa = wsm_table(table, buffer, KM_BUFFER, PTE_RW);

	if (pa == 0) {
		say("tee_probe: keymaster's buffer's table will not do\n");
		return false;
	}
	message_clear();
	/* { id, session 4, type 8, offset 12, table 16, length 24 } */
	put32(MCI_MESSAGE + 4, session);
	put32(MCI_MESSAGE + 8, WSM_L2);
	put64(MCI_MESSAGE + 16, pa);
	put32(MCI_MESSAGE + 24, KM_BUFFER);
	if (!mcp_command("MAP", MCP_MAP, &result) || result != 0) {
		return false;
	}
	*secure = get32(MCI_MESSAGE + 8);
	say("tee_probe: keymaster's buffer mapped at secure %#x\n", *secure);
	return true;
}

static int
km_blob_file(const char *path, enum uio_rw rw, uint8_t *blob, uint32_t *length)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int fmode = rw == UIO_READ ? FREAD : (FREAD | FWRITE | O_CREAT | O_TRUNC);
	int resid = 0, error;

	error = vnode_open(path, fmode, 0600, VNODE_LOOKUP_NOFOLLOW, &vp, ctx);
	if (error != 0) {
		return error;
	}
	error = vn_rdwr(rw, vp, (caddr_t)blob, (int)(rw == UIO_READ ? KM_BLOB_MAX : *length), 0, UIO_SYSSPACE,
	    rw == UIO_WRITE ? IO_SYNC : 0, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	if (rw == UIO_READ && error == 0) {
		*length = KM_BLOB_MAX - (uint32_t)resid;
	}
	(void)vnode_close(vp, fmode, ctx);
	return error;
}

/* One HMAC of the fixed test value under the blob; the 32 bytes logged. */
static bool
km_hmac(uint32_t session, const uint8_t *blob, uint32_t blob_length, uint8_t mac[32])
{
	static const uint8_t test_value[32] = "HakiOS TEE keymaster test value";
	struct der d;
	struct km_answer a;
	uint32_t start, mark, inner;
	uint64_t handle;

	start = km_request(&d, KM_CMD_BEGIN);
	der_explicit_integer(&d, 0, KM_PURPOSE_SIGN);
	der_explicit_octets(&d, 4, blob, blob_length);
	mark = d.n;
	inner = d.n;
	der_param(&d, TAG_DIGEST, KM_DIGEST_SHA256);
	der_param(&d, TAG_MAC_LENGTH, 256);
	der_wrap(&d, inner, 0x31);
	der_wrap(&d, mark, 0xA8);
	km_finish_request(&d, start);
	if (!km_call(session, "begin", &d, &a) || a.err != 0 || !a.has_long0) {
		return false;
	}
	handle = a.long0;

	start = km_request(&d, KM_CMD_UPDATE);
	der_explicit_integer(&d, 1, handle);
	der_explicit_octets(&d, 2, test_value, sizeof(test_value));
	km_finish_request(&d, start);
	if (!km_call(session, "update", &d, &a) || a.err != 0) {
		return false;
	}

	start = km_request(&d, KM_CMD_FINISH);
	der_explicit_integer(&d, 1, handle);
	km_finish_request(&d, start);
	if (!km_call(session, "finish", &d, &a) || a.err != 0) {
		return false;
	}
	if (a.bin0_length == 32) {
		memcpy(mac, a.bin0, 32);
	} else if (a.bin1_length == 32) {
		memcpy(mac, a.bin1, 32);
	} else {
		say("tee_probe: keymaster's finish: no 32 bytes back\n");
		return false;
	}
	return true;
}

/* Runs with the secure OS initialised, on core 0. */
static void
keymaster_test(void)
{
	static uint8_t blob[KM_BLOB_MAX];
	struct der d;
	struct km_answer a;
	uint8_t *ta = NULL;
	uint32_t ta_length = 0, session, in_secure = 0, out_secure = 0, start, mark, inner, blob_length = 0;
	uint8_t first[32], second[32];
	int error;

	session = session_open(TA_KEYMASTER, &ta, &ta_length);
	if (session == 0) {
		return;
	}
	if (!km_map(session, km_in, table_in, &in_secure) || !km_map(session, km_out, table_out, &out_secure)) {
		goto close;
	}
	bzero(tci, 16);
	((volatile uint32_t *)(void *)tci)[0] = in_secure;
	((volatile uint32_t *)(void *)tci)[1] = out_secure;

	start = km_request(&d, KM_CMD_CONFIGURE);
	mark = d.n;
	inner = d.n;
	der_param(&d, TAG_OS_VERSION, KM_OS_VERSION);
	der_param(&d, TAG_OS_PATCHLEVEL, KM_OS_PATCHLEVEL);
	der_wrap(&d, inner, 0x31);
	der_wrap(&d, mark, 0xA8);
	km_finish_request(&d, start);
	if (!km_call(session, "configure", &d, &a)) {
		goto close;
	}

	error = km_blob_file(KM_TEST_BLOB, UIO_READ, blob, &blob_length);
	if (error == 0 && blob_length != 0) {
		say("tee_probe: keymaster: reusing %s, %u bytes, from an earlier boot\n", KM_TEST_BLOB, blob_length);
	} else {
		clock_sec_t seconds;
		clock_nsec_t nanos;

		say("tee_probe: keymaster: no blob on /private/var (%d): generating one\n", error);
		clock_get_calendar_nanotime(&seconds, &nanos);
		start = km_request(&d, KM_CMD_GENERATE);
		mark = d.n;
		inner = d.n;
		der_param(&d, TAG_ALGORITHM, KM_ALGORITHM_HMAC);
		der_param(&d, TAG_KEY_SIZE, 256);
		der_param(&d, TAG_PURPOSE, KM_PURPOSE_SIGN);
		der_param(&d, TAG_DIGEST, KM_DIGEST_SHA256);
		der_param(&d, TAG_MIN_MAC_LENGTH, 256);
		der_param(&d, TAG_NO_AUTH_REQUIRED, 1);
		der_param(&d, TAG_CREATION_DATETIME, (uint64_t)seconds * 1000);
		der_wrap(&d, inner, 0x31);
		der_wrap(&d, mark, 0xA8);
		km_finish_request(&d, start);
		if (!km_call(session, "generate", &d, &a) || a.err != 0) {
			goto close;
		}
		{
			const uint8_t *out = a.bin1_length != 0 ? a.bin1 : a.bin0;
			uint32_t out_length = a.bin1_length != 0 ? a.bin1_length : a.bin0_length;

			if (out == NULL || out_length == 0 || out_length > KM_BLOB_MAX) {
				say("tee_probe: keymaster's generate: no blob back (%u bytes)\n", out_length);
				goto close;
			}
			memcpy(blob, out, out_length);
			blob_length = out_length;
		}
		error = km_blob_file(KM_TEST_BLOB, UIO_WRITE, blob, &blob_length);
		say("tee_probe: keymaster: a blob of %u bytes, written to %s: %d\n", blob_length, KM_TEST_BLOB, error);
	}
	if (!km_hmac(session, blob, blob_length, first) || !km_hmac(session, blob, blob_length, second)) {
		goto close;
	}
	say("tee_probe: keymaster's HMAC of the test value: %016llx%016llx%016llx%016llx (again: %s)\n",
	    OSSwapBigToHostInt64(*(uint64_t *)(void *)first), OSSwapBigToHostInt64(*(uint64_t *)(void *)(first + 8)),
	    OSSwapBigToHostInt64(*(uint64_t *)(void *)(first + 16)), OSSwapBigToHostInt64(*(uint64_t *)(void *)(first + 24)),
	    memcmp(first, second, 32) == 0 ? "the same" : "DIFFERENT");
close:
	session_close(session, ta, ta_length);
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
	if (call("INFO", MC_FC_INFO, EXT_MCI_VERSION, 0, 0, answer, NULL) && on >= 2) {
		if (answer[1] == 0 && (answer[2] & 0xffffffffull) == STATE_NOT_INITIALIZED) {
			if (start() && on >= 3 && mcp_round() && on == 4) {
				keymaster_test();
			}
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

	if (!PE_parse_boot_argn("tee_probe", &on, sizeof(on)) || (on < 1 || on > 4)) {
		return;
	}
	say("tee_probe=%u: %s, once core 0 is the kernel's\n", on, on == 1 ? "one read-only MC_FC_INFO" : on == 2 ? "MC_FC_INFO, then the start" : on == 3 ? "the start, then one MCP round" : "the start, one MCP round, keymaster's test");
	if (kernel_thread_start(tee_thread, (void *)(uintptr_t)on, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		say("tee_probe: the thread could not be started: NO CALL MADE\n");
	}
}
