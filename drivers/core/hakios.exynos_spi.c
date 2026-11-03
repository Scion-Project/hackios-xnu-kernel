/*
 * An Exynos SPI master -- the SPI of a USI block of the Exynos 9810 -- polled: no DMA, no interrupt, the chip select
 * held by hand. sys/hakios.exynos_spi.h says how a driver describes its bus; the Galaxy S9's audio codec
 * (hakios.cs47l93.c, spi@10470000, USI01 of PERIC0) is the first to use it.
 *
 * What it takes before the first transfer, none of which the UEFI does: the block's clock gates forced on and its
 * Q-channel enabled (in its CMU), the IP clock's divider set (the SPI's SCLK is a quarter of the IP clock: Samsung's
 * driver asks its clock for four times the bus speed), the USI block made an SPI (its mode word: 2), the pins set to
 * their function. Then the controller: USI_CON cleared (its reset value holds the block in reset and drives the pads
 * to a stable level), the chip select made inactive, the interrupts off, no DMA, 8-bit words, the FIFOs flushed.
 *
 * A transfer, a FIFO's worth at a time: the channels off and the controller reset (both FIFOs emptied), the bytes put
 * in the transmit FIFO, then both channels on; the clock runs for as long as there is something to send, and every
 * byte sent brings one back into the receive FIFO, which is read once it holds them all. The chip select is the
 * controller's own pin (nSS, the pin in function 2), driven by SLAVE_SEL's bit 0 (Samsung's MANUAL_CS_MODE: 0 selects,
 * 1 is inactive), so it stays low between FIFO loads and across transfers until deselected.
 *   CH_CFG 0x00 (TXCH_ON 0, RXCH_ON 1, CPHA 2, CPOL 3, SLAVE 4, SW_RST 5, HS_EN 6), MODE_CFG 0x08 (TRAILCNT [28:19],
 *   CH_TSZ [30:29], BUS_TSZ [18:17], DMA 1-2), SLAVE_SEL 0x0C, INT_EN 0x10, STATUS 0x14 (TX level [6+], RX level [15+],
 *   the level's mask (fifo size << 1) - 1, TX_DONE 25), TX_DATA 0x18, RX_DATA 0x1C, PACKET_CNT 0x20, PENDING_CLR 0x24,
 *   SWAP_CFG 0x28 (0 with 8-bit words), FB_CLK 0x2C, USI_CON 0xC4
 * Sources: Linux's drivers/spi/spi-s3c64xx.c as the S9's kernel has it (exynos_usi_init, s3c64xx_spi_hwinit,
 * s3c64xx_spi_config, enable_datapath, wait_for_xfer, flush_fifo, enable_cs/disable_cs) and drivers/soc/samsung/usi_v2.c
 * (SPI_SW_CONF), GPL: read for these facts, not copied.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <sys/hakios.exynos_spi.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);

#define CH_CFG                  0x00
#define  CH_TXCH_ON             (1u << 0)
#define  CH_RXCH_ON             (1u << 1)
#define  CH_CPHA                (1u << 2)
#define  CH_CPOL                (1u << 3)
#define  CH_SLAVE               (1u << 4)
#define  CH_SW_RST              (1u << 5)
#define MODE_CFG                0x08
#define  MODE_TRAILCNT          (0x3ffu << 19)
#define SLAVE_SEL               0x0C
#define  SLAVE_SIG_INACT        (1u << 0)
#define  SLAVE_AUTO             (1u << 1)
#define INT_EN                  0x10
#define STATUS                  0x14
#define  ST_TX_DONE             (1u << 25)
#define TX_DATA                 0x18
#define RX_DATA                 0x1C
#define PACKET_CNT              0x20
#define PENDING_CLR             0x24
#define  PND_ALL                0x1e    /* the RX and TX over- and underrun bits */
#define SWAP_CFG                0x28
#define FB_CLK                  0x2C
#define USI_CON                 0xC4
#define USI_OPTION              0xC8

#define CMU_GATE_MANUAL         (1u << 20)
#define CMU_GATE_ON             (1u << 21)
#define CMU_DIV_BUSY            (1u << 16)
#define USI_MODE_SPI            0x2

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

static uint64_t
deadline_us(uint64_t us)
{
	uint64_t interval;

	nanoseconds_to_absolutetime(us * NSEC_PER_USEC, &interval);
	return mach_absolute_time() + interval;
}

static vm_offset_t
map_word(uint64_t pa)
{
	vm_offset_t page = ml_io_map(pa & ~0xfffULL, 0x1000);

	return page ? page + (pa & 0xfff) : 0;
}

