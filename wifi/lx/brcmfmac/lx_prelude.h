/*
 * Included before every file of brcmfmac (tools/mk-brcmfmac.sh), as include/lx_prelude.h is before
 * ath11k's: what LinuxKPI's headers (FreeBSD 15.1) lack or say otherwise than Linux, small things each.
 */

/*
 * LinuxKPI's PCI functions (linux/pci.h) reach the device through FreeBSD's bus (pci_read_config()
 * and its kin of dev/pci/pcivar.h, which ask the device's parent by kobj). There is no such bus
 * here: FreeBSD's header is read first and those few names are then made ours (lx_pci.c), so that
 * linux/pci.h's own functions, read after, call them.
 */
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/nv.h>
#include <sys/pciio.h>
#include <dev/pci/pcivar.h>
uint32_t lx_pci_read_config(device_t dev, int reg, int width);
void	 lx_pci_write_config(device_t dev, int reg, uint32_t value, int width);
int	 lx_pci_busmaster(device_t dev, int on);
#define pci_read_config(dev, reg, width)		lx_pci_read_config(dev, reg, width)
#define pci_write_config(dev, reg, value, width)	lx_pci_write_config(dev, reg, value, width)
#define pci_enable_busmaster(dev)			lx_pci_busmaster(dev, 1)
#define pci_disable_busmaster(dev)			lx_pci_busmaster(dev, 0)
#define pci_enable_io(dev, space)			(0)	/* (stage 1 turned memory decoding on) */
#define pci_release_msi(dev)				(0)	/* (there is none) */

#include <lx_more.h>
#include <linux/delay.h>		/* msleep(), usleep_range(): Linux's, not FreeBSD's msleep() */
#include <linux/list.h>
#include <linux/device.h>

/* LinuxKPI's does nothing (FreeBSD's bus would detach); Linux's has the driver's removal run */
void	 lx_device_release_driver(struct device *dev);
#undef device_release_driver
#define device_release_driver(dev)	lx_device_release_driver(dev)

/* a flexible array's bounds for the compiler's checks: this compiler cannot take the type of one so marked */
#undef __counted_by
#define __counted_by(member)

#ifndef LIST_HEAD_INIT
#define LIST_HEAD_INIT(name)	LINUX_LIST_HEAD_INIT(name)
#endif
