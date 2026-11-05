/*
 * The Samsung Exynos 9820 (S5E9820): its blocks, each number with the source named in soc.conf. Compile-tested at most; never booted.
 * Only numbers the sources give; a driver whose numbers are not all here is not built for this SoC (tools/soc-check.py).
 *
 * B  LineageOS/android_kernel_samsung_exynos9820@a412cfaf86b9 arch/arm64/boot/dts/exynos/exynos9820.dts
 * M  same tree, exynos9820-mali_common.dtsi and exynos9820-mfc.dtsi
 * D  same tree, drivers/video/fbdev/exynos/dpu20/cal_9820/regs-decon.h
 */
#ifndef _SYS_HAKIOS_EXYNOS9820_SOC_H_
#define _SYS_HAKIOS_EXYNOS9820_SOC_H_

/* The SoC */
#define EXYNOS_SOC_BASE_PA                    0x10000000UL   /* B */
#define EXYNOS_CHIPID_PA                      0x10000000UL   /* B */
#define EXYNOS_PRODUCT_ID                     0xE9820000u    /* B: include/linux/soc/samsung/exynos-soc.h */
#define EXYNOS_PMU_PA                         0x15860000UL   /* B */

/* Interrupts, timer, watchdog */
#define EXYNOS_GIC_DIST_PA                    0x10101000UL   /* B: GIC-400 */
#define EXYNOS_GIC_CPU_PA                     0x10102000UL   /* B */
#define EXYNOS_MCT_PA                         0x10040000UL   /* B */
#define EXYNOS_WDT_CL0_PA                     0x10050000UL   /* B */
#define EXYNOS_WDT_CL1_PA                     0x10060000UL   /* B */

/* The power processor */
#define EXYNOS_APM_MBOX_PA                    0x15900000UL   /* B */
#define EXYNOS_APM_SRAM_PA                    0x02039000UL   /* B */
#define EXYNOS_ACPM_SRAM_SIZE                 0x30000        /* B */
#define EXYNOS_ACPM_SRAM_FIRST                0x3000         /* guess: the 9810's */
#define EXYNOS_ACPM_INITDATA                  0x7000         /* B */
#define EXYNOS_ACPM_FVMAP_OFFSET              0xA400         /* B */

/* UFS */
#define EXYNOS_UFS_HCI_PA                     0x13D60000UL   /* B */
#define EXYNOS_UFS_VS_PA                      0x13D61100UL   /* B */
#define EXYNOS_UFS_UNIPRO_PA                  0x13D50000UL   /* B */
#define EXYNOS_UFS_PMA_PA                     0x13D64000UL   /* B */
#define EXYNOS_UFS_SYSREG_COH_PA              0x13C10700UL   /* B */
#define EXYNOS_UFS_EXY_SYSREG_COH_BITS        0xC00000u      /* B */

/* USB */
#define EXYNOS_USB_DWC3_PA                    0x10C00000UL   /* B */

/* Display */
#define EXYNOS_DECON_PA                       0x19030000UL   /* B */
#define EXYNOS_DPP_PA                         0x19020000UL   /* B */
#define EXYNOS_DPU_DMA_PA                     0x19070000UL   /* B */
#define EXYNOS_DSIM0_PA                       0x19080000UL   /* B */

/* GPU */
#define EXYNOS_MALI_PA                        0x18500000UL   /* M */

/* Video */
#define EXYNOS_MFC_PA                         0x188D0000UL   /* M */

#endif /* _SYS_HAKIOS_EXYNOS9820_SOC_H_ */
