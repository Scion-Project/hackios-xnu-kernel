# Drivers

The kernel's own drivers, by kind of hardware. They are files of Apple's XNU tree (`bsd/dev/`, `iokit/Kernel/`), kept here so that
they can be found and reviewed by what they do.

```
drivers/
├── map                 each file: its directory here, and its path in the XNU tree
├── <class>/conf        which drivers there are, and which files each one is
├── core/               what every kernel has: version, shared locks, stand-ins for drivers left out, physical memory
├── soc/exynos/         what the Exynos family's drivers share: I2C and SPI on USI, ACPM, the big cluster, the secure OS
├── usb/                DWC3 in device mode, CDC-ACM, the Exynos glue
├── virtio/             virtio over PCI and MMIO and its devices (QEMU, AVF)
├── display/            dpu/ (DECON, DPP), panel/ (S6E3HA8, the backlight service)
├── media/mfc/          the video codec
├── gpu/                adreno/, mali/, vgpu/, and the IOKit object user space talks to
├── camera/             the camera interface and the torch; exynos/ (CSIS, the sensors on it)
├── audio/              /dev/hakios-audio, the mic, the guard, Qualcomm's DSP; abox/, codec/
├── input/              the event queue and buttons; touch/, ssp/ (the sensor hub); the S9's keys
├── power/              battery, thermal, PMIC, frequency; exynos/ (TMU, power off), max77705/
├── storage/            ufs/ (UFSHCI with per-SoC hooks), ext4, sparse and image devices
├── wifi/               the station and Apple80211 surface; lx/ (the Linux-driver layer), brcmfmac/ (BCM4361 on Exynos PCIe)
├── bluetooth/          bcm4361/, the WCN6750's first step
├── modem/, net/, haptic/
```

What each driver is, its files, its boot arguments and which devices build it: [docs/reference/drivers.md](../docs/reference/drivers.md),
made from the conf files. SoC numbers are not here: they are in `socs/<soc>/include/` ([How the repositories fit](../docs/porting/repositories.md)).

| Tool | Does |
|---|---|
| `xnu/apply.sh` | lays Apple's tag, `xnu/hakios.patch`, `xnu/files/` and these drivers into one tree |
| `xnu/export.sh` | the reverse, run before committing; refuses a new `hakios.*` file that `drivers/map` does not list |
| `tools/drivers-check.py` | CI: the map and the directories agree, nothing is listed twice, nothing is kept in two places |

## Adding a driver

Put it in `drivers/<class>/` (a directory of its own if it is more than one file or more than a few hundred lines), add a line to
`drivers/map`, a `driver` block (or a `standard` line) to `drivers/<class>/conf`, then `python3 tools/drivers-check.py`. The
`bsd/conf/files` and `iokit/conf/files` lines are made from the conf files (`tools/hakios-config.py kernel`). If the rest of the
kernel calls the driver, a stand-in for when it is left out goes into `core/hakios.stubs.c`. A driver started by a boot argument has to
be in `docs/boot-args.md` (`python3 tools/boot-args.py --check`).

Every driver opens with a comment of this shape, short enough to read at a glance:

```c
/*
 * What it drives, on which hardware, in one or two sentences.
 *
 * What the hardware needs that the code cannot say: ordering, what the firmware leaves behind, why a workaround exists.
 *
 * <name>=  each boot argument and what its values do.
 *
 * Sources: the device tree, the GPL kernel's files read for the facts, public ports. Not yet run on a phone, if so.
 */
```

Logs go through `hakios_say()` (`sys/hakios.log.h`). Addresses, interrupts and PMU and CMU offsets go in the SoC's header, under
`EXYNOS_*`; the driver keeps its own names for them. What has been seen working on a phone goes in the device's `device.conf`.

## Reviews

`docs/driver-review/` has one read-only audit per group (what was checked, what was found, how sure). They are
findings to verify, not a certificate: read the report before relying on a driver, and fix findings in the driver,
not in the report.

The head of each driver says what was checked about it (compiled, run in QEMU, run on the phone) and what is a guess.

The kernel is built from the C and C++ files only; the drivers need no files that are not in this repository,
except the touch firmware header (`input/hakios.focaltech_fw.h`), which `tools/provenance.allow` marks as pending.
