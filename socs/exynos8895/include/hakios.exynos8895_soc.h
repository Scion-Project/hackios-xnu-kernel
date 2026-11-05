/*
 * The Samsung Exynos 8895 (S5E8895): its blocks, each number with the source named in soc.conf. Compile-tested at most; never booted.
 * Only numbers the sources give; a driver whose numbers are not all here is not built for this SoC (tools/soc-check.py).
 *
 * L  linux@05441d697f7f arch/arm64/boot/dts/exynos/exynos8895.dtsi
 * B  acroreiser/android_kernel_samsung_universal8895@0288020e9751 arch/arm64/boot/dts/exynos/exynos8895.dtsi
 * D  same tree, drivers/video/fbdev/exynos/dpu/regs-decon.h
 */
#ifndef _SYS_HAKIOS_EXYNOS8895_SOC_H_
#define _SYS_HAKIOS_EXYNOS8895_SOC_H_

/* The SoC */
#define EXYNOS_SOC_BASE_PA                    0x10000000UL   /* L */
#define EXYNOS_CHIPID_PA                      0x10000000UL   /* L */
#define EXYNOS_PRODUCT_ID                     0xE8895000u    /* L: exynos-chipid.c */
#define EXYNOS_PMU_PA                         0x16480000UL   /* L */

/* Interrupts, timer, watchdog */
#define EXYNOS_GIC_DIST_PA                    0x10201000UL   /* L: GIC-400 */
#define EXYNOS_GIC_CPU_PA                     0x10202000UL   /* L */
#define EXYNOS_MCT_PA                         0x10040000UL   /* L */
#define EXYNOS_WDT_CL0_PA                     0x10060000UL   /* B */

/* The power processor */
#define EXYNOS_APM_MBOX_PA                    0x16440000UL   /* B */
#define EXYNOS_APM_SRAM_PA                    0x16500000UL   /* B */
#define EXYNOS_ACPM_SRAM_SIZE                 0xB400         /* B */
#define EXYNOS_ACPM_SRAM_FIRST                0              /* guess: nothing known to fault */
#define EXYNOS_ACPM_INITDATA                  0x2850         /* B */

/* UFS */
#define EXYNOS_UFS_HCI_PA                     0x11120000UL   /* B */
#define EXYNOS_UFS_VS_PA                      0x11121100UL   /* B */
#define EXYNOS_UFS_UNIPRO_PA                  0x11110000UL   /* B */
#define EXYNOS_UFS_PMA_PA                     0x11124000UL   /* B */
#define EXYNOS_UFS_PMU_PHY_PA                 0x16480724UL   /* B */
#define EXYNOS_UFS_SYSREG_COH_PA              0x11020700UL   /* B */
#define EXYNOS_UFS_EXY_SYSREG_COH_BITS        0x300u         /* B */

/* USB */
#define EXYNOS_USB_DWC3_PA                    0x10C00000UL   /* B */

/* Display */
#define EXYNOS_DECON_PA                       0x12860000UL   /* B */
#define EXYNOS_DPP_PA                         0x128B0000UL   /* B */
#define EXYNOS_DPU_DMA_PA                     0x12850000UL   /* B */
#define EXYNOS_DSIM0_PA                       0x12870000UL   /* B */

/* GPU */
#define EXYNOS_MALI_PA                        0x13900000UL   /* B */

/* Video */
#define EXYNOS_MFC_PA                         0x13CE0000UL   /* B */

#endif /* _SYS_HAKIOS_EXYNOS8895_SOC_H_ */
