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
 *   the supplies   all four on the S2MPB02 (0x59 on hsi2c@10920000, USI10 of PERIC1: the flashlight's chip, see
 *                  hakios.s2mpb02_torch.c). An LDO's control register: bit 7 on, [6:0] the voltage, 600 mV + n steps
 *                  (12.5 mV for LDOs 1-5, 25 mV for 6-18):
 *                    VDDD_1.05V_VT   LDO5,  L5CTRL  0x22, 1.05 V = 36 (0x24)
 *                    VDDIO_1.8V_VT   LDO6,  L6CTRL  0x23, 1.8 V  = 48 (0x30)
 *                    VDDA_2.8V_VT    LDO13, L13CTRL 0x2A, 2.8 V  = 88 (0x58)
 *                    VDDAF_2.8V_VT   LDO16, L16CTRL 0x2D, 2.8 V  = 88 (0x58): the lens motor's, read and never switched here
 *   power-up       (the module file's, fimc-is-device-module-3h1.c, less the lens motor's supply): CIS_CLK1 on at 26
 *                  MHz; reset low; VDDA on; VDDIO on; VDDD at 1.05 V and on, 1 ms; the bus pins to I2C, 10 us; reset
 *                  high; MCLK out on its pin, 9 ms. Each LDO is given 1 ms (the regulator's enable time is 180 us).
 *   power-down     MCLK's pin to an output, low; reset low; the bus pins back to inputs; VDDD, VDDA, VDDIO off (the
 *                  module file's order); CIS_CLK1's gate, Q-channel, mux and divider as found.
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
 *                  MMU_STATUS 0x8, MMU_VERSION 0x34, read only. Off (its reset state after the domain's power-on), it
 *                  passes the DMA's addresses through: the buffer's physical address is the DMA's. Found on, there
 *                  is no frame: this driver gives it no table
 *   the buffer     4 MB in one piece below 4 GB (the WDMA's addresses are 32 bits), cleaned out of the processor's
 *                  cache before and invalidated after; filled with 0xA5 first, so that what the DMA wrote shows
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
 *              common and the SysMMU's state
 *   4  writes  the D-PHY's isolation off and its reset released, CSIS1 reset and set up for 1632 x 1224 RAW10, the
 *              buffer given to VC0's WDMA, the test pattern generator on for ONE frame (its frame end waited for, 2 s
 *              at the most), then the WDMA and the pattern off; the buffer's checksum, its first bytes, how much of it
 *              changed
 *   5  writes  the sensor's mode table, the D-PHY's settle for 719 Mb/s, the link on, streaming; two frame ends of
 *              VC0's WDMA waited for (1.5 s at the most), the WDMA off, streaming off; the counters, the link's errors,
 *              the checksum and the mean of a 64 x 64 patch in the middle (a black frame reads about 64 in 10 bits)
 * Whatever a run switched on is switched off again in the reverse order, also when a step fails, and at the run's end
 * (loader_return(), a panic's way out too): the WDMA and the pattern first, then the sensor, the link, the PHY, the
 * domain (if this run switched it on). The buffer is given back only once the WDMA is seen stopped.
 *
 * Boot-args:
 *   cam_s9=N           the front camera's first N steps: 1 a look, 2 the sensor's ID, 3 the ISPPRE domain and CSIS1,
 *                      4 one frame of the test pattern into memory, 5 one frame of the sensor's (0 or none: nothing)
 *   cam_s9_ipclk=MHZ   the two HSI2Cs' input clock in MHz (default 400, as the phone showed for the other buses on 1
 *                      November: a wrong guess on the high side only makes the bus slower)
 *   cam_s9_exp=LINES   the exposure of step 5's frame in lines of 26.3 us (default 1000, at most 1262)
 *   cam_s9_gain=CODE   the analog gain of step 5's frame, 0x20 being 1x (default 0x40, at most 0x200)
 *   cam_s9_pack12=1    the WDMA's PACK12 layout rather than RAW10 as it comes (the BSP sets PACK12 for its V4L2
 *                      SBGGR10 buffers; which of the two is MIPI's four pixels in five bytes is what step 4 shows)
 *
 * Limits, whatever asks. The S2MPB02 makes other supplies too (the flash LED, the rear camera's lens motor, the grip
 * sensor, the proximity LED): every write to it goes through one function that takes only the three LDO registers above,
 * only the values above (Android's voltages, on or off), and reads each back; a write that does not read back, or a byte
 * nobody acknowledges, and everything is powered down in the power-down's order and nothing more is done for the run.
 * An LDO found on is left as it is and left on at the end; one found on at another voltage stops the run before
 * anything is written (someone else has it). No other PMIC register is written. The flashlight's driver stays off in a
 * run with cam_s9=N (the same bus, one driver per bus). Nothing of the ISPPRE block is read before its domain is seen
 * on. The DMA is given only the buffer's address, checked to lie below 4 GB, and only with the SysMMU seen off. Every
 * wait is bounded by the clock.
 *
 * Not written yet (devices/starlte/CAMERA.md): /dev/hakios-camera's frames for libhakios.media's capture device, a ring
 * of buffers on the CSIS's frame-end interrupt (stage 4 of the plan).
 *
 * Sources: the phone's device tree (exynos9810-starlte_eur_open_26.dts of the BSP: hsi2c@108F0000 and fimc-is-3h1@35,
 * fimc-is_sensor_3h1@6A, s2mpb02_pmic@59 and its regulators, the fimc_is_mclk1 pin states, hsi2c21-bus, usi@1081102C,
 * pd-isppre@14064048 and pd-dpu, fimc_is_sensor@16240000, fimc_is@16290000, dphy_m0s4s2_csis1, sysmmu@16310000);
 * the S9 kernel's fimc-is-device-module-3h1.c, fimc-is-cis-3h1.c/.h, fimc-is-cis-3h1-setA.h, fimc-is-cis.c,
 * ischain/fimc-is-v6_0_0/setup-fimc-is-sensor.c, fimc-is-config.h and fimc-is-hw-chain.c/.h,
 * include/fimc-is-hw-settle-10nm-lpp.h, include/fimc-is-hw.h, fimc-is-device-csi_v4.c, sensor/csi2/fimc-is-hw-csi-v5_1.c
 * and .h, drivers/phy/phy-exynos8895-mipi.c, drivers/iommu/exynos-iommu.h, drivers/soc/samsung/exynos-pd.c,
 * cal-if/pmucal_local.c, cal-if/ra.c, cal-if/exynos9810/flexpmu_cal_local_exynos9810.h and
 * flexpmu_cal_system_exynos9810.h, cmucal-sfr.c, cmucal-node.c and cmucal-qch.c, drivers/clk/samsung/clk-exynos9810.c,
 * s2mpb02-regulator.c, s2mpb02-private.h, pinctrl-exynos.c (android_kernel_samsung_exynos9810, lineage-24.0, e5906667;
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
#include <sys/systm.h>
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

static const struct ldo {
	const char *name;
	uint8_t reg, vsel;
	bool switched;          /* by this driver (the lens motor's is only read) */
} ldos[] = {
	{ "VDDD_1.05V_VT (LDO5)", 0x22, 0x24, true },
	{ "VDDIO_1.8V_VT (LDO6)", 0x23, 0x30, true },
	{ "VDDA_2.8V_VT (LDO13)", 0x2A, 0x58, true },
	{ "VDDAF_2.8V_VT (LDO16)", 0x2D, 0x58, false },
};
#define VDDD            0
#define VDDIO           1
#define VDDA            2
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

#define SENSOR          0x35

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

/* The only way anything reaches the S2MPB02: one of the three LDOs, Android's voltage, on or off; read back. */
static bool
ldo_put(uint32_t i, bool on)
{
	uint8_t value, buf[2], back = 0;

	if (broken || i >= NLDOS || !ldos[i].switched) {
		return false;
	}
	value = (uint8_t)((on ? LDO_ON : 0) | ldos[i].vsel);
	buf[0] = ldos[i].reg;
	buf[1] = value;
	if (hakios_hsi2c_message(&pmic_bus, PMIC, false, buf, 2, true) != 0 || pmic_get(ldos[i].reg, &back) != 0 ||
	    back != value) {
		say("exynos-camera: S2MPB02 %#x (%s): wrote %#x, read back %#x (bus %#x): no more writing for this run\n",
		    ldos[i].reg, ldos[i].name, value, back, pmic_bus.last_error);
		broken = true;
		return false;
	}
	return true;
}

/* an LDO to Android's voltage and on, unless it was found on (then it is left as it is) */
static bool
ldo_on(uint32_t i)
{
	if (found[i] & LDO_ON) {
		say("exynos-camera: 2: %s was on already (%#x): left as it is\n", ldos[i].name, found[i]);
		return true;
	}
	if ((found[i] & LDO_VSEL) != ldos[i].vsel && !ldo_put(i, false)) {
		return false;
	}
	mine[i] = true;
	if (!ldo_put(i, true)) {
		return false;
	}
	say("exynos-camera: 2: %s on (%#x)\n", ldos[i].name, LDO_ON | ldos[i].vsel);
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
	say("exynos-camera: 2: CIS_CLK1: mux %x -> %x, divider %x -> %x, gate %x -> %x (26 MHz from OSCCLK)\n", mux_found,
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
	ldo_off(VDDD);
	ldo_off(VDDA);
	ldo_off(VDDIO);
	if (clock_mine) {
		wr(cmu_top, CIS1_GATE, gate_found);
		wr(cmu_top, CIS1_QCH, qch_found);
		wr(cmu_top, CIS1_DIV, div_found);
		wr(cmu_top, CIS1_MUX, mux_found);
		clock_mine = false;
	}
	powered = false;
	say("exynos-camera: powered down: MCLK and reset low, the bus pins inputs, LDOs %s; CIS_CLK1's gate %x again\n",
	    (mine[VDDD] || mine[VDDA] || mine[VDDIO]) ? "NOT ALL OFF (a write failed)" : "off as found", gate_found);
}

/* a register of the sensor: its 16-bit address written, a repeated start, len bytes read */
static uint32_t
sensor_get(uint16_t reg, uint8_t *buf, uint32_t len)
{
	uint8_t address[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
	uint32_t error = hakios_hsi2c_message(&sensor_bus, SENSOR, false, address, 2, false);

	return error ? error : hakios_hsi2c_message(&sensor_bus, SENSOR, true, buf, len, true);
}

static void
look_pins(const char *when)
{
	say("exynos-camera: %s: reset gpg2-5 CON %x DAT %u; MCLK1 gpc1-5 CON %x DAT %u; bus gpp5-2/3 CON %x %x\n", when,
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
/* the Q-channels asked on: the four CSISs' (as setup-fimc-is-sensor.c does), the PDP's DMA's and the SysMMU's */
static const uint32_t qchs[] = { 0x3020, QCH_CSIS1, 0x3028, 0x302C, 0x3038, QCH_SYSMMU };
#define NQCHS                   (sizeof(qchs) / sizeof(qchs[0]))
#define QCH_REQ                 (1u << 1)
#define TOP_ISPPRE_MUX          0x1094          /* CMU_TOP: MUX_CLKCMU_ISPPRE_BUS [1:0] */
#define TOP_ISPPRE_DIV          0x1890          /* DIV_CLKCMU_ISPPRE_BUS [3:0] */
#define TOP_ISPPRE_GATE         0x209C          /* GATE_CLKCMU_ISPPRE_BUS: bit 20 manual, 21 on */

#define SYSREG_ISPPRE_PA        0x16210000UL
#define DPHY_RESET              0x0500          /* the MIPI PHYs' resets: bit 2 CSIS1's M0S4S2 (1: running) */
#define DPHY_RESET_CSIS1        (1u << 2)

#define CSIS1_PA                0x16240000UL
#define CSIS_VERSION            0x000
#define CSIS_CMN_CTRL           0x004
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
#define BUF_BYTES               (4u << 20)
#define CANARY                  0xA5

static const uint64_t other_cmn_pa[3] = { 0x162B0400, 0x162D0400, 0x162E0400 };        /* CSIS0, 2, 3: read only */

static vm_offset_t pmu, cmu_isppre, sysreg_isppre, csis, dphy, wdma, cdma, mmu, other_cmn[3];
static bool domain_mine;                /* this run switched ISPPRE on */
static bool domain_up;                  /* ISPPRE is on and its blocks may be read */
static bool phy_mine, link_on, dma_on, pattern_on, streaming;
static uint32_t iso_found, dphy_reset_found, top_gate_found, cdma_found[4];
static bool top_gate_mine, cdma_mine, clocks_mine;
static uint32_t user_found, isppre_qch_found[NQCHS];
static vm_offset_t buffer;              /* the frame's, cached view */
static uint64_t buffer_pa;
static uint32_t cam_exp = 1000, cam_gain = 0x40, cam_pack12;

extern uint64_t kvtophys(vm_offset_t va);

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
	error = hakios_hsi2c_message(&sensor_bus, SENSOR, false, buf, 2 + bytes, true);
	if (error != 0) {
		say("exynos-camera: the sensor did not take %#06x = %#x (%x)\n", reg, value, error);
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
	say("exynos-camera: the sensor told to stop streaming (0x0100 = 0)\n");
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
		say("exynos-camera: the ISPPRE domain off again: MEMORY_ISPPRE_OPTION %#x -> %#x; SMC %#x (%u, %#x): the DTZPC "
		    "save, returned %#llx\n", was, rd(pmu, PMU_ISPPRE_MEMORY), DTZPC_SMC, DTZPC_SAVE, DTZPC_ISPPRE, answer);
		if ((uint32_t)answer != 0) {
			say("exynos-camera: the protection was not saved: the domain is left ON\n");
		} else {
			domain_up = false;
			wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) & ~PD_ON);
			if (!wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, 0, PD_WAIT_US, false)) {
				say("exynos-camera: the domain did not go off (status %#x): on again, its protection restored, left on\n",
				    rd(pmu, PMU_ISPPRE_STATUS));
				wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) | PD_ON);
				if (wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, PD_ON, PD_WAIT_US, false)) {
					answer = smc(DTZPC_SMC, DTZPC_RESTORE, DTZPC_ISPPRE, 0);
					say("exynos-camera: on again; the DTZPC restore returned %#llx\n", answer);
				} else {
					say("exynos-camera: NOT ON AGAIN EITHER: status %#x\n", rd(pmu, PMU_ISPPRE_STATUS));
				}
			}
		}
		domain_mine = false;
		say("exynos-camera: ISPPRE: configuration %#x, status %#x\n", rd(pmu, PMU_ISPPRE_CONFIG),
		    rd(pmu, PMU_ISPPRE_STATUS));
	}
	if (top_gate_mine) {
		wr(cmu_top, TOP_ISPPRE_GATE, top_gate_found);
		top_gate_mine = false;
	}
}

