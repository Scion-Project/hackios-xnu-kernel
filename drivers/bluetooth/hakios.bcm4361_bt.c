/*
 * The Galaxy S9's Bluetooth, stage 1 (starlte, Exynos 9810): the Bluetooth half of the Broadcom BCM4361 on its UART, as
 * far as the chip answering HCI commands, taking Broadcom's patch (the .hcd file of the user's own phone) and saying
 * what it runs after it. No host stack: nothing here pairs, scans or carries a byte of anyone's data, and nothing is
 * handed to iOS (devices/starlte/BLUETOOTH.md says what stage 2 would need). Only with boot-arg bt=N on this phone (the
 * SoC's registers at 0x10000000); nothing is touched without it. The POCO's bt=N is drivers/bluetooth/hakios.bt.c, which
 * is not in the S9's kernel.
 *
 * What is where (Samsung's device tree for the S9, exynos9810-starlte_eur_open_26.dts of its kernel, and its drivers):
 *   UART_BT      0x10840000  uart@10840000 (dts 9480-9495): Samsung's "uart1", /dev/ttySAC1 to Android
 *                            (init.samsungexynos9810.rc:637), the port bcm43xx.c's low-power code names (BT_UPORT 1,
 *                            bcm43xx.c:46). Not a USI block: its clocks are PERIC1's UART_BT (below), but the serial
 *                            driver treats it as "usi-serial-v2" (USI_CON 0xC4, USI_OPTION 0xC8). A 256-byte FIFO.
 *                            Its interrupt (SPI 422) is not used: this polls.
 *   CMU_PERIC1   0x10800000  UART_BT's clocks (cmucal-sfr.c): gates GATE_CLK_PERIC1_UART_BT +0x2034 (1612),
 *                            RSTNSYNC_CLK_PERIC1_UART_BT +0x201C (1571), UART_BT_IPCLK +0x20C4 (1601), UART_BT_PCLK
 *                            +0x20C8 (1541) -- bit 20 manual, bit 21 on (6655-6656); its Q-channel +0x3028 (1641; bit 0
 *                            enable, bit 1 clock request, 6958-6959); its divider DIV_CLK_PERIC1_UART_BT +0x1814 (1577,
 *                            [3:0], 6762) after MUX_CLKCMU_PERIC1_IP_USER +0x0120 (1593, bit 4, 6809). Android leaves
 *                            the divider at 1 from a 400 MHz CLKCMU_PERIC1_IP (cmucal-vclklut.c:533-534, 1905-1907,
 *                            2074-2075): 200 MHz, the rate the baud divisor is worked out from (boot-arg bt_uart_mhz)
 *   the UART's pins  gpq0-0 RXD, -1 TXD, -2 RTS, -3 CTS of ALIVE's GPIO (0x14050000, bank at +0xA0, pinctrl-exynos.c:1934):
 *                            function 2, pull-up (pud 3), "uart1-bus" and "uart1-bus-rxd" (dts 268-282). gpq0-4 and -5
 *                            are the PMIC's SPEEDY bus: nothing here writes their bits
 *   BT_EN        pin 7 (port 0, bit 7) of the PCAL6524 GPIO expander at 0x22 on hsi2c@10930000 (the bluetooth node's first
 *                            gpio, <76 7 0>, dts 13866-13872; pcal6524@22, dts 6767-6786; bcm43xx.c:272 takes it as bt_en).
 *                            This is BT_REG_ON. It is NOT shared with Wi-Fi: WLAN_REG_ON is gpg0-5 of the SoC (bcmdhd's
 *                            dhd_custom_exynos.c:222-236, "cfg_wlanen", dts 2670), driven by drivers/wifi/hakios.exynos_pcie.c
 *                            (wifi_pcie=2); the BCM4361 has the two lines apart, and bcmdhd's one BT/WLAN REG_ON workaround,
 *                            si_pmu_wlan_bt_reg_on_war() (hndpmu.c:680-710), is for the 4389 alone (PMU rev 40)
 *   the expander's registers  INPUT 0x00, DAT_OUT 0x04, CONFIG 0x0C (+ port; a CONFIG bit 1 is an input), one byte each
 *                            (include/linux/i2c/pcal6524.h:8-11); an output is set by its DAT_OUT bit first, then its
 *                            CONFIG bit cleared (gpio-pcal6524.c, pcal6524_gpio_direction_output()). The same port holds
 *                            the DisplayPort switch (pins 1, 4) and the fingerprint sensor's sleep pin (6); port 1 the
 *                            NFC (9, 11) and wireless charging (8): every write here is a read-modify-write of bit 7 of
 *                            port 0's two registers, read back
 *   the expander's bus  hsi2c@10930000 (dts 6747-6766): USI10_I2C of PERIC1, clocks GATE_USI10_I2C and PERIC1_USI_I2C
 *                            (dt-bindings exynos9810.h:122, 144; clk-exynos9810.c:348, 456): gates GATE_CLK_PERIC1_USI_I2C
 *                            +0x2050 (1613), RSTNSYNC_USI10_I2C +0x20A8 (1592), USI10_I2C_IPCLK +0x210C (1610),
 *                            USI10_I2C_PCLK +0x2110 (1564), Q-channel +0x304C (1650), divider +0x1830 (1578); its input
 *                            clock 200 MHz (the node's default-clk). Pins gpp6-2 and -3 of PERIC1's GPIO, function 3
 *                            ("hsi2c25-bus", dts 2969-2975; gpp6 at +0x40, pinctrl-exynos.c:2001). USI10's mode word
 *                            (SYSREG_PERIC1 0x1081103C, usi@1081103C, dts 5612) is I2C (4, usi_v2.c:18-36) under Android,
 *                            for USI10's own I2C (the flashlight's bus, hakios.s2mpb02_torch.c), which leaves the block's
 *                            pins 2 and 3 to USI10_I2C; the word USI10_I2C would have, 0x10811040, is disabled in the tree
 *                            and not written. The bus runs at 100 kHz.
 *   the expander's reset  gpg3-6 of PERIC1's GPIO (0x10830000, bank at +0xC0, pinctrl-exynos.c:2005): output, high to run
 *                            ("expander_reset_active", dts 3405-3412; Android pulses it at probe, gpio-pcal6524.c:418-445)
 *   BT_WAKE      gpg2-0 of PERIC0's GPIO (0x10430000, +0xC0, pinctrl-exynos.c:1994): output, the host's "stay awake" to
 *                            the chip ("bt-btwake", dts 2691-2699), set low at probe and high whenever the UART sends
 *                            (bcm43xx.c:318, 131)
 *   BT_HOST_WAKE gpa2-3 of ALIVE's GPIO (+0x60, pinctrl-exynos.c:1932): the chip's to the host, read only here
 *                            ("bt-hostwake", dts 531-537)
 *   UART registers (include/linux/serial_s3c.h:32-49, 111, 170, 214-267): ULCON 0x00, UCON 0x04 (bit 5 loopback, [3:2] and
 *                            [1:0] the TX and RX modes, 1 = by the CPU), UFCON 0x08 (bit 0 FIFO on, 1 and 2 reset RX
 *                            and TX), UMCON 0x0C (bit 4 auto flow control), UTRSTAT 0x10, UERSTAT 0x14, UFSTAT 0x18 (RX
 *                            count [7:0], RX full bit 8, TX count [23:16], TX full bit 24), UMSTAT 0x1C (bit 0 CTS),
 *                            UTXH 0x20, URXH 0x24, UBRDIV 0x28, UFRACVAL 0x2C, UINTP 0x30, UINTSP 0x34, UINTM 0x38. USI_CON
 *                            is written 1 then 0, USI_OPTION 2 (CLKREQ on), all interrupts masked and cleared
 *                            (samsung.c:1555-1571, 1730-1737). The divisor: hakios.bcm_hci.h, as samsung.c works it out.
 *
 * What the chip is told, as Linux's Broadcom drivers tell it (btbcm.c, hci_bcm.c of the S9's kernel): it starts at 115200
 * baud, 8N1, with RTS/CTS (hci_bcm.c:817 init_speed; samsung.c:1268-1274); HCI_Reset (0x0C03, btbcm.c:173) and Read Local
 * Version (0x1001, 209); the patch: Download Minidriver (0xFC2E), 50 ms, the .hcd's records sent one by one as commands,
 * each answered before the next, then 250 ms after the last, Launch_RAM (btbcm.c:106-167); then HCI_Reset and Read Local
 * Version again. The baud rate is never raised (0xFC18, hci_bcm.c:117): 108 KB at 115200 takes about ten seconds, and one
 * less thing can go wrong. BT_EN high and 100 ms before the first byte (bcm43xx.c:96-99); low to power it off (112).
 *
 *   bt=1   a look, reading only: UART_BT's clocks and pins, BT_WAKE, BT_HOST_WAKE, the expander's reset pin and bus as
 *          found; UART_BT's registers if its clocks are open (a block whose clock is off can stop the bus)
 *   bt=2   ... the expander's bus (its clocks on, USI10 to I2C, its two pins), and the expander read: its three ports' input,
 *          output and direction, BT_EN's among them. With the expander held in reset (gpg3-6 low) it stops there, unless
 *          bt_expander_reset=1, which lets it go as Android's pinctrl does (gpg3-6 output high)
 *   bt=3   ... UART_BT set up (clocks, pins, 115200 8N1 from its clock) and tried in its internal loopback: 16 bytes out
 *          and back with no chip involved; then flow control on
 *   bt=4   ... the chip powered: BT_WAKE high, BT_EN output low (Android's probe), 10 ms, BT_EN high, 100 ms; each expander
 *          write read back. What the chip says on its own (nothing is expected), BT_HOST_WAKE and CTS logged
 *   bt=5   ... HCI over H4: HCI_Reset, Read Local Version (Broadcom is manufacturer 0x000F), Read Local Name: the chip's ROM
 *   bt=6   ... the patch: fw/<bt_hcd> of the small image (default bcm4361B2.hcd: fw-bt/ of the build, the phone's own
 *          /vendor/firmware/bcm4361B2_murata.hcd or _semco.hcd, whichever its module is, devices/starlte/BLUETOOTH.md),
 *          read through and checked first (hakios.bcm_hci.h), then sent; HCI_Reset, Read Local Version and Name again:
 *          the LMP subversion and the name now the patch's
 *   bt_keep=1        leave the chip powered at the end (default: BT_EN and BT_WAKE low once the steps are done or one fails)
 *   bt_hcd=<name>    the patch file's name in fw/ (default bcm4361B2.hcd)
 *   bt_expander_reset=1  at bt=2 and up, take the expander out of reset if it is held there
 *   bt_uart_mhz=<n>  UART_BT's clock in MHz, if bt=1 shows the divider or mux not as Android leaves them (default 200)
 * Each step says what it is about to do before it does it.
 *
 * What was checked: compiled, with the driver on and off; every register, bit and number above against the BSP files
 * named with them (the PR has the table); the H4 framing, the events, the .hcd reading and the divisor on a computer by
 * tools/bcm_hci_test.c (tools/bcm-hci-test.sh). It has not run on the phone, nor in QEMU (which has no such UART or
 * expander: bt=N must not be given there).
 *
 * Sources (Samsung's GPL kernel for the S9, Scion-Project/android_kernel_samsung_exynos9810 at e5906667068e, read for
 * these facts, not copied): drivers/bluetooth/bcm43xx.c, btbcm.c, hci_bcm.c, include/net/bluetooth/hci.h,
 * drivers/tty/serial/samsung.c, include/linux/serial_s3c.h, drivers/gpio/gpio-pcal6524.c, include/linux/i2c/pcal6524.h,
 * drivers/pinctrl/samsung/pinctrl-exynos.c, drivers/soc/samsung/usi_v2.c, drivers/soc/samsung/cal-if/exynos9810/
 * (cmucal-sfr.c, cmucal-vclklut.c, cmucal-node.c), drivers/clk/samsung/clk-exynos9810.c, include/dt-bindings/clock/
 * exynos9810.h, drivers/net/wireless/broadcom/bcmdhd_101_12/dhd_custom_exynos.c and hndpmu.c,
 * arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts; LineageOS's device_samsung_exynos9810-common
 * (init.samsungexynos9810.rc, proprietary-files.txt: the two .hcd files).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

#include "hakios.bcm_hci.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern void delay(int usec);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

/* ---- where ---------------------------------------------------------------------------------------------------- */

