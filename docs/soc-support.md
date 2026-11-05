# SoCs other than the Exynos 9810

**Only the Exynos 9810 has ever booted.** Everything else here is compile-tested at most; nothing on this page has run on a
phone, in an emulator of that SoC, or anywhere else, and no port is claimed. Each number comes from a device tree or a kernel
source that `soc.conf` names, and has been read, not tried.

Does "the Exynos family just needs different headers" hold? Not for a working phone, on any SoC. It holds for the interrupt
controller, the timer, the watchdog, the UARTs, and UFS as the boot firmware leaves it. It does not hold for the GPU's power and
clock, USB's PHY, the video decoder or the CPU's frequency tables, which need code on every SoC, and the GPU's interface is a
different one from Tensor G2 on. The verdicts:

| SoC | Phones | Different headers? | What stops it, first |
|---|---|---|---|
| `exynos9820` / 9825 | S10 family, Note10 | Closest: GIC, ACPM, DECON's registers, UFS (6 of 7 numbers) | M4 and A75 clusters, GPU power, USB PHY. 9825 has no table: its tree was not read |
| `exynos990` | S20, Note20 | Same as the 9820 | The same; no G77 id in Panfrost or Mesa's model table |
| `exynos8895` | S8, Note8 | No | No core has the 8.2 the kernel keeps to (below); 37 of 71 DECON registers match |
| `exynos2100` | S21 | Partly: ACPM, GICv3, UFS (5 of 7) | New DECON register map (5 of 71), two MFC cores, no G78 id |
| `exynos850` | A21s, E850-96 | No | No UFS: eMMC, for which there is no driver. 8.2 cores, GIC-400 |
| `gs101` Tensor G1 | Pixel 6 | Partly: GIC, timer, watchdog, UFS (5 of 7) | ABL, not S-Boot; another display driver family; USI v2; no G78 id |
| `gs201` Tensor G2 | Pixel 7 | No | The above, and a CSF GPU |
| `zuma` Tensor G3 | Pixel 8 | No | The above; ARMv9 cores never tried under this kernel |
| `zumapro` Tensor G4 | Pixel 9 | No | The above; fewest numbers: no UFS, GPU or display table read |
| `lga` Tensor G5 | Pixel 10 | No | `scion-soc-lga` holds its layer. Another UART, another UFS host, another GPU vendor. Written, not yet seen on a phone |

No table has been made for the Exynos 2200 (GICv3 and Cortex-X2 in mainline, Xclipse GPU: different IP), the 8855, 7885, 7870 or
the Auto SoCs: mainline has trees for them, nobody has read further.

The cheapest next SoC to boot is the **Exynos 9820**, in a used S10e or S10. It keeps what the 9810's port depends on: S-Boot, a
GIC-400, the same ACPM SRAM and a DECON whose 71 registers read the same in Samsung's headers but for one mask the driver does not
use. A Silicium UEFI for the phone would have to exist or be written; that was not checked. A Pixel 6 is the other candidate, with
better mainline sources and no Knox, at the cost of a different boot chain and display.

## The matrix

A cell's numbers are how many of the driver's `EXYNOS_*` the table has; `built` is that it is compiled for that SoC, against that
table. Cells that are not `built` are not stubbed: the driver is off in the SoC's fragment, the kernel has no such block, and
`make soc-status` names it. Made from each `soc.conf` by `tools/soc-check.py`, which refuses a mismatch.

