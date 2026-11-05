/*
 * The Google Tensor G1 (gs101): its blocks, each number with the source named in soc.conf. Compile-tested at most; never booted.
 * Only numbers the sources give; a driver whose numbers are not all here is not built for this SoC (tools/soc-check.py).
 *
 * L  linux@05441d697f7f arch/arm64/boot/dts/exynos/google/gs101.dtsi
 * B  android.googlesource.com/kernel/gs@af03195582e9 arch/arm64/boot/dts/google/gs101.dtsi, gs101-ufs.dtsi, gs101-gpu.dtsi, gs101-drm-dpu.dtsi, gs101-mfc.dtsi
 * P  PixOS-Project/pixelios-kernel@04457e5ddae2 (MIT) soc/google/gs101/soc.json, docs/research/27 and 32, soc/google/gs101/docs/08
 */
#ifndef _SYS_HAKIOS_GS101_SOC_H_
#define _SYS_HAKIOS_GS101_SOC_H_

/* The SoC */
#define EXYNOS_SOC_BASE_PA                    0x10000000UL   /* L */
#define EXYNOS_CHIPID_PA                      0x10000000UL   /* L: google,gs101-otp */
#define EXYNOS_PRODUCT_ID                     0x09845000u    /* L: exynos-chipid.c */
#define EXYNOS_PMU_PA                         0x17460000UL   /* L */

/* Interrupts, timer, watchdog */
#define EXYNOS_GIC_DIST_PA                    0x10400000UL   /* L: GICv3 */
#define EXYNOS_GIC_REDIST_PA                  0x10440000UL   /* L */
#define EXYNOS_MCT_PA                         0x10050000UL   /* L */
#define EXYNOS_WDT_CL0_PA                     0x10060000UL   /* L */
#define EXYNOS_WDT_CL1_PA                     0x10070000UL   /* L */

/* The power processor */
#define EXYNOS_APM_MBOX_PA                    0x17610000UL   /* L */
#define EXYNOS_APM_SRAM_PA                    0x02039000UL   /* L */
#define EXYNOS_ACPM_SRAM_SIZE                 0x40000        /* P */
#define EXYNOS_ACPM_INITDATA                  0xA000         /* L: exynos-acpm.c */
#define EXYNOS_ACPM_FVMAP_OFFSET              0x31000        /* P: fvmap at 0x0206A000 */

/* UFS */
#define EXYNOS_UFS_HCI_PA                     0x14700000UL   /* L */
#define EXYNOS_UFS_VS_PA                      0x14701100UL   /* B */
#define EXYNOS_UFS_UNIPRO_PA                  0x14780000UL   /* B */
#define EXYNOS_UFS_PMA_PA                     0x14704000UL   /* L */
#define EXYNOS_UFS_PMU_PHY_PA                 0x17463EC8UL   /* B: PMU + 0x3EC8 */

/* USB */
#define EXYNOS_USB_DWC3_PA                    0x11110000UL   /* L */
#define EXYNOS_USB_PHY_PA                     0x11100000UL   /* L */

/* Display */
#define EXYNOS_DECON_PA                       0x1C300000UL   /* B */
#define EXYNOS_DSIM0_PA                       0x1C2C0000UL   /* B */

/* GPU */
#define EXYNOS_MALI_PA                        0x1C500000UL   /* B */

/* Video */
#define EXYNOS_MFC_PA                         0x1C8D0000UL   /* B */

#endif /* _SYS_HAKIOS_GS101_SOC_H_ */
