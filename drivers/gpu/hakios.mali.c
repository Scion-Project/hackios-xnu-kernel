/*
 * The GPU of the Galaxy S9 (starlte, Exynos 9810), a Mali-G72 MP18, brought up from the kernel step by step and then
 * handed to HakiosMali (drivers/gpu/hakios.mali.cpp), the IOKit service Mesa's panfrost draws through. Only with
 * boot-arg mali=N: every address below belongs to this phone, and nothing is touched without it.
 *
 * WHERE IT COMES FROM. This is HakiOS's Mali driver for the Infinix Hot 20 (X6826B: MediaTek MT6768, a Mali-G52 MC2),
 * kernel/bsd/dev/redwood_mali.c of github.com/oosexgang/hakios-mt6768 at commit 93ea250, under the Apple Public Source
 * License 2.0 as every file of that repository's kernel/ is (its NOTICE), and as this tree's drivers/ are. It is renamed
 * (redwood_ to hakios_, kern.redwood to kern.hakios) and MediaTek's power and clocks are replaced by the Exynos 9810's.
 * What is the original's: the GPU's reset and cores, address space 0 and the GPU's own page tables, the two first jobs,
 * Linux panfrost's parameters, the GPU addresses and what is remembered of each buffer, the queue of job chains, the
 * waits, the kept lines and the minute's report. On the X6826B it drew iOS's window server through Mesa's panfrost and a
 * Metal driver (that repository's README, "GPU: works"). Here it has not run.
 *
 * THE GPU. A Bifrost Mali needs no firmware: bringing it up is power and a clock, then its own registers, which are Linux
 * panfrost's (drivers/gpu/drm/panfrost/panfrost_regs.h) and the same on the G72 as on the G52 for all that is used here.
 *   registers     0x17500000, 0x5000 bytes (the device tree's mali@17500000). GPU_ID >> 16 reads 0x6221: architecture
 *                 6.2, revision 2, product 1, the G72 ("tHEx"); Mesa's panfrost knows it by that number
 *   interrupts    SPI 282 job, 281 MMU, 283 GPU: GIC INTIDs 314, 313 and 315 on the GIC-400. Not used: as the original
 *                 did, the driver asks the registers (a moment of polling, then asleep 200 us at a time)
 *   power         the domain pd-embedded_g3d of the PMU (0x14060000): its configuration at 0x14064040, [3:0] = 0xF asks
 *                 for it on; its status at 0x14064044, [3:0] = 0xF when it is
 *   clock         PLL_G3D of CMU_G3D (0x17400000). Its PLL_CON0 is CMU_G3D +0x140: S [2:0], P [13:8], M [25:16], STABLE
 *                 bit 29, ENABLE bit 31, fed by 26 MHz, so 26 MHz x M / (P x 2^S). Its rate is set by ACPM's DVFS (260 to
 *                 572 MHz); this driver never programs it, only reads it and says what it is
 *   coherency     none: the GPU does not look into the processors' caches. COHERENCY_ENABLE = 31 (none) after every
 *                 reset; the processor cleans its cache to memory around what the GPU reads and writes of the kernel's
 *                 (job pages, page tables), and the buffers of programs are write-combined, as in the original
 *
 * Samsung's power-on of the G3D, as the kernel's register writes (the CAL of the GPL kernel named below):
 *   CMU_G3D +0x148 = 0x30000003; CMU_G3D +0x140 |= bit 4; CMU_G3D +0x100 |= bit 4; PMU 0x14064040 [3:0] = 0xF; then wait
 *   until 0x14064044 [3:0] == 0xF. The CMU_G3D writes come before the domain has power, so (an inference, which step 1
 *   relies on to read them) those registers answer without it.
 * After it, the CAL makes an SMC, 0x82000410 with (1, 0x17410204): the DTZPC restore, the G3D's TrustZone protection
 * controller set up again by EL3. Under this port Carbon Hypervisor has EL2 and passes an SMC it does not know on to EL3.
 * Power-off is the same in reverse, ending with CMU_G3D +0x148 = 0x10000001.
 *
 * A KNOWN GAP. Before it powers the G3D on, Samsung's CAL also sends ACPM (the always-on power manager's firmware) a PMIC
 * update: type 3, register 0x2a of the S2MPS18, bits [7:6] = 11 -- its BUCK6, vdd_g3d, handed over. THAT IS NOT DONE
 * HERE: this kernel has no ACPM driver, and a write to a PMIC is made only where tools/provenance.allow says why. If the
 * rail is off as the phone's boot leaves it, step 2 waits for a domain that does not come up, says so, and stops; if the
 * domain says on without its supply, step 4's first read of the GPU is where the run would end. Step 2 says this too.
 *
 * Every Exynos fact above is from the GPL-2.0 kernel of the Exynos 9810 (github.com/ExyHyperBrick/
 * android_kernel_samsung_exynos9810, branch lineage-24.0: the G3D's CAL and power domain, the Mali's platform code) and
 * Samsung's device tree (Silicium's starlte.dts), through a fact sheet: read for these numbers, not one line copied.
 *
 * WHAT CHANGED FROM THE ORIGINAL, AND WHY
 *   - MediaTek's clock mux, SPM power domains, bus protection and MFG clock gate are gone; the Exynos 9810's sequence is
 *     steps 2 and 3, and step 1 looks before anything is written.
 *   - The original's steps ran all at once with mali=1; here mali=N makes the first N of nine, each said before it is
 *     made, and the domain is switched off again if the steps end short of serving and it was this driver that
 *     switched it on (with a soft reset first once the GPU has answered).
 *   - Its page tables assume 35 bits of GPU address (the G52's MMU_FEATURES, 0x2823). Step 4 reads the G72's and refuses
 *     to go on with fewer, or with fewer than 36 bits of physical address: the S9's memory reaches 0x8FFFFFFFF.
 *   - Step 4 refuses a GPU that is not a G72, or whose shader cores do not fit the 32 bits of affinity the jobs are given.
 *   - COHERENCY_ENABLE = 31 after every reset, the one of a hung job's recovery included (a reset may clear it).
 *   - A physical address is checked to be memory the kernel manages (pmap_valid_address()) before the kernel's address
 *     of it is asked for and the cache cleaned over it -- a table of the GPU's, found through an entry -- and before a
 *     page is entered into the GPU's tables. phystokv() panics on an address outside, and PixOS, on another phone,
 *     panicked flushing the cache over an address it had not checked.
 *   - The page of the first jobs is taken out of the GPU's tables before it is freed (the original left its entry).
 *   - NR_CORE_GROUPS is the number of L2 caches present, as Linux panfrost answers it, not a fixed 1.
 *   - The steps' lines go to the loader's log as they are said (say()), as this tree's drivers do: the end of a run
 *     that dies in a step is what is kept. The minute's report and the per-job lines stay in the kernel's log only.
 *
 * THE STEPS. mali=N makes the first N and stops; 0, or no mali= at all, leaves the GPU alone. A step says what it will
 * touch before it does, and one that does not find what it needs ends the sequence:
 *   1  reads   the PMU's G3D configuration and status; CMU_G3D +0x100, +0x140 (PLL_G3D, and the rate it gives) and
 *              +0x148; GPU_ID only if the domain reads as on. Nothing is written
 *   2  writes  the power-on above (nothing, if the domain is on already); then PLL_G3D must read stable
 *   3  asks    EL3, through Carbon, for the DTZPC restore: SMC 0x82000410 (1, 0x17410204)
 *   4  reads   GPU_ID (a G72, or nothing more is done), MMU_FEATURES (35 bits of GPU address at least), the cores
 *   5  writes  a soft reset; COHERENCY_ENABLE = 31
 *   6  writes  the L2, the tiler and the shader cores powered up
 *   7  runs    one job, the smallest there is (write a value into memory), through address space 0 in identity mode
 *   8  runs    the same job through the GPU's own page tables, at a GPU address of their making
 *   9  serves  HakiosMali started: programs can open the GPU (the original's "step 6 of 6" and RedwoodMaliStart())
 * sysctl kern.hakios.mali says the last step done: 9 is the GPU there for programs.
 *
 * Boot-args:
 *   mali=N           the first N of the GPU's nine steps of bring-up (9: all, then served); 0 or none: left alone
 *   mali_fastmap=0   the GPU walks its tables through its own caches, and every buffer entered is an UPDATE of AS 0
 *   mali_trace=1     a line for every job: "tl: j SEQ f|v q QUEUED b BEGAN e ENDED", in microseconds
 *
 * The user client is opened by the window server's process, backboardd, which does not run as root (PixOS learned that
 * on another phone): nothing here or in hakios.mali.cpp asks for root.
 *
 * Status: written, not yet run on the phone, and not compiled: the XNU interfaces are the original's, as it used them
 * against the same xnu-8796.121.2.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);                       /* pmap_paddr_t */
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);   /* osfmk/arm64/caches_asm.s */
extern vm_offset_t phystokv(uint64_t pa);
extern boolean_t pmap_valid_address(uint64_t addr);             /* memory the kernel manages */
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern void HakiosMaliStart(void);                              /* drivers/gpu/hakios.mali.cpp */
extern uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed, gMaliShown;   /* hakios.mali.cpp */
extern struct mali_made { uint64_t address, size; uint32_t asked, flags; } gMaliMade[32];

/* (every line of the steps to the loader's log as well, as the other drivers do: see hakios.buttons.c) */
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

#define STARLTE_SOC_BASE        0x10000000UL    /* where the loader's platform "starlte" says the SoC's registers begin */

#define PMU_PHYS                0x14060000UL    /* the PMU */
#define PMU_SIZE                0x5000
#define PMU_G3D_CONFIGURATION   0x4040          /* 0x14064040: [3:0] = 0xF asks for pd-embedded_g3d on, 0 off */
#define PMU_G3D_STATUS          0x4044          /* 0x14064044: [3:0] = 0xF when it is on */
#define G3D_ON                  0xfu
#define G3D_WAIT_MS             100u

