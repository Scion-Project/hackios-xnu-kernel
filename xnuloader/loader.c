/*
 * XNU loader: a UEFI application that plays the role of iBoot for an
 * open-source arm64 XNU kernel on non-Apple hardware.
 *
 *   - finds the "XNUP" payload in RAM (placed there by the boot shim on the
 *     phone, by `-device loader` under QEMU) and the Mach-O kernel inside it
 *   - picks a block of free RAM to be XNU's [physBase, physBase + memSize)
 *   - copies the Mach-O segments there, builds an Apple-format device tree
 *     and a boot_args structure behind the kernel
 *   - exits boot services, turns the MMU off and jumps to the kernel entry
 *
 * One translation unit: this file is the spine and #includes the others in order (log.c, devicetree.c, platform.h,
 * platforms/<device>.c, pci.c, smp.c, ufs_survey.c, ufs_service.c, wdt_*.c, fastboot.c). A new device is a table in
 * platforms/ and a line in platforms[] below.
 */
#include "efi.h"
#include "identity.h"         /* who Darwin shall think we are: from the iPhone's own device tree (mkidentity.py) */

/* ------------------------------------------------------------------------ */
/* Fixed conventions shared with the boot shim, the packer and the kernel   */

/*
 * Where the boot shim (or QEMU's loader device, or U-Boot's hakios command) puts the payload, and where the firmware's
 * boot manager finds loader.efi in it: fixed in the UEFI image (hakios-uefi, XnuLoader.h) and the shim. A platform with a
 * payload_home moves it from there at the start (payload_move) and works from payload_base after that.
 */
#define PAYLOAD_BASE        0xF0000000ULL
#define PAYLOAD_MAGIC       0x50554E58u /* "XNUP" */
static uint64_t payload_base = PAYLOAD_BASE;   /* where the payload is read from: PAYLOAD_BASE, or its home once moved */

struct payload_header { uint32_t magic; uint32_t size; };
struct payload_entry { char name[16]; uint32_t size; uint32_t reserved; };

#define KERNEL_VIRT_BASE    0xFFFFFFF004000000ULL /* boot_args.virtBase */
#define KERNEL_LINK_OFFSET  0x03000000ULL         /* kernel text sits 48 MB above the base */
#define BASE_ALIGN          0x02000000ULL         /* 32 MB: L2 block size with 16K pages */
#define MAX_MEM_SIZE        0x180000000ULL        /* 6 GB; the kernel's bootstrap page tables cover 8 */
/*
 * A root image too big for the payload area is placed here, at a fixed
 * offset inside the kernel's memory window, once; it then survives any
 * number of kernel runs (nothing writes to it: the root is read-only).
 */
#define RESIDENT_RD_OFFSET  0x08000000ULL
#define RESIDENT_RD_RESERVE 0x30000000ULL         /* RAM that must remain for the kernel itself */
#define PAGE_SIZE_MAX       0x4000ULL

#define LOG_PARTITION       u"logdump"
#define CHECKPOINT_SELFTEST 999
#define PANIC_LOG_SIZE      0x80000u
#define NVRAM_BANK_SIZE     0x2000u
#define PANIC_LOG_BASE      (payload_base + PAYLOAD_MAX_SIZE - 0x100000ULL)
#define PAYLOAD_MAX_SIZE    0x04000000ULL         /* RAM reserved for the payload in the UEFI memory map (and taken at its home) */

/* ------------------------------------------------------------------------ */
/* XNU structures (pexpert/pexpert/arm64/boot.h, mach-o/loader.h)           */

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

#define BOOT_LINE_LENGTH 608

struct boot_args {
	uint16_t revision;
	uint16_t version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

#define MH_MAGIC_64      0xFEEDFACFu
#define LC_SEGMENT_64    0x19
#define LC_UNIXTHREAD    0x5

struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
	uint32_t cmd, cmdsize; char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

/* ------------------------------------------------------------------------ */
/* Globals and small helpers                                                */

static efi_system_table *ST;
static efi_boot_services *BS;
static efi_handle IMAGE;

static char log_buf[32768];
static uint32_t log_len;

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	while (n--) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n > 0; n--, x++, y++) {
		if (*x != *y) {
			return *x < *y ? -1 : 1;
		}
	}
	return 0;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	/*
	 * Word at a time when possible: USB transfer buffers are uncached, where
	 * a byte loop costs one bus access per byte (6 MB/s instead of 30+).
	 */
	if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
		uint64_t *dw = (uint64_t *)d;
		const uint64_t *sw = (const uint64_t *)s;

		for (; n >= 32; n -= 32, dw += 4, sw += 4) {
			uint64_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
			dw[0] = a;
			dw[1] = b;
			dw[2] = c;
			dw[3] = e;
		}
		for (; n >= 8; n -= 8) {
			*dw++ = *sw++;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

static size_t
str_len(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}
	return *a == *b;
}

/*
 * Set once the loader takes commands over USB: its log travels that way, and
 * the screen is left to the kernel (and to "oem screenshot").
 */
static int screen_quiet;
/*
 * And while the kernel runs, whatever started the loader: under HakiOS's U-Boot (the menu of an installed HakiOS) there
 * is no USB command loop, the text console is U-Boot's, drawn into the framebuffer the kernel shows Darwin in, and every
 * line core 0 printed during a run (the storage's, the battery's, the watchdog's) ran over the system's screen.
 * What it was before the run, to go back to.
 */
static int screen_quiet_before_run;

static void
put_char(char c)
{
	char16 w[3] = { 0, 0, 0 };

	if (log_len < sizeof(log_buf) - 1) {
		log_buf[log_len++] = c;
	}
	if (ST->con_out != NULL && !screen_quiet) {
		if (c == '\n') {
			w[0] = '\r', w[1] = '\n';
		} else {
			w[0] = (char16)c;
		}
		ST->con_out->output_string(ST->con_out, w);
	}
}

/* printf subset: %s %x %d (64-bit values), %% */
static void
print(const char *fmt, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put_char(*fmt);
			continue;
		}
		fmt++;
		if (*fmt == 's') {
			const char *s = __builtin_va_arg(ap, const char *);
			while (*s) {
				put_char(*s++);
			}
		} else if (*fmt == 'x' || *fmt == 'd') {
			uint64_t v = __builtin_va_arg(ap, uint64_t);
			unsigned base = (*fmt == 'x') ? 16 : 10;
			char tmp[24];
			int i = 0;
			do {
				tmp[i++] = "0123456789abcdef"[v % base];
				v /= base;
			} while (v);
			while (i) {
				put_char(tmp[--i]);
			}
		} else {
			put_char(*fmt);
		}
	}
	__builtin_va_end(ap);
}

#define READ_SYSREG(name) ({ uint64_t _v; __asm__ volatile ("mrs %0, " #name : "=r" (_v)); _v; })

/*
 * Write back and drop cached copies of a range. Required for memory the
 * kernel will touch with the MMU (and so the data cache) off: its early code
 * builds page tables that way, and a stale cached copy of those pages would
 * be what the table walker sees once caches are turned on again.
 */
static void
flush_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc civac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

static void
clean_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

#include "log.c"
#include "devicetree.c"
#include "platform.h"
#include "platforms/qemu_virt.c"
#include "platforms/redwood.c"
#include "platforms/starlte.c"
#include "platforms/star2lte.c"
#include "platforms/crownlte.c"
#include "platforms/frankel.c"
#include "platforms/qemu_lga.c"

/*
 * The devices the loader knows, told apart by the framebuffer base their UEFI reports. A port to another
 * device adds its description here (devices/<name>/README.md says where each value comes from); a
 * display that matches none is QEMU's ramfb.
 */
static const struct platform *const platforms[] = {
	&platform_redwood,
	&platform_starlte,
	&platform_frankel,
	&platform_qemu_lga,
};

/* ... and those the framebuffer does not tell apart from one of these, named on the payload's command line instead */
static const struct platform *const sibling_platforms[] = {
	&platform_star2lte,
	&platform_crownlte,
};

/* Core N's MPIDR (its affinity fields), and the number of the core with that MPIDR (-1: none of the platform's) */
static uint64_t
core_mpidr(const struct platform *plat, int core)
{
	return plat->core_mpidr != NULL ? plat->core_mpidr[core] : (uint64_t)core << plat->core_shift;
}

static int
core_number(const struct platform *plat, uint64_t mpidr)
{
	for (int core = 0; core < plat->cores; core++) {
		if (core_mpidr(plat, core) == (mpidr & 0xFFFFFF)) {
			return core;
		}
	}
	return -1;
}

static int starts_with(const char *s, const char *prefix);

/*
 * loader_platform=NAME on the payload's command line: the platform of that name in place of `plat`, if it is one of
 * plat's siblings (sibling_platforms: the same SoC, board and memory map, only the name differs), else `plat` as it was.
 * Without the argument nothing changes.
 */
static const struct platform *
platform_sibling(const struct platform *plat, const char *cmdline)
{
	for (const char *c = cmdline; c != NULL && *c; c++) {
		if ((c != cmdline && c[-1] != ' ') || !starts_with(c, "loader_platform=")) {
			continue;
		}
		const char *want = c + 16;
		for (unsigned i = 0; i < sizeof(sibling_platforms) / sizeof(sibling_platforms[0]); i++) {
			const char *n = sibling_platforms[i]->name;
			unsigned k = 0;

			while (n[k] != 0 && want[k] == n[k]) {
				k++;
			}
			if (n[k] == 0 && (want[k] == 0 || want[k] == ' ') && sibling_platforms[i]->sibling_of == plat) {
				return sibling_platforms[i];
			}
		}
		return plat;
	}
	return plat;
}

/* QEMU virt started with gic-version=2 (qemu/run.sh, QEMU_GIC=2): its GICv2's two frames */
static struct platform platform_qemu_virt_gicv2;

#include "pci.c"

/* ------------------------------------------------------------------------ */

/*
 * A Samsung phone restarted from here goes to its recovery (TWRP), where the run's log is read, and not back into BOOT:
 * that is this image again, and its next run would write over the log in RAM. S-Boot reads why it was restarted from
 * two registers of the PMU, which a reset leaves alone, and the phone's own kernel sets them for "reboot recovery" so
 * (Samsung's sec_reboot.c, sec_reboot(); the offsets in include/linux/sec_ext.h): INFORM2 (+0x808) 0x12345678, a
 * restart and not a power-off; INFORM3 (+0x80C) 0x12345674, recovery. recovery=0 on the command line: back into BOOT.
 */
static const struct platform *PLAT;      /* (the device found, below) */
static int reset_to_recovery = 1;

