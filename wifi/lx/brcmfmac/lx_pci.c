/*
 * The radio's bus for brcmfmac: one struct pci_dev, the BCM4361 at 1:00.0, which the kernel's PCIe root complex has
 * enumerated, placed and handed over (lx_os_pci_*() of sys/hakios.lx.h; the kernel's side is
 * drivers/wifi/hakios.brcmfmac.c on drivers/wifi/hakios.exynos_pcie.c). What brcmfmac does through LinuxKPI's
 * linux/pci.h ends here: its configuration space a word at a time (lx_prelude.h turns FreeBSD's pci_read_config()
 * and its kin into the functions below), its BARs, its driver's registration and probe, its interrupt.
 *
 * The interrupt: the root complex has no MSI (use-msi = "false"). From brcmfmac=5 it is the chip's INTx, SPI 253 of the
 * GIC through the root complex's latch (lx_os_irq_line_*() of sys/hakios.lx.h, the kernel's hakios.brcmfmac.c): when it
 * fires, the handler brcmfmac asks for is called on the kernel's thread for it, then its threaded half if it asks for
 * that, then the line is unmasked again. Up to brcmfmac=4, or with brcmfmac_poll=1, a thread of ours calls the handler
 * every brcmfmac_poll_ms milliseconds (2 if not given) instead: brcmfmac's own handler reads the chip's mailbox register
 * through BAR0 and says whether there is anything, so looking costs one read.
 */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/pci.h>

#include <lx_os.h>
#include "lx.h"
#include "lx_brcmf.h"
#include "lx_pci_word.h"

static struct {
	struct pci_dev pdev;
	struct pci_bus bus;
	struct pci_devinfo devinfo;		/* what FreeBSD keeps of a device, as far as linux/pci.h reads it */
	struct pci_driver *driver;
	bool there, probed;
	unsigned writes;			/* configuration writes said so far */

	/* the interrupt: INTx, or looked at by a thread */
	irq_handler_t handler, thread;
	void *argument;
	bool line;				/* INTx (brcmfmac=5) */
	volatile bool polling;			/* the thread is wanted ... */
	volatile bool polls;			/* ... and runs */
	void *volatile poll_self;		/* (lx_os_thread_self() of that thread) */
	unsigned long long looks, handled;
} lp;

#define LX_PCI_IRQ	285			/* (the GIC's number of SPI 253: pdev->irq, which nothing here uses) */

/* ---- configuration space -------------------------------------------------------------------------- */

uint32_t
lx_pci_read_config(device_t dev, int reg, int width)
{
	return (lx_pci_word_get(lx_os_pci_config_read((unsigned)reg & ~3u), (unsigned)reg, width));
}

/* a byte or a half-word is written as its word, read, changed and written back (lx_pci_word.h) */
void
lx_pci_write_config(device_t dev, int reg, uint32_t value, int width)
{
	unsigned offset = (unsigned)reg & ~3u;
	uint32_t word = lx_pci_word_put(width < 4 ? lx_os_pci_config_read(offset) : 0, (unsigned)reg, value, width);

	/* (BAR0's window into the chip, 0x70 and 0x80, moves at every access of another core: not said) */
	if (offset != 0x70 && offset != 0x80 && lp.writes++ < 32) {
		printf("lx: pci: configuration %#x <- %08x\n", offset, word);
	}
	lx_os_pci_config_write(offset, word);
}

int
lx_pci_busmaster(device_t dev, int on)
{
	uint32_t command = lx_os_pci_config_read(PCIR_COMMAND) & 0xffff;

	command = on ? command | PCIM_CMD_BUSMASTEREN : command & ~PCIM_CMD_BUSMASTEREN;
	printf("lx: pci: bus mastering %s (the chip may now read and write memory)\n", on ? "ON" : "off");
	lx_os_pci_config_write(PCIR_COMMAND, command);
	return (0);
}

/* pci_pme_capable(): no power management event here (pp_cap 0) */
void *
device_get_ivars(device_t dev)
{
	return (&lp.devinfo);
}

/* ---- the BARs ----------------------------------------------------------------------------------- */

unsigned long
pci_resource_start(struct pci_dev *pdev, int bar)
{
	unsigned long long address = 0, size = 0;

	return (bar >= 0 && lx_os_pci_bar((unsigned)bar, &address, &size) ? (unsigned long)address : 0);
}