#define CMU_G3D_PHYS            0x17400000UL
#define CMU_G3D_SIZE            0x1000
#define CMU_G3D_100             0x100           /* bit 4 set on, cleared off */
#define CMU_G3D_PLL_CON0        0x140           /* PLL_G3D's PLL_CON0; bit 4 set on, cleared off */
#define CMU_G3D_148             0x148           /* written CMU_G3D_148_ON first on, CMU_G3D_148_OFF last off */
#define CMU_G3D_BIT4            (1u << 4)
#define CMU_G3D_148_ON          0x30000003u
#define CMU_G3D_148_OFF         0x10000001u
#define PLL_ENABLE              (1u << 31)
#define PLL_STABLE              (1u << 29)
#define PLL_FIN_KHZ             26000ull
#define PLL_STABLE_MS           10u
/*
 * When the boot leaves PLL_G3D off (its rate is ACPM's DVFS's to set, and nothing here speaks to ACPM yet), step 2 starts
 * it itself the way Samsung's CAL programs a PLL_1018X (cal-if/ra.c, ra_pll_set_pmsk(); the rate from cmucal-node.c's
 * pll_g3d_rate_table): its lock time (PLL_LOCKTIME at +0x0: P x 150), then P, M, S with ENABLE and the output mux in one
 * write of PLL_CON0, then STABLE waited for. 320 MHz (M 320, P 13, S 1: a VCO of 640 MHz from 26 MHz), the table's lowest
 * and under every DVFS level the GPU is normally run at, so that the rail's voltage, whatever it is, is enough.
 * Boot-arg mali_pll=0: a PLL_G3D that does not run is left alone, and step 2 stops there.
 */
#define CMU_G3D_PLL_LOCKTIME    0x000
#define PLL_1018X_LOCK_FACTOR   150u
#define PLL_FALLBACK_M          320u
#define PLL_FALLBACK_P          13u
#define PLL_FALLBACK_S          1u
static uint32_t mali_pll = 1;

#define DTZPC_RESTORE           0x82000410u     /* Samsung's SiP call, as the CAL makes it after power-on */
#define DTZPC_ARGUMENT          1u
#define DTZPC_G3D               0x17410204u

#define MALI_PHYS               0x17500000UL
#define MALI_SIZE               0x5000
#define MALI_G72                0x6221u         /* GPU_ID >> 16 */

SYSCTL_DECL(_kern_hakios);              /* the node is drivers/core/hakios.platform.c's */

static vm_offset_t pmu, cmu_g3d, mali;
static uint32_t reached, gpu_id, shader_mask, l2_mask, last_step, step;
static bool powered_here;               /* step 2 switched the domain on: off again if the steps end short */
static bool gpu_answered;               /* step 4 read a G72: a soft reset before the power goes */
/*
 * The supply and the clock through ACPM (drivers/core/hakios.exynos_acpm.c), as Samsung's kernel has them made: the
 * S2MPS18's BUCK6 (vdd_g3d) held on before the domain's power-on -- PMIC register 0x2a, bits [7:6] = 11; the CAL sends
 * it as an update of "type" 3 (cal-if.c:178-186), the regulator driver addresses the same register as type 1, so it is
 * read back through type 1 and sent that way as well if type 3 left it unchanged -- and, once the domain is on, the
 * GPU's DVFS rate set (index 4, kHz) so that ACPM programs PLL_G3D and the voltage together from its own tables. Bits
 * [7:6] are put back as found when the domain goes off again. Boot-arg mali_acpm=0: none of it.
 */
#define B6CTRL                  0x2a
#define B6CTRL_ON               0xc0u           /* [7:6] = 11: always on; 10 the G3D power pin's, 00 off */
#define DVFS_G3D                4               /* GET_IDX(0x0B040004), cal-if */
#define DVFS_G3D_KHZ            260000u         /* the lowest of the device tree's levels */
static uint32_t mali_acpm = 1;
static bool acpm_ok, b6_changed;
static uint8_t b6_found;

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali, CTLFLAG_RD | CTLFLAG_LOCKED, &reached, 0,
    "how far the Mali came up: 9 is the GPU there for programs (drivers/gpu/hakios.mali.c)");
/*
 * What a program said about the GPU (the original's userland/src/mali_test.c writes it): repeated in the log once a
 * minute, so that it lands in the end of a run's log, which is the part that is kept.
 */
static uint32_t user_said;
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_user, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &user_said, 0,
    "what user space found: 1 a job ran, 2 no service, 3 no client, 4 no buffer, 5 no mapping, 6 the job did nothing");

/*
 * What gl_test drew, for the log's end as well: who drew it (1 the Mali, 2 the processor) times 1000 plus the share of
 * the picture inside the triangle in percent; and the middle pixel, red in the low byte.
 */
/*
 * kern.hakios.say: a line from a program, into the kernel's log (a job's own output is kept nowhere):
 * sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line)).
 */
#define KEPT_LINES 96
static char kept_lines[KEPT_LINES][200];
static uint32_t kept_next;
static LCK_GRP_DECLARE(kept_lock_group, "hakios-said");
static LCK_MTX_DECLARE(kept_lock, &kept_lock_group);

/* A line from the kernel itself, kept and said again every minute as kern.hakios.say's "!" ones are. */
void
hakios_keep(const char *format, ...)
{
	char line[200];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("%s\n", line);
	lck_mtx_lock(&kept_lock);
	strlcpy(kept_lines[kept_next % KEPT_LINES], line, sizeof(kept_lines[0]));
	kept_next++;
	lck_mtx_unlock(&kept_lock);
}

static int
say_line(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char line[512];
	size_t length = req->newlen;

	if (req->newptr == USER_ADDR_NULL) {
		return 0;
	}
	if (length > sizeof(line) - 1) {
		length = sizeof(line) - 1;
	}
	if (SYSCTL_IN(req, line, length) != 0) {
		return EINVAL;
	}
	line[length] = 0;
	printf("%s%s", line, length != 0 && line[length - 1] == '\n' ? "" : "\n");
	if (line[0] == '!') {                   /* kept, and said again every minute (mali_thread) */
		lck_mtx_lock(&kept_lock);
		strlcpy(kept_lines[kept_next % KEPT_LINES], line + 1, sizeof(kept_lines[0]));
		kept_next++;
		lck_mtx_unlock(&kept_lock);
	}
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, say, CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    say_line, "A", "");

static uint32_t gl_said, gl_middle;
/*
 * sysctl kern.hakios.bind_cpu=N: the calling thread runs on logical processor N from now on (-1: on any). The
 * scheduler of this kernel does not tell big cores from small ones, and on the X6826B CoreAnimation's render thread
 * spent 14 to 18 ms a frame wherever it happened to be (the original's run 147, 26 October). The Metal driver puts
 * that thread and its own worker on the big cores.
 */
extern void *cpu_to_processor(int cpu);
extern void *thread_bind(void *processor);
extern int thread_block(void *continuation);
extern unsigned int real_ncpus;

static int
bind_cpu(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int cpu = -1, error;

	error = SYSCTL_IN(req, &cpu, sizeof(cpu));
	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (cpu >= (int)real_ncpus) {
		return EINVAL;
	}
	(void)thread_bind(cpu < 0 ? NULL : cpu_to_processor(cpu));
	(void)thread_block(NULL);             /* over to it now */
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, bind_cpu, CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    bind_cpu, "I", "bind the calling thread to a logical processor");

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_gl, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &gl_said, 0, "");
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_gl_middle, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &gl_middle, 0, "");
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_id, CTLFLAG_RD | CTLFLAG_LOCKED, &gpu_id, 0, "GPU_ID");

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
	__asm__ volatile ("dsb sy");
}

/* Until (register & mask) == want, for at most `ms`. */
static bool
wait_for(vm_offset_t base, uint32_t at, uint32_t mask, uint32_t want, uint32_t ms)
{
	for (uint32_t us = 0; us < ms * 1000; us += 10) {
		if ((rd(base, at) & mask) == want) {
			return true;
		}
		delay(10);
	}
	return (rd(base, at) & mask) == want;
}

/* ---- the GPU's registers (Linux drivers/gpu/drm/panfrost/panfrost_regs.h) ---- */

#define GPU_ID              0x000
#define GPU_L2_FEATURES     0x004
#define GPU_CORE_FEATURES   0x008
#define GPU_TILER_FEATURES  0x00c
#define GPU_MEM_FEATURES    0x010
#define GPU_MMU_FEATURES    0x014       /* [7:0] bits of GPU address, [15:8] bits of physical address */
#define GPU_AS_PRESENT      0x018
#define GPU_JS_PRESENT      0x01c
#define GPU_INT_RAWSTAT     0x020
#define GPU_INT_CLEAR       0x024
#define GPU_INT_MASK        0x028
#define GPU_CMD             0x030
#define THREAD_MAX_THREADS  0x0a0
#define SHADER_PRESENT      0x100
#define SHADER_PRESENT_HI   0x104
#define TILER_PRESENT       0x110
#define L2_PRESENT          0x120
#define SHADER_READY        0x140
#define TILER_READY         0x150
#define L2_READY            0x160
#define SHADER_PWRON        0x180
#define TILER_PWRON         0x190
#define L2_PWRON            0x1a0
#define COHERENCY_FEATURES  0x300
#define COHERENCY_ENABLE    0x304
#define COHERENCY_NONE      31u
#define THREAD_TLS_ALLOC    0x310
#define IRQ_RESET_COMPLETED (1u << 8)