static void
recovery_after_reset(void)
{
	if (PLAT == NULL || PLAT->recovery_pmu == 0 || !reset_to_recovery) {
		return;
	}
	*(volatile uint32_t *)(uintptr_t)(PLAT->recovery_pmu + 0x808) = 0x12345678;
	*(volatile uint32_t *)(uintptr_t)(PLAT->recovery_pmu + 0x80C) = 0x12345674;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * The restart, from anywhere in the loader (the end of a run, a hung kernel, an exception in the loader itself).
 *
 * On the Galaxy S9 the INFORM registers are written again right before it, and the reset is the PMU's software reset,
 * as Samsung's kernel does it (sec_reboot.c sets INFORM2/3, then exynos-reboot.c writes SWRESET): S-Boot's log after
 * such a reset reads "SW_RST (0x20000000)", "INF2: 12345678 / INF3: 1234567x" and goes where INFORM3 says. Not
 * UEFI's ResetSystem (PSCI SYSTEM_RESET, Silicium's ArmPsciResetSystemLib): no run's restart was ever seen to come
 * from it. And not the watchdog: every one of the S9's logs after its reset (WDT_RST2, RST_STAT 0x800000) has
 * "INF2: 0 / INF3: 0", the values written at the start gone, and S-Boot starts BOOT, the run again. Only the keys
 * held (S-Boot's "s5p_check_keypad: recovery mode") sent those to TWRP. Should SWRESET not take, the firmware's
 * ResetSystem is tried after a second, and after that the watchdog is what is left.
 *
 * recovery=0 on the command line: INFORM2/3 not written, a restart comes back into BOOT, as before.
 * SYSIP_DAT0 (+0x810), which exynos_reboot() also sets for "reboot recovery" (0xFF), is not written: whether S-Boot
 * ever clears it is not known, and a value left there could send every later start to recovery.
 */
static void
platform_restart(void)
{
	static int restarting;          /* an exception in the log's write below comes back here: then no more writing */
	int again = restarting++;

	recovery_after_reset();
	if (PLAT != NULL && PLAT->recovery_pmu != 0 && PLAT->pmu_swreset != 0) {
		volatile uint32_t *pmu = (volatile uint32_t *)(uintptr_t)PLAT->recovery_pmu;
		uint64_t t0, hz = timer_hz();

		wdt_pet();
		if (!again) {
			print("restart: INFORM2 %x, INFORM3 %x (%s); the PMU's software reset (SWRESET +%x)\n",
			    (uint64_t)pmu[0x808 / 4], (uint64_t)pmu[0x80C / 4],
			    reset_to_recovery ? "S-Boot starts recovery" : "recovery=0: BOOT again", (uint64_t)PLAT->pmu_swreset);
			flush_log();
		}
		recovery_after_reset();         /* (again: nothing may come between these and the reset) */
		pmu[PLAT->pmu_swreset / 4] = 1;
		__asm__ volatile ("dsb sy\nisb" ::: "memory");
		for (t0 = READ_SYSREG(CNTVCT_EL0); READ_SYSREG(CNTVCT_EL0) - t0 < hz;) {
		}
		if (!again) {
			print("restart: still running a second after SWRESET: the firmware's ResetSystem instead\n");
			flush_log();
		}
		recovery_after_reset();
	}
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

static void
reboot(void)
{
	hc_take_back("before the reset");
	flush_log();
	platform_restart();
}

/*
 * Return-from-kernel support.
 *
 * Boot services are never exited: UEFI stays resident outside XNU's memory
 * window. loader_save_ctx() records the UEFI execution environment and
 * returns 0. When the kernel is done it jumps to loader_return_stub (mapped
 * 1:1 or with the MMU off) with x0 = PA of its log, x1 = length, x2 = reason
 * (0 = orderly, 1 = early exception, then x3..x7 = ESR, ELR, FAR, LR, SP).
 * The stub switches the MMU off, reinstates the saved environment and makes
 * loader_save_ctx() return a second time, with 1.
 *
 * The EL1 physical timer (CNTP_*) is deliberately left alone: the Qualcomm
 * hypervisor traps it. The CTX_CNTP_* slots are unused.
 */
enum {
	CTX_TTBR0, CTX_TCR, CTX_MAIR, CTX_SCTLR, CTX_VBAR, CTX_CPACR, CTX_SP, CTX_SPSEL, CTX_DAIF,
	CTX_CNTV_CTL, CTX_CNTV_CVAL, CTX_CNTP_CTL, CTX_CNTP_CVAL, CTX_CNTKCTL,
	CTX_X18, CTX_X19, /* ... x30 */
	CTX_LOG_PA = CTX_X19 + 12, CTX_LOG_LEN, CTX_REASON, CTX_ESR, CTX_ELR, CTX_FAR, CTX_LR, CTX_KSP,
	CTX_TPIDR_EL1, CTX_TPIDR_EL0, CTX_TPIDRRO_EL0, CTX_SP_EL0, CTX_CONTEXTIDR, CTX_MDSCR, CTX_FPCR, CTX_FPSR,
	CTX_ICC_PMR, CTX_ICC_IGRPEN1, CTX_ICC_BPR1, CTX_COUNT
};
uint64_t loader_ctx[CTX_COUNT];
/* 0: the GIC is a GICv2, whose CPU interface is memory, not ICC_* registers (those are not there to read) */
uint64_t loader_gic_sysregs = 1;
int loader_save_ctx(void);
void loader_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl loader_save_ctx\n"
        "loader_save_ctx:\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  mrs x9, ttbr0_el1\n   str x9, [x10, #0]\n"
        "  mrs x9, tcr_el1\n     str x9, [x10, #8]\n"
        "  mrs x9, mair_el1\n    str x9, [x10, #16]\n"
        "  mrs x9, sctlr_el1\n   str x9, [x10, #24]\n"
        "  mrs x9, vbar_el1\n    str x9, [x10, #32]\n"
        "  mrs x9, cpacr_el1\n   str x9, [x10, #40]\n"
        "  mov x9, sp\n          str x9, [x10, #48]\n"
        "  mrs x9, spsel\n       str x9, [x10, #56]\n"
        "  mrs x9, daif\n        str x9, [x10, #64]\n"
        "  mrs x9, cntv_ctl_el0\n  str x9, [x10, #72]\n"
        "  mrs x9, cntv_cval_el0\n str x9, [x10, #80]\n"
        "  mrs x9, cntkctl_el1\n   str x9, [x10, #104]\n"
        /* Qualcomm's UEFI has its own thread scheduler and keeps its current-thread
         * pointer in TPIDR_EL1; the kernel overwrites it, so it must come back intact. */
        "  mrs x9, tpidr_el1\n     str x9, [x10, #280]\n"
        "  mrs x9, tpidr_el0\n     str x9, [x10, #288]\n"
        "  mrs x9, tpidrro_el0\n   str x9, [x10, #296]\n"
        "  mrs x9, contextidr_el1\n str x9, [x10, #312]\n"
        "  mrs x9, mdscr_el1\n     str x9, [x10, #320]\n"
        "  mrs x9, s3_3_c4_c4_0\n          str x9, [x10, #328]\n"
        "  mrs x9, s3_3_c4_c4_1\n          str x9, [x10, #336]\n"
        /* GICv3 CPU interface: the kernel reprograms it for its own interrupts */
        "  adrp x11, loader_gic_sysregs\n"
        "  ldr  x11, [x11, :lo12:loader_gic_sysregs]\n"
        "  cbz  x11, 3f\n"
        "  mrs x9, icc_pmr_el1\n     str x9, [x10, #344]\n"
        "  mrs x9, icc_igrpen1_el1\n str x9, [x10, #352]\n"
        "  mrs x9, icc_bpr1_el1\n    str x9, [x10, #360]\n"
        "3:\n"
        "  mrs x9, spsel\n"
        "  cbz x9, 1f\n"                  /* SP_EL0 is only accessible while running on SP_EL1 */
        "  mrs x9, sp_el0\n        str x9, [x10, #304]\n"
        "1:\n"
        "  str x18, [x10, #112]\n"
        "  stp x19, x20, [x10, #120]\n"
        "  stp x21, x22, [x10, #136]\n"
        "  stp x23, x24, [x10, #152]\n"
        "  stp x25, x26, [x10, #168]\n"
        "  stp x27, x28, [x10, #184]\n"
        "  stp x29, x30, [x10, #200]\n"
        "  mov x0, #0\n"
        "  ret\n"
        ".balign 64\n"
        ".globl loader_return_stub\n"
        "loader_return_stub:\n"
        "  msr daifset, #0xf\n"
        /* PSTATE bits the kernel may have left set. PAN in particular is fatal:
         * UEFI maps its memory as EL0-accessible, so with PAN every data access
         * (including the exception handler's own stack push) faults. */
        "  .inst 0xd500409f\n"          /* msr PAN, #0 */
        "  .inst 0xd500407f\n"          /* msr UAO, #0 */
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #0]\n   msr ttbr0_el1, x9\n"
        "  ldr x9, [x10, #8]\n   msr tcr_el1, x9\n"
        "  ldr x9, [x10, #16]\n  msr mair_el1, x9\n"
        "  ldr x9, [x10, #32]\n  msr vbar_el1, x9\n"
        "  ldr x9, [x10, #40]\n  msr cpacr_el1, x9\n"
        "  isb\n"
        "  tlbi vmalle1\n"
        "  dsb sy\n"
        "  ic iallu\n"
        "  isb\n"
        "  ldr x9, [x10, #24]\n  msr sctlr_el1, x9\n"
        "  isb\n"
        "  ldr x9, [x10, #56]\n"
        "  cbz x9, 1f\n"
        "  msr spsel, #1\n"
        "  ldr x9, [x10, #304]\n msr sp_el0, x9\n"
        "  b 2f\n"
        "1:\n"
        "  msr spsel, #0\n"
        "2:\n"
        "  ldr x9, [x10, #48]\n  mov sp, x9\n"
        "  ldr x9, [x10, #280]\n msr tpidr_el1, x9\n"
        "  ldr x9, [x10, #288]\n msr tpidr_el0, x9\n"
        "  ldr x9, [x10, #296]\n msr tpidrro_el0, x9\n"
        "  ldr x9, [x10, #312]\n msr contextidr_el1, x9\n"
        "  ldr x9, [x10, #320]\n msr mdscr_el1, x9\n"
        "  adrp x11, loader_gic_sysregs\n"   /* (x0-x7 are what the kernel returned: x11 is free) */
        "  ldr  x11, [x11, :lo12:loader_gic_sysregs]\n"
        "  cbz  x11, 3f\n"
        "  ldr x9, [x10, #344]\n msr icc_pmr_el1, x9\n"
        "  ldr x9, [x10, #360]\n msr icc_bpr1_el1, x9\n"
        "  ldr x9, [x10, #352]\n msr icc_igrpen1_el1, x9\n"
        "3:\n"
        "  ldr x9, [x10, #328]\n msr s3_3_c4_c4_0, x9\n"
        "  ldr x9, [x10, #336]\n msr s3_3_c4_c4_1, x9\n"
        "  ldr x9, [x10, #104]\n msr cntkctl_el1, x9\n"
        "  ldr x9, [x10, #80]\n  msr cntv_cval_el0, x9\n"
        "  ldr x9, [x10, #72]\n  msr cntv_ctl_el0, x9\n"
        "  ldr x18, [x10, #112]\n"
        "  ldp x19, x20, [x10, #120]\n"
        "  ldp x21, x22, [x10, #136]\n"
        "  ldp x23, x24, [x10, #152]\n"
        "  ldp x25, x26, [x10, #168]\n"
        "  ldp x27, x28, [x10, #184]\n"
        "  ldp x29, x30, [x10, #200]\n"
        "  stp x0, x1, [x10, #216]\n"   /* kernel log PA and length, stored with caches on */
        "  stp x2, x3, [x10, #232]\n"   /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"   /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"   /* LR, SP */
        "  ldr x9, [x10, #64]\n  msr daif, x9\n"
        "  mov x0, #1\n"
        "  ret\n"
        );

__attribute__((noreturn)) static void
jump_to_kernel(uint64_t entry, uint64_t boot_args_pa)
{
	__asm__ volatile (
                "msr daifset, #0xf\n"
                /* Last thing before the MMU goes off: write this stack back to RAM
                 * (dirty cache lines do not survive, and we come back to this frame). */
                "mov x2, sp\n"
                "sub x2, x2, #0x400\n"
                "bic x2, x2, #63\n"
                "add x3, x2, #0x4000\n"
                "1: dc cvac, x2\n"
                "add x2, x2, #64\n"
                "cmp x2, x3\n"
                "b.lo 1b\n"
                "dsb sy\n"
                "mrs x2, sctlr_el1\n"
                "bic x2, x2, #0x1\n"    /* M: MMU off */
                "bic x2, x2, #0x4\n"    /* C: data cache off */
                "bic x2, x2, #0x1000\n" /* I: instruction cache off */
                "msr sctlr_el1, x2\n"
                "isb\n"
                "ic iallu\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, %1\n"
                "br %0\n"
                : : "r" (entry), "r" (boot_args_pa) : "x0", "x2", "x3");
	__builtin_unreachable();
}

#include "smp.c"
#include "ufs_survey.c"
#include "ufs_service.c"
#include "wdt_exynos.c"
#include "wdt_haven.c"

/* ------------------------------------------------------------------------ */
/* UEFI exception hook: put the fault into the log instead of on the screen  */

extern char __ImageBase;

static void EFIAPI
exception_handler(int64_t type, uint64_t *ctx)
{
	print("\nUEFI EXCEPTION type=%d elr=%x esr=%x far=%x lr=%x sp=%x spsr=%x\n", (uint64_t)type, ctx[EXC_CTX_ELR],
	    ctx[EXC_CTX_ESR], ctx[EXC_CTX_FAR], ctx[EXC_CTX_LR], ctx[EXC_CTX_SP], ctx[EXC_CTX_SPSR]);
	print("loader image base %x (elr offset %x)\n", (uint64_t)(uintptr_t)&__ImageBase,
	    ctx[EXC_CTX_ELR] - (uint64_t)(uintptr_t)&__ImageBase);
	flush_log();
	platform_restart();
}

static void
install_exception_hook(void)
{
	static const efi_guid cpu_guid = EFI_CPU_ARCH_GUID;
	efi_cpu_arch *cpu;

	if (EFI_ERROR(BS->locate_protocol(&cpu_guid, NULL, (void **)&cpu))) {
		return;
	}
	for (int64_t type = 0; type <= 3; type += 3) { /* synchronous, SError */
		cpu->register_interrupt_handler(cpu, type, NULL);
		efi_status s = cpu->register_interrupt_handler(cpu, type, exception_handler);
		if (EFI_ERROR(s)) {
			print("exception hook %d: status %x\n", (uint64_t)type, s);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Payload                                                                  */

struct payload_info {
	const uint8_t *kernel;
	uint32_t kernel_size;           /* the kernel entry's size: what its Mach-O header may point into */
	const uint8_t *ramdisk;
	uint32_t ramdisk_size;
	const char *cmdline;
	uint64_t checkpoint;
	uint64_t forced_base;
	uint64_t resident_rd;           /* size of a root image the host put at the resident address */
	const uint8_t *resident;        /* ... or one the payload carries, which the loader puts there itself */
	uint32_t resident_size;
	const char *kernel_version;     /* kernel.ver: the version of the kernel in this payload (tools/kernel-version.py) */
	int usb_mode;
};

static uint64_t
parse_number(const char *c, unsigned base)
{
	uint64_t v = 0;
	for (; *c; c++) {
		uint64_t d = (*c >= '0' && *c <= '9') ? (uint64_t)(*c - '0') : (*c >= 'a' && *c <= 'f') ? (uint64_t)(*c - 'a' + 10) : 16;
		if (d >= base) {
			break;
		}
		v = v * base + d;
	}
	return v;
}

/* How many bytes after the header the walk over a payload's entries may cover: the header's size, but never more than fits */
static uint64_t
payload_walk_size(uint32_t header_size)
{
	uint64_t most = PAYLOAD_MAX_SIZE - 0x100000ULL - sizeof(struct payload_header);

	return header_size < most ? header_size : most;
}

static int
parse_payload(struct payload_info *pi, int verbose)
{
	struct payload_header *ph = (struct payload_header *)payload_base;

	memset(pi, 0, sizeof(*pi));
	pi->cmdline = "";
	if (ph->magic != PAYLOAD_MAGIC) {
		print("no payload at %x (magic %x)\n", payload_base, (uint64_t)ph->magic);
		return 0;
	}
	/*
	 * The size in the header is not trusted past what the payload region can hold (the file's size is not
	 * known here), so a wild value cannot send the walk through memory that is not the payload.
	 */
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + payload_walk_size(ph->size);
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		const char *text = (const char *)(p + sizeof(*e));
		if (e->name[0] == 0) {
			break;
		}
		if (verbose) {
			print("payload entry '%s' size %x\n", e->name, (uint64_t)e->size);
		}
		/*
		 * An entry that claims more bytes than are left is a torn or made-up payload: the ramdisk copy would read
		 * past it, and the 32-bit rounding below would wrap. Not booted, as with a payload of the wrong magic.
		 */
		if ((uint64_t)e->size > (uint64_t)(pend - (p + sizeof(*e)))) {
			print("payload entry is larger than the payload (size %x): not used\n", (uint64_t)e->size);
			return 0;
		}
		if (str_eq(e->name, "ramdisk")) {
			pi->ramdisk = p + sizeof(*e);             /* becomes /dev/md0, the root device */
			pi->ramdisk_size = e->size;
		} else if (str_eq(e->name, "kernel")) {
			pi->kernel = p + sizeof(*e);
			pi->kernel_size = e->size;
		} else if (str_eq(e->name, "cmdline")) {
			pi->cmdline = text;
		} else if (str_eq(e->name, "physbase")) {
			pi->forced_base = parse_number(text, 16); /* testing aid; must be free RAM */
		} else if (str_eq(e->name, "resident")) {
			pi->resident = p + sizeof(*e);            /* the image xl ramdisk would send: a boot with no host */
			pi->resident_size = e->size;
		} else if (str_eq(e->name, "rdres")) {
			pi->resident_rd = parse_number(text, 16); /* e.g. loaded there by QEMU; see RESIDENT_RD_OFFSET */
		} else if (str_eq(e->name, "kernel.ver")) {
			pi->kernel_version = text;                /* printed under the banner; the kernel gets it as hakios_kver */
		} else if (str_eq(e->name, "checkpoint")) {
			pi->checkpoint = parse_number(text, 10);  /* where the kernel should stop and come back */
		} else if (str_eq(e->name, "usb")) {
			pi->usb_mode = 1;                         /* stay in the loader and take commands over USB */
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return 1;
}

/* ------------------------------------------------------------------------ */
/* NVRAM image                                                              */
/*                                                                          */
/* iBoot hands the kernel a copy of the NVRAM contents in CHRP format       */
/* (iokit/Kernel/IONVRAMCHRPHandler.cpp). There is no NVRAM here, so build  */
/* an image with a valid header and one empty "common" partition.           */

static void
nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, str_len(name));
	/* checksum over signature, length and name, folded to 8 bits */
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void
build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");                           /* Apple header: 32 bytes */
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common"); /* the rest: common partition */
	memcpy(image + 20, &generation, 4);
	/* Adler-32 over everything after the checksum field itself */
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* ------------------------------------------------------------------------ */
/* Running the kernel                                                       */

static efi_gop *GOP;
/* The display handed to the kernel: the firmware's, or one we set up ourselves (QEMU). base 0 = none. */
static struct {
	uint64_t base;
	uint32_t width, height, stride;
} SCREEN;
static uint64_t win_base, win_size; /* XNU's physical memory window, claimed once */
static uint64_t resident_rd_size;   /* root image kept at win_base + RESIDENT_RD_OFFSET, 0 if none */
static char last_result[64] = "idle";

static void
set_result(const char *what, uint64_t a, uint64_t b)
{
	uint32_t saved = log_len;
	uint32_t start = log_len;
	print("%s %d %x", what, a, b);
	uint32_t n = log_len - start;
	if (n > sizeof(last_result) - 1) {
		n = sizeof(last_result) - 1;
	}
	memcpy(last_result, log_buf + start, n);
	last_result[n] = 0;
	log_len = saved;
}

/* Every range of the firmware's memory map of 1 MB or more, and the total of each type. */
static void
print_memory_map(void)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size, totals[16];
	uint32_t desc_ver;

	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return;
	}
	memset(totals, 0, sizeof(totals));
	print("memory map (type 7 = free, 1/2 = loader, 3/4 = boot services, 5/6 = runtime, 0 = reserved):\n");
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);

		if (d->type < 16) {
			totals[d->type] += d->pages << 12;
		}
		if (d->pages >= 256) {
			print("  %x - %x  type %d  %d MB\n", d->phys_start, d->phys_start + (d->pages << 12), (uint64_t)d->type,
			    d->pages >> 8);
		}
	}
	for (int t = 0; t < 16; t++) {
		if (totals[t] != 0) {
			print("  total of type %d: %d MB\n", (uint64_t)t, totals[t] >> 20);
		}
	}
}

