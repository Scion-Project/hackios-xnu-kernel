# Samsung Exynos 9810

The SoC's layer, kept in the common tree: every phone with this SoC shares this one copy (`soc exynos9810` in its `device.conf`,
`#include exynos9810.config` in its defconfig).

| File | What |
|---|---|
| `soc.conf` | what the SoC is |
| `configs/exynos9810.config` | what is true of the SoC whatever phone it is in: the Exynos drivers on, their defaults |
| `include/hakios.exynos9810_soc.h` | the numbers: where each block is, interrupts, PMU and CMU offsets, the USI blocks. Goes to `bsd/sys/` |
| `include/hakios.exynos9810_mem.h` | how the phones use the memory outside XNU's RAM, checked at build time. Goes to `bsd/sys/` |

The drivers themselves are in `drivers/`: they are shared by the Exynos family. A new Exynos: [How the repositories fit](../../docs/porting/repositories.md).