<!-- soc-check: matrix -->
| SoC | cpu | acpm | gic | timer | uart | ufs | usb | dpu | gpu | mfc | usi | pmic | wdt |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `exynos2100` | needs code | table only, 6/6, built | table only | table only | table only | table only, 5/7 | needs code | different IP | needs code | needs code | table only, built | unknown | table only |
| `exynos850` | needs code | unknown | table only | table only | table only | different IP | needs code | unknown | needs code | unknown | needs code | unknown | table only |
| `exynos8895` | different IP | table only, 5/6 | table only | table only | table only | table only, 7/7, built | needs code | needs code | needs code | needs code | table only, built | unknown | table only |
| `exynos9810` | runs | runs | runs | runs | table only | runs | runs | runs | runs | runs | runs | runs | runs |
| `exynos9820` | needs code | table only, 6/6, built | table only | table only | table only | table only, 6/7 | needs code | table only, 3/3, built | needs code | needs code | table only, built | unknown | table only |
| `exynos990` | needs code | table only, 6/6, built | table only | table only | table only | table only, 6/7 | needs code | table only, 3/3, built | needs code | needs code | table only, built | unknown | table only |
| `gs101` | needs code | needs code | table only | table only | table only | table only, 5/7 | needs code | different IP | needs code | needs code | needs code | different IP | table only |
| `gs201` | needs code | needs code | table only | table only | table only | table only, 5/7 | needs code | different IP | different IP | needs code | needs code | different IP | table only |
| `zuma` | needs code | needs code | table only | table only | table only | table only, 5/7 | needs code | different IP | different IP | needs code | needs code | different IP | table only |
| `zumapro` | needs code | needs code | table only | table only | table only | table only, 0/7 | needs code | different IP | different IP | needs code | needs code | different IP | table only |


### `exynos2100`: Samsung Exynos 2100 (S5E9840), Mali-G78 MP14 (Valhall v9, job manager)

| Block | State | Source | Why |
|---|---|---|---|
| cpu | code | B | A55s boot; A78 and X1 a third cluster type; DVFS tables absent |
| acpm | table | B | 9810 SRAM place; PMIC through other transports not looked at |
| gic | table | B | GICv3: the driver XNU already has |
| timer | table | B | architected |
| uart | table | B | Samsung UART |
| ufs | table | B | taken as UEFI left it; coherency word not found in the tree |
| usb | code | B | PHY moved, CAL differs |
| dpu | different | D | 5 of 71 DECON registers named the same: a new register map |
| gpu | code | B | no G78 id in Linux panfrost or Mesa's model table |
| mfc | code | B | two MFC cores, firmware |
| usi | table | B | same USI |
| pmic | unknown | - | not established from these sources |
| wdt | table | B | samsung,exynos2100-*-wdt |

- B: GoRhanHee/android_kernel_samsung_exynos2100_o1s@5fdc9b765eb3 arch/arm64/boot/dts/exynos/exynos2100.dts
- D: same tree, drivers/video/fbdev/exynos/dpu30/cal_2100/regs-decon.h

### `exynos850`: Samsung Exynos 850 (S5E3830), Mali-G52 r1 (Bifrost, job manager)

| Block | State | Source | Why |
|---|---|---|---|
| cpu | code | L | all A55s, 8.2, two clusters; no DVFS tables |
| acpm | unknown | L | mailbox and SRAM known; IPC and initdata not established |
| gic | table | L | GIC-400 |
| timer | table | L | architected |
| uart | table | L | samsung,exynos850-uart |
| ufs | different | L | no UFS: eMMC (DW MMC), for which there is no driver |
| usb | code | L | samsung,exynos850-usbdrd-phy |
| dpu | unknown | - | no display node in mainline; no BSP read |
| gpu | code | L | Panfrost and Mesa know G52; no power sequence |
| mfc | unknown | - | no node in mainline |
| usi | code | L | USI v2: a control register at the block's +0xC0 |
| pmic | unknown | - | not established from these sources |
| wdt | table | L | samsung,exynos850-wdt |

- L: linux@05441d697f7f arch/arm64/boot/dts/exynos/exynos850.dtsi

### `exynos8895`: Samsung Exynos 8895 (S5E8895), Mali-G71 MP20 (Bifrost, job manager)