static int extra_ram_wanted(const char *cmdline);
static void extra_ram_claim(void);
static int extra_ram_keep_mfc;
static int mfc_wanted(const char *cmdline);

static int
choose_window(const struct payload_info *pi)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size;
	uint32_t desc_ver;
	uint64_t best_base = 0, best_size = 0;
	/* what XNU's window must stay clear of: the payload where it landed and where it is now, and the platform's keep-outs */
	uint64_t avoid[4][2] = {
		{ PAYLOAD_BASE, PAYLOAD_BASE + PAYLOAD_MAX_SIZE }, { payload_base, payload_base + PAYLOAD_MAX_SIZE },
		{ PLAT->keep_out[0].base, PLAT->keep_out[0].base + PLAT->keep_out[0].size },
		{ PLAT->keep_out[1].base, PLAT->keep_out[1].base + PLAT->keep_out[1].size },
	};

	if (win_size != 0) {
		return 1;
	}
	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return 0;
	}
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
		if (d->type != EfiConventionalMemory) {
			continue;
		}
		uint64_t lo = d->phys_start, hi = d->phys_start + (d->pages << 12);
		/* the payload and the keep-outs may sit in RAM that UEFI considers free: stay clear of them (the larger side) */
		for (int a = 0; a < 4; a++) {
			uint64_t pay_lo = avoid[a][0], pay_hi = avoid[a][1];

			if (pay_hi > pay_lo && lo < pay_hi && hi > pay_lo) {
				if (pay_lo - lo > hi - pay_hi && pay_lo > lo) {
					hi = pay_lo;
				} else {
					lo = pay_hi;
				}
			}
		}
		if (hi <= lo) {
			continue;
		}
		lo = (lo + BASE_ALIGN - 1) & ~(BASE_ALIGN - 1);
		hi &= ~(BASE_ALIGN - 1);
		if (hi > lo && hi - lo > best_size) {
			best_base = lo;
			best_size = hi - lo;
		}
	}
	print("largest free block %x size %x\n", best_base, best_size);
	if (pi->forced_base != 0) {
		best_base = pi->forced_base;
		best_size = MAX_MEM_SIZE;
	}
	if (best_size > MAX_MEM_SIZE) {
		best_size = MAX_MEM_SIZE;
	}
	for (int k = 0; k < 2; k++) {
		uint64_t ko = PLAT->keep_out[k].base, ke = ko + PLAT->keep_out[k].size;

		if (ke > ko && best_base < ke && best_base + best_size > ko) {
			print("phys window %x size %x would cover %x..%x (%s): refused\n", best_base, best_size, ko, ke - 1,
			    PLAT->keep_out[k].what);
			return 0;
		}
	}
	/* UEFI stays alive while the kernel runs, so the kernel must own all of its window. */
	uint64_t claim = best_base;
	efi_status s = BS->allocate_pages(AllocateAddress, EfiLoaderData, best_size >> 12, &claim);
	print("phys window %x size %x, claim status %x\n", best_base, best_size, s);
	if (EFI_ERROR(s)) {
		return 0;
	}
	win_base = best_base;
	win_size = best_size;
	if (extra_ram_wanted(pi->cmdline)) {
		extra_ram_keep_mfc = PLAT->extra_ram == exynos9810_extra_ram && mfc_wanted(pi->cmdline);
		extra_ram_claim();
	}
	return 1;
}

/*
 * Free RAM beside the window that is the kernel's too (struct platform extra_ram): taken from UEFI once, with the window,
 * and given to the kernel in the device tree, /chosen "hakios-extra-ram" (pairs of base and size; the kernel's patch 1512,
 * arm/pmap/pmap.h). The kernel's pages are 16 KB: each piece is cut to that, and a piece under EXTRA_RAM_MIN is left
 * to UEFI. The kernel takes at most HAKIOS_EXTRA_RAM_MAX (12) ranges: the largest are kept.
 */
#define EXTRA_RAM_MAX 12
#define EXTRA_RAM_MIN 0x200000ULL
static struct { uint64_t base, size; } extra_ram[EXTRA_RAM_MAX];
static int extra_ram_count;
static uint64_t extra_ram_total;

/* extra_ram=0 / extra_ram=1 among the kernel's arguments, else the platform's default */
static int
extra_ram_wanted(const char *cmdline)
{
	int want = PLAT->extra_ram != NULL && !PLAT->extra_ram_off;

	for (const char *c = cmdline; c != NULL && *c; c++) {
		if ((c == cmdline || c[-1] == ' ') && starts_with(c, "extra_ram=") && (c[10] == '0' || c[10] == '1') &&
		    (c[11] == 0 || c[11] == ' ')) {
			want = c[10] == '1' && PLAT->extra_ram != NULL;
		}
	}
	return want;
}

/*
 * mfc= (not 0) among the kernel's arguments: the top of the 0xC0080000-0xCBFFFFFF piece stays out of XNU's RAM on the Exynos 9810, for the
 * video decoder's 32-bit DMA (sys/hakios.exynos9810_mem.h: EXYNOS9810_MEM_MFC_PA, _END; hakios.exynos_mfc.c)
 */
#define EXYNOS9810_MFC_PA 0xC4000000ULL
#define EXYNOS9810_MFC_END 0xCC000000ULL

static int
mfc_wanted(const char *cmdline)
{
	for (const char *c = cmdline; c != NULL && *c; c++) {
		if ((c == cmdline || c[-1] == ' ') && starts_with(c, "mfc=") && c[4] != 0 && c[4] != ' ' && !(c[4] == '0' && (c[5] == 0 || c[5] == ' '))) {
			return 1;
		}
	}
	return 0;
}

