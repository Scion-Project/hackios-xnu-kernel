# Release notes (draft)

**Draft, not a release.** No tag has been cut for this. The notes cover `main` since `v0.1.0`, upstream's
open-source release commit (oosexgang, 22 October 2026). The tree's `VERSION` is 2.1.13 (`-hakios`, Darwin 22.5.0,
`xnu-8796.121.2`). `CHANGELOG.md` has the line-by-line record from 29 October on; the commits have the detail.

## Highlights

- **Galaxy S9 (SM-G960F, Exynos 9810), a new device.** Its device repository is
  [hakios-starlte](https://github.com/Scion-Project/hakios-starlte) and its TWRP installer is
  [scion-packaging](https://github.com/Scion-Project/scion-packaging). It boots the iOS 16.5 user space that you build
  from your own IPSW, from the phone's flash (`ufs=3 system_apfs=ufs_SYSTEM`).
- **Android's own hypervisor (AVF/crosvm) as a host** (`avf/`): XNU as a guest on the Pixel, for
  [Scion](https://github.com/Scion-Project/scion). This adds virtio-pci, a virtio GPU with a GLES/Vulkan path through
  gfxstream, display, input, sound, camera, battery, the flashlight and `/private/var` kept across runs.
- **Pixel 10 (`frankel`, Tensor G5 `lga`)**: a device and SoC repository. Its first steps only; see
  [docs/devices/frankel.md](devices/frankel.md).
- **Upstream HakiOS merged in** (the POCO X5 Pro 5G, QEMU, U-Boot, the configuration system, the boot-arguments
  catalogue, HFS Plus `/private/var`, the device/SoC repository split), with every upstream attribution kept.

## Galaxy S9: state on the phone

Seen on hardware by 3 November 2026, each on its own test image. `devices/starlte/configs/starlte.config` holds the
defaults, and [hakios-starlte's README](https://github.com/Scion-Project/hakios-starlte) has the detail.

| Part | Boot-arguments |
|---|---|
| All eight cores (4 A55s, 4 Exynos M3s); the S9's default | `cpus=8 bootcpu=2 m3_prep=3 pan=0` (`isa_floor` comes on with `m3_prep=3`) |
| The low RAM bank too: `hw.memsize` 3375284224 (3218 MB) | default on the S9; `extra_ram=0` turns it off |
| iOS drawn in the panel's shape, 1125 x 2313 (375 x 771 points); needs `libhakios.sb.dylib` from this version | default; `fb_native=0` is the old picture |
| A passcode in Settings and on the lock screen; it gates the screen and encrypts nothing; needs `passcoded` | none; `passcode_reset=1` discards one |
| Storage: the kernel's own UFS driver, `/private/var` in USERDATA | `ufs=3 ufs_write=1 var_hfs=ufs_USERDATA_data` |
| Touch, keys, battery and charger, vibration, thermal watch, USB console, the log kept for TWRP | `s6sy761=1 exynos_keys=1 max77705=1 max77705_haptic=1 tmu=3 usb_gadget=5 esslog=1` |
| GPU through Panfrost, zero-copy display, brightness from iOS's slider | `mali=9 fb_dpp=1 fb_zc=1 backlight=2 brightness_bridge=1` |
| Wi-Fi from iOS's Settings (WPA2, DHCP, the Internet) | `wifi_pcie=6 brcmfmac=8 wifi=1` |
| The front camera in the Camera app at 30 frames a second | `cam_s9=7 cam=5` |
| Speakers through ABOX | `abox=8 max98512=3` |
| Given on the line, not defaults: sensors, microphone in iOS, Bluetooth in iOS (a paired device stays connected; needs `libhakios.bluetoothd.dylib`) | `shub=6 abox_mic=1 bt=7` |

Written, but not working yet:

- the iris camera (`iris_s9=N`, off unless asked): the sensor answers with `iris_s9_smc=1`, and no frame has reached
  memory. `fp_s9=N` panics the kernel on its probe.
- the headphones (`cs47l93_hp=1`).
- booting on an M3 (`bootcpu=4`).

Not supported: cellular, the rear cameras, and Apple's services (activation, Apple ID, iCloud, the App Store).

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
