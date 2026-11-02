<!-- Made by tools/docs-gen.py from devices/star2lte/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Galaxy S9+ (`star2lte`)

| | |
|---|---|
| Vendor | Samsung |
| SoC | `exynos9810` |
| Configuration | `make star2lte_defconfig` |
| Maintainer | Scion (README.md) |
| Repository | [hakios-star2lte](https://github.com/Scion-Project/hakios-star2lte) |

## What works

| Part | State | |
|---|---|---|
| display | no | UEFI's framebuffer, 1440 x 2960 at 0xCC000000 as on the S9, from the S9's UEFI borrowed (mkimage.sh -U); not seen on an S9+; the brightness driver of the S9 left out (this phone's panel is s6e3ha8_star2, another variant); fb_flip and fb_dpp (the same DECON and DPP): written for the S9, not seen here |
| cpu | no | A55s 2 and 3 (cpus=2 bootcpu=2, the default), the S9's; the M3s (m3_prep=3) and the clusters' clocks (cpufreq_lit, cpufreq_gov): the same SoC, not seen on an S9+ |
| touch | no | Samsung S6SY761 (sec_ts, firmware y761_star2) at 0x48 on USI03's HSI2C, its IRQ gpa1-0 and supplies LDO35/LDO43 as on the S9 (s6sy761=1): not seen on an S9+ |
| buttons | no | power gpa2-4, volume gpa0-3/gpa0-4, Bixby gpa0-6, the S9's pins (exynos_keys=1): not seen on an S9+ |
| storage | no | UFS read through the loader and the S9's UEFI (ufs=2), the same controller; /private/var in USERDATA (ufs_write=1 var_hfs=ufs_USERDATA_data): not seen on an S9+ |
| gpu | no | Mali-G72 MP18, the same SoC (mali=N): not seen on an S9+ |
| audio | no | ABOX and the MAX98512s at the S9's addresses, codec CS47L93 as on the S9 (abox=N, max98512=N): not seen, not heard on an S9+ |
| battery | no | the MAX77705's gauge and charger, read only, with this phone's battery: 3500 mAh (fuel gauge capacity 3303), its own thermistor table (max77705=1): not seen on an S9+ |
| wifi | no | BCM4361 on PCIe channel 0 as on the S9 (wifi_pcie=N, brcmfmac=N; firmware from the phone): not seen on an S9+ |
| bluetooth | no | BCM4361's Bluetooth on UART_BT as on the S9 (bt=N): not seen on an S9+ |
| cellular | no |  |
| haptic | no | the MAX77705's haptic block and PWM channel 1 (max77705_haptic=1), at the S9's strength (at most 70 % duty; Samsung drives this motor harder): not felt on an S9+ |
| thermal | no | the TMU (tmu=N), the same SoC; the CPU governor's limits (cpufreq_gov=1): not seen on an S9+ |
| camera | no | rear: S5K2L3 wide (0x20) and S5K3M3 tele (0x5A, the S9 has none); front: S5K3H1 (0x6A), as the S9's: the S9's front-camera stages (cam_s9=N) may apply; nothing tried on an S9+; no driver for the tele |
| torch | no | the S2MPB02's flash block, as on the S9 (torch=N): not seen on an S9+ |
| usb | no | USB serial log and console (usb_gadget=5 max77705_usbsw=1), the same PHY and MUIC: not seen on an S9+ |

The state is what has been seen on this device, written down in `devices/star2lte/device.conf`; a part the table does not name has not been looked at.
