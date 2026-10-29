<!-- Made by tools/docs-gen.py from devices/frankel/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Pixel 10 (`frankel`)

| | |
|---|---|
| Vendor | Google |
| SoC | `lga` |
| Configuration | `make frankel_defconfig` |
| Maintainer | Scion (README.md) |
| Repository | [`devices/frankel/`](https://github.com/oosexgang/hakios/tree/main/devices/frankel) |

## What works

| Part | State | |
|---|---|---|
| display | partly | the boot loader's framebuffer (1080 x 2424) as the shim finds it (the DPU's layer, or scion.fb= on the boot image's command line), through the loader: written, not yet seen; the panel is a command-mode DSI panel, whether a write reaches it without a frame trigger is unknown |
| cpu | partly | core 0 alone (the first boot); all eight with bootcpu=7 cpus=8 through the shim's PSCI (EL2 to EL1): seen in QEMU, not on the phone |
| touch | no | FocalTech on SPI (0x0DB33000), shared with the AOC; interrupt on an AOC pin |
| buttons | no |  |
| storage | no | Google's UFS host at 0x3C400000; the root is in RAM (the payload) |
| gpu | no | PowerVR DXT-48-1536 |
| audio | no |  |
| battery | no | MAX77779 fuel gauge |
| wifi | no |  |
| bluetooth | no |  |
| cellular | no |  |
| thermal | no |  |
| usb | no |  |

The state is what has been seen on this device, written down in `devices/frankel/device.conf`; a part the table does not name has not been looked at.
