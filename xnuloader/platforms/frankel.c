static const struct platform platform_frankel = {
	/*
	 * Google Pixel 10 (frankel), Tensor G5 ("laguna", lga), bare metal. There is no UEFI for this phone: Google's boot
	 * loader (ABL) starts the SoC's boot shim (socs/lga/shim), which offers the few UEFI services this loader uses
	 * over the boot loader's device tree, and stays at EL2 to pass PSCI on to EL3. Its framebuffer has no fixed address
	 * either (the boot loader's own), so the phone is told apart by the root "compatible" of that device tree.
	 * Every value is from the mainline device tree (arch/arm64/boot/dts/google/lga.dtsi, Linux 7.4, GPL-2.0 OR
	 * BSD-3-Clause) and Google's kernel for the phone (private/devices/google/lga/dts/lga-base.dtsi, -rmem.dtsi);
	 * devices/frankel/README.md has each one's source.
	 */
	.name = "frankel", .compatible = "google,lga-frankel",
	/* the SoC's registers from the GIC (0x05880000) to the UFS host (0x3C400000) and the PWM (0x3BD90000) */
	.io_base = 0x04000000, .io_size = 0x3C000000,
	/* a GICv3 ("arm,gic-v3"): distributor 0x05880000, redistributors 0x05900000 + 2 MB, GICv4 frames of 256 KB */
	.gic_dist = 0x05880000, .gic_redist = 0x05900000, .gic_redist_size = 0x200000,
	/* the console UART (0x0DB62000) is a DesignWare 16550, not a PL011, and reaches the USB-C port only after `fastboot oem uart enable` */
	.uart = 0,
	/* PSCI by HVC: to the shim at EL2, which makes the SMC and starts a core at EL1 where the firmware would start it at EL2 */
	.psci_hvc = 1,
	/*
	 * Each core in an affinity field of its own (MPIDR 0x000-0x700, Aff1 = the core): 0-1 Cortex-A520, 2-6 Cortex-A725,
	 * 7 Cortex-X4. All ARMv9.2: any one of them may be the boot core.
	 */
	.cores = 8, .core_shift = 8,
	.small_cores = 0x03,
	/*
	 * Linux's ramoops (reserved-memory ramoops@95200000, 4 MB: 2 MB console, then 2 MB pmsg), its pmsg zone, which the
	 * boot loader leaves alone across a warm restart (the region is reserved "for access by the ABL following a
	 * crash-reset"): Android reads it back as /sys/fs/pstore/pmsg-ramoops-0.
	 */
	.ram_log = 0x95400000, .ram_log_size = 0x10000,
	/* CNTFRQ_EL0: set by the firmware (no clock-frequency in the timer's node) */
	.timer_hz = 0,
	/* the framebuffer is the boot loader's, wherever it is: not a way to tell this phone apart */
	.fb_base = 0,
};