unsigned long
pci_resource_len(struct pci_dev *pdev, int bar)
{
	unsigned long long address = 0, size = 0;

	return (bar >= 0 && lx_os_pci_bar((unsigned)bar, &address, &size) ? (unsigned long)size : 0);
}

/* ---- the driver: registered, and the one device probed at once, as Linux's PCI core would ---------------- */

static const struct pci_device_id *
match(const struct pci_device_id *id)
{
	for (; id->vendor != 0 || id->device != 0 || id->class != 0; id++) {
		if ((id->vendor == PCI_ANY_ID || id->vendor == lp.pdev.vendor) &&
		    (id->device == PCI_ANY_ID || id->device == lp.pdev.device) &&
		    (id->subvendor == PCI_ANY_ID || id->subvendor == lp.pdev.subsystem_vendor) &&
		    (id->subdevice == PCI_ANY_ID || id->subdevice == lp.pdev.subsystem_device) &&
		    ((id->class ^ lp.pdev.class) & id->class_mask) == 0) {
			return (id);
		}
	}
	return (NULL);
}

/* The device as brcmfmac will see it: 0, or an errno (there is none: the root complex has not handed one over). */
int
lx_pci_find(void)
{
	static struct device_driver nothing;
	unsigned vendor, device;
	uint32_t word;

	if (!lx_os_pci_device(&vendor, &device)) {
		lx_tell("lx: pci: the root complex has handed over no device (wifi_pcie=6)\n");
		return (ENODEV);
	}
	lp.pdev.vendor = vendor;
	lp.pdev.device = device;
	word = lx_os_pci_config_read(PCIR_REVID);
	lp.pdev.class = word >> 8;
	lp.pdev.revision = word & 0xff;
	word = lx_os_pci_config_read(PCIR_SUBVEND_0);
	lp.pdev.subsystem_vendor = word & 0xffff;
	lp.pdev.subsystem_device = word >> 16;
	lp.pdev.irq = LX_PCI_IRQ;
	lp.pdev.devfn = 0;
	lp.pdev.bus = &lp.bus;
	lp.bus.number = 1;
	lp.bus.domain = 0;
	lp.bus.self = &lp.pdev;
	lp.pdev.dev.bsddev = (device_t)&lp;	/* (what FreeBSD's functions would be given: only ours above take it) */
	lp.pdev.dev.driver = &nothing;
	lp.pdev.dev.kobj.name = "1:00.0";
	INIT_LIST_HEAD(&lp.pdev.links);
	lp.there = true;
	lx_tell("lx: pci: 1:00.0 is %04x:%04x, class %06x revision %02x, subsystem %04x:%04x\n", vendor, device,
	    lp.pdev.class, lp.pdev.revision, lp.pdev.subsystem_vendor, lp.pdev.subsystem_device);
	for (int b = 0; b < 6; b++) {
		if (pci_resource_len(&lp.pdev, b) != 0) {
			lx_tell("lx: pci: BAR%d at %#lx, %lu KB\n", b, pci_resource_start(&lp.pdev, b),
			    pci_resource_len(&lp.pdev, b) >> 10);
		}
	}
	return (0);
}

int
linux_pci_register_driver(struct pci_driver *pdrv)
{
	const struct pci_device_id *id;
	int ret;

	lp.driver = pdrv;
	if (!lp.there) {
		return (0);
	}
	id = match(pdrv->id_table);
	if (id == NULL) {
		lx_tell("lx: pci: %s's table of devices has no line for %04x:%04x class %06x: not probed\n", pdrv->name,
		    lp.pdev.vendor, lp.pdev.device, lp.pdev.class);
		return (0);
	}
	lx_tell("lx: pci: %s's probe of 1:00.0 (its table's line %04x:%04x)\n", pdrv->name, id->vendor, id->device);
	lp.pdev.pdrv = pdrv;
	ret = pdrv->probe(&lp.pdev, id);
	lp.probed = ret == 0;
	lx_tell("lx: pci: the probe returns %d\n", ret);
	return (0);
}

void
linux_pci_unregister_driver(struct pci_driver *pdrv)
{
	if (lp.probed && pdrv->remove != NULL) {
		lx_tell("lx: pci: %s's removal of 1:00.0\n", pdrv->name);
		pdrv->remove(&lp.pdev);
	}
	lp.probed = false;
	lp.driver = NULL;
}

