/*
 * The Samsung Exynos 850 (S5E3830): its blocks, each number with the source named in soc.conf. Compile-tested at most; never booted.
 * Only numbers the sources give; a driver whose numbers are not all here is not built for this SoC (tools/soc-check.py).
 *
 * L  linux@05441d697f7f arch/arm64/boot/dts/exynos/exynos850.dtsi
 */
#ifndef _SYS_HAKIOS_EXYNOS850_SOC_H_
#define _SYS_HAKIOS_EXYNOS850_SOC_H_

/* The SoC */
#define EXYNOS_SOC_BASE_PA                    0x10000000UL   /* L */
#define EXYNOS_CHIPID_PA                      0x10000000UL   /* L */
#define EXYNOS_PRODUCT_ID                     0xE3830000u    /* L: exynos-chipid.c */
#define EXYNOS_PMU_PA                         0x11860000UL   /* L */

/* Interrupts, timer, watchdog */
#define EXYNOS_GIC_DIST_PA                    0x12A01000UL   /* L: GIC-400 */
#define EXYNOS_GIC_CPU_PA                     0x12A02000UL   /* L */
#define EXYNOS_MCT_PA                         0x10040000UL   /* L */
#define EXYNOS_WDT_CL0_PA                     0x10050000UL   /* L */
#define EXYNOS_WDT_CL1_PA                     0x10060000UL   /* L */

/* The power processor */
#define EXYNOS_APM_MBOX_PA                    0x11900000UL   /* L */
#define EXYNOS_APM_SRAM_PA                    0x02039000UL   /* L */

/* USB */
#define EXYNOS_USB_DWC3_PA                    0x13600000UL   /* L */
#define EXYNOS_USB_PHY_PA                     0x135D0000UL   /* L */

/* GPU */
#define EXYNOS_MALI_PA                        0x11500000UL   /* L */

#endif /* _SYS_HAKIOS_EXYNOS850_SOC_H_ */
