/*
 * Where the kernel enters brcmfmac's object (lx_prepare(), lx_start(), lx_stop(), lx_transmit() of sys/hakios.lx.h),
 * and how far it goes: the kernel's option brcmfmac=N, each step said before it is taken.
 *
 *   1  the device handed over by the root complex made a struct pci_dev (lx_pci.c), and brcmfmac's module start
 *      (brcmf_core_init(): its PCI driver registered, the device probed): BAR0 and BAR1 mapped, bus mastering on,
 *      the chip's ID, revision and cores read through BAR0's window, the driver's private state made. The firmware
 *      is asked for and never given (lx_kpi.c): the driver waits for it, and nothing else happens
 *   2  ... the firmware, NVRAM and CLM asked for by brcmfmac's names (brcm/brcmfmac4361b2-pcie.bin, .txt,
 *      .clm_blob), found or not, the NVRAM parsed; nothing goes to the chip
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
 *   7, 8  the station and joining: roaming off (lx_board.c); the rest is not in this object yet
 * What stands between steps 2 and 3 and 3 and 4 is two lines brcmfmac's pcie.c is given for this
 * (wifi/lx/brcmfmac/patches/bcm4361.patch): where the step ends, brcmf_pcie_setup() returns and leaves things as they
 * are (its way of failing would reset the chip and say that its firmware crashed). A third tells this file that
 * brcmf_attach() is through.
 *
 * lx_stop() (the kernel's hakios_wifi_stop(), where a run ends in order): brcmfmac's module end (its PCI driver's removal: the interrupt let go, the chip reset through its watchdog,
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
		lx_tell("lx: brcmfmac=%u: the station (7, 8) is not in this object yet\n", b.steps);
	}
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
	brcmf_core_exit();
	lx_tell("lx: brcmfmac is down again\n");
}

/* frames: not until a station above the firmware's interface is built (brcmfmac=7 and 8) */
int
lx_transmit(const unsigned char *frame, unsigned long length)
{
	return (ENETDOWN);
}
