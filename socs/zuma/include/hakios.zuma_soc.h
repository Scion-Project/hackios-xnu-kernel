/*
 * The Google Tensor G3 (zuma): its blocks, each number with the source named in soc.conf. Compile-tested at most; never booted.
 * Only numbers the sources give; a driver whose numbers are not all here is not built for this SoC (tools/soc-check.py).
 *
 * B  android.googlesource.com/kernel/devices/google/zuma@42395e8adcb8 dts/zuma.dtsi, zuma-ufs.dtsi, zuma-gpu.dtsi, zuma-drm-dpu.dtsi, zuma-usb.dtsi, zuma-mfc.dtsi
 * L  linux@05441d697f7f the gs101 drivers, the family only: mainline has no such SoC
 */
#ifndef _SYS_HAKIOS_ZUMA_SOC_H_
#define _SYS_HAKIOS_ZUMA_SOC_H_

/* The SoC */
#define EXYNOS_SOC_BASE_PA                    0x10000000UL   /* B */
#define EXYNOS_CHIPID_PA                      0x10000000UL   /* B */
#define EXYNOS_PMU_PA                         0x15460000UL   /* B */

/* Interrupts, timer, watchdog */
#define EXYNOS_GIC_DIST_PA                    0x10400000UL   /* B: GICv3 */
#define EXYNOS_GIC_REDIST_PA                  0x10440000UL   /* B */
#define EXYNOS_MCT_PA                         0x100D0000UL   /* B: mct-v3 */
#define EXYNOS_WDT_CL0_PA                     0x10060000UL   /* B */

/* The power processor */
#define EXYNOS_APM_MBOX_PA                    0x15110000UL   /* B */
#define EXYNOS_APM_SRAM_PA                    0x15700000UL   /* B */
#define EXYNOS_ACPM_SRAM_SIZE                 0x58000        /* B */
#define EXYNOS_ACPM_INITDATA                  0xA000         /* B */

/* UFS */
#define EXYNOS_UFS_HCI_PA                     0x13200000UL   /* B */
#define EXYNOS_UFS_VS_PA                      0x13201100UL   /* B */
#define EXYNOS_UFS_UNIPRO_PA                  0x13280000UL   /* B */
#define EXYNOS_UFS_PMA_PA                     0x13204000UL   /* B */
#define EXYNOS_UFS_PMU_PHY_PA                 0x15463EC0UL   /* B: PMU + 0x3EC0 */

/* USB */
#define EXYNOS_USB_DWC3_PA                    0x11210000UL   /* B */

/* Display */
#define EXYNOS_DECON_PA                       0x19470000UL   /* B */
#define EXYNOS_DSIM0_PA                       0x19440000UL   /* B */

/* GPU */
#define EXYNOS_MALI_PA                        0x1F000000UL   /* B: 0x480000 long, CSF doorbells */

/* Video */
#define EXYNOS_MFC_PA                         0x1A4D0000UL   /* B */

#endif /* _SYS_HAKIOS_ZUMA_SOC_H_ */
