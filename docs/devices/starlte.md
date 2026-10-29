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
| cpu | partly | the four A55s (cpus=4), all eight with bootcpu=4 cpus=8 (Carbon starts the M3s): written, not yet seen |
| touch | partly | Samsung S6SY761 on USI03's HSI2C, ten fingers (s6sy761=1): written, not yet seen on the phone |
| buttons | partly | power, volume, Bixby as home (exynos_keys=1): written, not yet seen on the phone |
| storage | partly | UFS read through the loader and Silicium's UEFI (ufs=2): written, not yet seen on the phone |
| gpu | no | Mali-G72 MP18: no driver |
| audio | no |  |
| battery | partly | the MAX77705's gauge and charger, read only, and the thermistor (ADC), into iOS's battery (max77705=1): written, not yet seen |
| wifi | no | BCM4361 on PCIe |
| cellular | no |  |

The state is what has been seen on this device, written down in `devices/starlte/device.conf`; a part the table does not name has not been looked at.
