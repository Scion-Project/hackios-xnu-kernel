/*
 * Where the kernel enters brcmfmac's object (lx_prepare(), lx_start(), lx_stop(), lx_transmit() of sys/hakios.lx.h),
 * and how far it goes: the kernel's option brcmfmac=N, each step said before it is taken.
 *
 *   1  the device handed over by the root complex made a struct pci_dev (lx_pci.c), and brcmfmac's module start
 *      (brcmf_core_init(): its PCI driver registered, the device probed): BAR0 and BAR1 mapped, bus mastering on,
 *      the chip's ID, revision and cores read through BAR0's window, the driver's private state made. The firmware
 *      is asked for and never given (lx_kpi.c): the driver waits for it, and nothing else happens
 *   2  ... which module the chip is, from its OTP (lx_brcmf_identify(): two of its registers written as bcmdhd writes
 *      them, then read), and the firmware, NVRAM and CLM asked for by brcmfmac's names (brcm/brcmfmac4361b2-pcie.bin,
 *      .txt, .clm_blob), found or not, the NVRAM parsed; nothing more goes to the chip
 *   3  ... the chip's processor held, the firmware and NVRAM written into its RAM through BAR1, the processor let go
 *      and waited for (up to 5 s) to say where its shared area is; the rings set up in memory the chip is to read
 *      and write, and their addresses given to it. Nothing has gone over them yet
 *   4  ... the chip's interrupt (lx_pci.c's thread that looks), and the firmware's interface: brcmf_attach(), whose
 *      first command goes over the rings (the first time the chip reads and writes memory: DMA), its events,
 *      cfg80211's wiphy (registered: lx_cfg.c says what the firmware can do) and the network device
 *   5  ... the chip's interrupt by INTx instead (lx_pci.c; SPI 253 through the root complex's latch, the kernel's
 *      hakios.brcmfmac.c); brcmfmac_poll=1 keeps the thread that looks
 *   6  ... the phone's address given to brcmfmac at its probe (lx_cfg.c's brcmf_dmi_probe()), which sets it in the
 *      firmware at step 4 (cur_etheraddr)
 *   7  ... the station (lx_cfg.c): roaming off; once brcmf_attach() is through, the interface up (the firmware's "up"),
 *      en0 on it and the station given to the kernel (sys/hakios.sta.h): scans for iOS's Wi-Fi settings
 *   8  ... joining: brcmfmac's connect, the four-way handshake by lx_wpa.c, the keys to the firmware, frames
 * What stands between steps 2 and 3 and 3 and 4 is two lines brcmfmac's pcie.c is given for this
 * (wifi/lx/brcmfmac/patches/bcm4361.patch): where the step ends, brcmf_pcie_setup() returns and leaves things as they
 * are (its way of failing would reset the chip and say that its firmware crashed). A third tells lx_cfg.c that
 * brcmf_attach() is through.
 *
 * lx_stop() (the kernel's hakios_wifi_stop(), where a run ends in order): the station taken back and the interface down,
 * then brcmfmac's module end (its PCI driver's removal: the interrupt let go, the chip reset through its watchdog,
 * memory given back), if the driver got as far as its interface; before that (brcmfmac=1 to 3, or a step 4 still
 * under way) the driver is left as it is, and the kernel only takes the radio's power.
 *
 * brcmfmac's own log: brcmfmac_debug=MASK (debug.h: 0x4 INFO, 0x80000 PCIE, 0x100000 FWCON, 0x2 TRACE...);
 * 0x180004 if not given. A run that only looks is quieter with brcmfmac_debug=1 (errors only).
 */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/workqueue.h>

#include "core.h"
#include "debug.h"

#include <lx_os.h>
#include "lx.h"
#include "lx_brcmf.h"

extern int brcmf_msg_level;			/* common.c: the driver's module parameter "debug" */
int brcmf_core_init(void);
void brcmf_core_exit(void);

static struct {
	unsigned steps;				/* brcmfmac=N */
	bool started;
	bool attached;				/* brcmf_attach() came through: the firmware's interface is there */
} b;

bool
lx_brcmf_reached(unsigned step)
{
	return (b.steps >= step);
}

bool
lx_brcmf_may(unsigned step, const char *what)
{
	if (step > b.steps) {
		lx_tell("lx: brcmfmac=%u: stops before step %u (%s)\n", b.steps, step, what);
		return (false);
	}
	lx_tell("lx: brcmfmac %u: %s\n", step, what);
	lx_mark(what);
	return (true);
}

/* brcmf_pcie_setup() has come through brcmf_attach() (bcm4361.patch), on the thread the firmware was given on */
void
lx_brcmf_attached(struct device *dev)
{
	b.attached = true;
	lx_tell("lx: brcmfmac's interface to the firmware is up\n");
	if (lx_brcmf_reached(7)) {
		lx_cfg_begin();
	} else {
		lx_brcmf_may(7, "the station: the interface up, en0 on it, the kernel's to ask");
	}
}