| Block | State | Source | Why |
|---|---|---|---|
| cpu | different | B | A53 and M2 are ARMv8.0; the kernel keeps to cores with the A11's 8.2 (socs/exynos9810/configs/exynos9810.config) |
| acpm | table | B | same IPC shape, no fvmap in the tree |
| gic | table | L | GIC-400, as the 9810 |
| timer | table | L | architected, 26 MHz |
| uart | table | L | Samsung UART; no driver of ours uses a console UART |
| ufs | table | B | same addresses as the 9810 but the PMU; taken as UEFI left it |
| usb | code | B | PHY at another place with a different CAL |
| dpu | code | D | 37 of 71 DECON registers same name and value as the 9810 |
| gpu | code | B | Panfrost and Mesa know G71; rail, PLL and PMU sequence are the 9810's |
| mfc | code | B | samsung,mfc-v6; firmware |
| usi | table | L | same USI, mainline has HSI2C and SPI nodes |
| pmic | unknown | - | not established from these sources |
| wdt | table | B | samsung,exynos8-wdt |

- L: linux@05441d697f7f arch/arm64/boot/dts/exynos/exynos8895.dtsi
- B: acroreiser/android_kernel_samsung_universal8895@0288020e9751 arch/arm64/boot/dts/exynos/exynos8895.dtsi
- D: same tree, drivers/video/fbdev/exynos/dpu/regs-decon.h

### `exynos9820`: Samsung Exynos 9820 (S5E9820), Mali-G76 MP12 (Bifrost, job manager)

| Block | State | Source | Why |
|---|---|---|---|
| cpu | code | B | A55s boot as on the 9810; M4 and A75 are 8.2 but a third cluster type; no DVFS tables |
| acpm | table | B | 9810 SRAM and shape; fvmap_offset given |
| gic | table | B | GIC-400 |
| timer | table | B | architected, 26 MHz |
| uart | table | B | Samsung UART |
| ufs | table | B | taken as UEFI left it; PMU word for the PHY not found in the tree |
| usb | code | B | PHY blocks moved, CAL differs |
| dpu | table | D | 71 of 71 DECON registers named the same, 70 same value, the other a mask the driver does not use; DPP and IDMA not compared |
| gpu | code | M | Panfrost and Mesa know G76; rail, PLL and PMU sequence are the 9810's |
| mfc | code | M | firmware, PMU, CMU |
| usi | table | B | same USI |
| pmic | unknown | - | not established from these sources |
| wdt | table | B | two cluster watchdogs |

- B: LineageOS/android_kernel_samsung_exynos9820@a412cfaf86b9 arch/arm64/boot/dts/exynos/exynos9820.dts
- M: same tree, exynos9820-mali_common.dtsi and exynos9820-mfc.dtsi
- D: same tree, drivers/video/fbdev/exynos/dpu20/cal_9820/regs-decon.h

### `exynos990`: Samsung Exynos 990 (S5E9830), Mali-G77 MP11 (Valhall v9, job manager)

| Block | State | Source | Why |
|---|---|---|---|
| cpu | code | L | A55s boot; M5, A76 a third cluster type; DVFS tables absent |
| acpm | table | B | 9810 shape, larger SRAM |
| gic | table | L | GIC-400 |
| timer | table | L | architected, 26 MHz |
| uart | table | B | Samsung UART |
| ufs | table | B | taken as UEFI left it; PMU word for the PHY not found in the tree |
| usb | code | L | PHY moved; mainline has samsung,exynos990-usbdrd-phy |
| dpu | table | D | 70 of 71 DECON registers named the same, 69 same value, the other a mask the driver does not use; DPP and IDMA not compared |
| gpu | code | B | no G77 id in Linux panfrost or Mesa's model table |
| mfc | code | B | firmware, PMU, CMU |
| usi | table | B | same USI |
| pmic | unknown | - | not established from these sources |
| wdt | table | L | samsung,exynos990-wdt |

- L: linux@05441d697f7f arch/arm64/boot/dts/exynos/exynos990.dtsi
- B: GoRhanHee/android_kernel_samsung_exynos990_x1s@458e649f59c3 arch/arm64/boot/dts/exynos/exynos9830.dts
- D: same tree, drivers/video/fbdev/exynos/dpu30/cal_9830/regs-decon.h

### `gs101`: Google Tensor G1 (gs101), Mali-G78 MP20 (Valhall v9, job manager)

