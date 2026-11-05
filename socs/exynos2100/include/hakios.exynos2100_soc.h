/*
 * The Samsung Exynos 2100 (S5E9840): its blocks, each number with the source named in soc.conf. Compile-tested at most; never booted.
 * Only numbers the sources give; a driver whose numbers are not all here is not built for this SoC (tools/soc-check.py).
 *
 * B  GoRhanHee/android_kernel_samsung_exynos2100_o1s@5fdc9b765eb3 arch/arm64/boot/dts/exynos/exynos2100.dts
 * D  same tree, drivers/video/fbdev/exynos/dpu30/cal_2100/regs-decon.h
 */
#ifndef _SYS_HAKIOS_EXYNOS2100_SOC_H_
#define _SYS_HAKIOS_EXYNOS2100_SOC_H_

/* The SoC */
#define EXYNOS_SOC_BASE_PA                    0x10000000UL   /* B */
#define EXYNOS_CHIPID_PA                      0x10000000UL   /* B */
#define EXYNOS_PRODUCT_ID                     0xE9840000u    /* B: include/linux/soc/samsung/exynos-soc.h */
#define EXYNOS_PMU_PA                         0x15860000UL   /* B */

/* Interrupts, timer, watchdog */
#define EXYNOS_GIC_DIST_PA                    0x10200000UL   /* B: GICv3 */
#define EXYNOS_GIC_REDIST_PA                  0x10240000UL   /* B */
#define EXYNOS_MCT_PA                         0x10040000UL   /* B */
#define EXYNOS_WDT_CL0_PA                     0x10050000UL   /* B */
#define EXYNOS_WDT_CL1_PA                     0x10060000UL   /* B */

/* The power processor */
#define EXYNOS_APM_MBOX_PA                    0x15900000UL   /* B */
#define EXYNOS_APM_SRAM_PA                    0x02039000UL   /* B */
#define EXYNOS_ACPM_SRAM_SIZE                 0x41000        /* B */
#define EXYNOS_ACPM_SRAM_FIRST                0x3000         /* guess: the 9810's */
#define EXYNOS_ACPM_INITDATA                  0x8000         /* B */
#define EXYNOS_ACPM_FVMAP_OFFSET              0xF800         /* B */

/* UFS */
#define EXYNOS_UFS_HCI_PA                     0x13100000UL   /* B */
#define EXYNOS_UFS_VS_PA                      0x13101100UL   /* B */
#define EXYNOS_UFS_UNIPRO_PA                  0x13180000UL   /* B */
#define EXYNOS_UFS_PMA_PA                     0x13104000UL   /* B */
#define EXYNOS_UFS_PMU_PHY_PA                 0x15860728UL   /* B: PMU + 0x728 */

/* USB */
#define EXYNOS_USB_DWC3_PA                    0x10E00000UL   /* B */
#define EXYNOS_USB_PHY_PA                     0x10C00000UL   /* B */

/* Display */
#define EXYNOS_DECON_PA                       0x19F00000UL   /* B */
#define EXYNOS_DPP_PA                         0x19CD0000UL   /* B */
#define EXYNOS_DPU_DMA_PA                     0x19CB0000UL   /* B */
#define EXYNOS_DSIM0_PA                       0x19EC0000UL   /* B */

/* GPU */
#define EXYNOS_MALI_PA                        0x18500000UL   /* B */

/* Video */
#define EXYNOS_MFC_PA                         0x186D0000UL   /* B: first of two cores */

#endif /* _SYS_HAKIOS_EXYNOS2100_SOC_H_ */