/*
 * Which module the chip is (Murata or SEMCO, and its revision), read from its OTP the way Samsung's bcmdhd reads it
 * before it chooses the NVRAM (xnu/files/bsd/sys/hakios.bcm4361_cis.h has the facts and the addresses): two of the chip's
 * registers written and read back (OTP Control 1, the PMU's minimum resources: what bcmdhd writes there, to have the
 * OTP readable), then the 22 words of its CIS, given to the kernel (lx_os_radio_identity(), hakios.brcmfmac.c), which
 * reads them and from then on asks for this module's NVRAM (and Bluetooth's patch). Called by brcmfmac's probe once the
 * chip is attached and before the firmware is asked for (bcm4361.patch); from brcmfmac=2 on, when the files are asked
 * for at all.
 */
u32 lx_brcmf_bp_read32(void *ctx, u32 addr);
void lx_brcmf_bp_write32(void *ctx, u32 addr, u32 value);

void
lx_brcmf_identify(void *ctx, u32 chip, u32 chiprev)
{
	static const struct {
		u32 address, value;
		const char *what;
	} on[] = {
		{ 0x18010324, 0x00fa0000, "OTP Control 1" },
		{ 0x18012618, 0x0e4fffff, "the PMU's minimum resources" },
	};
	unsigned char cis[22 * 4];
	unsigned i;

	if (!lx_brcmf_reached(2)) {
		lx_tell("lx: brcmfmac=%u: the module's identity in OTP is read from brcmfmac=2 on\n", b.steps);
		return;
	}
	lx_tell("lx: brcmfmac 2: the module's identity from the chip's OTP (chip %x revision %u), as bcmdhd reads it\n",
	    chip, chiprev);
	for (i = 0; i < sizeof(on) / sizeof(on[0]); i++) {
		u32 before = lx_brcmf_bp_read32(ctx, on[i].address), after;

		lx_tell("lx: %s (%#x) reads %#x; writing %#x\n", on[i].what, on[i].address, before, on[i].value);
		lx_brcmf_bp_write32(ctx, on[i].address, on[i].value);
		after = lx_brcmf_bp_read32(ctx, on[i].address);
		lx_tell("lx: %s now reads %#x\n", on[i].what, after);
	}
	for (i = 0; i < sizeof(cis) / 4; i++) {
		u32 word = lx_brcmf_bp_read32(ctx, 0x18011110 + 4 * i);

		cis[4 * i] = word & 0xff;
		cis[4 * i + 1] = (word >> 8) & 0xff;
		cis[4 * i + 2] = (word >> 16) & 0xff;
		cis[4 * i + 3] = word >> 24;
	}
	lx_os_radio_identity(cis, sizeof(cis), chip, chiprev);
}

int
lx_prepare(void)
{
	return (lx_base_init() != 0 || lx_pool_init() != 0 ? ENOMEM : 0);
}

int
lx_start(void)
{
	unsigned debug = lx_os_option("brcmfmac_debug");
	int ret;

	b.steps = lx_os_option("brcmfmac");
	if (b.steps == 0 || b.started) {
		return (b.started ? EBUSY : 0);
	}
	if (lx_prepare() != 0) {
		lx_tell("lx: no thread, or no memory the radio can be given\n");
		return (ENOMEM);
	}
	brcmf_msg_level = debug != 0 ? (int)debug : BRCMF_INFO_VAL | BRCMF_PCIE_VAL | BRCMF_FWCON_VAL;
	if (system_wq == NULL && (system_wq = alloc_workqueue("brcmfmac-events", 0, 1)) == NULL) {
		return (ENOMEM);
	}
	if (!lx_brcmf_may(1, "the device as a struct pci_dev, brcmfmac's PCI driver registered and its probe: the chip's ID")) {
		return (0);
	}
	if ((ret = lx_pci_find()) != 0) {
		return (ret);
	}
	b.started = true;
	ret = brcmf_core_init();
	lx_tell("lx: brcmfmac's start returns %d; %s\n", ret, b.steps >= 2 ? "the firmware is asked for in the background, "
	    "and what follows is said as it happens" : "brcmfmac=1 ends here");
	return (ret < 0 ? -ret : ret);
}

void
lx_stop(void)
{
	if (!b.started) {
		return;
	}
	if (!b.attached) {
		lx_tell("lx: brcmfmac did not get as far as its interface: it is left as it is\n");
		return;
	}
	b.started = false;
	lx_cfg_stop();
	brcmf_core_exit();
	lx_tell("lx: brcmfmac is down again\n");
}

/* frames: once a network is joined (brcmfmac=8) */
int
lx_transmit(const unsigned char *frame, unsigned long length)
{
	return (lx_cfg_transmit(frame, length));
}

/* brcmfmac_tcm_idx=1: the rings' indices stay in the chip's RAM, not written into ours by its DMA (lx-state.patch) */
bool
lx_brcmf_tcm_idx(void)
{
	return (lx_os_option("brcmfmac_tcm_idx") != 0);
}
