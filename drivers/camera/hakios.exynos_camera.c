/*
 * The Galaxy S9's cameras (starlte, Exynos 9810): the front camera, a Samsung S5K3H1 (Android's camera 1,
 * "fimc-is_sensor_3h1"), from its supplies to one raw frame in memory, without the ISP's libraries. Only with boot-arg
 * cam_s9=N on this phone; nothing is written at cam_s9=0 (the default). Every address and number below belongs to this
 * phone, from its device tree and the S9 kernel's camera driver (fimc-is2), read as data.
 *
 * STAGE 1, THE SENSOR (cam_s9=1, 2)
 *   the sensor     0x35 (7 bits; 0x6A as Samsung's node name writes it) on hsi2c@108F0000, the second I2C of USI08
 *                  of PERIC1 ("USI08_I2C"), 400 kHz; its pins gpp5-2 (SCL) and gpp5-3 (SDA), function 3 ("on_i2c"),
 *                  function 0, inputs, when it is off ("off_i2c"). 16-bit register addresses, MIPI CCS's map: the
 *                  model ID at 0x0000 (16 bits), the revision at 0x0002, the frame count at 0x0005 (0xff: not
 *                  streaming), mode_select at 0x0100
 *   reset          gpg2-5 of PERIC0's GPIO (0x104300C0), an output, high = running
 *   MCLK           CIS_CLK1 of CMU_TOP (0x1A240000): MUX_CLKCMU_CIS_CLK1 0x101C [0] (0: OSCCLK, 26 MHz), the divider
 *                  0x1818 [4:0], the gate 0x201C (bit 20 manual, 21 on), its Q-channel 0x3008; Android asks 26 MHz
 *                  (exynos9810_fimc_is_sensor_mclk_on), which is OSCCLK undivided. Out on gpc1-5 of PERIC1's GPIO
 *                  (0x10830080), function 2 ("fimc_is_mclk1_fn"); output low (function 1) when off
 *   the supplies   all five on the S2MPB02 (0x59 on hsi2c@10920000, USI10 of PERIC1: the flashlight's chip, see
 *                  hakios.s2mpb02_torch.c). An LDO's control register: bit 7 on, [6:0] the voltage, 600 mV + n steps
 *                  (12.5 mV for LDOs 1-5, 25 mV for 6-18):
 *                    VDDD_1.05V_VT   LDO5,  L5CTRL  0x22, 1.05 V = 36 (0x24)
 *                    VDDIO_1.8V_VT   LDO6,  L6CTRL  0x23, 1.8 V  = 48 (0x30)
 *                    VDDA_2.8V_VT    LDO13, L13CTRL 0x2A, 2.8 V  = 88 (0x58)
 *                    VDDAF_2.8V_VT   LDO16, L16CTRL 0x2D, 2.8 V  = 88 (0x58)
 *                  and BUCK2, VDD_EXT_1P3_PB02, 1.3 V: B2CTRL1 0x0D [7:6] the enable (3 on, the regulator's enable
 *                  mask 0xC0), B2CTRL2 0x0E the voltage, 400 mV + 6.25 mV n: 1.3 V = 144 (0x90); enable time 100 us.
 *                  The star boards define USE_BUCK2_REGULATOR_CONTROL (fimc-is-vendor-config_star.h), and the 3H1's
 *                  module file then switches BUCK2 on first and off last: the 1.3 V the low-voltage LDOs (VDDD's
 *                  LDO5 among them) are made from. Without it VDDD is "on" with nothing behind it, and the sensor does
 *                  not answer (the first phone run, 2 November: 0x35 NACKed, INT_STAT's no-device bit, 0x400)
 *   power-up       (the module file's, fimc-is-device-module-3h1.c, star's): CIS_CLK1 on at 26 MHz; reset low; BUCK2
 *                  at 1.3 V and on; VDDAF on; VDDA on; VDDIO on; VDDD at 1.05 V and on, 1 ms; the bus pins to I2C, 10
 *                  us; reset high; MCLK out on its pin, 9 ms. Each supply is given 200 us (the regulator's enable
 *                  times are 100 us for a buck, 180 us for an LDO). If the model ID is not read: 20 ms more and again,
 *                  then the bus's and the pins' registers logged and the bus scanned (below)
 *   power-down     MCLK's pin to an output, low; reset low; the bus pins back to inputs; VDDAF, VDDD, VDDA, VDDIO off,
 *                  BUCK2 off (the module file's order); CIS_CLK1's gate, Q-channel, mux and divider as found. BUCK2's
 *                  voltage register stays at 1.3 V, the value Linux's regulator core gives it at boot (min = max)
 *   the scan       only when the sensor does not answer at 0x35: a one-byte READ at every 7-bit address 0x08-0x77
 *                  (nothing is written to anyone), the addresses that acknowledge listed; then, of those, only the
 *                  other addresses a 3H1 can be strapped to (0x10, 0x1A, 0x20, 0x2D, 0x36, 0x5A: never 0x50-0x57, an
 *                  EEPROM's) are asked for the model ID (a 16-bit register address written, two bytes read), and
 *                  one that gives an ID other than 0 and 0xffff is used for the run. cam_s9_addr=A forces one
 *
 * STAGE 2, CSIS1 AND ITS DMA (cam_s9=3, 4)
 *   the domain     ISPPRE (pd-isppre@14064048): the PMU's ISPPRE_CONFIGURATION 0x14064048 [3:0] = 0xF, its STATUS
 *                  0x1406404C [3:0] waited for 0xF (pmucal isppre_on); off: [3:0] = 0, STATUS 0. Its parent in the
 *                  device tree is pd-dpu (DPU_STATUS 0x14064024), which the display keeps on: checked, never touched.
 *                  need_smc = 0x16210004: after the power comes, SMC 0x82000410 (1, 0x16210004, 0), the DTZPC
 *                  restore (exynos_pd_tz_restore; without it the bus refuses the first access, an SError, as the G3D
 *                  did in hakios.mali.c); before it goes, the low-power setting of pmucal_lpm_init for it,
 *                  MEMORY_ISPPRE_OPTION 0x14064730 [1:0] = 2, then SMC 0x82000410 (0, 0x16210004, 0), the DTZPC save.
 *                  A domain found on is used as it is: no SMC, and it is left on.
 *   its clocks     CMU_ISPPRE 0x16200000: PLL_CON0_MUX_CLKCMU_ISPPRE_BUS_USER +0x100 (bit 4: CMU_TOP's
 *                  CLKCMU_ISPPRE_BUS rather than OSCCLK, bit 7 busy), DIV_CLK_ISPPRE_BUSP +0x1804; the Q-channels
 *                  (GATE_IS_ISPPRE_CSISn in the clock driver are Q-channels: bit 0 hardware gating, bit 1 the clock
 *                  requested) of the four CSISs +0x3020-0x302C (Linux's sensor path asks all four on), the PDP's
 *                  DMA +0x3038 and the SysMMU +0x305C: bit 1 set (put back as found when the domain was found on).
 *                  CMU_TOP's CLKCMU_ISPPRE_BUS: mux 0x1094 [1:0], divider 0x1890 [3:0], gate 0x209C (bit 20 manual,
 *                  21 on): the gate held on for the run and put back as found
 *   CSIS1          the link, 0x16240000 (fimc-is-hw-csi-v5_1.h): CSIS_VERSION 0x0, CSIS_CMN_CTRL 0x4 (bit 0 CSI_EN, 1
 *                  SW_RESET, [9:8] the lanes less one, [11:10] INTERLEAVE_MODE, [19:16] UPDATE_SHADOW, 20 QCHANNEL_EN,
 *                  21 PHY_SEL: 0 D-PHY), INT_MSK0/SRC0 0x10/0x14 (FRAMESTART [27:24], FRAMEEND [23:20], ERR_SOT_HS
 *                  [19:16], LOST_FS [15:12], LOST_FE [11:8], OVER 4, WRONG_CFG 3, ECC 2, CRC 1, ID 0), INT_MSK1/SRC1
 *                  0x18/0x1C (VRESOL [15:12], HRESOL [11:8] mismatches), ISP_CONFIG_CHn 0x40 + 0x10 n (VC [1:0],
 *                  DATAFORMAT [7:2], PIXEL_MODE [13:12]: 1 dual), ISP_RESOL_CHn 0x44 + 0x10 n (height [31:16],
 *                  width [15:0]), FRM_CNT_CH0 0x100, PHY_STATUS 0x700, PHY_CMN_CTRL 0x704 (bit 0 the clock lane,
 *                  [4:1] the data lanes). The reset: SW_RESET set and waited for to clear, 10 x 10 us.
 *   its WDMA       VC0's at 0x162C0000: DMA0_CTRL 0x0 (bit 0 the output on, 1 UPDT_PTR_EN, [4:2] UPDT_FRAMEPTR),
 *                  DMA0_FMT 0x4 ([1:0] 0 RAW10, 3 PACK12, 4 the storage: 0 legacy, 5 the dimension: 0 2D),
 *                  DMA0_ADDR1-8 0x10-0x2C (32-bit addresses: all eight given the one buffer, as csi_hw_s_dma_addr
 *                  does), DMA0_ACT_CTRL 0x30, DMA0_RESOL 0x40 [13:0] the width, DMA0_FCNTSEQ_STAT 0x54; its DMA
 *                  common at +0x400: DMA_DATA_CTRL 0x0 (bit 0 the input: 0 OTF), DMA_INT_MASK 0x4 (0x00F11F00, the
 *                  BSP's), DMA_INT_SRC 0x8 (write 1 to clear: FRM_START bit 4, FRM_END 8, DMA_ERROR 12, ABORT_DONE
 *                  16, OTF_OVERLAP 20, for VC0), DMA_ERR_CODE 0x18, OTF_FORMAT 0x1C (0 RAW10). The other CSISs' DMA
 *                  commons (0x162B0400, 0x162D0400, 0x162E0400) are read, never written, for the log
 *   the DMA common 0x162F0000, of all four CSISs (fimc_is@16290000's first region): COMMON_DMA_ARB_PRI 0x0 ([3:2] 1,
 *                  [1:0] 2), COMMON_DMA_CTRL 0x8 (bit 1 IP_PROCESSING), COMMON_DMA_SRAM_SPLIT 0xC (0x140 each),
 *                  COMMON_DMA_MATRIX 0x10 (0), as csi_hw_s_dma_common sets them; its test pattern generator:
 *                  TEST_PATTERN_ENABLE 0x20 (bit 31 on, 30 two pixels a clock, [29:0] the vertical blank in clocks),
 *                  TEST_PATTERN_CTRL 0x24 (V-to-H blank [23:16] 16, H blank [15:8] 8, H-to-V blank [7:0] 2),
 *                  TEST_PATTERN_RESOL 0x28 (height [29:16], width [13:0]). With the pattern on, Android does not
 *                  enable the link (csi_stream_on): the pattern stands in for it
 *   the D-PHY      M0S4S2's slave 0 (CSIS1): its isolation in the PMU at 0x14060730 (bit 0: 1 bypassed, the PHY in
 *                  use; shared with CSIS3), its reset bit 2 of SYSREG_ISPPRE's 0x16210500 (cleared, then set: running)
 *                  (phy-exynos8895-mipi.c, the device tree's dphy_m0s4s2_csis1). Its registers at 0x16180000 (the
 *                  CSIS1 node's second region): DPHY_ACTRL_SC_06 +0x1018 = 2, DPHY_DCTRL_SC_02 +0x1084 [7:0] = 1
 *                  (CLKSETTLECTL), and for each of the two lanes (lane n at +0x400 n): DPHY_ACTRL_SDn_08 +0x143C
 *                  [6:5] = 0 (the skew delay), DPHY_DCTRL_SDn_05 +0x14AC bit 2 = 1 (HS_MODE_SEL, below 1.5 Gb/s),
 *                  DPHY_DCTRL_SDn_06 +0x14B0 [7:0] = the settle, DPHY_DCTRL_SDn_13 +0x14E0 bit 0 = 0 (no skew
 *                  calibration below 1.5 Gb/s) (csi_hw_s_phy_default_value, csi_hw_s_phy_config)
 *   the SysMMU     sysmmu@16310000 ("3AAM, 3AA, PDP": the CSIS nodes' iommus): MMU_CTRL 0x0 (bit 0 on), MMU_CFG 0x4,
 *                  MMU_STATUS 0x8, MMU_VERSION 0x34, read only and only with cam_s9_mmu_probe=1. Off (its reset
 *                  state after the domain's power-on), it passes the DMA's addresses through: the buffer's physical
 *                  address is the DMA's. Found on, there is no frame: this driver gives it no table. Unprobed (the
 *                  default), it is taken as off and never touched
 *   the buffer     4 MB below 4 GB (the WDMA's addresses are 32 bits). XNU has no memory there on the S9 (its RAM is
 *                  the bank from 0x880000000: kmem's "below 4 GB" never succeeds), so it is physical memory XNU does
 *                  not manage, in the device tree's gap: the camera's 32 MB from 0xD8000000 in sys/hakios.exynos9810_mem.h
 *                  (the raw frames, then camshot's picture), above the display's buffers and the moved payload,
 *                  below UFS's own-mode memory; never 0xF0000000 and above (the modem's, ABOX's).
 *                  Mapped once through IOKit, write-combined (hakios_physmap_wc()), never with ml_io_map()
 *                  (io_submap is 8 MB); filled with 0xA5 first, so that what the DMA wrote shows
 *
 * STAGE 3, A FRAME (cam_s9=5)
 *   the mode       1632 x 1224 RAW10 at 30 frames a second, two lanes at 719 Mb/s, VC0 (module-3h1's mode 5, with the
 *                  embedded data on CH2 as its CSI_MODE_VC_DT config gives it): setfile A's (the device tree's
 *                  "default") table sensor_3h1_setfile_A_1632x1224_30fps, 86 16-bit writes taken as given, after 3 ms
 *                  (sensor_cis_set_registers); then 0x3092 = 0 (no companion chip: sensor_3h1_cis_mode_change). The
 *                  table leaves 0x0340 (frame length) 0x04F2 lines, 0x0342 (line length) 0x2970 pixel clocks of
 *                  403.6 MHz: 26.3 us a line, 33.3 ms a frame. The exposure: coarse integration 0x0202 (cam_s9_exp,
 *                  1000 lines unless said; at most the frame's less 4) and analog gain 0x0204 (cam_s9_gain, 0x40:
 *                  32 is 1x, sensor_cis_calc_again_code)
 *   the settle     36 (fimc-is-hw-settle-10nm-lpp.h at 720 Mb/s, the table fimc-is-v6_0_0's chain takes)
 *   stream on      0x0104 = 0 (group hold off), 8 ms, 0x6028 = 0x4000, 0x0100 = 1, 0x0216 = 0 (no WDR)
 *                  (sensor_3h1_cis_stream_on); the frame count 0x0005 leaves 0xff
 *   the link       csi_hw_enable: UPDATE_SHADOW 0xF, PHY_SEL 0, the clock lane on, QCHANNEL_EN 0, CSI_EN 1
 *   stream off     0x0104 = 0, 0x6028 = 0x4000, 0x0100 = 0; the frame count back to 0xff. The link: lanes off,
 *                  CSI_EN 0, QCHANNEL_EN 1 (csi_hw_disable), the reset; the WDMA's output off
 *
 * THE STEPS. cam_s9=N makes the first N and stops; each is said before it is taken:
 *   1  reads   the pins, CMU_TOP's CIS_CLK1, the two buses as found, the S2MPB02's ID and the four LDOs' registers
 *   2  writes  the sensor powered up, its model ID, revision, frame count and mode read; powered down again (at 5:
 *              kept up for step 5)
 *   3  writes  the ISPPRE domain on (and its DTZPC restore), its clocks; reads CSIS1's version, its WDMA, the DMA
 *              common (the SysMMU's state with cam_s9_mmu_probe=1)
 *   4  writes  the D-PHY's isolation off and its reset released, CSIS1 reset and set up for 1632 x 1224 RAW10, the
 *              buffer given to VC0's WDMA (a stale DMA abort request in its DMA_CMN_CTRL cleared first), the test
 *              pattern generator on: first as Android has it, CSIS1's link left off (csi_stream_on with pattern_en),
 *              500 ms; if no frame has started on CSIS1's WDMA by then, the pattern off and on again with the link
 *              enabled as csi_hw_enable does (no clock lane: no sensor), 1.5 s; then the WDMA and the pattern off. The
 *              other three CSISs' DMA interrupts are unmasked for the while (put back after), so the log says which
 *              WDMA the pattern reached if not CSIS1's. The buffer's checksum, its first bytes, how much of it changed
 *   5  writes  the sensor's mode table, the D-PHY's settle for 719 Mb/s, the link on, the ring (four 4 MB slots)
 *              given to VC0's WDMA's eight frame pointers, streaming; the auto-exposure: the green of the frame's middle
 *              metered on every third frame and the exposure stepped towards 18 % grey (coarse integration time, then
 *              analog gain, then digital gain, within the mode's 1266-line frame) until it is there or
 *              cam_s9_ae_frames frames have passed (15 unless said); that frame is the one taken: the WDMA off, streaming off; the
 *              counters, the link's errors, the checksum and the mean of a 64 x 64 patch in the middle (a black frame
 *              reads about 64 in 10 bits)
 *   6  camshot: steps 1 to 5 as above at the start, then the thread stays for the USB console's "camshot [N] [tp]"
 *              (hakios.dwc3.c, usb_gadget=5): each one makes steps 2 to 5 again (with "tp", 3 and 4: the test pattern),
 *              everything off again, then the frame demosaiced on the processor (bilinear, the whole 1632 x 1224 at
 *              N = 1, every Nth pixel at N; the black level off, a grey-world balance kept to sane gains, a saturation,
 *              a gamma of about 1/2.2) and handed to the USB link, which sends it as it sends a shot of the panel. A
 *              capture that fails says "usb: camshot failed: ..." with CSIS1's and the WDMA's status.
 *              tools/usb-shot.py --cam
 *   7  the stream: after step 1's look, the sensor streams for the rest of the run into the ring, with the
 *              auto-exposure running all along; /dev/hakios-camera gives the newest frame to user space (the POCO's
 *              protocol, bsd/sys/hakios.camera.h: libhakios.media's capture device and camera_test read it); camshot
 *              takes the newest frame without powering anything up or down; the log says the frame rate every ten
 *              seconds
 * Whatever a run switched on is switched off again in the reverse order, also when a step fails, and at the run's end
 * (loader_return(), a panic's way out too): the WDMA and the pattern first, then the sensor, the link, the PHY, the
 * domain (if this run switched it on). A WDMA not seen to stop: no more DMA into the buffer for the run.
 *
 * Boot-args:
 *   cam_s9=N           the front camera's first N steps: 1 a look, 2 the sensor's ID, 3 the ISPPRE domain and CSIS1,
 *                      4 one frame of the test pattern into memory, 5 one frame of the sensor's, 6 that and then the
 *                      USB console's camshot, 7 the stream and /dev/hakios-camera (0 or none: nothing)
 *   cam_s9_addr=A      the sensor's 7-bit I2C address, if not 0x35 (the device tree's) and not found by the scan
 *   cam_s9_ipclk=MHZ   the two HSI2Cs' input clock in MHz (default 400, as the phone showed for the other buses on 1
 *                      November: a wrong guess on the high side only makes the bus slower)
 *   cam_s9_exp=LINES   the first exposure of step 5's frame in lines of 26.3 us (default 1000, at most 1262)
 *   cam_s9_gain=CODE   the first analog gain of step 5's frame, 0x20 being 1x (default 0x40, at most 0x200)
 *   cam_s9_ae=0        no auto-exposure: the frames as cam_s9_exp and cam_s9_gain have them (default 1: on)
 *   cam_s9_ae_frames=N the auto-exposure's frames at most before a camshot's frame is taken (2 to 30, default 15)
 *   cam_s9_dgain=CODE  the most digital gain the auto-exposure may use, 0x100 being 1x (default 0x400, at most 0x1000)
 *   cam_s9_sat=PCT     camshot's colour saturation in per cent (default 140; 100: none; at most 300)
 *   cam_s9_pack12=1    the WDMA's PACK12 layout rather than RAW10 as it comes (the BSP sets PACK12 for its V4L2
 *                      SBGGR10 buffers). Without it the WDMA writes each RAW10 pixel in 16 bits, little-endian, the
 *                      value in [9:0] (the phone, 3 November): camshot's debayer reads only that)
 *   cam_s9_bayer=N     camshot's Bayer order: 0 RGGB, 1 GRBG (the default, the sensor's), 2 GBRG, 3 BGGR
 *   cam_s9_stride=B    camshot's bytes a line of the frame (default 3264: the WDMA's RAW10 is a pixel in 16 bits)
 *   cam_s9_ispdiv=D    CLKCMU_ISPPRE_BUS (CSIS1's and its WDMA's clock) = SHARED1_DIV4, 466 MHz, / (D + 1); default
 *                      1, 233 MHz (Android's own is 466 MHz, D = 0). The boot leaves it at about 50 MHz, too slow:
 *                      CSIS1 overflows and the WDMA's FIFO fills a few lines into the frame
 *   cam_s9_probe=1     the risky reads too, for the log: CMU_ISPPRE's whole Q-channel sweep with each DBG_NFO (+0x4000),
 *                      and the other three CSISs' DMA commons (read, and their interrupt masks for the pattern's while).
 *                      Off by default: on 3 November the sweep's read of CMU_ISPPRE 0x303C took a synchronous external
 *                      abort (isp_status(), the phone's cam-fix.img)
 *   cam_s9_mmu_probe=1 the SysMMU (0x16310000) mapped and read: MMU_CTRL/CFG/STATUS/VERSION in the log, and a SysMMU
 *                      found translating stops the DMA. Off by default (its clock or Q-channel may be off, and a read
 *                      then aborts, as the PCIe SysMMU's did): the SysMMU is taken as off, pass-through, and the DMA
 *                      starts without asking it
 *
 * Limits, whatever asks. The S2MPB02 makes other supplies too (the flash LED, the rear camera's lens motor, the grip
 * sensor, the proximity LED): every write to it goes through one function that takes only the four LDO registers and
 * BUCK2's two above, only the values above (Android's voltages, on or off; BUCK2's control register only in its enable
 * bits), and reads each back; a write that does not read back, or a byte
 * nobody acknowledges, and everything is powered down in the power-down's order and nothing more is done for the run.
 * A supply found on is left as it is and left on at the end; one found on at another voltage stops the run before
 * anything is written (someone else has it). No other PMIC register is written. The flashlight's driver stays off in a
 * run with cam_s9=N (the same bus, one driver per bus). Nothing of the ISPPRE block is read before its domain is seen
 * on. The DMA is given only the buffer's address, checked to lie below 4 GB, and with the SysMMU taken as off (seen off, with cam_s9_mmu_probe=1). Every
 * wait is bounded by the clock.
 *
 * Not written yet (docs/devices/starlte-camera.md): the CSIS's frame-end interrupt (the stream polls every millisecond),
 * the camera powered only while /dev/hakios-camera is open, a colour matrix and lens shading of the module's own.
 *
 * Sources: the phone's device tree (exynos9810-starlte_eur_open_26.dts of the BSP: hsi2c@108F0000 and fimc-is-3h1@35,
 * fimc-is_sensor_3h1@6A, s2mpb02_pmic@59 and its regulators (s2mpb02-buck2: VDD_EXT_1P3_PB02), the fimc_is_mclk1 pin states, hsi2c21-bus, usi@1081102C,
 * pd-isppre@14064048 and pd-dpu, fimc_is_sensor@16240000, fimc_is@16290000, dphy_m0s4s2_csis1, sysmmu@16310000);
 * the S9 kernel's fimc-is-device-module-3h1.c (and vendor/mcd/fimc-is-vendor-config_star.h: USE_BUCK2_REGULATOR_CONTROL), fimc-is-cis-3h1.c/.h (and its bayer_order, GR_BG, for camshot), fimc-is-cis-3h1-setA.h, fimc-is-cis.c,
 * ischain/fimc-is-v6_0_0/setup-fimc-is-sensor.c, fimc-is-config.h and fimc-is-hw-chain.c/.h,
 * include/fimc-is-hw-settle-10nm-lpp.h, include/fimc-is-hw.h, fimc-is-device-csi_v4.c, sensor/csi2/fimc-is-hw-csi-v5_1.c
 * and .h, drivers/phy/phy-exynos8895-mipi.c, drivers/iommu/exynos-iommu.h, drivers/soc/samsung/exynos-pd.c,
 * cal-if/pmucal_local.c, cal-if/ra.c, cal-if/exynos9810/flexpmu_cal_local_exynos9810.h and
 * flexpmu_cal_system_exynos9810.h, cmucal-sfr.c, cmucal-node.c and cmucal-qch.c, drivers/clk/samsung/clk-exynos9810.c,
 * s2mpb02-regulator.c, s2mpb02-private.h (BUCK2's registers, steps and enable mask),
 * flexpmu_cal_local_exynos9810.h's isppre_save (the CMU_ISPPRE and SYSREG_ISPPRE registers logged at step 3), pinctrl-exynos.c (android_kernel_samsung_exynos9810, lineage-24.0, e5906667;
 * GPL: read for these facts, not copied). The SMC's convention and the lesson of the half-off domain: hakios.mali.c.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.input.h>
#include <kern/locks.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/hakios.camera.h>
#include <sys/hakios.exynos9810_mem.h>
#include <sys/hakios.physmap.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_line(const char *line);

static bool leaving;

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[512];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	if (!leaving) {
		printf("%s", line);
	}
	return length;
}

/* ---- the S2MPB02 ---- */