static void
extra_ram_claim(void)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size, cand[64][2], bad = 0;
	uint32_t desc_ver;
	int n = 0;

	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("extra RAM: GetMemoryMap failed, none\n");
		return;
	}
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);

		if (d->type != EfiConventionalMemory) {
			continue;
		}
		for (int w = 0; PLAT->extra_ram[w][1] != 0; w++) {
			uint64_t lo = d->phys_start, hi = d->phys_start + (d->pages << 12);

			lo = lo < PLAT->extra_ram[w][0] ? PLAT->extra_ram[w][0] : lo;
			hi = hi > PLAT->extra_ram[w][1] ? PLAT->extra_ram[w][1] : hi;
			if (extra_ram_keep_mfc && lo < EXYNOS9810_MFC_END && hi > EXYNOS9810_MFC_PA) {
				hi = lo < EXYNOS9810_MFC_PA ? EXYNOS9810_MFC_PA : lo;
			}
			lo = (lo + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
			hi &= ~(PAGE_SIZE_MAX - 1);
			if (hi > lo && hi - lo >= EXTRA_RAM_MIN && n < 64) {
				cand[n][0] = lo;
				cand[n][1] = hi - lo;
				n++;
			}
		}
	}
	/* the largest first (the kernel takes EXTRA_RAM_MAX), then each taken from UEFI and tested */
	for (int i = 1; i < n; i++) {
		for (int j = i; j > 0 && cand[j][1] > cand[j - 1][1]; j--) {
			uint64_t t0 = cand[j][0], t1 = cand[j][1];

			cand[j][0] = cand[j - 1][0];
			cand[j][1] = cand[j - 1][1];
			cand[j - 1][0] = t0;
			cand[j - 1][1] = t1;
		}
	}
	for (int i = 0; i < n && extra_ram_count < EXTRA_RAM_MAX; i++) {
		uint64_t at = cand[i][0], lo = cand[i][0], hi = cand[i][0] + cand[i][1], wrong = 0;

		/*
		 * Taken as reserved memory, as the keep-outs are (keep_out_reserve): UEFI merges neighbours of one type, and a
		 * piece of loader data next to the loader's own would hide that from the cache cleaning before the hand-over.
		 */
		if (EFI_ERROR(BS->allocate_pages(AllocateAddress, 0 /* EfiReservedMemoryType */, cand[i][1] >> 12, &at))) {
			print("extra RAM: %x..%x free but not taken\n", lo, hi - 1);
			continue;
		}
		/*
		 * Is it RAM, and all there? A word at each end of each 16 KB page, written, put out of the cache, read back from
		 * the RAM itself. (The kernel's pages are given out unwritten: what is left in them does not matter.)
		 */
		for (uint64_t p = lo; p < hi; p += PAGE_SIZE_MAX) {
			*(volatile uint64_t *)p = p ^ 0x5A5AC3C3A5A53C3CULL;
			*(volatile uint64_t *)(p + PAGE_SIZE_MAX - 8) = ~p;
			flush_dcache(p, p + 8);
			flush_dcache(p + PAGE_SIZE_MAX - 8, p + PAGE_SIZE_MAX);
		}
		for (uint64_t p = lo; p < hi; p += PAGE_SIZE_MAX) {
			wrong += *(volatile uint64_t *)p != (p ^ 0x5A5AC3C3A5A53C3CULL) ||
			    *(volatile uint64_t *)(p + PAGE_SIZE_MAX - 8) != ~p;
		}
		if (wrong != 0) {
			print("extra RAM: %x..%x: %d pages of %d read back wrong: NOT given to the kernel (kept from UEFI)\n", lo,
			    hi - 1, wrong, (hi - lo) / PAGE_SIZE_MAX);
			bad += hi - lo;
			continue;
		}
		extra_ram[extra_ram_count].base = lo;
		extra_ram[extra_ram_count].size = hi - lo;
		extra_ram_count++;
		extra_ram_total += hi - lo;
	}
	/* in address order, for the log and the kernel */
	for (int i = 1; i < extra_ram_count; i++) {
		for (int j = i; j > 0 && extra_ram[j].base < extra_ram[j - 1].base; j--) {
			uint64_t t0 = extra_ram[j].base, t1 = extra_ram[j].size;

			extra_ram[j].base = extra_ram[j - 1].base;
			extra_ram[j].size = extra_ram[j - 1].size;
			extra_ram[j - 1].base = t0;
			extra_ram[j - 1].size = t1;
		}
	}
	print("extra RAM: %d MB in %d ranges taken from UEFI beside the window (%d pieces free, %d MB failed the test)\n",
	    extra_ram_total >> 20, (uint64_t)extra_ram_count, (uint64_t)n, bad >> 20);
}

/*
 * Load and run the kernel from the payload. Returns when the kernel comes
 * back (panic, checkpoint, early exception); the combined log is staged and
 * written to the log partition. Returns 0 if the kernel could not be started.
 */
static int starts_with(const char *s, const char *prefix);

