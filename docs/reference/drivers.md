<!-- Made by tools/docs-gen.py from drivers/*/conf and the SoC and device repositories' drivers. Do not edit: change that, then run tools/docs-gen.py. -->


# Drivers

Every driver the kernel can be built with. `CONFIG_HAKIOS_<NAME>=y` in a defconfig builds it in; [Drivers in the configuration](../drivers-config.md) says how, and [Writing a driver](../porting/drivers.md) how to add one.

## Always built

| File | What |
|---|---|
| `hakios.audio.c` | /dev/hakios-audio, the sound Darwin plays, to whatever plays it; and where the camera, flashlight, motor and speaker drivers are started from |
| `hakios.scsnd.c` | SCSND: what iOS plays, to the Scion app's audio ring in the shared framebuffer (patch 1401) |
| `hakios.sccam_torch.c` | the phone's flashlight under AVF, through the Scion app's camera block (patch 1402) |
| `hakios.version.c` | kern.hakios.version, and the version in the boot log |
| `hakios.platform.c` | what the drivers share (the locks of the PMIC arbiter, the always-on processor, RPMh and the SMMU), and kern.hakios.drivers, the list of the drivers built in |
| `hakios.stubs.c` | what the rest of the kernel calls in a driver that is left out: nothing, and "not there" |
| `hakios.stubs.cpp` | the same, for the calls that are C++ (the display asking whether the GPU is done with a surface) (with XNU's `iokitcpp`) |
| `hakios.pci.c` | the AVF guest's PCI bus: crosvm's virtio devices are found on it (patch 1100) |
| `hakios.virtio.c` | the virtio transport (PCI and MMIO) every virtio driver goes through: queues, interrupts, configuration |
| `hakios.virtio_console.c` | the virtio console, the AVF guest's boot log to the host |
| `hakios.vgpu.c` | the virtio GPU (AVF): the 2D display path, the cursor queue and the device's interrupts (patches 1202, 1300-1305) |
| `hakios.vgpu3d.c` | SCGPU: /dev/vgpu3d, OpenGL ES through the Scion app's own gfxstream (its stream ring and doorbell) |
| `hakios.vgpu3d_xnu.c` | SCGPU's glue to XNU: memory, mapping and waiting |
| `hakios.backlight.cpp` | the panel's brightness, whichever phone: panel drivers register (hakios_panel_register), the display service's "brightness" reaches them; boot-arg backlight=1 publishes AppleARMBacklight and says what iOS sets on it, backlight=2 also takes its brightness (with XNU's `iokitcpp`) |
| `hakios.input.c` | /dev/hakios-touch, the queue of touches and buttons, and where the phone's drivers are started from |
| `hakios.net.c` | en0, an interface that is Ethernet to the rest of the kernel, for whatever link carries its frames |
| `hakios.thermal_provider.c` | the SoC's temperature, whichever phone: the driver that reads the sensors registers (hakios_thermal_register), and kern.hakios.thermal_* and hakios_thermal_max_mc() read it; THERMAL and EXYNOS_TMU both register |
| `hakios.scfb_battery.c` | the phone's battery under AVF, as the Scion app writes it into the shared framebuffer (scfb.addr=) |
| `hakios.image.c` | a file of one of the RAM disks by path, for the drivers that load firmware before there is a root |
| `hakios.sparse.c` | /dev/rsp0, a block device over an image that holds only some of its blocks (apfs_sparse=) |
| `hakios.apfs_check.c` | a walk of the APFS volume on /dev/rvblk0 through VFS, to compare with tools/apfs/apfs.py (with XNU's `apfs`) |
| `hakios.lx.c` | the kernel's side of a Linux Wi-Fi driver's object (sys/hakios.lx.h): memory, time, threads, the station. What it asks of the radio (hakios_wifi_*() of sys/hakios.input.h) is the radio driver's: WIFI's (the WCN6750) or BRCMFMAC's (the BCM4361); hakios.stubs.c answers for a kernel with neither, and for the station without WIFI_STA. The object itself is linked in only with one of those two |

## audio

| Driver | Files | Requires | Boot arguments | frankel | qemu | redwood | starlte | |
|---|---|---|---|---|---|---|---|
| **`HAKIOS_ADSP`**<br>the audio DSP started and linked (adsp=1, adsp_step=N); the battery's state comes through it too. Needs the DSP's firmware from the phone (fw-audio/) | `hakios.adsp.c` |  | `adsp=`, `adsp_again=`, `adsp_apr_wait=`, `adsp_keep=`, `adsp_listen=`, `adsp_new=`, `adsp_pause=`, `adsp_return=` and 3 more |  |  | built |  | |
| **`HAKIOS_SPEAKER`**<br>the two speaker amplifiers on I2C, and their watch of temperature and battery | `hakios.speaker.c` |  | `speaker_probe=` |  |  | built |  | |
| **`HAKIOS_GUARD`**<br>the speakers' guard: what the samples go through on their way to the amplifiers where no DSP protects the speakers (a high-pass, three power averages, a peak limiter); for SOUND and EXYNOS_ABOX, nothing of its own | `hakios.guard.c` |  |  |  |  | built | built | |
| **`HAKIOS_SOUND`**<br>the speakers playing (sound=N): what the DSP's services are told, and the feeder, through the speakers' guard that limits what the amplifiers get | `hakios.sound.c` | `HAKIOS_ADSP`, `HAKIOS_SPEAKER`, `HAKIOS_GUARD` | `sound=`, `sound_below=`, `sound_gain=`, `sound_guard_as=`, `sound_pause=`, `sound_secs=` |  |  | built |  | |
| **`HAKIOS_MIC`**<br>the microphones, through the audio DSP (mic=N) | `hakios.mic.c` | `HAKIOS_ADSP`, `HAKIOS_SOUND` | `mic=`, `mic_adc=`, `mic_input=`, `mic_main=`, `mic_secs=`, `mic_tap=`, `mic_test=` |  |  | built |  | |
| **`HAKIOS_VIRTIO_SOUND`**<br>QEMU's virtio sound device as the player of /dev/hakios-audio (virtio_sound=1). QEMU only | `hakios.virtio_sound.c` |  | `virtio_sound=` |  | built | built |  | |
| **`HAKIOS_MAX98512`**<br>the Galaxy S9's two speaker amplifiers (MAX98512s on PERIC1's HSI2C), handed to ABOX's UAIF1 (max98512=N: 1 a look, 2 set up and muted, 3 turned on and off by UAIF1's start and stop) | `hakios.max98512.c` | `HAKIOS_EXYNOS_HSI2C` | `max98512=`, `max98512_gain=` |  |  | built | built | |
| **`HAKIOS_EXYNOS_ABOX`**<br>the Galaxy S9's speaker path through ABOX, Samsung's audio block, with its CPU off (abox=N, the first N of 8 steps: power, clocks, pins, UAIF1, the route, RDMA0 with a tone, then /dev/hakios-audio's sink); the MAX98512s through their driver's hook (sys/hakios.exynos_abox.h) | `hakios.exynos_abox.c` | `HAKIOS_GUARD`, `HAKIOS_EXYNOS_ACPM` | `abox=`, `abox_bclk_on=`, `abox_burst=`, `abox_dvfs=`, `abox_gain=`, `abox_rdma_ctrl0=`, `abox_rdma_ctrl1=`, `abox_route=` and 3 more |  |  | built | built | |

## bluetooth

| Driver | Files | Requires | Boot arguments | frankel | qemu | redwood | starlte | |
|---|---|---|---|---|---|---|---|
| **`HAKIOS_BT`**<br>the Bluetooth half of the WCN6750: step one, a read-only look at its three pins (bt=1) | `hakios.bt.c` |  | `bt=`, `bt_again=` |  |  | built |  | |
| **`HAKIOS_BCM4361_BT`**<br>the Galaxy S9's Bluetooth, stage 1: the BCM4361's Bluetooth half on UART_BT, powered by BT_EN on the PCAL6524 expander, HCI over H4, and Broadcom's patch from the phone (bt=N there, 6 steps: 1 a look, 2 the expander, 3 the UART in loopback, 4 the chip powered, 5 HCI_Reset and its version, 6 the patch and the version again). No host stack. Needs the phone's bcm4361B2 .hcd in fw-bt/ for step 6 (BLUETOOTH.md of starlte) | `hakios.bcm4361_bt.c` | `HAKIOS_EXYNOS_HSI2C` | `bt=`, `bt_expander_reset=`, `bt_hcd=`, `bt_keep=`, `bt_uart_mhz=` |  |  | built | built | |

## camera

| Driver | Files | Requires | Boot arguments | frankel | qemu | redwood | starlte | |
|---|---|---|---|---|---|---|---|
| **`HAKIOS_CAMERA`**<br>the cameras (cam=N): the sensors on the CCI bus and the ISP's interface. The sensors, their tables (fw-camera/) and the pins are the POCO X5 Pro's | `hakios.camera.c` |  | `cam_after=`, `cam_fake=`, `cam_fake_fps=`, `cam_frames=`, `cam_gain=`, `cam_keep_mb=`, `cam_mode=`, `cam_pattern=` and 4 more |  |  | built |  | |
| **`HAKIOS_TORCH`**<br>the flashlight: the flash LED module of the PM8350C (torch=1); it stays off while the battery is too warm | `hakios.torch.c` |  | `torch_max_s=`, `torch_secs=` |  |  | built |  | |
| **`HAKIOS_S2MPB02_TORCH`**<br>the Galaxy S9's flashlight: the S2MPB02's flash block on PERIC1's HSI2C, lit by a pin (torch=N there, the same steps as the POCO's: 1 a look, 2 once, 3 for iOS through /dev/hakios-torch) | `hakios.s2mpb02_torch.c` | `HAKIOS_EXYNOS_HSI2C` | `cam_s9=`, `torch_max_s=`, `torch_secs=` |  |  | built | built | |
| **`HAKIOS_EXYNOS_CAMERA`**<br>the Galaxy S9's front camera, stages 1 to 3 (cam_s9=N): the S5K3H1's supplies on the S2MPB02, its reset and MCLK, its model ID over I2C; the ISPPRE domain, CSIS1, its D-PHY and VC0's WDMA; one frame of the CSIS's test pattern, then one of the sensor's, 1632 x 1224 RAW10, into memory (1 a look, 2 the ID, 3 the domain, 4 the pattern, 5 the sensor's frame; devices/starlte/CAMERA.md has the plan) | `hakios.exynos_camera.c` | `HAKIOS_EXYNOS_HSI2C` | `cam_s9=`, `cam_s9_exp=`, `cam_s9_gain=`, `cam_s9_ipclk=`, `cam_s9_pack12=` | built |  | built | built | |

## core

| Driver | Files | Requires | Boot arguments | frankel | qemu | redwood | starlte | |
|---|---|---|---|---|---|---|---|
| **`HAKIOS_SOCINFO`**<br>the chip's identity from the firmware's shared memory: kern.hakios.soc_* (socinfo=1). Any Qualcomm SoC that keeps SMEM item 137; where the shared memory is comes from soc_smem= | `hakios.socinfo.c` |  | `soc_smem=`, `soc_smem_size=`, `socinfo=` |  |  | built |  | |
| **`HAKIOS_EXYNOS_HSI2C`**<br>an Exynos 9810 HSI2C (the I2C master of a USI block), for the Galaxy S9's touch screen and battery (sys/hakios.exynos_hsi2c.h); nothing of its own on the command line | `hakios.exynos_hsi2c.c` |  |  |  |  | built | built | |
| **`HAKIOS_EXYNOS_ACPM`**<br>the Exynos 9810's power processor asked through its ACPM firmware: the S2MPS18 PMIC's registers and the clocks' DVFS rates (sys/hakios.exynos_acpm.h), for the Galaxy S9's GPU; nothing of its own on the command line | `hakios.exynos_acpm.c` |  |  |  |  | built | built | |
| **`HAKIOS_USB_GADGET`**<br>a USB device link for phones with a DesignWare DWC3: the kernel's log streamed to a host over a CDC-ACM serial port (/dev/ttyACM0) and a line console back, in steps (usb_gadget=N, 5 for all); the SoC's half is a glue driver (EXYNOS_USB). Device-neutral; descriptors and requests in hakios.usb_acm.h | `hakios.dwc3.c` |  | `usb_gadget=`, `usb_gadget_clk=`, `usb_gadget_crlf=`, `usb_gadget_drain=`, `usb_gadget_fs=`, `usb_gadget_ldo=`, `usb_gadget_poll=`, `usb_gadget_soc=` and 1 more |  |  |  | built | |
| **`HAKIOS_EXYNOS_USB`**<br>the Galaxy S9's USB port, the SoC's half for USB_GADGET: the Exynos 9810's PMU isolation, clocks and USB 2.0 PHY (the BSP's USB 3.1 CAL sequence), the SuperSpeed side held idle; usb_gadget_clk=1, usb_gadget_ldo=1 | `hakios.exynos_usb.c` | `HAKIOS_USB_GADGET`, `HAKIOS_EXYNOS_ACPM` |  |  |  |  | built | |

## gpu

| Driver | Files | Requires | Boot arguments | frankel | qemu | redwood | starlte | |
|---|---|---|---|---|---|---|---|
| **`HAKIOS_ADRENO`**<br>the Adreno 642L of the SC7280 woken step by step (adreno=N), and HakiosGPU, the IOKit service Mesa and the window server draw through (gpu=1, adreno_keep=1). Needs the GPU's firmware from the phone (fw-gpu/) | `hakios.adreno.c`<br>`hakios.gpu.cpp` |  | `adreno=`, `adreno_blind=`, `adreno_dirty=`, `adreno_fine=`, `adreno_keep=`, `adreno_only=`, `adreno_pause=`, `adreno_return=` and 12 more |  |  | built |  | |
| **`HAKIOS_MALI`**<br>the Galaxy S9's Mali-G72 MP18 (Exynos 9810) brought up step by step (mali=N, 9 for all), and HakiosMali, the IOKit service Mesa's panfrost draws through. HakiOS's Mali-G52 driver of the Infinix Hot 20, ported | `hakios.mali.c`<br>`hakios.mali.cpp` | `HAKIOS_EXYNOS_ACPM` | `mali=`, `mali_acpm=`, `mali_fastmap=`, `mali_flush=`, `mali_mhz=`, `mali_pll=`, `mali_trace=` |  |  | built | built | |
| **`HAKIOS_S6E3HA8`**<br>the Galaxy S9's panel brightness through DSIM0's command FIFO: the gamma worked out from the panel's MTP as Samsung's kernel does (hakios.s6e3ha8_gamma.h) and the AOR, Samsung's tables for each level (panel=1; kern.hakios.brightness 0-255, panel_brightness=N at start; panel_gamma=0 the AOR alone) | `hakios.s6e3ha8.c` |  | `panel=`, `panel_brightness=`, `panel_gamma=`, `panel_gamma_log=` |  |  | built | built | |

## haptic

| Driver | Files | Requires | Boot arguments | frankel | qemu | redwood | starlte | |
|---|---|---|---|---|---|---|---|
| **`HAKIOS_HAPTIC`**<br>the vibration motor: an Awinic AW86927 driving a linear resonant actuator (haptic=N), its waveforms from fw-haptic/ | `hakios.haptic.c` |  | `haptic=`, `haptic_effect=` |  |  | built |  | |

## input

| Driver | Files | Requires | Boot arguments | frankel | qemu | redwood | starlte | |
|---|---|---|---|---|---|---|---|
| **`HAKIOS_VIRTIO_INPUT`**<br>QEMU's virtio tablet and keyboard as the touch screen and the buttons (virtio_input=1). QEMU only | `hakios.virtio_input.c` |  | `virtio_input=` |  | built | built |  | |
| **`HAKIOS_GOODIX`**<br>the touch screen: a Goodix GT9916 ("Berlin-D") or a FocalTech FT3680 on SPI (touchscreen=1). The bus, pins and supplies are the POCO X5 Pro's; another phone with one of these controllers changes those | `hakios.goodix.c` |  | `touch_flip=`, `touch_poll_us=`, `touch_rate=`, `touch_spi_hz=`, `touchscreen=` |  |  | built |  | |
| **`HAKIOS_BUTTONS`**<br>the power and volume keys, read from the PMIC through the SPMI arbiter (buttons=1) | `hakios.buttons.c` |  | `buttons=`, `buttons_steps=`, `press_power=`, `press_power_ms=`, `touchscreen=` |  |  | built |  | |
| **`HAKIOS_EXYNOS_KEYS`**<br>the Galaxy S9's power, volume and Bixby keys, pins of the Exynos 9810's ALIVE GPIO block (exynos_keys=1). Bixby is the home button | `hakios.exynos_keys.c` |  | `exynos_keys=` |  |  | built | built | |
| **`HAKIOS_S6SY761`**<br>the Galaxy S9's touch screen: a Samsung S6SY761 on the Exynos 9810's HSI2C of USI03, the bus driven by this file (s6sy761=1; s6sy761_steps=N, s6sy761_ipclk=MHz) | `hakios.s6sy761.c` | `HAKIOS_EXYNOS_HSI2C`, `HAKIOS_EXYNOS_ACPM` | `s6sy761=`, `s6sy761_ipclk=`, `s6sy761_ldo=`, `s6sy761_steps=` |  |  | built | built | |

## modem

| Driver | Files | Requires | Boot arguments | frankel | qemu | redwood | starlte | |
|---|---|---|---|---|---|---|---|
| **`HAKIOS_QMI`**<br>QMI messages over the IPC router: how the Wi-Fi firmware's service and the modem's are spoken to | `hakios.qmi.c` |  |  |  |  | built |  | |
| **`HAKIOS_MODEM`**<br>the modem (modem=1): started, its file systems served from the flash, the IPA's data path as en1, the SIM and the network registration. Research; needs fw-modem/ from the phone | `hakios.mpss.c`<br>`hakios.cell.c`<br>`hakios.nas.c`<br>`hakios.ipa.c` | `HAKIOS_QMI`, `HAKIOS_UFS` | `modem=`, `modem_again=`, `modem_apn=`, `modem_buffer=`, `modem_data_nobind=`, `modem_data_off=`, `modem_data_wait=`, `modem_dpm_off=` and 45 more |  |  | built |  | |

## net

| Driver | Files | Requires | Boot arguments | frankel | qemu | redwood | starlte | |
|---|---|---|---|---|---|---|---|
| **`HAKIOS_VIRTIO_NET`**<br>QEMU's virtio network device as the link behind en0 (virtio_net=1). QEMU only | `hakios.virtio_net.c` | `HAKIOS_WIFI_STA` | `virtio_net=`, `virtio_net_ssid=`, `virtio_net_wifi=` |  | built | built |  | |

## power

| Driver | Files | Requires | Boot arguments | frankel | qemu | redwood | starlte | |
|---|---|---|---|---|---|---|---|
| **`HAKIOS_BATTERY`**<br>the battery: kern.hakios.battery_*, and HakiosBattery, the IOPMPowerSource Darwin reads the charge from (the state comes from the audio DSP's battery manager, with adsp=1) | `hakios.battery.c`<br>`hakios.battery.cpp` |  | `battery_shared_pa=` |  |  | built | built | |
| **`HAKIOS_CPUFREQ`**<br>the clock of the cores the kernel runs on (cpufreq=1), and the memory bus's votes (ddr=) | `hakios.cpufreq.c` |  | `cpufreq=`, `cpuload=`, `ddr=`, `ddr_rest=`, `ddr_vote=`, `membench=` |  |  | built |  | |
| **`HAKIOS_THERMAL`**<br>the SoC's temperature sensors (thermal=1): kern.hakios.thermal_*, and the run handed back to the loader when the phone gets too hot (thermal_stop=) | `hakios.thermal.c` |  | `thermal=`, `thermal_log=`, `thermal_poll=`, `thermal_stop=`, `thermal_tm0=`, `thermal_tm1=` |  |  | built |  | |
| **`HAKIOS_PMIC`**<br>the blocks of the power-management chips the SPMI arbiter shows, and the PMIC's clock (pmic=1); read only | `hakios.pmic.c` |  | `pmic=`, `pmic_list=` |  |  | built |  | |
| **`HAKIOS_BATTGUARD`**<br>the run handed back to the loader before the battery is flat (battery_stop=<percent>) | `hakios.battguard.c` |  | `battery_stop=`, `battery_stop_poll=` |  |  | built |  | |
| **`HAKIOS_MAX77705`**<br>the Galaxy S9's battery: the fuel gauge and charger of its MAX77705, read (never written) over CMGP's HSI2C and handed to HakiosBattery, the power source iOS reads (max77705=1; max77705_steps=N, max77705_ipclk=MHz); and its haptic block, the vibration motor for iOS through /dev/hakios-haptic (max77705_haptic=1) | `hakios.max77705.c` | `HAKIOS_EXYNOS_HSI2C`, `HAKIOS_BATTERY` | `max77705=`, `max77705_haptic=`, `max77705_ipclk=`, `max77705_steps=`, `max77705_usbsw=` |  |  | built | built | |
| **`HAKIOS_EXYNOS_TMU`**<br>the Galaxy S9's temperatures: the Exynos 9810's TMU (BIG, LITTLE, G3D, ISP), read with each sensor's fuses (tmu=1), published as kern.hakios.temp.* and the POCO's kern.hakios.thermal_* (tmu=2), and the run ended or the phone powered off when a zone or the battery is too hot (tmu=3; tmu_stop=, tmu_batt_stop=, tmu_off=1) | `hakios.exynos_tmu.c` |  | `tmu=`, `tmu_batt_stop=`, `tmu_enable=`, `tmu_log=`, `tmu_off=`, `tmu_poll=`, `tmu_stop=`, `tmu_ungate=` | built |  | built | built | |

## storage

| Driver | Files | Requires | Boot arguments | frankel | qemu | redwood | starlte | |
|---|---|---|---|---|---|---|---|
| **`HAKIOS_UFS`**<br>the phone's UFS flash: a block device for each GPT partition (ufs=1, ufs=2), and the UFS host controller driven from the kernel (ufs_native=): beside UEFI's (1, the POCO), or its own for the run (4; QEMU's -device ufs, the S9's with ufs_native_soc=exynos9810 as UEFI left it, never reset, untried), in steps (ufs_native_step=1 look, 2 the link, 3 NOP OUT, 4 READ CAPACITY, 5 the GPTs; the S9's default 1). THE ONLY CODE OF THE KERNEL THAT MAY WRITE TO THE PHONE (ufs_write=1, the partition `darwin` only; ufs_native_write=, one partition, never EFS, BOOT, PARAM ...): see tools/provenance.allow | `hakios.ufs.c`<br>`hakios.ufshc.c` |  | `ufs_bench=`, `ufs_check=`, `ufs_native_bench=`, `ufs_native_buffer=`, `ufs_native_check=`, `ufs_native_hang=`, `ufs_native_leave=`, `ufs_native_slots=` and 7 more |  | built | built | built | |
| **`HAKIOS_VIRTIO_BLK`**<br>a disk image of the host as /dev/rvblk0 (virtio_blk=1). QEMU only | `hakios.virtio_blk.c` |  | `virtio_blk=`, `virtio_blk_rw=` |  | built | built |  | |

## wifi

| Driver | Files | Requires | Boot arguments | frankel | qemu | redwood | starlte | |
|---|---|---|---|---|---|---|---|
| **`HAKIOS_WIFI`**<br>Wi-Fi of the WCN6750 (wifi=1, wifi_step=N): the DSP that runs the radio's firmware, Linux's ath11k (its object, made by tools/mk-ath11k.sh, is linked in only with this), a station, and the Apple80211 surface Darwin's wifid talks to. Needs fw-wifi/ from the phone | `hakios.wifi.c`<br>`hakios.wlfw.c` | `HAKIOS_QMI`, `HAKIOS_WIFI_STA` | `wifi=`, `wifi_again=`, `wifi_ask=`, `wifi_cal=`, `wifi_dirty=`, `wifi_file=`, `wifi_fine=`, `wifi_keep=` and 14 more |  |  | built |  | |
| **`HAKIOS_WIFI_STA`**<br>the station and the Apple80211 surface Darwin's wifid talks to, over whichever link is below: the WCN6750's (WIFI) or QEMU's virtio network device seen as Wi-Fi (VIRTIO_NET, virtio_net_wifi=1) | `hakios.sta.c`<br>`hakios.apple80211.c`<br>`hakios.wlan.cpp` |  |  |  | built | built | built | |
| **`HAKIOS_EXYNOS_PCIE`**<br>the Galaxy S9's Wi-Fi, stage 1: the Exynos 9810's PCIe root complex of channel 0 (DesignWare and Samsung's PHY), WLAN_REG_ON and PERST (active low), the link to the BCM4361 and its ID and BARs (wifi_pcie=N, 5 ends stage 1, 6 hands the device to BRCMFMAC's driver); no firmware here (devices/starlte/WIFI.md) | `hakios.exynos_pcie.c` |  | `wifi_pcie=`, `wifi_pcie_tries=`, `wifi_pcie_ungate=` | built |  | built | built | |
| **`HAKIOS_BRCMFMAC`**<br>the Galaxy S9's Wi-Fi, stage 2: Linux's brcmfmac on the BCM4361 (its object, made by tools/mk-brcmfmac.sh, is linked in with this unless WIFI's is), on the device stage 1 hands over (wifi_pcie=6, brcmfmac=N, 8 steps: 4 the firmware's interface, 5 INTx, 6 the phone's address, 7 the station, 8 joining), with WIFI_STA above it (without it there is no station to give Darwin), and the radio unpowered on the way back to the loader. Needs the firmware, NVRAM and CLM of the user's own phone in fw-wifi/ (WIFI.md of starlte) | `hakios.brcmfmac.c` | `HAKIOS_EXYNOS_PCIE` | `brcmfmac=`, `wifi_mac=` | built |  | built | built | |

The columns on the right are the devices' defconfigs: "built" means that defconfig builds the driver in.
