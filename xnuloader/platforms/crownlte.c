static const struct platform platform_crownlte = {
	.name = "crownlte", .io_base = 0x10000000, .io_size = 0x10000000,
	.gic_dist = 0x10101000, .gic_cpu = 0x10102000, .uart = 0,
	.cores = 8, .core_mpidr = (const uint32_t[]){ 0x000, 0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103 },
	.small_cores = 0x0f, .timer_hz = 26000000, .fb_base = 0xCC000000,
	.ram_log = 0xFED14000, .ram_log_size = 0x4000, .ram_dump = 0xFED10000, .ram_dump_size = 0x4000,
	.log_other_cpu = 1, .recovery_pmu = 0x14060000, .pmu_swreset = 0x400, .data_part = "USERDATA",
	.exynos_wdt = 0x10060000, .exynos_wdt_other = 0x10050000, .exynos_wdt_pmu_bit = 23,
	.parked_cores = 0x2, .affinity_unreliable = 1,
	.payload_home = 0xD0000000,
	.keep_out = { { 0xF0000000, 0x0B200000, "CP: modem_if, cp_ram_logging" }, { 0xFB200000, 0x02800000, "ABOX: abox_rmem" } },
	.extra_ram = exynos9810_extra_ram, .extra_ram_off = 1,   /* (seen on the S9 only: on this phone with extra_ram=1) */
	.sibling_of = &platform_starlte,
};
