/* What the files of wifi/lx/brcmfmac know of one another (and lx.h of wifi/lx/src, which they share with ath11k's). */
#ifndef _LX_BRCMF_H_
#define _LX_BRCMF_H_

int	lx_pci_find(void);		/* lx_pci.c: the device handed over, as a struct pci_dev; 0, or an errno */
bool	lx_brcmf_may(unsigned step, const char *what);	/* lx_brcmf.c: brcmfmac=N allows this step (said, either way) */

#endif
