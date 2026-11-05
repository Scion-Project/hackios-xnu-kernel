/*
 * The numbers of the Exynos this kernel is built for: EXYNOS_* from socs/<soc>/include/hakios.<soc>_soc.h, chosen by CONFIG_SOC
 * (tools/hakios-config.py writes HAKIOS_SOC_HEADER into hakios.drivers.h; xnu/apply.sh lays the header into sys/).
 *
 * Exynos drivers are also built into kernels for other SoCs, where they check the SoC's base at run time and do nothing;
 * those kernels take the 9810's numbers to compile.
 */
#ifndef _SYS_HAKIOS_EXYNOS_SOC_H_
#define _SYS_HAKIOS_EXYNOS_SOC_H_

#include "../dev/hakios.drivers.h"

#ifdef HAKIOS_SOC_HEADER
#include HAKIOS_SOC_HEADER
#else
#include <sys/hakios.exynos9810_soc.h>
#endif

#endif /* _SYS_HAKIOS_EXYNOS_SOC_H_ */