#define UART_PA                 0x10840000UL
#define CMU_PERIC1_PA           0x10800000UL
#define CMU_PERIC1_SIZE         0x4000UL
#define GPIO_ALIVE_PA           0x14050000UL
#define GPIO_PERIC0_PA          0x10430000UL
#define GPIO_PERIC1_PA          0x10830000UL

/* UART_BT's clocks in CMU_PERIC1 */
#define GATE_MANUAL             (1u << 20)
#define GATE_ON                 (1u << 21)
static const struct { uint16_t off; const char *name; } uart_gates[] = {
	{ 0x2034, "GATE_CLK_UART_BT" }, { 0x201C, "RSTNSYNC" }, { 0x20C4, "IPCLK" }, { 0x20C8, "PCLK" },
};
#define UART_QCH                0x3028
#define UART_DIV                0x1814
#define PERIC1_IP_USER_MUX      0x0120  /* bit 4: 1 = CLKCMU_PERIC1_IP from CMU_TOP, 0 = the oscillator */

/* the UART */
#define ULCON                   0x00
#define UCON                    0x04
#define  UCON_LOOPBACK          (1u << 5)
#define  UCON_CPU_MODES         0x5u    /* TX [3:2] and RX [1:0] by the CPU (interrupt or polling) */
#define UFCON                   0x08
#define  UFCON_FIFO             (1u << 0)
#define  UFCON_RESET_BOTH       (3u << 1)
#define UMCON                   0x0C
#define  UMCON_AFC              (1u << 4)
#define UTRSTAT                 0x10
#define UERSTAT                 0x14
#define UFSTAT                  0x18
#define  UFSTAT_RX_COUNT        0xffu
#define  UFSTAT_RX_FULL         (1u << 8)
#define  UFSTAT_TX_FULL         (1u << 24)
#define  UFSTAT_TX_COUNT(v)     (((v) >> 16) & 0xffu)
#define UMSTAT                  0x1C
#define  UMSTAT_CTS             (1u << 0)
#define UTXH                    0x20
#define URXH                    0x24
#define UBRDIV                  0x28
#define UFRACVAL                0x2C
#define UINTP                   0x30
#define UINTSP                  0x34
#define UINTM                   0x38
#define USI_CON                 0xC4
#define USI_OPTION              0xC8
#define  USI_CLKREQ_ON          (1u << 1)
#define ULCON_8N1               0x3u
#define BAUD                    115200