#define PMIC            0x59
#define PMIC_ID         0x00
#define LDO_ON          0x80
#define LDO_VSEL        0x7f
#define B2CTRL1         0x0D            /* BUCK2, VDD_EXT_1P3_PB02: [7:6] the enable (3: on) */
#define B2CTRL2         0x0E            /* BUCK2's voltage: 400 mV + 6.25 mV n */
#define BUCK_ON         0xC0
#define BUCK2_VSEL      0x90            /* 1.3 V */

static const struct ldo {
	const char *name;
	uint8_t reg, vsel;
	bool switched;          /* by this driver */
} ldos[] = {
	{ "VDDD_1.05V_VT (LDO5)", 0x22, 0x24, true },
	{ "VDDIO_1.8V_VT (LDO6)", 0x23, 0x30, true },
	{ "VDDA_2.8V_VT (LDO13)", 0x2A, 0x58, true },
	{ "VDDAF_2.8V_VT (LDO16)", 0x2D, 0x58, true },
};
#define VDDD            0
#define VDDIO           1
#define VDDA            2
#define VDDAF           3
#define NLDOS           (sizeof(ldos) / sizeof(ldos[0]))

static struct hakios_hsi2c pmic_bus = {
	.name = "cam-s2mpb02",
	.regs_pa = 0x10920000,                  /* hsi2c@10920000: USI10 of PERIC1 */
	.usi_mode_pa = 0x1081103C,              /* usi@1081103C: its mode word */
	.cmu_pa = 0x10800000, .cmu_size = 0x4000,       /* CMU_PERIC1 */
	.gates = { 0x2048, 0x20AC, 0x2114, 0x2118 }, .ngates = 4,
	.qch = 0x3050, .div = 0x1828, .mux = 0x0120,       /* DIV_CLK_PERIC1_USI10_USI; MUX_CLKCMU_PERIC1_IP_USER */
	.pins_pa = 0x10830040,                  /* gpp6: SCL pin 0, SDA pin 1, function 2 */
	.pins_mask = 0xffu, .pins_want = 0x22u,
	.ipclk_mhz = 400, .bus_hz = 100000,     /* the device tree gives this bus no speed: Linux's default */
};

/* ---- the sensor's bus ---- */

#define SENSOR          0x35            /* the device tree's (fimc-is-3h1@35, reg = <53>): 7 bits */
static uint8_t sensor_addr = SENSOR;    /* the one used: the scan's or cam_s9_addr's, if not 0x35 */
static uint8_t sensor_addr_given = SENSOR;      /* cam_s9_addr's, or 0x35 */

static struct hakios_hsi2c sensor_bus = {
	.name = "cam-3h1",
	.regs_pa = 0x108F0000,                  /* hsi2c@108F0000: USI08_I2C of PERIC1 (hsi2c21) */
	.usi_mode_pa = 0x1081102C,              /* usi@1081102C: USI08's mode word ("i2c" in the device tree) */
	.cmu_pa = 0x10800000, .cmu_size = 0x4000,       /* CMU_PERIC1 */
	/* GATE_CLK_PERIC1_USI_I2C, RSTNSYNC_CLK_PERIC1_USI08_I2C, USI08_I2C IPCLK, USI08_I2C PCLK */
	.gates = { 0x2050, 0x2098, 0x20EC, 0x20F0 }, .ngates = 4,
	.qch = 0x303C, .div = 0x1830, .mux = 0x0120,       /* QCH_CON_USI08_I2C_QCH; DIV_CLK_PERIC1_USI_I2C */
	.pins_pa = 0x10830020,                  /* gpp5: SCL pin 2, SDA pin 3, function 3 */
	.pins_mask = 0xff00u, .pins_want = 0x3300u,
	.ipclk_mhz = 400, .bus_hz = 400000,     /* clock-frequency = <400000> */
};

/* ---- pins and the clock ---- */

#define GPIO_PERIC0_PA  0x10430000UL
#define GPIO_PERIC1_PA  0x10830000UL
#define GPG2            0x0C0           /* PERIC0: the reset, pin 5 */
#define GPC1            0x080           /* PERIC1: MCLK1, pin 5 */
#define GPP5            0x020           /* PERIC1: the sensor's bus, pins 2 and 3 */
#define RESET_PIN       5
#define MCLK_PIN        5

#define CMU_TOP_PA      0x1A240000UL
#define CIS1_MUX        0x101C          /* [0]: 0 OSCCLK, 1 DIV_PLL_SHARED2_DIV2 */
#define CIS1_DIV        0x1818          /* [4:0]: the ratio less one */
#define CIS1_GATE       0x201C          /* bit 20 manual, bit 21 on */
#define CIS1_QCH        0x3008          /* DMYQCH_CON_DFTMUX_TOP_QCH_CIS_CLK1: [0] enable, [1] clock request */
#define GATE_MANUAL     (1u << 20)
#define GATE_ON         (1u << 21)

static vm_offset_t peric0, peric1, cmu_top;
static uint8_t found[NLDOS];            /* each LDO's register as found */
static bool mine[NLDOS];                /* switched on by this run */
static uint8_t b2ctrl1_found, b2ctrl2_found;    /* BUCK2's two registers as found */
static bool buck2_mine;                 /* BUCK2 switched on by this run */
static bool broken;                     /* a write did not read back, or the bus failed: no more writing */
static bool powered;                    /* the sensor's power-up was begun */
static uint32_t gate_found, mux_found, div_found, qch_found;
static bool clock_mine;

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

/* by the clock, never sleeping: also on the run's way out */
static uint64_t
absolutetime_ms(uint64_t t)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(t, &ns);
	return ns / NSEC_PER_MSEC;
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static uint32_t
pmic_get(uint8_t reg, uint8_t *value)
{
	return hakios_hsi2c_read_reg(&pmic_bus, PMIC, reg, value, 1);
}

/* what may be written to the S2MPB02: the four LDOs at Android's voltage, on or off; BUCK2 at 1.3 V, on or off */
static bool
pmic_allowed(uint8_t reg, uint8_t value)
{
	for (uint32_t i = 0; i < NLDOS; i++) {
		if (ldos[i].switched && reg == ldos[i].reg) {
			return value == ldos[i].vsel || value == (LDO_ON | ldos[i].vsel);
		}
	}
	if (reg == B2CTRL2) {
		return value == BUCK2_VSEL;
	}
	if (reg == B2CTRL1) {
		return (value & ~BUCK_ON) == (b2ctrl1_found & ~BUCK_ON) && ((value & BUCK_ON) == 0 || (value & BUCK_ON) == BUCK_ON);
	}
	return false;
}

/* The only way anything reaches the S2MPB02: a register and a value pmic_allowed() takes; read back. */
static bool
pmic_put(uint8_t reg, uint8_t value, const char *name)
{
	uint8_t buf[2], back = 0;
	bool same;

	if (broken || !pmic_allowed(reg, value)) {
		if (!broken) {
			say("exynos_camera: S2MPB02 %#x (%s) = %#x refused: not a value this driver writes\n", reg, name, value);
		}
		return false;
	}
	buf[0] = reg;
	buf[1] = value;
	if (hakios_hsi2c_message(&pmic_bus, PMIC, false, buf, 2, true) != 0 || pmic_get(reg, &back) != 0) {
		same = false;
	} else {
		/* BUCK2's control register: its enable bits read back (the rest is as found and not ours) */
		same = reg == B2CTRL1 ? (back & BUCK_ON) == (value & BUCK_ON) : back == value;
	}
	if (!same) {
		say("exynos_camera: S2MPB02 %#x (%s): wrote %#x, read back %#x (bus %#x): no more writing for this run\n",
		    reg, name, value, back, pmic_bus.last_error);
		broken = true;
		return false;
	}
	return true;
}

static bool
ldo_put(uint32_t i, bool on)
{
	if (i >= NLDOS || !ldos[i].switched) {
		return false;
	}
	return pmic_put(ldos[i].reg, (uint8_t)((on ? LDO_ON : 0) | ldos[i].vsel), ldos[i].name);
}

/* BUCK2 (VDD_EXT_1P3_PB02) at 1.3 V and on, unless it was found on (then it is left as it is) */
static bool
buck2_on(void)
{
	if ((b2ctrl1_found & BUCK_ON) == BUCK_ON) {
		say("exynos_camera: 2: VDD_EXT_1P3_PB02 (BUCK2) was on already (%#x, voltage %#x): left as it is\n",
		    b2ctrl1_found, b2ctrl2_found);
		return true;
	}
	if (b2ctrl2_found != BUCK2_VSEL && !pmic_put(B2CTRL2, BUCK2_VSEL, "BUCK2's voltage")) {
		return false;
	}
	buck2_mine = true;
	if (!pmic_put(B2CTRL1, (uint8_t)((b2ctrl1_found & ~BUCK_ON) | BUCK_ON), "VDD_EXT_1P3_PB02 (BUCK2)")) {
		return false;
	}
	say("exynos_camera: 2: VDD_EXT_1P3_PB02 (BUCK2) on at 1.3 V (B2CTRL1 %#x, B2CTRL2 %#x)\n",
	    (b2ctrl1_found & ~BUCK_ON) | BUCK_ON, BUCK2_VSEL);
	return true;
}

static void
buck2_off(void)
{
	if (buck2_mine) {
		bool was_broken = broken;

		broken = false;         /* switching off is always tried */
		if (pmic_put(B2CTRL1, (uint8_t)(b2ctrl1_found & ~BUCK_ON), "VDD_EXT_1P3_PB02 (BUCK2)")) {
			buck2_mine = false;
		}
		broken = broken || was_broken;
	}
}

/* an LDO to Android's voltage and on, unless it was found on (then it is left as it is) */
static bool
ldo_on(uint32_t i)
{
	if (found[i] & LDO_ON) {
		say("exynos_camera: 2: %s was on already (%#x): left as it is\n", ldos[i].name, found[i]);
		return true;
	}
	if ((found[i] & LDO_VSEL) != ldos[i].vsel && !ldo_put(i, false)) {
		return false;
	}
	mine[i] = true;
	if (!ldo_put(i, true)) {
		return false;
	}
	say("exynos_camera: 2: %s on (%#x)\n", ldos[i].name, LDO_ON | ldos[i].vsel);
	spin_us(200);
	return true;
}

static void
ldo_off(uint32_t i)
{
	if (mine[i]) {
		bool was_broken = broken;

		broken = false;         /* switching off is always tried */
		if (ldo_put(i, false)) {
			mine[i] = false;
		}
		broken = broken || was_broken;
	}
}

/* a pin: an output (CON nibble 1) at the level given */
static void
pin_out(vm_offset_t bank, uint32_t number, bool high)
{
	uint32_t data = rd(bank, 4);

	wr(bank, 4, high ? data | (1u << number) : data & ~(1u << number));
	wr(bank, 0, (rd(bank, 0) & ~(0xfu << (4 * number))) | (1u << (4 * number)));
}

static void
pin_function(vm_offset_t bank, uint32_t number, uint32_t function)
{
	wr(bank, 0, (rd(bank, 0) & ~(0xfu << (4 * number))) | (function << (4 * number)));
}

static void
clock_on(void)
{
	gate_found = rd(cmu_top, CIS1_GATE);
	mux_found = rd(cmu_top, CIS1_MUX);
	div_found = rd(cmu_top, CIS1_DIV);
	qch_found = rd(cmu_top, CIS1_QCH);
	if ((mux_found & 1) != 0 || (div_found & 0x1f) != 0) {
		/* the mux on OSCCLK and undivided: 26 MHz, what Android sets; written only when it is not so already */
		wr(cmu_top, CIS1_MUX, mux_found & ~1u);
		wr(cmu_top, CIS1_DIV, div_found & ~0x1fu);
	}
	wr(cmu_top, CIS1_QCH, qch_found | 3);
	wr(cmu_top, CIS1_GATE, gate_found | GATE_MANUAL | GATE_ON);
	clock_mine = true;
	say("exynos_camera: 2: CIS_CLK1: mux %x -> %x, divider %x -> %x, gate %x -> %x (26 MHz from OSCCLK)\n", mux_found,
	    rd(cmu_top, CIS1_MUX), div_found, rd(cmu_top, CIS1_DIV), gate_found, rd(cmu_top, CIS1_GATE));
}

/* The power-down, in the module file's order; only what this run switched on. */
static void
power_down(void)
{
	if (!powered) {
		return;
	}
	pin_out(peric1 + GPC1, MCLK_PIN, false);
	pin_out(peric0 + GPG2, RESET_PIN, false);
	spin_us(1000);
	pin_function(peric1 + GPP5, 2, 0);
	pin_function(peric1 + GPP5, 3, 0);
	ldo_off(VDDAF);
	ldo_off(VDDD);
	ldo_off(VDDA);
	ldo_off(VDDIO);
	buck2_off();
	if (clock_mine) {
		wr(cmu_top, CIS1_GATE, gate_found);
		wr(cmu_top, CIS1_QCH, qch_found);
		wr(cmu_top, CIS1_DIV, div_found);
		wr(cmu_top, CIS1_MUX, mux_found);
		clock_mine = false;
	}
	powered = false;
	say("exynos_camera: powered down: MCLK and reset low, the bus pins inputs, BUCK2 and the LDOs %s; CIS_CLK1's gate "
	    "%x again\n", (mine[VDDD] || mine[VDDA] || mine[VDDIO] || mine[VDDAF] || buck2_mine) ?
	    "NOT ALL OFF (a write failed)" : "off as found", gate_found);
}

