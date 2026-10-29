<!-- Made by tools/docs-gen.py from devices/starlte/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Galaxy S9 (`starlte`)

| | |
|---|---|
| Vendor | Samsung |
| SoC | `exynos9810` |
| Configuration | `make starlte_defconfig` |
| Maintainer | Scion (README.md) |
| Repository | [`devices/starlte/`](https://github.com/oosexgang/hakios/tree/main/devices/starlte) |

## What works

| Part | State | |
|---|---|---|
| display | partly | the panel through UEFI's framebuffer (1440 x 2960 at 0xCC000000): written, not yet seen on the phone |
| cpu | partly | the four Cortex-A55s (cpus=4); the M3 cluster is left off: written, not yet seen on the phone |
| touch | no | Samsung S6SY761 on HSI2C: no driver yet |
| buttons | no | GPIO keys (Silicium's KeypadDeviceLib has the pins): no driver yet |
| storage | no | Exynos UFS: no driver yet; the root is a RAM disk |
| gpu | no | Mali-G72 MP18: no driver |
| audio | no |  |
| battery | no |  |
| wifi | no | BCM4361 on PCIe |
| cellular | no |  |

The state is what has been seen on this device, written down in `devices/starlte/device.conf`; a part the table does not name has not been looked at.