/* pins: banks of 4-bit CON (+0), 1-bit DAT (+4), 4-bit PUD (+8) */
#define BANK_GPQ0               0x0A0   /* ALIVE */
#define BANK_GPA2               0x060   /* ALIVE */
#define BANK_GPG2               0x0C0   /* PERIC0 */
#define BANK_GPG3               0x0C0   /* PERIC1 */
#define BANK_GPP6               0x040   /* PERIC1 */
#define PIN_BT_WAKE             0       /* gpg2 */
#define PIN_HOST_WAKE           3       /* gpa2 */
#define PIN_EXP_RESET           6       /* gpg3 */
#define UART_PINS_MASK          0xffffu /* gpq0-0..3; -4 and -5 are SPEEDY's */
#define UART_PINS_CON           0x2222u
#define UART_PINS_PUD           0x3333u

/* the expander */
#define EXPANDER                0x22
#define PCAL_INPUT              0x00
#define PCAL_DAT_OUT            0x04
#define PCAL_CONFIG             0x0C
#define BT_EN_BIT               7       /* port 0 */

#define BT_POWER_MS             100     /* bcm43xx.c:99 */
#define MINIDRV_MS              50      /* btbcm.c:126 */
#define LAUNCH_MS               250     /* btbcm.c:162 */
#define CMD_TIMEOUT_MS          3000

