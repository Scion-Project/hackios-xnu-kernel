# A modular kernel: how this tree maps onto UniXNU's layers

What it would take for this kernel to sit on pristine XNU with an ordered patch series and separate drivers, as
[UniXNU](https://github.com/UniXNU/UniXNU) does. This page describes; nothing here has been converted. Read from their README,
`components.toml`, `feeds.toml`, `docs/porting/index.md` and `UniXNU-sheng/board.toml` on 5 November 2026.

## Layers

UniXNU has six layers: boot image, UEFI firmware, boot manager, XNU loader (DarwinBoot), kernel, user space. Source is split into
shared code (UniXNU and its component repositories), SoC repositories and device repositories.

| UniXNU | Here | Difference |
|---|---|---|
| `feeds.toml`: pinned XNU and other Apple sources, never copied | `xnu/apply.sh` fetches Apple's tag; `xnu/hfs/hfs.sh` fetches HFS | same idea; ours is written in scripts, not data |
| `patches/xnu/series`, per-tree: arch, then SoC, then OS | `xnu/hakios.patch`, `xnu/patches/NNNN-*`, `xnu/files/` | ours is one large patch plus a numbered set |
| `components.toml`: 26 components, each its own repository, with `hardware`, `os_abi`, `verified_devices`, `build_stage` | `drivers/<class>/conf`, `drivers/map`, built in by `CONFIG_HAKIOS_*` | ours are in one tree, chosen at build; `conf` has no `os_abi` or `verified_devices`. `device.conf` status lines cover the second |
| `build_stage`: kernel, prelink, embedded, payload, firmware | everything linked into the kernel | we have no prelinked drivers and no kits |
| `UniXNU-<soc>-common` | `socs/<soc>/` | the same layer. `socs/exynos9810` holds the numbers; shared Exynos drivers read them through `sys/hakios.exynos_soc.h` |
| `UniXNU-<device>/board.toml`: name, SoC, kit, partition LBAs, defconfig, config fragments, patch series, firmware | `devices/<name>/device.conf`, `configs/`, `mkimage.sh` | ours has status and no firmware recipe; the patch series is shared by all devices |
| `uefi-<device>` | `hakios-uefi`, one for all; `devices/<name>/uefi/` | one tree here against one per device there |
| DarwinBoot, a UEFI driver | `xnuloader/` | the same job. UniXNU's boot manager has no counterpart here: UEFI starts the loader directly |

The kernel layer is where the two differ most. UniXNU pins xnu-10063 and xnu-12377; this tree is on xnu-8796.121.2.

## How much is ours in XNU proper

Changed lines (added plus removed) in Apple's files, counted from the patches on 5 November 2026:

| Directory | `hakios.patch` | Numbered patches | Sum |
|---|---:|---:|---:|
| `osfmk/arm64` | 391 | 1370 | 1761 |
| `bsd/dev` (mostly `hakios.input.c`, a driver of ours) | 136 | 811 | 947 |
| `osfmk/arm` | 68 | 268 | 336 |
| `bsd/kern` | 333 | 95 | 428 |
| `iokit/Kernel` | 58 | 376 | 434 |
| `bsd/vfs` | 161 | 0 | 161 |
| `osfmk/kern` | 130 | 9 | 139 |
| `pexpert` | 19 | 125 | 144 |
| `bsd/conf`, `osfmk/conf`, `iokit/conf`, `config`, `makedefs` | 212 | 1 | 213 |
| the rest | 165 | 108 | 273 |
| All | 1673 lines, 82 files | 3163 lines, 39 files | 4836 |

About 4800 lines, so a `patches/xnu-8796/series` would be small. Much of the numbered set's `bsd/dev` part changes our own drivers and
would be folded into them. Beside the patches:

| Kind | Size |
|---|---|
| `drivers/` | 102 files, about 100,000 lines |
| `xnu/files/`: filesystems (`bsd/miscfs`, 32 files) | 17,300 lines |
| `xnu/files/`: `iokit/Kernel`, the display and framebuffer classes user space needs | 9,300 lines |
| `xnu/files/`: `osfmk/minicrypto` | 6,400 lines |
| `xnu/files/`: `bsd/sys` headers | 3,500 lines |
| `xnu/files/`: `osfmk/arm64` | 2,700 lines |

Only the first is a clean driver. Whether `bsd/miscfs` and `iokit/Kernel` count as components or kernel changes needs a decision per file.

## What each driver uses of XNU

From a scan of the source for each interface, so this shows what is used, not how deeply. The right-hand column is a guess: none of this has
been built against another XNU.

| Driver | Lines | Uses | On xnu-10063 and xnu-12377 (guess) |
|---|---:|---|---|
| DWC3 and USB glue | 2500 | `ml_io_map`, sysctl, boot args, threads, delays, kalloc | likely holds; `kalloc` forms change between releases |
| UFS | 7500 | `ml_io_map`, cdevsw, devfs, sysctl, boot args, threads, locks, pmap, interrupts, SMC | block device registration likely holds; pmap calls are the risk |
| MFC | 1200 | `ml_io_map`, cdevsw, devfs, boot args, threads, locks, pmap, interrupts, copyin, SMC | likely holds |
| Mali | 2900 | `ml_io_map`, IOKit, sysctl, boot args, threads, locks, pmap, kalloc, copyin, SMC | the IOKit user-client side depends on what the OS expects of a GPU, not only on XNU: unknown |
| DPU | 700 | `ml_io_map` | holds; the risk is in `xnu/files/iokit/Kernel` above it |
| ABOX | 2200 | `ml_io_map`, boot args, threads, delays, pmap, SMC | likely holds |
| Camera (CSIS) | 2900 | `ml_io_map`, cdevsw, devfs, boot args, threads, locks, pmap, SMC | likely holds |
| Kinibi TEE | 1300 | boot args, threads, pmap, interrupts, kalloc, SMC | likely holds |
| brcmfmac and PCIe | 1800 | `ml_io_map`, boot args, threads, locks, delays, pmap, interrupts | likely holds; the Apple80211 surface above it is OS-dependent: unknown |
| S6SY761 | 500 | `ml_io_map`, boot args, threads, delays | holds |

## The seam

A driver should depend on a small interface, with its XNU touch points in one place, and SoC numbers in `socs/`, board numbers in the device
repository. Today:

- Done: SoC numbers (`sys/hakios.exynos_soc.h`), the log (`sys/hakios.log.h`), and a directory per sizeable driver.
- Not done: most drivers declare what they use of XNU with their own `extern` lines (`ml_io_map`, `delay`, `kernel_thread_start`). One header per
  group of these, then per-driver `*_xnu.c` where the driver has more than a few calls (MFC, UFS, Mali), is the next step.
- Not done: board wiring such as which USI a chip is on still sits in each driver, though the addresses are named in the SoC's header. That data
  belongs in the device repository.

`boot-args` are read directly with `PE_parse_boot_argn`, which has been stable in XNU for years, and need no wrapper.

## Licences

This is a reading of the files, not legal advice, and it needs a person's review before anything is contributed.

| Our files | Licence | With BSD-2-Clause-Patent |
|---|---|---|
| `xnu/hakios.patch`, `xnu/files/`, `drivers/` | APSL 2.0 (`LICENSE`: treated as a derivative of XNU) | can sit beside it in a separate repository or patch set, keeping APSL; cannot be relabelled. UniXNU's own XNU patches face the same question |
| `xnuloader/`, `userland/src/`, `tools/`, docs, `devices/`, `socs/` | MIT | compatible, keeping the notice |
| Upstream HakiOS's part of each | as above, by the same `LICENSE` | Scion cannot relicense what it did not write |
| `wifi/lx/include/` | GPL-2.0 for part of the LinuxKPI and Linux headers (`THIRD_PARTY_LICENSES.md`) | needs review on its own, as it does today |
| `brcmfmac`, `ath11k` | ISC, BSD-3-Clause-Clear, fetched and built, not kept | compatible |

Drivers state that Samsung's GPL kernel was "read for these facts, not copied". Facts are generally not protected, but whether a given table of
register sequences stays on the right side of that is a question for a lawyer. `THIRD_PARTY_LICENSES.md` has 5 rows marked to verify, and
`tools/provenance-check.py` fails on `drivers/media/mfc/hakios.exynos_mfc_test.h` (bytes compiled into source, no explanation in `provenance.allow`).

## OS profiles

`profiles/<name>.conf` says what a build is for: XNU's tag and commit, the patches, the ADT, what user space expects. `OS_PROFILE` picks one, default
`ios-16.5`. `xnu/apply.sh` takes the tag from it, and `tools/os-profile.py` fails the build for any profile not `real`. `ios-27` is named and fails:

```
$ OS_PROFILE=ios-27 make kernel
os-profile: ios-27 is not-implemented: this tree has no kernel for xnu-12377.121.6 (arm64e). Only ios-16.5 builds; docs/modular-kernel.md has what is missing.
```

Not in a profile yet: `xnu-src/xnu-8796.121.2` as a path in `tools/make-kernel.sh`, the `Makefile` and `xnu/export.sh`, and `tools/kernel-version.py`'s Darwin version. No compat layer exists,
because no difference between XNU versions has been established for any driver: it should be added only where one is.

### What iOS 27 needs

UniXNU's `docs/porting/arm64e.md`: iOS and iPadOS 27 are arm64e only, 18 is the last with arm64. An arm64e profile builds XNU with pointer authentication and links the original arm64e kexts. Every enabled
CPU has to implement FEAT_PAuth with address and generic keys; XNU has to write the key registers at EL1; PAC instructions must not trap at EL1 and EL0. Their 24A446 profile pins xnu-12377.121.6, with Darwin 26's
system calls added, and notes that no Darwin 26 XNU source is published yet. Its kernelcache is an `MH_FILESET` with stripped symbols, and their prelink handles that; ours has no prelink at all.

| SoC here | CPUs | FEAT_PAuth | Basis |
|---|---|---|---|
| Exynos 9810 | Exynos M3, Cortex-A55 | no | `exynos9810.config`: the M3s implement ARMv8.0; the A55 is ARMv8.2. So the S9 cannot run iOS 27 |
| SC7280 (SM7325, redwood) | Cortex-A78, A55 | no, by the architecture they implement | guess, from Arm's documents; not read from `ID_AA64ISAR1_EL1` on the phone |
| Tensor G5 (lga, frankel) | Armv9.2 cores | probably | guess: the architecture has it, whether the hypervisor leaves it usable is not known |
| QEMU virt | `-cpu max` | yes | guess: not run here with a PAC guest |

Differences in the driver-facing interfaces between 8796 and 12377, all guesses, as none of this has been built: `kalloc` type-based forms have tightened over releases, which would touch most drivers' allocations; `ml_io_map`
and `PE_parse_boot_argn` are likely unchanged; XNU-side pointer signing would need the function pointers in our `cdevsw` and IOKit tables to be correctly typed, which they mostly are, but the `extern` declarations in drivers
are not checked against the kernel's and would need the interface headers above first; `pmap` calls are the most likely to have moved. The files in `bsd/miscfs` and the IOKit classes in `xnu/files` are the least known.

## Order of work

1. One XNU interface header per group of driver `extern`s, and per-driver interface files for the largest drivers. No change to the code that runs.
2. On a branch, generate pristine xnu-8796 plus `patches/xnu-8796/series` and the drivers from main, mechanically. A kernel from it has to match main's
   apart from version and UUID, the same proof used for the driver tree.
3. Descriptions beside the manifest (`board.toml`-like, `feeds.toml`-like) where they replace or generate what exists: `devices/manifest.conf`, the Apple
   tag in `apply.sh`. No second source of truth.
4. Components as repositories, in the order DWC3, UFS, MFC, Kinibi: DWC3 is device-neutral already, UFS has per-SoC hooks and the most users, MFC and Kinibi
   are one SoC family's.
5. A newer XNU and arm64e, on a device with PAC. Not started, and not for the S9.

## Not determined

- Whether the IOKit classes in `xnu/files/iokit/Kernel` and the `bsd/miscfs` files hold on newer XNU. Not tried.
- Whether UniXNU's series could take any of ours as they are. We did not read their patches beyond the directory names.
- How much of `osfmk/arm64` is GICv2 and Exynos support that could be a SoC series, and how much is core. Needs a read of each hunk.
- Whether `drivers/` must be APSL at all: `LICENSE` says so, and only the authors can change that for their own code.