#define JOB_INT_RAWSTAT     0x1000
#define JOB_INT_CLEAR       0x1004
#define JOB_INT_MASK        0x1008
#define JS1                 0x1880      /* job slot 1, where compute-like jobs go */
#define JS_STATUS           0x24
#define JS_HEAD_NEXT_LO     0x40
#define JS_HEAD_NEXT_HI     0x44
#define JS_AFFINITY_NEXT_LO 0x50
#define JS_AFFINITY_NEXT_HI 0x54
#define JS_CONFIG_NEXT      0x58
#define JS_COMMAND_NEXT     0x60
#define JS_CONFIG           ((3u << 8) | (3u << 12) | (8u << 16))  /* AS 0, caches flushed both ends */

#define AS0                 0x2400
#define AS_TRANSTAB_LO      0x00
#define AS_TRANSTAB_HI      0x04
#define AS_MEMATTR_LO       0x08
#define AS_MEMATTR_HI       0x0c
#define AS_COMMAND          0x18
#define AS_FAULTSTATUS      0x1c
#define AS_STATUS           0x28
#define AS_TRANSCFG_LO      0x30
#define AS_TRANSCFG_HI      0x34

/*
 * A soft reset, and what a reset clears put back: COHERENCY_ENABLE, which says the GPU is to expect no coherency from
 * the rest of the chip (the original left it as the reset made it). False if the reset did not finish.
 */
static bool
gpu_reset(void)
{
	wr(mali, GPU_INT_MASK, 0);
	wr(mali, GPU_INT_CLEAR, 0xffffffffu);
	wr(mali, GPU_CMD, 1);                   /* soft reset */
	if (!wait_for(mali, GPU_INT_RAWSTAT, IRQ_RESET_COMPLETED, IRQ_RESET_COMPLETED, 100)) {
		return false;
	}
	wr(mali, GPU_INT_CLEAR, 0xffffffffu);
	wr(mali, COHERENCY_ENABLE, COHERENCY_NONE);
	return true;
}

/* ---- one job ---- */

/*
 * The smallest job there is, in a page of the kernel's own: a job header saying "write value", and its payload --
 * where, what kind of value (a 32-bit immediate), and the value -- the "where" being the same page 0x800 further on.
 * `gpu` is the address the GPU knows the page by: its physical address in identity mode, or wherever the page tables
 * put it. The processor's caches are cleaned either side, since the GPU here does not look into them.
 */