/* a register of the sensor: its 16-bit address written, a repeated start, len bytes read */
static uint32_t
sensor_get(uint16_t reg, uint8_t *buf, uint32_t len)
{
	uint8_t address[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
	uint32_t error = hakios_hsi2c_message(&sensor_bus, sensor_addr, false, address, 2, false);

	return error ? error : hakios_hsi2c_message(&sensor_bus, sensor_addr, true, buf, len, true);
}

static void
look_pins(const char *when)
{
	say("exynos_camera: %s: reset gpg2-5 CON %x DAT %u; MCLK1 gpc1-5 CON %x DAT %u; bus gpp5-2/3 CON %x %x\n", when,
	    (rd(peric0 + GPG2, 0) >> (4 * RESET_PIN)) & 0xf, (rd(peric0 + GPG2, 4) >> RESET_PIN) & 1,
	    (rd(peric1 + GPC1, 0) >> (4 * MCLK_PIN)) & 0xf, (rd(peric1 + GPC1, 4) >> MCLK_PIN) & 1,
	    (rd(peric1 + GPP5, 0) >> 8) & 0xf, (rd(peric1 + GPP5, 0) >> 12) & 0xf);
}

/* ---- stage 2: the ISPPRE domain, CSIS1, its D-PHY and its DMA ---- */

#define PMU_PA                  0x14060000UL
#define PMU_SIZE                0x5000
#define PMU_DPHY_ISO_CSIS1      0x0730          /* bit 0: 1 the isolation bypassed (CSIS1 and CSIS3's M0S4S2) */
#define PMU_DPU_STATUS          0x4024          /* pd-dpu, pd-isppre's parent */
#define PMU_ISPPRE_CONFIG       0x4048
#define PMU_ISPPRE_STATUS       0x404C
#define PMU_ISPPRE_MEMORY       0x4730          /* MEMORY_ISPPRE_OPTION: [1:0] = 2 before the power goes */
#define PD_ON                   0xfu
#define PD_WAIT_US              10000u
#define ISO_BYPASS              1u

#define DTZPC_SMC               0x82000410u     /* Samsung's SiP call, SMC_CMD_PREPARE_PD_ONOFF */
#define DTZPC_SAVE              0u
#define DTZPC_RESTORE           1u
#define DTZPC_ISPPRE            0x16210004u     /* pd-isppre's need_smc */

#define CMU_ISPPRE_PA           0x16200000UL
#define ISPPRE_BUS_USER         0x0100          /* bit 4 MUX_SEL (1: CMU_TOP's CLKCMU_ISPPRE_BUS), bit 7 busy */
#define ISPPRE_BUSP_DIV         0x1804
#define QCH_CSIS1               0x3024
#define QCH_SYSMMU              0x305C
/*
 * the Q-channels asked on: the four CSISs' (as setup-fimc-is-sensor.c does), the PDP's DMA's and the SysMMU's, and the
 * way the WDMA's writes leave the block: LHS_AXI_D_ISPPRE (+0x306C, the AXI master to the bus) and BTM_ISPPRE (+0x3010,
 * its traffic monitor), both in pmucal's isppre_save list
 */
static const uint32_t qchs[] = { 0x3020, QCH_CSIS1, 0x3028, 0x302C, 0x3038, QCH_SYSMMU, 0x306C, 0x3010 };
#define NQCHS                   (sizeof(qchs) / sizeof(qchs[0]))
#define QCH_REQ                 (1u << 1)
#define TOP_ISPPRE_MUX          0x1094          /* CMU_TOP: MUX_CLKCMU_ISPPRE_BUS [1:0] */
#define TOP_ISPPRE_DIV          0x1890          /* DIV_CLKCMU_ISPPRE_BUS [3:0] */
#define TOP_ISPPRE_GATE         0x209C          /* GATE_CLKCMU_ISPPRE_BUS: bit 20 manual, 21 on */
#define TOP_BUSY                (1u << 16)      /* the mux's and the divider's busy bit */
#define ISPPRE_MUX_SHARED1_DIV4 1u              /* the mux's parents: 0 SHARED0_DIV4, 1 SHARED1_DIV4, 2 SHARED2_DIV2, 3 */
#define ISPPRE_CTRL_OPTION      0x0800          /* ISPPRE_CMU_ISPPRE_CONTROLLER_OPTION: [29:28] the CMU's power management */
#define QCH_FIRST               0x3010          /* QCH_CON_*: 0x3010 (BTM) to 0x3070 (SYSREG), bit 0 enable, 1 clock request */
#define QCH_LAST                0x3070
#define QCH_DBG                 0x4000          /* DBG_NFO_QCH_CON_*: each QCH_CON's state at +0x4000 */
#define SYSREG_DRCG_EN          0x0104          /* SYSREG_ISPPRE's BUS_COMPONENT_DRCG_EN */

#define SYSREG_ISPPRE_PA        0x16210000UL
#define DPHY_RESET              0x0500          /* the MIPI PHYs' resets: bit 2 CSIS1's M0S4S2 (1: running) */
#define DPHY_RESET_CSIS1        (1u << 2)

#define CSIS1_PA                0x16240000UL
#define CSIS_VERSION            0x000
#define CSIS_CMN_CTRL           0x004
#define CSIS_CLK_CTRL           0x008           /* CLKGATE_TRAIL [31:16], CLKGATE_EN [7:4] (reset 0xF), read only here */
#define CSIS_INT_MSK0           0x010
#define CSIS_INT_SRC0           0x014
#define CSIS_INT_MSK1           0x018
#define CSIS_INT_SRC1           0x01C
#define CSIS_ISP_CONFIG(n)      (0x040 + 0x10 * (n))
#define CSIS_ISP_RESOL(n)       (0x044 + 0x10 * (n))
#define CSIS_FRM_CNT_CH0        0x100
#define CSIS_PHY_STATUS         0x700
#define CSIS_PHY_CMN_CTRL       0x704
#define CMN_CSI_EN              (1u << 0)
#define CMN_SW_RESET            (1u << 1)
#define CMN_LANES_SHIFT         8               /* [9:8] the lanes less one */
#define CMN_INTERLEAVE_SHIFT    10              /* [11:10]: 3 CSI_MODE_VC_DT */
#define CMN_UPDATE_SHADOW       (0xfu << 16)
#define CMN_QCHANNEL_EN         (1u << 20)
#define CMN_PHY_SEL             (1u << 21)
#define PHY_ENABLE_CLK          (1u << 0)
#define PHY_ENABLE_DAT_SHIFT    1               /* [4:1] */
#define CSIS_IRQ_MASK0          0x011FFF1Fu     /* the BSP's default masks */
#define CSIS_IRQ_MASK1          0x0000FFF0u

#define CSIS1_PHY_PA            0x16180000UL
#define DPHY_ACTRL_SC_06        0x1018
#define DPHY_DCTRL_SC_02        0x1084
#define DPHY_LANE(n)            (0x400u * (n))
#define DPHY_ACTRL_SD_08        0x143C          /* [6:5] SKEW_DLYSEL */
#define DPHY_DCTRL_SD_05        0x14AC          /* bit 2 HS_MODE_SEL */
#define DPHY_DCTRL_SD_06        0x14B0          /* [7:0] HSSETTLECTL */
#define DPHY_DCTRL_SD_13        0x14E0          /* bit 0 SKEWCAL_EN */
#define SETTLE_719              36              /* fimc-is-hw-settle-10nm-lpp.h at 720 */

#define CSIS1_WDMA_PA           0x162C0000UL    /* VC0's; the DMA common of CSIS1 at +0x400 */
#define DMA_CTRL                0x000
#define DMA_FMT                 0x004
#define DMA_ADDR(n)             (0x010 + 4 * (n))
#define DMA_ACT_CTRL            0x030
#define DMA_RESOL               0x040
#define DMA_FCNTSEQ_STAT        0x054
#define DMA_ON                  (1u << 0)       /* the BSP's DMA_N_DISABLE field, which its code sets to switch it on */
#define DMA_UPDT_PTR_EN         (1u << 1)
#define DMA_FRAMEPTR_MASK       (7u << 2)
#define DMA_PACK12              (1u << 3)
#define VCMN                    0x400           /* the DMA common of this CSIS: */
#define VCMN_DATA_CTRL          0x00
#define VCMN_INT_MASK           0x04
#define VCMN_INT_SRC            0x08
#define VCMN_CRC_RESULT         0x0C
#define VCMN_CMN_CTRL           0x14            /* DMA_CMN_CTRL: bit 0 DMA_ABORT_REQ */
#define VCMN_ERR_CODE           0x18
#define VCMN_OTF_FORMAT         0x1C
#define VCMN_IRQ_MASK           0x00F11F00u
#define DMA_FRM_START0          (1u << 4)
#define DMA_FRM_END0            (1u << 8)
#define DMA_ERROR               (1u << 12)
#define DMA_ABORT_DONE          (1u << 16)
#define DMA_OVERLAP0            (1u << 20)

#define CSIS_DMA_PA             0x162F0000UL    /* the DMA common of the four */
#define CDMA_ARB_PRI            0x00
#define CDMA_CTRL               0x08
#define CDMA_SRAM_SPLIT         0x0C
#define CDMA_MATRIX             0x10
#define CDMA_CLK_CTRL           0x14            /* COMMON_DMA_CLK_CTRL: bit 0 CLKGATE_OFF, read only here */
#define TP_ENABLE               0x20
#define TP_CTRL                 0x24
#define TP_RESOL                0x28
#define TP_ON                   (1u << 31)
#define TP_DUAL                 (1u << 30)
#define TP_VBLANK               100000u         /* clocks: a short pause before the frame, whatever the clock is */

#define SYSMMU_PA               0x16310000UL
#define MMU_CTRL                0x000
#define MMU_CFG                 0x004
#define MMU_STATUS              0x008
#define MMU_VERSION             0x034

#define HW_FORMAT_RAW10         0x2B
#define HW_FORMAT_EMBEDDED      0x12
#define HW_FORMAT_UNKNOWN       0x3F
#define FRAME_W                 1632
#define FRAME_H                 1224
#define LINE_BYTES              (FRAME_W * 2)   /* the WDMA's RAW10 (PACK12 off): a pixel in 16 bits, little-endian, [9:0] */
#define BUF_BYTES               (4u << 20)
#define CANARY                  0xA5
/*
 * The frame's memory: none of XNU's own is below 4 GB on the S9 (its RAM is the bank from 0x880000000), so it is
 * physical memory of the lower bank that XNU does not manage: the camera's range in the table of that bank,
 * sys/hakios.exynos9810_mem.h, 32 MB from 0xD8000000 to 0xD9FFFFFF (the display's scan-out buffers below it, UFS's
 * own-mode memory above): a ring of four raw frames of 4 MB each (the WDMA's eight frame pointers take them in turn, pointer i
 * slot i % 4), then camshot's still (a raw frame copied out of the ring while the sensor streams on, cam_s9=7), then
 * its RGB picture (1632 x 1224 x 3). Mapped once, write-combined, through IOKit (hakios_physmap_wc(),
 * sys/hakios.physmap.h), never ml_io_map(). Nothing at or above 0xF0000000 (the modem's and ABOX's memory) is ever
 * given to the DMA.
 */
#define MEM_PA                  EXYNOS9810_MEM_CAMERA_PA
#define MEM_BYTES               ((uint32_t)EXYNOS9810_MEM_SIZE(CAMERA))
#define MEM_FLOOR               EXYNOS9810_MEM_DPU_SCAN_END     /* the display's scan-out buffers end here */
#define MEM_CEILING             EXYNOS9810_MEM_CAMERA_END       /* UFS's own-mode memory */
#define RING_SLOTS              4
#define SLOT_BYTES              BUF_BYTES       /* a raw frame's place: 3264 x 1224 = 3995136 bytes, in 4 MB */
#define STILL_OFF               (RING_SLOTS * SLOT_BYTES)       /* camshot's copy of a frame of the stream */
#define RGB_OFF                 (STILL_OFF + SLOT_BYTES)        /* camshot's picture */
#define RGB_BYTES               (FRAME_W * FRAME_H * 3)

static const uint64_t other_cmn_pa[3] = { 0x162B0400, 0x162D0400, 0x162E0400 };        /* CSIS0, 2, 3: read only */

static vm_offset_t pmu, cmu_isppre, sysreg_isppre, csis, dphy, wdma, cdma, mmu, other_cmn[3];
static bool domain_mine;                /* this run switched ISPPRE on */
static bool domain_up;                  /* ISPPRE is on and its blocks may be read */
static bool phy_mine, link_on, dma_on, pattern_on, streaming;
static uint32_t iso_found, dphy_reset_found, top_gate_found, cdma_found[4];
static bool top_gate_mine, cdma_mine, clocks_mine;
static uint32_t top_mux_found, top_div_found;
static bool top_rate_mine;              /* CLKCMU_ISPPRE_BUS's mux and divider are this run's */
static uint32_t cam_ispdiv = 1;         /* cam_s9_ispdiv: CLKCMU_ISPPRE_BUS = SHARED1_DIV4 (466 MHz) / (1 + this) */
static uint32_t user_found, isppre_qch_found[NQCHS];
static vm_offset_t buffer;              /* the frame's (write-combined: MEM_PA's mapping) */
static vm_offset_t frame;               /* the frame looked at: the ring's slot of the frame taken, or the still */
static uint64_t buffer_pa;
static bool buffer_busy;                /* the WDMA was not seen to stop: no more DMA into it this run */
static uint32_t frame_last;             /* buffer_report()'s: the bytes up to the last one the DMA changed */
static uint32_t frame_ends;             /* sensor_frame()'s frame ends */
static uint32_t cam_exp = 1000, cam_gain = 0x40, cam_pack12;
static uint32_t cam_probe, cam_mmu_probe;      /* cam_s9_probe, cam_s9_mmu_probe: the risky reads, off by default */

/* the SysMMU found translating; never read unless cam_s9_mmu_probe=1 (else taken as off: pass-through) */
static bool
mmu_translating(void)
{
	return cam_mmu_probe && mmu != 0 && (rd(mmu, MMU_CTRL) & 1);
}


/* One SMC to EL3 (through Carbon at EL2): x0 back. */
static uint64_t
smc(uint64_t function, uint64_t first, uint64_t second, uint64_t third)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = first;
	register uint64_t x2 __asm__("x2") = second;
	register uint64_t x3 __asm__("x3") = third;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/* (reg & mask) == want within us microseconds, by the clock; sleeping a millisecond at a time when it may */
static bool
wait_for(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t want, uint32_t us, bool may_sleep)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	for (;;) {
		if ((rd(base, off) & mask) == want) {
			return true;
		}
		if (mach_absolute_time() > until) {
			return (rd(base, off) & mask) == want;
		}
		if (may_sleep && !leaving) {
			delay_for_interval(1, NSEC_PER_MSEC);
		} else {
			spin_us(10);
		}
	}
}

static void
cache_flush(vm_offset_t start, uint32_t bytes)
{
	for (vm_offset_t line = start; line < start + bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
	}
	__asm__ volatile ("dsb sy" : : : "memory");
}

/* ---- the sensor's registers, written ---- */

static bool sensor_broken;

static bool
sensor_put(uint16_t reg, uint16_t value, uint32_t bytes)
{
	uint8_t buf[4] = { (uint8_t)(reg >> 8), (uint8_t)reg, 0, 0 };
	uint32_t error;

	if (bytes == 2) {
		buf[2] = (uint8_t)(value >> 8);
		buf[3] = (uint8_t)value;
	} else {
		buf[2] = (uint8_t)value;
	}
	error = hakios_hsi2c_message(&sensor_bus, sensor_addr, false, buf, 2 + bytes, true);
	if (error != 0) {
		say("exynos_camera: the sensor did not take %#06x = %#x (%x)\n", reg, value, error);
		sensor_broken = true;
		return false;
	}
	return true;
}

/*
 * Setfile A's 1632 x 1224 at 30 frames a second (fimc-is-cis-3h1-setA.h, sensor_3h1_setfile_A_1632x1224_30fps): the
 * sensor's own register values, as data. Every one is a 16-bit write.
 */
static const uint16_t mode_1632x1224[][2] = {
	{ 0xFCFC, 0x4000 }, { 0x6028, 0x2000 }, { 0x6214, 0x7971 }, { 0x6218, 0x7150 }, { 0x0344, 0x0010 },
	{ 0x0348, 0x0CCF }, { 0x0346, 0x0010 }, { 0x034A, 0x099F }, { 0x034C, 0x0660 }, { 0x034E, 0x04C8 },
	{ 0x0408, 0x0000 }, { 0x040A, 0x0000 }, { 0x0342, 0x2970 }, { 0x0340, 0x04F2 }, { 0x0900, 0x0112 },
	{ 0x0386, 0x0003 }, { 0x0400, 0x0001 }, { 0x0404, 0x0020 }, { 0x379E, 0x0104 }, { 0x31B8, 0x0020 },
	{ 0x37FA, 0x0032 }, { 0xF46C, 0x0010 }, { 0xF414, 0x64C6 }, { 0xF416, 0x002F }, { 0xF462, 0x0000 },
	{ 0xF464, 0x0000 }, { 0xF476, 0x0082 }, { 0x37C2, 0x0804 }, { 0x37C6, 0x0403 }, { 0x37BE, 0x0909 },
	{ 0x37BA, 0x130D }, { 0xF44C, 0x0014 }, { 0xF452, 0x000B }, { 0xF44E, 0x0012 }, { 0x37DE, 0x0000 },
	{ 0x37E0, 0x7FE7 }, { 0x37E2, 0x03FF }, { 0x37F0, 0x007F }, { 0x0200, 0x13B8 }, { 0x021C, 0x13B8 },
	{ 0x3158, 0x0060 }, { 0x3254, 0x0001 }, { 0x3258, 0x0001 }, { 0x325C, 0x0003 }, { 0x3260, 0x0002 },
	{ 0x3298, 0x000A }, { 0x32B8, 0x0002 }, { 0x32BC, 0x0000 }, { 0x31B2, 0x0200 }, { 0x3ACE, 0xFECD },
	{ 0x3B00, 0xFEF6 }, { 0x3B32, 0xFEF6 }, { 0x3E0C, 0x0FF7 }, { 0x3DF8, 0x1008 }, { 0x3E02, 0x1008 },
	{ 0x3AC4, 0xFF29 }, { 0x3AF6, 0xFF3E }, { 0x3B28, 0xFF3E }, { 0x3E0A, 0x0FF7 }, { 0x3DF6, 0x1000 },
	{ 0x3E00, 0x1000 }, { 0x3ABA, 0xFF43 }, { 0x3AEC, 0xFF7B }, { 0x3B1E, 0xFF7B }, { 0x3E08, 0x1000 },
	{ 0x3DF4, 0x0FF7 }, { 0x3DFE, 0x0FF7 }, { 0x3DE4, 0x0001 }, { 0x3AA4, 0x0100 }, { 0xF4A2, 0x000C },
	{ 0x0136, 0x1A00 }, { 0x1130, 0x43CA }, { 0x1132, 0x0000 }, { 0x0304, 0x0007 }, { 0x0306, 0x00A3 },
	{ 0x030C, 0x0003 }, { 0x030E, 0x0053 }, { 0x300A, 0x0001 }, { 0x3092, 0x0000 }, { 0x31AA, 0x0001 },
	{ 0xB134, 0x0000 }, { 0x307C, 0x0340 }, { 0x3052, 0x0104 }, { 0x6214, 0x7970 }, { 0x0216, 0x0000 },
};
#define MODE_ENTRIES    (sizeof(mode_1632x1224) / sizeof(mode_1632x1224[0]))
#define MODE_LINES      0x04F2          /* 0x0340 as the table leaves it */
#define MODE_MARGIN     4               /* SENSOR_3H1_COARSE_INTEGRATION_TIME_MAX_MARGIN */

/* ---- stopping things (also on the way out: no sleeping, bounded) ---- */

static bool others_watched;             /* the other CSISs' DMA interrupt masks are this run's (step 4) */
static void others_watch(bool on);

static void
dma_stop(void)
{
	if (wdma == 0 || !domain_up) {
		return;
	}
	if (dma_on) {
		wr(wdma, DMA_CTRL, rd(wdma, DMA_CTRL) & ~(DMA_ON | DMA_UPDT_PTR_EN));
		dma_on = false;
	}
	if (pattern_on) {
		wr(cdma, TP_ENABLE, rd(cdma, TP_ENABLE) & ~TP_ON);
		pattern_on = false;
	}
}

/* the WDMA seen idle: its active state off, or its domain gone */
static bool
dma_quiet(void)
{
	if (!domain_up) {
		return true;
	}
	return wait_for(wdma, DMA_ACT_CTRL, DMA_ON, 0, 100000, false);
}

static void
stream_stop(void)
{
	if (!streaming) {
		return;
	}
	(void)sensor_put(0x0104, 0x00, 1);
	(void)sensor_put(0x6028, 0x4000, 2);
	(void)sensor_put(0x0100, 0x00, 1);
	streaming = false;
	say("exynos_camera: the sensor told to stop streaming (0x0100 = 0)\n");
}

static void
link_stop(void)
{
	if (!link_on || !domain_up) {
		return;
	}
	wr(csis, CSIS_PHY_CMN_CTRL, rd(csis, CSIS_PHY_CMN_CTRL) & ~(PHY_ENABLE_CLK | (0xfu << PHY_ENABLE_DAT_SHIFT)));
	wr(csis, CSIS_CMN_CTRL, (rd(csis, CSIS_CMN_CTRL) & ~CMN_CSI_EN) | CMN_QCHANNEL_EN);
	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) | CMN_SW_RESET);
	(void)wait_for(csis, CSIS_CMN_CTRL, CMN_SW_RESET, 0, 100, false);
	wr(csis, CSIS_INT_MSK0, 0);
	wr(csis, CSIS_INT_MSK1, 0);
	link_on = false;
}

/* the PHY's reset held and its isolation back as found */
static void
phy_off(void)
{
	if (!phy_mine) {
		return;
	}
	if (domain_up && (dphy_reset_found & DPHY_RESET_CSIS1) == 0) {
		wr(sysreg_isppre, DPHY_RESET, rd(sysreg_isppre, DPHY_RESET) & ~DPHY_RESET_CSIS1);
	}
	if ((iso_found & ISO_BYPASS) == 0) {
		wr(pmu, PMU_DPHY_ISO_CSIS1, rd(pmu, PMU_DPHY_ISO_CSIS1) & ~ISO_BYPASS);
	}
	phy_mine = false;
}

/*
 * The domain off again, if this run switched it on (hakios.mali.c's lesson: the low-power setting first, the DTZPC
 * save, then the power; one that does not go off is switched on again with its protection restored, and left on).
 */