/* ---- step 3: the domain on, its clocks, what is there ---- */

static bool
isp_up(void)
{
	uint64_t answer;
	uint32_t status, user;

	say("exynos-camera: 3: map the PMU; read pd-dpu (the parent) and pd-isppre\n");
	pmu = ml_io_map(PMU_PA, PMU_SIZE);
	if (pmu == 0) {
		say("exynos-camera: 3: the PMU could not be mapped\n");
		return false;
	}
	iso_found = rd(pmu, PMU_DPHY_ISO_CSIS1);
	status = rd(pmu, PMU_ISPPRE_STATUS);
	say("exynos-camera: 3: DPU status %#x; ISPPRE configuration %#x, status %#x; MEMORY_ISPPRE_OPTION %#x; the D-PHY's "
	    "isolation (0x14060730) %#x\n", rd(pmu, PMU_DPU_STATUS), rd(pmu, PMU_ISPPRE_CONFIG), status,
	    rd(pmu, PMU_ISPPRE_MEMORY), iso_found);
	if ((rd(pmu, PMU_DPU_STATUS) & PD_ON) != PD_ON) {
		say("exynos-camera: 3: the parent domain (DPU) is off: ISPPRE is not switched on under it\n");
		return false;
	}
	if ((status & PD_ON) == PD_ON) {
		say("exynos-camera: 3: ISPPRE is on already (the boot left it): used as it is, no SMC, and left on\n");
	} else {
		say("exynos-camera: 3: ISPPRE on: configuration [3:0] = 0xF, status waited for (%u ms at the most)\n",
		    PD_WAIT_US / 1000);
		domain_mine = true;
		wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) | PD_ON);
		if (!wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, PD_ON, PD_WAIT_US, true)) {
			say("exynos-camera: 3: the domain did not come on (status %#x): its configuration back to off\n",
			    rd(pmu, PMU_ISPPRE_STATUS));
			wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) & ~PD_ON);
			(void)wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, 0, PD_WAIT_US, true);
			domain_mine = false;
			return false;
		}
		say("exynos-camera: 3: the domain is on (status %#x); about to make SMC %#x (%u, %#x, 0): the DTZPC restore; "
		    "Carbon, at EL2, passes it on to EL3\n", rd(pmu, PMU_ISPPRE_STATUS), DTZPC_SMC, DTZPC_RESTORE, DTZPC_ISPPRE);
		answer = smc(DTZPC_SMC, DTZPC_RESTORE, DTZPC_ISPPRE, 0);
		say("exynos-camera: 3: SMC %#x returned %#llx\n", DTZPC_SMC, answer);
		if ((uint32_t)answer != 0) {
			say("exynos-camera: 3: %s: nothing in the domain is touched; its power goes again (no save: nothing was "
			    "restored)\n", answer == (uint64_t)-1 ? "the call is not known to whoever answered it" : "a refusal");
			wr(pmu, PMU_ISPPRE_CONFIG, rd(pmu, PMU_ISPPRE_CONFIG) & ~PD_ON);
			(void)wait_for(pmu, PMU_ISPPRE_STATUS, PD_ON, 0, PD_WAIT_US, true);
			domain_mine = false;
			return false;
		}
	}
	domain_up = true;

	say("exynos-camera: 3: map CMU_ISPPRE, SYSREG_ISPPRE, CSIS1, its D-PHY, its WDMA, the DMA common and the SysMMU\n");
	cmu_isppre = ml_io_map(CMU_ISPPRE_PA, 0x4000);
	sysreg_isppre = ml_io_map(SYSREG_ISPPRE_PA, 0x1000);
	csis = ml_io_map(CSIS1_PA, 0x1000);
	dphy = ml_io_map(CSIS1_PHY_PA, 0x3000);
	wdma = ml_io_map(CSIS1_WDMA_PA, 0x1000);
	cdma = ml_io_map(CSIS_DMA_PA, 0x1000);
	mmu = ml_io_map(SYSMMU_PA, 0x1000);
	for (uint32_t i = 0; i < 3; i++) {
		other_cmn[i] = ml_io_map(other_cmn_pa[i] & ~0xfffULL, 0x1000);
	}
	if (cmu_isppre == 0 || sysreg_isppre == 0 || csis == 0 || dphy == 0 || wdma == 0 || cdma == 0 || mmu == 0 ||
	    other_cmn[0] == 0 || other_cmn[1] == 0 || other_cmn[2] == 0) {
		say("exynos-camera: 3: a block could not be mapped\n");
		return false;
	}

	top_gate_found = rd(cmu_top, TOP_ISPPRE_GATE);
	say("exynos-camera: 3: CMU_TOP's CLKCMU_ISPPRE_BUS: mux %#x, divider %#x, gate %#x; CMU_ISPPRE: BUS_USER %#x, BUSP "
	    "divider %#x, Q-channels CSIS1 %#x, SysMMU %#x\n", rd(cmu_top, TOP_ISPPRE_MUX), rd(cmu_top, TOP_ISPPRE_DIV),
	    top_gate_found, rd(cmu_isppre, ISPPRE_BUS_USER), rd(cmu_isppre, ISPPRE_BUSP_DIV), rd(cmu_isppre, QCH_CSIS1),
	    rd(cmu_isppre, QCH_SYSMMU));
	if ((top_gate_found & (GATE_MANUAL | GATE_ON)) != (GATE_MANUAL | GATE_ON)) {
		wr(cmu_top, TOP_ISPPRE_GATE, top_gate_found | GATE_MANUAL | GATE_ON);
		top_gate_mine = true;
	}
	user = user_found = rd(cmu_isppre, ISPPRE_BUS_USER);
	for (uint32_t i = 0; i < NQCHS; i++) {
		isppre_qch_found[i] = rd(cmu_isppre, qchs[i]);
	}
	clocks_mine = true;
	if ((user & (1u << 4)) == 0) {
		wr(cmu_isppre, ISPPRE_BUS_USER, user | (1u << 4));
		if (!wait_for(cmu_isppre, ISPPRE_BUS_USER, 1u << 7, 0, 1000, true)) {
			say("exynos-camera: 3: the ISPPRE bus's mux stays busy (%#x)\n", rd(cmu_isppre, ISPPRE_BUS_USER));
		}
	}
	for (uint32_t i = 0; i < NQCHS; i++) {
		wr(cmu_isppre, qchs[i], isppre_qch_found[i] | QCH_REQ);
	}
	say("exynos-camera: 3: clocks: CMU_TOP's gate %#x, BUS_USER %#x (bit 4: CMU_TOP's clock), Q-channels CSIS1 %#x, "
	    "SysMMU %#x (bit 1: requested)\n", rd(cmu_top, TOP_ISPPRE_GATE), rd(cmu_isppre, ISPPRE_BUS_USER),
	    rd(cmu_isppre, QCH_CSIS1), rd(cmu_isppre, QCH_SYSMMU));

	dphy_reset_found = rd(sysreg_isppre, DPHY_RESET);
	say("exynos-camera: 3: CSIS1: version %#x, CMN_CTRL %#x, PHY_STATUS %#x, PHY_CMN_CTRL %#x; the PHYs' resets "
	    "(0x16210500) %#x\n", rd(csis, CSIS_VERSION), rd(csis, CSIS_CMN_CTRL), rd(csis, CSIS_PHY_STATUS),
	    rd(csis, CSIS_PHY_CMN_CTRL), dphy_reset_found);
	say("exynos-camera: 3: VC0's WDMA: CTRL %#x, FMT %#x, ACT_CTRL %#x, ADDR1 %#x; its DMA common: INT_SRC %#x, ERR_CODE "
	    "%#x; the DMA common: ARB_PRI %#x, CTRL %#x, SRAM_SPLIT %#x, MATRIX %#x, TEST_PATTERN %#x\n",
	    rd(wdma, DMA_CTRL), rd(wdma, DMA_FMT), rd(wdma, DMA_ACT_CTRL), rd(wdma, DMA_ADDR(0)),
	    rd(wdma, VCMN + VCMN_INT_SRC),
	    rd(wdma, VCMN + VCMN_ERR_CODE), rd(cdma, CDMA_ARB_PRI), rd(cdma, CDMA_CTRL), rd(cdma, CDMA_SRAM_SPLIT),
	    rd(cdma, CDMA_MATRIX), rd(cdma, TP_ENABLE));
	say("exynos-camera: 3: the SysMMU: MMU_CTRL %#x (bit 0: translating), CFG %#x, STATUS %#x, VERSION %#x\n",
	    rd(mmu, MMU_CTRL), rd(mmu, MMU_CFG), rd(mmu, MMU_STATUS), rd(mmu, MMU_VERSION));
	if ((rd(csis, CSIS_VERSION) == 0 || rd(csis, CSIS_VERSION) == 0xffffffffu)) {
		say("exynos-camera: 3: CSIS1's version reads %#x: is its clock there?\n", rd(csis, CSIS_VERSION));
	}
	return true;
}

