/*
 * brcmfmac's settings for this board (brcmfmac=6 and 7). brcmf_get_module_param() asks a board's own settings of
 * brcmf_dmi_probe() (dmi.c in Linux, for x86 boards by their DMI tables: not built here; mk-brcmfmac.sh builds with
 * CONFIG_DMI so that this one is called instead), at brcmfmac's probe:
 *   brcmfmac=6  the phone's address (the kernel's: boot-arg wifi_mac, else fw/mac.info, the phone's /efs/wifi/.mac.info),
 *               which brcmfmac's brcmf_c_preinit_dcmds() sets in the firmware ("cur_etheraddr") at step 4, as Samsung's
 *               bcmdhd does with the same file (dhd_custom_cis.c, dhd_set_macaddr_from_file()). Without one, the
 *               firmware's own (its NVRAM's or OTP's), as before
 *   brcmfmac=7  roaming off (brcmfmac's roamoff: the "roam_off" iovar at step 4): the station joins one access point,
 *               and the handshake's keys are for that one
 */
#include <linux/kernel.h>
#include <linux/device.h>

#include "core.h"
#include "common.h"

#include <lx_os.h>
#include "lx.h"
#include "lx_brcmf.h"

void
brcmf_dmi_probe(struct brcmf_mp_device *settings, u32 chip, u32 chiprev)
{
	uint8_t address[ETH_ALEN];

	if (lx_brcmf_reached(6) && lx_brcmf_may(6, "the phone's address given to brcmfmac (set in the firmware at step 4)")) {
		if (lx_os_radio_address(address)) {
			memcpy(settings->mac, address, ETH_ALEN);
		} else {
			lx_tell("lx: none: the firmware's own address is used\n");
		}
	}
	if (lx_brcmf_reached(7)) {
		settings->roamoff = true;
	}
}