static bool
run_write_job(vm_offset_t page, uint64_t gpu, const char *what)
{
	volatile uint32_t *job = (volatile uint32_t *)page, *target = (volatile uint32_t *)(page + 0x800);
	bool ok;

	bzero((void *)page, PAGE_SIZE);
	job[4] = (2u << 1) | 1u | (1u << 16);   /* write value; 64-bit pointers; job index 1 */
	job[8] = (uint32_t)(gpu + 0x800);
	job[9] = (uint32_t)((gpu + 0x800) >> 32);
	job[10] = 6;                            /* a 32-bit immediate */
	job[12] = 0xc0ffee01u;
	FlushPoC_DcacheRegion(page, PAGE_SIZE);

	wr(mali, JOB_INT_MASK, 0);
	wr(mali, JOB_INT_CLEAR, 0xffffffffu);
	wr(mali, JS1 + JS_HEAD_NEXT_LO, (uint32_t)gpu);
	wr(mali, JS1 + JS_HEAD_NEXT_HI, (uint32_t)(gpu >> 32));
	wr(mali, JS1 + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(mali, JS1 + JS_AFFINITY_NEXT_HI, 0);
	wr(mali, JS1 + JS_CONFIG_NEXT, JS_CONFIG);
	wr(mali, JS1 + JS_COMMAND_NEXT, 1);     /* start */
	(void)wait_for(mali, JOB_INT_RAWSTAT, (1u << 1) | (1u << 17), 1u << 1, 100);

	FlushPoC_DcacheRegion(page, PAGE_SIZE);
	ok = *target == 0xc0ffee01u;
	say("mali: %u: %s: interrupts %#x, slot status %#x, MMU fault %#x, job status %#x, the word %#x%s\n", step, what,
	    rd(mali, JOB_INT_RAWSTAT), rd(mali, JS1 + JS_STATUS), rd(mali, AS0 + AS_FAULTSTATUS), job[0], *target,
	    ok ? " -- the GPU ran it" : " -- NOT WRITTEN");
	wr(mali, JOB_INT_CLEAR, 0xffffffffu);
	return ok;
}

static void
as0_update(uint64_t transtab, uint32_t transcfg)
{
	wr(mali, AS0 + AS_TRANSTAB_LO, (uint32_t)transtab);
	wr(mali, AS0 + AS_TRANSTAB_HI, (uint32_t)(transtab >> 32));
	wr(mali, AS0 + AS_TRANSCFG_LO, transcfg);
	wr(mali, AS0 + AS_TRANSCFG_HI, 0);
	wr(mali, AS0 + AS_MEMATTR_LO, 0x88888888u);     /* every attribute: write-back, the GPU's own caches */
	wr(mali, AS0 + AS_MEMATTR_HI, 0x88888888u);
	wr(mali, AS0 + AS_COMMAND, 1);                  /* update */
	(void)wait_for(mali, AS0 + AS_STATUS, 1, 0, 50);
}

/* ---- the GPU's own page tables ---- */

/*
 * The format Linux's panfrost gives a Bifrost GPU: ordinary AArch64 stage-1 tables with 4 KB pages (AS_TRANSCFG mode
 * 6). They translate 35 bits of address (the G52's MMU_FEATURES were 0x2823; step 4 checks the G72 has at least as
 * many), which makes three levels: the top table is indexed by bits 34..30, then 29..21, then 20..12. A page's entry is
 * its address, the access flag, inner shareable, readable and writable (AP 01), memory attribute 0, and the low bits 11
 * ("page").
 */
#define PTE_TABLE           3ull
#define PTE_PAGE            3ull
#define PTE_AF              (1ull << 10)
#define PTE_SH_INNER        (3ull << 8)
#define PTE_AP_RW           (1ull << 6)
#define PTE_ADDRESS         0x0000fffffffff000ull
#define GPU_VA_BITS         35u
#define GPU_PA_BITS_NEEDED  36u         /* the S9's memory: 0x80000000-0xFFFFFFFF and 0x880000000-0x8FFFFFFFF */
#define TRANSCFG_WALK_WB    (6u | (20u << 6) | (2u << 24) | (1u << 30))  /* 55 - 35 address bits; walks write-back */
#define TRANSCFG_WALK_NC    (6u | (20u << 6) | (1u << 24))              /* ... walks not cached */
/*
 * Boot-arg mali_fastmap (1 unless =0): the GPU walks its tables without caching them, so an entry the processor has
 * written (and cleaned to memory) is what the next walk reads -- and a buffer newly entered needs no command to the MMU
 * at all, an empty entry being one no TLB keeps. Each new buffer used to be an UPDATE of address space 0 and a wait for
 * it: 2.4 ms a buffer, 320 buffers every five seconds in the window server (the original's run 139, 26 October). Taking
 * a buffer out still updates.
 */
static int mali_fastmap = 1;
#define TRANSCFG_AARCH64_4K (mali_fastmap ? TRANSCFG_WALK_NC : TRANSCFG_WALK_WB)

static uint64_t *pt_top;
static uint64_t pt_top_phys;
static uint32_t pt_refused;             /* addresses not taken into, or not followed through, the tables */

/*
 * The kernel's address of the table an entry points to; NULL for an empty entry, and for one whose address is not
 * memory the kernel manages (said, the first few times). The check comes before phystokv(), which panics on an address
 * outside the kernel's memory, and before the cache is cleaned over the table's entries: PixOS, on another phone,
 * panicked cleaning the cache over an address it had not checked. The tables are all the kernel's own pages, so this
 * should never refuse; if it does, an entry has been overwritten.
 */
static uint64_t *
table_of(uint64_t entry)
{
	uint64_t phys = entry & PTE_ADDRESS;

	if (!(entry & 1)) {
		return NULL;
	}
	if (phys == 0 || !pmap_valid_address(phys)) {
		if (pt_refused++ < 4) {
			printf("mali: a table entry %#llx points outside the kernel's memory: not followed\n", entry);
		}
		return NULL;
	}
	return (uint64_t *)phystokv(phys);
}

static uint64_t *
pt_page(uint64_t *phys)
{
	vm_offset_t page = 0;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	*phys = kvtophys(page);
	if (*phys == 0 || !pmap_valid_address(*phys)) {
		kmem_free(kernel_map, page, PAGE_SIZE);
		return NULL;
	}
	FlushPoC_DcacheRegion(page, PAGE_SIZE);  /* its zeroes in memory, where the GPU's walk reads them */
	return (uint64_t *)page;
}

/* The table below `entry`, made if there is none yet. */
static uint64_t *
pt_below(uint64_t *entry)
{
	uint64_t phys;
	uint64_t *table;

	if (*entry & 1) {
		return table_of(*entry);
	}
	table = pt_page(&phys);
	if (table == NULL) {
		return NULL;
	}
	*entry = phys | PTE_TABLE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return table;
}

/* One 4 KB page of physical memory at GPU address `gpu` (with the lock held, or before anyone else runs). */
static bool
map_page(uint64_t gpu, uint64_t phys)
{
	uint64_t *middle, *bottom, *entry;

	if (gpu >= (1ull << GPU_VA_BITS)) {
		return false;
	}
	if (phys == 0 || !pmap_valid_address(phys)) {
		/* the GPU is given memory the kernel manages, and nothing else (a register block, memory of the firmware's) */
		if (pt_refused++ < 4) {
			printf("mali: physical address %#llx is not memory the kernel manages: not entered at %#llx\n", phys, gpu);
		}
		return false;
	}
	if (pt_top == NULL && (pt_top = pt_page(&pt_top_phys)) == NULL) {
		return false;
	}
	if ((middle = pt_below(&pt_top[(gpu >> 30) & 31])) == NULL || (bottom = pt_below(&middle[(gpu >> 21) & 511])) == NULL) {
		return false;
	}
	entry = &bottom[(gpu >> 12) & 511];
	*entry = (phys & PTE_ADDRESS) | PTE_AF | PTE_SH_INNER | PTE_AP_RW | PTE_PAGE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return true;
}

/* The entries of `size` bytes from GPU address `gpu` emptied, as far as there are tables (with the lock held). */
static void
unmap_pages(uint64_t gpu, uint64_t size)
{
	if (pt_top == NULL) {
		return;
	}
	for (uint64_t at = gpu; at < gpu + size; at += 4096) {        /* the GPU's pages, not the kernel's 16 KB */
		uint64_t *table = table_of(pt_top[(at >> 30) & 31]), *entry;

		if (table == NULL) {
			continue;
		}
		table = table_of(table[(at >> 21) & 511]);
		if (table == NULL) {
			continue;
		}
		entry = &table[(at >> 12) & 511];
		*entry = 0;
		FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	}
}

/* ---- the Exynos 9810's side: power, the clock, the TrustZone protection controller ---- */

/* PLL_G3D as it is, said; its PLL_CON0 back. */
static uint32_t
pll_say(const char *when)
{
	uint32_t con0 = rd(cmu_g3d, CMU_G3D_PLL_CON0);
	uint32_t m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, s = con0 & 7;
	uint32_t khz = p != 0 ? (uint32_t)(PLL_FIN_KHZ * m / ((uint64_t)p << s)) : 0;

	say("mali: %u: PLL_G3D %s: PLL_CON0 %#x: M %u, P %u, S %u, %u.%03u MHz from 26 MHz; %s, %s%s\n", step, when, con0, m,
	    p, s, khz / 1000, khz % 1000, (con0 & PLL_ENABLE) ? "enabled" : "not enabled",
	    (con0 & PLL_STABLE) ? "stable" : "NOT STABLE",
	    khz != 0 && (khz < 260000 || khz > 572000) ? " (outside the 260 to 572 MHz of ACPM's DVFS)" : "");
	return con0;
}

/*
 * The domain off again, the power-on in reverse: the PMU's G3D configuration [3:0] = 0 and its status waited for, then
 * CMU_G3D +0x100 and +0x140 bit 4 cleared, and last CMU_G3D +0x148 = 0x10000001. A GPU that has answered is reset first,
 * so that nothing of it is running when the power goes.
 */
static void
g3d_off(void)
{
	if (gpu_answered) {
		say("mali: before the power goes: a soft reset (GPU_CMD = 1)\n");
		if (!gpu_reset()) {
			say("mali: the reset did not finish (GPU_INT_RAWSTAT %#x): the power goes anyway\n", rd(mali, GPU_INT_RAWSTAT));
		}
		gpu_answered = false;
	}
	say("mali: switching the G3D domain off again, as it was found: PMU configuration [3:0] = 0, then CMU_G3D +0x100 and "
	    "+0x140 bit 4 cleared, +0x148 = %#x\n", CMU_G3D_148_OFF);
	wr(pmu, PMU_G3D_CONFIGURATION, rd(pmu, PMU_G3D_CONFIGURATION) & ~G3D_ON);
	if (!wait_for(pmu, PMU_G3D_STATUS, G3D_ON, 0, G3D_WAIT_MS)) {
		say("mali: the domain did not go off: status %#x after %u ms\n", rd(pmu, PMU_G3D_STATUS), G3D_WAIT_MS);
	}
	wr(cmu_g3d, CMU_G3D_100, rd(cmu_g3d, CMU_G3D_100) & ~CMU_G3D_BIT4);
	wr(cmu_g3d, CMU_G3D_PLL_CON0, rd(cmu_g3d, CMU_G3D_PLL_CON0) & ~CMU_G3D_BIT4);
	wr(cmu_g3d, CMU_G3D_148, CMU_G3D_148_OFF);
	if (b6_changed) {
		say("mali: BUCK6 back as it was found: B6CTRL bits [7:6] = %u\n", (b6_found >> 6) & 3);
		(void)hakios_acpm_pmic_update(HAKIOS_ACPM_PMIC_PMIC, B6CTRL, b6_found & B6CTRL_ON, B6CTRL_ON);
		b6_changed = false;
	}
	say("mali: the G3D domain: configuration %#x, status %#x; CMU_G3D +0x100 %#x, +0x140 %#x, +0x148 %#x\n",
	    rd(pmu, PMU_G3D_CONFIGURATION), rd(pmu, PMU_G3D_STATUS), rd(cmu_g3d, CMU_G3D_100), rd(cmu_g3d, CMU_G3D_PLL_CON0),
	    rd(cmu_g3d, CMU_G3D_148));
	powered_here = false;
}

/* One SMC to EL3 (through Carbon at EL2): x0 back. */
static uint64_t
smc(uint64_t function, uint64_t first, uint64_t second, uint64_t third)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = first;
	register uint64_t x2 __asm__("x2") = second;
	register uint64_t x3 __asm__("x3") = third;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/* ---- the steps ---- */

/* 1: what the boot has left, read only. */
static bool
step_look(void)
{
	uint32_t status, word;

	say("mali: 1: mapping the PMU (%#lx), CMU_G3D (%#lx) and the GPU (%#lx); nothing is written in this step\n", PMU_PHYS,
	    CMU_G3D_PHYS, MALI_PHYS);
	pmu = ml_io_map(PMU_PHYS, PMU_SIZE);
	cmu_g3d = ml_io_map(CMU_G3D_PHYS, CMU_G3D_SIZE);
	mali = ml_io_map(MALI_PHYS, MALI_SIZE);
	if (pmu == 0 || cmu_g3d == 0 || mali == 0) {
		say("mali: 1: cannot map them\n");
		return false;
	}
	say("mali: 1: reading the PMU's G3D configuration (%#lx) and status (%#lx)\n", PMU_PHYS + PMU_G3D_CONFIGURATION,
	    PMU_PHYS + PMU_G3D_STATUS);
	status = rd(pmu, PMU_G3D_STATUS);
	say("mali: 1: G3D configuration %#x, status %#x: the domain is %s\n", rd(pmu, PMU_G3D_CONFIGURATION), status,
	    (status & G3D_ON) == G3D_ON ? "on" : (status & G3D_ON) == 0 ? "off" : "NEITHER ON NOR OFF");
	say("mali: 1: reading CMU_G3D +0x100, +0x140 and +0x148 (outside the domain: Samsung's sequence writes them before "
	    "it has power)\n");
	word = rd(cmu_g3d, CMU_G3D_148);
	say("mali: 1: CMU_G3D +0x100 %#x (bit 4 %s), +0x148 %#x (%#x is what switching on writes, %#x switching off)\n",
	    rd(cmu_g3d, CMU_G3D_100), (rd(cmu_g3d, CMU_G3D_100) & CMU_G3D_BIT4) ? "set" : "clear", word, CMU_G3D_148_ON,
	    CMU_G3D_148_OFF);
	(void)pll_say("as found");
	if ((status & G3D_ON) == G3D_ON) {
		say("mali: 1: the domain is on: reading GPU_ID\n");
		say("mali: 1: GPU_ID %#x\n", rd(mali, GPU_ID));
	} else {
		say("mali: 1: the domain is not on: the GPU's own registers are not read\n");
	}
	if (mali_acpm && (acpm_ok = hakios_acpm_init(say))) {
		uint8_t b6 = 0;
		uint32_t khz = 0;
		int error = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6CTRL, &b6);

		say("mali: 1: ACPM: the S2MPS18's B6CTRL (vdd_g3d) %s %#x: [7:6] = %u (%s)\n", error ? "NOT READ, error" : "is",
		    error ? (unsigned)error : b6, (b6 >> 6) & 3, error ? "?" : (b6 >> 6) == 3 ? "always on" :
		    (b6 >> 6) == 2 ? "the G3D power pin's" : (b6 >> 6) == 1 ? "on in standby" : "OFF");
		error = hakios_acpm_dvfs_get(DVFS_G3D, &khz);
		say("mali: 1: ACPM: the GPU's DVFS rate %s %u kHz\n", error ? "NOT READ, error" : "is", error ? (unsigned)error : khz);
	}
	return true;
}

