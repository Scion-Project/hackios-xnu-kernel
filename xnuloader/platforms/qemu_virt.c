static const struct platform platform_qemu_virt = {
	.name = "qemu-virt", .io_base = 0x08000000, .io_size = 0x08000000,
	.gic_dist = 0x08000000, .gic_redist = 0x080A0000, .uart = 0x09000000, .psci_hvc = 1,
	.cores = 8, .core_shift = 0,
	/*
	 * QEMU's emulated cores stop each other all the time (every flush of a page from all the
	 * TLBs is a rendezvous of all its CPU threads), and the kernel's patience with a spin lock
	 * is that of real hardware: an eighth of a second by a 24 MHz clock, a twentieth here.
	 */
	.timeout_scale = 20,
	/*
	 * A test of the phone's two banks (extra_ram=1 among the kernel's arguments): with the payload at 0xF0000000 in the
	 * middle of QEMU's RAM (from 0x40000000), the window is the block above it and the RAM below, past 128 MB left to
	 * UEFI, is the kernel's beside it, as the S9's low bank is.
	 */
	.extra_ram = (const uint64_t[][2]){ { 0x48000000, 0xF0000000 }, { 0, 0 } },
	.extra_ram_off = 1,
};