| Block | State | Source | Why |
|---|---|---|---|
| cpu | code | L | no UEFI, no S-Boot: ABL, as socs/lga has; A55s or A510s boot; three cluster types |
| acpm | code | L | initdata 0xA000; Google's ACPM differs from Samsung's fvmap |
| gic | table | L | GICv3: the driver XNU already has |
| timer | table | L | architected; CNTFRQ from the firmware |
| uart | table | L | google,gs101-uart |
| ufs | table | B | mainline google,gs101-ufs; taken as the boot loader left it, unproven; coherency word not found |
| usb | code | L | google,gs101-usb31drd-phy |
| dpu | different | B | Google DRM DPU, not fbdev DECON |
| gpu | code | B | no G78 id in Linux panfrost or Mesa's model table |
| mfc | code | B | BigOcean or MFC, firmware |
| usi | code | L | USI v2: a control register at the block's +0xC0 |
| pmic | different | L | S2MPG over ACPM |
| wdt | table | L | google,gs101-wdt |

- L: linux@05441d697f7f arch/arm64/boot/dts/exynos/google/gs101.dtsi
- B: android.googlesource.com/kernel/gs@af03195582e9 arch/arm64/boot/dts/google/gs101.dtsi, gs101-ufs.dtsi, gs101-gpu.dtsi, gs101-drm-dpu.dtsi, gs101-mfc.dtsi

### `gs201`: Google Tensor G2 (gs201), Mali-G710 MP7 (Valhall v10, CSF)

| Block | State | Source | Why |
|---|---|---|---|
| cpu | code | L | no UEFI, no S-Boot: ABL, as socs/lga has; A55s or A510s boot; three cluster types |
| acpm | code | L | initdata 0xA000; Google's ACPM differs from Samsung's fvmap |
| gic | table | L | GICv3: the driver XNU already has |
| timer | table | L | architected; CNTFRQ from the firmware |
| uart | table | L | google,gs101-uart |
| ufs | table | B | mainline google,gs101-ufs; taken as the boot loader left it, unproven; coherency word not found |
| usb | code | L | google,gs101-usb31drd-phy |
| dpu | different | B | Google DRM DPU, not fbdev DECON |
| gpu | different | B | CSF: firmware loaded and scheduled; Linux panthor, Mesa v10, not the job manager interface we drive |
| mfc | code | B | BigOcean or MFC, firmware |
| usi | code | L | USI v2: a control register at the block's +0xC0 |
| pmic | different | L | S2MPG over ACPM |
| wdt | table | L | google,gs101-wdt |

- B: android.googlesource.com/kernel/devices/google/gs201@05c125c7e224 dts/gs201.dtsi, gs201-ufs.dtsi, gs201-gpu.dtsi, gs201-drm-dpu.dtsi, gs201-mfc.dtsi
- L: linux@05441d697f7f the gs101 drivers, the family only: mainline has no gs201

### `zuma`: Google Tensor G3 (zuma), Mali-G715 MP7 (Valhall v11, CSF)

| Block | State | Source | Why |
|---|---|---|---|
| cpu | code | L | no UEFI, no S-Boot: ABL, as socs/lga has; A55s or A510s boot; three cluster types |
| acpm | code | L | initdata 0xA000; Google's ACPM differs from Samsung's fvmap |
| gic | table | L | GICv3: the driver XNU already has |
| timer | table | L | architected; CNTFRQ from the firmware |
| uart | table | L | google,gs101-uart |
| ufs | table | B | mainline google,gs101-ufs; taken as the boot loader left it, unproven; coherency word not found |
| usb | code | L | google,gs101-usb31drd-phy |
| dpu | different | B | Google DRM DPU, not fbdev DECON |
| gpu | different | B | CSF: firmware loaded and scheduled; Linux panthor, Mesa v11, not the job manager interface we drive |
| mfc | code | B | BigOcean or MFC, firmware |
| usi | code | L | USI v2: a control register at the block's +0xC0 |
| pmic | different | L | S2MPG over ACPM |
| wdt | table | L | google,gs101-wdt |