/* ---- step 4: the PHY, the link set up, the WDMA, the test pattern ---- */

static bool
buffer_get(void)
{
	if (buffer != 0) {
		return true;
	}
	if (kmem_alloc_contig(kernel_map, &buffer, BUF_BYTES, 0, (ppnum_t)atop_64(0x100000000ULL) - 1, 0,
	    KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("exynos-camera: 4: no %u bytes in one piece below 4 GB\n", BUF_BYTES);
		buffer = 0;
		return false;
	}
	buffer_pa = kvtophys(buffer);
	if (buffer_pa == 0 || buffer_pa + BUF_BYTES > 0x100000000ULL || kvtophys(buffer + BUF_BYTES - 1) !=
	    buffer_pa + BUF_BYTES - 1) {
		say("exynos-camera: 4: the buffer is at %#llx: not below 4 GB in one piece; not used\n", buffer_pa);
		kmem_free(kernel_map, buffer, BUF_BYTES);
		buffer = 0;
		return false;
	}
	say("exynos-camera: 4: the buffer: %u bytes at %#llx (physical)\n", BUF_BYTES, buffer_pa);
	return true;
}

static void
buffer_fill(void)
{
	memset((void *)buffer, CANARY, BUF_BYTES);
	cache_flush(buffer, BUF_BYTES);
}

/* What the DMA wrote: how much, how far, a checksum, the first bytes. Returns the bytes up to the last one changed. */
static uint32_t
buffer_report(const char *step)
{
	const uint8_t *b = (const uint8_t *)buffer;
	uint32_t changed = 0, last = 0, sum = 0, fnv = 2166136261u;

	cache_flush(buffer, BUF_BYTES);
	for (uint32_t i = 0; i < BUF_BYTES; i++) {
		if (b[i] != CANARY) {
			changed++;
			last = i + 1;
		}
	}
	for (uint32_t i = 0; i < last; i++) {
		sum += b[i];
		fnv = (fnv ^ b[i]) * 16777619u;
	}
	say("exynos-camera: %s: the buffer: %u bytes changed, the last at %u (%u a line over %u lines); sum %#x, FNV-1a "
	    "%#x\n", step, changed, last, last / FRAME_H, FRAME_H, sum, fnv);
	say("exynos-camera: %s: its first bytes: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x "
	    "%02x; at %u: %02x %02x %02x %02x %02x %02x %02x %02x\n", step, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
	    b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15], FRAME_W * 10 / 8, b[2040], b[2041], b[2042], b[2043],
	    b[2044], b[2045], b[2046], b[2047]);
	return last;
}