/* the driver's own failure, from a thread of its own (brcmf_pcie_setup()): its removal, as in Linux (lx_prelude.h) */
void
lx_device_release_driver(struct device *dev)
{
	if (lp.probed && lp.driver != NULL && lp.driver->remove != NULL) {
		lx_tell("lx: pci: the driver lets 1:00.0 go\n");
		lp.probed = false;
		lp.driver->remove(&lp.pdev);
	}
}

/* ---- the interrupt: INTx, or looked at --------------------------------------------------------------- */

int
_lkpi_pci_enable_msi_range(struct pci_dev *pdev, int minvec, int maxvec)
{
	return (-ENOTSUP);			/* (none on this root complex: INTx it is) */
}

static void
poller(void *argument)
{
	unsigned ms = lx_os_option("brcmfmac_poll_ms");

	ms = ms == 0 ? 2 : ms;
	lp.poll_self = lx_os_thread_self();
	lp.polls = true;
	while (lp.polling) {
		irqreturn_t r = lp.handler(LX_PCI_IRQ, lp.argument);

		lp.looks++;
		if (r == IRQ_WAKE_THREAD && lp.thread != NULL) {
			lp.handled++;
			lp.thread(LX_PCI_IRQ, lp.argument);
		} else if (r == IRQ_HANDLED) {
			lp.handled++;
		}
		lx_pause_ms(ms);
	}
	lx_os_lock();
	lp.polls = false;
	lx_os_wakeup(&lp.polls);
	lx_os_unlock();
}

/* INTx has fired (the kernel's thread for it; the line masked, the root complex's latch cleared) */
static void
line_fired(void *argument)
{
	irqreturn_t r = lp.handler(LX_PCI_IRQ, lp.argument);

	lp.looks++;
	if (r == IRQ_WAKE_THREAD && lp.thread != NULL) {
		lp.handled++;
		lp.thread(LX_PCI_IRQ, lp.argument);
	} else if (r == IRQ_HANDLED) {
		lp.handled++;
	}
	if (lp.line) {
		lx_os_irq_line_enable();
	}
}

int
lkpi_request_irq(struct device *dev, unsigned int irq, irq_handler_t handler, irq_handler_t thread_handler,
    unsigned long flags, const char *name, void *arg)
{
	int error;

	if (lp.polling || lp.line || handler == NULL) {
		return (-EBUSY);
	}
	lp.handler = handler;
	lp.thread = thread_handler;
	lp.argument = arg;
	if (lx_os_option("brcmfmac_poll") != 0) {
		lx_tell("lx: pci: brcmfmac_poll=1: the interrupt is looked at by a thread, not taken by INTx\n");
	} else if (lx_brcmf_reached(5) && lx_brcmf_may(5, "the chip's interrupt by INTx: SPI 253 through the root complex's latch")) {
		lp.line = true;
		if ((error = lx_os_irq_line_attach(line_fired, NULL)) != 0) {
			lp.line = false;
			lx_tell("lx: pci: INTx cannot be attached (%d): the interrupt is not taken\n", error);
			return (-EIO);
		}
		lx_tell("lx: pci: %s's interrupt handler is called when INTx fires\n", name);
		lx_os_irq_line_enable();
		return (0);
	}
	lp.polling = true;
	lx_tell("lx: pci: %s's interrupt handler is called by a thread every brcmfmac_poll_ms (INTx from brcmfmac=5)\n",
	    name);
	if (lx_os_thread("brcmfmac-irq", poller, NULL) != 0) {
		lp.polling = false;
		return (-ENOMEM);
	}
	return (0);
}

/* returns when the thread is not in the handlers (unless it is the thread that asks) */
void
lkpi_free_irq(unsigned int irq, void *arg)
{
	if (lp.line) {
		lp.line = false;
		lx_os_irq_line_detach();
		lx_tell("lx: pci: INTx let go: %llu handled, %llu with something\n", lp.looks, lp.handled);
		return;
	}
	if (!lp.polling) {
		return;
	}
	lp.polling = false;
	if (lx_os_thread_self() == lp.poll_self) {
		return;				/* (from inside a handler: the thread ends when it returns) */
	}
	lx_os_lock();
	while (lp.polls) {
		lx_os_sleep(&lp.polls, 5 * 1000000ull);
	}
	lx_os_unlock();
	lx_tell("lx: pci: the interrupt let go: %llu looks, %llu with something\n", lp.looks, lp.handled);
}
