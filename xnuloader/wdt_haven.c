/* Qualcomm's Haven virtual watchdog, by SMC. */

/* used by Linux drivers/virt/haven/hh_virt_wdt.c). Armed for the whole stay */
/* in the loader and the kernel: any hang ends in a reset without a human.   */

#define VIRT_WDT_CONTROL   0x86000005u
#define VIRT_WDT_STATUS    0x86000006u
#define VIRT_WDT_PET       0x86000007u
#define VIRT_WDT_SET_TIME  0x86000008u
#define WDT_BITE_MS        30000u

static const struct platform *PLAT;
static int wdt_armed;

static uint64_t smc_x3;             /* the fourth argument register, for the one call that has one */

static uint64_t
smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2)
{
	register uint64_t x0 __asm__("x0") = id;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = smc_x3;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	if (r1 != NULL) {
		*r1 = x1;
	}
	if (r2 != NULL) {
		*r2 = x2;
	}
	return x0;
}

static void
wdt_pet(void)
{
	if (exynos_wdt_owns()) {
		exynos_wdt_feed();      /* (the Galaxy S9's: see above) */
		return;
	}
	if (wdt_armed) {
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

/* Change the time to the bite, and start counting again. The hypervisor takes 16 bits of milliseconds. */
static void
wdt_set_ms(uint64_t ms)
{
	if (ms > 65000) {
		ms = 65000;
	}
	if (exynos_wdt_owns()) {
		exynos_wdt_set_ms(ms);  /* (the Galaxy S9's: see above) */
		return;
	}
	if (wdt_armed) {
		smc_call(VIRT_WDT_SET_TIME, ms, ms, NULL, NULL);
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

/*
 * core0=1, the kernel has asked (LOADER_CORE_WANTED): this core into the kernel, as the firmware's CPU_ON would start it
 * (core0_jump: caches emptied, MMU and caches off, interrupts masked, x0 = 0, at the kernel's entry), and back here when
 * the kernel gives it back
 * (core0_return_stub). What the kernel finds of the loader's use of this core is quieted first, and put back after:
 * the architected timers (UEFI's tick) and this core's banked half of a GICv2 (CPU interface, SGI/PPI enables).
 */
static void
core0_into_kernel(void)
{
	uint64_t t0 = READ_SYSREG(CNTVCT_EL0), cntp_ctl, cntp_cval, cntv_ctl, cntv_cval;
	uint32_t gicc_ctlr = 0, gicc_pmr = 0, gicc_bpr = 0, gicd_en0 = 0;
	uint64_t gicc = PLAT->gic_cpu, gicd = PLAT->gic_dist;

	print("core0: the kernel takes this core (%x) at %x; until it gives it back the watchdog is the kernel's to feed, "
	    "and a silent kernel is the watchdog's to catch (a restart into BOOT), not the loader's\n", core_box->mpidr,
	    core_box->entry);
	wdt_pet();
	cntp_ctl = READ_SYSREG(CNTP_CTL_EL0);
	cntp_cval = READ_SYSREG(CNTP_CVAL_EL0);
	cntv_ctl = READ_SYSREG(CNTV_CTL_EL0);
	cntv_cval = READ_SYSREG(CNTV_CVAL_EL0);
	__asm__ volatile ("msr cntp_ctl_el0, %0\nmsr cntv_ctl_el0, %0\nisb" : : "r" ((uint64_t)0));
	if (gicc != 0) {
		/* GICv2: this core's CPU interface off, its SGIs and PPIs disabled and nothing left pending */
		gicc_ctlr = *(volatile uint32_t *)(uintptr_t)(gicc + 0x0);
		gicc_pmr = *(volatile uint32_t *)(uintptr_t)(gicc + 0x4);
		gicc_bpr = *(volatile uint32_t *)(uintptr_t)(gicc + 0x8);
		gicd_en0 = *(volatile uint32_t *)(uintptr_t)(gicd + 0x100);
		*(volatile uint32_t *)(uintptr_t)(gicc + 0x0) = 0;
		*(volatile uint32_t *)(uintptr_t)(gicd + 0x180) = 0xFFFFFFFFu;
		*(volatile uint32_t *)(uintptr_t)(gicd + 0x280) = 0xFFFFFFFFu;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	if (core0_save_ctx() == 0) {
		uint64_t sp_now = (uint64_t)(uintptr_t)__builtin_frame_address(0);

		/* what this core reads with its caches off on the way back: the stub's state, and this stack */
		clean_dcache((uint64_t)(uintptr_t)core0_ctx, (uint64_t)(uintptr_t)core0_ctx + sizeof(core0_ctx));
		clean_dcache(sp_now - 0x2000, sp_now + 0x2000);
		uint64_t entry = core_box->entry;

		core_box->state = LOADER_CORE_GONE;
		__asm__ volatile ("dsb sy" ::: "memory");
		core0_jump(entry);
	}
	/*
	 * back: the kernel has ended, or let the core go. It may have given the core back from the handler of its stop
	 * signal (an SGI acknowledged and never ended): nothing of the kernel's stays active or pending in this core's half.
	 */
	if (gicc != 0) {
		*(volatile uint32_t *)(uintptr_t)(gicd + 0x180) = 0xFFFFFFFFu;
		*(volatile uint32_t *)(uintptr_t)(gicd + 0x280) = 0xFFFFFFFFu;
		*(volatile uint32_t *)(uintptr_t)(gicd + 0x380) = 0xFFFFFFFFu;
		*(volatile uint32_t *)(uintptr_t)(gicc + 0xD0) = 0;    /* GICC_APR0: no active priority left */
		*(volatile uint32_t *)(uintptr_t)(gicd + 0x100) = gicd_en0;
		*(volatile uint32_t *)(uintptr_t)(gicc + 0x4) = gicc_pmr;
		*(volatile uint32_t *)(uintptr_t)(gicc + 0x8) = gicc_bpr;
		*(volatile uint32_t *)(uintptr_t)(gicc + 0x0) = gicc_ctlr;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	__asm__ volatile ("msr cntp_cval_el0, %0\nmsr cntv_cval_el0, %1\nisb" : : "r" (cntp_cval), "r" (cntv_cval));
	__asm__ volatile ("msr cntp_ctl_el0, %0\nmsr cntv_ctl_el0, %1\nisb" : : "r" (cntp_ctl), "r" (cntv_ctl));
	core_box->state = LOADER_CORE_BACK;
	__asm__ volatile ("dsb sy" ::: "memory");
	print("core0: back in the loader after %d s with the kernel\n", seconds_since(t0));
}

static void
wdt_arm(void)
{
	uint64_t ctl = 0, since = 0;

	if (PLAT != &platform_redwood) {
		return; /* SMC is not routed anywhere useful on QEMU virt */
	}
	uint64_t st = smc_call(VIRT_WDT_STATUS, 0, 0, &ctl, &since);
	uint64_t r_time = smc_call(VIRT_WDT_SET_TIME, WDT_BITE_MS, WDT_BITE_MS, NULL, NULL);
	uint64_t r_ctl = smc_call(VIRT_WDT_CONTROL, 3, 0, NULL, NULL);
	uint64_t r_pet = smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	print("watchdog: status ret=%x ctl=%x since_pet=%x; set_time=%x enable=%x pet=%x\n", st, ctl, since, r_time,
	    r_ctl, r_pet);
	wdt_armed = (r_pet == 0);
}
