/*
 * The Galaxy S9's Wi-Fi, stage 2 (starlte, devices/starlte/WIFI.md): the kernel's side of brcmfmac, Broadcom's FullMAC
 * driver of Linux, compiled outside this tree on the same layer as the POCO's ath11k (tools/mk-brcmfmac.sh, wifi/lx)
 * and linked in as one object when it is there (xnu-build.sh). hakios.lx.c gives that object memory, time, threads
 * and the station; this file gives hakios.lx.c what it asks of the phone's radio -- the log, the options, the files,
 * how the radio sees memory -- and the object its PCIe device: the BCM4361 at 1:00.0 behind the root complex of
 * hakios.exynos_pcie.c (sys/hakios.exynos_pcie.h), which brcmfmac takes as a struct pci_dev (wifi/lx/brcmfmac/lx_pci.c).
 *
 * Only with boot-args wifi_pcie=6 (stage 1, then the device handed over) and brcmfmac=N; nothing is touched without
 * them. The steps are the object's (wifi/lx/brcmfmac/lx_brcmf.c), each said before it is taken:
 *   brcmfmac=1   brcmfmac's probe: its PCI device table, BAR0 and BAR1 mapped, bus mastering on, the chip's ID,
 *                revision and cores read through BAR0's window; the firmware is not asked for
 *   brcmfmac=2   ... the firmware, NVRAM and CLM asked for (fw/brcmfmac4361b2-pcie.bin, .txt, .clm_blob of the small
 *                image, from the user's own phone, never in a repository) and checked; not loaded
 *   brcmfmac=3   ... the firmware and NVRAM written to the chip's RAM (through BAR1), its processor started, its
 *                shared area read, the rings set up in memory the chip is to read and write
 *   brcmfmac=4   ... the firmware's interface: the first command over the rings (the first time the chip reads and
 *                writes memory: DMA), events, cfg80211's wiphy (registered with nothing above it yet)
 * The chip's interrupt (INTx, SPI 253) is not used: a thread of the object looks every few milliseconds instead.
 *
 * Memory the chip reads and writes is shown to it at its physical addresses: the PCIe SysMMU (0x11420000), which
 * Android puts in between, is not touched. Whether the controller's master then reaches memory is the open question
 * step 4 answers.
 *
 * What was checked: compiled, and the object built and linked (tools/mk-brcmfmac.sh); it has not run on the phone.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.exynos_pcie.h>
#include <sys/hakios.input.h>
#include <sys/hakios.lx.h>
#include <sys/systm.h>

extern void loader_log_putc(char c);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static LCK_GRP_DECLARE(brcmfmac_lock_group, "hakios-brcmfmac");
static LCK_MTX_DECLARE(say_lock, &brcmfmac_lock_group);
static uint32_t steps;                  /* brcmfmac=N */

/*
 * The hooks below (hakios_wifi_*()) are weak: a kernel built with the WCN6750's driver as well (WIFI, every driver being
 * built unless a configuration leaves it out) has that driver's, and that driver's object (xnu-build.sh); brcmfmac is then
 * not started (hakios_brcmfmac_start()).
 */

/* ---- the log: every line both into the loader's log (pstore on this phone) and the console -------------------- */

static void
say_line(const char *line)
{
	lck_mtx_lock(&say_lock);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
	lck_mtx_unlock(&say_lock);
}

__attribute__((weak)) int
hakios_wifi_say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line);
	return 0;
}

/* (no line is kept back here: what the WCN6750's driver keeps for a quiet session, this one says) */
__attribute__((weak)) int
hakios_wifi_tell(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line);
	return 0;
}

#define say hakios_wifi_say

__attribute__((weak)) uint32_t
hakios_wifi_option(const char *name)
{
	uint32_t value = 0;

	if (strcmp(name, "brcmfmac") == 0) {
		return steps;
	}
	PE_parse_boot_argn(name, &value, sizeof(value));
	return value;
}

/* ---- what hakios.lx.c asks of the radio --------------------------------------------------------------------- */

/* There is no processor to start before the driver: the radio's own is started by brcmfmac (step 3). */
__attribute__((weak)) bool
hakios_wifi_dsp_is_up(void)
{
	return true;
}