static int
run_kernel(int have_checkpoint, uint64_t checkpoint)
{
	static uint8_t map_buf[65536];
	uint64_t map_size, map_key, desc_size;
	uint32_t desc_ver;
	struct payload_info pi;

	last_klog = NULL;               /* (the last run's kernel log is in memory this run reuses) */
	last_klog_len = 0;
	if (!parse_payload(&pi, 1) || pi.kernel == NULL) {
		print("no kernel in payload\n");
		return 0;
	}
	if (!have_checkpoint) {
		checkpoint = pi.checkpoint;
	}
	const uint8_t *kernel = pi.kernel;
	const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
	if (pi.kernel_size < sizeof(*mh) || mh->magic != MH_MAGIC_64) {
		print("kernel is not a 64-bit Mach-O\n");
		return 0;
	}
	/* the load commands must lie inside the kernel entry, or the walk below reads whatever follows it */
	if (mh->sizeofcmds > pi.kernel_size - sizeof(*mh)) {
		print("kernel: the load commands are longer than the kernel\n");
		return 0;
	}

	/* --- kernel extent -------------------------------------------------- */
	uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		/*
		 * Every command must fit in the commands the header counts, and be at least its own header long: a
		 * cmdsize of 0 would keep the walk on one command for up to 2^32 rounds, a huge one would leave the kernel.
		 */
		uint64_t at = (uint64_t)((const uint8_t *)lc - (const uint8_t *)(mh + 1));

		if (at > mh->sizeofcmds || sizeof(*lc) > mh->sizeofcmds - at || lc->cmdsize < sizeof(*lc) ||
		    lc->cmdsize > mh->sizeofcmds - at) {
			print("kernel: load command %d does not fit in the load commands\n", (uint64_t)i);
			return 0;
		}
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (lc->cmdsize < sizeof(*sg)) {
				print("kernel: segment command %d is too short\n", (uint64_t)i);
				return 0;
			}
			if (sg->vmsize == 0) {
				continue;
			}
			/*
			 * What is loaded must be inside the kernel (file) and inside its own extent: a vmaddr + vmsize that
			 * wraps, a filesize above vmsize, or a file range past the entry would write over, or read from, memory
			 * that is not the kernel's.
			 */
			if (sg->vmaddr + sg->vmsize < sg->vmaddr || sg->filesize > sg->vmsize || sg->fileoff > pi.kernel_size ||
			    sg->filesize > pi.kernel_size - sg->fileoff) {
				print("kernel: segment %d is outside the kernel or wraps\n", (uint64_t)i);
				return 0;
			}
			if (sg->vmaddr < vm_lo) {
				vm_lo = sg->vmaddr;
			}
			if (sg->vmaddr + sg->vmsize > vm_hi) {
				vm_hi = sg->vmaddr + sg->vmsize;
			}
		} else if (lc->cmd == LC_UNIXTHREAD && lc->cmdsize >= 16 + 33 * 8) {
			/* cmd, cmdsize, flavor, count, x0..x28, fp, lr, sp, pc (a command too short to hold pc gives no entry) */
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}
	print("kernel va %x..%x entry %x\n", vm_lo, vm_hi, entry_va);
	/* (a kernel that reaches more than the largest window past the virtual base is no kernel of this loader's) */
	if (vm_lo < KERNEL_VIRT_BASE + KERNEL_LINK_OFFSET || entry_va == 0 || vm_hi < vm_lo ||
	    vm_hi - KERNEL_VIRT_BASE > MAX_MEM_SIZE) {
		print("unexpected kernel layout\n");
		return 0;
	}
	if (!choose_window(&pi)) {
		return 0;
	}

	uint64_t phys_base = win_base;
	uint64_t slide = phys_base - KERNEL_VIRT_BASE; /* pa = va + slide */
	uint64_t kern_lo = vm_lo + slide, kern_hi = (vm_hi + slide + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	/* The ramdisk must sit inside the kernel's static mapping, i.e. below topOfKernelData. */
	uint64_t rd_pa = kern_hi;
	uint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	int rd_resident = 0;
	/* A resident image next to a payload ramdisk: two disks (which is which is decided where the device tree is written). */
	uint64_t rd1_pa = 0, rd1_size = 0;

	if (pi.resident_rd != 0) {
		/*
		 * The size is the payload's word: one that does not even fit above the resident address would wrap
		 * the page rounding or the device tree address below, and the room check after them would pass.
		 * (Whatever fits is still held to the room check as before.)
		 */
		if (win_size <= RESIDENT_RD_OFFSET || pi.resident_rd > win_size - RESIDENT_RD_OFFSET) {
			print("rdres %x does not fit in the memory window\n", pi.resident_rd);
			return 0;
		}
		resident_rd_size = pi.resident_rd;
	}
	/*
	 * The payload's own resident image (a boot from the phone's boot partition, where no host sends one): put where
	 * "oem ramdisk" puts it, once per run, and taken as if it had been sent.
	 */
	if (pi.resident != NULL) {
		if (win_size <= RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE ||
		    pi.resident_size > win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE) {
			print("resident %x does not fit in the memory window\n", (uint64_t)pi.resident_size);
			return 0;
		}
		memcpy((void *)(phys_base + RESIDENT_RD_OFFSET), pi.resident, pi.resident_size);
		resident_rd_size = pi.resident_size;
		print("resident image from the payload: %x bytes\n", (uint64_t)pi.resident_size);
	}
	if (resident_rd_size != 0) {
		uint64_t res_pa = phys_base + RESIDENT_RD_OFFSET;
		uint64_t res_size = (resident_rd_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);

		if (kern_hi + rd_size > res_pa) {
			print("kernel and ramdisk overlap the resident image\n");
			return 0;
		}
		if (rd_size == 0) {
			rd_pa = res_pa;
			rd_size = res_size;
			rd_resident = 1;
		} else {
			rd1_pa = res_pa;
			rd1_size = res_size;
		}
	}
	/* everything up to topOfKernelData is the kernel's static region: it must cover both images */
	uint64_t dt_pa = rd1_size != 0 ? rd1_pa + rd1_size : rd_pa + rd_size;
	uint64_t dt_max = 0x10000;
	uint64_t ba_pa = dt_pa + dt_max;
	uint64_t top = ba_pa + PAGE_SIZE_MAX;

	if (win_size < top - phys_base + 0x4000000ULL) {
		print("not enough contiguous RAM\n");
		return 0;
	}

	/* --- load segments -------------------------------------------------- */
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		const struct segment_command_64 *sg = (const void *)lc;
		if (lc->cmd != LC_SEGMENT_64 || sg->vmsize == 0) {
			continue;
		}
		memset((void *)(sg->vmaddr + slide), 0, sg->vmsize);
		memcpy((void *)(sg->vmaddr + slide), kernel + sg->fileoff, sg->filesize);
	}
	print("kernel loaded at pa %x..%x\n", kern_lo, kern_hi);
	if (rd_resident) {
		print("resident ramdisk at pa %x size %x\n", rd_pa, rd_size);
	} else if (rd_size != 0) {
		memset((void *)rd_pa, 0, rd_size);
		memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
		print("ramdisk at pa %x size %x\n", rd_pa, rd_size);
	}
	if (rd1_size != 0) {
		print("resident image at pa %x size %x: it is the root (md0), the payload ramdisk is md1\n", rd1_pa, rd1_size);
	}
	/* the whole layout in two lines, so that one run's log proves where everything went */
	print("layout: payload %x..%x%s, kernel %x..%x, ramdisk %x..%x, device tree %x..%x, boot_args %x, panic log %x\n",
	    payload_base, payload_base + PAYLOAD_MAX_SIZE - 1, payload_base != PAYLOAD_BASE ? " (moved)" : "", kern_lo,
	    kern_hi - 1, rd_pa, rd_pa + rd_size - 1, dt_pa, dt_pa + dt_max - 1, ba_pa, PANIC_LOG_BASE);
	print("layout: XNU's RAM %x..%x (memSize %x); kept from XNU and UEFI:", win_base, win_base + win_size - 1, win_size);
	for (int k = 0; k < 2; k++) {
		if (PLAT->keep_out[k].size != 0) {
			print(" %x..%x (%s)", PLAT->keep_out[k].base, PLAT->keep_out[k].base + PLAT->keep_out[k].size - 1,
			    PLAT->keep_out[k].what);
		}
	}
	print("%s\n", PLAT->keep_out[0].size != 0 ? "" : " nothing");
	/* the RAM beside the window that is the kernel's too (extra_ram_claim), and how much it has in all */
	int give_extra = extra_ram_count != 0 && extra_ram_wanted(pi.cmdline);
	if (give_extra) {
		print("layout: extra RAM for XNU:");
		for (int i = 0; i < extra_ram_count; i++) {
			print(" %x..%x", extra_ram[i].base, extra_ram[i].base + extra_ram[i].size - 1);
		}
		print(" (%d MB); XNU's RAM in all %d MB\n", extra_ram_total >> 20, (win_size + extra_ram_total) >> 20);
	} else if (extra_ram_count != 0) {
		print("layout: extra RAM (%d MB) not given: extra_ram=0\n", extra_ram_total >> 20);
	}

	/* "bootcpu=N" among the kernel's arguments: run it on core N (see run_on_other_cpu) */
	uint64_t this_mpidr = READ_SYSREG(MPIDR_EL1) & 0xFFFFFF, kernel_mpidr = this_mpidr;
	/* "cpus=N" or "cpumask=M" there too: the kernel may have more cores than the one it starts on */
	int more_cores = 0, battery_requested = 0, ufs_requested = 0;
	/* "torch=N" (not 0) there too: the phone's flashlight is the kernel's (drivers/camera/hakios.torch.c) */
	int torch_requested = 0;
	/* "cam=5" there too: the phone's wide camera is Darwin's (drivers/camera/hakios.camera.c) */
	int camera_requested = 0;
	/* (the loader stays for run after run: what the last run's arguments said must not hold for this one) */
	gamut_p3 = 0;
	/* a run that does not ask for writes does not get them, whatever an earlier run of this
	 * loader asked for (the loader stays resident across runs) */
	block_write_requested = 0;
	block_write_part = -1;
	block_write_first = BLOCK_WRITE_FIRST;
	/*
	 * the same for the other storage requests (they are only ever OR-ed in below): without this a run
	 * that did not ask for ufs=2 is still offered a stale page and given its ufs_shared_pa
	 */
	hc_requested = 0;
	block_probe_requested = 0;
	block_enabled = 0;
	core0_requested = core1_requested = 0;
	int core0_off = 0, cpus_all = 0, ufs3 = 0;
	core_box = NULL;
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "bootcpu=") && (c == pi.cmdline || c[-1] == ' ') && c[8] >= '0' && c[8] <= '7') {
			int want = c[8] - '0';

			/* a core left out by default (parked_cores) is not one to boot on: the next one instead */
			while (want < PLAT->cores && ((PLAT->parked_cores >> want) & 1)) {
				print("bootcpu=%d: that core is left out here (parked_cores); core %d instead\n", want, want + 1);
				want++;
			}
			if (want < PLAT->cores) {
				kernel_mpidr = core_mpidr(PLAT, want);
			}
		}
		more_cores |= (starts_with(c, "cpus=") || starts_with(c, "cpumask=")) && (c == pi.cmdline || c[-1] == ' ');
		battery_requested |= starts_with(c, "battery=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[9] == 0 || c[9] == ' ');
		torch_requested |= starts_with(c, "torch=") && (c == pi.cmdline || c[-1] == ' ') && c[6] >= '1' && c[6] <= '9';
		camera_requested |= starts_with(c, "cam=5") && (c == pi.cmdline || c[-1] == ' ') && (c[5] == 0 || c[5] == ' ');
		if (c == pi.cmdline || c[-1] == ' ') {
			/*
			 * core0=1: this core the kernel's once it no longer needs the loader (core0=2: as soon as it is offered, for a
			 * platform whose loader serves the kernel nothing: QEMU); core1=1: core 1 listed too. cpus=8 on a phone with
			 * parked_cores says both, below: the boot line has no room for more (607 characters)
			 */
			core0_requested |= (starts_with(c, "core0=1") || starts_with(c, "core0=2")) && (c[7] == 0 || c[7] == ' ');
			core1_requested |= starts_with(c, "core1=1") && (c[7] == 0 || c[7] == ' ');
			core0_off |= starts_with(c, "core0=0") && (c[7] == 0 || c[7] == ' ');
			cpus_all |= starts_with(c, "cpus=8") && (c[6] == 0 || c[6] == ' ');
			ufs3 |= starts_with(c, "ufs=3") && (c[5] == 0 || c[5] == ' ');
		}
		gamut_p3 |= starts_with(c, "gamut=p3") && (c == pi.cmdline || c[-1] == ' ');
		/*
		 * ufs=3: served as ufs=2. The kernel reads its references through this service, then its own driver takes
		 * the controller (drivers/storage/ufs/hakios.ufs.c, ufs_nufs_start); while the page says its list is in, nothing
		 * is asked of UEFI's driver here (block_poll).
		 */
		if (starts_with(c, "ufs=") && (c == pi.cmdline || c[-1] == ' ') && c[4] >= '1' && c[4] <= '3' &&
		    (c[5] == 0 || c[5] == ' ')) {
			ufs_requested = c[4] - '0';
		}
		if (starts_with(c, "recovery=0") && (c == pi.cmdline || c[-1] == ' ') && (c[10] == 0 || c[10] == ' ')) {
			reset_to_recovery = 0;
		}
		block_probe_requested |= starts_with(c, "ufs_probe=1") && (c == pi.cmdline || c[-1] == ' ');
		block_write_requested |= starts_with(c, "ufs_write=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[11] == 0 || c[11] == ' ');
		/* 1-3: the kernel's list beside UEFI's, in this loader's area; 4 and 5 are the kernel's alone (it has the
		 * controller for the run, and this loader serves no storage then: drivers/storage/ufs/hakios.ufshc.c, own mode) */
		hc_requested |= starts_with(c, "ufs_native=") && (c == pi.cmdline || c[-1] == ' ') && c[11] >= '1' &&
		    c[11] <= '3';
	}
	if (hc_requested && HC_PA == 0) {
		print("ufs: ufs_native= refused: platform %s has no UFS host controller for the kernel's own driver\n", PLAT->name);
		hc_requested = 0;
	}
	if (battery_requested) {
		charger_start();
	}
	if (ufs_requested >= 2) {
		hc_init();              /* (ufs_native=: UEFI's transfer list kept, before anything reads) */
	}
	if (ufs_requested == 3) {
		print("ufs: ufs=3: the block service as ufs=2, for the kernel's own driver to check itself against and to fall "
		    "back to; nothing asked of UEFI's driver while the kernel's list is in\n");
	}
	if (ufs_requested >= 1) {
		ufs_survey();
	}
	if (ufs_requested >= 2) {
		block_start();
	}
	if (checkpoint == CHECKPOINT_SELFTEST) {
		kernel_mpidr = this_mpidr;
	}

	/* where the kernel will show its signs of life, and how long it has said it will run */
	const struct kernel_live *kernel_live = NULL;
	uint64_t kernel_budget = 0;
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == KERNEL_LIVE_MAGIC) {
			kernel_live = (const struct kernel_live *)q;
			break;
		}
	}
	/*
	 * cpus=8 on a phone that leaves a core out by default: all eight, so that core too and, with ufs=3 (nothing needs the
	 * loader after its switch), this one; core0=0 keeps this core the loader's (for a run whose hang is to be logged).
	 */
	if (PLAT->parked_cores != 0 && cpus_all) {
		core1_requested = 1;
		core0_requested |= ufs3 && !core0_off;
	}
	/* core0=1: the kernel's box for this core (above), if it has one, and only with a kernel on another core to watch */
	if (core0_requested) {
		for (uint64_t *q = (uint64_t *)kern_lo; q + 8 <= (uint64_t *)kern_hi; q++) {
			if (q[0] == LOADER_CORE_MAGIC && q[1] + slide == (uint64_t)(uintptr_t)q) {
				core_box = (struct loader_core_box *)q;
				break;
			}
		}
		if (core_box == NULL || kernel_live == NULL || kernel_mpidr == this_mpidr || !more_cores) {
			print("core0=1: %s: this core stays the loader's\n", core_box == NULL ? "the kernel has no box for it (patch 1513)" :
			    kernel_live == NULL ? "the kernel shows no signs of life to watch" : kernel_mpidr == this_mpidr ?
			    "the kernel starts on this core itself" : "the kernel is given one core (no cpus=)");
			core_box = NULL;
		} else {
			int n = core_number(PLAT, this_mpidr);

			core_box->mpidr = n >= 0 ? core_mpidr(PLAT, n) : this_mpidr;
			core_box->state = LOADER_CORE_NONE;
			core_box->entry = 0;
			core_box->back = (uint64_t)(uintptr_t)&core0_return_stub;
			core_box->wdt_pa = core_box->wdt_count = 0;
			print("core0=1: this core (%x) goes to the kernel when it asks (after its switch to its own UFS driver); box at %x\n",
			    core_box->mpidr, (uint64_t)(uintptr_t)core_box);
		}
	}
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "loader_return_after=")) {
			for (c += 20; *c >= '0' && *c <= '9'; c++) {
				kernel_budget = kernel_budget * 10 + (uint64_t)(*c - '0');
			}
			break;
		}
	}

	/* Give the kernel the address of our return stub and the checkpoint to stop at. */
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == 0x545352444C554E58ULL /* "XNULDRST" */) {
			q[1] = kernel_mpidr != this_mpidr ? (uint64_t)(uintptr_t)&other_cpu_return_stub :
			    (uint64_t)(uintptr_t)&loader_return_stub;
			q[2] = checkpoint;
			q[3] = slide;
			print("return slot patched at %x, checkpoint %d\n", (uint64_t)(uintptr_t)q, checkpoint);
			break;
		}
	}

	/* --- boot_args ------------------------------------------------------ */
	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->physBase = phys_base;
	ba->memSize = win_size;
	ba->memSizeActual = win_size;
	ba->topOfKernelData = top;
	/* any display the firmware has set up: the phone's panel, or QEMU's ramfb */
	if (SCREEN.base != 0) {
		/*
		 * v_display is how iBoot says "graphics mode": the kernel then keeps
		 * its text off the screen. A verbose boot (-v) wants the text.
		 */
		int verbose = 0;
		for (const char *c = pi.cmdline; *c; c++) {
			verbose |= starts_with(c, "-v") && (c[2] == ' ' || c[2] == 0) && (c == pi.cmdline || c[-1] == ' ');
		}
		ba->video.v_baseAddr = SCREEN.base;
		ba->video.v_display = verbose ? 0 : 1;
		ba->video.v_rowBytes = SCREEN.stride;
		ba->video.v_width = SCREEN.width;
		ba->video.v_height = SCREEN.height;
		ba->video.v_depth = 32;
	}
	/*
	 * No "-v" by default: on the phone the verbose console is a text
	 * framebuffer that scrolls 10 MB per line and slows boot to a crawl.
	 * The log comes back through the loader instead. Add -v via the
	 * payload's cmdline entry to watch the boot on screen.
	 */
	/*
	 * serial=3 sends the console to the UART. Where there is none it would
	 * only silence the screen; and a "serial=" in the payload's own
	 * arguments must win, which means ours must not come first.
	 */
	int own_serial = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_serial |= starts_with(c, "serial=");
	}
	const char *base_cmdline = (PLAT->uart != 0 && !own_serial) ? "debug=0x8 serial=3 " : "debug=0x8 ";
	uint32_t n = 0;
	for (const char *c = base_cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
		ba->commandLine[n++] = *c;
	}
	/* one processor unless the payload asks for more (of two "cpus=" the kernel takes the first) */
	if (!more_cores) {
		for (const char *c = "cpus=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	/* the root is the first RAM disk unless the payload says otherwise */
	int own_root = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_root |= starts_with(c, "rd=") && (c == pi.cmdline || c[-1] == ' ');
	}
	if (rd_size != 0 && !own_root) {
		for (const char *c = "rd=md0 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	if (wdt_armed && !exynos_wdt_owns()) {       /* (the Exynos watchdog is fed by the loader's core, not the kernel) */
		for (const char *c = "hv_wdt_pet=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	if (PLAT->psci_hvc) {
		/* the shim's platforms: ABL's device tree stays in place, outside XNU's window; ufs_native_cold=1 reads phy-cal-data from it */
		static const efi_guid dtb_guid = EFI_DTB_TABLE_GUID;
		static const char prefix[] = "abl_fdt=0x";
		static const char hex[] = "0123456789abcdef";

		for (uint64_t i = 0; i < ST->nr_tables; i++) {
			if (guid_eq(&ST->tables[i].guid, &dtb_guid)) {
				uint64_t address = (uint64_t)(uintptr_t)ST->tables[i].table;

				for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
					ba->commandLine[n++] = *c;
				}
				for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
					ba->commandLine[n++] = hex[(address >> shift) & 0xf];
				}
				if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
			}
		}
	}
	if (block_enabled) {
		static const char prefix[] = "ufs_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&block_shared;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	if (battery_requested) {
		static const char prefix[] = "battery_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&charger_shared;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	/* the version of this kernel, for sysctl kern.hakios.version and the kernel's log (no spaces in it) */
	if (pi.kernel_version != NULL && pi.kernel_version[0] != 0) {
		static const char prefix[] = "hakios_kver=";
		uint32_t before = n, need = (uint32_t)(sizeof(prefix) - 1 + str_len(pi.kernel_version) + 1);

		if (n + need < BOOT_LINE_LENGTH - 1) {
			for (const char *c = prefix; *c; c++) {
				ba->commandLine[n++] = *c;
			}
			for (const char *c = pi.kernel_version; *c && *c != ' '; c++) {
				ba->commandLine[n++] = *c;
			}
			ba->commandLine[n++] = ' ';
		} else {
			print("WARNING: hakios_kver= does not fit in the kernel command line (%d of %d used)\n", (uint64_t)before,
			    (uint64_t)(BOOT_LINE_LENGTH - 1));
		}
	}
	/*
	 * The payload's own command line, last. The kernel's boot_args holds BOOT_LINE_LENGTH characters and the
	 * copy used to stop at the end without a word: a long line lost its tail (the last argument arrived
	 * shortened and the kernel took it as that). Now it is said, with how much was lost.
	 */
	const char *rest = pi.cmdline;
	for (; *rest && n < BOOT_LINE_LENGTH - 1; rest++) {
		ba->commandLine[n++] = *rest;
	}
	if (*rest != 0) {
		print("WARNING: the kernel command line is TRUNCATED: %d characters of the payload's cmdline did not fit in the "
		    "%d the kernel has; the last argument is cut\n", (uint64_t)str_len(rest), (uint64_t)(BOOT_LINE_LENGTH - 1));
	}
	print("cmdline: %d of %d characters\n", (uint64_t)n, (uint64_t)(BOOT_LINE_LENGTH - 1));

	/* --- device tree ---------------------------------------------------- */
	const struct platform *plat = PLAT;
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .end = (uint8_t *)(dt_pa + dt_max), .depth = 0 };
	uint64_t cntfrq = timer_hz();
	uint64_t ranges[3] = { 0, plat->io_base, plat->io_size };
	uint64_t reg2[2];
	uint8_t seed[64];

	for (unsigned i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(READ_SYSREG(CNTVCT_EL0) >> (i & 7)) ^ (uint8_t)(i * 37);
	}

	dt_begin(&w, "device-tree");
	/*
	 * The identity is the iPhone's (model, compatible, target-type ...): Darwin user space looks
	 * its device up by these, and its files -- the dyld shared cache to begin with -- are
	 * those of that phone. XNU itself does not care; the hardware is described further down.
	 */
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	/* what iBoot takes from the factory configuration of a real phone; fixed size fields */
	dt_prop_padded(&w, "serial-number", "RDW00POCOX5P", 32);
	dt_prop_padded(&w, "mlb-serial-number", "RDW00POCOX5PMLB00", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", phys_base);
	dt_prop_u64(&w, "dram-size", win_size);
	if (give_extra) {
		/* RAM beside the window that is the kernel's too: pairs of base and size (extra_ram_claim; patch 1512) */
		static uint64_t extra_reg[EXTRA_RAM_MAX][2];

		for (int i = 0; i < extra_ram_count; i++) {
			extra_reg[i][0] = extra_ram[i].base;
			extra_reg[i][1] = extra_ram[i].size;
		}
		dt_prop(&w, "hakios-extra-ram", extra_reg, (uint32_t)(extra_ram_count * sizeof(extra_reg[0])));
	}
	dt_prop_u32(&w, "debug-enabled", 1);
	/* what MobileGestalt answers "main screen scale" from: 3 device pixels per point */
	dt_prop_u32(&w, "display-scale", 3);
	dt_prop_u32(&w, "display-rotation", 0);
	/* how the kernel is to ask the firmware for more cores (PSCI) */
	dt_prop_str(&w, "psci-method", plat->psci_hvc ? "hvc" : "smc");
	/* the time now, by the firmware's clock (the kernel has no real-time clock driver) */
	uint64_t epoch = firmware_epoch();
	if (epoch != 0) {
		dt_prop_u64(&w, "boot-epoch", epoch);
	}
	/*
	 * More of what iBoot reports about the phone. MobileGestalt builds the device identifier
	 * (UDID) from the serial number, the chip ID and the two addresses; when one is missing it
	 * asks its helper for the factory data, is refused and waits forever (and lsd with it).
	 */
	static const uint8_t wifi_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };
	static const uint8_t bluetooth_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
	static const uint8_t manifest_hash[48] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64 };
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
	dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	/* An empty but well-formed NVRAM image: IOKit refuses to start without it. */
	static uint8_t nvram_image[NVRAM_BANK_SIZE];
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	dt_begin(&w, "memory-map");
	/*
	 * "RAMDisk" becomes /dev/md0, the root (the kernel and launchd treat
	 * only md0 as a RAM disk boot); "RAMDisk1" becomes /dev/md1. With both
	 * images present the big resident one is the root and the payload's
	 * small one is the second disk.
	 */
	if (rd_size != 0 || rd1_size != 0) {
		uint64_t first[2] = { rd1_size != 0 ? rd1_pa : rd_pa, rd1_size != 0 ? rd1_size : rd_size };
		dt_prop(&w, "RAMDisk", first, sizeof(first));
	}
	if (rd_size != 0 && rd1_size != 0) {
		uint64_t second[2] = { rd_pa, rd_size };
		dt_prop(&w, "RAMDisk1", second, sizeof(second));
	}
	dt_end(&w);
	if (more_cores && plat->timeout_scale != 0) {
		/* multiplies every "machine timeout" of the kernel (machine_timeout_init_with_suffix()) */
		dt_begin(&w, "machine-timeouts");
		dt_prop_u32(&w, "global-scale", plat->timeout_scale);
		dt_end(&w);
	}
	dt_end(&w);

	/*
	 * What iBoot's device tree tells the kernel to do unless told otherwise. The compressor: an
	 * embedded kernel left to itself keeps it for the freezer alone, and with nothing being
	 * compressed the memory is gone twice as fast (2 = VM_PAGER_COMPRESSOR_NO_SWAP, as on a phone).
	 */
	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	/* the product: screen class, artwork scale, name ... as on the real phone */
	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	/*
	 * and its haptics: "closed-loop" (MobileGestalt's DeviceSupportsClosedLoopHaptics) makes Darwin play
	 * its vibrations and feedbacks as waveforms on the "Actuator" audio device (the audio plug-in
	 * hands them to the kernel's motor driver); without it Darwin looks for an IOKit vibration motor
	 * that the iPhone X does not have, and nothing vibrates.
	 */
	dt_begin(&w, "haptics");
	dt_identity(&w, identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]), identity_haptics_data);
	dt_end(&w);
	/*
	 * and of its camera what is here: with torch=N the flash (MobileGestalt's camera-flash; without
	 * it Control Center has no flashlight); with cam=5 the node itself, for a camera there is (with it
	 * MobileGestalt's video-camera). The rest of the iPhone's camera is not here, nor said to be.
	 */
	if (torch_requested || camera_requested) {
		dt_begin(&w, "camera");
		if (torch_requested) {
			dt_identity_one(&w, identity_camera, sizeof(identity_camera) / sizeof(identity_camera[0]), identity_camera_data, "flash");
		}
		dt_end(&w);
	}
	dt_end(&w);

	/* IOKit publishes its NVRAM object on this node; without it IOKit asserts. */
	dt_begin(&w, "options");
	dt_end(&w);

	/* Panic log region: RAM outside XNU's window (tail of the payload reservation). */
	uint64_t pram_reg[2] = { PANIC_LOG_BASE, PANIC_LOG_SIZE };
	memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	/* the PCI host bridge, if there is one (virtio-pci devices under QEMU or crosvm) */
	dt_pci_node(&w);

	/*
	 * The processors. The first is the one the kernel starts on. With "cpus=N" in its
	 * arguments the kernel takes that one and the next N-1 of this list and starts them
	 * itself (PSCI CPU_ON); "cpumask=M" picks by position, bit 0 being the first. So the
	 * order matters: after the boot core come those whose numbers are nearest to its own
	 * (on the phone, where the kernel boots on the biggest core, 7: the other big ones
	 * 6, 5, 4, then the small ones 3, 2, 1). Only cores that are off are listed: the one
	 * this loader stays on while it watches is not the kernel's, nor any the firmware runs.
	 */
	uint64_t cpu_mpidr[8] = { kernel_mpidr };
	int cpu_count = 1, boot_core = core_number(plat, kernel_mpidr);

	if (boot_core < 0) {
		boot_core = 0;          /* (a boot core the platform does not number: the others counted from 0) */
	}

	if (more_cores) {
		/*
		 * The cores of the boot core's own kind first (both small or both not), then the others: the kernel takes the
		 * first cpus=N, and on the Galaxy S9 a fourth core by nearness alone was an Exynos M3, an ARMv8.0 core under a
		 * kernel that had said ARMv8.2 from the A55 it boots on.
		 */
		int boot_small = (int)((plat->small_cores >> boot_core) & 1);

		for (int pass = 0; pass < 2; pass++)
		for (int distance = 1; distance < plat->cores; distance++) {
			for (int side = 0; side < 2; side++) {
				int core = side == 0 ? boot_core + distance : boot_core - distance;
				uint64_t mpidr;
				int64_t state;

				if (core < 0 || core >= plat->cores || (mpidr = core_mpidr(plat, core)) == this_mpidr) {
					continue;
				}
				if (((int)((plat->small_cores >> core) & 1) == boot_small) != (pass == 0)) {
					continue;
				}
				if ((plat->parked_cores >> core) & 1) {
					if (!core1_requested) {
						print("core %x is left out (parked_cores; core1=1 lists it)\n", mpidr);
						continue;
					}
					/* core1=1: core 1, which S-Boot uses for its own display work, listed for the kernel all the same */
					print("core %x is the one the boot loader used for itself; core1=1: listed for the kernel\n", mpidr);
					cpu_mpidr[cpu_count++] = mpidr;
					continue;
				}
				state = psci_call(PSCI_AFFINITY_INFO, mpidr, 0, 0);
				if (state == 1 || state == PSCI_NOT_SUPPORTED || (state == 0 && plat->affinity_unreliable)) {
					cpu_mpidr[cpu_count++] = mpidr;
				} else if (state >= 0) {
					print("core %x is not off (the firmware says %d): not the kernel's to take\n", mpidr, (uint64_t)state);
				}
			}
		}
		if (core_box != NULL && cpu_count < 8) {
			/* last: the kernel starts it only once the loader is done with it */
			cpu_mpidr[cpu_count++] = core_box->mpidr;
		}
		print("cores for the kernel, in the order it takes them:");
		for (int i = 0; i < cpu_count; i++) {
			print(" %x", cpu_mpidr[i]);
		}
		print("\n");
	}

	dt_begin(&w, "cpus");
	for (int i = 0; i < cpu_count; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };

		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", (uint32_t)cpu_mpidr[i]);
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		if (core_number(plat, cpu_mpidr[i]) >= 0 && (plat->small_cores & (1u << core_number(plat, cpu_mpidr[i])))) {
			dt_prop_u32(&w, "small-core", 1);
		}
		dt_end(&w);
	}
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", plat->name);
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);
	/*
	 * With cam=5, where MobileGestalt looks for the cameras (IODeviceTree:/arm-io/isp, its still-camera and
	 * rear-facing-camera, by which SpringBoard shows Camera.app): the iPhone's ISP node, of its
	 * properties only the camera at the back. Neither a compatible nor a device_type: no driver matches it.
	 */
	if (camera_requested) {
		dt_begin(&w, "isp");
		dt_prop_u32(&w, "camera-rear", 1);
		dt_end(&w);
	}
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	if (plat->gic_cpu != 0) {
		/* GICv2: the distributor, then the CPU interface (osfmk/arm64/gic_v3_irq.c tells the two apart by this) */
		dt_prop_str(&w, "compatible", "arm,gic-400");
		uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x1000, plat->gic_cpu - plat->io_base, 0x2000 };
		dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	} else {
		dt_prop_str(&w, "compatible", "arm,gic-v3");
		uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x10000, plat->gic_redist - plat->io_base,
		    plat->gic_redist_size != 0 ? plat->gic_redist_size : 0x100000 };
		dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	}
	dt_end(&w);
	if (plat->uart != 0) {
		dt_begin(&w, "uart0");
		dt_prop_str(&w, "device_type", "uart");
		dt_prop_str(&w, "compatible", "arm,pl011");
		reg2[0] = plat->uart - plat->io_base;
		reg2[1] = 0x1000;
		dt_prop(&w, "reg", reg2, sizeof(reg2));
		dt_prop_u32(&w, "boot-console", 1);
		dt_end(&w);
	}
	dt_end(&w);

	dt_end(&w);

	/* a tree that did not fit in the room before boot_args was cut short by dt_prop: not handed to the kernel */
	if (w.overflow) {
		print("device tree does not fit in %x bytes: not started\n", dt_max);
		return 0;
	}
	uint32_t dt_len = (uint32_t)(w.cur - w.base);
	ba->deviceTreeP = dt_pa - slide;
	ba->deviceTreeLength = dt_len;
	print("device tree %x len %x, boot_args %x, top %x\n", dt_pa, (uint64_t)dt_len, ba_pa, top);
	print("cmdline: %s\n", ba->commandLine);
	if (kernel_mpidr != this_mpidr) {
		print("the kernel will run on core %x (this is core %x)\n", kernel_mpidr, this_mpidr);
	}
	print("jumping to %x\n", entry_va + slide);
	flush_log();
	screen_quiet_before_run = screen_quiet;
	screen_quiet = 1;               /* the screen is the kernel's now; the log still takes every line */
	/* From here on the kernel feeds the watchdog (boot-arg hv_wdt_pet, see loader_return.c in XNU). */
	wdt_set_ms(60000);

	/* --- hand off (UEFI is left resident, see loader_save_ctx) ------------ */
	if (loader_save_ctx() == 0) {
		/*
		 * On real hardware (under the Qualcomm hypervisor) dirty data-cache
		 * lines do not survive the MMU-off round trip. UEFI stays resident,
		 * so everything it owns must be written back to RAM first: clean
		 * every allocated region in the memory map, then this stack again.
		 */
		map_size = sizeof(map_buf);
		if (!EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
			for (uint64_t off = 0; off < map_size; off += desc_size) {
				efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
				/* loader/boot-services/runtime code and data, ACPI tables; XNU's own window is handled below */
				if (d->phys_start >= win_base && d->phys_start < win_base + win_size) {
					continue;
				}
				if ((d->type >= 1 && d->type <= 6) || d->type == 9 || d->type == 10) {
					clean_dcache(d->phys_start, d->phys_start + (d->pages << 12));
				}
			}
		}
		uint64_t sp_now = (uint64_t)(uintptr_t)__builtin_frame_address(0);
		clean_dcache(sp_now - 0x2000, sp_now + 0x2000);
		flush_dcache(phys_base, top);
		if (checkpoint == CHECKPOINT_SELFTEST) {
			/* Do not enter the kernel at all: exercise only the return path. */
			jump_to_kernel((uint64_t)(uintptr_t)&loader_return_stub, 0);
		}
		if (kernel_mpidr != this_mpidr) {
			int64_t status = run_on_other_cpu(kernel_mpidr, entry_va + slide, ba_pa, kernel_live, slide, kernel_budget);

			screen_quiet = screen_quiet_before_run;
			wdt_set_ms(WDT_BITE_MS);
			if (status != 0) {
				/* a kernel that hung may have left its own transfer list in the controller */
				hc_take_back(status == OTHER_CPU_HUNG ? "the kernel hung" : "the kernel did not start");
			}
			if (status == OTHER_CPU_HUNG) {
				/* the kernel still has that core; what it logged can be had all the same */
				print("THE KERNEL HUNG on core %x: no sign of life for %d seconds; its log follows\n", kernel_mpidr,
				    (uint64_t)SILENCE_SECONDS);
				set_result("hung", loader_ctx[CTX_LOG_LEN], 0);
				write_logs((const char *)loader_ctx[CTX_LOG_PA], loader_ctx[CTX_LOG_LEN]);
				return 1;
			} else if (status != 0) {
				/* the kernel was told it would be on that core (return path, device tree): no second try */
				if (status == OTHER_CPU_NOT_STARTED) {
					print("core %x was started but never arrived\n", kernel_mpidr);
				} else {
					print("core %x did not start: PSCI error %d\n", kernel_mpidr, (uint64_t)-status);
				}
				set_result("no-such-core", (uint64_t)(status < 0 ? -status : status), 0);
				write_logs(NULL, 0);
				return 1;
			}
		} else {
			exynos_wdt_pause("the kernel runs on this core, and nothing would feed it");
			jump_to_kernel(entry_va + slide, ba_pa);
		}
	}

	/* --- the kernel came back ------------------------------------------- */
	screen_quiet = screen_quiet_before_run;
	wdt_set_ms(WDT_BITE_MS);
	hc_take_back("the kernel came back");   /* before anything here touches the storage */
	if (block_enabled) {
		flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
		    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
		if (block_shared.kernel_native.list_in != 0) {
			/* (ufs=3) the kernel did not say UEFI's list is back: the storage is not used again until a restart */
			print("ufs: the kernel left word that its own transfer list is in: the block service stays refused\n");
		}
	}
	if (hc_known) {
		flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
		    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
		print("ufs: the kernel put its own list in %d times and took it out %d times\n",
		    (uint64_t)block_shared.kernel_native.switched_in, (uint64_t)block_shared.kernel_native.switched_out);
	}
	if (block_write_requested) {
		print("ufs: %d writes into %s, %d bytes, read back the same; %d refused\n", block_writes,
		    PLAT->data_part != NULL ? PLAT->data_part : "darwin", block_write_bytes, block_writes_refused);
	}
	/* ... and its other cores should all be off again: the next kernel cannot start one that is not */
	for (int i = 1; i < cpu_count; i++) {
		if (core_box != NULL && cpu_mpidr[i] == core_box->mpidr) {
			continue;       /* core0=1: this core, given to the kernel and back (or never taken): the loader's own */
		}
		int64_t state = psci_call(PSCI_AFFINITY_INFO, cpu_mpidr[i], 0, 0);

		if (state != 1 && state != PSCI_NOT_SUPPORTED) {
			print("THE KERNEL LEFT CORE %x RUNNING (the firmware says %d): restart before running another\n", cpu_mpidr[i],
			    (uint64_t)state);
		}
	}
	recording_data = NULL;
	recording_len = 0;
	if (kernel_live != NULL && kernel_live->recording != 0 && checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *recording = (const uint64_t *)(kernel_live->recording + slide);

		if (recording[0] != 0 && recording[1] > 24 && recording[1] <= (1ULL << 30)) {
			recording_data = (const char *)recording[0];
			recording_len = recording[1];
			print("the kernel left a screen recording: %d bytes at %x\n", recording_len, recording[0]);
		}
	}
	sound_data = NULL;
	sound_len = 0;
	if (kernel_live != NULL && kernel_live->magic2 == KERNEL_LIVE_MAGIC2 && kernel_live->sound != 0 &&
	    checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *sound = (const uint64_t *)(kernel_live->sound + slide);

		if (sound[0] != 0 && sound[1] > 24 && sound[1] <= (1ULL << 30)) {
			sound_data = (const char *)sound[0];
			sound_len = sound[1];
			print("the kernel left a sound recording: %d bytes at %x\n", sound_len, sound[0]);
		}
	}
	const char *klog = (const char *)loader_ctx[CTX_LOG_PA];
	uint64_t klog_len = loader_ctx[CTX_LOG_LEN];
	uint64_t reason = loader_ctx[CTX_REASON];
	if (checkpoint == CHECKPOINT_SELFTEST) {
		print("return-path self test passed\n");
		set_result("selftest-ok", 0, 0);
		klog = NULL;
		klog_len = 0;
	} else {
		print("kernel returned: log at %x, %d bytes\n", (uint64_t)(uintptr_t)klog, klog_len);
		if (reason == 2) {
			print("STOPPED AT CHECKPOINT %d\n", loader_ctx[CTX_ESR]);
			set_result("checkpoint", loader_ctx[CTX_ESR], 0);
		} else if (reason == 1) {
			print("EARLY KERNEL EXCEPTION: esr=%x elr=%x far=%x lr=%x sp=%x\n", loader_ctx[CTX_ESR], loader_ctx[CTX_ELR],
			    loader_ctx[CTX_FAR], loader_ctx[CTX_LR], loader_ctx[CTX_KSP]);
			print("  (pa-va slide %x: subtract it from physical addresses to symbolicate)\n", slide);
			set_result("early-exception", 0, loader_ctx[CTX_ELR]);
			klog_len = klog_len > (1u << 20) ? 0 : klog_len;
		} else {
			set_result("returned", klog_len, 0);
		}
	}
	write_logs(klog, klog_len);
	return 1;
}

