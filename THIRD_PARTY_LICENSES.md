# Third-party licenses

Every third-party component HakiOS redistributes or fetches at build time, with its origin,
license, and status. Where "origin uncertain" or "to verify" is written, the item is **not**
asserted to be cleared for redistribution; treat it as pending review (see `COMPLIANCE.md`).

This supersedes the summary in `NOTICE`, which stays as the short, canonical list.

## Redistributed in this repository

| Component | Path | Origin | License | Notes |
|---|---|---|---|---|
| XNU modifications | `xnu/hakios.patch`, `xnu/files/`, `drivers/` | derivative of Apple XNU | APSL 2.0 (`LICENSE.APSL`) | base tag `xnu-8796.121.2`, fetched by `xnu/apply.sh` |
| libdeflate | `xnu/files/bsd/miscfs/apfs/libdeflate/` | Eric Biggers | MIT | see its `COPYING` |
| libzbitmap | `xnu/files/bsd/miscfs/apfs/libzbitmap/`, `xnu-deps/build/lzbitmap/` | Corellium LLC (port of eafer/libzbitmap) | GPL-2.0+ OR MIT | used under MIT |
| SoftwareAES (UniXNU) | `xnu/files/iokit/Kernel/MiniIOAESAccelerator.cpp` | ArmVirtPlatform contributors, github.com/UniXNU/SoftwareAES at `4a45326da5c2` | BSD-2-Clause-Patent | the whole file is under it; its notice is in the file's head |
| LinuxKPI / Linux headers | `wifi/lx/include/linux/`, `wifi/lx/include/net/` | Linux / FreeBSD LinuxKPI | GPL-2.0 (Linux) / BSD-2-Clause (FreeBSD) | small compatibility headers; see each file's notice |
| brcmfmac patch | `wifi/lx/brcmfmac/patches/` | changes to Linux's brcmfmac (Broadcom) | ISC | the BCM4361's IDs, RAM base and firmware names, and two lines where the kernel's `brcmfmac=N` stops; its context lines are brcmfmac's |
| Linux DRM shim | `tools/mesa/hakios-drm/`, `xf86drm.h` | Linux / X.Org | MIT | build-time shim for Mesa |
| AIR / Metal-library tooling | `tools/mesa/air/` | HakiOS (Darwin on redwood contributors) | MIT | our own; LLVM bitcode layout is a published format |
| gfxstream Vulkan guest patch | `tools/gfxstream/vk/mesa-gfxstream-scion.patch` | changes to Mesa's `src/gfxstream` (Google / AOSP) | MIT | applied by `tools/gfxstream/mk-vk.sh`; `tools/gfxstream/vk/scion/` is our own (MIT) |
| AdrenoMetal (Metal frontend and C-ABI bridge) | `userland/src/metalvk/NativeBridgeMetalFrontend.{mm,h}`, `tools/metal/bridge/NativeMetal{Objects,Bridge}.{rs,h}` | UniXNU, github.com/UniXNU/AdrenoMetal at `ce5e19e33e05` ("NativeMetal contributors") | MIT (`LICENSE.AdrenoMetal` beside each) | copied with the notice; Scion's changes marked `Scion:`; METAL=vk only |
| mkbootimg.py | `devices/redwood/prebuilt/mkbootimg.py` | Android Open Source Project | Apache-2.0 | |
| Mu-Silicium / Project Mu derived files | `devices/redwood/uefi/` | Project Silicium, Microsoft, contributors | BSD-2-Clause-Patent; some GPL-2.0+ OR MIT | derived parts keep upstream license |
| Qualcomm/Xiaomi UEFI driver binaries | `devices/redwood/uefi/files/Binaries/` | Qualcomm / Xiaomi (from device `xbl.img`) | vendor's — **to verify** | needed only to rebuild UEFI; removable, re-extract with UEFIReader |
| FocalTech touch firmware header | `drivers/input/hakios.focaltech_fw.h` | FocalTech (device firmware blob) | vendor's — **to verify** | candidate to move to pull-from-device |
| Apple host headers (build only) | `xnu-deps/hostinc/mach-o/`, `.../mach/` | Apple (open-source cctools/xnu headers) | APSL 2.0 | host-side Mach-O headers for the cross build |
| Pre-built kernel & user space | `prebuilt/` | built from this repo | APSL 2.0 / MIT (as source) | reproducible from source here |

## Fetched at build time, never committed

