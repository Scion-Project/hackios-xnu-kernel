# Scion

English · [中文](README.zh.md) · [Українська](README.uk.md)

**Scion runs iOS 16.5 on Android phones, on Apple's open-source XNU kernel and our own drivers, with no emulator on the Galaxy S9.**
This is an experiment, not a finished ROM: you build it yourself from your own copy of iOS, and nothing of Apple's is in these repositories.
Website: **[scion-project.github.io](https://scion-project.github.io)**.

Scion is built on [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel), and this repository is a fork of it.

## Phones

| Phone | State | What to expect |
|---|---|---|
| **Galaxy S9** (SM-G960F, Exynos) | **Works** | iOS on all eight cores: touch, GPU, Wi-Fi, speakers, microphone, front camera, Bluetooth, sensors, vibration, battery. Tested on only a few phones so far |
| **Galaxy S9+** (SM-G965F, Exynos) | **Unproven** | Nobody has run it. The installer does a 2-minute first run that only collects a log, then returns to TWRP. The screen may stay black |
| **Galaxy Note9** (SM-N960F, Exynos) | **Unproven** | The same first run as the S9+ |
| **Pixel 10** | **Virtual machine, in progress** | iOS 16.5 in a VM on the phone's own hypervisor, through the [Scion app](https://github.com/Scion-Project/scion). It reaches the home screen; touch was still being fixed at its last status. It has no ready-made install for other owners yet |

Snapdragon and Korean (`N`) models are not supported, and the installer refuses them.

## Install on a Galaxy S9

You need an unlocked bootloader, TWRP, a computer with about 20 GB free, and your own `iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw`. No ready-made download exists, because the installer contains iOS's own files and is yours alone.

1. **Back up.** In TWRP, back up Boot, System and EFS, and copy the backup to a computer. EFS holds your IMEI.
2. **Build the installer** with [scion-installer](https://github.com/Scion-Project/scion-installer):
   ```sh
   git clone https://github.com/Scion-Project/scion-installer && cd scion-installer
   ./build-installer.sh --ipsw /path/to/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw
   ```
3. **Flash it.** Copy `out/scion-exynos9810-installer.zip` to the phone, install it in TWRP, and reboot to System. Don't use `adb sideload`, which can't send a file over 2 GB.

To leave iOS, hold five fingers on the screen for 1.5 seconds; the phone restarts into TWRP and keeps the log. Restore your Boot and System backups to go back to Android.
The installer's [README](https://github.com/Scion-Project/scion-installer#readme) and [step-by-step notes](https://github.com/Scion-Project/scion-installer/blob/main/universal/TESTING.txt) have the detail.

## Known issues

- **Unexpected restarts:** one tester has reported the phone rebooting by itself. The cause isn't known yet, so please send a log if it happens to you.
- **No mobile network:** no cellular data, calls or SMS. Use Wi-Fi.
- **No rear cameras:** only the front camera works. Headphones don't work either.
- **No App Store or Apple services:** no Apple ID, iCloud, App Store or iMessage, and that won't change. Sileo is included, but refreshing sources and installing packages haven't been tested.
- **Settings may not survive a restart:** if USERDATA still holds Android's data, `/private/var` lives in memory and is emptied at every boot. Keeping it needs a separate installer that formats USERDATA, which erases Android's data.
- **The passcode doesn't protect your files:** there is no Secure Enclave and files aren't encrypted. The passcode protects keychain items only, a 6-digit code can be guessed offline by anyone who reads the flash, and forgetting it loses the keychain ([details](docs/keystore.md)). Don't enable Erase Data.
- **The clock may be wrong at first:** the installer leaves the hardware clock out.

## Send logs

Save the logs in TWRP right after the problem, before anything else, because a full power-off loses them:

```sh
cp /sys/fs/pstore/* /sdcard/ 2>/dev/null; cat /proc/last_kmsg > /sdcard/last_kmsg.txt
```

If `/sdcard` isn't there, use `/external_sd`, or `/tmp` and `adb pull`. Then [report a bug](https://github.com/Scion-Project/scion-kernel/issues/new?template=bug.yml) with `last_kmsg.txt` and your phone's model, or [send a tester report](https://github.com/Scion-Project/scion-kernel/issues/new?template=tester-report.yml) if it worked. Never attach the installer, the system image or your IPSW: they contain Apple's files.

---

# For developers

Everything below is about the kernel itself. Scion's code still uses HakiOS's names (`hakios_*`, `/dev/hakios-*`, `kern.hakios.*`) so that it keeps merging with upstream, so "HakiOS" below means this kernel.

**Apple's open-source Darwin kernel (XNU, xnu-8796.121.2) on Android phones (Qualcomm, Samsung Exynos)**, the way Android is Linux on
phones: a boot loader of its own, drivers of its own, a configuration system and a version of its own. The same kernel runs in
QEMU with nothing of Apple's and nothing of any phone's (`make qemu-open`).

**Documentation: [docs/](docs/index.md)**, also on the project's website (`hakios-web`). Start with [Getting started](docs/getting-started/index.md), [Port a device](docs/porting/index.md) or the
[Architecture](docs/concepts/architecture.md).

## What works

| | POCO X5 Pro 5G (`redwood`) | QEMU `virt` |
|---|---|---|
| Lock screen, home screen, Control Center, the stock apps | yes | yes (slow: the CPU is emulated) |
| Touch, multitouch, buttons | yes | yes |
| All 8 CPU cores, the big one at 2.4 GHz | yes | no |
| GPU rendering on Adreno (OpenGL ES through Mesa) | yes | no |
| Speakers, microphone, the battery's level | yes | sound only |
| Wi-Fi, cameras, vibration | yes | network only |
| `/private/var` kept across reboots, Sileo, SSH | yes | no |
| Metal | partly: a device Metal loads; buffers and libraries; nothing drawn yet | partly |
| Cellular | partly: the modem is up, the SIM bound, the network seen; no registration yet | no |
| Bluetooth, charging under this kernel, Apple's services | no | no |

Every part, as each device's own `device.conf` records it: [Devices](docs/devices/index.md). The rows about the lock screen and the
apps describe what happens when you add layer 2 below yourself.

### Galaxy S9 (`starlte`, SM-G960F, Exynos 9810)

This repository is Scion-Project's fork of HakiOS. Its main addition is the Galaxy S9 port, which boots the iOS 16.5 user space
you build from your own IPSW. The device repository is [scion-starlte](https://github.com/Scion-Project/scion-starlte), and the
TWRP installer comes from [scion-installer](https://github.com/Scion-Project/scion-installer).
[scion-starlte's README](https://github.com/Scion-Project/scion-starlte#readme) says what has been seen on the phone and how to
build and install by hand, with each part's boot-arguments. [docs/devices/starlte.md](docs/devices/starlte.md) is generated from
the device's own `device.conf` at the revision `devices/manifest.conf` pins.

### Galaxy S9+ (`star2lte`, SM-G965F) and Galaxy Note9 (`crownlte`, SM-N960F): written, never run

The same SoC and, by Samsung's device trees, the same board as the S9's for everything the drivers touch, so they share the
S9's drivers and the loader's S9 platform under their own names (`loader_platform=`, written by their image scripts).
**Nobody has run any of it on either phone.** There is no UEFI of their own (Silicium has no target for them): their images
borrow the S9's, only when asked (`mkimage.sh -U`). Their device repositories,
[scion-star2lte](https://github.com/Scion-Project/scion-star2lte) and [scion-crownlte](https://github.com/Scion-Project/scion-crownlte),
say what differs from the S9, where each fact comes from, and what to try first; scion-installer's `universal/` installer
takes all three phones.

## The three layers

1. **The open core**: the kernel port, the loader, the drivers, the tools. All source, in these repositories.
2. **What you put on top**: the kernel can start a user space that you supply. It was built so that it can start the Darwin userland 16.5
   user space, which you lay out yourself from files you download from Apple; nothing of it is shipped, and nothing is
   circumvented to get it. Whether you may do that, and whether you want to, is your decision.
3. **Apple's services** (activation, Apple ID, iCloud, the App Store): not supported and not a goal.

By default the loader goes into RAM through `fastboot boot`, the system lives in RAM for a session, and a reboot brings the phone
back to its own Android. The one thing written by default is the loader's own log (a few KB) to the `logdump` partition; with
`ufs_write=1` the kernel writes only to a partition `darwin` that you create yourself. This is a research bench, not a ROM.

## Quick start

```sh
git clone https://github.com/Scion-Project/scion-kernel.git   # this fork; upstream is oosexgang/hakios-xnu-kernel
cd scion-kernel
make doctor           # what this machine lacks, and what to do
make qemu-open        # the open kernel in QEMU: nothing of Apple's, nothing of a phone's
```

Darwin in QEMU (Linux, 12 GB of disk, Apple's IPSW downloaded for you), and a session on the phone:
[QEMU](docs/getting-started/qemu.md), [On the phone](docs/getting-started/phone.md).

```sh
make redwood_defconfig   # the phone's configuration: the common kernel + the SC7280's + the phone's own
make kernel              # the kernel with the drivers that configuration builds
make phone SECS=600      # a 10-minute session on the phone, in RAM
```

## Repositories: common, SoC, device, UEFI

| Repository | What |
|---|---|
| [scion-kernel](https://github.com/Scion-Project/scion-kernel) (upstream: [hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel/tree/main)) | **common**: the kernel port, every common driver, the loader, user space, the tools, this documentation, and `devices/qemu` |
| [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325) | POCO X5 Pro 5G: its defconfig, status, UEFI platform, firmware script, and its SoC's layer (`soc/`: the Qualcomm SC7280 family) |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | the firmware that starts the loader: Mu-Silicium with HakiOS's boot path, banner and name, and HakiOS's U-Boot (`u-boot/`) |
| [hakios-metal](https://github.com/oosexgang/hakios-metal) | Metal's command model on the Adreno, through the kernel's GPU driver |
| [scion-starlte](https://github.com/Scion-Project/scion-starlte) | Galaxy S9 (Scion fork): its defconfig, status, UEFI platform and prebuilt UEFI, image script, and its SoC's layer (`soc/`: the Exynos 9810) |
| [scion-star2lte](https://github.com/Scion-Project/scion-star2lte), [scion-crownlte](https://github.com/Scion-Project/scion-crownlte) | Galaxy S9+ and Note9: their defconfigs, status (nothing seen), image script, and the same `soc/` as the S9's; no UEFI yet |
| [scion-installer](https://github.com/Scion-Project/scion-installer) | Scion fork: the S9's TWRP installer (boot image to `BOOT`, the system image you build to `SYSTEM`) |

`devices/manifest.conf` in the common repository names each one and the revision it is at (in this fork, the Scion-Project forks of them); `tools/hakios-device.py sync` clones them
into `devices/` and `uefi/`. One repository per device: it carries its SoC's layer, and `tools/hakios-device.py new` makes one. [How the repositories fit](docs/porting/repositories.md).

```sh
git clone https://github.com/Scion-Project/scion-kernel.git && cd scion-kernel
tools/hakios-device.py sync      # the SoC, device and UEFI repositories, at the revisions the manifest names
```

A device of your own is a repository over the common one: `tools/hakios-device.py new` makes it from the phone's own device tree,
with every phone driver left out until it is ported. Each driver is an option of the kernel's own configuration (`CONFIG_HAKIOS_*`,
XNU's `options` and `conf/files`), so a device's defconfig builds exactly the drivers it has.
[Port a device](docs/porting/index.md), [Drivers in the configuration](docs/drivers-config.md).

## What is in the repository, and what never is

In it: only our own code and data: the patch to Apple's open-source XNU (`xnu/hakios.patch`, APSL 2.0) and our files, the boot
loader, our programs and libraries, the tools, `prebuilt/` (built from this source; the loader **without** any Apple data:
`make loader` makes the one that starts Darwin, on your machine, from your own IPSW), and the device and SoC directories.

This fork adds:

| Directory | Contents |
|---|---|
| `avf/` | XNU as a guest of Android's own hypervisor (AVF/crosvm) for [Scion](https://github.com/Scion-Project/scion); see `avf/README.md` |
| `xnu/patches/` | numbered kernel patches applied after `hakios.patch` and the drivers (AVF platform, virtio-pci, virtio-gpu, display, camera, sound, torch, battery) |

Never in it: any file of Darwin (you build the system image on your machine from an IPSW you get yourself and hand to
`sysimage/unpack.sh`; nothing is downloaded for you, decrypted or circumvented); the phone's firmware (pulled from your own phone into
`fw-*/`, which git ignores); keys, certificates, activation, or anything that would make the phone pass as an iPhone to Apple.

## Independent, and Apple-free at the core

HakiOS is an independent research project. It is **not affiliated with or endorsed by Apple**;
"Darwin", "iPhone", "XNU" and "Apple" are named only to describe what the code works with. The core
kernel, the boot loader and the open tools build and run with nothing of Apple's:

```sh
make qemu-open      # the open kernel + a placeholder loader + a tiny raw-syscall user space, in QEMU
```

No IPSW, no Darwin user space, no firmware. Booting the stock Darwin user space (`make qemu`,
`make phone`) is an optional path that needs the IPSW and the phone's firmware, which you supply
from your own device.

## Licenses and the legal side

My code is under the license of the files it changes: the XNU patch under the Apple Public Source
License 2.0 (Apple publishes XNU under it; the terms are met: changes are published as source,
notices are kept), the rest as the file headers say. Darwin, its images, Apple's trademarks and the
phone's firmware belong to their owners, are not in this repository and are not distributed by me;
everyone takes them from their own phone and their own IPSW. "Darwin", "iPhone" and "Apple" are
named only to describe what the code works with. If you believe a file here infringes anyone's
rights, open an issue and I will remove it.

The detail is in separate files: `LEGAL.md` (what is and is not distributed, and the limits of
these statements), `COMPLIANCE.md` (how the APSL 2.0 and other terms are met, and the items still
to verify), `THIRD_PARTY_LICENSES.md` (every third-party component with origin and license), and
`NOTICE` (the short canonical list). None of this is legal advice.

hakios is an experimental project focused on bringing darwin to android hardware. our goal is to make hakios free and accessible to everyone.

owners:
@oodze - founder
@netusd - adversting / ceo-dev
@wannqn - dev 
@hxfuxyyXD - dev
Rhaenja Hun（杭润嘉） — Developer / Sponsor

tym sicilium for the sources!