#include "fastboot.c"

/* ------------------------------------------------------------------------ */

/*
 * QEMU's "ramfb" display shows whatever guest memory it is told to, in any size; the firmware
 * only offers a few small landscape modes of it. So we point it at memory of our own with the
 * shape of the iPhone's screen, through the fw_cfg file "etc/ramfb" (the firmware's own mode
 * is simply replaced). In QEMU the display is then exactly what Darwin expects; on the real phone
 * the kernel's display driver scales that picture to the panel.
 */
#define PHONE_WIDTH     1125            /* the screen of the iPhone X, whose identity we carry */
#define PHONE_HEIGHT    2436
#define FW_CFG_BASE     0x09020000ULL   /* QEMU virt: data at +0, selector at +8, DMA address at +16 */
#define FW_CFG_FILE_DIR 0x19
#define FW_CFG_DMA_SELECT 0x08
#define FW_CFG_DMA_WRITE  0x10

static uint32_t
be32(uint32_t v)
{
	return __builtin_bswap32(v);
}

static void
fw_cfg_read(void *buf, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		((uint8_t *)buf)[i] = *(volatile uint8_t *)FW_CFG_BASE;
	}
}

static void
ramfb_phone_screen(void)
{
	static struct {
		uint64_t addr;
		uint32_t fourcc, flags, width, height, stride;
	} __attribute__((packed)) cfg;
	/*
	 * volatile: QEMU writes `control' back (to 0) when it has done the request, behind the compiler's back. Without the
	 * qualifier a build is free to keep the value written a few lines below and never look again, and the loader then
	 * says the request failed, leaves ramfb at the firmware's 800x600 and Darwin draws its 1125x2436 into a corner: a black window
	 * (it did, with a newer build of this very file; the older one happened to reload).
	 */
	static volatile struct {
		uint32_t control, length;
		uint64_t address;
	} dma;
	struct {
		uint32_t size;
		uint16_t select, reserved;
		char name[56];
	} file;
	uint32_t count, select = 0;
	uint64_t fb = 0, bytes = (uint64_t)PHONE_WIDTH * 4 * PHONE_HEIGHT;

	*(volatile uint16_t *)(FW_CFG_BASE + 8) = __builtin_bswap16(FW_CFG_FILE_DIR);
	fw_cfg_read(&count, sizeof(count));
	count = be32(count);
	for (uint32_t i = 0; i < count && i < 256 && select == 0; i++) {
		fw_cfg_read(&file, sizeof(file));
		if (str_eq(file.name, "etc/ramfb")) {
			select = __builtin_bswap16(file.select);
		}
	}
	if (select == 0 || EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, (bytes + 0xFFF) >> 12, &fb))) {
		return;
	}
	memset((void *)(uintptr_t)fb, 0, (size_t)bytes);
	cfg.addr = __builtin_bswap64(fb);
	cfg.fourcc = be32(0x34325258);          /* 'XR24': blue, green, red, unused */
	cfg.flags = 0;
	cfg.width = be32(PHONE_WIDTH);
	cfg.height = be32(PHONE_HEIGHT);
	cfg.stride = be32(PHONE_WIDTH * 4);
	dma.control = be32(select << 16 | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE);
	dma.length = be32(sizeof(cfg));
	dma.address = __builtin_bswap64((uint64_t)(uintptr_t)&cfg);
	__asm__ volatile ("dsb sy" ::: "memory");       /* (cfg and dma are in memory before the device is told to read them) */
	*(volatile uint64_t *)(FW_CFG_BASE + 16) = __builtin_bswap64((uint64_t)(uintptr_t)&dma);
	/* the device does it at once; allow it some time all the same instead of reading once */
	for (uint32_t spin = 0; dma.control != 0 && spin < 1000000; spin++) {
		__asm__ volatile ("" ::: "memory");
	}
	if (dma.control != 0) {
		print("ramfb: control %x after the request\n", (uint64_t)be32(dma.control));
		return;
	}
	SCREEN.base = fb;
	SCREEN.width = PHONE_WIDTH;
	SCREEN.height = PHONE_HEIGHT;
	SCREEN.stride = PHONE_WIDTH * 4;
}