static void
isp_down(void)
{
	uint64_t answer;
	uint32_t was;

	dma_stop();
	if (others_watched) {
		others_watch(false);
	}
	link_stop();
	phy_off();
	if (cdma_mine && domain_up) {
		wr(cdma, CDMA_ARB_PRI, cdma_found[0]);
		wr(cdma, CDMA_CTRL, cdma_found[1]);
		wr(cdma, CDMA_SRAM_SPLIT, cdma_found[2]);
		wr(cdma, CDMA_MATRIX, cdma_found[3]);
		cdma_mine = false;
	}
	if (clocks_mine && !domain_mine && domain_up) {
		/* a domain the boot left on stays on: its clocks as they were found */
		for (uint32_t i = 0; i < NQCHS; i++) {
			wr(cmu_isppre, qchs[i], isppre_qch_found[i]);
		}
		wr(cmu_isppre, ISPPRE_BUS_USER, user_found);
	}
	clocks_mine = false;
	if (domain_mine) {
		was = rd(pmu, PMU_ISPPRE_MEMORY);
		wr(pmu, PMU_ISPPRE_MEMORY, (was & ~3u) | 2u);
		answer = smc(DTZPC_SMC, DTZPC_SAVE, DTZPC_ISPPRE, 0);
		say("exynos_camera: the ISPPRE domain off again: MEMORY_ISPPRE_OPTION %#x -> %#x; SMC %#x (%u, %#x): the DTZPC "
		    "save, returned %#llx\n", was, rd(pmu, PMU_ISPPRE_MEMORY), DTZPC_SMC, DTZPC_SAVE, DTZPC_ISPPRE, answer);
		if ((uint32_t)answer != 0) {
			say("exynos_camera: the protection was not saved: the domain is left ON\n");
		} else {
			domain_up = false;
			wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) & ~PD_ON);
			if (!wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, 0, PD_WAIT_US, false)) {
				say("exynos_camera: the domain did not go off (status %#x): on again, its protection restored, left on\n",
				    rd(pmu, PMU_ISPPRE_STATUS));
				wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) | PD_ON);
				if (wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, PD_ON, PD_WAIT_US, false)) {
					answer = smc(DTZPC_SMC, DTZPC_RESTORE, DTZPC_ISPPRE, 0);
					say("exynos_camera: on again; the DTZPC restore returned %#llx\n", answer);
				} else {
					say("exynos_camera: NOT ON AGAIN EITHER: status %#x\n", rd(pmu, PMU_ISPPRE_STATUS));
				}
			}
		}
		domain_mine = false;
		say("exynos_camera: ISPPRE: configuration %#x, status %#x\n", rd(pmu, PMU_ISPPRE_CONFIG),
		    rd(pmu, PMU_ISPPRE_STATUS));
	}
	if (top_rate_mine) {
		/* the divider first, then the mux: the way back goes through the slower of the two */
		wr(cmu_top, TOP_ISPPRE_DIV, top_div_found);
		(void)wait_for(cmu_top, TOP_ISPPRE_DIV, TOP_BUSY, 0, 1000, false);
		wr(cmu_top, TOP_ISPPRE_MUX, top_mux_found);
		(void)wait_for(cmu_top, TOP_ISPPRE_MUX, TOP_BUSY, 0, 1000, false);
		top_rate_mine = false;
	}
	if (top_gate_mine) {
		wr(cmu_top, TOP_ISPPRE_GATE, top_gate_found);
		top_gate_mine = false;
	}
}

/* ---- step 3: the domain on, its clocks, what is there ---- */

/* the power domain's and CMU_ISPPRE's state, every Q-channel with its debug state, for the log (reads only) */
static void
isp_status(const char *step)
{
	char line[2][300];
	uint32_t len[2] = { 0, 0 };

	say("exynos_camera: %s: PD: ISPPRE configuration %#x, status %#x (0xf: on), DPU status %#x; CMU_TOP CLKCMU_ISPPRE_BUS "
	    "mux %#x, divider %#x, gate %#x; CMU_ISPPRE: BUS_USER %#x, PLL_CON2 %#x, BUSP divider %#x, CONTROLLER_OPTION %#x; "
	    "SYSREG_ISPPRE DRCG_EN %#x, MIPI PHY resets %#x\n", step, rd(pmu, PMU_ISPPRE_CONFIG), rd(pmu, PMU_ISPPRE_STATUS),
	    rd(pmu, PMU_DPU_STATUS), rd(cmu_top, TOP_ISPPRE_MUX), rd(cmu_top, TOP_ISPPRE_DIV), rd(cmu_top, TOP_ISPPRE_GATE),
	    rd(cmu_isppre, ISPPRE_BUS_USER), rd(cmu_isppre, 0x0108), rd(cmu_isppre, ISPPRE_BUSP_DIV),
	    rd(cmu_isppre, ISPPRE_CTRL_OPTION), rd(sysreg_isppre, SYSREG_DRCG_EN), rd(sysreg_isppre, DPHY_RESET));
	if (!cam_probe) {
		return;         /* the Q-channel sweep aborted on CMU_ISPPRE 0x303C: cam_s9_probe=1 only */
	}
	for (uint32_t off = QCH_FIRST; off <= QCH_LAST; off += 4) {
		uint32_t h = off < 0x3040 ? 0 : 1;

		if (len[h] < sizeof(line[h])) {
			len[h] += (uint32_t)snprintf(line[h] + len[h], sizeof(line[h]) - len[h], " %x:%x/%x", off & 0xff,
			    rd(cmu_isppre, off), rd(cmu_isppre, off + QCH_DBG));
		}
	}
	say("exynos_camera: %s: CMU_ISPPRE Q-channels, 0x30XX: QCH_CON/DBG_NFO (10 BTM, 14 CMU, 18 3AA, 1c 3AAM, 20-2c "
	    "CSIS0-3, 30/34 PDP cores, 38 PDP_DMA, 3c PDP_STAT_DMA):%s\n", step, line[0]);
	say("exynos_camera: %s: CMU_ISPPRE Q-channels, 0x30XX (40/44 PGEN, 48 PPMU, 4c-58 QE, 5c SysMMU, 60 AXI_P, 64/68 ATB, "
	    "6c AXI_D, 70 SYSREG):%s\n", step, line[1]);
}

static bool
isp_up(void)
{
	uint64_t answer;
	uint32_t status, user;

	say("exynos_camera: 3: map the PMU; read pd-dpu (the parent) and pd-isppre\n");
	if (pmu == 0) {
		pmu = ml_io_map(PMU_PA, PMU_SIZE);       /* once a run: camshot comes back here (io_submap is 8 MB) */
	}
	if (pmu == 0) {
		say("exynos_camera: 3: the PMU could not be mapped\n");
		return false;
	}
	iso_found = rd(pmu, PMU_DPHY_ISO_CSIS1);
	status = rd(pmu, PMU_ISPPRE_STATUS);
	say("exynos_camera: 3: DPU status %#x; ISPPRE configuration %#x, status %#x; MEMORY_ISPPRE_OPTION %#x; the D-PHY's "
	    "isolation (0x14060730) %#x\n", rd(pmu, PMU_DPU_STATUS), rd(pmu, PMU_ISPPRE_CONFIG), status,
	    rd(pmu, PMU_ISPPRE_MEMORY), iso_found);
	if ((rd(pmu, PMU_DPU_STATUS) & PD_ON) != PD_ON) {
		say("exynos_camera: 3: the parent domain (DPU) is off: ISPPRE is not switched on under it\n");
		return false;
	}
	if ((status & PD_ON) == PD_ON) {
		say("exynos_camera: 3: ISPPRE is on already (the boot left it): used as it is, no SMC, and left on\n");
	} else {
		say("exynos_camera: 3: ISPPRE on: configuration [3:0] = 0xF, status waited for (%u ms at the most)\n",
		    PD_WAIT_US / 1000);
		domain_mine = true;
		wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) | PD_ON);
		if (!wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, PD_ON, PD_WAIT_US, true)) {
			say("exynos_camera: 3: the domain did not come on (status %#x): its configuration back to off\n",
			    rd(pmu, PMU_ISPPRE_STATUS));
			wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) & ~PD_ON);
			(void)wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, 0, PD_WAIT_US, true);
			domain_mine = false;
			return false;
		}
		say("exynos_camera: 3: the domain is on (status %#x); about to make SMC %#x (%u, %#x, 0): the DTZPC restore; "
		    "Carbon, at EL2, passes it on to EL3\n", rd(pmu, PMU_ISPPRE_STATUS), DTZPC_SMC, DTZPC_RESTORE, DTZPC_ISPPRE);
		answer = smc(DTZPC_SMC, DTZPC_RESTORE, DTZPC_ISPPRE, 0);
		say("exynos_camera: 3: SMC %#x returned %#llx\n", DTZPC_SMC, answer);
		if ((uint32_t)answer != 0) {
			say("exynos_camera: 3: %s: nothing in the domain is touched; its power goes again (no save: nothing was "
			    "restored)\n", answer == (uint64_t)-1 ? "the call is not known to whoever answered it" : "a refusal");
			wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) & ~PD_ON);
			(void)wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, 0, PD_WAIT_US, true);
			domain_mine = false;
			return false;
		}
	}
	domain_up = true;

	say("exynos_camera: 3: map CMU_ISPPRE, SYSREG_ISPPRE, CSIS1, its D-PHY, its WDMA and the DMA common%s%s\n",
	    cam_mmu_probe ? ", the SysMMU" : "", cam_probe ? ", the other CSISs' DMA commons" : "");
	if (cmu_isppre == 0) {
		cmu_isppre = ml_io_map(CMU_ISPPRE_PA, 0x8000);    /* the Q-channels' debug states are at +0x7010 */
		sysreg_isppre = ml_io_map(SYSREG_ISPPRE_PA, 0x1000);
		csis = ml_io_map(CSIS1_PA, 0x1000);
		dphy = ml_io_map(CSIS1_PHY_PA, 0x3000);
		wdma = ml_io_map(CSIS1_WDMA_PA, 0x1000);
		cdma = ml_io_map(CSIS_DMA_PA, 0x1000);
		if (cam_mmu_probe) {
			mmu = ml_io_map(SYSMMU_PA, 0x1000);
		}
		if (cam_probe) {
			for (uint32_t i = 0; i < 3; i++) {
				other_cmn[i] = ml_io_map(other_cmn_pa[i] & ~0xfffULL, 0x1000);
			}
		}
	}
	if (cmu_isppre == 0 || sysreg_isppre == 0 || csis == 0 || dphy == 0 || wdma == 0 || cdma == 0 ||
	    (cam_mmu_probe && mmu == 0) ||
	    (cam_probe && (other_cmn[0] == 0 || other_cmn[1] == 0 || other_cmn[2] == 0))) {
		say("exynos_camera: 3: a block could not be mapped\n");
		return false;
	}

	top_gate_found = rd(cmu_top, TOP_ISPPRE_GATE);
	say("exynos_camera: 3: CMU_TOP's CLKCMU_ISPPRE_BUS: mux %#x, divider %#x, gate %#x; CMU_ISPPRE: BUS_USER %#x, BUSP "
	    "divider %#x, Q-channels CSIS1 %#x, SysMMU %#x\n", rd(cmu_top, TOP_ISPPRE_MUX), rd(cmu_top, TOP_ISPPRE_DIV),
	    top_gate_found, rd(cmu_isppre, ISPPRE_BUS_USER), rd(cmu_isppre, ISPPRE_BUSP_DIV), rd(cmu_isppre, QCH_CSIS1),
	    rd(cmu_isppre, QCH_SYSMMU));
	if ((top_gate_found & (GATE_MANUAL | GATE_ON)) != (GATE_MANUAL | GATE_ON)) {
		wr(cmu_top, TOP_ISPPRE_GATE, top_gate_found | GATE_MANUAL | GATE_ON);
		top_gate_mine = true;
	}
	/*
	 * The bus's rate. The boot leaves CLKCMU_ISPPRE_BUS at its reset setting, mux 2 (SHARED2_DIV2) divided by 16:
	 * about 50 MHz, and at that rate CSIS1 overflowed and VC0's WDMA filled its FIFO (DMA_ERROR) 28 lines into the
	 * frame (cam-fix2, 3 November). Android's INT table (cmucal-vclklut.c, vddi's nm level) has mux 1, SHARED1_DIV4,
	 * undivided: 466 MHz (BUSP, half of it, 233 MHz: blk_isppre). Taken here: mux 1 divided by 1 + cam_s9_ispdiv
	 * (default 1: 233 MHz, half Android's, no voltage asked of INT). Mux first (to the lower rate), then the divider
	 */
	top_mux_found = rd(cmu_top, TOP_ISPPRE_MUX);
	top_div_found = rd(cmu_top, TOP_ISPPRE_DIV);
	if ((top_mux_found & 3u) != ISPPRE_MUX_SHARED1_DIV4 || (top_div_found & 0xfu) != cam_ispdiv) {
		top_rate_mine = true;
		wr(cmu_top, TOP_ISPPRE_MUX, (top_mux_found & ~3u) | ISPPRE_MUX_SHARED1_DIV4);
		if (!wait_for(cmu_top, TOP_ISPPRE_MUX, TOP_BUSY, 0, 1000, true)) {
			say("exynos_camera: 3: CLKCMU_ISPPRE_BUS's mux stays busy (%#x)\n", rd(cmu_top, TOP_ISPPRE_MUX));
		}
		wr(cmu_top, TOP_ISPPRE_DIV, (top_div_found & ~0xfu) | cam_ispdiv);
		if (!wait_for(cmu_top, TOP_ISPPRE_DIV, TOP_BUSY, 0, 1000, true)) {
			say("exynos_camera: 3: CLKCMU_ISPPRE_BUS's divider stays busy (%#x)\n", rd(cmu_top, TOP_ISPPRE_DIV));
		}
	}
	say("exynos_camera: 3: CLKCMU_ISPPRE_BUS: mux %#x -> %#x, divider %#x -> %#x (SHARED1_DIV4, 466 MHz, / %u)\n",
	    top_mux_found, rd(cmu_top, TOP_ISPPRE_MUX), top_div_found, rd(cmu_top, TOP_ISPPRE_DIV), cam_ispdiv + 1);
	user = user_found = rd(cmu_isppre, ISPPRE_BUS_USER);
	for (uint32_t i = 0; i < NQCHS; i++) {
		isppre_qch_found[i] = rd(cmu_isppre, qchs[i]);
	}
	clocks_mine = true;
	if ((user & (1u << 4)) == 0) {
		wr(cmu_isppre, ISPPRE_BUS_USER, user | (1u << 4));
		if (!wait_for(cmu_isppre, ISPPRE_BUS_USER, 1u << 7, 0, 1000, true)) {
			say("exynos_camera: 3: the ISPPRE bus's mux stays busy (%#x)\n", rd(cmu_isppre, ISPPRE_BUS_USER));
		}
	}
	for (uint32_t i = 0; i < NQCHS; i++) {
		wr(cmu_isppre, qchs[i], isppre_qch_found[i] | QCH_REQ);
	}
	say("exynos_camera: 3: clocks: CMU_TOP's gate %#x, BUS_USER %#x (bit 4: CMU_TOP's clock), Q-channels CSIS1 %#x, "
	    "SysMMU %#x (bit 1: requested)\n", rd(cmu_top, TOP_ISPPRE_GATE), rd(cmu_isppre, ISPPRE_BUS_USER),
	    rd(cmu_isppre, QCH_CSIS1), rd(cmu_isppre, QCH_SYSMMU));
	isp_status("3");

	dphy_reset_found = rd(sysreg_isppre, DPHY_RESET);
	say("exynos_camera: 3: CSIS1: version %#x, CMN_CTRL %#x, PHY_STATUS %#x, PHY_CMN_CTRL %#x; the PHYs' resets "
	    "(0x16210500) %#x\n", rd(csis, CSIS_VERSION), rd(csis, CSIS_CMN_CTRL), rd(csis, CSIS_PHY_STATUS),
	    rd(csis, CSIS_PHY_CMN_CTRL), dphy_reset_found);
	say("exynos_camera: 3: VC0's WDMA: CTRL %#x, FMT %#x, ACT_CTRL %#x, ADDR1 %#x; its DMA common: INT_SRC %#x, ERR_CODE "
	    "%#x; the DMA common: ARB_PRI %#x, CTRL %#x, SRAM_SPLIT %#x, MATRIX %#x, TEST_PATTERN %#x\n",
	    rd(wdma, DMA_CTRL), rd(wdma, DMA_FMT), rd(wdma, DMA_ACT_CTRL), rd(wdma, DMA_ADDR(0)),
	    rd(wdma, VCMN + VCMN_INT_SRC),
	    rd(wdma, VCMN + VCMN_ERR_CODE), rd(cdma, CDMA_ARB_PRI), rd(cdma, CDMA_CTRL), rd(cdma, CDMA_SRAM_SPLIT),
	    rd(cdma, CDMA_MATRIX), rd(cdma, TP_ENABLE));
	if (cam_mmu_probe) {
		say("exynos_camera: 3: the SysMMU: MMU_CTRL %#x (bit 0: translating), CFG %#x, STATUS %#x, VERSION %#x\n",
		    rd(mmu, MMU_CTRL), rd(mmu, MMU_CFG), rd(mmu, MMU_STATUS), rd(mmu, MMU_VERSION));
	} else {
		say("exynos_camera: 3: the SysMMU is not read (cam_s9_mmu_probe=1 reads it): taken as off, pass-through\n");
	}
	if ((rd(csis, CSIS_VERSION) == 0 || rd(csis, CSIS_VERSION) == 0xffffffffu)) {
		say("exynos_camera: 3: CSIS1's version reads %#x: is its clock there?\n", rd(csis, CSIS_VERSION));
	}
	return true;
}

/* ---- step 4: the PHY, the link set up, the WDMA, the test pattern ---- */

static bool
buffer_get(void)
{
	if (buffer != 0) {
		return !buffer_busy;
	}
	if (MEM_PA < MEM_FLOOR || MEM_PA + MEM_BYTES > MEM_CEILING || MEM_PA + MEM_BYTES > 0x100000000ULL) {
		say("exynos_camera: the frame's memory %#llx + %u is not in the gap %#llx - %#llx: not used\n", MEM_PA, MEM_BYTES,
		    MEM_FLOOR, MEM_CEILING);
		return false;
	}
	buffer = hakios_physmap_wc(MEM_PA, MEM_BYTES);
	if (buffer == 0) {
		say("exynos_camera: the frame's memory (%u bytes at %#llx, physical) cannot be mapped\n", MEM_BYTES, MEM_PA);
		return false;
	}
	buffer_pa = MEM_PA;
	say("exynos_camera: the buffer: %u bytes at %#llx (physical, outside XNU's RAM, below 4 GB; mapped write-combined "
	    "at %#lx)\n", BUF_BYTES, buffer_pa, (unsigned long)buffer);
	return true;
}

static void
buffer_fill(void)
{
	memset((void *)buffer, CANARY, RING_SLOTS * SLOT_BYTES);
	cache_flush(buffer, RING_SLOTS * SLOT_BYTES);
	frame = buffer;
}

/* What the DMA wrote: how much, how far, a checksum, the first bytes. Returns the bytes up to the last one changed. */
static uint32_t
buffer_report(const char *step)
{
	const uint8_t *b = (const uint8_t *)frame;
	uint32_t changed = 0, last = 0, sum = 0, fnv = 2166136261u;

	cache_flush(frame, BUF_BYTES);
	/* eight bytes a read: the mapping is not cached, and a byte a read over 4 MB is slow */
	for (uint32_t i = 0; i < BUF_BYTES; i += 8) {
		uint64_t w = *(const volatile uint64_t *)(frame + i);

		if (w == 0xA5A5A5A5A5A5A5A5ULL) {
			continue;
		}
		for (uint32_t k = 0; k < 8; k++) {
			if (((w >> (8 * k)) & 0xff) != CANARY) {
				changed++;
				last = i + k + 1;
			}
		}
	}
	for (uint32_t i = 0; i < last; i += 8) {
		uint64_t w = *(const volatile uint64_t *)(frame + i);

		for (uint32_t k = 0; k < 8 && i + k < last; k++) {
			uint8_t c = (uint8_t)(w >> (8 * k));

			sum += c;
			fnv = (fnv ^ c) * 16777619u;
		}
	}
	frame_last = last;
	say("exynos_camera: %s: the buffer: %u bytes changed, the last at %u (%u a line over %u lines); sum %#x, FNV-1a "
	    "%#x\n", step, changed, last, last / FRAME_H, FRAME_H, sum, fnv);
	say("exynos_camera: %s: its first bytes: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x "
	    "%02x; at %u: %02x %02x %02x %02x %02x %02x %02x %02x\n", step, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
	    b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15], LINE_BYTES, b[LINE_BYTES], b[LINE_BYTES + 1],
	    b[LINE_BYTES + 2], b[LINE_BYTES + 3], b[LINE_BYTES + 4], b[LINE_BYTES + 5], b[LINE_BYTES + 6], b[LINE_BYTES + 7]);
	return last;
}

/* the D-PHY's isolation off and its reset released (exynos_mipi_phy_power_on) */
static void
phy_on(void)
{
	say("exynos_camera: 4: the D-PHY (M0S4S2, CSIS1): isolation bypass, PMU 0x730 %#x |= 1; reset bit 2 of 0x16210500 "
	    "(%#x) cleared, then set\n", iso_found, rd(sysreg_isppre, DPHY_RESET));
	phy_mine = true;
	wr(pmu, PMU_DPHY_ISO_CSIS1, rd(pmu, PMU_DPHY_ISO_CSIS1) | ISO_BYPASS);
	wr(sysreg_isppre, DPHY_RESET, rd(sysreg_isppre, DPHY_RESET) & ~DPHY_RESET_CSIS1);
	wr(sysreg_isppre, DPHY_RESET, rd(sysreg_isppre, DPHY_RESET) | DPHY_RESET_CSIS1);
	say("exynos_camera: 4: the D-PHY: isolation %#x, resets %#x\n", rd(pmu, PMU_DPHY_ISO_CSIS1),
	    rd(sysreg_isppre, DPHY_RESET));
}