static uint32_t
level_mask(struct hakios_exynos_spi *bus)
{
	return (bus->fifo_bytes << 1) - 1;
}

static uint32_t
tx_level(struct hakios_exynos_spi *bus, uint32_t status)
{
	return (status >> 6) & level_mask(bus);
}

static uint32_t
rx_level(struct hakios_exynos_spi *bus, uint32_t status)
{
	return (status >> 15) & level_mask(bus);
}

bool
hakios_exynos_spi_map(struct hakios_exynos_spi *bus, int (*say)(const char *, ...))
{
	if (bus->fifo_bytes == 0 || bus->fifo_bytes > 256) {
		say("%s: no FIFO size\n", bus->name);
		return false;
	}
	bus->regs = ml_io_map(bus->regs_pa, 0x1000);
	bus->usi = map_word(bus->usi_mode_pa);
	bus->cmu = bus->cmu_pa ? ml_io_map(bus->cmu_pa, bus->cmu_size) : 0;
	bus->pins = bus->pins_pa ? map_word(bus->pins_pa) : 0;
	if (!bus->regs || !bus->usi || (bus->cmu_pa && !bus->cmu) || (bus->pins_pa && !bus->pins)) {
		say("%s: a block of the bus could not be mapped\n", bus->name);
		return false;
	}
	if (bus->cmu) {
		uint32_t div = bus->div ? rd(bus->cmu, bus->div) : 0;

		say("%s: USI mode %x (2 is SPI), CMU mux %x, divider %x (%u MHz / %u = %u MHz, SCLK a quarter of it), gates %x %x "
		    "%x %x, Q-channel %x\n", bus->name, *(volatile uint32_t *)bus->usi, bus->mux ? rd(bus->cmu, bus->mux) : 0, div,
		    bus->ipclk_mhz, (div & 0xf) + 1, bus->ipclk_mhz / ((div & 0xf) + 1),
		    bus->ngates > 0 ? rd(bus->cmu, bus->gates[0]) : 0, bus->ngates > 1 ? rd(bus->cmu, bus->gates[1]) : 0,
		    bus->ngates > 2 ? rd(bus->cmu, bus->gates[2]) : 0, bus->ngates > 3 ? rd(bus->cmu, bus->gates[3]) : 0,
		    bus->qch ? rd(bus->cmu, bus->qch) : 0);
	}
	if (bus->pins) {
		say("%s: the pins' CON %x PUD %x DAT %x\n", bus->name, *(volatile uint32_t *)bus->pins,
		    *(volatile uint32_t *)(bus->pins + 8), *(volatile uint32_t *)(bus->pins + 4));
	}
	return true;
}

void
hakios_exynos_spi_power(struct hakios_exynos_spi *bus)
{
	for (uint32_t i = 0; bus->cmu && i < bus->ngates; i++) {
		wr(bus->cmu, bus->gates[i], rd(bus->cmu, bus->gates[i]) | CMU_GATE_MANUAL | CMU_GATE_ON);
	}
	if (bus->cmu && bus->qch) {
		wr(bus->cmu, bus->qch, rd(bus->cmu, bus->qch) | 3);
	}
	if (bus->cmu && bus->div && bus->div_ratio <= 0xf) {
		uint64_t deadline = deadline_us(1000);

		wr(bus->cmu, bus->div, (rd(bus->cmu, bus->div) & ~0xfu) | bus->div_ratio);
		while ((rd(bus->cmu, bus->div) & CMU_DIV_BUSY) && mach_absolute_time() < deadline) {
		}
	}
	*(volatile uint32_t *)bus->usi = USI_MODE_SPI;
	if (bus->pins) {
		uint32_t con = *(volatile uint32_t *)bus->pins;

		if ((con & bus->pins_mask) != bus->pins_want) {
			*(volatile uint32_t *)bus->pins = (con & ~bus->pins_mask) | bus->pins_want;
		}
		*(volatile uint32_t *)(bus->pins + 8) = (*(volatile uint32_t *)(bus->pins + 8) & ~bus->pud_mask) | bus->pud_want;
	}
}

/* Samsung's flush_fifo(): the channels off, the controller reset while both FIFOs empty, then out of reset */
static void
flush(struct hakios_exynos_spi *bus)
{
	vm_offset_t r = bus->regs;
	uint32_t ch = rd(r, CH_CFG) & ~(CH_TXCH_ON | CH_RXCH_ON);
	uint64_t deadline = deadline_us(1000);

	wr(r, PACKET_CNT, 0);
	wr(r, CH_CFG, ch);
	wr(r, CH_CFG, ch | CH_SW_RST);
	while (tx_level(bus, rd(r, STATUS)) && mach_absolute_time() < deadline) {
	}
	while (rx_level(bus, rd(r, STATUS)) && mach_absolute_time() < deadline) {
		(void)rd(r, RX_DATA);
	}
	wr(r, CH_CFG, ch);
	wr(r, PENDING_CLR, PND_ALL);
	wr(r, PENDING_CLR, 0);
}