/* 2: power, and a clock that runs. */
static bool
step_power(void)
{
	uint32_t status = rd(pmu, PMU_G3D_STATUS) & G3D_ON;

	if (status == G3D_ON) {
		say("mali: 2: the domain is on already: nothing written, and it is left on whatever happens\n");
	} else if (status != 0) {
		say("mali: 2: the domain is neither on nor off (status %#x): left alone\n", rd(pmu, PMU_G3D_STATUS));
		return false;
	} else {
		uint32_t con0 = rd(cmu_g3d, CMU_G3D_PLL_CON0);

		if (!(con0 & PLL_ENABLE) && mali_pll) {
			uint32_t want = (con0 & ~((0x3ffu << 16) | (0x3fu << 8) | 7u)) | PLL_FALLBACK_M << 16 | PLL_FALLBACK_P << 8 |
			    PLL_FALLBACK_S | CMU_G3D_BIT4 | PLL_ENABLE;

			say("mali: 2: PLL_G3D is off (PLL_CON0 %#x): starting it at 320 MHz as the CAL starts a PLL_1018X: "
			    "PLL_LOCKTIME = %u, then PLL_CON0 = %#x (mali_pll=0 would not)\n", con0,
			    PLL_FALLBACK_P * PLL_1018X_LOCK_FACTOR, want);
			wr(cmu_g3d, CMU_G3D_PLL_LOCKTIME, PLL_FALLBACK_P * PLL_1018X_LOCK_FACTOR);
			wr(cmu_g3d, CMU_G3D_PLL_CON0, want);
			if (!wait_for(cmu_g3d, CMU_G3D_PLL_CON0, PLL_STABLE, PLL_STABLE, PLL_STABLE_MS)) {
				(void)pll_say("started here");
				say("mali: 2: PLL_G3D did not become stable in %u ms: stopping it again\n", PLL_STABLE_MS);
				wr(cmu_g3d, CMU_G3D_PLL_CON0, con0);
				return false;
			}
			(void)pll_say("started here");
		}
		if (acpm_ok && hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6CTRL, &b6_found) == 0) {
			uint8_t now = b6_found;

			if ((b6_found & B6CTRL_ON) != B6CTRL_ON) {
				say("mali: 2: ACPM: holding BUCK6 (vdd_g3d) on as Samsung's CAL does: PMIC update type 3, register %#x, "
				    "bits [7:6] = 11 (B6CTRL was %#x)\n", B6CTRL, b6_found);
				b6_changed = true;
				(void)hakios_acpm_pmic_update(3, B6CTRL, B6CTRL_ON, B6CTRL_ON);
				if (hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6CTRL, &now) == 0 && (now & B6CTRL_ON) != B6CTRL_ON) {
					say("mali: 2: ACPM: type 3 left B6CTRL at %#x: the same through type 1, the regulator driver's\n", now);
					(void)hakios_acpm_pmic_update(HAKIOS_ACPM_PMIC_PMIC, B6CTRL, B6CTRL_ON, B6CTRL_ON);
					(void)hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6CTRL, &now);
				}
			}
			say("mali: 2: ACPM: B6CTRL %#x: BUCK6 %s\n", now, (now & B6CTRL_ON) == B6CTRL_ON ? "held on" :
			    "NOT HELD ON (the domain may not come up, or the GPU not answer)");
		} else {
			say("mali: 2: no ACPM (mali_acpm=0, or it did not answer): BUCK6 (vdd_g3d) is left as it is, under the G3D "
			    "power pin if the PMIC is set so\n");
		}
		say("mali: 2: writing CMU_G3D +0x148 = %#x (was %#x)\n", CMU_G3D_148_ON, rd(cmu_g3d, CMU_G3D_148));
		powered_here = true;            /* from the first write on, the way back is g3d_off() */
		wr(cmu_g3d, CMU_G3D_148, CMU_G3D_148_ON);
		say("mali: 2: writing CMU_G3D +0x140 |= bit 4 (was %#x)\n", rd(cmu_g3d, CMU_G3D_PLL_CON0));
		wr(cmu_g3d, CMU_G3D_PLL_CON0, rd(cmu_g3d, CMU_G3D_PLL_CON0) | CMU_G3D_BIT4);
		say("mali: 2: writing CMU_G3D +0x100 |= bit 4 (was %#x)\n", rd(cmu_g3d, CMU_G3D_100));
		wr(cmu_g3d, CMU_G3D_100, rd(cmu_g3d, CMU_G3D_100) | CMU_G3D_BIT4);
		say("mali: 2: writing the PMU's G3D configuration [3:0] = 0xf (was %#x), then waiting up to %u ms for its status\n",
		    rd(pmu, PMU_G3D_CONFIGURATION), G3D_WAIT_MS);
		wr(pmu, PMU_G3D_CONFIGURATION, (rd(pmu, PMU_G3D_CONFIGURATION) & ~G3D_ON) | G3D_ON);
		if (!wait_for(pmu, PMU_G3D_STATUS, G3D_ON, G3D_ON, G3D_WAIT_MS)) {
			say("mali: 2: the domain did not come up: status %#x after %u ms (its supply, the known gap above?)\n",
			    rd(pmu, PMU_G3D_STATUS), G3D_WAIT_MS);
			return false;
		}
		say("mali: 2: the domain is on: status %#x\n", rd(pmu, PMU_G3D_STATUS));
	}
	if (acpm_ok) {
		uint32_t khz = 0;
		int error = hakios_acpm_dvfs_set(DVFS_G3D, DVFS_G3D_KHZ);

		(void)hakios_acpm_dvfs_get(DVFS_G3D, &khz);
		say("mali: 2: ACPM: the GPU's DVFS rate set to %u kHz (%s): it reads %u kHz\n", DVFS_G3D_KHZ,
		    error ? "the request failed" : "asked", khz);
	}
	/* only looked at here (ACPM's DVFS sets its rate, or step 2 started it): without it running, the GPU has no clock */
	if (!wait_for(cmu_g3d, CMU_G3D_PLL_CON0, PLL_STABLE, PLL_STABLE, PLL_STABLE_MS)) {
		(void)pll_say("with the domain on");
		say("mali: 2: PLL_G3D is not stable after %u ms: the GPU would have no clock to answer with\n", PLL_STABLE_MS);
		return false;
	}
	(void)pll_say("with the domain on");
	return true;
}

/* 3: the G3D's TrustZone protection controller set up again by EL3. */
static bool
step_dtzpc(void)
{
	uint64_t answer;

	say("mali: 3: about to make SMC %#x (%u, %#x): the DTZPC restore Samsung's CAL makes once the G3D has power; Carbon, "
	    "at EL2, passes it on to EL3\n", DTZPC_RESTORE, DTZPC_ARGUMENT, DTZPC_G3D);
	answer = smc(DTZPC_RESTORE, DTZPC_ARGUMENT, DTZPC_G3D, 0);
	say("mali: 3: SMC %#x returned %#llx\n", DTZPC_RESTORE, answer);
	if ((uint32_t)answer != 0) {
		/* (an assumption: 0 is taken for success; the facts this driver comes from do not say what success returns) */
		say("mali: 3: not 0: %s\n", (uint32_t)answer == 0xffffffffu ?
		    "the call is not known to whoever answered it (SMCCC's NOT_SUPPORTED)" : "taken as a refusal");
		return false;
	}
	return true;
}

/* 4: which GPU, and what it has. */
static bool
step_gpu(void)
{
	uint32_t mmu, va_bits, pa_bits, shaders_hi, as_present, js_present, tiler;

	say("mali: 4: reading GPU_ID at %#lx: should the run end here, the domain says on and the GPU does not answer "
	    "(its supply, the known gap, or the DTZPC)\n", MALI_PHYS);
	gpu_id = rd(mali, GPU_ID);
	say("mali: 4: GPU_ID %#x: architecture %u.%u revision %u, product %u, version r%up%u, status %u\n", gpu_id,
	    gpu_id >> 28, (gpu_id >> 24) & 0xf, (gpu_id >> 20) & 0xf, (gpu_id >> 16) & 0xf, (gpu_id >> 12) & 0xf,
	    (gpu_id >> 4) & 0xff, gpu_id & 0xf);
	if ((gpu_id >> 16) != MALI_G72) {
		if (((gpu_id >> 16) & 0xf00f) != 0x6001) {
			say("mali: 4: not a Mali-G72 (GPU_ID >> 16 should be %#x): nothing more is done with it\n", MALI_G72);
			return false;
		}
		say("mali: 4: a G72 of another revision than the %#x expected: going on\n", MALI_G72);
	}
	mmu = rd(mali, GPU_MMU_FEATURES);
	va_bits = mmu & 0xff;
	pa_bits = (mmu >> 8) & 0xff;
	say("mali: 4: MMU_FEATURES %#x: %u bits of GPU address, %u of physical (the page tables want %u and %u; the G52 "
	    "the driver was written for had 0x2823)\n", mmu, va_bits, pa_bits, GPU_VA_BITS, GPU_PA_BITS_NEEDED);
	if (va_bits < GPU_VA_BITS) {
		say("mali: 4: fewer than %u bits of GPU address: the page tables here start at bit 34; not going on\n", GPU_VA_BITS);
		return false;
	}
	if (pa_bits < GPU_PA_BITS_NEEDED) {
		say("mali: 4: fewer than %u bits of physical address, and this phone's memory reaches 0x8FFFFFFFF: not going on\n",
		    GPU_PA_BITS_NEEDED);
		return false;
	}
	shader_mask = rd(mali, SHADER_PRESENT);
	shaders_hi = rd(mali, SHADER_PRESENT_HI);
	tiler = rd(mali, TILER_PRESENT);
	l2_mask = rd(mali, L2_PRESENT);
	as_present = rd(mali, GPU_AS_PRESENT);
	js_present = rd(mali, GPU_JS_PRESENT);
	say("mali: 4: shader cores %#x (%u of them; above bit 31 %#x), tiler %#x, L2 %#x; address spaces %#x, job slots %#x\n",
	    shader_mask, __builtin_popcount(shader_mask), shaders_hi, tiler, l2_mask, as_present, js_present);
	say("mali: 4: L2_FEATURES %#x, CORE_FEATURES %#x, TILER_FEATURES %#x, MEM_FEATURES %#x, THREAD_MAX_THREADS %u, "
	    "THREAD_TLS_ALLOC %u, COHERENCY_FEATURES %#x\n", rd(mali, GPU_L2_FEATURES), rd(mali, GPU_CORE_FEATURES),
	    rd(mali, GPU_TILER_FEATURES), rd(mali, GPU_MEM_FEATURES), rd(mali, THREAD_MAX_THREADS), rd(mali, THREAD_TLS_ALLOC),
	    rd(mali, COHERENCY_FEATURES));
	if (shader_mask == 0 || shaders_hi != 0) {
		say("mali: 4: shader cores %#x:%08x: the jobs are given 32 bits of affinity; not going on\n", shaders_hi, shader_mask);
		return false;
	}
	if (!(as_present & 1) || (js_present & 3) != 3 || tiler == 0 || l2_mask == 0) {
		say("mali: 4: no address space 0, job slots 0 and 1, tiler or L2: not going on\n");
		return false;
	}
	gpu_answered = true;
	return true;
}