/*
 * UEFI-native boot: a payload from the volume the loader was started from.
 *
 * On the phone the payload (the kernel, the small root image, the command line) is put in memory at PAYLOAD_BASE
 * before the loader runs: by the boot shim behind Project Silicium's image (fastboot boot), or by QEMU's loader
 * device. This is the other way, the one every UEFI boot loader has: when there is no payload in memory, look for
 * \EFI\hakios\payload.bin on the volume this image was loaded from (the EFI system partition, where
 * \EFI\BOOT\BOOTAA64.EFI is the loader itself), read it to PAYLOAD_BASE and go on as if it had been there all along.
 * That is what lets the kernel start from a boot menu, a USB stick or an SD card, without fastboot.
 *
 * It prints nothing (the banner has not been drawn yet): volume_payload_bytes and volume_payload_note say what
 * happened, and efi_main says it under the banner. The file has to fit in the payload region (less the last MiB,
 * which holds the panic log), and the region has to be free: where a firmware keeps it reserved, the allocation is
 * refused and the loader goes on without a payload, as it always did.
 * Only the kernel and a root image that fits in the payload come this way; the big Darwin system image does not
 * (docs/uefi-boot.md says what does).
 */
static uint64_t volume_payload_bytes;           /* 0: nothing was read; else the payload's size */
static const char *volume_payload_note;         /* why not, if it was looked for */

static int
load_payload_from_volume(void)
{
	static const efi_guid image_guid = EFI_LOADED_IMAGE_GUID, fs_guid = EFI_SIMPLE_FILE_SYSTEM_GUID,
	    info_guid = EFI_FILE_INFO_GUID;
	static const char16 path[] = u"\\EFI\\hakios\\payload.bin";
	efi_loaded_image *image = NULL;
	efi_simple_fs *fs = NULL;
	efi_file *root = NULL, *file = NULL;
	uint8_t info[512] __attribute__((aligned(8)));
	uint64_t info_size = sizeof(info), size, done = 0, pages;
	uint64_t address = PAYLOAD_BASE;

	if (EFI_ERROR(BS->handle_protocol(IMAGE, &image_guid, (void **)&image)) || image == NULL || image->device == NULL ||
	    EFI_ERROR(BS->handle_protocol(image->device, &fs_guid, (void **)&fs)) || fs == NULL) {
		volume_payload_note = "the loader was not started from a volume with a file system";
		return 0;
	}
	if (EFI_ERROR(fs->open_volume(fs, &root)) || root == NULL) {
		volume_payload_note = "the volume cannot be opened";
		return 0;
	}
	if (EFI_ERROR(root->open(root, &file, path, EFI_FILE_MODE_READ, 0)) || file == NULL) {
		volume_payload_note = "there is no \\EFI\\hakios\\payload.bin on the volume";
		root->close(root);
		return 0;
	}
	if (EFI_ERROR(file->get_info(file, &info_guid, &info_size, info))) {
		volume_payload_note = "the size of \\EFI\\hakios\\payload.bin cannot be read";
		goto close;
	}
	size = *(uint64_t *)(info + EFI_FILE_INFO_FILE_SIZE_OFFSET);
	if (size < sizeof(struct payload_header) || size > PAYLOAD_MAX_SIZE - 0x100000ULL) {
		volume_payload_note = "\\EFI\\hakios\\payload.bin is empty or does not fit in the payload region (63 MiB)";
		goto close;
	}
	/*
	 * Take the whole payload region, not only the pages the file fills: the rest of it is written later (a payload
	 * sent over USB with `download:` goes up to the 63 MiB limit, and the panic log lives in the last MiB), and
	 * memory the firmware has not given to us must not be written.
	 */
	pages = PAYLOAD_MAX_SIZE / 0x1000;
	if (EFI_ERROR(BS->allocate_pages(2 /* AllocateAddress */, 2 /* EfiLoaderData */, pages, &address))) {
		volume_payload_note = "the payload region is not free (the firmware keeps it reserved)";
		goto close;
	}
	while (done < size) {
		uint64_t chunk = size - done < 0x100000ULL ? size - done : 0x100000ULL;       /* 1 MiB at a time */

		if (EFI_ERROR(file->read(file, &chunk, (void *)(uintptr_t)(PAYLOAD_BASE + done))) || chunk == 0) {
			volume_payload_note = "\\EFI\\hakios\\payload.bin could not be read to its end";
			/* what was read stays in memory after the pages are given back: without this, the first MiB's XNUP
			 * header would still be there and a half-read payload would be taken for a whole one */
			((volatile struct payload_header *)PAYLOAD_BASE)->magic = 0;
			BS->free_pages(PAYLOAD_BASE, pages);
			goto close;
		}
		done += chunk;
	}
	if (((const struct payload_header *)PAYLOAD_BASE)->magic != PAYLOAD_MAGIC) {
		volume_payload_note = "\\EFI\\hakios\\payload.bin is not a payload (no XNUP at its start)";
		BS->free_pages(PAYLOAD_BASE, pages);
		goto close;
	}
	volume_payload_bytes = size;
close:
	file->close(file);
	root->close(root);
	return volume_payload_bytes != 0;
}