| Component | Fetched by | Origin | License |
|---|---|---|---|
| Darwin userland 16.5 IPSW (user space of the system image) | `sysimage/unpack.sh` | Apple | Apple's — user supplies, user's own copy |
| iPhoneOS 16.5 SDK | `xnu-deps/setup.sh` (github.com/theos/sdks) | Apple (headers) via theos | Apple's / theos |
| Apple open sources (AvailabilityVersions, libdispatch, libpthread, cctools, bootstrap_cmds) | `xnu-deps/setup.sh` | Apple | APSL 2.0 |
| MacOSX13.3 SDK (Kernel.framework DriverKit headers only) | `xnu-deps/setup.sh` | Apple via joseluisq/macosx-sdks | Apple's |
| lzfse | `xnuloader/mkidentity.sh` (when no decoder present) | Apple, github.com/lzfse/lzfse | BSD-3-Clause |
| ath11k (Wi-Fi driver) | `tools/mk-ath11k.sh` | Linux | BSD-3-Clause-Clear |
| brcmfmac and brcmutil (the Galaxy S9's Wi-Fi driver) | `tools/mk-brcmfmac.sh` (the Linux release `tools/mk-ath11k.sh` pins) | Linux (Broadcom) | ISC |
| Mesa (GL=1 images) | `tools/mk-mesa.sh` | Mesa project | MIT and others |
| Mesa's gfxstream guest Vulkan driver (`src/gfxstream`, with the Vulkan runtime it builds on) | `tools/gfxstream/mk-vk.sh` | Google / AOSP via the Mesa project | MIT |
| gfxstream (codegen descriptions; host renderer for the Scion app) | `tools/gfxstream/fetch.sh`, `mk-host-android.sh` | Google | Apache-2.0 (some MIT) |
| metal2vulkan (runtime, translator, AIR reader; METAL=vk images) | `tools/metal/mk-metal2vulkan.sh` (github.com/steelbrain/metal2vulkan at `720ef1ce9ae2`, with AdrenoMetal's patches and ours in `tools/metal/m2v/`) | Anees Iqbal | GPL-2.0-or-later OR LGPL-3.0-or-later; only ever in its own `libmetal2vulkan.dylib`, which can be replaced |
| Rust toolchain and crates (ash, libc, sha2, serde ...) | `tools/metal/mk-metal2vulkan.sh` (rustup into `xnu-deps/rust`, crates.io) | the Rust project, crate authors | MIT / Apache-2.0 |
| Procursus / Sileo packages | `tools/mk-jb.sh` | Procursus team | their own licenses |
| Phone firmware (GPU, Wi-Fi, DSP, modem, haptics, cameras) | `devices/redwood/pull-firmware.sh` | Qualcomm / Xiaomi | vendor's — user's own device |

## Read for reference, not copied

| Component | Read for | Origin | License | How it was used |
|---|---|---|---|---|
| ShengUfs | `drivers/storage/hakios.ufshc.c/.h` | [UniXNU/ShengUfs](https://github.com/UniXNU/ShengUfs) at `f2d6a4a72f5e7be7d2c9b78eba16cf373868fa30`, Copyright (c) 2026, blu-sharky | BSD-2-Clause (files marked BSD-2-Clause-Patent) | compared with this driver item by item (`hakios.ufshc.h`); the behaviour adopted was written again here, no line of it is in this tree. Its notice, below, goes with it all the same |
| Samsung's Exynos 9810 kernel (the Galaxy S9's) | the S9's UFS facts in `hakios.ufshc.c` | Samsung, Linux | GPL-2.0 | facts only (addresses, register offsets, descriptor layout, quirks), each named with its file there; no code |
| Samsung's bcmdhd (the S9's Broadcom driver) | the BCM4361's chip and PCI IDs, RAM base and revisions in `wifi/lx/brcmfmac/patches/bcm4361.patch`; the firmware's file names in `devices/starlte/WIFI.md` | Samsung / Broadcom, the S9's kernel at `e5906667068e` (`drivers/net/wireless/broadcom/bcmdhd_101_12`) | GPL-2.0 | facts only, named in the patch's head; no code |
| galaxy-s9plus-mainline-linux | the same, cross-checked | [SheroAbi/galaxy-s9plus-mainline-linux](https://github.com/SheroAbi/galaxy-s9plus-mainline-linux) at `bcee6014fc2e44a4c4f1c34bca0a880d4b543ec3` (`src/ufs/ufs-exynos9810.c`, `src/mainline-dts/exynos9810-star2lte.dts`) | GPL-2.0 | facts only (the protector's descriptor type and PRDT entry size, 4 KB entries, OCS 7 for device errors, LUN 0's layout), named in `hakios.ufshc.c`; no code |

ShengUfs's notice:

```
BSD 2-Clause License

Copyright (c) 2026, blu-sharky
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## How to rebuild the inventory

- Tracked non-text/binary files: `git ls-files -z | xargs -0 file --mime-type | grep -v text/`
- License mentions in tracked files: `git grep -li "SPDX-License-Identifier\|GPL\|BSD\|MIT\|APSL\|copyright"`

If a component's origin is uncertain, it is marked **to verify** above and must be treated as not
yet cleared for redistribution until confirmed with its owner.
