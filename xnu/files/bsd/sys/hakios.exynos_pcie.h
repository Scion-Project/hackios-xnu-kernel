/*
 * Darwin-on-hakios: the Galaxy S9's PCIe root complex of channel 0 (drivers/wifi/hakios.exynos_pcie.c), as the radio's
 * driver behind it sees it (drivers/wifi/hakios.brcmfmac.c, which gives it to Linux's brcmfmac as a struct pci_dev).
 *
 * There is one endpoint, 1:00.0, the BCM4361. Stage 1 (wifi_pcie=1..5) brings the link up and places the endpoint's
 * BARs; wifi_pcie=6 then turns its memory decoding on and hands it over: hakios_brcmfmac_start() is called on the
 * stage's own thread, and what is below may be used from then on, from any thread (one at a time: the radio's
 * driver has one caller). Before that, or when stage 1 stopped short, hakios_exynos_pcie_ready() is false and the
 * rest answers nothing (all ones, as a bus with nothing on it).
 */
#ifndef _SYS_HAKIOS_EXYNOS_PCIE_H_
#define _SYS_HAKIOS_EXYNOS_PCIE_H_

#include <stdbool.h>
#include <stdint.h>

bool     hakios_exynos_pcie_ready(void);                         /* the link is up and 1:00.0 placed and decoding */
uint32_t hakios_exynos_pcie_config_read(uint32_t offset);        /* a word of 1:00.0's configuration space (offset & ~3) */
void     hakios_exynos_pcie_config_write(uint32_t offset, uint32_t value);
/* BAR n (0-5) where stage 1 placed it: its bus address (the CPU's, the window is an identity) and size; false: none */
bool     hakios_exynos_pcie_bar(unsigned bar, uint64_t *address, uint64_t *size);

/* the radio's driver, called once by wifi_pcie=6 when the link is handed over (a kernel without one has a stub) */
void     hakios_brcmfmac_start(void);

#endif