/*
 * The platform's keep-outs (struct platform keep_out) taken from UEFI's allocator: every free piece of [lo, hi) becomes
 * EfiReservedMemoryType, so that nothing UEFI hands out later (pool, the loader's own pages, a driver's buffer) lands
 * there while it stays alive beside the kernel, and choose_window never sees it as free. Pieces the firmware already has
 * are said, not taken.
 */
static void
keep_out_reserve(uint64_t lo, uint64_t hi, const char *what)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size, taken = 0, held = 0, failed = 0;
	uint32_t desc_ver;

	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("keep-out %x..%x (%s): GetMemoryMap failed, NOT reserved\n", lo, hi - 1, what);
		return;
	}
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
		uint64_t a = d->phys_start, b = d->phys_start + (d->pages << 12);

		if (b <= lo || a >= hi) {
			continue;
		}
		a = a < lo ? lo : a;
		b = b > hi ? hi : b;
		if (d->type == EfiConventionalMemory) {
			uint64_t at = a;

			if (EFI_ERROR(BS->allocate_pages(AllocateAddress, 0 /* EfiReservedMemoryType */, (b - a) >> 12, &at))) {
				failed += b - a;
				print("  keep-out %x..%x free but NOT taken\n", a, b - 1);
			} else {
				taken += b - a;
			}
		} else {
			held += b - a;
			print("  keep-out %x..%x is the firmware's already: type %d\n", a, b - 1, (uint64_t)d->type);
		}
	}
	print("keep-out %x..%x (%s): %d KB reserved now, %d KB the firmware's already, %d KB not taken\n", lo, hi - 1, what,
	    taken >> 10, held >> 10, failed >> 10);
}

/*
 * A platform whose payload landing place is memory that is not ours to keep (the Galaxy S9: the CP's region, struct
 * platform payload_home) has it moved out at the start, before anything else reads it: a 64 MiB block at payload_home
 * (or a step of 64 MiB above, up to three) is taken from UEFI, the payload copied there and checked, and the copy at
 * PAYLOAD_BASE made no payload (its magic cleared). If no block can be had, or the copy does not read back the same,
 * the payload stays where it landed and the run goes on as before the move: a run is never lost to it.
 * Then the keep-outs are reserved.
 */
static void
payload_move(void)
{
	const struct payload_header *ph = (const struct payload_header *)PAYLOAD_BASE;
	uint64_t home = 0, bytes;

	if (PLAT->payload_home != 0 && ph->magic == PAYLOAD_MAGIC) {
		bytes = (sizeof(*ph) + payload_walk_size(ph->size) + 15) & ~15ULL;
		for (int i = 0; i < 4 && home == 0; i++) {
			uint64_t at = PLAT->payload_home + (uint64_t)i * PAYLOAD_MAX_SIZE;

			if (!EFI_ERROR(BS->allocate_pages(AllocateAddress, EfiLoaderData, PAYLOAD_MAX_SIZE >> 12, &at))) {
				home = at;
			} else {
				print("payload: %x..%x is not free\n", at, at + PAYLOAD_MAX_SIZE - 1);
			}
		}
		if (home == 0) {
			print("payload: NOT moved (no 64 MiB free from %x): it stays at %x, in memory the platform keeps out\n",
			    PLAT->payload_home, PAYLOAD_BASE);
		} else {
			memcpy((void *)(uintptr_t)home, (const void *)(uintptr_t)PAYLOAD_BASE, bytes);
			if (memcmp((const void *)(uintptr_t)home, (const void *)(uintptr_t)PAYLOAD_BASE, bytes) != 0) {
				print("payload: the copy at %x does not read back the same: NOT moved, it stays at %x\n", home,
				    PAYLOAD_BASE);
				BS->free_pages(home, PAYLOAD_MAX_SIZE >> 12);
			} else {
				clean_dcache(home, home + bytes);
				payload_base = home;
				((volatile struct payload_header *)(uintptr_t)PAYLOAD_BASE)->magic = 0;
				clean_dcache(PAYLOAD_BASE, PAYLOAD_BASE + 64);
				print("payload: moved from %x to %x (%x bytes; its region %x..%x, the panic log at %x)\n", PAYLOAD_BASE,
				    home, bytes, home, home + PAYLOAD_MAX_SIZE - 1, PANIC_LOG_BASE);
			}
		}
	} else if (PLAT->payload_home != 0) {
		print("payload: none at %x to move (magic %x)\n", PAYLOAD_BASE, (uint64_t)ph->magic);
	}
	for (int k = 0; k < 2; k++) {
		if (PLAT->keep_out[k].size != 0) {
			keep_out_reserve(PLAT->keep_out[k].base, PLAT->keep_out[k].base + PLAT->keep_out[k].size,
			    PLAT->keep_out[k].what);
		}
	}
}

/*
 * The version of the kernel in the payload (the entry kernel.ver, made by xnuloader/mkpayload.py from
 * tools/kernel-version.py), or NULL if the payload has none. A look at the payload that changes
 * nothing and prints nothing, so that the banner can use it before the rest of the loader runs.
 */
static const char *
payload_kernel_version(void)
{
	const struct payload_header *ph = (const struct payload_header *)payload_base;

	if (ph->magic != PAYLOAD_MAGIC) {
		return NULL;
	}
	/* (the same limits as parse_payload: a header size or an entry size that reaches past the payload ends the walk) */
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + payload_walk_size(ph->size);
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;

		if (e->name[0] == 0) {
			break;
		}
		if ((uint64_t)e->size > (uint64_t)(pend - (p + sizeof(*e)))) {
			break;
		}
		if (str_eq(e->name, "kernel.ver")) {
			return (const char *)(p + sizeof(*e));
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return NULL;
}

/*
 * The greeting. Printed from efi_main, while the UEFI text console is still ours (on the phone that is
 * Project Silicium's UEFI): the USB command loop that follows sets screen_quiet and leaves the screen
 * alone, so what is on it now stays there. It also goes into the log, which "xl log" fetches.
 * The two lines under the letters are the ones the project's banner has: a greeting from the kernel,
 * and its version, kernel.ver=, as tools/kernel-version.py makes it.
 */
static void
print_banner(void)
{
	const char *version = payload_kernel_version();

	print("\n");
	print("   #   #   ###   #   #  #####   ###    ####\n");
	print("   #   #  #   #  #  #     #    #   #  #\n");
	print("   #####  #####  ###      #    #   #   ###\n");
	print("   #   #  #   #  #  #     #    #   #      #\n");
	print("   #   #  #   #  #   #  #####   ###   ####\n");
	print("\n");
	print("   hello hakios-kernel!\n");
	print("   kernel.ver=%s\n", version ? version : "unknown");
	print("\n");
}

efi_status EFIAPI
efi_main(efi_handle image, efi_system_table *st)
{
	static const efi_guid gop_guid = EFI_GOP_GUID;
	struct payload_info pi;

	ST = st;
	BS = st->bs;
	IMAGE = image;

	BS->set_watchdog_timer(0, 0, 0, NULL);
	/* no payload in memory (nobody put one there): the volume this loader came from may have one, as a UEFI boot loader's does */
	if (((const struct payload_header *)PAYLOAD_BASE)->magic != PAYLOAD_MAGIC) {
		load_payload_from_volume();
	}
	print("== XNULOADER v3 ==\n");
	print_banner();
	if (volume_payload_bytes != 0) {
		print("payload: %d bytes from \\EFI\\hakios\\payload.bin on the volume this loader was started from\n",
		    volume_payload_bytes);
	} else if (volume_payload_note != NULL) {
		print("payload: none in memory, and %s\n", volume_payload_note);
	}
	print("CurrentEL=%x MIDR=%x MMFR0=%x CNTFRQ=%d image=%x\n", READ_SYSREG(CurrentEL) >> 2, READ_SYSREG(MIDR_EL1),
	    READ_SYSREG(ID_AA64MMFR0_EL1), READ_SYSREG(CNTFRQ_EL0) /* the register itself */, (uint64_t)(uintptr_t)&__ImageBase);

	PLAT = platform_from_fdt(&platform_qemu_virt);  /* (the Pixel 10: by its device tree, before the framebuffer) */
	if (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
	    GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
		GOP = NULL;                     /* no display (QEMU without -device ramfb) */
	} else {
		SCREEN.base = GOP->mode->fb_base;
		SCREEN.width = GOP->mode->info->width;
		SCREEN.height = GOP->mode->info->height;
		SCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
		for (unsigned i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++)
			if (platforms[i]->fb_base == SCREEN.base)
				PLAT = platforms[i];
		if (PLAT == &platform_qemu_virt)
			ramfb_phone_screen();
		print("display %d x %d at %x\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
	}
	if (PLAT == &platform_qemu_virt) {
		/*
		 * A GICv2: the FDT says where, when the firmware publishes one; else the processor says there is no
		 * GICv3 register interface (ID_AA64PFR0_EL1.GIC, bits 27:24, 0) and it is QEMU virt's, at its usual place.
		 */
		static const efi_guid dtb_guid = EFI_DTB_TABLE_GUID;
		uint64_t dist = 0, cpu = 0;

		for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables; i++) {
			if (guid_eq(&ST->tables[i].guid, &dtb_guid) && gic_v2_from_fdt(ST->tables[i].table, &dist, &cpu)) {
				break;
			}
		}
		if (cpu == 0 && ((READ_SYSREG(ID_AA64PFR0_EL1) >> 24) & 0xf) == 0) {
			dist = 0x08000000, cpu = 0x08010000;
		}
		if (cpu != 0) {
			platform_qemu_virt_gicv2 = platform_qemu_virt;
			platform_qemu_virt_gicv2.name = "qemu-virt-gicv2";
			platform_qemu_virt_gicv2.gic_dist = dist;
			platform_qemu_virt_gicv2.gic_redist = 0;
			platform_qemu_virt_gicv2.gic_cpu = cpu;
			PLAT = &platform_qemu_virt_gicv2;
		}
	}
	if (PLAT->gic_cpu == 0 && ((READ_SYSREG(ID_AA64PFR0_EL1) >> 24) & 0xf) == 0) {
		print("platform %s: a GICv3 by its description, but this processor has no GICv3 registers\n", PLAT->name);
	}
	loader_gic_sysregs = PLAT->gic_cpu == 0;
	ram_log_base = PLAT->ram_log, ram_log_size = PLAT->ram_log_size;
	ram_dump_base = PLAT->ram_dump, ram_dump_size = PLAT->ram_dump_size;
	clear_ram_dump();
	print("platform %s, %s\n", PLAT->name, PLAT->gic_cpu != 0 ? "GICv2" : "GICv3");
	pci_probe(PLAT);

	install_exception_hook();
	wdt_arm();

	int have_payload = parse_payload(&pi, 0);
	if (have_payload && platform_sibling(PLAT, pi.cmdline) != PLAT) {
		/* the S9+ or the Note9 (loader_platform=): the S9's description in all but the name, so nothing above changes */
		print("platform %s (loader_platform=), the description of %s with its own name\n",
		    platform_sibling(PLAT, pi.cmdline)->name, PLAT->name);
		PLAT = platform_sibling(PLAT, pi.cmdline);
	}
	exynos_wdt_start(have_payload ? pi.cmdline : "");   /* the Galaxy S9's hardware watchdog, and INFORM3 */
	/*
	 * The Galaxy S9: the payload out of the CP's region, which is then kept from UEFI and XNU. Here, under the watchdog
	 * just armed, so that a hang in it is a restart into recovery; and read again from where it is now.
	 */
	if (PLAT->payload_home != 0 || PLAT->keep_out[0].size != 0) {
		payload_move();
		have_payload = parse_payload(&pi, 0);
	}
	if (have_payload && pi.usb_mode && usb_start()) {
		flush_log();
		usb_command_loop();
	}

	/* One-shot mode: run the kernel that came with the image, then reset. */
	if (have_payload) {
		run_kernel(0, 0);
	}
	reboot();
	return 0;
}