static struct hakios_hsi2c bus = {
	.name = "bt-i2c",
	.regs_pa = 0x10930000,                  /* hsi2c@10930000: USI10_I2C of PERIC1 */
	.usi_mode_pa = 0x1081103C,              /* USI10's mode word: I2C, as Android leaves it (the header) */
	.cmu_pa = CMU_PERIC1_PA, .cmu_size = CMU_PERIC1_SIZE,
	.gates = { 0x2050, 0x20A8, 0x210C, 0x2110 }, .ngates = 4,
	.qch = 0x304C, .div = 0x1830, .mux = 0x0120,
	.pins_pa = GPIO_PERIC1_PA + BANK_GPP6,  /* gpp6-2 SCL, gpp6-3 SDA: [15:8] */
	.pins_mask = 0xff00u, .pins_want = 0x3300u,
	.ipclk_mhz = 200, .bus_hz = 100000,
};

static vm_offset_t uart, cmu, alive, peric0, peric1;
static uint32_t uart_mhz = 200;
static bool powered;                    /* BT_EN driven high by this run */
static bool i2c_up;
static struct h4_rx rx;
static uint32_t rx_errors;

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

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
deadline_ms(uint32_t ms)
{
	uint64_t interval;

	nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &interval);
	return mach_absolute_time() + interval;
}

/* ---- pins ---------------------------------------------------------------------------------------------------- */

static uint32_t
pin_con(vm_offset_t gpio, uint32_t bank, unsigned pin)
{
	return (rd(gpio, bank) >> (4 * pin)) & 0xf;
}

static uint32_t
pin_dat(vm_offset_t gpio, uint32_t bank, unsigned pin)
{
	return (rd(gpio, bank + 4) >> pin) & 1;
}

/* an output at a level: the level first, then the function (so it never drives the other one) */
static void
pin_output(vm_offset_t gpio, uint32_t bank, unsigned pin, bool high)
{
	uint32_t dat = rd(gpio, bank + 4), con = rd(gpio, bank);

	wr(gpio, bank + 4, high ? dat | 1u << pin : dat & ~(1u << pin));
	wr(gpio, bank, (con & ~(0xfu << (4 * pin))) | 1u << (4 * pin));
}

static void
look_pins(const char *when)
{
	say("bt: %s: UART pins gpq0 CON %08x PUD %08x DAT %02x (RXD TXD RTS CTS want function 2, pull-up: [15:0] %04x, %04x)\n",
	    when, rd(alive, BANK_GPQ0), rd(alive, BANK_GPQ0 + 8), rd(alive, BANK_GPQ0 + 4), UART_PINS_CON, UART_PINS_PUD);
	say("bt: %s: BT_WAKE gpg2-0 function %x level %u; BT_HOST_WAKE gpa2-3 function %x level %u; expander reset gpg3-6 "
	    "function %x level %u (1, 1 is running)\n", when, pin_con(peric0, BANK_GPG2, PIN_BT_WAKE),
	    pin_dat(peric0, BANK_GPG2, PIN_BT_WAKE), pin_con(alive, BANK_GPA2, PIN_HOST_WAKE),
	    pin_dat(alive, BANK_GPA2, PIN_HOST_WAKE), pin_con(peric1, BANK_GPG3, PIN_EXP_RESET),
	    pin_dat(peric1, BANK_GPG3, PIN_EXP_RESET));
}

/* ---- UART_BT ------------------------------------------------------------------------------------------------- */

/* a gate is open while hardware runs it (manual clear) or it is forced on */
static bool
uart_clocks_open(void)
{
	bool open = true;

	for (unsigned i = 0; i < sizeof(uart_gates) / sizeof(uart_gates[0]); i++) {
		uint32_t g = rd(cmu, uart_gates[i].off);

		if ((g & GATE_MANUAL) && !(g & GATE_ON)) {
			open = false;
		}
	}
	return open;
}

static void
look_clocks(void)
{
	uint32_t div = rd(cmu, UART_DIV), mux = rd(cmu, PERIC1_IP_USER_MUX);

	say("bt: UART_BT's clock: PERIC1_IP_USER mux %08x (bit 4: %s), divider %08x (ratio %u: %u MHz if the input is "
	    "Android's 400; %u assumed), Q-channel %08x\n", mux, (mux & (1u << 4)) ? "CMU_TOP's" : "the oscillator, 26 MHz",
	    div, (div & 0xf) + 1, 400 / ((div & 0xf) + 1), uart_mhz, rd(cmu, UART_QCH));
	for (unsigned i = 0; i < sizeof(uart_gates) / sizeof(uart_gates[0]); i++) {
		say("bt:   gate %s %08x\n", uart_gates[i].name, rd(cmu, uart_gates[i].off));
	}
}

