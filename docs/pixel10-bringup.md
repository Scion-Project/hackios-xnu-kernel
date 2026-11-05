# Pixel 10: to a framebuffer, with software rendering

**Draft. Nothing here has run on a phone, and this page does not ask anyone to run it.** Only the Exynos 9810 has booted
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
| 7. Picture from CoreAnimation | the lock screen | the software route is the same as QEMU's | a framebuffer that keeps updating (below); a kernel console that sees the panel |

No GPU, no UFS driver (Google's own host, in RAM until written), no touch, no USB, no modem are in this list on purpose.

## Can the boot loader's framebuffer be inherited?

Partly known, and the unknown part is what matters.

- **Finding it.** No tree says where it is: no `simple-framebuffer`, no splash region. The shim takes, in order, `scion.fb=` on the command line, a
  `simple-framebuffer` node should ABL add one, and DC9x00 layer 0's address, read from its shadow registers, if SMMU_CR0.SMMUEN is off.
  Whether ABL leaves that SMMU off is **unknown**.
- **Pixels reaching the panel: the risk.** The panel is **command mode** with DSC (`scion-frankel`'s README, from Google's `panel-gs-flea.c`). In command mode
  the panel keeps its own copy of the picture and shows nothing new until the controller sends a frame. If ABL leaves no self-refresh running, writing to
  the buffer changes nothing on the panel: the phone looks hung while it is running. **Unknown; this is the first thing to learn.** A
  run that paints the bands and reads back the DPU's frame counter, if one is found, would tell.
- **If a kick is needed.** Then a minimal driver, not a stub: start one frame on output 0 (0xEE00000 + 0xB6000 by the SoC README), on the panel's TE or a timer.
  The 9810's DECON already works this way (`hakios.exynos_dpu.c`). The register to write has not been established.
- **Also unknown:** whether ABL turns the display off at hand-over, as PixOS found it does to UFS on a 6a (`soc/google/gs101/docs/08-abl-ufs-shutdown.md`). PixOS
  draws on ABL's framebuffer on the 6a, but its panel and controller are others.

## Sources for a DC9x00 driver

| Source | What | Licence | Use |
|---|---|---|---|
| `private/google-modules/display/vsi-drm/verisilicon/` in [azagramac/pixel10-kernel-aosp](https://github.com/azagramac/pixel10-kernel-aosp) at 6f689be7fa23, a public copy of Google's kernel | the phone's own DRM driver: register layout, start sequence | check each file: the repository is marked GPL-2.0 | facts only |
| Linux `drivers/gpu/drm/verisilicon` at 05441d697f7f | mainline DC-series driver; Kconfig `depends on RISCV`. Whether it covers a DC9x00 is **not established** | GPL-2.0-only | facts only |
| `lga-dpu.dtsi`, `lga-smmu.dtsi` through `scion-soc-lga`'s README | addresses, layers, SMMU | GPL-2.0 | facts, as already recorded |

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

No GPU driver is planned here, and none of this is copied. Facts from Google's `private/google-modules/powervr/pvrsrvkm` at the same commit: files headed **Dual MIT/GPLv2**,
as the Pixel 10's firmware facts are in [soc-support.md](soc-support.md).

- **To user space:** one DRM node. `DRM_PVR_SRVKM_CMD` (command 0) carries nearly everything, as a services "bridge" call: 27 server bridges (`srvcore`, `mm`, `sync`, `rgxta3d`
  and others), with `DRM_PVR_SRVKM_INIT` and a few fence commands beside it (`pvr_drm.h`, `pvr_bridge.h`). This is not mainline's `drm/imagination` UAPI, which Mesa's driver speaks.
- **To the firmware:** a kernel CCB, a firmware CCB and client CCBs, per `volcanic/rgx_fwif_km.h`, with its layout tied to the DDK version: 25.3 here.
- **Size:** 559 files; 191 under `volcanic/`, the generation's own.
- **What a port would face:** Imagination's closed user-space driver expects this bridge and is not an option under iOS, Mesa has no Volcanic, the open firmware does not exist for this core
  and the phone's goes through a Trusty app. Metal on Vulkan needs a Vulkan first. The estimate in soc-support.md stands.
