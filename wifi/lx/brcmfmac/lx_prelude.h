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

/*
 * cfg80211's callbacks and the network device's registration: empty bodies in LinuxKPI's net/cfg80211.h and
 * linux/netdevice.h (FreeBSD's net80211 does that work there). Read here first, so that brcmfmac's calls of them, which
 * come after, are lx_cfg.c's: the station above brcmfmac (brcmfmac=7 and 8).
 */
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
struct cfg80211_bss *lx_cfg80211_inform_bss(struct wiphy *wiphy, struct linuxkpi_ieee80211_channel *channel, int signal_mbm,
    const uint8_t *bssid, uint16_t capability, uint16_t interval, const uint8_t *ie, size_t ie_length);
void	 lx_cfg80211_scan_done(struct cfg80211_scan_request *request, struct cfg80211_scan_info *info);
void	 lx_cfg80211_connect_done(struct net_device *ndev, struct cfg80211_connect_resp_params *params);
void	 lx_cfg80211_disconnected(struct net_device *ndev, uint16_t reason, bool locally_generated);
void	 lx_cfg80211_roamed(struct net_device *ndev, struct cfg80211_roam_info *info);
void	 lx_wiphy_unregister(struct wiphy *wiphy);
int	 lx_register_netdev(struct net_device *ndev);
void	 lx_unregister_netdev(struct net_device *ndev);
void	 lx_netif_rx(struct sk_buff *skb);
__be16	 lx_eth_type_trans(struct sk_buff *skb, struct net_device *ndev);
void	 lx_netif_carrier(struct net_device *ndev, bool on);
bool	 lx_netif_carrier_ok(struct net_device *ndev);
#define cfg80211_inform_bss_data(wiphy, data, type, bssid, tsf, capability, interval, ie, ie_length, gfp) \
	lx_cfg80211_inform_bss(wiphy, (data)->chan, (data)->signal, bssid, capability, interval, ie, ie_length)
#define cfg80211_inform_bss(wiphy, channel, type, bssid, tsf, capability, interval, ie, ie_length, signal, gfp) \
	lx_cfg80211_inform_bss(wiphy, channel, signal, bssid, capability, interval, ie, ie_length)
#define cfg80211_put_bss(wiphy, bss)				((void)0)
#define cfg80211_scan_done(request, info)			lx_cfg80211_scan_done(request, info)
#define cfg80211_connect_done(ndev, params, gfp)		lx_cfg80211_connect_done(ndev, params)
#define cfg80211_disconnected(ndev, reason, ie, ie_length, local, gfp) lx_cfg80211_disconnected(ndev, reason, local)
#define cfg80211_roamed(ndev, info, gfp)			lx_cfg80211_roamed(ndev, info)
#define wiphy_unregister(wiphy)					lx_wiphy_unregister(wiphy)
#define register_netdev(ndev)					lx_register_netdev(ndev)
#define cfg80211_register_netdevice(ndev)			lx_register_netdev(ndev)
#define unregister_netdev(ndev)					lx_unregister_netdev(ndev)
#define cfg80211_unregister_netdevice(ndev)			lx_unregister_netdev(ndev)
#define netif_rx(skb)						lx_netif_rx(skb)
#define eth_type_trans(skb, ndev)				lx_eth_type_trans(skb, ndev)
#define netif_carrier_on(ndev)					lx_netif_carrier(ndev, true)
#define netif_carrier_off(ndev)					lx_netif_carrier(ndev, false)
#define netif_carrier_ok(ndev)					lx_netif_carrier_ok(ndev)