/* CSIS1 reset and set up for the mode (csi_hw_reset, csi_hw_s_lane, _s_control, _s_config, _s_config_dma(_cmn)) */
static bool
link_setup(void)
{
	uint32_t v;

	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) | CMN_SW_RESET);
	if (!wait_for(csis, CSIS_CMN_CTRL, CMN_SW_RESET, 0, 100, false)) {
		say("exynos_camera: CSIS1's reset does not finish (CMN_CTRL %#x)\n", rd(csis, CSIS_CMN_CTRL));
		return false;
	}
	link_on = true;         /* (from here, the way down resets it again) */
	wr(csis, CSIS_INT_MSK0, CSIS_IRQ_MASK0);
	wr(csis, CSIS_INT_MSK1, CSIS_IRQ_MASK1);
	wr(csis, CSIS_INT_SRC0, rd(csis, CSIS_INT_SRC0));
	wr(csis, CSIS_INT_SRC1, rd(csis, CSIS_INT_SRC1));
	v = rd(csis, CSIS_CMN_CTRL) & ~((3u << CMN_LANES_SHIFT) | (3u << CMN_INTERLEAVE_SHIFT));
	wr(csis, CSIS_CMN_CTRL, v | (1u << CMN_LANES_SHIFT) | (3u << CMN_INTERLEAVE_SHIFT));
	wr(csis, CSIS_PHY_CMN_CTRL, (rd(csis, CSIS_PHY_CMN_CTRL) & ~(0xfu << PHY_ENABLE_DAT_SHIFT)) |
	    (3u << PHY_ENABLE_DAT_SHIFT));
	/* VC0 RAW10 1632 x 1224, two pixels a clock; CH1 nothing; CH2 VC0's embedded data (0 x 0); CH3 nothing */
	wr(csis, CSIS_ISP_CONFIG(0), (rd(csis, CSIS_ISP_CONFIG(0)) & ~0x33ffu) | (1u << 12) | (HW_FORMAT_RAW10 << 2) | 0);
	wr(csis, CSIS_ISP_RESOL(0), (FRAME_H << 16) | FRAME_W);
	wr(csis, CSIS_ISP_CONFIG(1), (rd(csis, CSIS_ISP_CONFIG(1)) & ~0x33ffu) | (1u << 12) | (HW_FORMAT_UNKNOWN << 2) | 1);
	wr(csis, CSIS_ISP_RESOL(1), 0);
	wr(csis, CSIS_ISP_CONFIG(2), (rd(csis, CSIS_ISP_CONFIG(2)) & ~0x33ffu) | (1u << 12) | (HW_FORMAT_EMBEDDED << 2) | 0);
	wr(csis, CSIS_ISP_RESOL(2), 0);
	wr(csis, CSIS_ISP_CONFIG(3), (rd(csis, CSIS_ISP_CONFIG(3)) & ~0x33ffu) | (1u << 12) | (HW_FORMAT_UNKNOWN << 2) | 3);
	wr(csis, CSIS_ISP_RESOL(3), 0);

	/* VC0's WDMA: 2D, legacy storage, RAW10 (or PACK12), the width; its common: OTF in, RAW10 */
	v = rd(wdma, DMA_FMT) & ~0x3fu;
	wr(wdma, DMA_FMT, v | (cam_pack12 ? DMA_PACK12 : 0));
	wr(wdma, DMA_RESOL, (rd(wdma, DMA_RESOL) & ~0x3fffu) | FRAME_W);
	wr(wdma, VCMN + VCMN_OTF_FORMAT, rd(wdma, VCMN + VCMN_OTF_FORMAT) & ~3u);
	wr(wdma, VCMN + VCMN_DATA_CTRL, rd(wdma, VCMN + VCMN_DATA_CTRL) & ~1u);
	wr(wdma, VCMN + VCMN_INT_MASK, VCMN_IRQ_MASK);
	wr(wdma, VCMN + VCMN_INT_SRC, rd(wdma, VCMN + VCMN_INT_SRC));
	say("exynos_camera: CSIS1 set up: CMN_CTRL %#x, PHY_CMN_CTRL %#x, CH0 %#x %#x, CH2 %#x; WDMA FMT %#x, RESOL %#x; "
	    "its common: DATA_CTRL %#x, OTF_FORMAT %#x\n", rd(csis, CSIS_CMN_CTRL), rd(csis, CSIS_PHY_CMN_CTRL),
	    rd(csis, CSIS_ISP_CONFIG(0)), rd(csis, CSIS_ISP_RESOL(0)), rd(csis, CSIS_ISP_CONFIG(2)), rd(wdma, DMA_FMT),
	    rd(wdma, DMA_RESOL), rd(wdma, VCMN + VCMN_DATA_CTRL), rd(wdma, VCMN + VCMN_OTF_FORMAT));
	return true;
}

/* the buffer to all eight frame pointers and the output on (csi_hw_s_dma_addr, csi_hw_s_output_dma) */
static void
dma_start(void)
{
	wr(wdma, DMA_CTRL, rd(wdma, DMA_CTRL) & ~DMA_FRAMEPTR_MASK);
	for (uint32_t i = 0; i < 8; i++) {
		wr(wdma, DMA_ADDR(i), (uint32_t)buffer_pa);
	}
	wr(wdma, VCMN + VCMN_INT_SRC, rd(wdma, VCMN + VCMN_INT_SRC));
	dma_on = true;
	wr(wdma, DMA_CTRL, rd(wdma, DMA_CTRL) | DMA_ON | DMA_UPDT_PTR_EN);
}

/* the DMA common as csi_hw_s_dma_common leaves it (found values kept, put back on the way down) */
static void
dma_common(void)
{
	if (!cdma_mine) {
		cdma_found[0] = rd(cdma, CDMA_ARB_PRI);
		cdma_found[1] = rd(cdma, CDMA_CTRL);
		cdma_found[2] = rd(cdma, CDMA_SRAM_SPLIT);
		cdma_found[3] = rd(cdma, CDMA_MATRIX);
		cdma_mine = true;
	}
	wr(cdma, CDMA_ARB_PRI, (cdma_found[0] & ~0xfu) | (1u << 2) | 2u);
	wr(cdma, CDMA_CTRL, cdma_found[1] | (1u << 1));
	wr(cdma, CDMA_SRAM_SPLIT, (cdma_found[2] & ~0x03ff03ffu) | (0x140u << 16) | 0x140u);
	wr(cdma, CDMA_MATRIX, cdma_found[3] & ~0x1fu);
}

/* the link's and the WDMA's state as last read (camshot's failure line gives it) */
static struct {
	bool taken;
	uint32_t src0, src1, frm, phy, ctrl, act, fcnt, isrc, err;
} snap;

static void
snap_take(void)
{
	if (!domain_up || csis == 0 || wdma == 0) {
		return;
	}
	snap.src0 = rd(csis, CSIS_INT_SRC0);
	snap.src1 = rd(csis, CSIS_INT_SRC1);
	snap.frm = rd(csis, CSIS_FRM_CNT_CH0);
	snap.phy = rd(csis, CSIS_PHY_STATUS);
	snap.ctrl = rd(wdma, DMA_CTRL);
	snap.act = rd(wdma, DMA_ACT_CTRL);
	snap.fcnt = rd(wdma, DMA_FCNTSEQ_STAT);
	snap.isrc = rd(wdma, VCMN + VCMN_INT_SRC);
	snap.err = rd(wdma, VCMN + VCMN_ERR_CODE);
	snap.taken = true;
}

static void
dma_log(const char *step)
{
	snap_take();
	say("exynos_camera: %s: WDMA CTRL %#x, ACT_CTRL %#x, FCNTSEQ_STAT %#x; its common: INT_SRC %#x (frame start [7:4], "
	    "end [11:8], error 12, abort done 16), ERR_CODE %#x, CRC %#x, CMN_CTRL %#x; the pattern %#x\n", step,
	    rd(wdma, DMA_CTRL), rd(wdma, DMA_ACT_CTRL), rd(wdma, DMA_FCNTSEQ_STAT), rd(wdma, VCMN + VCMN_INT_SRC),
	    rd(wdma, VCMN + VCMN_ERR_CODE), rd(wdma, VCMN + VCMN_CRC_RESULT), rd(wdma, VCMN + VCMN_CMN_CTRL),
	    rd(cdma, TP_ENABLE));
	if (cam_probe) {
		say("exynos_camera: %s: the others' INT_SRC (VC0 ACT_CTRL): CSIS0 %#x (%#x), CSIS2 %#x (%#x), CSIS3 %#x (%#x)\n",
		    step, rd(other_cmn[0], 0x400 + VCMN_INT_SRC), rd(other_cmn[0], DMA_ACT_CTRL),
		    rd(other_cmn[1], 0x400 + VCMN_INT_SRC), rd(other_cmn[1], DMA_ACT_CTRL),
		    rd(other_cmn[2], 0x400 + VCMN_INT_SRC), rd(other_cmn[2], DMA_ACT_CTRL));
	}
	say("exynos_camera: %s: CSIS1 INT_SRC0 %#x (frame start [27:24], end [23:20], SOT [19:16], lost FS [15:12], lost FE "
	    "[11:8], overflow 4, wrong config 3, ECC 2, CRC 1, ID 0), INT_SRC1 %#x (V/H size mismatch [15:12]/[11:8]), "
	    "FRM_CNT_CH0 %u, PHY_STATUS %#x\n", step, rd(csis, CSIS_INT_SRC0), rd(csis, CSIS_INT_SRC1),
	    rd(csis, CSIS_FRM_CNT_CH0), rd(csis, CSIS_PHY_STATUS));
}

/* the other three CSISs' DMA interrupts unmasked for the pattern's while (so a frame that goes there shows), and back */
static uint32_t other_mask_found[3];

static void
others_watch(bool on)
{
	if (!cam_probe) {
		return;         /* the other CSISs are not touched unless cam_s9_probe=1 */
	}
	for (uint32_t i = 0; i < 3; i++) {
		if (on) {
			other_mask_found[i] = rd(other_cmn[i], 0x400 + VCMN_INT_MASK);
			wr(other_cmn[i], 0x400 + VCMN_INT_MASK, VCMN_IRQ_MASK);
			wr(other_cmn[i], 0x400 + VCMN_INT_SRC, rd(other_cmn[i], 0x400 + VCMN_INT_SRC));
		} else if (others_watched && domain_up) {
			wr(other_cmn[i], 0x400 + VCMN_INT_MASK, other_mask_found[i]);
		}
	}
	others_watched = on;
}

static void
pattern_on_now(void)
{
	wr(cdma, TP_CTRL, (rd(cdma, TP_CTRL) & ~0xffffffu) | (16u << 16) | (8u << 8) | 2u);
	wr(cdma, TP_RESOL, (FRAME_H << 16) | FRAME_W);
	wr(cdma, TP_ENABLE, TP_DUAL | TP_VBLANK);
	pattern_on = true;
	wr(cdma, TP_ENABLE, rd(cdma, TP_ENABLE) | TP_ON);
}

static bool
pattern_frame(void)
{
	uint32_t last, cmn;
	bool ended;

	if (mmu_translating()) {
		say("exynos_camera: 4: the SysMMU is on (MMU_CTRL %#x): the WDMA's addresses would be translated by a table "
		    "this driver did not make; no DMA\n", rd(mmu, MMU_CTRL));
		return false;
	}
	if (!buffer_get()) {
		return false;
	}
	buffer_fill();
	phy_on();
	say("exynos_camera: 4: CSIS1 reset and set up for %u x %u RAW10 on VC0 (%s), two lanes\n", FRAME_W, FRAME_H,
	    cam_pack12 ? "PACK12" : "RAW10 as it comes");
	if (!link_setup()) {
		return false;
	}
	dma_common();
	say("exynos_camera: 4: VC0's WDMA to %#llx, the output on; then the test pattern generator, %u x %u, two pixels a "
	    "clock, vertical blank %u clocks: ONE frame, its end waited for (2 s at the most)\n", buffer_pa, FRAME_W, FRAME_H,
	    TP_VBLANK);
	cmn = rd(wdma, VCMN + VCMN_CMN_CTRL);
	if (cmn & 1) {
		wr(wdma, VCMN + VCMN_CMN_CTRL, cmn & ~1u);
	}
	say("exynos_camera: 4: before: WDMA ACT_CTRL %#x (bit 16: aborted, bit 0: active), DMA_CMN_CTRL %#x -> %#x (bit 0: "
	    "abort request); CSIS1 CLK_CTRL %#x; the DMA common: CTRL %#x, CLK_CTRL %#x\n", rd(wdma, DMA_ACT_CTRL), cmn,
	    rd(wdma, VCMN + VCMN_CMN_CTRL), rd(csis, CSIS_CLK_CTRL), rd(cdma, CDMA_CTRL), rd(cdma, CDMA_CLK_CTRL));
	if (cam_mmu_probe) {
		say("exynos_camera: 4: the SysMMU: CTRL %#x, CFG %#x, STATUS %#x\n", rd(mmu, MMU_CTRL), rd(mmu, MMU_CFG),
		    rd(mmu, MMU_STATUS));
	}
	others_watch(true);
	dma_start();
	pattern_on_now();
	ended = wait_for(wdma, VCMN + VCMN_INT_SRC, DMA_FRM_END0, DMA_FRM_END0, 500000, true);
	if (!ended && (rd(wdma, VCMN + VCMN_INT_SRC) & DMA_FRM_START0) == 0) {
		dma_log("4, the link off");
		say("exynos_camera: 4: no frame start on CSIS1's WDMA in 500 ms with the link off (Android's way for the "
		    "pattern): the pattern off, CSIS1's link enabled as csi_hw_enable does (UPDATE_SHADOW, PHY_SEL 0, no "
		    "Q-channel, CSI_EN; no clock lane: no sensor), the pattern on again, 1.5 s\n");
		wr(cdma, TP_ENABLE, rd(cdma, TP_ENABLE) & ~TP_ON);
		delay_for_interval(5, NSEC_PER_MSEC);
		wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) | CMN_UPDATE_SHADOW);
		wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) & ~(CMN_PHY_SEL | CMN_QCHANNEL_EN));
		wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) | CMN_CSI_EN);
		wr(wdma, VCMN + VCMN_INT_SRC, rd(wdma, VCMN + VCMN_INT_SRC));
		pattern_on_now();
		ended = wait_for(wdma, VCMN + VCMN_INT_SRC, DMA_FRM_END0, DMA_FRM_END0, 1500000, true);
		say("exynos_camera: 4: with the link on: %s (CMN_CTRL %#x)\n", ended ? "THE FRAME ENDED" :
		    "no frame end either", rd(csis, CSIS_CMN_CTRL));
	} else if (!ended) {
		/* it started: give it the rest of the 2 s */
		ended = wait_for(wdma, VCMN + VCMN_INT_SRC, DMA_FRM_END0, DMA_FRM_END0, 1500000, true);
	}
	dma_stop();
	delay_for_interval(50, NSEC_PER_MSEC);
	say("exynos_camera: 4: %s; the WDMA and the pattern off\n", ended ? "THE FRAME ENDED" : "no frame end in 2 s");
	dma_log("4");
	others_watch(false);
	if (!ended) {
		isp_status("4");
	}
	if (!dma_quiet()) {
		say("exynos_camera: 4: the WDMA still reads active (ACT_CTRL %#x): no more DMA into the buffer this run\n",
		    rd(wdma, DMA_ACT_CTRL));
		buffer_busy = true;
	}
	last = buffer_report("4");
	if (last == 0) {
		say("exynos_camera: 4: nothing reached the buffer\n");
		return false;
	}
	say("exynos_camera: 4: THE PATTERN REACHED MEMORY: %u bytes (%u x %u RAW10 packed would be %u)\n", last, FRAME_W,
	    FRAME_H, FRAME_W * FRAME_H * 10 / 8);
	return true;
}

/* ---- step 5: a frame of the sensor's ---- */

static uint32_t
sensor_get16(uint16_t reg)
{
	uint8_t b[2] = { 0, 0 };

	return sensor_get(reg, b, 2) ? 0xffffffffu : (uint32_t)((b[0] << 8) | b[1]);
}

/* the D-PHY's timing for 719 Mb/s a lane (csi_hw_s_phy_default_value, csi_hw_s_phy_config, not USE_PHY_LINK) */
static void
phy_setup(void)
{
	wr(dphy, DPHY_ACTRL_SC_06, 2);
	wr(dphy, DPHY_DCTRL_SC_02, (rd(dphy, DPHY_DCTRL_SC_02) & ~0xffu) | 1u);
	for (uint32_t lane = 0; lane < 2; lane++) {
		wr(dphy, DPHY_ACTRL_SD_08 + DPHY_LANE(lane), rd(dphy, DPHY_ACTRL_SD_08 + DPHY_LANE(lane)) & ~(3u << 5));
		wr(dphy, DPHY_DCTRL_SD_13 + DPHY_LANE(lane), rd(dphy, DPHY_DCTRL_SD_13 + DPHY_LANE(lane)) & ~1u);
		wr(dphy, DPHY_DCTRL_SD_05 + DPHY_LANE(lane), rd(dphy, DPHY_DCTRL_SD_05 + DPHY_LANE(lane)) | (1u << 2));
		wr(dphy, DPHY_DCTRL_SD_06 + DPHY_LANE(lane), (rd(dphy, DPHY_DCTRL_SD_06 + DPHY_LANE(lane)) & ~0xffu) |
		    SETTLE_719);
	}
	say("exynos_camera: 5: the D-PHY: SC_06 %#x, SC_02 %#x; lane 0: SD_08 %#x, SD_05 %#x, SD_06 %#x (settle), SD_13 %#x; "
	    "lane 1: %#x %#x %#x %#x\n", rd(dphy, DPHY_ACTRL_SC_06), rd(dphy, DPHY_DCTRL_SC_02), rd(dphy, DPHY_ACTRL_SD_08),
	    rd(dphy, DPHY_DCTRL_SD_05), rd(dphy, DPHY_DCTRL_SD_06), rd(dphy, DPHY_DCTRL_SD_13),
	    rd(dphy, DPHY_ACTRL_SD_08 + DPHY_LANE(1)), rd(dphy, DPHY_DCTRL_SD_05 + DPHY_LANE(1)),
	    rd(dphy, DPHY_DCTRL_SD_06 + DPHY_LANE(1)), rd(dphy, DPHY_DCTRL_SD_13 + DPHY_LANE(1)));
}

/* the mean of a 64 x 64 patch in the middle, read as RAW10 in 16 bits (the WDMA's) at the stride given */
static void
patch_mean(uint32_t stride)
{
	const uint8_t *b = (const uint8_t *)frame;
	uint64_t sum = 0;
	uint32_t lo = 1023, hi = 0, n = 0, over = 0;

	if (stride < LINE_BYTES || (uint64_t)stride * FRAME_H > BUF_BYTES) {
		say("exynos_camera: 5: a stride of %u does not hold the frame: no patch\n", stride);
		return;
	}
	for (uint32_t y = FRAME_H / 2 - 32; y < FRAME_H / 2 + 32; y++) {
		const uint8_t *line = b + (uint64_t)y * stride;

		for (uint32_t x = FRAME_W / 2 - 32; x < FRAME_W / 2 + 32; x++) {
			uint32_t w = (uint32_t)line[2 * x] | ((uint32_t)line[2 * x + 1] << 8);
			uint32_t p = w & 0x3ff;

			over += w > 0x3ff;
			sum += p;
			lo = p < lo ? p : lo;
			hi = p > hi ? p : hi;
			n++;
		}
	}
	say("exynos_camera: 5: THE PATCH (64 x 64 in the middle, 16-bit RAW10 at a stride of %u): mean %llu.%02llu of 1023 "
	    "(about 64 is black), %u to %u; %u samples above 10 bits (the fill's 0xA5A5, or not RAW10 in 16 bits)\n", stride,
	    sum / n, (sum % n) * 100 / n, lo, hi, over);
}

/*
 * ---- the exposure: the 3H1's coarse integration time, analog gain and digital gain over I2C ----
 *
 * Registers (fimc-is-cis-3h1.c, as facts): 0x0202 the coarse integration time in lines (at least 2, at most the frame's
 * length 0x0340 less 4), 0x0204 the analog gain (0x20 is 1x; the sensor's own least and most at 0x0084 and 0x0086),
 * 0x020E the digital gain (0x100 is 1x, 8.8; its most at 0x1086), 0x0104 the grouped parameter hold around them. The
 * mode's frame stays 1266 lines (30 frames a second): the exposure is at most 1262 lines of 26.3 us, 33.2 ms.
 *
 * The meter: the mean of the green of every 16th 2 x 2 cell across and down the middle half of the frame (about 1900
 * cells), the black level off; the aim 18 % of the range above it (173 of 959). The exposure asked is the one before
 * times aim / mean (a step of at most 8x either way, half the step while streaming), spent on time first, then on
 * analog gain, then on digital gain (at most cam_s9_dgain, 4x unless said). A change written after frame N's end
 * shows in frame N + 3 (seen on the phone, 3 November: the sensor latches it at a frame start, and the next frame has
 * begun integrating already), so each one is measured on the third frame after it.
 */
