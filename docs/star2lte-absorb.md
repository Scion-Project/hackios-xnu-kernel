# Absorbing the community S9+ port

Source: [danstronics/ios-s9plus-exynos](https://github.com/danstronics/ios-s9plus-exynos), a Scion-based
port that runs iOS 16.5 on one Galaxy S9+ (SM-G965F, `star2lte`). Its repository was read as data only; nothing from it
was run. This page lists what it has that this tree does not, what was taken, and what was left.

## Licence position

* Its `LICENSE` says two licences apply by directory: the kernel parts (`xnu/hakios.patch`, `xnu/files/`, `drivers/`) are
  APSL 2.0 derivatives of Apple's XNU; "everything else that is ours" (loader, `userland/src/`, tools, scripts, docs, device
  layer) is MIT, (c) 2025-2026 the HakiOS authors. `NOTICE` and `SOURCES.md` add: no blanket licence for every component,
  original notices stay with the source, Mesa keeps its per-file licences, Metal/Vulkan work carries MIT and GPL-2.0+/LGPL-3.0+.
* Taken as code: `touch_queue.h` and its test (MIT, ours-layer files), and the Panfrost mapping-cache patch (a patch to
  Mesa 25.0.7, so Mesa's licence applies to the lines it adds). Each carries a header naming the source.
* `core/` is a Scion snapshot with their changes, so most of it is already ours; the APSL parts that differ were not copied,
  only read for facts.
* Not taken: anything in `metal-wip/` (GPL-family metal2vulkan patch; summary only below), and nothing Apple or vendor:
  they ship no SYSTEM image, firmware or MAC. This tree must not gain any either.

## Where their tree stands against ours

Their `core/` is an older Scion revision (`67297be`). On the framebuffer, touch, Mali and Mesa DRM code ours is
**ahead**, so a file diff is mostly noise. Already ours: Bixby as Home (`hakios.exynos_keys.c`), `fb_native` (375-point-wide
canvas on the full panel), swap-wait before the GPU draws into the scanned surface (their "tearing" fix is `fb_swapwait=1`,
which ours turns on with `fb_zc`), slow-frame filtering, `fb_dpp`, `isa_floor`, `mali_gov`, the cpufreq governor, `ufs=2`.

## Candidates

| Topic | Theirs | Status here |
|---|---|---|
| Touch backlog / latency | `touch_coalesce=1`: a slow reader gets the newest pending MOVE; DOWN/UP kept | **Ported** (PR "touch_coalesce"), off by default |
| Mesa mapping cache | `HMC=1` keeps a cached BO's CPU mapping (`panfrost-cache-mapping.patch`) | **Ported** as `tools/mesa/panfrost-mapping-cache.patch`, env-gated; applies to Mesa 25.0.7, untested on a phone |
| Mesa pack pointer | `panfrost-pack-pointer.patch` | Already covered by our `panfrost-cast-and-pack.patch` |
| Mesa advisory/DRM client | `HFT=1` versioned submit trap, `HWC=1` private-BO completion cache (`hakios.drm.c`, `hakios.mali_fast.h`, `hakios-wait-cache.h`) | Left: needs a matching kernel trap in `hakios.mali.cpp` (not in ours) and a phone to show a gain |
| Flicker / tearing | `fb_swapwait`, zero-copy hold, surface-hash skip (`glhash=1\|2`, XXH3 in `eagl.c`) | Swapwait is ours; `glhash` left (userland, changes what is drawn, needs frame timings from a phone) |
| 1188x2442 canvas, status-bar insets | `fbwidth=1188 fbheight=2442 fb_nativeui=2`: MobileGestalt kept at the iPhone X identity, `display_native.c` injects logical geometry, insets and keyboard layout hooks for the curved edges | Left: ours solves the shape with `fb_native` instead; theirs is a private dyld-injected UIKit shim built on the older framebuffer code. Worth a trial on a phone once someone has one; the 1188x2442 numbers and the supported-size list (1368/1440x2960, 1080x2220, 1188x2442) are recorded here |
| Render-thread CPU placement | `fb_cpu=N` binds backboardd's swap thread (`hakios_mali_bind_current_cpu`) | Diagnostic only; left |
| Bixby as Home | `exynos_keys.c` | Already ours |
| Per-phone MAC | Public BOOT template has MAC slot cleared (18-byte `fw/mac.info` at one offset); installer reads `/efs/wifi/.mac.info` in TWRP, checks size, hex format, unicast bit, not all-zero, then writes it into a RAM copy only | Policy matches ours (no MAC in the repo). Design notes kept; no code taken, the offsets are their build's |
| Installer verification keeping TWRP | `installer/update-binary`: star2lte-only, bootloader string, BOOT size, SYSTEM sha256 before any write, RAM backup of BOOT, hash of the built image before write, `dd conv=fsync`, full read-back, RECOVERY hash unchanged, rollback from RAM on failure, writes BOOT only | Pattern worth copying into `scion-installer`; every hash in it is their build's, so it would be rewritten rather than copied |
| Memory / cores | `cpus=6 bootcpu=2`; contiguous bank only; "full RAM and all cores unfinished"; `star2lte-ram.md` records 4 GB high banks `0x880000000`/`0x900000000` and the first bring-up steps | Notes only. `star2lte-ram.md` has addresses worth cross-checking with `docs/devices/star2lte.md` |
| GPU voltage | `mali_uv=750000` and `star2lte-gpu-stock.config`: the S9+ sample's stock ASV top is 750 mV, not the 950 mV the old driver used | Left: our Mali driver sets its own rail (`hakios.mali.c`, B6OUT capped at 0x68) and the figure is **one phone's** ASV table. Candidate for a per-device argument once measured on a second phone |
| Installed command line | `loader_platform=star2lte ... cpus=6 bootcpu=2 mali=9 ufs=2 ufs_native_ro=1 s6sy761=1 exynos_keys=1 fb_dpp=1 tmu=3 max77705=1 usb_gadget=5 wifi_pcie=6 brcmfmac=8 ...` (full line in their `profiles/installed-gles.json`) | A hardware-verified line for one phone. Several of these are not in our catalogue (`mcp`, `mcs`, `fb_cpu`, `fb_nativeui`, `glhash`) so a `configs/star2lte-installed.config` is a follow-up, not guessable here |
| `metal-wip` | AdrenoMetal frontend + metal2vulkan patch, native Vulkan for IOSurface import, unfinished kernel cache-range API; 104 Metal checks and 56 Vulkan-import checks reported in development; per-line cache trap about 163 ms for 12 MiB | Summary only. Not default, not 60 FPS, GPL-family patch: not copied |

## What was not done

No phone was attached and nothing was booted. Items marked left need hardware to judge, a kernel counterpart, or a decision
on a UIKit injection shim.
