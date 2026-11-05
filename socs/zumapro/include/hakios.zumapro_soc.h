/*
 * The Google Tensor G4 (zumapro): its blocks, each number with the source named in soc.conf. Compile-tested at most; never booted.
 * Only numbers the sources give; a driver whose numbers are not all here is not built for this SoC (tools/soc-check.py).
 *
 * B  android.googlesource.com/kernel/devices/google/zumapro@8831630eee2a dts/zumapro.dtsi, zumapro-usb.dtsi; zuma-ufs.dtsi shape for UFS in the zuma tree
 * L  linux@05441d697f7f the gs101 drivers, the family only: mainline has no such SoC
 */
#ifndef _SYS_HAKIOS_ZUMAPRO_SOC_H_
#define _SYS_HAKIOS_ZUMAPRO_SOC_H_

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

#endif /* _SYS_HAKIOS_ZUMAPRO_SOC_H_ */