static void
look_uart(const char *when)
{
	say("bt: %s: ULCON %x UCON %x UFCON %x UMCON %x UTRSTAT %x UFSTAT %x UMSTAT %x UBRDIV %x UFRACVAL %x UINTM %x "
	    "USI_CON %x USI_OPTION %x\n", when, rd(uart, ULCON), rd(uart, UCON), rd(uart, UFCON), rd(uart, UMCON),
	    rd(uart, UTRSTAT), rd(uart, UFSTAT), rd(uart, UMSTAT), rd(uart, UBRDIV), rd(uart, UFRACVAL), rd(uart, UINTM),
	    rd(uart, USI_CON), rd(uart, USI_OPTION));
}

static void
fifo_reset(void)
{
	uint64_t until = deadline_ms(10);

	wr(uart, UFCON, UFCON_FIFO | UFCON_RESET_BOTH);
	while ((rd(uart, UFCON) & UFCON_RESET_BOTH) && mach_absolute_time() < until) {
	}
	wr(uart, UFCON, UFCON_FIFO);
}

static bool
bt_uart_setup(void)
{
	struct exynos_uart_div d;

	if (!exynos_uart_divisor(uart_mhz * 1000000u, BAUD, &d)) {
		say("bt: 3: no divisor for %u baud from %u MHz\n", BAUD, uart_mhz);
		return false;
	}
	say("bt: 3: UART_BT's clocks forced on and its Q-channel enabled; gpq0-0..3 to the UART, pulled up\n");
	for (unsigned i = 0; i < sizeof(uart_gates) / sizeof(uart_gates[0]); i++) {
		wr(cmu, uart_gates[i].off, rd(cmu, uart_gates[i].off) | GATE_MANUAL | GATE_ON);
	}
	wr(cmu, UART_QCH, rd(cmu, UART_QCH) | 3);
	wr(alive, BANK_GPQ0 + 8, (rd(alive, BANK_GPQ0 + 8) & ~UART_PINS_MASK) | UART_PINS_PUD);
	wr(alive, BANK_GPQ0, (rd(alive, BANK_GPQ0) & ~UART_PINS_MASK) | UART_PINS_CON);
	say("bt: 3: the UART: out of USI reset, interrupts masked, FIFOs on and emptied, 8N1, UBRDIV %u UFRACVAL %u "
	    "(%u baud from %u MHz)\n", d.ubrdiv, d.fracval, d.actual, uart_mhz);
	wr(uart, USI_CON, 1);
	delay(1);
	wr(uart, USI_CON, 0);
	delay(1);
	wr(uart, USI_OPTION, USI_CLKREQ_ON);
	wr(uart, UINTM, 0xf);
	wr(uart, UINTP, 0xf);
	wr(uart, UINTSP, 0xf);
	wr(uart, UCON, 0);
	wr(uart, UMCON, 0);
	fifo_reset();
	wr(uart, ULCON, ULCON_8N1);
	wr(uart, UBRDIV, d.ubrdiv);
	wr(uart, UFRACVAL, d.fracval);
	(void)rd(uart, UERSTAT);
	look_uart("3: set up");
	return true;
}

/* bytes out, waiting while the FIFO is full (with flow control on, while the chip holds CTS) */
static bool
bt_write(const uint8_t *b, size_t n, uint32_t timeout_ms)
{
	uint64_t until = deadline_ms(timeout_ms);

	for (size_t i = 0; i < n; i++) {
		while (rd(uart, UFSTAT) & UFSTAT_TX_FULL) {
			if (mach_absolute_time() > until) {
				say("bt: sending stopped at byte %zu of %zu: UFSTAT %x UMSTAT %x (CTS %s)\n", i, n, rd(uart, UFSTAT),
				    rd(uart, UMSTAT), (rd(uart, UMSTAT) & UMSTAT_CTS) ? "asserted" : "not asserted");
				return false;
			}
		}
		wr(uart, UTXH, b[i]);
	}
	return true;
}

/* one byte in, or -1 when there is none */
static int
bt_getc(void)
{
	uint32_t st = rd(uart, UFSTAT);

	if ((st & UFSTAT_RX_COUNT) == 0 && !(st & UFSTAT_RX_FULL)) {
		return -1;
	}
	if (rd(uart, UERSTAT) & 0xf) {
		rx_errors++;
	}
	return (int)(rd(uart, URXH) & 0xff);
}

static bool
bt_uart_loopback(void)
{
	static const char probe[] = "HakiOS BT loop!";
	uint8_t got[sizeof(probe)];
	size_t n = 0;
	uint64_t until;

	say("bt: 3: the UART's internal loopback (no chip involved): %zu bytes out and back\n", sizeof(probe));
	wr(uart, UCON, UCON_LOOPBACK | UCON_CPU_MODES);
	fifo_reset();
	if (!bt_write((const uint8_t *)probe, sizeof(probe), 100)) {
		wr(uart, UCON, UCON_CPU_MODES);
		return false;
	}
	until = deadline_ms(50);
	while (n < sizeof(probe) && mach_absolute_time() < until) {
		int c = bt_getc();

		if (c >= 0) {
			got[n++] = (uint8_t)c;
		}
	}
	wr(uart, UCON, UCON_CPU_MODES);
	fifo_reset();
	if (n != sizeof(probe) || memcmp(got, probe, sizeof(probe)) != 0) {
		say("bt: 3: loopback: %zu of %zu bytes came back%s: the UART is not running (UFSTAT %x)\n", n, sizeof(probe),
		    n == sizeof(probe) ? ", not the same" : "", rd(uart, UFSTAT));
		return false;
	}
	say("bt: 3: loopback: all %zu bytes came back\n", n);
	return true;
}

