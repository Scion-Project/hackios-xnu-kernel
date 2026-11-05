/*
 * The Samsung Exynos 990 (S5E9830): its blocks, each number with the source named in soc.conf. Compile-tested at most; never booted.
 * Only numbers the sources give; a driver whose numbers are not all here is not built for this SoC (tools/soc-check.py).
 *
 * L  linux@05441d697f7f arch/arm64/boot/dts/exynos/exynos990.dtsi
 * B  GoRhanHee/android_kernel_samsung_exynos990_x1s@458e649f59c3 arch/arm64/boot/dts/exynos/exynos9830.dts
 * D  same tree, drivers/video/fbdev/exynos/dpu30/cal_9830/regs-decon.h
 */
#ifndef _SYS_HAKIOS_EXYNOS990_SOC_H_
#define _SYS_HAKIOS_EXYNOS990_SOC_H_

/* The SoC */
#define EXYNOS_SOC_BASE_PA                    0x10000000UL   /* L */
#define EXYNOS_CHIPID_PA                      0x10000000UL   /* L */
#define EXYNOS_PRODUCT_ID                     0xE9830000u    /* L: exynos-chipid.c */
#define EXYNOS_PMU_PA                         0x15860000UL   /* L */

/* Interrupts, timer, watchdog */
#define EXYNOS_GIC_DIST_PA                    0x10101000UL   /* L: GIC-400 */
#define EXYNOS_GIC_CPU_PA                     0x10102000UL   /* L */
#define EXYNOS_MCT_PA                         0x10040000UL   /* L */
#define EXYNOS_WDT_CL0_PA                     0x10050000UL   /* L */
#define EXYNOS_WDT_CL1_PA                     0x10060000UL   /* L */

/* The power processor */
#define EXYNOS_APM_MBOX_PA                    0x15900000UL   /* B */
#define EXYNOS_APM_SRAM_PA                    0x02039000UL   /* B */
#define EXYNOS_ACPM_SRAM_SIZE                 0x38000        /* B */
#define EXYNOS_ACPM_SRAM_FIRST                0x3000         /* guess: the 9810's */
#define EXYNOS_ACPM_INITDATA                  0x7000         /* B */
#define EXYNOS_ACPM_FVMAP_OFFSET              0xBC00         /* B */

/* UFS */
#define EXYNOS_UFS_HCI_PA                     0x13100000UL   /* B */
#define EXYNOS_UFS_VS_PA                      0x13101100UL   /* B */
#define EXYNOS_UFS_UNIPRO_PA                  0x13180000UL   /* B */
#define EXYNOS_UFS_PMA_PA                     0x13104000UL   /* B */
#define EXYNOS_UFS_SYSREG_COH_PA              0x13020700UL   /* B */
#define EXYNOS_UFS_EXY_SYSREG_COH_BITS        0xC00000u      /* B */

/* USB */
#define EXYNOS_USB_DWC3_PA                    0x10E00000UL   /* L */
#define EXYNOS_USB_PHY_PA                     0x10C00000UL   /* L */

/* Display */
#define EXYNOS_DECON_PA                       0x19050000UL   /* B */
#define EXYNOS_DPP_PA                         0x19040000UL   /* B */
#define EXYNOS_DPU_DMA_PA                     0x19090000UL   /* B */
#define EXYNOS_DSIM0_PA                       0x190C0000UL   /* B */

/* GPU */
#define EXYNOS_MALI_PA                        0x18500000UL   /* B */

/* Video */
#define EXYNOS_MFC_PA                         0x186D0000UL   /* B */

#endif /* _SYS_HAKIOS_EXYNOS990_SOC_H_ */
