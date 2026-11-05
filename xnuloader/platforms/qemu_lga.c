/*
 * Not a phone: QEMU virt as close to the Pixel 10 as it goes (socs/lga/shim/qemu-test.sh): EL2 (virtualization=on) for
 * the boot shim, which starts this loader without UEFI and passes PSCI on; a GICv4 (gic-version=4: 256 KB redistributor
 * frames, 2 MB of them for 8 cores); eight cores, two of them small. QEMU numbers its cores in Aff0 whatever -smp says,
 * so the core numbering is QEMU's (core_shift 0); the Pixel's, Aff1, is redwood's. Told apart by the root "compatible"
 * of the test's device tree.
 */
static const struct platform platform_qemu_lga = {
	.name = "qemu-lga", .compatible = "scion,qemu-lga",
	.io_base = 0x08000000, .io_size = 0x08000000,
	.gic_dist = 0x08000000, .gic_redist = 0x080A0000, .gic_redist_size = 0x200000, .uart = 0x09000000, .psci_hvc = 1,
	.cores = 8, .core_shift = 0, .small_cores = 0x03,
	.timeout_scale = 20,
};
