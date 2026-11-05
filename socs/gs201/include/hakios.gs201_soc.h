/*
 * The Google Tensor G2 (gs201): its blocks, each number with the source named in soc.conf. Compile-tested at most; never booted.
 * Only numbers the sources give; a driver whose numbers are not all here is not built for this SoC (tools/soc-check.py).
 *
 * B  android.googlesource.com/kernel/devices/google/gs201@05c125c7e224 dts/gs201.dtsi, gs201-ufs.dtsi, gs201-gpu.dtsi, gs201-drm-dpu.dtsi, gs201-mfc.dtsi
 * L  linux@05441d697f7f the gs101 drivers, the family only: mainline has no gs201
 */
#ifndef _SYS_HAKIOS_GS201_SOC_H_
#define _SYS_HAKIOS_GS201_SOC_H_

/* The SoC */
#define EXYNOS_SOC_BASE_PA                    0x10000000UL   /* B */
#define EXYNOS_CHIPID_PA                      0x10000000UL   /* B */
#define EXYNOS_PMU_PA                         0x18060000UL   /* B */

/* Interrupts, timer, watchdog */
#define EXYNOS_GIC_DIST_PA                    0x10400000UL   /* B: GICv3 */
#define EXYNOS_GIC_REDIST_PA                  0x10440000UL   /* B */
#define EXYNOS_MCT_PA                         0x10050000UL   /* B */
#define EXYNOS_WDT_CL0_PA                     0x10060000UL   /* B */

/* The power processor */
#define EXYNOS_APM_MBOX_PA                    0x18210000UL   /* B */
#define EXYNOS_APM_SRAM_PA                    0x18500000UL   /* B */
#define EXYNOS_ACPM_SRAM_SIZE                 0x48000        /* B */
#define EXYNOS_ACPM_INITDATA                  0xA000         /* B */

/* UFS */
#define EXYNOS_UFS_HCI_PA                     0x14700000UL   /* B */
#define EXYNOS_UFS_VS_PA                      0x14701100UL   /* B */
#define EXYNOS_UFS_UNIPRO_PA                  0x14780000UL   /* B */
#define EXYNOS_UFS_PMA_PA                     0x14704000UL   /* B */
#define EXYNOS_UFS_PMU_PHY_PA                 0x18063EC8UL   /* B: PMU + 0x3EC8 */

/* USB */
#define EXYNOS_USB_DWC3_PA                    0x11210000UL   /* B */
#define EXYNOS_USB_PHY_PA                     0x11200000UL   /* B */

/* Display */
#define EXYNOS_DECON_PA                       0x1C240000UL   /* B */
#define EXYNOS_DSIM0_PA                       0x1C2C0000UL   /* B */

/* GPU */
#define EXYNOS_MALI_PA                        0x28000000UL   /* B: 0x480000 long, CSF doorbells */

/* Video */
#define EXYNOS_MFC_PA                         0x1C8D0000UL   /* B */

#endif /* _SYS_HAKIOS_GS201_SOC_H_ */