void
hakios_exynos_spi_reset(struct hakios_exynos_spi *bus)
{
	vm_offset_t r = bus->regs;
	uint32_t ch = 0;

	wr(r, USI_CON, 0);              /* USI_RESET, active high and 1 out of reset: cleared before any transfer */
	wr(r, SLAVE_SEL, SLAVE_SIG_INACT);
	wr(r, INT_EN, 0);
	wr(r, MODE_CFG, 0);
	wr(r, PACKET_CNT, 0);
	wr(r, PENDING_CLR, PND_ALL);
	wr(r, PENDING_CLR, 0);
	wr(r, SWAP_CFG, 0);             /* 8-bit words: no swapping, whatever the device tree's swap-mode */
	if (bus->mode & 2) {
		ch |= CH_CPOL;
	}
	if (bus->mode & 1) {
		ch |= CH_CPHA;
	}
	wr(r, CH_CFG, ch);              /* master (SLAVE clear), the channels off */
	wr(r, MODE_CFG, MODE_TRAILCNT); /* byte-wide channel and bus, no DMA, the longest trailing count */
	wr(r, FB_CLK, bus->fb_delay & 3);
	flush(bus);
}

void
hakios_exynos_spi_select(struct hakios_exynos_spi *bus, bool on)
{
	wr(bus->regs, SLAVE_SEL, on ? 0 : SLAVE_SIG_INACT);
}

uint32_t
hakios_exynos_spi_transfer(struct hakios_exynos_spi *bus, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	vm_offset_t r = bus->regs;
	uint32_t done = 0;

	bus->transfers++;
	while (done < len) {
		uint32_t n = len - done, status = 0, got = 0;
		uint64_t deadline;

		if (n > bus->fifo_bytes / 2) {
			n = bus->fifo_bytes / 2;        /* half the FIFO: the receive side never fills */
		}
		flush(bus);
		for (uint32_t i = 0; i < n; i++) {
			wr(r, TX_DATA, tx ? tx[done + i] : 0);
		}
		wr(r, CH_CFG, rd(r, CH_CFG) | CH_TXCH_ON | CH_RXCH_ON);
		/* 10 ms at most for a FIFO's worth: at 100 kHz, 32 bytes take 2.6 ms */
		for (deadline = deadline_us(10000);;) {
			status = rd(r, STATUS);
			while (got < n && rx_level(bus, status)) {
				uint8_t b = (uint8_t)rd(r, RX_DATA);

				if (rx) {
					rx[done + got] = b;
				}
				got++;
				status = rd(r, STATUS);
			}
			if (got == n) {
				break;
			}
			if (mach_absolute_time() > deadline) {
				break;
			}
		}
		wr(r, CH_CFG, rd(r, CH_CFG) & ~(CH_TXCH_ON | CH_RXCH_ON));
		if (got != n) {
			bus->errors++;
			bus->last_status = status ? status : 1;
			if (bus->errors <= 3) {
				printf("%s: spi transfer of %u bytes stopped at %u of a %u-byte load: CH_CFG %08x MODE_CFG %08x "
				    "SLAVE_SEL %08x STATUS %08x PACKET_CNT %08x USI_CON %08x\n", bus->name, len, done + got, n,
				    rd(r, CH_CFG), rd(r, MODE_CFG), rd(r, SLAVE_SEL), status, rd(r, PACKET_CNT), rd(r, USI_CON));
			}
			flush(bus);
			return bus->last_status;
		}
		done += n;
	}
	return 0;
}

void
hakios_exynos_spi_report(struct hakios_exynos_spi *bus, int (*say)(const char *, ...))
{
	vm_offset_t r = bus->regs;

	if (!r) {
		return;
	}
	say("%s: CH_CFG %08x MODE_CFG %08x SLAVE_SEL %08x INT_EN %08x STATUS %08x PACKET_CNT %08x SWAP_CFG %08x FB_CLK %08x "
	    "USI_CON %08x USI_OPTION %08x; %llu transfers, %llu errors\n", bus->name, rd(r, CH_CFG), rd(r, MODE_CFG),
	    rd(r, SLAVE_SEL), rd(r, INT_EN), rd(r, STATUS), rd(r, PACKET_CNT), rd(r, SWAP_CFG), rd(r, FB_CLK), rd(r, USI_CON),
	    rd(r, USI_OPTION), bus->transfers, bus->errors);
}