#define AE_AIM                  ((1023 - BLACK_LEVEL) * 18 / 100)
#define BLACK_LEVEL             64
#define AE_CLIP                 1000            /* a sample at or above this is clipped */

static uint32_t ae_lines, ae_again = 0x20, ae_dgain = 0x100;   /* as last written */
static uint32_t again_min = 0x20, again_max = 0x200, dgain_max = 0x400;
static uint32_t cam_ae = 1, cam_ae_frames = 15, cam_dgain = 0x400;
static uint32_t shot_slot;               /* the ring's slot of the frame taken */
static uint32_t dma_errors, slot_fix, ring_written, ring_checks;     /* frame_wait()'s */

struct meter {
	uint32_t mean[3];       /* R, G, B less the black level */
	uint32_t clipped;       /* per mille of the green samples */
	uint32_t cells;
};

static uint32_t cam_bayer = 1, cam_stride;
static const uint8_t bayer_place[4][4] = {      /* the colour of a cell's four places (0 R, 1 G, 2 B), row by row */
	{ 0, 1, 1, 2 }, { 1, 0, 2, 1 }, { 1, 2, 0, 1 }, { 2, 1, 1, 0 },
};

static void
meter(vm_offset_t raw, struct meter *m)
{
	const uint8_t *place = bayer_place[cam_bayer & 3];
	uint64_t sum[3] = { 0, 0, 0 };
	uint32_t n = 0, clip = 0;

	for (uint32_t y = FRAME_H / 4; y < FRAME_H * 3 / 4; y += 16) {
		const volatile uint32_t *top = (const volatile uint32_t *)(raw + (uint64_t)y * LINE_BYTES);
		const volatile uint32_t *bottom = (const volatile uint32_t *)(raw + (uint64_t)(y + 1) * LINE_BYTES);

		for (uint32_t x = FRAME_W / 4; x < FRAME_W * 3 / 4; x += 16) {
			uint32_t a = top[x / 2], b = bottom[x / 2];
			uint32_t v[4] = { a & 0x3ff, (a >> 16) & 0x3ff, b & 0x3ff, (b >> 16) & 0x3ff };

			for (uint32_t i = 0; i < 4; i++) {
				sum[place[i]] += v[i] > BLACK_LEVEL ? v[i] - BLACK_LEVEL : 0;
				clip += place[i] == 1 && v[i] >= AE_CLIP;
			}
			n++;
		}
	}
	m->cells = n;
	m->mean[0] = (uint32_t)(sum[0] / n);
	m->mean[1] = (uint32_t)(sum[1] / (2 * n));
	m->mean[2] = (uint32_t)(sum[2] / n);
	m->clipped = clip * 1000 / (2 * n);
}

static bool
exposure_put(uint32_t lines, uint32_t again, uint32_t dgain)
{
	bool ok;

	ok = sensor_put(0x0104, 0x01, 1) && sensor_put(0x0202, (uint16_t)lines, 2) && sensor_put(0x0204, (uint16_t)again, 2)
	    && sensor_put(0x020E, (uint16_t)dgain, 2);
	(void)sensor_put(0x0104, 0x00, 1);
	if (ok) {
		ae_lines = lines;
		ae_again = again;
		ae_dgain = dgain;
	}
	return ok;
}

/* the exposure in 1/256 of a line at 1x: lines x analog x digital */
static uint64_t
exposure_now(void)
{
	return (uint64_t)ae_lines * ae_again * ae_dgain / 32;
}

/* the exposure for an amount (lines x gain x 256): time first, then analog gain, then digital gain */
static void
exposure_split(uint64_t want, uint32_t *lines, uint32_t *again, uint32_t *dgain)
{
	uint32_t max_lines = MODE_LINES - MODE_MARGIN, l, a, d;
	uint64_t g;

	l = (uint32_t)(want / 256 > max_lines ? max_lines : want / 256);
	l = l < 2 ? 2 : l;
	g = want / l;                                   /* the gain left, 256 = 1x */
	a = (uint32_t)(g * 32 / 256);
	a = a < again_min ? again_min : (a > again_max ? again_max : a);
	d = (uint32_t)(g * 32 / a);
	d = d < 0x100 ? 0x100 : (d > dgain_max ? dgain_max : d);
	*lines = l;
	*again = a;
	*dgain = d;
}

/* one step of the meter's: true when the green is near the aim (or the exposure can go no further that way) */
static bool
ae_step(const struct meter *m, bool half)
{
	uint64_t now = exposure_now(), want;
	uint32_t g = m->mean[1] + 1, l, a, d;
	bool near = g * 100 >= AE_AIM * 85 && g * 100 <= AE_AIM * 118;

	if (m->clipped > 30 && g > AE_AIM / 2) {
		near = false;
		want = now / 2;                         /* highlights clipped: down, whatever the mean says */
	} else {
		want = now * AE_AIM / g;
	}
	if (near) {
		return true;
	}
	want = want > now * 8 ? now * 8 : (want < now / 8 ? now / 8 : want);
	if (half) {
		want = (want + now) / 2;
	}
	exposure_split(want, &l, &a, &d);
	if (l == ae_lines && a == ae_again && d == ae_dgain) {
		return true;                            /* at an end of the range */
	}
	(void)exposure_put(l, a, d);
	return false;
}

/* the gains' range as the sensor gives it (0x0084, 0x0086, 0x1086), sanity-checked */
static void
gains_read(void)
{
	uint32_t lo = sensor_get16(0x0084), hi = sensor_get16(0x0086), dhi = sensor_get16(0x1086);

	again_min = lo >= 0x20 && lo <= 0x40 ? lo : 0x20;
	again_max = hi > again_min && hi <= 0x400 ? hi : 0x200;
	dgain_max = dhi >= 0x100 && dhi <= 0x1000 ? dhi : 0x400;
	dgain_max = dgain_max > cam_dgain ? cam_dgain : dgain_max;
	say("exynos_camera: 5: the gains: analog %#x to %#x (0x0084, 0x0086: %#x, %#x), digital to %#x (0x1086: %#x, "
	    "capped at cam_s9_dgain %#x)\n", again_min, again_max, lo, hi, dgain_max, dhi, cam_dgain);
}

/* the mode's table, then the exposure; false if the sensor did not take them */
static bool
mode_load(void)
{
	uint32_t exp = cam_exp;

	exp = exp < 2 ? 2 : exp;
	exp = exp > MODE_LINES - MODE_MARGIN ? MODE_LINES - MODE_MARGIN : exp;
	say("exynos_camera: 5: the sensor's mode: setfile A's 1632 x 1224 at 30 frames a second, %u 16-bit writes; then "
	    "0x3092 = 0, exposure %u lines (0x0202), analog gain %#x (0x0204, 0x20 is 1x), digital 1x; auto-exposure %s\n",
	    (unsigned)MODE_ENTRIES, exp, cam_gain, cam_ae ? "on" : "off (cam_s9_ae=0)");
	delay_for_interval(3, NSEC_PER_MSEC);
	for (uint32_t i = 0; i < MODE_ENTRIES; i++) {
		if (!sensor_put(mode_1632x1224[i][0], mode_1632x1224[i][1], 2)) {
			return false;
		}
	}
	if (!sensor_put(0x3092, 0x0000, 2) || !sensor_put(0x6028, 0x2000, 2)) {
		return false;
	}
	gains_read();
	if (!exposure_put(exp, cam_gain, 0x100)) {
		return false;
	}
	say("exynos_camera: 5: read back: output %u x %u, frame %u lines of %u clocks, exposure %u, gain %#x, digital %#x\n",
	    sensor_get16(0x034C), sensor_get16(0x034E), sensor_get16(0x0340), sensor_get16(0x0342), sensor_get16(0x0202),
	    sensor_get16(0x0204), sensor_get16(0x020E));
	return true;
}