- B: android.googlesource.com/kernel/devices/google/zuma@42395e8adcb8 dts/zuma.dtsi, zuma-ufs.dtsi, zuma-gpu.dtsi, zuma-drm-dpu.dtsi, zuma-usb.dtsi, zuma-mfc.dtsi
- L: linux@05441d697f7f the gs101 drivers, the family only: mainline has no such SoC

### `zumapro`: Google Tensor G4 (zumapro), Mali-G715 MP7 (Valhall v11, CSF)

| Block | State | Source | Why |
|---|---|---|---|
| cpu | code | L | no UEFI, no S-Boot: ABL, as socs/lga has; A55s or A510s boot; three cluster types |
| acpm | code | L | initdata 0xA000; Google's ACPM differs from Samsung's fvmap |
| gic | table | L | GICv3: the driver XNU already has |
| timer | table | L | architected; CNTFRQ from the firmware |
| uart | table | L | google,gs101-uart |
| ufs | table | B | mainline google,gs101-ufs; taken as the boot loader left it, unproven; coherency word not found |
| usb | code | L | google,gs101-usb31drd-phy |
| dpu | different | B | Google DRM DPU, not fbdev DECON |
| gpu | different | B | CSF: firmware loaded and scheduled; Linux panthor, Mesa v11, not the job manager interface we drive |
| mfc | code | B | BigOcean or MFC, firmware |
| usi | code | L | USI v2: a control register at the block's +0xC0 |
| pmic | different | L | S2MPG over ACPM |
| wdt | table | L | google,gs101-wdt |

- B: android.googlesource.com/kernel/devices/google/zumapro@8831630eee2a dts/zumapro.dtsi, zumapro-usb.dtsi; zuma-ufs.dtsi shape for UFS in the zuma tree
- L: linux@05441d697f7f the gs101 drivers, the family only: mainline has no such SoC
<!-- soc-check: end -->

`table only, n/m` with n less than m is the same IP with numbers not found in the sources read. `guess:` on a number in a header
is exactly that: `ACPM_SRAM_FIRST` is the 9810's measured value, assumed for its relatives.