/* 5: a soft reset, and no coherency. */
static bool
step_reset(void)
{
	say("mali: 5: writing GPU_INT_MASK = 0, GPU_CMD = 1 (soft reset), then COHERENCY_ENABLE = %u (none)\n", COHERENCY_NONE);
	if (!gpu_reset()) {
		say("mali: 5: GPU_ID %#x, but it did not finish its reset (GPU_INT_RAWSTAT %#x)\n", gpu_id, rd(mali, GPU_INT_RAWSTAT));
		return false;
	}
	say("mali: 5: reset; COHERENCY_ENABLE reads %#x\n", rd(mali, COHERENCY_ENABLE));
	return true;
}

/* 6: the cores. */
static bool
step_cores(void)
{
	uint32_t tiler = rd(mali, TILER_PRESENT);

	say("mali: 6: writing L2_PWRON %#x, TILER_PWRON %#x, SHADER_PWRON %#x\n", l2_mask, tiler, shader_mask);
	wr(mali, L2_PWRON, l2_mask);
	wr(mali, TILER_PWRON, tiler);
	wr(mali, SHADER_PWRON, shader_mask);
	if (!wait_for(mali, L2_READY, l2_mask, l2_mask, 50) || !wait_for(mali, TILER_READY, tiler, tiler, 50) ||
	    !wait_for(mali, SHADER_READY, shader_mask, shader_mask, 50)) {
		say("mali: 6: its cores did not all power up: L2 ready %#x, tiler %#x, shaders %#x\n", rd(mali, L2_READY),
		    rd(mali, TILER_READY), rd(mali, SHADER_READY));
		return false;
	}
	say("mali: 6: GPU_ID %#x (Mali-G72 r%up%u), %u shader cores %#x, tiler %#x, L2 %#x: all powered\n", gpu_id,
	    (gpu_id >> 12) & 0xf, (gpu_id >> 4) & 0xff, __builtin_popcount(shader_mask), shader_mask, tiler, l2_mask);
	return true;
}

/*
 * 7 and 8: two jobs, first in identity mode, which needs no tables and so says whether the job manager works at all;
 * then the same job at a GPU address the tables give the page, which says whether they do. Afterwards address space 0
 * stays on the tables, which is how anything that draws will need it, and the page's entry is taken out of them again.
 */
#define FIRST_JOB_GPU       0x00100000ull

static vm_offset_t job_page;

static bool
step_job_identity(void)
{
	if (kmem_alloc(kernel_map, &job_page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		job_page = 0;
		say("mali: 7: no page for the first jobs\n");
		return false;
	}
	say("mali: 7: address space 0 in identity mode (AS_TRANSCFG 2); one job on slot 1 at %#llx\n", kvtophys(job_page));
	as0_update(0, 2);                       /* identity: a GPU address is the physical one */
	return run_write_job(job_page, kvtophys(job_page), "first job, identity mode");
}

static bool
step_job_tables(void)
{
	if (!map_page(FIRST_JOB_GPU, kvtophys(job_page))) {
		say("mali: 8: no memory for page tables\n");
		return false;
	}
	say("mali: 8: the page at GPU address %#llx in the GPU's tables (at %#llx), address space 0 on them; the same job "
	    "again\n", FIRST_JOB_GPU, pt_top_phys);
	as0_update(pt_top_phys, TRANSCFG_AARCH64_4K);
	return run_write_job(job_page, FIRST_JOB_GPU, "second job, through the GPU's page tables");
}

/* 9: there for programs. */
static bool
step_serve(void)
{
	say("mali: 9: HakiosMali started: programs can draw with the GPU (Mesa's panfrost; drivers/gpu/hakios.mali.cpp)\n");
	reached = 9;                            /* (the user client asks hakios_mali_ready()) */
	HakiosMaliStart();
	return true;
}

static const struct {
	const char *name;
	bool (*make)(void);
} steps[] = {
	{ "what the boot left, read only", step_look },
	{ "power on", step_power },
	{ "the DTZPC restore", step_dtzpc },
	{ "GPU_ID and what the GPU has", step_gpu },
	{ "a soft reset", step_reset },
	{ "the cores", step_cores },
	{ "a job in identity mode", step_job_identity },
	{ "a job through page tables", step_job_tables },
	{ "HakiosMali", step_serve },
};
#define STEPS ((uint32_t)(sizeof(steps) / sizeof(steps[0])))

/* ---- what the service that programs talk to uses (drivers/gpu/hakios.mali.cpp) ---- */

/*
 * Linux's panfrost interface, as far as Mesa's panfrost needs it to draw, is answered by the IOKit service HakiosMali;
 * this is its half in the hardware: parameters, GPU addresses, pages entered into and taken out of the tables, and
 * jobs. One address space (0) for everyone, and one job at a time -- simple, and enough to see Mesa draw; overlapping
 * jobs and per-client address spaces come after.
 */
static LCK_GRP_DECLARE(mali_lock_group, "hakios-mali");
static LCK_MTX_DECLARE(mali_lock, &mali_lock_group);
/*
 * Held while a job runs, and by what must not change the address space under one (an UPDATE when entries are taken
 * out); taken before mali_lock, never after. mali_lock itself is no longer held for the length of a job: making a
 * buffer had to wait for the job running to finish (2.5 ms a buffer, 26 October).
 */
static LCK_MTX_DECLARE(mali_job_lock, &mali_lock_group);

/*
 * Until a job is done, without keeping a core busy: a moment of polling for the short ones, then asleep 200 us at a
 * time. The wait used to spin (delay(10) in a loop) for as long as the GPU worked -- a whole core of the four lost to
 * every frame's rendering.
 */
static int mali_wait_event;
static LCK_MTX_DECLARE(mali_wait_lock, &mali_lock_group);

static bool
wait_job(uint32_t at, uint32_t mask, uint32_t want, uint32_t ms)
{
	uint64_t deadline;

	for (int i = 0; i < 20; i++) {
		if ((rd(mali, at) & mask) == want) {
			return true;
		}
		delay(10);
	}
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if ((rd(mali, at) & mask) == want) {
			return true;
		}
		uint64_t nap;

		clock_interval_to_deadline(200, NSEC_PER_USEC, &nap);
		lck_mtx_lock(&mali_wait_lock);
		(void)lck_mtx_sleep_deadline(&mali_wait_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_wait_event, THREAD_UNINT, nap);
		lck_mtx_unlock(&mali_wait_lock);
	}
	return (rd(mali, at) & mask) == want;
}

#define MMU_INT_RAWSTAT     0x2000
#define MMU_INT_CLEAR       0x2004
#define AS_LOCKADDR_LO      0x10
#define AS_LOCKADDR_HI      0x14
#define AS_COMMAND_LOCK     2
#define AS_COMMAND_FLUSH_PT 4
#define JS0                 0x1800      /* fragment jobs; everything else goes to slot 1 */
#define GPU_ADDRESS_FIRST   0x01000000ull
#define GPU_ADDRESS_END     (1ull << GPU_VA_BITS)

static uint64_t next_address = GPU_ADDRESS_FIRST;

/*
 * The last buffers handed GPU addresses, and whether they have been taken out of the tables since: addresses are never
 * given twice, so a translation fault is a job writing into a buffer already freed or past the end of one, and this
 * says which. With mali_lock held.
 */
#define MALI_RECENT 4096
static struct {
	uint64_t address, size;
	uint32_t freed, kind;                   /* kind: 0 not said, 1 a panfrost BO, 2 an IOSurface */
	uint32_t width, height, row_bytes, format, offset;
} mali_recent[MALI_RECENT];
static uint32_t mali_recent_next;

/* What a buffer just made is, for describe_address. */
void
hakios_mali_tag(uint64_t address, uint32_t kind, uint32_t width, uint32_t height, uint32_t row_bytes,
    uint32_t format, uint32_t offset)
{
	lck_mtx_lock(&mali_lock);
	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].address == address && mali_recent[i].size != 0) {
			mali_recent[i].kind = kind;
			mali_recent[i].width = width;
			mali_recent[i].height = height;
			mali_recent[i].row_bytes = row_bytes;
			mali_recent[i].format = format;
			mali_recent[i].offset = offset;
		}
	}
	lck_mtx_unlock(&mali_lock);
}

static void
say_kind(int i)
{
	if (mali_recent[i].kind == 2) {
		printf("mali:     an IOSurface %ux%u, %u bytes a row, format %#x, from byte %u\n", mali_recent[i].width,
		    mali_recent[i].height, mali_recent[i].row_bytes, mali_recent[i].format, mali_recent[i].offset);
	} else {
		printf("mali:     %s\n", mali_recent[i].kind == 1 ? "a panfrost BO" : "made by nothing that said");
	}
}

static void
describe_address(uint64_t address)
{
	int below = -1;

	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].size == 0) {
			continue;
		}
		if (address >= mali_recent[i].address && address < mali_recent[i].address + mali_recent[i].size) {
			printf("mali:   the fault is %#llx into a buffer of %#llx bytes at %#llx, %s\n",
			    address - mali_recent[i].address, mali_recent[i].size, mali_recent[i].address,
			    mali_recent[i].freed ? "FREED already" : "still there (a page not entered?)");
			say_kind(i);
			return;
		}
		if (mali_recent[i].address <= address && (below < 0 || mali_recent[i].address > mali_recent[below].address)) {
			below = i;
		}
	}
	if (below >= 0) {
		printf("mali:   the fault is %#llx past the end of a buffer of %#llx bytes at %#llx (%s)\n",
		    address - (mali_recent[below].address + mali_recent[below].size), mali_recent[below].size,
		    mali_recent[below].address, mali_recent[below].freed ? "freed" : "live");
		say_kind(below);
	} else {
		printf("mali:   the fault is in no buffer of the last %u\n", MALI_RECENT);
	}
}

/* The last job that did not finish, to be repeated where the end of the log keeps it. */
static struct {
	uint32_t count, slot, raw, status, mmu, fault, fault_address;
	uint64_t chain, transtab, walk[3];      /* the tables the GPU had, and their entries for the fault's address */
} last_failure;

