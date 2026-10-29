# Samsung Galaxy S9 (starlte, SM-G960F, Exynos 9810)

HakiOS on the Galaxy S9 the way it runs on the POCO: the phone's boot loader starts a UEFI, the UEFI starts `xnuloader`, the
loader starts XNU. Nothing of Android's virtualization is involved (that is the Pixel's route, `avf/`).

**State: written, not yet run on the phone.** The kernel's and loader's half is tested in QEMU with the same interrupt
controller (`qemu/run.sh` with `QEMU_GIC=2`). What is known and what is not is below; `device.conf` is the record.

## How it starts

```
S-Boot (Samsung's, unlocked)  -->  boot image on RECOVERY  -->  HakiOS UEFI                    -->  xnuloader  -->  XNU (EL1)
  Vol Up + Bixby + Power            BootShim + FD + XNUP         Mu-Silicium's starltePkg,           (platform        GIC-400, 4 x A55,
                                                                 Carbon Hypervisor at EL2 (PSCI),    "starlte")       UEFI's framebuffer
                                                                 the panel's buffer as GOP
```

- **The UEFI** is [Project Silicium's](https://github.com/Project-Silicium/Mu-Silicium) `starltePkg` on the `S5E9810Pkg`
  silicon package, with HakiOS's changes (`uefi/`: the boot shim copies the payload, the boot manager starts the loader).
  It is open source throughout, unlike a Qualcomm phone's: no vendor driver is extracted from the phone. The one binary in
  it, `Hyp.bin`, is Silicium's [Carbon Hypervisor](https://github.com/Project-Silicium/Carbon-Hypervisor) (GPL-2.0), which
  takes EL2 and answers PSCI itself, because the Exynos 9810's own firmware does not do it properly. `uefi/BASE` pins a
  Silicium commit of 2026-10-09, older than Carbon's update of 2026-10-13.
- **This port's change to the platform** (`uefi/files/`): the region the payload is copied to, 0xF0000000 + 64 MB, is
  reserved in the memory map. In Samsung's device tree that is the modem's region (`modem_if`); Android's `cbd` puts the modem
  image there and the boot loader does not, and HakiOS starts no modem.
- **The image** (`mkxnuimg.py`, `UEFI_TARGET=starlte`): an Android boot image (header v1) whose kernel is the boot shim, the
  UEFI image and the payload, **uncompressed and without a device tree** (Samsung's boot loader takes the device tree from
  the phone's own `dtb` partition and does not unpack a gzip), as Silicium's `starlte.toml` says.

## The hardware, and where each fact comes from

| What | Value | Source |
|---|---|---|
| RAM | 4 GB: 0x80000000-0xFFFFFFFF and 0x880000000 on; secure holes taken out by Silicium's RamManagerLib | Silicium `SamsungPkg/Library/RamManagerLib`, the device tree's `androidboot.dram_info` |
| Kernel window | the largest free block UEFI reports (the loader's choice) | `xnuloader/loader.c` `choose_window()` |
| Interrupts | GIC-400 (GICv2): distributor 0x10101000, CPU interface 0x10102000 | device tree `interrupt-controller@10100000` (`arm,cortex-a15-gic`) |
| Timer | the architected timer, 26 MHz; virtual timer PPI 27 | device tree `timer` |
| Cores | 0-3 Cortex-A55 ("ananke", MPIDR 0x000-0x003), 4-7 Exynos M3 ("meerkat", 0x100-0x103); PSCI by SMC | device tree `cpus`, `psci` |
| Display | 1440 x 2960, 32 bits, at 0xCC000000 (the boot loader's buffer, `s3cfb.bootloaderfb`); S6E3HA8 panel on DSIM 0x16080000 | Silicium `starlte.dsc`, the device tree |
| Touch | Samsung S6SY761 (`sec,sec_ts`) at 0x48 on HSI2C 0x104B0000 (SPI 0x194) | device tree `touchscreen@48` |
| Buttons | GPIO keys: power, volume up and down, Bixby | device tree `gpio_keys`; Silicium `KeypadDeviceLib` |
| Storage | UFS at 0x11120000 (UniPro 0x11130000) | device tree `ufs@0x11120000`; Silicium `PlatformUfsLib` |
| GPU | Mali-G72 MP18 at 0x17500000 | device tree `mali@17500000` |

The device tree is Samsung's own (Silicium's `Resources/DTBs/starlte.dts`). Pull your phone's
(`adb shell su -c 'cat /sys/firmware/fdt'`) and compare before believing an address from it. The
[WoA port for the S9+](https://github.com/ntdevlabs/exynos9810-woa) and
[ExyHyperBrick's kernel](https://github.com/ExyHyperBrick/android_kernel_samsung_exynos9810) (GPL-2.0: read for facts, never
copied into this tree) are the references for the drivers still to write.

## Why only four cores

The loader gives the kernel the four A55s. The M3s are a second cluster in MPIDR (0x100-0x103), which the loader's
`core_shift` cannot number. They also implement ARMv8.0, while the A55s implement 8.2 like the iPhone X's A11, which iOS 16 is
built for. Whether iOS's arm64 code uses anything past 8.0 (the LSE atomics, for example) decides whether the M3s can ever run
it. Carbon already starts both clusters.

## Build

```sh
make starlte_defconfig && make kernel        # the common kernel, no driver of another phone (configs/starlte.config)
make loader
python3 xnuloader/mkpayload.py -o payload.bin kernel=hakios-kernel ramdisk=userland/root.apfs --cmdline "$(tools/hakios-config.py args)"
UEFI_TARGET=starlte python3 mkxnuimg.py -p payload.bin -o xnu-starlte.img
```

`mkxnuimg.py` takes the UEFI image from `Mu-Silicium/` when there is one (`devices/starlte/uefi/apply.sh`, then
`cd Mu-Silicium && python3 build_uefi.py -d starlte -r RELEASE`), else from `prebuilt/`.

## On the phone

**First, the backups. Samsung has no `fastboot boot`, so every try is a flash.**

1. Make sure the bootloader really is unlocked: Download mode (Vol Down + Bixby, then plug in USB) shows `OEM LOCK: OFF`.
2. In TWRP, or with root, back up `EFS` (the IMEI: losing it is permanent), `BOOT` and `RECOVERY` to the computer.
3. Flash the image to **RECOVERY**, not BOOT, so Android stays bootable: `heimdall flash --RECOVERY xnu-starlte.img`, or
   Odin with the image in a tar, or TWRP's "Install Image" to Recovery.
4. Start it with Vol Up + Bixby + Power, held until the screen comes on. Back to Android: hold Vol Down + Power to restart.
   Flash TWRP back to RECOVERY when done.

What to look for, in this order:
1. HakiOS's logo (the UEFI started).
2. The loader's banner, with `platform starlte, GICv2` and `CurrentEL=1` (Carbon has EL2).
3. The kernel's log, `gic: v2 ... cpu 0 is interface 0`, `4 of 4 processors are running`.
4. launchd.

Photograph the screen as well. The loader's `logdump` is a Qualcomm partition, and this phone has none. Instead, the
loader keeps the log in Samsung's pstore RAM (0xFED14000, 16 KB: the head of the loader's log and the tail of the kernel's),
which a restart leaves alone. Restart into TWRP or Android (Vol Down + Power) and read it:
`adb shell su -c 'ls /sys/fs/pstore; cat /sys/fs/pstore/pmsg-ramoops-0'`. If no file shows up there, the phone's own kernel
lays the zones out differently: `cat /sys/module/ramoops/parameters/*` and the `log_pstore` node say how, and
`platform_starlte.ram_log` in `xnuloader/loader.c` is the one number to change.

## Storage: iOS from the phone's flash

There is no Exynos UFS driver in the kernel, and none is needed for reading. Silicium's UEFI drives the UFS and stays
resident. With `ufs=2 bootcpu=3`, the loader keeps core 0 and reads for the kernel, which runs on cores 1-3: the same
service the POCO uses (`drivers/storage/hakios.ufs.c`). Every GPT partition becomes `/dev/ufs_NAME`, read-only, and
`root_apfs=ufs_NAME` mounts iOS's root volume from one.

The iOS system image (`userland/system.apfs`) needs a partition of its own, written once from TWRP:
`adb push system.apfs /tmp/ && adb shell dd if=/tmp/system.apfs of=/dev/block/by-name/NAME bs=4M`. Which partition
decides what is lost:
- `SYSTEM`: Android stops booting, TWRP still works.
- `USERDATA`: Android's data is wiped.

List the partitions and their sizes first: `adb shell ls -l /dev/block/by-name`.
Then: `--cmdline "cpus=4 ufs=2 bootcpu=3 root_apfs=ufs_NAME"`. Nothing writes to the flash: `ufs_write=1` is for the
POCO's `darwin` partition only, and the loader refuses any other.

## What comes next

| Step | How |
|---|---|
| A longer log | 16 KB of pstore holds a panic but not a whole boot: a partition of the phone's, or USB |
| Storage, writing | /private/var kept between runs needs a writable partition of the port's own (the POCO's `darwin`) |
| Buttons | GPIO keys: a driver, the pins from `gpio_keys` |
| Touch | S6SY761 on HSI2C: an I2C controller driver and the touch protocol (the WoA port's are BSD and GPL; facts only) |
| The M3s | the ISA question above, then a loader that numbers two clusters |