The Tensor G5 is not a row: `scion-soc-lga` is another repository and has no `block` lines. By the same words: GIC and timer table
only, UART different IP (DesignWare 16550), UFS different IP (Google's host), GPU different IP.

## Findings that cross the table

**GIC.** GIC-400 on the 8895, 9810, 9820, 990 and 850; GICv3 on the 2100, 2200 and all Tensors (device trees named in each
`soc.conf`). `osfmk/arm64/gic_v3_irq.c` already drives both, chosen by the loader's `gic_cpu`, so neither needs code.

**CPU.** `socs/exynos9810/configs/exynos9810.config` keeps iOS to cores that implement what the A11 does (8.2). The 8895's A53s and
M2s do not: their ARMv8.0 is from Arm's and the kernel's own descriptions, and whether iOS 16 runs on it has not been tried. Every
later Exynos has A55s to boot on. The 9810's port took all eight cores only with `m3_prep` and `isa_floor`, so the other cluster
types are code per SoC. Tensor G3 and G4 are ARMv9 (A510, A520, A715, A720, X3, X4): whether XNU 8796's assumptions hold on them is
unknown. The 4 KB against 16 KB question is unknown too: the kernel's granule has been run on A55 and M3 alone. Core lists for the
2100 and Tensor G2 to G4 are from vendor sheets, as their trees do not name the cores.

**Boot chain.** Samsung's SoCs here come up through S-Boot, whose hand-over this port knows on the 9810 alone. Tensors use Google's
ABL, with no UEFI: `scion-soc-lga`'s shim is the existing answer, not seen on a phone. `xnuloader/platforms/` has an entry per phone,
not per SoC, so each phone needs its own.

**Mali.**

| GPU | SoC | Interface | Linux | Mesa | Evidence |
|---|---|---|---|---|---|
| G71, v6 | 8895 | job manager | panfrost | panfrost | `panfrost_gpu.c` `g71`, Mesa `pan_model.c` |
| G72, v6 | 9810 | job manager | panfrost | panfrost | runs here |
| G76, v7 | 9820 | job manager | panfrost | panfrost | `g76` in both |
| G52, v7 | 850 | job manager | panfrost | panfrost | `g52` in both |
| G77, v9 | 990 | job manager | no id | no id | G57 and G68 are v9's only products in both; the G77 name is a shader model there |
| G78, v9 | 2100, Tensor G1 | job manager | no id | no id | same; a small addition if it behaves as a G68, which nobody here has tried |
| G710, v10 | Tensor G2 | CSF | panthor | G610 and G310 listed, no G710 product | `reg` is 0x480000 long, a job manager Mali's is 0x5000; firmware to load |
| G715, v11 | Tensor G3, G4 | CSF | panthor | `G715` | same |

The driver here speaks the job manager interface and Panfrost's ioctls. A CSF Mali needs firmware loaded, its queues scheduled and
Panthor's ioctls: a new driver. Every Mali also needs its SoC's rail, PLL and PMU sequence, which is 9810 code in `hakios.mali.c`.

**PowerVR on the Tensor G5, DXT-48-1536.** Facts and an estimate only; no GPU driver work has been done.

| Question | Known | Source |
|---|---|---|
| Kernel driver | mainline `drivers/gpu/drm/imagination`; compatibles `img,img-rogue`, `img,img-axe`, `thead,th1520-gpu` | `pvr_drv.c` |
| Vulkan driver | Mesa's PowerVR: AXE-1-16M, BXM-4-64, BXS-4-64 in active work, GX6250 next, others listed unsupported. No D-Series listed | Mesa `docs/drivers/powervr.rst` |
| BVNC of DXT-48-1536 | not found in the open sources read | `pvr_device_info.c` knows it not |
| Firmware | the open driver loads `powervr/rogue_<b.v.n.c>_v1.fw`, from Imagination's linux-firmware. None for a D-Series is declared | `pvr_drv.c`, Mesa's docs |
| Stock Pixel firmware under the open driver | unknown; **guess**: refused, the closed DDK's firmware interface is its own | no source: `pvr_fw.c` checks a header, the DDK's was not read |
| OpenGL ES | Mesa's driver is Vulkan only, so Zink or similar over it | Mesa's docs |
| Metal | `tools/metal/mk-metal2vulkan.sh` already builds metal2vulkan for iOS 16; how far it fits this Vulkan is untried | this tree |

**Guess, unmeasured:** with Imagination publishing firmware and device data for the core, months of one person's time for a
kernel service plus Mesa bring-up; without them, no practical path but the closed user-space driver, which iOS cannot host.

**Display.** Register maps compared by name and value against the 9810's `regs-decon.h`, 71 registers: 9820 71 named the same, 70
same value; 990 70 and 69; 8895 40 and 37; 2100 5 and 2. The differing value on the 9820 and 990 is `INTERRUPT_ENABLE_MASK`, not
used by `hakios.exynos_dpu.c`. DPP and IDMA were not compared. Tensor's DPU is a DRM driver of Google's and was not compared.

**Identifying the SoC.** A chip id block at 0x10000000 on every Samsung row and on Tensor G1 to G4, by their device trees; the Tensor
G5's was not read. Linux's `exynos-chipid.c` gives the value for the 8895, 9810, 990, 850, 2200 and gs101. The values for the 9820,
2100 and Tensor G2 to G4 were not read, so those tables have no `EXYNOS_PRODUCT_ID`.

## Sources of the survey

Linux at 05441d697f7f, as named per SoC. Device trees of Samsung's GPL kernels through LineageOS and two community trees, and
Google's kernels on `android.googlesource.com`, each pinned in its `soc.conf`. Facts only: nothing of them is copied here beyond the
numbers. Mesa `main` for the model and driver tables. Commits of Mesa were not pinned.