/* ---- the expander -------------------------------------------------------------------------------------------- */

static bool
pcal_get(uint8_t reg, uint8_t *value)
{
	uint32_t e = hakios_hsi2c_read_reg(&bus, EXPANDER, reg, value, 1);

	if (e != 0) {
		say("bt: the expander's register %02x could not be read (%x)\n", reg, e);
		return false;
	}
	return true;
}

/* bit 7 of port 0's DAT_OUT or CONFIG, and nothing else; read back */
static bool
pcal_bt_en_bit(uint8_t reg, bool on)
{
	uint8_t now, want, back = 0, buf[2];

	if ((reg != PCAL_DAT_OUT && reg != PCAL_CONFIG) || !pcal_get(reg, &now)) {
		return false;
	}
	want = pcal_with_bit(now, BT_EN_BIT, on);
	buf[0] = reg;
	buf[1] = want;
	if (want != now && hakios_hsi2c_message(&bus, EXPANDER, false, buf, 2, true) != 0) {
		say("bt: the expander's register %02x could not be written\n", reg);
		return false;
	}
	if (!pcal_get(reg, &back) || back != want) {
		say("bt: the expander's register %02x: %02x written over %02x, %02x read back\n", reg, want, now, back);
		return false;
	}
	return true;
}

static bool
look_expander(const char *when)
{
	uint8_t in[3], out[3], cfg[3];

	for (unsigned p = 0; p < 3; p++) {
		if (!pcal_get((uint8_t)(PCAL_INPUT + p), &in[p]) || !pcal_get((uint8_t)(PCAL_DAT_OUT + p), &out[p]) ||
		    !pcal_get((uint8_t)(PCAL_CONFIG + p), &cfg[p])) {
			return false;
		}
	}
	say("bt: %s: the expander: input %02x %02x %02x, output %02x %02x %02x, direction %02x %02x %02x (1 an input); "
	    "BT_EN (P0_7) %s, driving %u, reads %u\n", when, in[0], in[1], in[2], out[0], out[1], out[2], cfg[0], cfg[1], cfg[2],
	    (cfg[0] >> BT_EN_BIT) & 1 ? "an input" : "an output", (out[0] >> BT_EN_BIT) & 1, (in[0] >> BT_EN_BIT) & 1);
	return true;
}

static bool
expander_up(void)
{
	if (!i2c_up) {
		say("bt: 2: the expander's bus: USI10_I2C's clocks on, USI10's mode I2C (Android's), gpp6-2/3 function 3, "
		    "the controller reset, %u kHz\n", bus.bus_hz / 1000);
		hakios_hsi2c_power(&bus);
		hakios_hsi2c_reset(&bus);
		i2c_up = true;
	}
	if (pin_con(peric1, BANK_GPG3, PIN_EXP_RESET) != 1 || pin_dat(peric1, BANK_GPG3, PIN_EXP_RESET) != 1) {
		uint32_t release = 0;

		PE_parse_boot_argn("bt_expander_reset", &release, sizeof(release));
		if (release != 1) {
			say("bt: 2: the expander is held in reset (gpg3-6 not an output high): it is left so; "
			    "bt_expander_reset=1 lets it go as Android does\n");
			return false;
		}
		say("bt: 2: the expander out of reset: gpg3-6 an output, high (bt_expander_reset=1), then 10 ms\n");
		pin_output(peric1, BANK_GPG3, PIN_EXP_RESET, true);
		sleep_ms(10);
	}
	return look_expander("2");
}

/* ---- power --------------------------------------------------------------------------------------------------- */

static void
power_off(const char *why)
{
	if (powered) {
		say("bt: the chip off (%s): BT_EN low, BT_WAKE low\n", why);
		if (!pcal_bt_en_bit(PCAL_DAT_OUT, false)) {
			say("bt: BT_EN could not be brought low: the chip may still be powered until the phone restarts\n");
		}
		powered = false;
	}
	if (peric0 && pin_con(peric0, BANK_GPG2, PIN_BT_WAKE) == 1) {
		pin_output(peric0, BANK_GPG2, PIN_BT_WAKE, false);
	}
}

static bool
power_on(void)
{
	say("bt: 4: BT_WAKE (gpg2-0) an output, high\n");
	pin_output(peric0, BANK_GPG2, PIN_BT_WAKE, true);
	say("bt: 4: BT_EN (expander P0_7) an output, low, as Android's probe leaves it; 10 ms\n");
	if (!pcal_bt_en_bit(PCAL_DAT_OUT, false) || !pcal_bt_en_bit(PCAL_CONFIG, false)) {
		return false;
	}
	sleep_ms(10);
	say("bt: 4: BT_EN high: the chip powered; %u ms\n", BT_POWER_MS);
	powered = true;
	if (!pcal_bt_en_bit(PCAL_DAT_OUT, true)) {
		return false;
	}
	sleep_ms(BT_POWER_MS);
	look_expander("4");
	look_pins("4");
	say("bt: 4: UMSTAT %x (CTS %s), UFSTAT %x\n", rd(uart, UMSTAT), (rd(uart, UMSTAT) & UMSTAT_CTS) ? "asserted" : "not "
	    "asserted", rd(uart, UFSTAT));
	{
		int c, n = 0;

		while ((c = bt_getc()) >= 0 && n < 64) {
			if (n++ == 0) {
				say("bt: 4: the chip sent on its own:");
			}
			say(" %02x", c);
		}
		say(n ? "\n" : "bt: 4: nothing from the chip on its own (as expected)\n");
	}
	return true;
}