/* the D-PHY's isolation off and its reset released (exynos_mipi_phy_power_on) */
static void
phy_on(void)
{
	say("exynos-camera: 4: the D-PHY (M0S4S2, CSIS1): isolation bypass, PMU 0x730 %#x |= 1; reset bit 2 of 0x16210500 "
	    "(%#x) cleared, then set\n", iso_found, rd(sysreg_isppre, DPHY_RESET));
	phy_mine = true;
	wr(pmu, PMU_DPHY_ISO_CSIS1, rd(pmu, PMU_DPHY_ISO_CSIS1) | ISO_BYPASS);
	wr(sysreg_isppre, DPHY_RESET, rd(sysreg_isppre, DPHY_RESET) & ~DPHY_RESET_CSIS1);
	wr(sysreg_isppre, DPHY_RESET, rd(sysreg_isppre, DPHY_RESET) | DPHY_RESET_CSIS1);
	say("exynos-camera: 4: the D-PHY: isolation %#x, resets %#x\n", rd(pmu, PMU_DPHY_ISO_CSIS1),
	    rd(sysreg_isppre, DPHY_RESET));
}

/* CSIS1 reset and set up for the mode (csi_hw_reset, csi_hw_s_lane, _s_control, _s_config, _s_config_dma(_cmn)) */
static bool
link_setup(void)
{
	uint32_t v;

	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) | CMN_SW_RESET);
	if (!wait_for(csis, CSIS_CMN_CTRL, CMN_SW_RESET, 0, 100, false)) {
		say("exynos-camera: CSIS1's reset does not finish (CMN_CTRL %#x)\n", rd(csis, CSIS_CMN_CTRL));
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
	say("exynos-camera: CSIS1 set up: CMN_CTRL %#x, PHY_CMN_CTRL %#x, CH0 %#x %#x, CH2 %#x; WDMA FMT %#x, RESOL %#x; "
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

static void
dma_log(const char *step)
{
	say("exynos-camera: %s: WDMA CTRL %#x, ACT_CTRL %#x, FCNTSEQ_STAT %#x; its common: INT_SRC %#x, ERR_CODE %#x, CRC "
	    "%#x; the others' INT_SRC: CSIS0 %#x, CSIS2 %#x, CSIS3 %#x\n", step, rd(wdma, DMA_CTRL), rd(wdma, DMA_ACT_CTRL),
	    rd(wdma, DMA_FCNTSEQ_STAT), rd(wdma, VCMN + VCMN_INT_SRC), rd(wdma, VCMN + VCMN_ERR_CODE),
	    rd(wdma, VCMN + VCMN_CRC_RESULT), rd(other_cmn[0], 0x400 + VCMN_INT_SRC), rd(other_cmn[1], 0x400 + VCMN_INT_SRC),
	    rd(other_cmn[2], 0x400 + VCMN_INT_SRC));
	say("exynos-camera: %s: CSIS1 INT_SRC0 %#x (frame start [27:24], end [23:20], SOT [19:16], lost FS [15:12], lost FE "
	    "[11:8], overflow 4, wrong config 3, ECC 2, CRC 1, ID 0), INT_SRC1 %#x (V/H size mismatch [15:12]/[11:8]), "
	    "FRM_CNT_CH0 %u, PHY_STATUS %#x\n", step, rd(csis, CSIS_INT_SRC0), rd(csis, CSIS_INT_SRC1),
	    rd(csis, CSIS_FRM_CNT_CH0), rd(csis, CSIS_PHY_STATUS));
}

static bool
pattern_frame(void)
{
	uint32_t last;
	bool ended;

	if (rd(mmu, MMU_CTRL) & 1) {
		say("exynos-camera: 4: the SysMMU is on (MMU_CTRL %#x): the WDMA's addresses would be translated by a table "
		    "this driver did not make; no DMA\n", rd(mmu, MMU_CTRL));
		return false;
	}
	if (!buffer_get()) {
		return false;
	}
	buffer_fill();
	phy_on();
	say("exynos-camera: 4: CSIS1 reset and set up for %u x %u RAW10 on VC0 (%s), two lanes\n", FRAME_W, FRAME_H,
	    cam_pack12 ? "PACK12" : "RAW10 as it comes");
	if (!link_setup()) {
		return false;
	}
	dma_common();
	say("exynos-camera: 4: VC0's WDMA to %#llx, the output on; then the test pattern generator, %u x %u, two pixels a "
	    "clock, vertical blank %u clocks: ONE frame, its end waited for (2 s at the most)\n", buffer_pa, FRAME_W, FRAME_H,
	    TP_VBLANK);
	dma_start();
	wr(cdma, TP_CTRL, (rd(cdma, TP_CTRL) & ~0xffffffu) | (16u << 16) | (8u << 8) | 2u);
	wr(cdma, TP_RESOL, (FRAME_H << 16) | FRAME_W);
	wr(cdma, TP_ENABLE, TP_DUAL | TP_VBLANK);
	pattern_on = true;
	wr(cdma, TP_ENABLE, rd(cdma, TP_ENABLE) | TP_ON);
	ended = wait_for(wdma, VCMN + VCMN_INT_SRC, DMA_FRM_END0, DMA_FRM_END0, 2000000, true);
	dma_stop();
	delay_for_interval(50, NSEC_PER_MSEC);
	say("exynos-camera: 4: %s; the WDMA and the pattern off\n", ended ? "THE FRAME ENDED" : "no frame end in 2 s");
	dma_log("4");
	if (!dma_quiet()) {
		say("exynos-camera: 4: the WDMA still reads active (ACT_CTRL %#x): the buffer is kept, never given back\n",
		    rd(wdma, DMA_ACT_CTRL));
	}
	last = buffer_report("4");
	if (last == 0) {
		say("exynos-camera: 4: nothing reached the buffer\n");
		return false;
	}
	say("exynos-camera: 4: THE PATTERN REACHED MEMORY: %u bytes (%u x %u RAW10 packed would be %u)\n", last, FRAME_W,
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
	say("exynos-camera: 5: the D-PHY: SC_06 %#x, SC_02 %#x; lane 0: SD_08 %#x, SD_05 %#x, SD_06 %#x (settle), SD_13 %#x; "
	    "lane 1: %#x %#x %#x %#x\n", rd(dphy, DPHY_ACTRL_SC_06), rd(dphy, DPHY_DCTRL_SC_02), rd(dphy, DPHY_ACTRL_SD_08),
	    rd(dphy, DPHY_DCTRL_SD_05), rd(dphy, DPHY_DCTRL_SD_06), rd(dphy, DPHY_DCTRL_SD_13),
	    rd(dphy, DPHY_ACTRL_SD_08 + DPHY_LANE(1)), rd(dphy, DPHY_DCTRL_SD_05 + DPHY_LANE(1)),
	    rd(dphy, DPHY_DCTRL_SD_06 + DPHY_LANE(1)), rd(dphy, DPHY_DCTRL_SD_13 + DPHY_LANE(1)));
}

/* the mean of a 64 x 64 patch in the middle, read as MIPI RAW10 (four pixels in five bytes) at the stride given */
static void
patch_mean(uint32_t stride)
{
	const uint8_t *b = (const uint8_t *)buffer;
	uint64_t sum = 0, bytesum = 0;
	uint32_t lo = 1023, hi = 0, n = 0, nb = 0;

	if (stride < FRAME_W * 10 / 8 || (uint64_t)stride * FRAME_H > BUF_BYTES) {
		say("exynos-camera: 5: a stride of %u does not hold the frame: no patch\n", stride);
		return;
	}
	for (uint32_t y = FRAME_H / 2 - 32; y < FRAME_H / 2 + 32; y++) {
		const uint8_t *line = b + (uint64_t)y * stride;

		for (uint32_t x = FRAME_W / 2 - 32; x < FRAME_W / 2 + 32; x += 4) {
			const uint8_t *g = line + (x / 4) * 5;

			for (uint32_t k = 0; k < 4; k++) {
				uint32_t p = ((uint32_t)g[k] << 2) | ((g[4] >> (2 * k)) & 3);

				sum += p;
				lo = p < lo ? p : lo;
				hi = p > hi ? p : hi;
				n++;
			}
		}
		for (uint32_t i = stride / 4; i < stride * 3 / 4; i++) {
			bytesum += line[i];
			nb++;
		}
	}
	say("exynos-camera: 5: THE PATCH (64 x 64 in the middle, MIPI RAW10 at a stride of %u): mean %llu.%02llu of 1023 "
	    "(about 64 is black), %u to %u; the middle half of those lines' bytes: mean %llu\n", stride, sum / n,
	    (sum % n) * 100 / n, lo, hi, bytesum / nb);
}

static bool
sensor_frame(void)
{
	uint32_t ends = 0, src, last, exp, stride;
	uint8_t frames = 0xff;
	uint64_t until, start;

	exp = cam_exp;
	exp = exp < 2 ? 2 : exp;
	exp = exp > MODE_LINES - MODE_MARGIN ? MODE_LINES - MODE_MARGIN : exp;
	say("exynos-camera: 5: the sensor's mode: setfile A's 1632 x 1224 at 30 frames a second, %u 16-bit writes; then "
	    "0x3092 = 0, exposure %u lines (0x0202), analog gain %#x (0x0204, 0x20 is 1x)\n", (unsigned)MODE_ENTRIES, exp,
	    cam_gain);
	delay_for_interval(3, NSEC_PER_MSEC);
	for (uint32_t i = 0; i < MODE_ENTRIES; i++) {
		if (!sensor_put(mode_1632x1224[i][0], mode_1632x1224[i][1], 2)) {
			return false;
		}
	}
	if (!sensor_put(0x3092, 0x0000, 2) || !sensor_put(0x6028, 0x2000, 2) || !sensor_put(0x0202, (uint16_t)exp, 2) ||
	    !sensor_put(0x0204, (uint16_t)cam_gain, 2)) {
		return false;
	}
	say("exynos-camera: 5: read back: output %u x %u, frame %u lines of %u clocks, exposure %u, gain %#x\n",
	    sensor_get16(0x034C), sensor_get16(0x034E), sensor_get16(0x0340), sensor_get16(0x0342), sensor_get16(0x0202),
	    sensor_get16(0x0204));

	phy_setup();
	say("exynos-camera: 5: CSIS1 reset and set up again; the buffer refilled; VC0's WDMA on\n");
	if (!link_setup()) {
		return false;
	}
	dma_common();
	buffer_fill();
	dma_start();
	say("exynos-camera: 5: the link on (UPDATE_SHADOW, D-PHY, clock lane, no Q-channel, CSI_EN)\n");
	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) | CMN_UPDATE_SHADOW);
	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) & ~CMN_PHY_SEL);
	wr(csis, CSIS_PHY_CMN_CTRL, rd(csis, CSIS_PHY_CMN_CTRL) | PHY_ENABLE_CLK);
	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) & ~CMN_QCHANNEL_EN);
	wr(csis, CSIS_CMN_CTRL, rd(csis, CSIS_CMN_CTRL) | CMN_CSI_EN);
	say("exynos-camera: 5: CMN_CTRL %#x, PHY_CMN_CTRL %#x, PHY_STATUS %#x (the lanes' stop states before streaming)\n",
	    rd(csis, CSIS_CMN_CTRL), rd(csis, CSIS_PHY_CMN_CTRL), rd(csis, CSIS_PHY_STATUS));

	say("exynos-camera: 5: STREAM ON: 0x0104 = 0, 8 ms, 0x6028 = 0x4000, 0x0100 = 1, 0x0216 = 0\n");
	if (!sensor_put(0x0104, 0x00, 1)) {
		return false;
	}
	delay_for_interval(8, NSEC_PER_MSEC);
	streaming = true;
	if (!sensor_put(0x6028, 0x4000, 2) || !sensor_put(0x0100, 0x01, 1) || !sensor_put(0x0216, 0x00, 1)) {
		return false;
	}
	start = mach_absolute_time();
	nanoseconds_to_absolutetime(300 * NSEC_PER_MSEC, &until);
	until += start;
	while (mach_absolute_time() < until) {
		if (sensor_get(0x0005, &frames, 1) == 0 && frames != 0xff) {
			break;
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	say("exynos-camera: 5: the sensor's frame count %#x (0xff: not streaming); PHY_STATUS %#x\n", frames,
	    rd(csis, CSIS_PHY_STATUS));

	/* two frame ends of VC0's WDMA (the first may be the sensor's settling one): 1.5 s at the most */
	nanoseconds_to_absolutetime(1500 * NSEC_PER_MSEC, &until);
	until += mach_absolute_time();
	while (ends < 2 && mach_absolute_time() < until) {
		src = rd(wdma, VCMN + VCMN_INT_SRC);
		if (src & DMA_FRM_END0) {
			wr(wdma, VCMN + VCMN_INT_SRC, DMA_FRM_END0);
			ends++;
			say("exynos-camera: 5: frame end %u of the WDMA (INT_SRC %#x, CSIS1 FRM_CNT_CH0 %u)\n", ends, src,
			    rd(csis, CSIS_FRM_CNT_CH0));
		} else {
			delay_for_interval(1, NSEC_PER_MSEC);
		}
	}
	dma_stop();
	say("exynos-camera: 5: %u frame end(s); the WDMA off; streaming off\n", ends);
	stream_stop();
	nanoseconds_to_absolutetime(300 * NSEC_PER_MSEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
		if (sensor_get(0x0005, &frames, 1) == 0 && frames == 0xff) {
			break;
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	say("exynos-camera: 5: the sensor's frame count %#x after the stop\n", frames);
	dma_log("5");
	link_stop();
	if (!dma_quiet()) {
		say("exynos-camera: 5: the WDMA still reads active (ACT_CTRL %#x): the buffer is kept, never given back\n",
		    rd(wdma, DMA_ACT_CTRL));
	}
	last = buffer_report("5");
	if (last == 0) {
		say("exynos-camera: 5: nothing reached the buffer\n");
		return false;
	}
	stride = last / FRAME_H;
	patch_mean(stride);
	if (stride != ((FRAME_W * 10 / 8 + 15) & ~15u)) {
		patch_mean((FRAME_W * 10 / 8 + 15) & ~15u);
	}
	return ends > 0;
}

/* ---- the run ---- */

/* On the way back to the loader (a panic's too): everything this run switched on, off again, the DMA first. */
void
hakios_exynos_camera_quiesce(void)
{
	if (!powered && !domain_mine && !phy_mine && !dma_on && !pattern_on && !top_gate_mine) {
		return;
	}
	leaving = true;
	say("exynos-camera: at the run's end: the WDMA and the pattern stopped, the front camera powered down\n");
	dma_stop();
	if (streaming && peric0 != 0) {
		stream_stop();
	}
	if (powered && peric0 != 0 && peric1 != 0) {
		power_down();
	}
	isp_down();
}

static void
camera_thread(void *arg, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg;
	uint8_t id = 0, model[2] = { 0 }, revision = 0, frames = 0, mode = 0;
	uint32_t error;
	bool kept = false;

	say("exynos-camera: 1: map the pins' GPIO blocks, CMU_TOP and the two buses (USI10 of PERIC1 for the S2MPB02, "
	    "USI08_I2C for the sensor)\n");
	peric0 = ml_io_map(GPIO_PERIC0_PA, 0x1000);
	peric1 = ml_io_map(GPIO_PERIC1_PA, 0x1000);
	cmu_top = ml_io_map(CMU_TOP_PA, 0x4000);
	if (peric0 == 0 || peric1 == 0 || cmu_top == 0 || !hakios_hsi2c_map(&pmic_bus, say) ||
	    !hakios_hsi2c_map(&sensor_bus, say)) {
		say("exynos-camera: 1: a block could not be mapped\n");
		goto out;
	}
	look_pins("1");
	say("exynos-camera: 1: CIS_CLK1: mux %x, divider %x, gate %x, Q-channel %x\n", rd(cmu_top, CIS1_MUX),
	    rd(cmu_top, CIS1_DIV), rd(cmu_top, CIS1_GATE), rd(cmu_top, CIS1_QCH));
	hakios_hsi2c_power(&pmic_bus);
	hakios_hsi2c_reset(&pmic_bus);
	if (pmic_get(PMIC_ID, &id) != 0) {
		say("exynos-camera: 1: the S2MPB02 does not answer at 0x%x (%x)\n", PMIC, pmic_bus.last_error);
		goto out;
	}
	say("exynos-camera: 1: S2MPB02 ID %#x\n", id);
	for (uint32_t i = 0; i < NLDOS; i++) {
		if (pmic_get(ldos[i].reg, &found[i]) != 0) {
			say("exynos-camera: 1: %s cannot be read (%x)\n", ldos[i].name, pmic_bus.last_error);
			goto out;
		}
		say("exynos-camera: 1: %s: %#x (%s, voltage code %#x, Android's %#x)\n", ldos[i].name, found[i],
		    (found[i] & LDO_ON) ? "on" : "off", found[i] & LDO_VSEL, ldos[i].vsel);
	}
	if (steps < 2) {
		goto out;
	}

	for (uint32_t i = 0; i < NLDOS; i++) {
		if (ldos[i].switched && (found[i] & LDO_ON) && (found[i] & LDO_VSEL) != ldos[i].vsel) {
			say("exynos-camera: 2: %s is on at another voltage: someone else has it; nothing is written\n", ldos[i].name);
			goto out;
		}
	}
	say("exynos-camera: 2: the front camera's power-up (Android's order, less the lens motor's supply)\n");
	powered = true;
	clock_on();
	pin_out(peric0 + GPG2, RESET_PIN, false);
	if (!ldo_on(VDDA) || !ldo_on(VDDIO) || !ldo_on(VDDD)) {
		goto down;
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
		say("exynos-camera: 2: the sensor does not answer at 0x%x (%x)\n", SENSOR, error);
		goto down;
	}
	(void)sensor_get(0x0002, &revision, 1);
	(void)sensor_get(0x0005, &frames, 1);
	(void)sensor_get(0x0100, &mode, 1);
	say("exynos-camera: 2: THE SENSOR ANSWERS: model ID %#06x, revision %#x, frame count %u, mode_select %#x "
	    "(0: standby)\n", (model[0] << 8) | model[1], revision, frames, mode);
	kept = steps >= 5;
	if (kept) {
		say("exynos-camera: 2: the sensor stays powered for step 5\n");
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
		say("exynos-camera: 5: A FRAME OF THE SENSOR'S IS IN MEMORY (the patch's mean tells a picture from black)\n");
	}
down:
	dma_stop();
	stream_stop();
	if (powered) {
		power_down();
	}
	isp_down();
	if (buffer != 0 && !dma_on && !pattern_on && dma_quiet()) {
		kmem_free(kernel_map, buffer, BUF_BYTES);
		buffer = 0;
	} else if (buffer != 0) {
		say("exynos-camera: the buffer at %#llx is kept: the WDMA was not seen to stop\n", buffer_pa);
	}
out:
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
	if (steps > 5) {
		steps = 5;
	}
	if (kernel_thread_start(camera_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