/* the link up and the ring given to the WDMA, then the sensor streaming; false if a step failed */
static bool
stream_start(void)
{
	uint8_t frames = 0xff;
	uint64_t until;

	phy_setup();
	say("exynos_camera: 5: CSIS1 reset and set up again; the ring filled; VC0's WDMA on, its eight frame pointers on "
	    "the ring's %u slots\n", RING_SLOTS);
	if (!link_setup()) {
		return false;
	}
	dma_common();
	buffer_fill();
	ring_written = 0;
	ring_checks = 0;
	wr(wdma, DMA_CTRL, rd(wdma, DMA_CTRL) & ~DMA_FRAMEPTR_MASK);
	for (uint32_t i = 0; i < 8; i++) {
		wr(wdma, DMA_ADDR(i), (uint32_t)(buffer_pa + (uint64_t)(i % RING_SLOTS) * SLOT_BYTES));
	}
	wr(wdma, VCMN + VCMN_INT_SRC, rd(wdma, VCMN + VCMN_INT_SRC));
	dma_on = true;
	wr(wdma, DMA_CTRL, rd(wdma, DMA_CTRL) | DMA_ON | DMA_UPDT_PTR_EN);
	say("exynos_camera: 5: the link on (UPDATE_SHADOW, D-PHY, clock lane, no Q-channel, CSI_EN)\n");
	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) | CMN_UPDATE_SHADOW);
	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) & ~CMN_PHY_SEL);
	wr(csis, CSIS_PHY_CMN_CTRL, rd(csis, CSIS_PHY_CMN_CTRL) | PHY_ENABLE_CLK);
	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) & ~CMN_QCHANNEL_EN);
	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) | CMN_CSI_EN);
	say("exynos_camera: 5: STREAM ON: 0x0104 = 0, 8 ms, 0x6028 = 0x4000, 0x0100 = 1, 0x0216 = 0\n");
	if (!sensor_put(0x0104, 0x00, 1)) {
		return false;
	}
	delay_for_interval(8, NSEC_PER_MSEC);
	streaming = true;
	if (!sensor_put(0x6028, 0x4000, 2) || !sensor_put(0x0100, 0x01, 1) || !sensor_put(0x0216, 0x00, 1)) {
		return false;
	}
	nanoseconds_to_absolutetime(300 * NSEC_PER_MSEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
		if (sensor_get(0x0005, &frames, 1) == 0 && frames != 0xff) {
			break;
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	say("exynos_camera: 5: the sensor's frame count %#x (0xff: not streaming); PHY_STATUS %#x\n", frames,
	    rd(csis, CSIS_PHY_STATUS));
	return true;
}

/*
 * The next frame end of VC0's WDMA, waited for (until `until'): the ring's slot it went to, or -1. Polled every
 * `poll_us' (0: 20 us spinning). The WDMA's active frame pointer (ACT_CTRL [4:2]) is read at the frame's end: a
 * frame start seen after it (the next frame's, its bit still set) means the pointer has moved on, and the frame done
 * is the one before. A frame start alone is taken as it comes. The first four frames after the ring was filled are
 * also checked against the fill (the last line of each slot): the slot that changed is the truth, and the pointer's
 * offset from it (slot_fix) is kept for the frames after.
 */
static uint32_t
slot_written(void)
{
	uint32_t mask = 0;

	for (uint32_t i = 0; i < RING_SLOTS; i++) {
		uint64_t w = *(const volatile uint64_t *)(buffer + (uint64_t)i * SLOT_BYTES + (uint64_t)FRAME_H * LINE_BYTES - 8);

		mask |= w != 0xA5A5A5A5A5A5A5A5ULL ? 1u << i : 0;
	}
	return mask;
}

static int
frame_wait(uint64_t until, uint32_t poll_us)
{
	while (mach_absolute_time() < until) {
		uint32_t src = rd(wdma, VCMN + VCMN_INT_SRC), ptr, src2, slot, fresh;

		if (src & DMA_ERROR) {
			wr(wdma, VCMN + VCMN_INT_SRC, DMA_ERROR);
			dma_errors++;
		}
		if ((src & (DMA_FRM_START0 | DMA_FRM_END0)) == DMA_FRM_START0) {
			wr(wdma, VCMN + VCMN_INT_SRC, DMA_FRM_START0);
			src = rd(wdma, VCMN + VCMN_INT_SRC);
		}
		if (src & DMA_FRM_END0) {
			ptr = (rd(wdma, DMA_ACT_CTRL) >> 2) & 7;
			src2 = rd(wdma, VCMN + VCMN_INT_SRC);
			if (src2 & DMA_FRM_START0) {
				ptr = (ptr + 7) & 7;
			}
			wr(wdma, VCMN + VCMN_INT_SRC, DMA_FRM_END0 | (src2 & DMA_FRM_START0));
			slot = (ptr + slot_fix) % RING_SLOTS;
			if (ring_checks < RING_SLOTS) {
				fresh = slot_written() & ~ring_written;
				ring_written |= fresh;
				ring_checks++;
				if (fresh != 0 && (fresh & (fresh - 1)) == 0) {
					uint32_t truth = (uint32_t)__builtin_ctz(fresh);

					if (truth != slot) {
						slot_fix = (slot_fix + truth + RING_SLOTS - slot) % RING_SLOTS;
					}
					say("exynos_camera: the ring: frame end %u, frame pointer %u (INT_SRC %#x then %#x): slot %u by the "
					    "pointer, slot %u by the fill (the pointer's offset now %u)\n", ring_checks, ptr, src, src2, slot,
					    truth, slot_fix);
					slot = truth;
				} else {
					say("exynos_camera: the ring: frame end %u, frame pointer %u (INT_SRC %#x then %#x): slot %u; the "
					    "fill shows slots %#x new\n", ring_checks, ptr, src, src2, slot, fresh);
				}
			}
			return (int)slot;
		}
		if (poll_us == 0) {
			spin_us(20);
		} else {
			delay_for_interval(poll_us, NSEC_PER_USEC);
		}
	}
	return -1;
}

/*
 * One frame of the sensor's: the mode, the ring, streaming; the auto-exposure over up to cam_s9_ae_frames frames
 * (measured on every third one); the frame where the green is at the aim (or the last) is the one taken: the WDMA
 * off, streaming off, `frame' the ring's slot that holds it.
 */
static bool
sensor_frame(void)
{
	uint32_t ends = 0, last, stride, measure_at = 2, limit = cam_ae ? cam_ae_frames : 2;
	uint8_t frames = 0xff;
	uint64_t until, start, per_frame;
	struct meter m = { { 0, 0, 0 }, 0, 0 };
	int slot = -1, taken = -1;

	frame_ends = 0;
	dma_errors = 0;
	if (mmu_translating()) {
		say("exynos_camera: 5: the SysMMU is on (MMU_CTRL %#x): no DMA\n", rd(mmu, MMU_CTRL));
		return false;
	}
	if (!buffer_get() || !mode_load() || !stream_start()) {
		return false;
	}
	start = mach_absolute_time();
	nanoseconds_to_absolutetime(200 * NSEC_PER_MSEC, &per_frame);
	until = start + per_frame * 3;          /* the first frame: the stream's start */
	while (ends < limit + 4) {
		slot = frame_wait(until, 0);
		if (slot < 0) {
			break;
		}
		ends++;
		until = mach_absolute_time() + per_frame;
		if (ends < measure_at) {
			continue;
		}
		meter(buffer + (uint64_t)slot * SLOT_BYTES, &m);
		if (ends <= 6 || ends >= limit) {
			say("exynos_camera: 5: frame %u (slot %d, CSIS1 FRM_CNT_CH0 %u): green %u, red %u, blue %u of %u (aim %u), "
			    "%u per mille clipped; exposure %u lines, gain %#x, digital %#x\n", ends, slot,
			    rd(csis, CSIS_FRM_CNT_CH0), m.mean[1], m.mean[0], m.mean[2], 1023 - BLACK_LEVEL, AE_AIM, m.clipped,
			    ae_lines, ae_again, ae_dgain);
		}
		if (!cam_ae || ends >= limit || ae_step(&m, false)) {
			taken = slot;
			break;
		}
		measure_at = ends + 3;
	}
	dma_stop();
	frame_ends = ends;
	say("exynos_camera: 5: %u frame end(s) in %llu ms, %u with the WDMA's FIFO error; the frame taken: %u (slot %d), "
	    "exposure %u lines (%u us), gain %#x, digital %#x; the WDMA off; streaming off\n", ends,
	    absolutetime_ms(mach_absolute_time() - start), dma_errors, ends, taken, ae_lines,
	    ae_lines * 263 / 10, ae_again, ae_dgain);
	stream_stop();
	nanoseconds_to_absolutetime(300 * NSEC_PER_MSEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
		if (sensor_get(0x0005, &frames, 1) == 0 && frames == 0xff) {
			break;
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	say("exynos_camera: 5: the sensor's frame count %#x after the stop\n", frames);
	dma_log("5");
	link_stop();
	if (!dma_quiet()) {
		say("exynos_camera: 5: the WDMA still reads active (ACT_CTRL %#x): no more DMA into the buffer this run\n",
		    rd(wdma, DMA_ACT_CTRL));
		buffer_busy = true;
	}
	if (taken < 0) {
		taken = slot;
	}
	if (taken < 0) {
		say("exynos_camera: 5: no frame end\n");
		return false;
	}
	shot_slot = (uint32_t)taken;
	frame = buffer + (uint64_t)shot_slot * SLOT_BYTES;
	last = buffer_report("5");
	if (last == 0) {
		say("exynos_camera: 5: nothing reached the buffer\n");
		return false;
	}
	stride = last / FRAME_H;
	patch_mean(stride < LINE_BYTES ? LINE_BYTES : stride);
	return ends > 0;
}

/* ---- the run ---- */

/* camshot (cam_s9=6): the USB console's request, taken by the camera's thread (hakios_exynos_camera_shot) */
static volatile uint32_t shot_ready;    /* the thread waits for a request */
static volatile uint32_t shot_asked;    /* a request is there */
static volatile uint32_t shots_closed;  /* the run is ending: no more */
static volatile uint32_t stream_live;   /* cam_s9=7: the stream's thread owns the camera ... */
static volatile uint32_t stream_done;   /* ... and has put it all down again */
static uint32_t shot_n, shot_pattern, n_camshots;
static bool shots_enabled;              /* cam_s9=6 */

/* On the way back to the loader (a panic's too): everything this run switched on, off again, the DMA first. */
void
hakios_exynos_camera_quiesce(void)
{
	shots_closed = 1;
	if (stream_live && !stream_done) {
		/* the stream's thread puts the camera down itself when it sees the end (it polls every millisecond) */
		for (uint32_t i = 0; i < 20000 && !__atomic_load_n(&stream_done, __ATOMIC_ACQUIRE); i++) {
			spin_us(10);
		}
		if (stream_done) {
			return;
		}
	}
	if (!powered && !domain_mine && !phy_mine && !dma_on && !pattern_on && !top_gate_mine) {
		return;
	}
	leaving = true;
	say("exynos_camera: at the run's end: the WDMA and the pattern stopped, the front camera powered down\n");
	dma_stop();
	if (streaming && peric0 != 0) {
		stream_stop();
	}
	if (powered && peric0 != 0 && peric1 != 0) {
		power_down();
	}
	isp_down();
}

/* step 1's blocks: the pins, CMU_TOP, the two buses (once a run) */
static bool
map_blocks(void)
{
	if (peric0 != 0) {
		return true;
	}
	say("exynos_camera: 1: map the pins' GPIO blocks, CMU_TOP and the two buses (USI10 of PERIC1 for the S2MPB02, "
	    "USI08_I2C for the sensor)\n");
	peric0 = ml_io_map(GPIO_PERIC0_PA, 0x1000);
	peric1 = ml_io_map(GPIO_PERIC1_PA, 0x1000);
	cmu_top = ml_io_map(CMU_TOP_PA, 0x4000);
	if (peric0 == 0 || peric1 == 0 || cmu_top == 0 || !hakios_hsi2c_map(&pmic_bus, say) ||
	    !hakios_hsi2c_map(&sensor_bus, say)) {
		say("exynos_camera: 1: a block could not be mapped\n");
		peric0 = 0;
		return false;
	}
	return true;
}

/* step 1's reads: the S2MPB02's ID and the four LDOs as found (what ldo_on() and ldo_off() go by) */
static bool
look_supplies(void)
{
	uint8_t id = 0;

	hakios_hsi2c_power(&pmic_bus);
	hakios_hsi2c_reset(&pmic_bus);
	if (pmic_get(PMIC_ID, &id) != 0) {
		say("exynos_camera: 1: the S2MPB02 does not answer at 0x%x (%x)\n", PMIC, pmic_bus.last_error);
		return false;
	}
	say("exynos_camera: 1: S2MPB02 ID %#x\n", id);
	if (pmic_get(B2CTRL1, &b2ctrl1_found) != 0 || pmic_get(B2CTRL2, &b2ctrl2_found) != 0) {
		say("exynos_camera: 1: BUCK2's registers cannot be read (%x)\n", pmic_bus.last_error);
		return false;
	}
	say("exynos_camera: 1: VDD_EXT_1P3_PB02 (BUCK2): B2CTRL1 %#x (%s), B2CTRL2 %#x (voltage code; Android's %#x, 1.3 V)\n",
	    b2ctrl1_found, (b2ctrl1_found & BUCK_ON) == BUCK_ON ? "on" : (b2ctrl1_found & BUCK_ON) ? "in another mode" : "off",
	    b2ctrl2_found, BUCK2_VSEL);
	for (uint32_t i = 0; i < NLDOS; i++) {
		if (pmic_get(ldos[i].reg, &found[i]) != 0) {
			say("exynos_camera: 1: %s cannot be read (%x)\n", ldos[i].name, pmic_bus.last_error);
			return false;
		}
		say("exynos_camera: 1: %s: %#x (%s, voltage code %#x, Android's %#x)\n", ldos[i].name, found[i],
		    (found[i] & LDO_ON) ? "on" : "off", found[i] & LDO_VSEL, ldos[i].vsel);
	}
	return true;
}

/* the sensor's bus as it stands: the HSI2C's registers, its USI and CMU_PERIC1 clocks, the pins, CIS_CLK1 */
static void
bus_look(const char *step)
{
	vm_offset_t r = sensor_bus.regs, c = sensor_bus.cmu;

	say("exynos_camera: %s: the sensor's HSI2C (0x108F0000): CTL %x FIFO_CTL %x INT_EN %x INT_STAT %x FIFO_STAT %x CONF %x "
	    "TIMEOUT %x TRANS_STATUS %x TIMING_FS1 %x FS2 %x FS3 %x USI_CON %x USI_OPT %x; %llu transfers, %llu errors, the "
	    "last %x (INT_STAT: 0x100 abort, 0x200 no ack, 0x400 no device, 0x800 time-out)\n", step, rd(r, 0x00),
	    rd(r, 0x04), rd(r, 0x20), rd(r, 0x24), rd(r, 0x30), rd(r, 0x40), rd(r, 0x48), rd(r, 0x50), rd(r, 0x60),
	    rd(r, 0x64), rd(r, 0x68), rd(r, 0xC4), rd(r, 0xC8), sensor_bus.transfers, sensor_bus.errors,
	    sensor_bus.last_error);
	say("exynos_camera: %s: its USI mode word (0x1081102C) %x (4: I2C); CMU_PERIC1: mux %x, divider %x, gates %x %x %x %x "
	    "(bit 21 on, 20 manual), Q-channel %x\n", step, *(volatile uint32_t *)sensor_bus.usi, rd(c, sensor_bus.mux),
	    rd(c, sensor_bus.div), rd(c, sensor_bus.gates[0]), rd(c, sensor_bus.gates[1]), rd(c, sensor_bus.gates[2]),
	    rd(c, sensor_bus.gates[3]), rd(c, sensor_bus.qch));
	say("exynos_camera: %s: pins (CON DAT PUD DRV): gpp5 (the bus, pins 2 and 3) %x %x %x %x; gpc1 (MCLK1, pin 5) %x %x %x "
	    "%x; gpg2 (the reset, pin 5) %x %x %x %x\n", step, rd(peric1 + GPP5, 0), rd(peric1 + GPP5, 4),
	    rd(peric1 + GPP5, 8), rd(peric1 + GPP5, 0xC), rd(peric1 + GPC1, 0), rd(peric1 + GPC1, 4), rd(peric1 + GPC1, 8),
	    rd(peric1 + GPC1, 0xC), rd(peric0 + GPG2, 0), rd(peric0 + GPG2, 4), rd(peric0 + GPG2, 8), rd(peric0 + GPG2, 0xC));
	say("exynos_camera: %s: CIS_CLK1 (CMU_TOP): mux %x (bit 0: 0 OSCCLK), divider %x ([4:0] + 1), gate %x (bit 21 on, 20 "
	    "manual), Q-channel %x: MCLK %s\n", step, rd(cmu_top, CIS1_MUX), rd(cmu_top, CIS1_DIV), rd(cmu_top, CIS1_GATE),
	    rd(cmu_top, CIS1_QCH), ((rd(cmu_top, CIS1_MUX) & 1) == 0 && (rd(cmu_top, CIS1_DIV) & 0x1f) == 0 &&
	    (rd(cmu_top, CIS1_GATE) & (GATE_MANUAL | GATE_ON)) == (GATE_MANUAL | GATE_ON)) ? "26 MHz, gated on" :
	    "NOT 26 MHz and on");
}

/*
 * The bus scanned, reads only: a one-byte read at each 7-bit address, the ones that acknowledge listed. Then only the
 * other addresses a 3H1 may sit at are asked for the model ID (two address bytes written, two read: never at 0x50-0x57,
 * where an EEPROM would take the second byte as data). true: the sensor found, sensor_addr set.
 */
static const uint8_t sensor_alternatives[] = { 0x10, 0x1A, 0x20, 0x2D, 0x36, 0x5A };

static bool
bus_scan(void)
{
	char list[3 * 112 + 1];
	uint8_t seen[0x78] = { 0 }, b, model[2];
	uint32_t n = 0, len = 0;

	for (uint8_t a = 0x08; a <= 0x77; a++) {
		if (hakios_hsi2c_message(&sensor_bus, a, true, &b, 1, true) == 0) {
			seen[a] = 1;
			n++;
			len += (uint32_t)snprintf(list + len, sizeof(list) - len, " %02x", a);
		}
	}
	say("exynos_camera: 2: the bus scanned (a one-byte read at 0x08-0x77, nothing written): %u address(es) answer%s\n",
	    n, n ? list : ": none (no device on the bus is powered, or the bus itself is wrong: the registers above)");
	for (uint32_t i = 0; i < sizeof(sensor_alternatives); i++) {
		uint8_t a = sensor_alternatives[i];

		if (!seen[a]) {
			continue;
		}
		sensor_addr = a;
		model[0] = model[1] = 0;
		if (sensor_get(0x0000, model, 2) == 0 && ((model[0] << 8) | model[1]) != 0 &&
		    ((model[0] << 8) | model[1]) != 0xffff) {
			say("exynos_camera: 2: at %#x: model ID %#06x: THE SENSOR IS AT %#x (not the device tree's 0x35); used for the "
			    "run\n", a, (model[0] << 8) | model[1], a);
			return true;
		}
		say("exynos_camera: 2: at %#x: no model ID (%02x %02x)\n", a, model[0], model[1]);
	}
	sensor_addr = sensor_addr_given;
	return false;
}

/* step 2: the front camera's power-up in Android's order, and its ID read; false (and powered down) if it does not answer */
static bool
sensor_up(void)
{
	uint8_t model[2] = { 0 }, revision = 0, frames = 0, mode = 0;
	uint32_t error;

	for (uint32_t i = 0; i < NLDOS; i++) {
		if (ldos[i].switched && (found[i] & LDO_ON) && (found[i] & LDO_VSEL) != ldos[i].vsel) {
			say("exynos_camera: 2: %s is on at another voltage: someone else has it; nothing is written\n", ldos[i].name);
			return false;
		}
	}
	if ((b2ctrl1_found & BUCK_ON) != 0 && ((b2ctrl1_found & BUCK_ON) != BUCK_ON || b2ctrl2_found != BUCK2_VSEL)) {
		say("exynos_camera: 2: BUCK2 is on in another mode or at another voltage (%#x, %#x): someone else has it; "
		    "nothing is written\n", b2ctrl1_found, b2ctrl2_found);
		return false;
	}
	say("exynos_camera: 2: the front camera's power-up (Android's order for star: BUCK2, VDDAF, VDDA, VDDIO, VDDD)\n");
	powered = true;
	clock_on();
	pin_out(peric0 + GPG2, RESET_PIN, false);
	if (!buck2_on()) {
		power_down();
		return false;
	}
	spin_us(200);
	if (!ldo_on(VDDAF) || !ldo_on(VDDA) || !ldo_on(VDDIO) || !ldo_on(VDDD)) {
		power_down();
		return false;
	}
	spin_us(1000);
	hakios_hsi2c_power(&sensor_bus);        /* "on_i2c": the pins to function 3 (and the bus's clocks) */
	hakios_hsi2c_reset(&sensor_bus);
	spin_us(10);
	pin_out(peric0 + GPG2, RESET_PIN, true);
	pin_function(peric1 + GPC1, MCLK_PIN, 2);
	delay_for_interval(9, NSEC_PER_MSEC);
	look_pins("2");

	error = sensor_get(0x0000, model, 2);
	if (error != 0) {
		say("exynos_camera: 2: the sensor does not answer at %#x (%x): 20 ms more, and again\n", sensor_addr, error);
		delay_for_interval(20, NSEC_PER_MSEC);
		error = sensor_get(0x0000, model, 2);
	}
	if (error != 0) {
		say("exynos_camera: 2: the sensor does not answer at %#x (%x)\n", sensor_addr, error);
		bus_look("2");
		if (!bus_scan()) {
			power_down();
			return false;
		}
		error = sensor_get(0x0000, model, 2);
		if (error != 0) {
			power_down();
			return false;
		}
	}
	(void)sensor_get(0x0002, &revision, 1);
	(void)sensor_get(0x0005, &frames, 1);
	(void)sensor_get(0x0100, &mode, 1);
	say("exynos_camera: 2: THE SENSOR ANSWERS: model ID %#06x, revision %#x, frame count %u, mode_select %#x "
	    "(0: standby)\n", (model[0] << 8) | model[1], revision, frames, mode);
	return true;
}

/* everything a run or a shot switched on, off again (the DMA first); the buffer is reused, never given back */
static void
all_down(void)
{
	dma_stop();
	stream_stop();
	if (powered) {
		power_down();
	}
	isp_down();
	if (buffer != 0 && (dma_on || pattern_on || !dma_quiet())) {
		say("exynos_camera: the WDMA was not seen to stop: no more DMA into %#llx this run\n", buffer_pa);
		buffer_busy = true;
	}
}

/*
 * ---- camshot: the frame taken as an RGB picture, for the USB console ----
 *
 * A bilinear demosaic of the whole frame, 1632 x 1224 (camshot N: every Nth pixel of it across and down): at a green
 * place the green as it is and the red and blue from the two neighbours on the line and the two above and below; at a
 * red (blue) place the green from its four neighbours and the blue (red) from its four diagonal ones. The pattern's
 * order is the sensor's GRBG (fimc-is-cis-3h1.c's cis->bayer_order, OTF_INPUT_ORDER_BAYER_GR_BG; cam_s9_bayer=0-3 for
 * RGGB, GRBG, GBRG, BGGR); the frame's edges mirrored. The black level, 64 of 1023, taken off; a grey-world white
 * balance from the meter's cells (red and blue to the green, each gain kept within 1/4x and 4x of the green's); the green brought to
 * the meter's aim if the exposure could not get it there (at most 4x more); a saturation of cam_s9_sat per cent about
 * the luminance (140 unless said: no colour matrix of the sensor's is in the GPL sources, whose setfile A is the
 * sensor's registers only; the ISP's tuning, setfile_3h1.bin, is a vendor binary and not read); then a gamma of about
 * 1/2.2 (a table of 4096). RAW10 as the WDMA writes it with PACK12 off: a pixel in 16 bits, little-endian, the value in
 * [9:0], 3264 bytes a line. The front camera's picture comes out as the sensor sees it (mirrored): no flip is made here.
 */
static const char *const bayer_name[4] = { "RGGB", "GRBG", "GBRG", "BGGR" };
static uint16_t rows[3][FRAME_W];               /* three lines of the frame, the black level off */
static int32_t row_of[3] = { -1, -1, -1 };
static uint8_t raw_line[LINE_BYTES];            /* a line copied out of the uncached mapping */
static uint8_t out_row[FRAME_W * 3];
static uint8_t gamma_lut[4096];
static uint32_t cam_sat = 140;

static const uint16_t *
row_get(int32_t y, uint32_t stride)
{
	uint32_t i;

	y = y < 0 ? 1 : (y >= FRAME_H ? FRAME_H - 2 : y);         /* mirrored: the same colours */
	i = (uint32_t)y % 3;
	if (row_of[i] != y) {
		memcpy(raw_line, (const void *)(frame + (uint64_t)y * stride), LINE_BYTES);
		for (uint32_t x = 0; x < FRAME_W; x++) {
			uint32_t v = ((uint32_t)raw_line[2 * x] | ((uint32_t)raw_line[2 * x + 1] << 8)) & 0x3ff;

			rows[i][x] = (uint16_t)(v > BLACK_LEVEL ? v - BLACK_LEVEL : 0);
		}
		row_of[i] = y;
	}
	return rows[i];
}

static uint32_t
isqrt(uint64_t v)
{
	uint64_t r = 0, bit = 1ULL << 62;

	while (bit > v) {
		bit >>= 2;
	}
	while (bit != 0) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return (uint32_t)r;
}

static uint32_t
icbrt(uint64_t v)
{
	uint32_t lo = 0, hi = 2097151;

	while (lo < hi) {
		uint32_t mid = (lo + hi + 1) / 2;

		if ((uint64_t)mid * mid * mid <= v) {
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}
	return lo;
}

/* the gamma: about t^(1/2.2) as 0.73 sqrt(t) + 0.27 cbrt(t), with sRGB's straight foot (12.92 t) near black */
static void
gamma_make(void)
{
	for (uint32_t i = 0; i < 4096; i++) {
		uint32_t s = isqrt((uint64_t)i * (1u << 20) / 4095), c = icbrt((uint64_t)i * (1u << 30) / 4095);
		uint32_t y = (73 * s + 27 * c) * 255 / (100 * 1024), foot = i * 1292 * 255 / (4095 * 100);

		y = foot < y ? foot : y;
		gamma_lut[i] = (uint8_t)(y > 255 ? 255 : y);
	}
}

/* the picture into RGB_OFF of MEM_PA's mapping, from `frame'; its width and height back */
static bool
picture(uint32_t n, uint32_t *wp, uint32_t *hp)
{
	const uint8_t *place = bayer_place[cam_bayer & 3];
	uint32_t stride = cam_stride, w = FRAME_W / n, h = FRAME_H / n, k[3];
	uint8_t *out = (uint8_t *)(buffer + RGB_OFF);
	struct meter m;
	uint64_t t0 = mach_absolute_time(), t1;

	if (stride == 0) {
		stride = LINE_BYTES;    /* the WDMA's RAW10 with PACK12 off: two bytes a pixel, 3264 a line */
	}
	if (stride < LINE_BYTES || (uint64_t)stride * FRAME_H > SLOT_BYTES) {
		say("usb: camshot failed: a stride of %u bytes does not hold %u pixels of RAW10 in 16 bits\n", stride, FRAME_W);
		return false;
	}
	if (frame_last != 0 && frame_last < (uint64_t)stride * (FRAME_H - 1)) {
		say("exynos_camera: camshot: the DMA wrote %u bytes, %u lines' worth at %u a line: the rest of the picture is "
		    "the buffer's fill (0xA5)\n", frame_last, frame_last / stride, stride);
	}
	gamma_make();
	meter(frame, &m);
	/*
	 * gains in 1/256: the green to the aim (1x to 4x more), red and blue to the green's mean, each kept within 1/4x
	 * and 4x of the green's (a warm lamp's red is well above the green in the raw frame: its gain goes below 1x)
	 */
	k[1] = AE_AIM * 256 / (m.mean[1] + 1);
	k[1] = k[1] < 256 ? 256 : (k[1] > 1024 ? 1024 : k[1]);
	for (uint32_t c = 0; c < 3; c += 2) {
		uint32_t g = (m.mean[1] + 1) * 256 / (m.mean[c] + 1);

		g = g < 64 ? 64 : (g > 1024 ? 1024 : g);
		k[c] = g * k[1] / 256;
	}
	row_of[0] = row_of[1] = row_of[2] = -1;
	for (uint32_t oy = 0; oy < h; oy++) {
		int32_t y = (int32_t)(oy * n);
		const uint16_t *up = row_get(y - 1, stride), *mid = row_get(y, stride), *dn = row_get(y + 1, stride);
		const uint8_t *pl = place + (y & 1) * 2, *pv = place + ((y + 1) & 1) * 2;
		uint8_t *o = out_row;

		for (uint32_t ox = 0; ox < w; ox++) {
			uint32_t x = ox * n, xl = x ? x - 1 : 1, xr = x + 1 < FRAME_W ? x + 1 : FRAME_W - 2, c = pl[x & 1];
			int32_t v[3], yl;

			if (c == 1) {
				v[1] = mid[x];
				v[pl[(x + 1) & 1]] = (mid[xl] + mid[xr]) / 2;
				v[pv[x & 1]] = (up[x] + dn[x]) / 2;
			} else {
				v[c] = mid[x];
				v[1] = (mid[xl] + mid[xr] + up[x] + dn[x]) / 4;
				v[2 - c] = (up[xl] + up[xr] + dn[xl] + dn[xr]) / 4;
			}
			for (uint32_t i = 0; i < 3; i++) {
				v[i] = (int32_t)(((uint32_t)v[i] * k[i]) >> 8);
			}
			yl = (77 * v[0] + 150 * v[1] + 29 * v[2]) >> 8;
			for (uint32_t i = 0; i < 3; i++) {
				int32_t s = yl + (v[i] - yl) * (int32_t)cam_sat / 100;

				s = s < 0 ? 0 : s * 4;
				*o++ = gamma_lut[s > 4095 ? 4095 : s];
			}
		}
		memcpy(out + (size_t)oy * w * 3, out_row, w * 3);
	}
	t1 = mach_absolute_time();
	say("exynos_camera: camshot: RAW10 %s at a stride of %u (%u bytes written); the meter's means less the black level "
	    "R %u G %u B %u of %u (aim %u, %u per mille clipped); gains R %u.%02u G %u.%02u B %u.%02u; saturation %u %%; "
	    "%u x %u RGB (N = %u: every Nth pixel of the demosaic), made in %llu ms\n", bayer_name[cam_bayer & 3], stride,
	    frame_last, m.mean[0], m.mean[1], m.mean[2], 1023 - BLACK_LEVEL, AE_AIM, m.clipped, k[0] / 256,
	    k[0] % 256 * 100 / 256, k[1] / 256, k[1] % 256 * 100 / 256, k[2] / 256, k[2] % 256 * 100 / 256, cam_sat, w, h,
	    n, absolutetime_ms(t1 - t0));
	*wp = w;
	*hp = h;
	return true;
}

extern int hakios_usb_picture(uintptr_t rgb, uint32_t width, uint32_t height, const char *what);   /* hakios.dwc3.c */

static void
shot_failed(const char *why)
{
	if (snap.taken) {
		say("usb: camshot failed: %s; CSIS1 INT_SRC0 %#x INT_SRC1 %#x FRM_CNT_CH0 %u PHY_STATUS %#x; WDMA CTRL %#x "
		    "ACT_CTRL %#x FCNTSEQ_STAT %#x INT_SRC %#x ERR_CODE %#x; %u frame end(s), %u bytes written\n", why, snap.src0,
		    snap.src1, snap.frm, snap.phy, snap.ctrl, snap.act, snap.fcnt, snap.isrc, snap.err, frame_ends, frame_last);
	} else {
		say("usb: camshot failed: %s (no CSIS1/WDMA status: the shot stopped before its DMA; the lines above say "
		    "where)\n", why);
	}
}

/* the picture made from `frame' and handed to the USB link */
static void
shot_send(uint32_t n, bool pattern)
{
	uint32_t w = 0, h = 0;

	if (!picture(n, &w, &h)) {
		return;
	}
	if (hakios_usb_picture(buffer + RGB_OFF, w, h, pattern ? "the test pattern" : "the front camera") != 0) {
		say("usb: camshot failed: the USB link is not up, or is sending another picture\n");
	}
}

/* one camshot: steps 2 to 5 again (or 3 and 4: the test pattern), everything off again, the picture to the USB link */
static void
shot_run(uint32_t n, bool pattern)
{
	const char *why = NULL;
	bool got;

	n_camshots++;
	snap.taken = false;
	frame_last = 0;
	frame_ends = 0;
	sensor_broken = false;
	say("exynos_camera: camshot %u: %s, then the debayered frame (N = %u) to the USB console\n", n_camshots,
	    pattern ? "one frame of CSIS1's test pattern (steps 3 and 4 again)" : "one frame of the sensor's (steps 2 to 5 "
	    "again)", n);
	if (pattern) {
		got = isp_up() && pattern_frame();
		why = got ? NULL : "no frame of the test pattern";
	} else if (!sensor_up()) {
		got = false;
		why = "the sensor did not come up (its supplies or its ID: the lines above)";
	} else if (!isp_up()) {
		got = false;
		why = "the ISPPRE domain or its blocks (step 3)";
	} else {
		if (mmu_translating()) {
			got = false;
			why = "the SysMMU is on";
		} else {
			phy_on();
			got = sensor_frame() && frame_last != 0;
			why = got ? NULL : (frame_last == 0 ? "no byte of the frame reached the buffer" : "no frame end");
		}
	}
	if (!snap.taken) {
		snap_take();
	}
	all_down();
	if (!got) {
		shot_failed(why);
		return;
	}
	shot_send(n, pattern);
}

const char *hakios_exynos_camera_shot(uint32_t n, int pattern);

/* asked by the USB console's "camshot [N] [tp]" (hakios.dwc3.c), from its thread call: never waits. NULL: taken */
const char *
hakios_exynos_camera_shot(uint32_t n, int pattern)
{
	uint32_t ready = 1;

	if (shots_closed) {
		return "the run is ending";
	}
	if (n == 0 || n > 16) {
		return "N is a number from 1 to 16 (camshot 1: the whole frame, 1632 x 1224; 2: every second pixel, 816 x 612)";
	}
	if (!shots_enabled) {
		return "not in this run: boot-arg cam_s9=6 (and a kernel with the S9's camera driver)";
	}
	if (!__atomic_compare_exchange_n(&shot_ready, &ready, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return "the camera is busy: the boot's camera run (steps 1 to 5) or the last camshot is not finished, or a step "
		       "failed for good (the log says)";
	}
	shot_n = n;
	shot_pattern = pattern ? 1 : 0;
	__atomic_store_n(&shot_asked, 1, __ATOMIC_RELEASE);
	return NULL;
}

/* cam_s9=6: after the boot's run, the thread stays for the console's camshots (one at a time) */
/*
 * ---- cam_s9=7: the stream, and /dev/hakios-camera ----
 *
 * The sensor streams for the rest of the run into the ring (four slots, the WDMA's frame pointers in turn); each
 * frame end is taken by the camera's thread, which polls the WDMA every millisecond (no interrupt is wired), gives
 * the frame its number (CSIS1's frame counter: frames the thread was too busy to see are counted as lost, not
 * renumbered), meters every third frame and steps the exposure half-way to the aim (cam_s9_ae=0: as asked). Every
 * ten seconds the log says the frame rate and how many were lost. camshot copies the newest frame out of the ring to
 * the still (the ring has three frames' grace, 100 ms, for the copy) and makes the picture from there.
 *
 * /dev/hakios-camera, as the POCO's driver makes it (bsd/sys/hakios.camera.h, the same structure, so that
 * libhakios.media's capture device and camera_test read either): read() waits (a second at the most) for the newest
 * frame not read yet and gives a struct hakios_camera_frame, then the frame as MIPI RAW10 (four pixels in five bytes,
 * 2040 a line: packed here from the WDMA's 16 bits a pixel, line by line), GRBG, black level 64; a buffer of the
 * header's size learns the size. write(): HAKIOS_CAMERA_EXPOSURE (a lines, b analog gain; the auto-exposure off while
 * they come, back on two seconds after the last, or at once with a = 0), HAKIOS_CAMERA_SENSOR (there is one camera here: taken, and the frames carry the
 * number asked for, so that a capture device that asks for its back camera gets these frames), HAKIOS_CAMERA_FOCUS
 * (no lens motor: taken, nothing done). EIO once the stream has failed.
 */
#define RAW10_LINE              (FRAME_W * 5 / 4)       /* 2040 */
#define LINE_NS                 26283                   /* 10608 clocks of 403.6 MHz */

static LCK_GRP_DECLARE(s9cam_grp, "hakios-exynos-camera");
static LCK_MTX_DECLARE(s9cam_lock, &s9cam_grp);       /* the ring's frames' numbers and the commands */
static LCK_MTX_DECLARE(s9read_lock, &s9cam_grp);      /* a reader's copy (the line buffers): never the thread's */
static struct {
	uint64_t sequence, time;
	uint32_t lines, again, dgain;
} slot_info[RING_SLOTS];
static uint64_t stream_newest;          /* the newest frame's number (0: none yet) */
static int32_t stream_newest_slot = -1;
static uint64_t stream_given;           /* the newest frame read */
static bool stream_running, stream_failed;
static uint32_t asked_sensor = 2;       /* the frames' "sensor": 2, the front camera, unless asked for another */
static uint32_t manual_lines, manual_again;
static bool manual, manual_pending;
static uint64_t manual_until;            /* the reader's exposure holds until then: two seconds after its last */
static uint32_t stream_readers;
static uint8_t pack_line[LINE_BYTES], packed[RAW10_LINE];

static int
s9dev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return stream_failed ? EIO : 0;
}

static int
s9dev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
s9dev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_camera_frame f;
	uint64_t since = mach_absolute_time(), second;
	int32_t slot;
	int error = 0;

	nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
	lck_mtx_lock(&s9cam_lock);
	while (!stream_failed && (stream_newest_slot < 0 || stream_newest <= stream_given)) {
		struct timespec ts = { 0, 100 * NSEC_PER_MSEC };

		if (mach_absolute_time() - since > second) {
			lck_mtx_unlock(&s9cam_lock);
			return EAGAIN;
		}
		error = msleep(&stream_newest, &s9cam_lock, PZERO | PCATCH, "hakios-camera", &ts);
		if (error != 0 && error != EWOULDBLOCK) {
			lck_mtx_unlock(&s9cam_lock);
			return error;
		}
	}
	if (stream_failed) {
		lck_mtx_unlock(&s9cam_lock);
		return EIO;
	}
	slot = stream_newest_slot;
	stream_given = stream_newest;
	bzero(&f, sizeof(f));
	memcpy(f.magic, HAKIOS_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = FRAME_W;
	f.height = FRAME_H;
	f.stride = RAW10_LINE;
	f.format = 1;
	f.bayer = cam_bayer & 3;
	f.black = BLACK_LEVEL;
	f.sequence = slot_info[slot].sequence;
	f.time = slot_info[slot].time;
	f.exposure_lines = slot_info[slot].lines;
	f.line_ns = LINE_NS;
	f.gain = slot_info[slot].again * slot_info[slot].dgain / 0x100;
	f.gain_unity = 0x20;
	f.gain_max = again_max;
	f.frame_lines = MODE_LINES;
	f.frame_lines_min = MODE_LINES;
	f.frame_lines_max = MODE_LINES;
	f.margin = MODE_MARGIN;
	f.sensor = asked_sensor;
	f.focus = HAKIOS_CAMERA_NO_FOCUS;
	stream_readers++;
	lck_mtx_unlock(&s9cam_lock);

	error = uiomove((caddr_t)&f, (int)MIN((user_ssize_t)sizeof(f), uio_resid(uio)), uio);
	/* the frame, packed a line at a time (one reader at a time: the line buffers are shared) */
	lck_mtx_lock(&s9read_lock);
	for (uint32_t y = 0; error == 0 && y < FRAME_H && uio_resid(uio) > 0; y++) {
		memcpy(pack_line, (const void *)(buffer + (uint64_t)slot * SLOT_BYTES + (uint64_t)y * LINE_BYTES), LINE_BYTES);
		for (uint32_t x = 0, o = 0; x < FRAME_W; x += 4, o += 5) {
			uint32_t v[4];

			for (uint32_t k = 0; k < 4; k++) {
				v[k] = ((uint32_t)pack_line[2 * (x + k)] | ((uint32_t)pack_line[2 * (x + k) + 1] << 8)) & 0x3ff;
				packed[o + k] = (uint8_t)(v[k] >> 2);
			}
			packed[o + 4] = (uint8_t)((v[0] & 3) | ((v[1] & 3) << 2) | ((v[2] & 3) << 4) | ((v[3] & 3) << 6));
		}
		error = uiomove((caddr_t)packed, (int)MIN((user_ssize_t)RAW10_LINE, uio_resid(uio)), uio);
	}
	lck_mtx_unlock(&s9read_lock);
	lck_mtx_lock(&s9cam_lock);
	stream_readers--;
	lck_mtx_unlock(&s9cam_lock);
	wakeup(&stream_readers);
	return error;
}

static int
s9dev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_camera_command c;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c) || uiomove((caddr_t)&c, sizeof(c), uio) != 0) {
		return EINVAL;
	}
	lck_mtx_lock(&s9cam_lock);
	switch (c.command) {
	case HAKIOS_CAMERA_EXPOSURE:
		nanoseconds_to_absolutetime(2 * NSEC_PER_SEC, &manual_until);
		manual_until += mach_absolute_time();
		manual = c.a != 0;
		manual_lines = c.a;
		manual_again = c.b;
		manual_pending = manual;
		break;
	case HAKIOS_CAMERA_SENSOR:
		if (c.a <= 3) {
			asked_sensor = c.a;
		}
		break;
	case HAKIOS_CAMERA_FOCUS:
	case HAKIOS_CAMERA_KEEP:
		break;
	default:
		lck_mtx_unlock(&s9cam_lock);
		return EINVAL;
	}
	lck_mtx_unlock(&s9cam_lock);
	return 0;
}

static const struct cdevsw s9dev_cdevsw = {
	.d_open = s9dev_open, .d_close = s9dev_close, .d_read = s9dev_read, .d_write = s9dev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static void
s9dev_make(void)
{
	int major = cdevsw_add(-1, &s9dev_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-camera") == NULL) {
		say("exynos_camera: 7: /dev/hakios-camera could not be made (%d)\n", major);
		return;
	}
	say("exynos_camera: 7: /dev/hakios-camera made (major %d): the frames as the POCO's driver gives them\n", major);
}

/* the stream, until the run ends (or it fails) */
static void
stream_loop(void)
{
	uint64_t start, until, wait, log_at, ten, frm_base, frm_last = 0, frm_now;
	uint64_t log_frames = 0, seen = 0, lost = 0, next_meter = 4;
	uint32_t timeouts = 0;
	struct meter m = { { 0, 0, 0 }, 0, 0 };
	int slot;

	say("exynos_camera: 7: THE STREAM: the front camera up, 1632 x 1224 RAW10 at 30 frames a second into the ring "
	    "(%u slots of 4 MB at %#llx), the auto-exposure %s\n", RING_SLOTS, MEM_PA, cam_ae ? "on" : "off");
	if (!sensor_up() || !isp_up() || mmu_translating()) {
		goto failed;
	}
	phy_on();
	if (!buffer_get() || !mode_load() || !stream_start()) {
		goto failed;
	}
	frm_base = rd(csis, CSIS_FRM_CNT_CH0);
	start = log_at = mach_absolute_time();
	nanoseconds_to_absolutetime(300 * NSEC_PER_MSEC, &wait);
	nanoseconds_to_absolutetime(10 * NSEC_PER_SEC, &ten);
	log_at += ten;
	lck_mtx_lock(&s9cam_lock);
	stream_running = true;
	lck_mtx_unlock(&s9cam_lock);
	while (!shots_closed) {
		until = mach_absolute_time() + wait;
		slot = frame_wait(until, 1000);
		if (slot < 0) {
			say("exynos_camera: 7: no frame end in 300 ms (CSIS1 INT_SRC0 %#x, FRM_CNT_CH0 %u; WDMA ACT_CTRL %#x)\n",
			    rd(csis, CSIS_INT_SRC0), rd(csis, CSIS_FRM_CNT_CH0), rd(wdma, DMA_ACT_CTRL));
			if (++timeouts >= 5) {
				break;
			}
			continue;
		}
		timeouts = 0;
		frm_now = (uint32_t)(rd(csis, CSIS_FRM_CNT_CH0) - (uint32_t)frm_base);
		frm_now = frm_now <= frm_last ? frm_last + 1 : frm_now;
		lost += frm_now - frm_last - 1;
		frm_last = frm_now;
		seen++;
		lck_mtx_lock(&s9cam_lock);
		slot_info[slot].sequence = frm_now;
		slot_info[slot].time = mach_absolute_time();
		slot_info[slot].lines = ae_lines;
		slot_info[slot].again = ae_again;
		slot_info[slot].dgain = ae_dgain;
		stream_newest = frm_now;
		stream_newest_slot = slot;
		if (manual_pending) {
			uint32_t l = manual_lines, a = manual_again;

			manual_pending = false;
			lck_mtx_unlock(&s9cam_lock);
			l = l < 2 ? 2 : (l > MODE_LINES - MODE_MARGIN ? MODE_LINES - MODE_MARGIN : l);
			a = a < again_min ? again_min : (a > again_max ? again_max : a);
			(void)exposure_put(l, a, 0x100);
		} else {
			lck_mtx_unlock(&s9cam_lock);
		}
		wakeup(&stream_newest);
		if (manual && mach_absolute_time() > manual_until) {
			manual = false;         /* the reader that asked has gone quiet: the kernel's own exposure again */
			say("exynos_camera: 7: no exposure asked for two seconds: the auto-exposure again\n");
		}
		if (cam_ae && !manual && frm_now >= next_meter) {
			meter(buffer + (uint64_t)slot * SLOT_BYTES, &m);
			next_meter = frm_now + (ae_step(&m, true) ? 6 : 3);
		}
		if (__atomic_load_n(&shot_asked, __ATOMIC_ACQUIRE)) {
			/* camshot: the newest frame out of the ring, then the picture from the still */
			memcpy((void *)(buffer + STILL_OFF), (const void *)(buffer + (uint64_t)slot * SLOT_BYTES), SLOT_BYTES);
			frame = buffer + STILL_OFF;
			frame_last = FRAME_H * LINE_BYTES;
			say("exynos_camera: camshot %u: frame %llu of the stream (slot %d; exposure %u lines, gain %#x, digital "
			    "%#x)\n", ++n_camshots, frm_now, slot, ae_lines, ae_again, ae_dgain);
			shot_send(shot_n, false);
			__atomic_store_n(&shot_asked, 0, __ATOMIC_RELEASE);
			__atomic_store_n(&shot_ready, 1, __ATOMIC_RELEASE);
		}
		if (mach_absolute_time() >= log_at) {
			uint64_t ms = absolutetime_ms(mach_absolute_time() - start), frames = frm_now - log_frames;

			meter(buffer + (uint64_t)slot * SLOT_BYTES, &m);

			say("exynos_camera: 7: the stream: frame %llu after %llu ms, %llu.%02llu frames a second over the last ten "
			    "seconds; %llu seen, %llu lost (the thread busy), %u with the WDMA's FIFO error; exposure %u lines, gain "
			    "%#x, digital %#x%s; green %u (aim %u)\n", frm_now, ms, frames / 10, frames * 10 % 100, seen, lost,
			    dma_errors, ae_lines, ae_again, ae_dgain, manual ? " (asked)" : "", m.mean[1], AE_AIM);
			log_frames = frm_now;
			log_at += ten;
		}
	}
	say("exynos_camera: 7: the stream ends: %llu frames, %llu seen, %llu lost, %u FIFO errors\n", frm_last, seen, lost,
	    dma_errors);
failed:
	all_down();
	lck_mtx_lock(&s9cam_lock);
	stream_running = false;
	stream_failed = true;
	while (stream_readers != 0) {
		struct timespec ts = { 0, 10 * NSEC_PER_MSEC };

		(void)msleep(&stream_readers, &s9cam_lock, PZERO, "hakios-camera", &ts);
	}
	lck_mtx_unlock(&s9cam_lock);
	wakeup(&stream_newest);
	__atomic_store_n(&stream_done, 1, __ATOMIC_RELEASE);
}

static void
shot_loop(void)
{
	if (broken) {
		say("exynos_camera: camshot: not taken in this run (a write to the S2MPB02 did not read back)\n");
		return;
	}
	say("exynos_camera: 6: CAMSHOT READY: the USB console's \"camshot [N]\" (tools/usb-shot.py --cam) powers the front "
	    "camera up, takes one frame and powers it down again; \"camshot N tp\": CSIS1's test pattern instead\n");
	__atomic_store_n(&shot_ready, 1, __ATOMIC_RELEASE);
	while (!shots_closed) {
		if (!__atomic_load_n(&shot_asked, __ATOMIC_ACQUIRE)) {
			delay_for_interval(20, NSEC_PER_MSEC);
			continue;
		}
		if (broken || buffer_busy) {
			say("usb: camshot failed: %s\n", broken ? "a write to the S2MPB02 did not read back earlier in this run" :
			    "the WDMA was not seen to stop earlier in this run");
		} else {
			shot_run(shot_n, shot_pattern != 0);
		}
		__atomic_store_n(&shot_asked, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&shot_ready, 1, __ATOMIC_RELEASE);
	}
}

static void
camera_thread(void *arg, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg, shots = steps >= 6, stream = steps >= 7;
	bool kept = false;

	steps = steps > 5 ? 5 : steps;
	if (!map_blocks()) {
		goto out;
	}
	if (stream) {
		look_pins("1");
		if (look_supplies()) {
			s9dev_make();
			__atomic_store_n(&shot_ready, 1, __ATOMIC_RELEASE);
			stream_live = 1;
			stream_loop();
			stream_live = 0;
		}
		goto out;
	}
	look_pins("1");
	say("exynos_camera: 1: CIS_CLK1: mux %x, divider %x, gate %x, Q-channel %x\n", rd(cmu_top, CIS1_MUX),
	    rd(cmu_top, CIS1_DIV), rd(cmu_top, CIS1_GATE), rd(cmu_top, CIS1_QCH));
	if (!look_supplies()) {
		goto out;
	}
	if (steps < 2) {
		goto out;
	}

	if (!sensor_up()) {
		goto down;
	}
	kept = steps >= 5;
	if (kept) {
		say("exynos_camera: 2: the sensor stays powered for step 5\n");
	} else {
		power_down();
	}
	if (steps < 3) {
		goto out;
	}

	if (!isp_up() || steps < 4) {
		goto down;
	}
	if (!pattern_frame() || steps < 5 || !kept) {
		goto down;
	}
	if (sensor_frame()) {
		say("exynos_camera: 5: A FRAME OF THE SENSOR'S IS IN MEMORY (the patch's mean tells a picture from black)\n");
	}
down:
	all_down();
out:
	if (shots) {
		shot_loop();
	}
	thread_terminate(current_thread());
}

void
hakios_exynos_camera_init(void)
{
	uint32_t steps = 0, ipclk = 400;
	thread_t thread;

	if (!PE_parse_boot_argn("cam_s9", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != 0x10000000) {
		return;                 /* not the Exynos 9810 */
	}
	PE_parse_boot_argn("cam_s9_ipclk", &ipclk, sizeof(ipclk));
	if (ipclk < 26 || ipclk > 400) {
		ipclk = 400;
	}
	pmic_bus.ipclk_mhz = sensor_bus.ipclk_mhz = ipclk;
	PE_parse_boot_argn("cam_s9_exp", &cam_exp, sizeof(cam_exp));
	PE_parse_boot_argn("cam_s9_gain", &cam_gain, sizeof(cam_gain));
	if (cam_gain < 0x20 || cam_gain > 0x200) {
		cam_gain = 0x40;
	}
	PE_parse_boot_argn("cam_s9_pack12", &cam_pack12, sizeof(cam_pack12));
	PE_parse_boot_argn("cam_s9_bayer", &cam_bayer, sizeof(cam_bayer));
	PE_parse_boot_argn("cam_s9_stride", &cam_stride, sizeof(cam_stride));
	PE_parse_boot_argn("cam_s9_ispdiv", &cam_ispdiv, sizeof(cam_ispdiv));
	if (cam_ispdiv > 15) {
		cam_ispdiv = 1;
	}
	PE_parse_boot_argn("cam_s9_probe", &cam_probe, sizeof(cam_probe));
	PE_parse_boot_argn("cam_s9_mmu_probe", &cam_mmu_probe, sizeof(cam_mmu_probe));
	{
		uint32_t addr = 0;

		if (PE_parse_boot_argn("cam_s9_addr", &addr, sizeof(addr)) && addr >= 0x08 && addr <= 0x77 &&
		    (addr < 0x50 || addr > 0x57)) {
			sensor_addr = sensor_addr_given = (uint8_t)addr;
		}
	}
	PE_parse_boot_argn("cam_s9_ae", &cam_ae, sizeof(cam_ae));
	PE_parse_boot_argn("cam_s9_ae_frames", &cam_ae_frames, sizeof(cam_ae_frames));
	if (cam_ae_frames < 2 || cam_ae_frames > 30) {
		cam_ae_frames = 15;
	}
	PE_parse_boot_argn("cam_s9_sat", &cam_sat, sizeof(cam_sat));
	if (cam_sat > 300) {
		cam_sat = 140;
	}
	PE_parse_boot_argn("cam_s9_dgain", &cam_dgain, sizeof(cam_dgain));
	if (cam_dgain < 0x100 || cam_dgain > 0x1000) {
		cam_dgain = 0x400;
	}
	if (steps > 7) {
		steps = 7;
	}
	shots_enabled = steps >= 6;
	if (kernel_thread_start(camera_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