/* ---- HCI ----------------------------------------------------------------------------------------------------- */

/*
 * A command, and its Command Complete waited for: true with its status 0 and the return parameters after the status
 * copied out. A Command Status with an error ends it; other events are logged and passed over.
 */
static bool
hci(uint16_t opcode, const uint8_t *param, uint8_t plen, uint8_t *ret, uint8_t *retlen, uint32_t timeout_ms, bool quiet)
{
	uint8_t pkt[4 + 255];
	size_t n = h4_command(pkt, sizeof(pkt), opcode, param, plen);
	uint64_t until;
	uint32_t junk_before = rx.dropped_bytes;

	h4_rx_reset(&rx);
	if (n == 0 || !bt_write(pkt, n, timeout_ms)) {
		return false;
	}
	until = deadline_ms(timeout_ms);
	while (mach_absolute_time() < until) {
		int c = bt_getc();
		uint16_t op;
		uint8_t status, rl;
		const uint8_t *r;

		if (c < 0) {
			sleep_ms(1);
			continue;
		}
		if (h4_rx_byte(&rx, (uint8_t)c) != H4_RX_EVENT) {
			continue;
		}
		if (hci_command_complete(rx.event, &op, &status, &r, &rl) && op == opcode) {
			if (rx.dropped_bytes != junk_before) {
				say("bt: %u bytes before %04x's answer were not a packet\n", rx.dropped_bytes - junk_before, opcode);
			}
			if (status != 0) {
				say("bt: command %04x: status %02x\n", opcode, status);
				return false;
			}
			if (ret != NULL) {
				memcpy(ret, r, rl);
				*retlen = rl;
			}
			return true;
		}
		if (hci_command_status(rx.event, &op, &status) && op == opcode && status != 0) {
			say("bt: command %04x refused: status %02x\n", opcode, status);
			return false;
		}
		if (!quiet) {
			say("bt: while waiting for %04x: event %02x, %u bytes, passed over\n", opcode, rx.event[0], rx.event[1]);
		}
	}
	say("bt: command %04x: no answer in %u ms (UFSTAT %x UMSTAT %x, %u bytes not a packet, %u receive errors)\n", opcode,
	    timeout_ms, rd(uart, UFSTAT), rd(uart, UMSTAT), rx.dropped_bytes - junk_before, rx_errors);
	return false;
}

static bool
identify(const char *when, struct hci_local_version *v)
{
	uint8_t ret[255], rl = 0;
	char name[64];

	say("bt: %s: HCI_Reset\n", when);
	if (!hci(HCI_RESET, NULL, 0, NULL, NULL, CMD_TIMEOUT_MS, false)) {
		return false;
	}
	say("bt: %s: Read Local Version\n", when);
	if (!hci(HCI_READ_LOCAL_VERSION, NULL, 0, ret, &rl, CMD_TIMEOUT_MS, false) || !hci_local_version(ret, rl, v)) {
		return false;
	}
	say("bt: %s: HCI version %u revision %04x, LMP version %u subversion %04x, manufacturer %04x%s\n", when, v->hci_ver,
	    v->hci_rev, v->lmp_ver, v->lmp_subver, v->manufacturer,
	    v->manufacturer == BCM_MANUFACTURER ? " (Broadcom)" : " (not Broadcom's 000f)");
	if (hci(HCI_READ_LOCAL_NAME, NULL, 0, ret, &rl, CMD_TIMEOUT_MS, false)) {
		hci_local_name(ret, rl, name, sizeof(name));
		say("bt: %s: local name \"%s\"\n", when, name);
	}
	return true;
}

