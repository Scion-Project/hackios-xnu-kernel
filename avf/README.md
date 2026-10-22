# AVF / crosvm platform

Scion runs HackiOS's XNU as a hardware-virtualized guest on Android's own hypervisor (pKVM), through
the Android Virtualization Framework (AVF), instead of emulating an iPhone. AVF's custom VMs boot
a Linux kernel `Image`, an initrd and kernel parameters under crosvm. Here:

- the `Image` is **avfboot** (`shim/`): it does under crosvm what `xnuloader/loader.efi` does under UEFI;
- the initrd is the loader's XNUP payload (`xnuloader/mkpayload.py`: `kernel=`, optional `ramdisk=`, cmdline);
- the VM's kernel parameters are appended to XNU's boot-args.

## avfboot

crosvm loads the Image at the start of RAM (0x80000000), the initrd 16 MB-aligned right behind it and
its FDT at the top of RAM, and enters at EL1 with the MMU off and x0 = FDT. avfboot relocates itself
(PIE), then from the FDT reads:

| | crosvm (Pixel 10) | QEMU virt |
|---|---|---|
| RAM | 0x80000000 + VM size | 0x40000000 + `-m` |
| GICv3 | GICD 0x3fff0000, GICR below it (0x20000 per CPU) | 0x08000000 / 0x080a0000 |
| virtual timer | PPI 11 (INTID 27) | same |
| PSCI | hvc | hvc |
| console | ns16550a at 0x3f8 (AVF's captured console) | PL011 at 0x09000000 |
| RTC | PL031 at 0x2000 | PL031 at 0x09010000 |
| PCI | pci-host-cam-generic, config space 0x2e000000 | pci-host-ecam-generic |

It puts XNU's window at the first 32 MB boundary above itself and the payload (up to the end of RAM
less the 512 KB panic-log region), copies the kernel's segments and the RAM disk, and writes the
same Apple device tree and `boot_args` as the loader: iPhone X identity, `/chosen` (dram, `psci-method`,
`boot-epoch` from the RTC, NVRAM image), `/cpus` from the FDT's MPIDRs, `/arm-io/gic`, `/arm-io/uart0`
(`ns16550a` or `arm,pl011`), `/pci0` for the virtio-pci host driver, `/pram`. There is no
return-to-loader path: on a panic the kernel stays put and the VM is stopped from outside.

Kernel side: `xnu/patches/1000-avf-ns16550-console.patch` drives the 8250 UART (crosvm accepts only
byte-wide accesses to it).

## Build and run

```sh
avf/shim/build.sh                                   # clang + ld.lld -> avf/shim/out/Image
python3 xnuloader/mkpayload.py -o payload.bin kernel=mach.development.bcm2837 ramdisk=hello
avf/qemu-avf.sh payload.bin 60                      # QEMU virt, hvf/kvm/tcg
avf/run-probe.sh avf/shim/out/Image payload.bin -s SERIAL --mem 2048 --params "-v"   # phone, AVF probe app
```

`hello` is `prebuilt/userland/hello` of hackios-xnu-userland: with it as the RAM disk the kernel's
mockfs serves it as `/sbin/launchd`, and it prints `Hello from user space on Darwin!` as process 1.

`run-probe.sh` drives the AVF probe app of dylanneve1/scion-avf (`probe/`). It takes the phone's
`/data/local/tmp/pixel.lock`, grants the VM permissions and the hidden-API exemption the custom-VM
API needs, and puts all of that back before it releases the lock.

## Status (2026-10-22)

| Where | Result |
|---|---|
| QEMU virt, TCG cortex-a76, 1 and 4 CPUs | PID 1 (`hello` as `/sbin/launchd`) |
| QEMU virt, hvf `-cpu host`, 1 CPU (also `pmu=off`) | PID 1 |
| QEMU virt, hvf, 4 CPUs | hangs before the kernel's first line (not looked into) |
| **Pixel 10, AVF non-protected VM, 2 GB, 1 vCPU** | XNU console on AVF's captured console, md0 root, PID 1 |
| **Pixel 10, `--match-host-cpus`, `cpus=8`** | 8 of 8 processors running, PID 1 |

Pitfall: crosvm rejects VM parameters that begin with `-` after the first word (`--params "-v cpus=8"`
fails with "Unrecognized argument: 8"); put the `-v` last (`--params "cpus=8 -v"`).
