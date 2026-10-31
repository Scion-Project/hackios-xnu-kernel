<!-- Made by tools/docs-gen.py from devices/starlte/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Galaxy S9 (`starlte`)

| | |
|---|---|
| Vendor | Samsung |
| SoC | `exynos9810` |
| Configuration | `make starlte_defconfig` |
| Maintainer | Scion (README.md) |
| Repository | [hakios-starlte](https://github.com/Scion-Project/hakios-starlte) |

## What works

| Part | State | |
|---|---|---|
| display | partly | the panel through UEFI's framebuffer (1440 x 2960 at 0xCC000000); brightness by gamma and AOR through DSIM0 (panel=1, kern.hakios.brightness; iOS's slider through hakios.backlight.cpp, backlight=1 logs what iOS sets): written, not yet seen |
| cpu | partly | A55s 2 and 3 (cpus=2 bootcpu=2, the default): seen on the phone; core 1 is parked by S-Boot and the M3s' CPU_ON never returns (README.md, The cores) |
| touch | partly | Samsung S6SY761 on USI03's HSI2C, ten fingers (s6sy761=1): written, not yet seen on the phone |
| buttons | partly | power, volume, Bixby as home (exynos_keys=1): written, not yet seen on the phone |
| storage | partly | UFS read through the loader and Silicium's UEFI (ufs=2); or by the kernel's own UFS driver, read only, as UEFI left the controller (ufs_native=4 ufs_native_soc=exynos9810, ufs_native_step=1..5): written, not yet seen on the phone |
| gpu | partly | Mali-G72 MP18 (mali=N, nine steps; HakiosMali for Mesa's panfrost: OpenGL ES, and Metal through it, GPU.md): written, not yet run on the phone |
| audio | partly | ABOX, its CPU off: RDMA0 -> SIFS0 -> UAIF1, I2S 48 kHz 2 x 16 bits, a tone, then iOS's sound and volume (abox=N, 8 steps); the two MAX98512s on I2C (max98512=N, 3 steps; AUDIO.md): written, not yet heard |
| battery | partly | the MAX77705's gauge and charger, read only, and the thermistor (ADC), into iOS's battery (max77705=1): written, not yet seen |
| wifi | partly | BCM4361 on PCIe: stage 1, the root complex of channel 0, WLAN_REG_ON, the link and the chip's ID and BARs (wifi_pcie=N, 6 steps); stage 2, Linux's brcmfmac, INTx, the phone's address, the station and joining WPA2 (brcmfmac=N, 8 steps; firmware from the phone; WIFI.md): written, not yet seen |
| bluetooth | partly | BCM4361's Bluetooth on UART_BT: BT_EN on the PCAL6524 expander, the UART in loopback, HCI_Reset and Read Local Version, Broadcom's patch from the phone and the version again (bt=N, 6 steps; no host stack; BLUETOOTH.md): written, not yet seen |
| cellular | no |  |
| haptic | partly | the MAX77705's haptic block and a PWM of the SoC, for iOS (max77705_haptic=1): written, not yet felt |
| thermal | partly | the Exynos 9810's TMU read by the kernel (BIG, LITTLE, G3D, ISP with their fuses), kern.hakios.temp, and the run ended at 110 C or the battery at 60 C (tmu=N, 3 steps; tmu_off=1 powers off): written, not yet seen; nothing slows the cores or the GPU |
| torch | partly | the S2MPB02's flash block, lit by a pin (torch=1 a look, 3 for iOS): written, not yet seen |
| usb | partly | the port as a USB serial device: the kernel's whole log live and a line console (usb_gadget=N, 5 steps; DWC3 in device mode, CDC-ACM 1209:0001, the 2.0 PHY by the BSP's CAL sequence): written, not yet seen |

The state is what has been seen on this device, written down in `devices/starlte/device.conf`; a part the table does not name has not been looked at.