static bool
patch(void)
{
	char file[64] = "bcm4361B2.hcd";
	const uint8_t *data = NULL;
	uint64_t size = 0;
	struct bcm_hcd_info info;
	struct bcm_hcd_record rec;
	size_t off = 0;
	uint32_t sent = 0;
	int r;

	PE_parse_boot_arg_str("bt_hcd", file, sizeof(file));
	for (int tries = 0; tries < 10 && data == NULL; tries++) {
		data = hakios_image_file("fw", file, &size, NULL);
		if (data == NULL) {
			sleep_ms(500);
		}
	}
	if (data == NULL) {
		say("bt: 6: no fw/%s in the small image: put the phone's /vendor/firmware/bcm4361B2_<module>.hcd into fw-bt/ "
		    "as %s (devices/starlte/BLUETOOTH.md) and rebuild the image\n", file, file);
		return false;
	}
	r = bcm_hcd_check(data, (size_t)size, &info);
	if (r != BCM_HCD_OK) {
		say("bt: 6: fw/%s (%llu bytes) is not a patch to send: %s (after %u records)\n", file, size, bcm_hcd_error(r),
		    info.records);
		return false;
	}
	say("bt: 6: fw/%s: %llu bytes, %u records: %u Write_RAM (%u bytes), %u other, Launch_RAM at %08x; \"%s\"\n", file,
	    size, info.records, info.writes, info.write_bytes, info.others, info.launch_address,
	    info.name[0] ? info.name : "(no description)");
	say("bt: 6: Download Minidriver (fc2e), then %u ms\n", MINIDRV_MS);
	if (!hci(BCM_DOWNLOAD_MINIDRIVER, NULL, 0, NULL, NULL, CMD_TIMEOUT_MS, false)) {
		return false;
	}
	sleep_ms(MINIDRV_MS);
	say("bt: 6: the records, one by one, each answered before the next\n");
	while (bcm_hcd_next(data, (size_t)size, &off, &rec) == 1) {
		bool last = rec.opcode == BCM_LAUNCH_RAM;

		if (last) {
			say("bt: 6: %u records sent; Launch_RAM\n", sent);
		}
		if (!hci(rec.opcode, rec.param, rec.plen, NULL, NULL, last ? 1000 : CMD_TIMEOUT_MS, true)) {
			if (!last) {
				say("bt: 6: record %u of %u (%04x) failed\n", sent + 1, info.records, rec.opcode);
				return false;
			}
			say("bt: 6: Launch_RAM unanswered: the chip may have restarted into the patch without saying so; going on\n");
		}
		sent++;
		if (!last && sent % 100 == 0) {
			say("bt: 6: %u of %u\n", sent, info.records);
		}
	}
	say("bt: 6: %u ms for the patch to start\n", LAUNCH_MS);
	sleep_ms(LAUNCH_MS);
	fifo_reset();
	return true;
}

/* ---- the steps ----------------------------------------------------------------------------------------------- */

static void
bt_thread(void *arg, wait_result_t wr_unused)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg, keep = 0;
	struct hci_local_version rom, patched;

	(void)wr_unused;
	PE_parse_boot_argn("bt_keep", &keep, sizeof(keep));
	PE_parse_boot_argn("bt_uart_mhz", &uart_mhz, sizeof(uart_mhz));
	if (uart_mhz < 26 || uart_mhz > 400) {
		uart_mhz = 200;
	}
	uart = ml_io_map(UART_PA, 0x1000);
	cmu = ml_io_map(CMU_PERIC1_PA, CMU_PERIC1_SIZE);
	alive = ml_io_map(GPIO_ALIVE_PA, 0x1000);
	peric0 = ml_io_map(GPIO_PERIC0_PA, 0x1000);
	peric1 = ml_io_map(GPIO_PERIC1_PA, 0x1000);
	if (!uart || !cmu || !alive || !peric0 || !peric1 || !hakios_hsi2c_map(&bus, say)) {
		say("bt: a block could not be mapped: off\n");
		goto out;
	}
	say("bt: bt=%u: the BCM4361's Bluetooth on UART_BT (0x10840000), BT_EN on the PCAL6524 at 0x22 of hsi2c@10930000\n",
	    steps);
	say("bt: 1: a look, reading only\n");
	look_clocks();
	look_pins("1");
	if (uart_clocks_open()) {
		look_uart("1");
	} else {
		say("bt: 1: a gate of UART_BT is forced off: its registers are not read\n");
	}
	if (steps < 2) {
		goto out;
	}
	if (!expander_up() || steps < 3) {
		goto out;
	}
	if (!bt_uart_setup() || !bt_uart_loopback()) {
		goto out;
	}
	say("bt: 3: flow control on (RTS/CTS, as Android's)\n");
	wr(uart, UMCON, UMCON_AFC);
	if (steps < 4) {
		goto out;
	}
	if (!power_on() || steps < 5) {
		goto done;
	}
	if (!identify("5", &rom) || steps < 6) {
		goto done;
	}
	if (!patch() || !identify("6", &patched)) {
		goto done;
	}
	say("bt: 6: before the patch %u.%u.%u build %u, after it %u.%u.%u build %u (as btbcm.c:497-499 prints them: the LMP "
	    "subversion's three fields and the HCI revision's low 12 bits)%s\n", rom.lmp_subver >> 13, (rom.lmp_subver >> 8) & 0x1f,
	    rom.lmp_subver & 0xff, rom.hci_rev & 0xfff, patched.lmp_subver >> 13, (patched.lmp_subver >> 8) & 0x1f,
	    patched.lmp_subver & 0xff, patched.hci_rev & 0xfff, rom.hci_rev == patched.hci_rev && rom.lmp_subver ==
	    patched.lmp_subver ? ": nothing changed, so the patch may not have taken" : "");
done:
	if (keep == 1 && powered) {
		say("bt: bt_keep=1: the chip left powered\n");
	} else {
		power_off("the steps are done");
	}
out:
	say("bt: done (%u receive errors, %u bytes not a packet)\n", rx_errors, rx.dropped_bytes);
	thread_terminate(current_thread());
}

void
hakios_bcm4361_bt_init(void)
{
	uint32_t steps = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("bt", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != 0x10000000) {
		return;                 /* not the Exynos 9810's (the POCO's bt=N is hakios.bt.c's) */
	}
	if (steps > 6) {
		steps = 6;
	}
	if (kernel_thread_start(bt_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
