# Pixel 10: to a framebuffer, with software rendering

**Draft. Nothing here has run on a phone. This page asks no one to run it; [scion-frankel's FIRST-TEST.md](https://github.com/Scion-Project/scion-frankel/blob/main/FIRST-TEST.md) does, with its wipe warning.** Only the Exynos 9810 has booted
([docs/soc-support.md](soc-support.md)). `scion-frankel` and `scion-soc-lga` are written and seen in QEMU only. The Pixel 6a results quoted are
[PixOS](https://github.com/PixOS-Project/pixelios-kernel)'s.

The goal is one picture drawn by iOS's CPU on a Pixel 10 (`frankel`, Tensor G5 "laguna"), without a GPU driver and without a display
driver if the boot loader's output can be inherited.

## The route

`fastboot boot` from RAM, on an unlocked phone. Nothing is written to storage, and a restart brings Android back. Unlocking wipes the phone,
and `fastboot flashing lock` wipes it again.

```
fastboot boot xnu-frankel.img  ->  ABL  ->  socs/lga/shim  ->  xnuloader ("frankel")  ->  XNU
                                   image v4,  EL2 kept for PSCI;    found by the tree's    GICv3, timer,
                                   AVB footer UEFI services at EL1  root compatible        ABL's framebuffer, root in RAM
```

| Step | Done when | Already there | Missing |
|---|---|---|---|
| 1. Unlock, record `ro.bootloader` | `fastboot flashing unlock` done | `devices/frankel/README.md`, Bring-up | a phone whose data is expendable |
| 2. A serial log, if possible | ABL's lines on a 1.8 V UART | README: USB-C SBU, `fastboot oem uart enable`; **assumed** from the Pixel 6a | not checked on a Pixel 10 |
| 3. `fastboot boot` accepted | the phone leaves fastboot | `mkimage.sh`, `mkxnuimg.py`, the AVB footer. PixOS: a 6a ignores an unsigned image | unknown on this boot loader's version |
| 4. The shim runs | a blue band on the panel | `shim/`, its bands, watchdogs stopped | the framebuffer has to be found: below |
| 5. The loader runs | green, then yellow; its banner | `platform_frankel`, unchanged `xnuloader` | |
| 6. XNU to `launchd` | `-v` on the panel | GICv4 frames in `gic_v3_irq.c`; PSCI by HVC through the shim; core 0 alone first; root in RAM | whether ARMv9.2 cores run XNU 8796 unchanged is unknown |
| 7. Picture from CoreAnimation | the lock screen | **guess**: the software route is QEMU's; the kernel's frame kicker (below) | whether its two writes start a frame |
| 8. The system image | SpringBoard | read-only UFS driver for Google's host, GPT, mount, to the home screen in QEMU (below) | whether ABL leaves the host up; DMA; everything on the phone |

No GPU, no touch, no USB, no modem are in this list on purpose. Storage is a second route, below ("Storage: the system image off `userdata`").

## Can the boot loader's framebuffer be inherited?

**Evidence levels in this section: *source-read* is Google's driver and device trees read, nothing run; *guess* is inference from
them; *unknown* is what no source here says. Nothing is *tested*.** Sources are the table below; Google's files are GPL-2.0, read for
facts, nothing copied. Paths are below `private/google-modules/display/vsi-drm/verisilicon/9x00/` unless they say otherwise
(`azagramac/pixel10-kernel-aosp` at `6f689be7fa23`).

### What Google's driver says

| # | Fact | Level | Where |
|---|---|---|---|
| 1 | The panel is a **command-mode** DSI panel with **DSC**: the `flea` family (`panels/muzel/panel-gs-flea.c`, beside `fleb` and `bmea`: which one a frankel has is not read from a tree), 1080 x 2424, 60 Hz (TE width 8606 us), 120 Hz and 30 Hz; `mode_flags` lacks `MIPI_DSI_MODE_VIDEO`; DSC 1.2a with two encoders, 8 bpc. The 60 Hz mode is commented "aligned to bootloader setting": ABL leaves the 60 Hz mode | source-read | `flea_modes`, `pps_config` |
| 2 | **ABL leaves the panel and the DPU running, and Google's kernel keeps them so.** The panel driver has a smooth hand-off (`gs_panel_handoff`, `CONFIG_GS_PANEL_SMOOTH_HANDOFF_ENABLED`): reset and enable GPIOs high and every rail on means "enabled at boot", no init commands are sent again. The DSI encoder skips the first modeset's frame when nothing changed "so that splash image can be kept", and only "if DPU is powered on" (`vs_mipi_dsi_encoder_check_skip_first_frame`) | source-read | `common/gs_panel/gs_panel.c`, `vs_mipi_dsi2h.c` |
| 3 | **Self-refresh is the panel's, not the DPU's.** In command mode the panel keeps its own copy of the picture; the splash stays on the glass whatever the DPU does. There is no DPU state that "refreshes" it. New content reaches the panel only as a frame the DPU sends. So writing to ABL's buffer **alone** changes nothing on the panel (inference from 1 to 3) | source-read, guess | the same |
| 4 | **Starting one frame** of a DC9x00 with chip id 0x9400 (`hw->rev == DC_REV_0`; the tree has tables 0x9400/0x316 and 0x9400/0x32A, product 0x02094000): `dc_hw_start_trigger` clears the pending FRM_START status, `online_trigger` writes **`OUTPUT0_START` (0xB6000) = 1** after setting the panel-to-output mux, and for a command-mode output not in auto mode `dc_hw_sw_sof_trigger` writes **`OUTPUT0_SW_CONFIG` (0xB6004) = 1** ("software panel start, ready"). Command mode with a software trigger is the default (`is_display_cmd_sw_trigger`: command mode and not `VS_OUTPUT_MODE_CMD_AUTO`, which only a debugfs knob sets). When it enables the TE interrupts the driver also writes `OUTPUT0_START` once and `SH_OUTPUT0_CLK_EN` (0xB60B4) = 1. These are written past the register cache (`dc_write_immediate`) | source-read | `vs_dc_hw.c`: `dc_hw_start_trigger`, `online_trigger`, `dc_hw_sw_sof_trigger`, `dc_hw_enable_vblank_irqs`; `vs_dc.c`: `is_display_cmd_sw_trigger` |
| 5 | The panel's **TE** pulls the transfer: TE rising and falling edges are interrupt bits of the back end (BE_INTR_STATUS 0x80008; for output 0 bit 0 frame start, 1 frame done, 2 underrun, TE edges in the same register); the driver counts a frame from FRM_DONE | source-read | `vs_dc.c` `vs_dc_handle_interrupts`, `vs_dc_reg_be.h` |
| 6 | **A frame counter exists: `DEBUG_DSI0_FRAME_CNT` at 0xB6434**, read-only, "DSI0 frame counter in timing gen". The panel is on output 0: the first DSI host has `verisilicon,mux-id = <0>`. That the counter counts frames *sent* is read from its name | source-read, guess | `vs_dc_reg_be.h`; `devices/google/lga/dts/lga-dpu.dtsi` |
| 7 | **Whether a kick of just these two registers is enough** with ABL's layers, mux and DSC left as they are is not shown by anything read: Google's driver starts frames of a pipeline it set up itself | guess | |
| 8 | **The SMMU.** The DPU's 21 stream ids (1 to 26) are behind `dpu_smmu`, an SMMUv3 at 0x0ED80000 (`non-coherent-ttw`, 8 stream-id bits), and `vs_drv` attaches an IOMMU domain for them when every output's node has `iommus`. Nothing in the display driver or the trees says what ABL does with that SMMU: **unknown from source**; no splash region or `simple-framebuffer` is in the trees either | unknown | `lga-smmu.dtsi`, `lga-dpu.dtsi`, `vs_drv.c` |
| 9 | **Finding the framebuffer** has no static answer. The layer's address is in FE0's layer registers (layer 0 at 0x01802C, high byte 0x018030, stride 0x018044, size 0x01805C, config 0x018088; layers 1 to 5 each 0x4000 on; FE1's layers 8 to 13 from 0x58000). Those are the **shadow** group: `FE0_REG_READ_SRC` (0x140, bit 0: 1 selects the active group) and the back end's (0x80000) pick which group a read returns, and the shim reads without changing them. If SMMU_CR0.SMMUEN is set the address is an IOVA and not physical | source-read | `vs_dc_reg_fe0.h`, `dc_hw_reg_dump` |

What that means for step 4 and 7: **a kick is very likely needed** (3, 4); the two registers to write are named (4); the counter to read
back is named (6); whether the DPU's own state survives the `fastboot boot` hand-over, and the SMMU, are only learned on a phone (8).

`scion-soc-lga`'s shim has the probe for exactly this (`scion.probe=HOLD`, `scion.kick=1`; its README): it logs the controller's
identity, output 0's registers and the counter, paints four bands on a timeline, starts one frame between the third and the fourth,
and writes its log to ramoops' console zone. What it cannot say: why a frame did not arrive, if none does.

### Still open

- **The framebuffer** is found, in order, by `scion.fb=` on the command line, a `simple-framebuffer` node should ABL add one, and DC9x00
  layer 0's address if SMMU_CR0.SMMUEN is off (9). Whether ABL leaves that SMMU off is unknown (8).
- **The kick** (4, 7). If it works, a kernel driver has to start a frame whenever the screen changes: none is written. Until then the
  shim's `scion.kick=1` gets the stage bands and the loader's banner to the panel, and the kernel's own picture does not appear.
- **Whether ABL turns the display off at hand-over**, as PixOS found it does to UFS on a 6a (`soc/google/gs101/docs/08-abl-ufs-shutdown.md`):
  2 says Google's own kernel finds it on after ABL, not that `fastboot boot` leaves it on.
- **Which panel a frankel has** (1): `flea`, `fleb` or `bmea`; the identity ABL reads is passed on in the tree (`gs_panel_read_extinfo`).

## Sources for a DC9x00 driver

| Source | What | Licence | Use |
|---|---|---|---|
| `private/google-modules/display/vsi-drm/verisilicon/` in [azagramac/pixel10-kernel-aosp](https://github.com/azagramac/pixel10-kernel-aosp) at 6f689be7fa23, a public copy of Google's kernel | the phone's own DRM driver: register layout, start sequence | GPL-2.0 (`vs_drv.c`'s SPDX line) | facts only |
| Linux `drivers/gpu/drm/verisilicon` at 05441d697f7f | mainline DC-series driver; Kconfig `depends on RISCV`. Whether it covers a DC9x00 is **not established** | GPL-2.0-only | facts only |
| `lga-dpu.dtsi`, `lga-smmu.dtsi` through `scion-soc-lga`'s README | addresses, layers, SMMU | GPL-2.0 | facts, as already recorded |

## The frame kicker (`lga_kick=HZ`)

`drivers/display/dpu/hakios.lga_dpu.c`, built into the Pixel 10's kernel alone (`CONFIG_HAKIOS_LGA_DPU=y` in `scion-soc-lga`'s `lga.config`, `default n`
elsewhere), off until `lga_kick=` asks (`mkimage.sh -K` asks for 30). A thread call every 1/HZ seconds writes `OUTPUT0_START` (0xB6000) = 1 and then
`OUTPUT0_SW_CONFIG` (0xB6004) = 1 (fact 4), reads `DEBUG_DSI0_FRAME_CNT` (0xB6434) before and after (fact 6), and starts the next frame only when the counter
has moved since the last kick or four periods have passed. It writes nothing unless the chip id (0x20) is 0x9400 and output 0 is in command mode
(0xB6008 bit 1). The first eight kicks log their writes and the counter; then every five seconds `lga_kick: N kicks, frame counter C (+D in 5 s)`, with
"IT DID NOT MOVE" when D is 0. The buffer it makes visible is the one the kernel and CoreAnimation already draw into (MiniIOMobileFramebuffer takes
ABL's buffer through the boot arguments): no framebuffer driver of its own is needed.

QEMU (`qemu/run-lga-ufs.sh`): RAM stands where the controller would be (`lga_dpu=0x90000000`, chip id and command mode put there by `-device loader`).
The log shows the identity line, `a frame of output 0 every 33 ms`, `kick 1: wrote b6000 <- 1, b6004 <- 1; frame counter 0 before, 0 after` for eight kicks, then
about 30 kicks per five seconds with the counter at 0: nothing counts frames in RAM, so QEMU shows the writes and the reporting, and **nothing about whether a
frame starts**. That is the phone's to say.

## Storage: the system image off `userdata`, read only

The Galaxy S9 mounts `system.apfs` from a partition of its flash and its small root from RAM; this is the same for the Pixel 10 with one
difference: **no write of any kind**. `frankel_ufs_defconfig` (`devices/frankel`) builds `hakios.ufshc.c` with its `lga` entry and the image says
`ufs_native=4 ufs_native_soc=lga ufs_native_ro=1 system_apfs=ufs_userdata`; init mounts `/dev/ufs_userdata` read only and makes it the root.

| What | Fact | Level |
|---|---|---|
| Host | Google's `google,ufshc` at 0x3C400000 (top block 0x3C410000, PHY SRAM 0x3C700000), a UFSHCI 4.0 host; SMMUv3 in front, identity-mapped | source-read |
| Doorbell mode | single doorbell is the default (`CONFIG.QT` 0, +0x300); Google's driver turns on its multi circular queue; the driver here refuses to go on if `QT` is set | source-read; unknown what ABL leaves |
| Quirks | `UFSHCD_ANDROID_QUIRK_36BIT_ADDRESS_DMA` (DMA width 36 bits whatever CAP says: the driver checks its memory is below 2^36) and `UFSHCD_ANDROID_QUIRK_SET_IID_TO_ONE` (lga only: the high nibble of UPIU byte 4 is 1 in every command, NOP and query included; done in the `nexus` hook before each doorbell) | source-read |
| Descriptors | 16-byte PRDT entries, offsets in dwords, no vendor nexus bit: as the standard (and QEMU's) host; nothing says otherwise | guess |
| Top block | reported before anything is taken: version, card detect, `ENC_STAT`, `PWR_CTRL` (which M-PHY firmware ABL selected: ROM, or the patched SRAM image, Google's mode 2), `AWUSER_VC` (Google sets 1 and 1), `CONFIG`, `MCQCONFIG` | source-read offsets |
| Cold re-init | **not implemented, and not possible from this tree**: Google's bring-up is the CPM's power domains, the PMIC's rails (DA9188 LDOs 1S and 24S, bucks 11S and 13S), the reset GPIO, the reference clock, `HSIOS_UFS_*` setup, the FIPS self-test wait, 125 KB of M-PHY firmware loaded into SRAM (mode 2), calibration, `HCLKDIV` 0xC8, link startup, T_ACTIVATE and C-port setup. None of that exists here and the firmware is Google's (GPL data: facts only, not copied) | source-read |
| What does apply from PixOS (Pixel 6a, `soc/google/gs101/docs/08-abl-ufs-shutdown.md`, MIT, nothing copied) | ABL's shutdown leaves the *device* in power-down and the *link* in Hibern8 while the host registers stay on; the cure there is hardware reset + HCE + link startup (Exynos/gs101 registers: do not apply), with "H8 exit + START STOP UNIT" as an untested softer way. Here: the driver already sends START STOP UNIT (ACTIVE) when `bCurrentPowerMode` is not active; `ufs_native_h8exit=1` (off) sends UIC DME_HIBERNATE_EXIT first (`mkimage.sh -U 5 -H`). Untested anywhere | partly guess |
| G5 vs the 6a | whether ABL shuts the host down at all on `fastboot boot`; Google's kernel assumes it is up ("We assume the power domain is in ON state when enter kernel"); an S2MPU stopped DMA from low RAM on the 6a; none known for a Pixel 10 | unknown |

**Read only, enforced three ways.** The `lga` entry has `.read_only = true`, which sets the controller layer's own fence (`hc_pol.ro`: every data-out command
and every write refused in `hc_refusal`, and no window can be opened over a partition) and nothing clears it; `ufs_native_write=` and `ufs_write=` then
find nothing to open. `audit-image.py --ufs-ro` refuses an image whose `.config` or command line could write (`ufs_write`, `ufs_native_write`, `var_hfs`,
`var_store`, `ufs_native_test` and more), requires `ufs_native_soc=lga` and `ufs_native_ro=1`, and checks the source still has the entry read only. And the
QEMU test attaches the disk `readonly=on` at the host, so a write could not have landed there either.

**What QEMU shows** (`qemu/run-lga-ufs.sh`, QEMU 11.1.3, `-cpu max`, 8 cores, 7 GB, XNU's window 4.2 GB, the QEMU `ufs` device holding a GPT
(`tools/mkgpt.py`: Pixel-like partition names, `userdata` holding the system image): the shim, the loader, `frankel_ufs` kernel, `ufs_native_soc=lga-qemu`
(the entry's quirks on QEMU's PCI controller), the GPT read (17 partitions), `/dev/ufs_userdata` mounted read only and made the root, launchd, the jobs
started (100 spawned lines by minute four), SpringBoard's home screen drawn by the CPU into the framebuffer (a picture taken through the monitor, `qemu/fbshot.py`), all eight
processors active (the loader's own core joins after the kernel asks). **What it cannot show**: the real `lga` entry's addresses and the top block (no such
device in QEMU), ABL's state, the M-PHY, the SMMU, DMA, timing, MCQ-versus-doorbell, whether the IID setting is what the host wants, and anything of a
panel. `lga-qemu` and `lga` differ in exactly the PCI discovery, `may_enable` (QEMU's controller is enabled from reset; the phone's never) and the `look` hook.

## CPU features

QEMU 11.1.3's `-cpu max` has pointer authentication (QARMA3, address and generic), BTI, MTE (instructions; no tag memory), SVE and SME; the shim's `cpu:` line
says `pauth addr 5 generic 1, bti 1, mte 1, sve 1, sme 2`. There is no A520, A725 or X4 in QEMU and its cores are all one kind per machine: `cortex-a710`
(Armv9.0) is the nearest; launchd spawned SpringBoard on it too (`cpu:` there: pauth 5/1, bti 1, mte 1, sve 1, sme 0). **XNU 8796 needed no fix** for any of it: it started on all eight cores (`cpus=8 bootcpu=7
core0=2`: `core0=2` gives the loader's own core to the kernel, else seven; `small_cores_after=0` starts the slower cores at once, else after 28 s), ran
launchd and SpringBoard, with no exception, PAC or BTI fault in the log. The kernel is arm64 (not arm64e) and iOS 16.5's iPhone X userland is arm64: neither uses
PAC, BTI, SVE or SME, so this shows they are *harmless* to XNU, not that they work. Nothing was seen that FPAC, `SCTLR` or `CPACR` settings of Google's
firmware could not change on the phone; the cores' real state is what the `cpu:` line there will say.

## Early console

In the order they exist:

1. **Coloured bands**: the shim, once it has the framebuffer. A photograph says how far it got; needs step 4's framebuffer and pixels reaching the panel.
2. **The DesignWare UART** at 0x0DB62000, 16550-style, 1.8 V on SBU: the shim could write it from entry, and needs nothing else on the phone but a breakout.
   `platform_frankel` has `.uart = 0` today: the loader's UART code is PL011's, so this is code to write in the loader and shim.
3. **ramoops** at 0x95200000, kept across a warm restart for ABL: `pmsg` read back from Android. Works with no screen at all, at the price of a restart per look.

## What to take from PixOS

Its top level is MIT, and parts are under other licences (`THIRD_PARTY.md`, row by row). Credited here, nothing copied so far. Worth having, by a reading of its
documents rather than a trial: a **build-time audit** that rejects any path writing to storage in code before the kernel (`docs/SAFETY.md`, Pixel-only: the loader on
the S9 needs writes for its log partition, so it does not transfer unchanged), and **an unattended loop** of build, `fastboot boot`, read the log, reset back to
fastboot (`docs/AUTOMATION.md`). Its QR log channel answers a problem step 4 also has: reading a screen without a cable.

## The GPU's interface, for later

No GPU driver is planned here, and none of this is copied. Facts from Google's `private/google-modules/powervr/pvrsrvkm` at the same commit, whose files are headed
**Dual MIT/GPLv2**. What the phone's firmware is: [soc-support.md](soc-support.md).

- **To user space:** one DRM node. `DRM_PVR_SRVKM_CMD` (command 0) carries nearly everything, as a services "bridge" call: 27 server bridges (`srvcore`, `mm`, `sync`, `rgxta3d`
  and others), with `DRM_PVR_SRVKM_INIT` and a few fence commands beside it (`pvr_drm.h`, `pvr_bridge.h`). This is not mainline's `drm/imagination` UAPI, which Mesa's driver speaks.
- **To the firmware:** a kernel CCB, a firmware CCB and client CCBs, per `volcanic/rgx_fwif_km.h`, with its layout tied to the DDK version: 25.3 here.
- **Size:** 559 files; 191 under `volcanic/`, the generation's own.
- **What a port would face:** Imagination's closed user-space driver expects this bridge and is not an option under iOS, Mesa has no Volcanic, the open firmware does not exist for this core
  and the phone's goes through a Trusty app. Metal on Vulkan needs a Vulkan first. The estimate in soc-support.md stands.

## What iOS 27 would need for this phone

Not implemented, not planned in this repository: this is what the evidence says, for whoever does it. The kernel here is XNU 8796 (iOS 16.5).

- **Kernel and arm64e.** iOS and iPadOS 27 is **xnu-12377.121.6** (Apple's `xnu-12377.*` tags; the newest is `.121.6`; UniXNU's
  `ipados-27.0.1-24A446` profile pins it, "with the Darwin 26 calls") and is **arm64e only**: UniXNU's `docs/porting/arm64e.md` says 18 is the
  last release that supports arm64. Everything in the userland and the kexts is arm64e, so the kernel has to be built with pointer
  authentication and the cores have to have it. 16.5, what runs today, is arm64.
- **PAC on these cores.** The Cortex-A520 (cores 0-1), A725 (2-6) and X4 (7) are Armv9.2-A. LLVM's AArch64 target lists
  `FeaturePAuth` and `FeatureFPAC` for each of the three (`AArch64Processors.td`, `A520`, `A725`, `X4`; main at 2026-11-06), and
  `HasV9_0aOps` reaches `FeaturePAuth` through v8.3. **Level: the compiler's model of the cores, not a register read.** The first
  probe run reads `ID_AA64ISAR1_EL1`, `ID_AA64ISAR2_EL1`, `ID_AA64PFR1_EL1` on the boot core and prints `pauth addr/generic`, BTI and MTE
  (`cpu:` line), which settles it for that core (UniXNU's check: APA or API, and GPA or GPI, non-zero, or APA3 and GPA3). Not read from a
  Google source: whether Google's firmware leaves them on at EL2 (the shim sets `HCR_EL2.API/APK` where the ID register says the core has
  them, so EL1 may write the five key registers).
- **FPAC.** FEAT_FPAC makes a failed `AUT*` fault at the instruction (ESR EC 0x1C) instead of poisoning the pointer. XNU 12377 knows the
  encodings (`ID_AA64ISAR1_EL1_API_FPAC_EN`, `_FPACCOMBINE` in `proc_reg.h`) and handles `ESR_EC_PAC_FAIL` in `sleh.c` (8796 has the handler
  too); how its boot code and the kernelcache's chained fixups behave on a QARMA3/FPAC core rather than Apple's is the thing to test.
  UniXNU's note: signatures are made on the device, so the CPU's algorithm need not match Apple's.
- **UniXNU's layering**, from its README and docs: a **UEFI environment** (the UniXNU package and `DarwinBoot`, the loader that starts
  XNU) over **shared SoC repositories** (`UniXNU-<soc>-common`: platform support) and **device repositories** (board configuration and
  hardware), with **OS profiles** (`profiles/os/*`: the XNU tag, the IPSW pins, prelink roots, patches; `profiles/arch/arm64` and
  `arm64e`) and **components** (drivers, `components.toml`), packaged by one script with `--os`. This repository is the same split
  by another route: `scion-soc-lga` (SoC), `scion-frankel` (device), `xnuloader` (the loader), a shim in place of a UEFI. The
  mapping, if the Pixel 10 were to move to UniXNU: the shim's job (EL2, RAM map, PSCI, framebuffer, watchdogs) is a UEFI port's
  `LagunaPkg`/`frankelPkg`, which does not exist (README of `scion-soc-lga`, "How HakiOS starts here"); a UEFI is the only thing UniXNU
  asks of the phone.
