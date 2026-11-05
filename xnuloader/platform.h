/* What a device is to the loader: one struct platform per device (platforms/), told apart at boot. */

struct platform {
	const char *name;
	uint64_t io_base, io_size;       /* one window covering the SoC peripherals */
	uint64_t gic_dist, gic_redist;   /* GICv3 */
	uint64_t gic_cpu;                /* not 0: the GIC is a GICv2 (GIC-400), with its CPU interface here and no redistributors */
	uint64_t uart;                   /* PL011, 0 if none */
	int psci_hvc;                    /* the firmware's power interface is called with HVC, not SMC */
	int log_other_cpu;               /* keep notes in the log partition while a kernel starts on another core */
	int cores;                       /* at most this many processor cores, numbered from 0 ... */
	int core_shift;                  /* ... in this bit of MPIDR: the phone numbers them in the second affinity field */
	const uint32_t *core_mpidr;      /* ... or this: core N's MPIDR, for a phone whose cores are in two clusters */
	uint32_t timeout_scale;          /* a kernel on several cores gets this much longer to wait for a lock, 0: as it is */
	uint32_t small_cores;            /* a bit for each of the slower cores, by number: "small-core" in the device tree */
	uint64_t fb_base;                /* the framebuffer the firmware reports (GOP): it tells this device apart */
	uint64_t ram_log, ram_log_size;  /* where the log is kept across a restart when there is no log partition (write_ram_log) */
	uint64_t ram_dump, ram_dump_size; /* ... and the start of the kernel's, in ramoops' dmesg record (write_ram_dump) */
	uint64_t ufs_hc;                 /* the UFS host controller the kernel's own driver may take over (ufs_native=); 0: none */
	uint64_t timer_hz;               /* the timer's frequency where CNTFRQ_EL0 reads 0 (the firmware never set it), 0: none */
	uint64_t recovery_pmu;           /* Samsung: the PMU whose INFORM registers send a restart into recovery (recovery_after_reset) */
	uint32_t pmu_swreset;            /* ... and its SWRESET register's offset: the restart itself (platform_restart), 0: none */
	uint64_t gic_redist_size;        /* the redistributors' region, where it is not 1 MB (GICv4 frames are 256 KB a core) */
	const char *compatible;          /* told apart by this in the device tree's root "compatible" instead (platform_from_fdt) */
	uint64_t exynos_wdt;             /* Samsung Exynos: the cluster watchdog armed for the run (exynos_wdt_start), 0: none */
	uint64_t exynos_wdt_other;       /* ... the other cluster's, stopped */
	uint32_t exynos_wdt_pmu_bit;     /* ... its bit in recovery_pmu's MASK_WDT_RESET_REQUEST and DISABLE_WDT */
	uint32_t parked_cores;           /* a bit for each core the boot loader used for itself: the kernel's only if asked (core1=1) */
	uint8_t affinity_unreliable;     /* PSCI AFFINITY_INFO says "on" of cores that are off: CPU_ON's answer is what counts */
	uint64_t payload_home;           /* not 0: the payload is moved here (or a 64 MiB step above) from PAYLOAD_BASE at the start */
	struct { uint64_t base, size; const char *what; } keep_out[2];  /* RAM neither UEFI nor XNU may have: reserved at the start */
	const uint64_t (*extra_ram)[2];  /* where free RAM beside XNU's window is XNU's too ([lo, hi) each, to a {0, 0}): extra_ram_claim */
	uint8_t extra_ram_off;           /* ... only with extra_ram=1 among the kernel's arguments (otherwise: unless extra_ram=0) */
	const char *data_part;           /* a port with no darwin: the one partition, by name, ufs_write=1 may write (block_is_data_part) */
	const struct platform *sibling_of; /* not NULL: told apart from this one only by loader_platform= (platform_sibling) */
};