bool
hakios_mali_map(uint64_t gpu, uint64_t phys)
{
	bool done;

	lck_mtx_lock(&mali_lock);
	done = map_page(gpu, phys);
	lck_mtx_unlock(&mali_lock);
	return done;
}

bool
hakios_mali_ready(void)
{
	return reached >= STEPS;
}

/* Linux's DRM_PANFROST_PARAM_*, in its order, from the registers they come from. */
bool
hakios_mali_parameter(uint32_t which, uint64_t *value)
{
	static const uint16_t simple[] = {
		[5] = 0xe00,    /* STACK_PRESENT (none on Bifrost: reads 0) */
		[6] = 0x018, [7] = 0x01c, [8] = 0x004, [9] = 0x008, [10] = 0x00c, [11] = 0x010, [12] = 0x014,
		[13] = 0x0ac, [14] = 0x0a0, [15] = 0x0a4, [16] = 0x0a8, [17] = 0x300,
		[18] = 0x0b0, [19] = 0x0b4, [20] = 0x0b8, [21] = 0x0bc,
	};

	if (!hakios_mali_ready()) {
		return false;
	}
	switch (which) {
	case 0:
		*value = gpu_id >> 16;
		return true;
	case 1:
		*value = gpu_id & 0xffff;
		return true;
	case 2:
	case 3:
	case 4:
		*value = rd(mali, 0x100 + (which - 2) * 0x10) | ((uint64_t)rd(mali, 0x104 + (which - 2) * 0x10) << 32);
		return true;
	case 38:                                /* NR_CORE_GROUPS: as Linux panfrost counts them, one per L2 */
		*value = __builtin_popcount(l2_mask) != 0 ? (uint64_t)__builtin_popcount(l2_mask) : 1;
		return true;
	case 39:
		/*
		 * THREAD_TLS_ALLOC (0x310). Answered 0 on the G52, panfrost sized the stack of a spilling shader by
		 * THREAD_MAX_THREADS instead, and the GPU, which gives each core more stacks than that, wrote past the end of
		 * it (25 October: every write fault of the uber shaders, up to 0x15870 past a 0x60000 buffer). Never less than
		 * that fallback, whatever the register says.
		 */
		*value = rd(mali, THREAD_TLS_ALLOC);
		if (*value < rd(mali, THREAD_MAX_THREADS)) {
			*value = rd(mali, THREAD_MAX_THREADS);
		}
		{
			static int said;
			if (!said++) {
				printf("mali: THREAD_TLS_ALLOC %#x, THREAD_MAX_THREADS %#x, SHADER_PRESENT %#x\n", rd(mali, THREAD_TLS_ALLOC),
				    rd(mali, THREAD_MAX_THREADS), rd(mali, SHADER_PRESENT));
			}
		}
		return true;
	case 40:                                /* AFBC_FEATURES: none said (Mesa then draws without AFBC) */
		*value = 0;
		return true;
	default:
		if (which >= 22 && which <= 37) {
			*value = rd(mali, 0x0c0 + (which - 22) * 4);
			return true;
		}
		if (which < sizeof(simple) / sizeof(simple[0]) && simple[which] != 0) {
			*value = rd(mali, simple[which]);
			return true;
		}
		return false;
	}
}

/* `size` bytes of GPU addresses, never given out twice: there are 32 GB of them. 0: none left. */
uint64_t
hakios_mali_address(uint64_t size)
{
	uint64_t address;

	lck_mtx_lock(&mali_lock);
	address = next_address;
	if (address + size > GPU_ADDRESS_END) {
		address = 0;
	} else {
		next_address += (size + 0xffff) & ~0xffffull;   /* 64 KB between buffers: an overrun faults */
		mali_recent[mali_recent_next % MALI_RECENT].address = address;
		mali_recent[mali_recent_next % MALI_RECENT].size = size;
		mali_recent[mali_recent_next % MALI_RECENT].freed = 0;
		mali_recent[mali_recent_next % MALI_RECENT].kind = 0;
		mali_recent_next++;
	}
	lck_mtx_unlock(&mali_lock);
	return address;
}

void
hakios_mali_unmap(uint64_t gpu, uint64_t size)
{
	lck_mtx_lock(&mali_lock);
	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].address == gpu && mali_recent[i].size != 0) {
			mali_recent[i].freed = 1;
		}
	}
	unmap_pages(gpu, size);
	lck_mtx_unlock(&mali_lock);
	lck_mtx_lock(&mali_job_lock);                   /* an entry taken out: the TLB may hold it */
	lck_mtx_lock(&mali_lock);
	as0_update(pt_top_phys, TRANSCFG_AARCH64_4K);
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
}

/*
 * After entries change: the GPU's MMU forgets what it remembered of the tables. The address space is set up again from
 * the top -- an UPDATE drops everything the MMU had cached for it -- rather than a region locked and its tables
 * flushed: with the region flush a fragment job faulted on a page of a buffer whose entry was there, written and
 * pushed out to memory. Jobs are run one at a time and to their end, so the GPU is idle whenever this runs.
 */
void
hakios_mali_mapped(void)
{
	if (mali_fastmap) {
		return;         /* see TRANSCFG_WALK_NC */
	}
	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	as0_update(pt_top_phys, TRANSCFG_AARCH64_4K);
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
}

/* The entries of each level for GPU address `gpu`, as far as there are tables (with the lock held). */
static void
walk_tables(uint64_t gpu, uint64_t walk[3])
{
	uint64_t *table = pt_top;
	static const unsigned shift[3] = { 30, 21, 12 };

	walk[0] = walk[1] = walk[2] = 0;
	for (int level = 0; level < 3 && table != NULL; level++) {
		walk[level] = table[(gpu >> shift[level]) & (level == 0 ? 31 : 511)];
		table = level < 2 ? table_of(walk[level]) : NULL;
	}
}

/*
 * One job chain, from GPU address `chain`, run to its end before this returns: fragment jobs (`fragment`) on slot 0,
 * the rest on slot 1. 0 when it finished; otherwise the slot's status, a fault or 0xff for no answer within a second --
 * and then the GPU is reset, so the next one finds it working.
 */
uint32_t
hakios_mali_submit(uint64_t chain, bool fragment)
{
	uint32_t slot = fragment ? 0 : 1, js = fragment ? JS0 : JS1, raw, status = 0;
	bool finished;

	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	wr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));
	wr(mali, js + JS_HEAD_NEXT_LO, (uint32_t)chain);
	wr(mali, js + JS_HEAD_NEXT_HI, (uint32_t)(chain >> 32));
	wr(mali, js + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(mali, js + JS_AFFINITY_NEXT_HI, 0);
	wr(mali, js + JS_CONFIG_NEXT, JS_CONFIG);
	wr(mali, js + JS_COMMAND_NEXT, 1);
	lck_mtx_unlock(&mali_lock);
	finished = wait_job(JOB_INT_RAWSTAT, (1u << slot) | (1u << (slot + 16)), 1u << slot, 1000);
	lck_mtx_lock(&mali_lock);
	if (!finished) {
		raw = rd(mali, JOB_INT_RAWSTAT);
		status = (raw & (1u << (slot + 16))) ? rd(mali, js + JS_STATUS) : 0xff;
		last_failure.count++;
		last_failure.slot = slot;
		last_failure.raw = raw;
		last_failure.status = rd(mali, js + JS_STATUS);
		last_failure.mmu = rd(mali, MMU_INT_RAWSTAT);
		last_failure.fault = rd(mali, AS0 + AS_FAULTSTATUS);
		last_failure.fault_address = rd(mali, AS0 + 0x20);
		last_failure.chain = chain;
		last_failure.transtab = rd(mali, AS0 + AS_TRANSTAB_LO) | ((uint64_t)rd(mali, AS0 + AS_TRANSTAB_HI) << 32);
		walk_tables(last_failure.fault_address | ((uint64_t)rd(mali, AS0 + 0x24) << 32), last_failure.walk);
		printf("mali: a job on slot %u did not finish: interrupts %#x, status %#x, MMU %#x fault %#x at %#x\n", slot, raw,
		    last_failure.status, last_failure.mmu, last_failure.fault, last_failure.fault_address);
		describe_address(last_failure.fault_address | ((uint64_t)rd(mali, AS0 + 0x24) << 32));
		(void)gpu_reset();                      /* soft reset, then everything back as it was */
		wr(mali, L2_PWRON, rd(mali, L2_PRESENT));
		wr(mali, TILER_PWRON, rd(mali, TILER_PRESENT));
		wr(mali, SHADER_PWRON, shader_mask);
		(void)wait_for(mali, SHADER_READY, shader_mask, shader_mask, 50);
		as0_update(pt_top_phys, TRANSCFG_AARCH64_4K);
		wr(mali, MMU_INT_CLEAR, 0xffffffffu);
	}
	wr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
	return status;
}

/*
 * The jobs in a queue, run by a thread of their own: a submission returns at once with the job's number, and whoever
 * needs a job done waits for that number (hakios_mali_wait). Run one by one in the order given, which is the order
 * panfrost needs -- a pass's vertex and tiler chain before its fragment chain, one pass before the next that reads it.
 * Before this every submission ran its job to the end before returning: a frame of CoreAnimation's with its blur passes
 * had the window server waiting for the GPU at every one of them, the processor and the GPU taking turns (24 October,
 * 11 to 17 ms a frame of the processor's).
 */
#define MALI_QUEUE      64
static struct {
	uint64_t chain, seq, queued;
	bool fragment;
} mali_queue[MALI_QUEUE];
/* boot-arg mali_trace=1: a line for every job, "tl: j SEQ f|v q QUEUED b BEGAN e ENDED" in us of mach_absolute_time */
static uint32_t mali_trace;
static unsigned mali_queue_head, mali_queue_count;
static uint64_t mali_queued_seq, mali_done_seq;
static bool mali_worker_started;
static LCK_MTX_DECLARE(mali_queue_lock, &mali_lock_group);

