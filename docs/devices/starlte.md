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
| display | works | the panel at 1440 x 2960: frames scaled by the DPP's VGF0 and flipped at the DECON's refresh (fb_dpp=1), the window server's own surface scanned out with nothing copied (fb_zc=1, kernel PR 160), the brightness from iOS's slider through AOR and gamma on DSIM0 (panel=1 backlight=2 brightness_bridge=1, kernel PRs 150, 154): all defaults, seen on the phone, 2 and 3 November; the buffers' and fb_zc's pool's memory in sys/hakios.exynos9810_mem.h |
| cpu | partly | six cores running iOS, the A55s 2 and 3 and the four M3s (cpus=6 bootcpu=2 m3_prep=3 pan=0 isa_floor=1, defaults): seen on the phone, 2 November; the clusters' rates moved with the load, the touch screen and the heat within Samsung's limits (cpufreq_gov=1, a default; kernel PRs 147, 158): seen on the phone; core 1 is parked by S-Boot and not reached (README.md, The cores); the RTC's time of day (rtc_pmic=1, a default): seen on the phone, 2 November |
| touch | works | Samsung S6SY761 on USI03's HSI2C, ten fingers (s6sy761=1, a default): seen on the phone, 1 November |
| buttons | partly | power, volume, Bixby as home (exynos_keys=1, a default): on in every run since 1 November; each key's events not yet recorded in a run log here |
| storage | works | iOS's system from SYSTEM through the kernel's own UFS driver after a self-test against the loader's reads, writes fenced to USERDATA (ufs=3 ufs_write=1 system_apfs=ufs_SYSTEM, defaults; kernel PR 155, starlte PR 33); /private/var kept between runs in USERDATA, HFS Plus made once from TWRP (var_hfs=ufs_USERDATA_data, a default; starlte PR 26): seen on the phone |
| gpu | partly | Mali-G72 MP18 (mali=9, its clock after its load mali_gov=1, defaults; HakiosMali for Mesa's panfrost: OpenGL ES, GPU.md): seen on the phone, CoreAnimation draws through it; Metal not yet run |
| audio | partly | the speakers: ABOX with its CPU off, RDMA0 -> SIFS1 -> UAIF1, I2S 48 kHz 2 x 16 bits, the two MAX98512s on I2C (abox=8 max98512=3, defaults; AUDIO.md, kernel PR 163, starlte PR 38): a tone and iOS's sounds played on the phone, 3 November; the codec, a CS47L93 on PERIC0's SPI (cs47l93=6, a default; kernel PR 164, starlte PR 39): its ID, power, clocks (FLL) and main microphone, seen on the phone, 3 November; that microphone is iOS's "iPhone Microphone" through UAIF0 -> WDMA1 -> /dev/hakios-mic (abox_mic=1, not a default: the defaults' line has no room; kernel PR 169, starlte PR 43), recorded by an app on phone 1, 3 November; headphones written behind cs47l93_hp=1, not yet tried |
| battery | works | the MAX77705's gauge and charger, read only, and the thermistor (ADC), into iOS's battery (max77705=1, a default): the level and charger state seen on the phone, 1 November |
| wifi | works | BCM4361 on PCIe (wifi_pcie=6) with Linux's brcmfmac, the module from the chip's OTP, the phone's address, joining WPA2 from iOS's Settings, DHCP, the Internet (brcmfmac=8 brcmfmac_poll=1 brcmfmac_tcm_idx=1 wifi=1, defaults; firmware from firmware/wifi or the phone's VENDOR; WIFI.md, kernel PR 157, starlte PR 34): seen on the phone, 3 November |
| network | works | Wi-Fi's en0 with configd's DHCP, the gateway and the Internet answering (WIFI.md, stages d and e) |
| bluetooth | partly | BCM4361's Bluetooth on UART_BT (out of its USI reset), BT_EN on the PCAL6524, Broadcom's patch from the phone's VENDOR, 4 Mbaud with RTS/CTS, an inquiry and an LE scan from the USB console (bt=7, not a default: it would take the defaults' line's last 5 characters, give it for a run; "bt scan"; seen on phone 2, 3 November); iOS (userland BT=1): BlueTool boots it through the kernel's com.apple.uart.bluetooth with this phone's own scripts, bluetoothd's stack runs on it, Settings > Bluetooth on and scanning (BLUETOOTH.md) |
| cellular | no | the Shannon CP is not started (docs/modem-plan.md) |
| sensors | partly | the sensor hub in the BCM4775 GNSS chip (USI_CHUB00's SPI): its firmware loaded through the chip's ROM, Samsung's sensor protocol, the accelerometer, gyroscope, magnetometer, barometer, light and proximity sensors read at 50 Hz (shub=6, not a default: the defaults' line has no room left, give it for a run; SENSORS.md, kernel PR 167, starlte PR 42): seen on the phone, 3 November; into iOS as HID services and the device orientation (userland/src/inject.c): backboardd took face up, portrait and landscape and the proximity sensor, 3 November; the gyroscope's and magnetometer's units and auto-brightness not yet confirmed |
| haptic | works | the MAX77705's haptic block and a PWM of the SoC, for iOS (max77705_haptic=1, a default): felt on the phone, 1 November |
| thermal | partly | the Exynos 9810's TMU read by the kernel (BIG, LITTLE, G3D, ISP with their fuses), kern.hakios.temp, and the run ended at 110 C or the battery at 60 C (tmu=3, a default): running on the phone since 1 November, the stop not yet reached; the cores slowed from 83 C (big) and 76 C (little) by the CPU frequency governor; nothing slows the GPU |
| camera | partly | the front camera's S5K3H1 (no ISP firmware; CAMERA.md): auto-exposed, demosaiced at 1632 x 1224, streaming at 30 frames a second into /dev/hakios-camera and the stock Camera app's preview (cam_s9=7 cam=5, defaults; kernel PRs 159, 162, starlte PR 36): seen on the phone, 3 November; its memory at 0xD8000000 (sys/hakios.exynos9810_mem.h); the rear cameras missing |
| torch | partly | the S2MPB02's flash block, lit by a pin (torch=1 a look, 3 for iOS; not a default): written, not yet seen |
| usb | works | the port as a USB serial device: the kernel's whole log live and a line console (usb_gadget=5 max77705_usbsw=1, defaults; DWC3 in device mode, CDC-ACM 1209:0001, the 2.0 PHY by the BSP's CAL sequence): seen on the phone, 1 November; a run's return to TWRP from the console (return): seen on the phone |

The state is what has been seen on this device, written down in `devices/starlte/device.conf`; a part the table does not name has not been looked at.
