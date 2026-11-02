# Release notes (draft)

**Draft, not a release.** No tag has been cut for this. The notes cover `main` since `v0.1.0`, upstream's
open-source release commit (oosexgang, 22 October 2026). The tree's `VERSION` is 2.1.13 (`-hakios`, Darwin 22.5.0,
`xnu-8796.121.2`). `CHANGELOG.md` has the line-by-line record from 29 October on; the commits have the detail.

## Highlights

- **Galaxy S9 (SM-G960F, Exynos 9810), a new device.** Its device repository is
  [hakios-starlte](https://github.com/Scion-Project/hakios-starlte) and its TWRP installer is
  [scion-packaging](https://github.com/Scion-Project/scion-packaging). It boots the iOS 16.5 user space that you build
  from your own IPSW, from the phone's flash (`ufs=2 system_apfs=ufs_SYSTEM`).
- **Android's own hypervisor (AVF/crosvm) as a host** (`avf/`): XNU as a guest on the Pixel, for
  [Scion](https://github.com/Scion-Project/scion). This adds virtio-pci, a virtio GPU with a GLES/Vulkan path through
  gfxstream, display, input, sound, camera, battery, the flashlight and `/private/var` kept across runs.
- **Pixel 10 (`frankel`, Tensor G5 `lga`)**: a device and SoC repository. Its first steps only; see
  [docs/devices/frankel.md](devices/frankel.md).
- **Upstream HakiOS merged in** (the POCO X5 Pro 5G, QEMU, U-Boot, the configuration system, the boot-arguments
  catalogue, HFS Plus `/private/var`, the device/SoC repository split), with every upstream attribution kept.

## Galaxy S9: state on the phone

Seen working on hardware (1 and 2 November 2026):

| Part | Boot-arguments |
|---|---|
| Six cores (A55s 2 and 3, and the four Exynos M3s). Needed: the big cluster's PMU fields written before CPU_ON, PAN off, and user space told only the ARMv8.0 features every core has | `cpus=6 bootcpu=2 m3_prep=3 pan=0` (`isa_floor` comes on with `m3_prep=3`) |
| Touch (S6SY761), polled at priority 92 so that moves do not come in bursts | `s6sy761=1` |
| Battery level and charger state (MAX77705, read only) | `max77705=1` |
| Vibration for iOS | `max77705_haptic=1` |
| The kernel's live log and a line console over USB (DWC3, CDC-ACM 1209:0001) | `usb_gadget=5 max77705_usbsw=1` |
| GPU: CoreAnimation draws through Mesa's Panfrost on the Mali-G72 | `mali=9`, root built with `GL=1` |
| CPU clocks through ACPM's DVFS (the A55s at 1.79 GHz) | `cpufreq_lit=1794000`, `cpufreq_big=` |
| Widgets and lock-screen posters (PosterBoard restarted after the start-up rush) | `poster_retry` (default on; `poster_retry=0` turns it off) |
| The log kept across a restart (exynos-snapshot's buffer, read in TWRP) | `esslog=1` |

Written, but not yet confirmed or tested on hardware:

- brightness from iOS's slider: `panel=1 backlight=2`. Not yet confirmed.
- the front camera, stage 1 (supplies and model ID over I2C): `cam_s9=1..2`. Not yet tested on the phone. The later
  stages (CSIS1's DMA, one sensor frame) are still under review.
- speaker audio (ABOX, MAX98512), Wi-Fi (PCIe and brcmfmac), Bluetooth (BCM4361, HCI only), temperatures (TMU), the
  flashlight, and the kernel's own UFS driver: all written, none seen yet.
- booting on an M3 (`bootcpu=4`): not tried.

Not supported: cellular, and Apple's services (activation, Apple ID, iCloud, the App Store).

## Other changes

- **Graphics:** a GLES encoder and damage tracking for the virtual GPU, state and pipeline caches, and cheaper fences.
  Metal work continues (AIR to NIR through Mesa, Metal on Vulkan/gfxstream). It is partial: buffers and libraries
  work, but nothing is drawn yet.
- **User space:** widgets (`chronod`), `locationd`, the thermal daemon, `/private/var` kept between runs (varstore),
  and a root that can be built without the AIR library (`NO_AIR=1`).
- **Storage:** the kernel's own UFS host controller driver (`ufs_native=`), a RAM block device with AES, and the
  system image mounted from a flash partition (`system_apfs=`).
- **Diagnostics:** `systrace` lines go to the loader's log too, along with the pstore and esslog logs, whole-line
  pstore records, and a hardware watchdog on the S9.
- **Tools:** `tools/boot-args.py` (the catalogue in [boot-args.md](boot-args.md)), `tools/docs-gen.py`,
  `tools/hakios-device.py` (sync, new, lock), and host tests for the S9 panel's gamma, USB-ACM, Mali VA and TMU code.

## Building and installing

- Build: [Building from source](getting-started/building.md). For the S9, follow
  [hakios-starlte's README, "Build"](https://github.com/Scion-Project/hakios-starlte#build).
- Install on the S9: [scion-packaging's README](https://github.com/Scion-Project/scion-packaging#install) gives the
  TWRP installer. Back up `EFS`, `BOOT` and `SYSTEM` first.
- The system image and the loader's identity come from **your own** IPSW (`make unpack IPSW=...`, `make loader`).
  Nothing of Apple's is in these repositories, and an installer you build is yours. Do not publish it.

## Known issues before a release

- `prebuilt/` dates from 25 October and is not an S9 build. Refresh it with `make prebuilt` before tagging, or say so.
- `cpufreq_lit` and `cpufreq_big` are read from a table in `hakios.exynos_acpm.c`, so `tools/boot-args.py` does not
  list them in [boot-args.md](boot-args.md).
- [docs/devices/starlte.md](devices/starlte.md) is generated from the pinned `hakios-starlte` revision. It shows the
  S9's parts as of that revision until `devices/manifest.conf` is bumped and `tools/docs-gen.py` is run again.
- The Samsung audio firmware in `hakios-starlte/firmware/audio/` and the FocalTech touch firmware header have
  redistribution terms still marked "to verify" (THIRD_PARTY_LICENSES.md).