static void
mali_worker(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint64_t chain, seq;
		bool fragment;

		lck_mtx_lock(&mali_queue_lock);
		while (mali_queue_count == 0) {
			lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_count, THREAD_UNINT);
		}
		chain = mali_queue[mali_queue_head].chain;
		seq = mali_queue[mali_queue_head].seq;
		uint64_t queued = mali_queue[mali_queue_head].queued;
		fragment = mali_queue[mali_queue_head].fragment;
		lck_mtx_unlock(&mali_queue_lock);

		{
			/* how busy the GPU is: every five seconds, its share of the time and by which slot */
			static uint64_t busy[2], since, jobs[2];
			uint64_t began = mach_absolute_time(), now, ns;

			if (hakios_mali_submit(chain, fragment) != 0) {
				gMaliFailed++;
			}
			now = mach_absolute_time();
			if (mali_trace) {
				uint64_t q, b, e;

				absolutetime_to_nanoseconds(queued, &q);
				absolutetime_to_nanoseconds(began, &b);
				absolutetime_to_nanoseconds(now, &e);
				printf("tl: j %llu %c q %llu b %llu e %llu\n", seq, fragment ? 'f' : 'v', q / 1000, b / 1000, e / 1000);
			}
			absolutetime_to_nanoseconds(now - began, &ns);
			busy[fragment] += ns;
			jobs[fragment]++;
			if (since == 0) {
				since = now;
			} else {
				absolutetime_to_nanoseconds(now - since, &ns);
				if (ns >= 5ull * NSEC_PER_SEC) {
					printf("mali: GPU busy %llu%% of %llu s: vertex and tiler %llu jobs %llu ms (%llu us each), "
					    "fragment %llu jobs %llu ms (%llu us each)\n", (busy[0] + busy[1]) * 100 / ns,
					    ns / NSEC_PER_SEC, jobs[0], busy[0] / 1000000, jobs[0] ? busy[0] / 1000 / jobs[0] : 0,
					    jobs[1], busy[1] / 1000000, jobs[1] ? busy[1] / 1000 / jobs[1] : 0);
					busy[0] = busy[1] = jobs[0] = jobs[1] = 0;
					since = now;
				}
			}
		}

		lck_mtx_lock(&mali_queue_lock);
		mali_queue_head = (mali_queue_head + 1) % MALI_QUEUE;
		mali_queue_count--;
		mali_done_seq = seq;
		lck_mtx_unlock(&mali_queue_lock);
		wakeup((caddr_t)&mali_done_seq);
		wakeup((caddr_t)&mali_queue_head);       /* (room in the queue) */
	}
}

/* A job chain into the queue: its number. */
uint64_t
hakios_mali_enqueue(uint64_t chain, bool fragment)
{
	uint64_t seq;

	lck_mtx_lock(&mali_queue_lock);
	if (!mali_worker_started) {
		thread_t thread;

		if (kernel_thread_start(mali_worker, NULL, &thread) == KERN_SUCCESS) {
			thread_set_thread_name(thread, "the Mali's jobs");
			thread_deallocate(thread);
			mali_worker_started = true;
		}
	}
	while (mali_queue_count == MALI_QUEUE) {
		lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_head, THREAD_UNINT);
	}
	seq = ++mali_queued_seq;
	unsigned at = (mali_queue_head + mali_queue_count) % MALI_QUEUE;

	mali_queue[at].chain = chain;
	mali_queue[at].fragment = fragment;
	mali_queue[at].seq = seq;
	mali_queue[at].queued = mach_absolute_time();
	mali_queue_count++;
	lck_mtx_unlock(&mali_queue_lock);
	wakeup((caddr_t)&mali_queue_count);
	return seq;
}

/* Wait until job `seq` is done: true when it is; with timeout 0 only a look. */
bool
hakios_mali_wait(uint64_t seq, uint64_t timeout_ns)
{
	uint64_t deadline;
	bool done;

	if (timeout_ns > 10ull * NSEC_PER_SEC) {
		timeout_ns = 10ull * NSEC_PER_SEC;     /* (an "infinite" one, INT64_MAX, would wrap) */
	}
	nanoseconds_to_absolutetime(timeout_ns, &deadline);
	deadline += mach_absolute_time();
	lck_mtx_lock(&mali_queue_lock);
	while (mali_done_seq < seq && timeout_ns != 0 && mach_absolute_time() < deadline) {
		lck_mtx_sleep_deadline(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_done_seq, THREAD_UNINT, deadline);
	}
	done = mali_done_seq >= seq;
	lck_mtx_unlock(&mali_queue_lock);
	return done;
}

/* ---- the run ---- */

/* The steps, then, if they all went, once a minute what the GPU has done. */
static void
mali_thread(__unused void *parameter, __unused wait_result_t result)
{
	say("mali: the Galaxy S9's Mali-G72: steps 1 to %u of %u (mali=%u)\n", last_step, STEPS, last_step);
	for (step = 1; step <= last_step; step++) {
		say("mali: step %u: %s\n", step, steps[step - 1].name);
		if (!steps[step - 1].make()) {
			say("mali: step %u: FAILED; the steps after it are not made\n", step);
			break;
		}
		reached = step;
		say("mali: step %u: done\n", step);
	}
	if (job_page != 0) {
		/* (after step 9 programs may be at the GPU already: under the locks, as any change of the tables) */
		lck_mtx_lock(&mali_job_lock);
		lck_mtx_lock(&mali_lock);
		if (pt_top != NULL) {
			unmap_pages(FIRST_JOB_GPU, PAGE_SIZE);  /* nothing of the GPU's to point at the page once it is freed */
			as0_update(pt_top_phys, TRANSCFG_AARCH64_4K);
		}
		lck_mtx_unlock(&mali_lock);
		lck_mtx_unlock(&mali_job_lock);
		kmem_free(kernel_map, job_page, PAGE_SIZE);
		job_page = 0;
	}
	if (reached < STEPS) {
		if (powered_here) {
			g3d_off();
		} else if (reached >= 2) {
			say("mali: the G3D domain was on when found: left on\n");
		}
		say("mali: came up to step %u of %u: not there for programs\n", reached, STEPS);
		return;
	}
	say("mali: came up to step %u of %u: the GPU is there for programs (HakiosMali)\n", reached, STEPS);
	for (;;) {
		delay(60 * 1000 * 1000);
		printf("mali: step %u of %u; user space says %u%s; %u clients, %u buffers, %u job chains (%u failed), "
		    "%u pictures shown\n", reached, STEPS, user_said, user_said == 1 ? " -- a program ran a job on the GPU" : "",
		    gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed, gMaliShown);
		lck_mtx_lock(&kept_lock);
		for (uint32_t i = kept_next > KEPT_LINES ? kept_next - KEPT_LINES : 0; i < kept_next; i++) {
			printf("kept: %s\n", kept_lines[i % KEPT_LINES]);
		}
		lck_mtx_unlock(&kept_lock);
		if (gl_said != 0) {
			printf("mali: gl_test drew on %s; %u%% of the picture inside the triangle (26%% by geometry); "
			    "the middle pixel red %u green %u blue %u alpha %u\n", gl_said / 1000 == 1 ? "THE MALI" : "the processor",
			    gl_said % 1000, gl_middle & 0xff, (gl_middle >> 8) & 0xff, (gl_middle >> 16) & 0xff, gl_middle >> 24);
		}
		if (last_failure.count != 0) {
			printf("mali: the last of %u unfinished: slot %u, chain at %#llx, interrupts %#x, slot status %#x, "
			    "MMU interrupts %#x, fault status %#x at address %#x\n", last_failure.count, last_failure.slot,
			    last_failure.chain, last_failure.raw, last_failure.status, last_failure.mmu, last_failure.fault,
			    last_failure.fault_address);
			printf("mali: the GPU's tables at %#llx (ours at %#llx); for the fault's address: %#llx, %#llx, %#llx\n",
			    last_failure.transtab, pt_top_phys, last_failure.walk[0], last_failure.walk[1], last_failure.walk[2]);
			for (uint32_t i = 0; i < gMaliBuffers && i < 32; i++) {
				printf("mali:   buffer %u at %#llx..%#llx (asked %#x, flags %#x)%s\n", i, gMaliMade[i].address,
				    gMaliMade[i].address + gMaliMade[i].size, gMaliMade[i].asked, gMaliMade[i].flags,
				    last_failure.fault_address >= gMaliMade[i].address &&
				    last_failure.fault_address < gMaliMade[i].address + gMaliMade[i].size ? "  <- the fault" : "");
			}
		}
	}
}

/* From hakios_input_init() (drivers/input/hakios.input.c), while the system starts: the steps go on in a thread. */
void
hakios_mali_init(void)
{
	thread_t thread;

	(void)PE_parse_boot_argn("mali_fastmap", &mali_fastmap, sizeof(mali_fastmap));
	(void)PE_parse_boot_argn("mali_trace", &mali_trace, sizeof(mali_trace));
	(void)PE_parse_boot_argn("mali_pll", &mali_pll, sizeof(mali_pll));
	(void)PE_parse_boot_argn("mali_acpm", &mali_acpm, sizeof(mali_acpm));
	if (!PE_parse_boot_argn("mali", &last_step, sizeof(last_step)) || last_step == 0) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != STARLTE_SOC_BASE) {
		say("mali: mali= is for the Galaxy S9 (the loader's platform starlte, registers from %#lx), and this is not it "
		    "(%#lx): nothing touched\n", STARLTE_SOC_BASE, (unsigned long)pe_arm_get_soc_base_phys());
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	if (kernel_thread_start(mali_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