/*
 * brcmfmac asks for "brcm/brcmfmac4361b2-pcie.bin" and the like (and first for a board's own name of each, which is
 * not there); they are looked for by the last part of the name in fw/ of the small image (userland/build.sh puts
 * fw-wifi/ there). The user's phone has them under bcmdhd's names (devices/starlte/WIFI.md says which is which).
 */
__attribute__((weak)) const uint8_t *
hakios_wifi_driver_file(const char *name, uint64_t *size)
{
	const char *last = strrchr(name, '/');
	const uint8_t *data;

	last = last == NULL ? name : last + 1;
	data = hakios_image_file("fw", last, size, NULL);  /* (hakios.image.c) */
	say("brcmfmac: the driver asks for %s: %s", name, data == NULL ? "not in fw/ of the small image\n" : "");
	if (data != NULL) {
		say("fw/%s, %llu bytes\n", last, *size);
	}
	return data;
}

/* At its physical address: the PCIe SysMMU is not used (above). */
__attribute__((weak)) bool
hakios_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address)
{
	static bool said;

	if (!said) {
		said = true;
		say("brcmfmac: memory for the radio is shown to it at its physical addresses (the first: %#llx, %llu KB); "
		    "the PCIe SysMMU is not used\n", physical, size >> 10);
	}
	*address = physical;
	return true;
}

/* MSI is off on this root complex (the device tree's use-msi = "false"): no vectors */
__attribute__((weak)) bool
hakios_wifi_radio_msi(__unused unsigned long long *address, __unused uint32_t *data, __unused uint32_t *first_spi)
{
	return false;
}

/* brcmfmac takes the address the firmware has from the NVRAM; wifi_mac is not used for this radio yet */
__attribute__((weak)) bool
hakios_wifi_radio_address(__unused uint8_t *address)
{
	return false;
}

/* not yet: joining a network is a later step of stage 2 */
__attribute__((weak)) bool
hakios_wifi_network(__unused uint8_t *name, __unused unsigned *name_length, __unused uint8_t *passphrase,
    __unused unsigned *passphrase_length)
{
	return false;
}

/* ---- the radio on PCIe, for the object's struct pci_dev (sys/hakios.lx.h) ------------------------------------- */

int
lx_os_pci_device(unsigned *vendor, unsigned *device)
{
	uint32_t id;

	if (!hakios_exynos_pcie_ready()) {
		return 0;
	}
	id = hakios_exynos_pcie_config_read(0);
	*vendor = id & 0xffff;
	*device = id >> 16;
	return 1;
}

unsigned
lx_os_pci_config_read(unsigned offset)
{
	return hakios_exynos_pcie_config_read(offset);
}

void
lx_os_pci_config_write(unsigned offset, unsigned value)
{
	hakios_exynos_pcie_config_write(offset, value);
}

int
lx_os_pci_bar(unsigned bar, unsigned long long *address, unsigned long long *size)
{
	uint64_t a, s;

	if (!hakios_exynos_pcie_bar(bar, &a, &s)) {
		return 0;
	}
	*address = a;
	*size = s;
	return 1;
}

/* ---- the start: called by wifi_pcie=6 on stage 1's thread ---------------------------------------------------- */

void
hakios_brcmfmac_start(void)
{
	int error;

#ifdef HAKIOS_WIFI
	say("brcmfmac: this kernel has the WCN6750's driver, and its object: brcmfmac is not started\n");
	return;
#endif
	if (!PE_parse_boot_argn("brcmfmac", &steps, sizeof(steps)) || steps == 0) {
		say("brcmfmac: the device is handed over but the driver is not started (brcmfmac=N, 1 to 4)\n");
		return;
	}
	say("brcmfmac: brcmfmac=%u: Linux's brcmfmac on the BCM4361, step by step (devices/starlte/WIFI.md)\n", steps);
	if ((error = hakios_lx_prepare()) != 0) {
		say("brcmfmac: the driver's preparation fails: %d\n", error);
		return;
	}
	error = hakios_lx_start();
	say("brcmfmac: the driver's start returns %d\n", error);
}
