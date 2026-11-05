/* Made by tools/soc-runtime.py from the tables in socs/. Do not edit. */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/hakios.soc_runtime.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

static const struct hakios_soc soc_exynos2100 = {
	.name = "exynos2100",
	.product_id = 0xE9840000u,
	.acpm_fvmap_offset = (uint32_t)(0xF800),
	.acpm_initdata = (uint32_t)(0x8000),
	.acpm_sram_first = (uint32_t)(0x3000),
	.acpm_sram_size = (uint32_t)(0x41000),
	.apm_mbox_pa = (uint32_t)(0x15900000UL),
	.apm_sram_pa = (uint32_t)(0x02039000UL),
};

static const struct hakios_soc soc_exynos850 = {
	.name = "exynos850",
	.product_id = 0xE3830000u,
};

static const struct hakios_soc soc_exynos8895 = {
	.name = "exynos8895",
	.product_id = 0xE8895000u,
	.ufs_exy_sysreg_coh_bits = (uint32_t)(0x300u),
	.ufs_hci_pa = (uint32_t)(0x11120000UL),
	.ufs_pma_pa = (uint32_t)(0x11124000UL),
	.ufs_pmu_phy_pa = (uint32_t)(0x16480724UL),
	.ufs_sysreg_coh_pa = (uint32_t)(0x11020700UL),
	.ufs_unipro_pa = (uint32_t)(0x11110000UL),
	.ufs_vs_pa = (uint32_t)(0x11121100UL),
};

static const struct hakios_soc soc_exynos9810 = {
	.name = "exynos9810",
	.product_id = 0xE9810000u,
	.acpm_fvmap_offset = (uint32_t)(0x6700),
	.acpm_initdata = (uint32_t)(0x7F00),
	.acpm_sram_first = (uint32_t)(0x3000),
	.acpm_sram_size = (uint32_t)(0x30000),
	.apm_mbox_pa = (uint32_t)(0x14100000UL),
	.apm_sram_pa = (uint32_t)(0x02039000UL),
	.decon_pa = (uint32_t)(0x16030000UL),
	.dpp_pa = (uint32_t)(0x16020000UL),
	.dpu_dma_pa = (uint32_t)(0x16070000UL),
	.ufs_exy_sysreg_coh_bits = (uint32_t)(0x300u),
	.ufs_hci_pa = (uint32_t)(0x11120000UL),
	.ufs_pma_pa = (uint32_t)(0x11124000UL),
	.ufs_pmu_phy_pa = (uint32_t)(0x14060724UL),
	.ufs_sysreg_coh_pa = (uint32_t)(0x11010700UL),
	.ufs_unipro_pa = (uint32_t)(0x11110000UL),
	.ufs_vs_pa = (uint32_t)(0x11121100UL),
};

static const struct hakios_soc soc_exynos9820 = {
	.name = "exynos9820",
	.product_id = 0xE9820000u,
	.acpm_fvmap_offset = (uint32_t)(0xA400),
	.acpm_initdata = (uint32_t)(0x7000),
	.acpm_sram_first = (uint32_t)(0x3000),
	.acpm_sram_size = (uint32_t)(0x30000),
	.apm_mbox_pa = (uint32_t)(0x15900000UL),
	.apm_sram_pa = (uint32_t)(0x02039000UL),
	.decon_pa = (uint32_t)(0x19030000UL),
	.dpp_pa = (uint32_t)(0x19020000UL),
	.dpu_dma_pa = (uint32_t)(0x19070000UL),
};

static const struct hakios_soc soc_exynos990 = {
	.name = "exynos990",
	.product_id = 0xE9830000u,
	.acpm_fvmap_offset = (uint32_t)(0xBC00),
	.acpm_initdata = (uint32_t)(0x7000),
	.acpm_sram_first = (uint32_t)(0x3000),
	.acpm_sram_size = (uint32_t)(0x38000),
	.apm_mbox_pa = (uint32_t)(0x15900000UL),
	.apm_sram_pa = (uint32_t)(0x02039000UL),
	.decon_pa = (uint32_t)(0x19050000UL),
	.dpp_pa = (uint32_t)(0x19040000UL),
	.dpu_dma_pa = (uint32_t)(0x19090000UL),
};

static const struct hakios_soc soc_gs101 = {
	.name = "gs101",
	.product_id = 0x09845000u,
};

static const struct hakios_soc *const socs[] = {
	&soc_exynos2100,
	&soc_exynos850,
	&soc_exynos8895,
	&soc_exynos9810,
	&soc_exynos9820,
	&soc_exynos990,
	&soc_gs101,
};

static const struct hakios_soc soc_none = { .name = "none" };

const struct hakios_soc *hakios_soc_table;

/* The chip id block is at 0x10000000 on every SoC with a table: Linux's exynos-chipid.c, PRO_ID, [31:12] the product */
const struct hakios_soc *
hakios_soc_select(void)
{
	uint32_t id = *(volatile uint32_t *)ml_io_map(0x10000000, 0x1000) & 0xFFFFF000u;

	for (unsigned i = 0; i < sizeof(socs) / sizeof(socs[0]); i++) {
		if (socs[i]->product_id == id) {
			hakios_soc_table = socs[i];
			return hakios_soc_table;
		}
	}
	printf("hakios_soc: chip id 0x%08x is in no table: no SoC driver starts\n", id);
	hakios_soc_table = &soc_none;
	return hakios_soc_table;
}

int
hakios_soc_is_9810(void)
{
	const struct hakios_soc *soc = hakios_soc_table != 0 ? hakios_soc_table : hakios_soc_select();

	return soc == &soc_exynos9810;
}
