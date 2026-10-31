/*
 * cfg80211 for brcmfmac, and the station above it (brcmfmac=7 and 8). brcmfmac is a FullMAC driver: the firmware
 * scans, authenticates and associates by itself and the driver registers with cfg80211, not mac80211, giving it a
 * table of operations (cfg80211_ops: scan, connect, disconnect, add_key ...) and calling back when something has
 * happened (cfg80211_scan_done(), cfg80211_inform_bss_data(), cfg80211_connect_done(), cfg80211_disconnected()).
 * LinuxKPI's net/cfg80211.h and linux/netdevice.h have empty bodies for the callbacks and for the network device's
 * registration; lx_prelude.h sends brcmfmac's calls of them here instead. Above, the kernel's station boundary
 * (sys/hakios.sta.h): what iOS's Wi-Fi settings ask, through hakios.sta.c, becomes calls of brcmfmac's operations, as
 * wifi/lx/src/lx_80211.c does it for the POCO's mac80211 driver. Frames are Ethernet both ways (brcmfmac's network
 * device): ndo_start_xmit() out, netif_rx() in. (brcmfmac's settings for this board, the phone's address and roaming
 * off, are lx_board.c's.)
 *
 * The four-way handshake: by lx_wpa.c here (the same code as the POCO's, which has joined networks on that phone), not
 * by the firmware. Samsung's bcmdhd for the BCM4361 is built without BCMSUP_4WAY_HANDSHAKE (bcmdhd_101_12/Makefile: no
 * chip's flags set it, so dhd_linux.c never sets "sup_wpa" and wl_cfg80211.c never offers
 * NL80211_FEATURE_FW_4WAY_HANDSHAKE): on the S9, Android's wpa_supplicant does the handshake on the host, and whether
 * this firmware has a supplicant ("idsup" among its capabilities) at all is not known. brcmfmac uses the firmware's
 * only when cfg80211's connect parameters carry the key (crypto.psk); they do not here, so brcmfmac sets WPA2_AUTH_PSK
 * with the host's supplicant, as for wpa_supplicant: the EAPOL frames come up through netif_rx(), the replies go down
 * through ndo_start_xmit(), and the keys go to the firmware through add_key() (brcmfmac waits for message 4 to have
 * left before it sets the pairwise key: brcmf_netdev_wait_pend8021x()).
 *
 * Begun by lx_brcmf.c once brcmf_attach() is through (lx_cfg_begin()), taken back by lx_stop() (lx_cfg_stop()).
 *
 * Threads: hakios.sta.c's calls the boundary (one call at a time); brcmfmac's event work calls the callbacks; the
 * interrupt's thread calls netif_rx(), which only copies: a handshake's message is answered, and keys are given, on a
 * work queue of ours, since add_key() waits for the firmware's answer, which comes on the interrupt's thread.
 *
 * What is not done: more than one network, roaming (brcmfmac's roamoff is set: the firmware does not move to another
 * access point by itself), WPA3 and enterprise networks, protected management frames, power saving.
 *
 * Nothing here has run on the phone.
 */
#include <linux/kernel.h>
#include <linux/etherdevice.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>

#include <lx_os.h>
#include "lx.h"
#include "lx_brcmf.h"
#include "lx_rsn.h"
#include "lx_wpa.h"

#ifndef WLAN_AKM_SUITE_PSK			/* (LinuxKPI has it in net/mac80211.h, which brcmfmac does not include) */
#define WLAN_AKM_SUITE_PSK	0x000fac02
#endif

#define HEARD_MOST	64
#define EAPOL_MOST	4
#define EAPOL_BYTES	(14 + 600)

struct lx_eapol {
	uint16_t length;
	uint8_t bytes[EAPOL_BYTES];
};

static struct {
	const struct cfg80211_ops *ops;
	struct wiphy *wiphy;
	struct net_device *ndev;		/* brcmfmac's first interface, once registered */
	bool offered, up, carrier;
	struct mutex send;			/* one frame at a time into brcmfmac */

	/* a scan */
	volatile int scan_over;
	bool scan_aborted;
	unsigned heard_count;
	struct hakios_sta_bss *heard;		/* HEARD_MOST */
	unsigned long long *heard_at;		/* ... when each was heard last (nanoseconds) */

	/* a network */
	volatile int connected;			/* 0 waiting, 1 associated, -1 refused or lost */
	int status;				/* the access point's status code, when it refused */
	volatile bool joining, joined, leaving;
	int last_error;
	struct hakios_sta_bss target;
	bool protected;
	unsigned group;				/* the group's cipher: LX_RSN_CCMP or LX_RSN_TKIP */
	uint8_t rsn[LX_RSN_OURS_LENGTH];
	struct lx_wpa wpa;
	volatile int handshake;			/* what the last message that counted called for (LX_WPA_*) */
	const char *why;
	volatile int keys;			/* 0 not given, 1 given, -1 refused */

	/* handshake messages from the radio, for the work below */
	struct lx_eapol *eapol;			/* EAPOL_MOST */
	unsigned eapol_in, eapol_out;
	struct workqueue_struct *work;
	struct work_struct eapol_work;

	unsigned long long sent, received, eapol_count;
} c;

static struct hakios_sta_ops boundary;

/* ---- the wiphy ---------------------------------------------------------------------------------------------- */

struct wiphy *
linuxkpi_wiphy_new(const struct cfg80211_ops *ops, size_t priv_length)
{
	struct wiphy *wiphy = kzalloc(sizeof(*wiphy) + priv_length, GFP_KERNEL);

	if (wiphy != NULL) {
		mutex_init(&wiphy->mtx);
		c.ops = ops;
	}
	return (wiphy);
}

void
linuxkpi_wiphy_free(struct wiphy *wiphy)
{
	if (wiphy == c.wiphy) {
		c.wiphy = NULL;
	}
	kfree(wiphy);
}

/* brcmf_cfg80211_attach(): what the firmware can do, said; the station takes it from brcmfmac=7 */
int
linuxkpi_80211_wiphy_register(struct wiphy *wiphy)
{
	for (int b = 0; b < NUM_NL80211_BANDS; b++) {
		if (wiphy->bands[b] != NULL) {
			lx_tell("lx: cfg80211: band %d: %d channels, %d rates\n", b, wiphy->bands[b]->n_channels,
			    wiphy->bands[b]->n_bitrates);
		}
	}
	c.wiphy = wiphy;
	lx_tell("lx: cfg80211: the wiphy is registered%s\n", lx_brcmf_reached(7) ? "" : "; no station takes it (brcmfmac=7)");
	return (0);
}

void
lx_wiphy_unregister(struct wiphy *wiphy)
{
	if (wiphy == c.wiphy) {
		c.wiphy = NULL;
	}
}

/* ---- the network device ----------------------------------------------------------------------------------- */

int
lx_register_netdev(struct net_device *ndev)
{
	ndev->reg_state = NETREG_REGISTERED;
	if (c.ndev == NULL) {
		c.ndev = ndev;
		lx_tell("lx: the network device %s is registered (%02x:%02x:%02x:..:..:..)\n", ndev->name, ndev->dev_addr[0],
		    ndev->dev_addr[1], ndev->dev_addr[2]);
	}
	return (0);
}

/* (its memory is not given back: brcmfmac's own is, by brcmfmac, and nothing here runs after the driver's removal) */
void
lx_unregister_netdev(struct net_device *ndev)
{
	ndev->reg_state = (enum net_device_reg_state)0;	/* (LinuxKPI has no NETREG_UNREGISTERED) */
	if (ndev == c.ndev) {
		c.ndev = NULL;
		c.up = false;
	}
}

void
lx_netif_carrier(struct net_device *ndev, bool on)
{
	if (ndev == c.ndev) {
		c.carrier = on;
	}
}

bool
lx_netif_carrier_ok(struct net_device *ndev)
{
	return (ndev == c.ndev && c.carrier);
}

/* Linux's takes the Ethernet header off; this one leaves it, for netif_rx() below, and says the frame's type */
__be16
lx_eth_type_trans(struct sk_buff *skb, struct net_device *ndev)
{
	skb->dev = ndev;
	if (skb->len < ETH_HLEN) {
		return (0);
	}
	skb->pkt_type = !(skb->data[0] & 1) ? PACKET_OTHERHOST : is_broadcast_ether_addr(skb->data) ? PACKET_BROADCAST :
	    PACKET_MULTICAST;			/* (LinuxKPI's has no PACKET_HOST; brcmfmac only counts multicast) */
	return (*(__be16 *)(skb->data + 12));
}

/* a frame from the network the radio has joined, the Ethernet header still on (on the interrupt's thread) */
void
lx_netif_rx(struct sk_buff *skb)
{
	if (skb->len >= ETH_HLEN && skb->data[12] == 0x88 && skb->data[13] == 0x8e) {
		bool queued = false;

		lx_os_lock();
		if (c.offered && c.eapol_in - c.eapol_out < EAPOL_MOST && skb->len <= EAPOL_BYTES) {
			struct lx_eapol *e = &c.eapol[c.eapol_in % EAPOL_MOST];

			memcpy(e->bytes, skb->data, skb->len);
			e->length = (uint16_t)skb->len;
			c.eapol_in++;
			queued = true;
		}
		lx_os_unlock();
		if (queued) {
			queue_work(c.work, &c.eapol_work);
		}
	} else if (c.joined && skb->len >= ETH_HLEN) {
		c.received++;
		lx_os_net_input(skb->data, skb->len);
	}
	dev_kfree_skb(skb);
}

/* an Ethernet frame to brcmfmac's interface: 0, or an errno */
static int
frame_send(const uint8_t *frame, unsigned length)
{
	struct net_device *ndev = c.ndev;
	struct sk_buff *skb;
	unsigned head;

	if (ndev == NULL || !c.up || length < ETH_HLEN) {
		return (ENETDOWN);
	}
	head = (unsigned)ndev->needed_headroom + 64;	/* (room for brcmfmac's own header: it need not move the frame) */
	if ((skb = dev_alloc_skb(head + length)) == NULL) {
		return (ENOBUFS);
	}
	skb_reserve(skb, head);
	memcpy(skb_put(skb, length), frame, length);
	skb->dev = ndev;
	mutex_lock(&c.send);
	ndev->netdev_ops->ndo_start_xmit(skb, ndev);	/* (it takes the frame whatever it says) */
	mutex_unlock(&c.send);
	c.sent++;
	return (0);
}

/* lx_transmit(): the kernel's frames, once a network is joined */
int
lx_cfg_transmit(const unsigned char *frame, unsigned long length)
{
	if (!c.joined) {
		return (ENETDOWN);
	}
	return (frame_send(frame, (unsigned)length));
}

/* ---- scanning --------------------------------------------------------------------------------------------- */

static struct cfg80211_bss heard_one;		/* what brcmfmac is given back and gives back again (put_bss) */

/* one access point, from brcmfmac's escan results (cfg80211_inform_bss_data(), on its event work) */
struct cfg80211_bss *
lx_cfg80211_inform_bss(struct wiphy *wiphy, struct linuxkpi_ieee80211_channel *channel, int signal_mbm, const uint8_t *bssid,
    uint16_t capability, uint16_t interval, const uint8_t *ie, size_t ie_length)
{
	const uint8_t *name = lx_rsn_element(ie, (unsigned)ie_length, 0);
	struct hakios_sta_bss *bss = NULL;
	unsigned kept = 0;

	/* as many whole elements as there is room for */
	while (kept + 2 <= ie_length && kept + 2 + ie[kept + 1] <= ie_length && kept + 2 + ie[kept + 1] <= HAKIOS_STA_ELEMENTS_MOST) {
		kept += 2 + ie[kept + 1];
	}
	lx_os_lock();
	for (unsigned i = 0; c.heard != NULL && i < c.heard_count; i++) {
		if (memcmp(c.heard[i].bssid, bssid, ETH_ALEN) == 0) {
			bss = &c.heard[i];
		}
	}
	if (bss == NULL && c.heard != NULL && c.heard_count < HEARD_MOST) {
		bss = &c.heard[c.heard_count++];
	}
	if (bss != NULL) {
		bzero(bss, sizeof(*bss));
		memcpy(bss->bssid, bssid, ETH_ALEN);
		if (name != NULL && name[1] <= sizeof(bss->ssid)) {
			memcpy(bss->ssid, name + 2, name[1]);
			bss->ssid_length = name[1];
		}
		bss->frequency = channel != NULL ? (uint16_t)channel->center_freq : 0;
		bss->rssi = (int16_t)(signal_mbm / 100);
		bss->beacon_interval = interval;
		bss->capability = capability;
		bss->elements_length = (uint16_t)kept;
		memcpy(bss->elements, ie, kept);
		c.heard_at[bss - c.heard] = lx_os_nanoseconds();
	}
	lx_os_unlock();
	return (&heard_one);
}

void
lx_cfg80211_scan_done(struct cfg80211_scan_request *request, struct cfg80211_scan_info *info)
{
	lx_os_lock();
	c.scan_aborted = info != NULL && info->aborted;
	c.scan_over = 1;
	lx_os_wakeup(&c.scan_over);
	lx_os_unlock();
}

/*
 * A scan by brcmfmac's escan: every channel (or those named), asking (one SSID, the wildcard or the name) or only
 * listening. Returns when brcmfmac says it is over (its own timeout is 10 s); 0, or an errno.
 */
static int
station_scan(const struct hakios_sta_scan *what)
{
	static struct cfg80211_ssid ssid;
	struct cfg80211_scan_request *request;
	unsigned count = 0, n;
	int error;

	if (c.wiphy == NULL || c.ndev == NULL || !c.up) {
		return (ENETDOWN);
	}
	n = what->frequency_count <= 64 ? what->frequency_count : 64;
	request = kzalloc(sizeof(*request) + 64 * sizeof(request->channels[0]), GFP_KERNEL);
	if (request == NULL) {
		return (ENOMEM);
	}
	for (unsigned i = 0; i < n; i++) {
		struct linuxkpi_ieee80211_channel *channel = ieee80211_get_channel(c.wiphy, what->frequencies[i]);

		if (channel != NULL && !(channel->flags & IEEE80211_CHAN_DISABLED)) {
			request->channels[count++] = channel;
		}
	}
	request->n_channels = (int)count;	/* (0: all, the firmware's way) */
	request->wiphy = c.wiphy;
	request->wdev = c.ndev->ieee80211_ptr;
	if (!what->passive) {
		bzero(&ssid, sizeof(ssid));
		ssid.ssid_len = what->ssid_length <= sizeof(ssid.ssid) ? what->ssid_length : 0;
		memcpy(ssid.ssid, what->ssid, (size_t)ssid.ssid_len);
		request->ssids = &ssid;
		request->n_ssids = 1;
	}
	lx_os_lock();
	c.scan_over = 0;
	c.heard_count = 0;
	lx_os_unlock();
	if ((error = c.ops->scan(c.wiphy, request)) != 0) {
		kfree(request);
		return (error < 0 ? -error : error);
	}
	lx_os_lock();
	for (unsigned long long until = lx_os_nanoseconds() + 15000000000ull; !c.scan_over && lx_os_nanoseconds() < until;) {
		lx_os_sleep(&c.scan_over, 100000000ull);
	}
	error = !c.scan_over ? ETIMEDOUT : c.scan_aborted ? EIO : 0;
	lx_os_unlock();
	if (error != ETIMEDOUT) {
		kfree(request);			/* (else brcmfmac may still hold it) */
	}
	return (error);
}

/* ---- the interface ------------------------------------------------------------------------------------------ */

/* brcmf_netdev_open(): the firmware's "up" (brcmf_cfg80211_up()) */
static int
station_up(void)
{
	int error;

	if (c.up) {
		return (0);
	}
	if (c.ndev == NULL || c.wiphy == NULL) {
		return (ENXIO);
	}
	if ((error = c.ndev->netdev_ops->ndo_open(c.ndev)) != 0) {
		lx_tell("lx: the interface does not come up: %d\n", error);
		return (error < 0 ? -error : error);
	}
	c.ndev->flags |= IFF_UP;
	c.up = true;
	lx_tell("lx: the interface %s is up\n", c.ndev->name);
	return (0);
}

static void
station_leave(void)
{
	if (!c.joined && !c.joining) {
		return;
	}
	lx_os_lock();
	c.leaving = true;
	c.connected = 0;
	lx_os_unlock();
	c.ops->disconnect(c.wiphy, c.ndev, WLAN_REASON_DEAUTH_LEAVING);
	lx_os_lock();
	for (unsigned long long until = lx_os_nanoseconds() + 2000000000ull; c.connected == 0 && lx_os_nanoseconds() < until;) {
		lx_os_sleep(&c.connected, 100000000ull);
	}
	if (c.joined) {
		c.joined = false;
		lx_os_unlock();
		lx_os_net_link(0);
		lx_os_lock();
	}
	c.joining = false;
	c.leaving = false;
	lx_os_unlock();
	lx_tell("lx: the network is left: %llu frames sent, %llu received\n", c.sent, c.received);
}

static void
station_down(void)
{
	station_leave();
	if (c.up && c.ndev != NULL) {
		c.ndev->netdev_ops->ndo_stop(c.ndev);
		c.ndev->flags &= ~IFF_UP;
		lx_tell("lx: the interface is down\n");
	}
	c.up = false;
}

/* ---- joining ---------------------------------------------------------------------------------------------- */

void
lx_cfg80211_connect_done(struct net_device *ndev, struct cfg80211_connect_resp_params *params)
{
	lx_os_lock();
	c.status = params->status;
	c.connected = params->status == 0 ? 1 : -1;
	lx_os_wakeup(&c.connected);
	lx_os_unlock();
}

void
lx_cfg80211_disconnected(struct net_device *ndev, uint16_t reason, bool locally_generated)
{
	bool lost;

	lx_os_lock();
	lost = c.joined && !c.leaving;
	if (c.joined && lost) {
		c.joined = false;
	}
	c.connected = -1;
	c.last_error = lost ? ECONNRESET : c.last_error;
	lx_os_wakeup(&c.connected);
	lx_os_wakeup(&c.handshake);
	lx_os_unlock();
	lx_tell("lx: the network is gone (reason %u%s)%s\n", reason, locally_generated ? ", ours" : "",
	    lost ? ": NOT ASKED FOR" : "");
	if (lost) {
		lx_os_net_link(0);
		lx_os_station_lost(ECONNRESET);
	}
}

void
lx_cfg80211_roamed(struct net_device *ndev, struct cfg80211_roam_info *info)
{
	lx_tell("lx: THE FIRMWARE HAS ROAMED, although roaming is off: the handshake's keys are for the old access point\n");
}

/* the keys the handshake has agreed, to the firmware: the pairwise one (CCMP) if asked, and the group's */
static int
keys_give(bool pairwise)
{
	struct key_params key;
	int error = 0;

	if (pairwise) {
		bzero(&key, sizeof(key));
		key.key = c.wpa.pairwise;
		key.key_len = 16;
		key.cipher = WLAN_CIPHER_SUITE_CCMP;
		error = c.ops->add_key(c.wiphy, c.ndev, -1, 0, true, c.target.bssid, &key);
	}
	if (error == 0) {
		bzero(&key, sizeof(key));
		key.key = c.wpa.group;
		key.key_len = (int)c.wpa.group_length;
		key.seq = c.wpa.group_counter;
		key.seq_len = 6;
		key.cipher = c.group == LX_RSN_TKIP ? WLAN_CIPHER_SUITE_TKIP : WLAN_CIPHER_SUITE_CCMP;
		error = c.ops->add_key(c.wiphy, c.ndev, -1, (uint8_t)c.wpa.group_index, false, NULL, &key);
	}
	lx_tell("lx: the handshake: %s given to the firmware: %d\n", pairwise ? "both keys" : "the group's new key", error);
	return (error);
}

/* a message of the handshake from the access point, answered (on our work queue) */
static void
handshake_message(const uint8_t *frame, unsigned length)
{
	uint8_t reply[14 + LX_WPA_REPLY_MOST];
	unsigned reply_length = 0;
	const char *why = "";
	int said, error = 0;

	c.eapol_count++;
	if (!c.protected || (!c.joining && !c.joined) || memcmp(frame + 6, c.target.bssid, ETH_ALEN) != 0) {
		return;
	}
	said = lx_wpa_message(&c.wpa, frame + 14, length - 14, reply + 14, &reply_length, &why);
	if (reply_length != 0) {
		memcpy(reply, c.target.bssid, ETH_ALEN);
		memcpy(reply + 6, c.wpa.own, ETH_ALEN);
		reply[12] = 0x88;
		reply[13] = 0x8e;
		frame_send(reply, 14 + reply_length);
	}
	lx_tell("lx: the handshake: a message of %u bytes from the access point: %s%s\n", length - 14, said == LX_WPA_ANSWER ?
	    "its first; answered with ours (message 2)" : said == LX_WPA_KEYS ? "its second, SIGNED WITH THE KEY WE HAVE TOO, and "
	    "with the group's key; answered (message 4)" : said == LX_WPA_GROUP ? "a new key for the group; answered" :
	    said == LX_WPA_BAD ? "NOT GOOD: " : "not one to answer", why);
	if (said == LX_WPA_KEYS || said == LX_WPA_GROUP) {
		error = keys_give(said == LX_WPA_KEYS);
	}
	if (said != LX_WPA_NOTHING) {
		lx_os_lock();
		/* (a verified message 3 is not hidden by a later frame anybody in range could send: lx_80211.c says why) */
		if (c.handshake != LX_WPA_KEYS) {
			c.handshake = said;
			c.why = why;
		}
		if (said == LX_WPA_KEYS) {
			c.keys = error == 0 ? 1 : -1;
		}
		lx_os_wakeup(&c.handshake);
		lx_os_unlock();
	}
}

static void
eapol_runs(struct work_struct *work)
{
	static struct lx_eapol e;		/* (one work item: never two at once) */

	for (;;) {
		lx_os_lock();
		if (c.eapol_out == c.eapol_in) {
			lx_os_unlock();
			return;
		}
		e = c.eapol[c.eapol_out % EAPOL_MOST];
		c.eapol_out++;
		lx_os_unlock();
		handshake_message(e.bytes, e.length);
	}
}

/* the access point of that name (and address, if one is given) heard best by the last scan: true, into c.target */
static bool
target_find(const struct hakios_sta_join *what)
{
	static const uint8_t anyone[ETH_ALEN];
	int best = -1;

	lx_os_lock();
	for (unsigned i = 0; i < c.heard_count; i++) {
		const struct hakios_sta_bss *bss = &c.heard[i];

		if (bss->ssid_length == what->ssid_length && memcmp(bss->ssid, what->ssid, what->ssid_length) == 0 &&
		    (memcmp(what->bssid, anyone, ETH_ALEN) == 0 || memcmp(what->bssid, bss->bssid, ETH_ALEN) == 0) &&
		    (best < 0 || bss->rssi > c.heard[best].rssi)) {
			best = (int)i;
		}
	}
	if (best >= 0) {
		c.target = c.heard[best];
	}
	lx_os_unlock();
	return (best >= 0);
}

static int
wait_for(volatile int *what, bool (*done)(void), unsigned ms)
{
	unsigned long long until = lx_os_nanoseconds() + ms * 1000000ull;

	lx_os_lock();
	while (!done() && lx_os_nanoseconds() < until) {
		lx_os_sleep((const void *)what, 100000000ull);
	}
	lx_os_unlock();
	return (done() ? 0 : ETIMEDOUT);
}

static bool connect_over(void) { return (c.connected != 0); }
static bool handshake_over(void) { return (c.connected < 0 || (c.handshake == LX_WPA_KEYS && c.keys != 0)); }

static int
station_join(const struct hakios_sta_join *what)
{
	static uint32_t pairwise = WLAN_CIPHER_SUITE_CCMP, akm = WLAN_AKM_SUITE_PSK;
	struct cfg80211_connect_params sme;
	unsigned long long began;
	const char *refused;
	int error, kind;

	if (!lx_brcmf_reached(8)) {
		lx_brcmf_may(8, "joining a network");
		return (EPERM);
	}
	if ((error = station_up()) != 0) {
		return (error);
	}
	station_leave();
	if (what->ssid_length == 0 || what->ssid_length > 32) {
		return (EINVAL);
	}
	if (!target_find(what)) {
		struct hakios_sta_scan scan;

		bzero(&scan, sizeof(scan));
		memcpy(scan.ssid, what->ssid, what->ssid_length);
		scan.ssid_length = what->ssid_length;
		lx_tell("lx: the network asked for was not heard by the last scan: a scan for it\n");
		if ((error = station_scan(&scan)) != 0 && error != EIO) {
			return (error);
		}
		if (!target_find(what)) {
			lx_tell("lx: the network asked for (a name of %u bytes) is not heard\n", what->ssid_length);
			return (ENOENT);
		}
	}
	kind = lx_rsn_kind(c.target.elements, c.target.elements_length, c.target.capability);
	c.protected = kind != LX_RSN_OPEN;
	refused = kind == LX_RSN_OLDER ? "it is protected by something older than WPA2" : NULL;
	if (refused == NULL && c.protected) {
		refused = lx_rsn_choose(c.target.elements, c.target.elements_length, c.rsn, &c.group);
	}
	if (refused == NULL && c.protected && what->security != HAKIOS_STA_WPA2_PSK) {
		refused = "it is protected and no passphrase was given";
	}
	if (refused != NULL) {
		lx_tell("lx: the access point on %u MHz cannot be joined: %s\n", c.target.frequency, refused);
		return (EOPNOTSUPP);
	}
	lx_tell("lx: brcmfmac 8: joining the network (a name of %u bytes) through %02x:%02x:%02x:..:..:.. on %u MHz, %d dBm, %s\n",
	    what->ssid_length, c.target.bssid[0], c.target.bssid[1], c.target.bssid[2], c.target.frequency, c.target.rssi,
	    !c.protected ? "open" : c.group == LX_RSN_TKIP ? "WPA2, CCMP and the group's TKIP" : "WPA2, CCMP");
	if (c.protected) {
		bzero(&c.wpa, sizeof(c.wpa));
		if (what->passphrase_length != 0) {
			lx_wpa_pmk(what->passphrase, what->passphrase_length, what->ssid, what->ssid_length, c.wpa.pmk);
		} else {
			memcpy(c.wpa.pmk, what->pmk, sizeof(c.wpa.pmk));
		}
		memcpy(c.wpa.own, c.ndev->dev_addr, ETH_ALEN);
		memcpy(c.wpa.peer, c.target.bssid, ETH_ALEN);
		lx_os_random(c.wpa.own_nonce, sizeof(c.wpa.own_nonce));
		c.wpa.rsn = c.rsn;
		c.wpa.rsn_length = LX_RSN_OURS_LENGTH;
	}
	lx_os_lock();
	c.handshake = LX_WPA_NOTHING;
	c.why = "";
	c.keys = 0;
	c.connected = 0;
	c.eapol_out = c.eapol_in;
	c.joining = true;
	lx_os_unlock();

	bzero(&sme, sizeof(sme));
	sme.channel = ieee80211_get_channel(c.wiphy, c.target.frequency);
	sme.bssid = c.target.bssid;
	sme.ssid = c.target.ssid;
	sme.ssid_len = c.target.ssid_length;
	sme.auth_type = NL80211_AUTHTYPE_OPEN_SYSTEM;
	sme.privacy = c.protected;
	if (c.protected) {
		sme.ie = c.rsn;
		sme.ie_len = LX_RSN_OURS_LENGTH;
		sme.crypto.wpa_versions = NL80211_WPA_VERSION_2;
		sme.crypto.cipher_group = c.group == LX_RSN_TKIP ? WLAN_CIPHER_SUITE_TKIP : WLAN_CIPHER_SUITE_CCMP;
		sme.crypto.ciphers_pairwise = &pairwise;
		sme.crypto.n_ciphers_pairwise = 1;
		sme.crypto.akm_suites = &akm;
		sme.crypto.n_akm_suites = 1;
		/* (no crypto.psk: the handshake is ours, not the firmware's; the comment at the top says why) */
	}
	began = lx_os_nanoseconds();
	if ((error = c.ops->connect(c.wiphy, c.ndev, &sme)) != 0) {
		lx_tell("lx: brcmfmac's connect refuses: %d\n", error);
		c.joining = false;
		return (error < 0 ? -error : error);
	}
	if (wait_for(&c.connected, connect_over, 15000) != 0 || c.connected < 0) {
		lx_tell("lx: THE NETWORK IS NOT JOINED: %s (status %d) after %llu ms\n", c.connected == 0 ? "no association" :
		    "the association is refused or lost", c.status, (lx_os_nanoseconds() - began) / 1000000ull);
		station_leave();
		return (c.connected == 0 ? ETIMEDOUT : ECONNREFUSED);
	}
	lx_tell("lx: associated, %llu ms after brcmfmac's connect%s\n", (lx_os_nanoseconds() - began) / 1000000ull,
	    c.protected ? "; the handshake follows" : "");
	if (c.protected && (wait_for(&c.handshake, handshake_over, 10000) != 0 || c.connected < 0 || c.keys != 1)) {
		lx_tell("lx: THE NETWORK IS NOT JOINED: the handshake %s (%s; %llu messages)\n", c.connected < 0 ? "was cut off" :
		    c.keys < 0 ? "agreed keys the firmware refused" : "did not finish", c.why, c.eapol_count);
		station_leave();
		return (c.keys < 0 ? EIO : ETIMEDOUT);
	}
	lx_os_lock();
	c.joining = false;
	c.joined = true;
	lx_os_unlock();
	lx_os_net_link(1);
	lx_tell("lx: THE NETWORK IS JOINED, %llu ms after brcmfmac's connect: frames pass\n", (lx_os_nanoseconds() - began) /
	    1000000ull);
	return (0);
}

/* ---- the station boundary: what the kernel asks through sys/hakios.sta.h ------------------------------------- */

static int
boundary_power(bool on)
{
	if (on) {
		return (station_up());
	}
	station_down();
	return (0);
}

static int
boundary_scan(const struct hakios_sta_scan *what)
{
	static unsigned said;
	unsigned long long began = lx_os_nanoseconds();
	int error;

	if ((error = station_up()) != 0) {
		return (error);
	}
	error = station_scan(what);
	if (said < 24) {			/* (the first few: iOS goes on asking as long as the system runs) */
		said++;
		lx_tell("lx: a scan for the kernel: %u channels named (0: all), %s: %d; %u access points heard in %llu ms\n",
		    what->frequency_count, what->passive ? "listening" : "asking", error, c.heard_count,
		    (lx_os_nanoseconds() - began) / 1000000ull);
	}
	return (error);
}

static void
boundary_heard(void (*each)(const struct hakios_sta_bss *, void *), void *context)
{
	static struct hakios_sta_bss bss;	/* (one thread calls this) */

	for (unsigned i = 0; i < c.heard_count; i++) {
		lx_os_lock();
		bss = c.heard[i];
		bss.age_ms = (uint32_t)((lx_os_nanoseconds() - c.heard_at[i]) / 1000000ull);
		lx_os_unlock();
		each(&bss, context);
	}
}

static int
boundary_join(const struct hakios_sta_join *what)
{
	int error = station_join(what);

	c.last_error = error;
	return (error);
}

static int
boundary_leave(void)
{
	station_leave();
	c.last_error = 0;
	return (0);
}

/* (on any thread: only reading) */
static void
boundary_state(struct hakios_sta_state *state)
{
	bzero(state, sizeof(*state));
	if (!c.offered) {
		return;
	}
	lx_os_lock();
	state->phase = !c.up ? HAKIOS_STA_OFF : c.joined ? HAKIOS_STA_JOINED : c.joining ? HAKIOS_STA_JOINING : c.scan_over == 0 ?
	    HAKIOS_STA_SCANNING : HAKIOS_STA_IDLE;
	state->last_error = c.last_error;
	if (c.joined || c.joining) {
		state->bss = c.target;
	}
	lx_os_unlock();
}

static unsigned
boundary_channels(struct hakios_sta_channel *channels, unsigned most)
{
	unsigned count = 0;

	for (unsigned band = 0; c.wiphy != NULL && band < NUM_NL80211_BANDS; band++) {
		const struct ieee80211_supported_band *has = c.wiphy->bands[band];

		for (int i = 0; has != NULL && i < has->n_channels && count < most; i++) {
			if (!(has->channels[i].flags & IEEE80211_CHAN_DISABLED)) {
				channels[count].frequency = (uint16_t)has->channels[i].center_freq;
				channels[count++].flags = (uint16_t)((has->channels[i].flags & IEEE80211_CHAN_NO_IR ?
				    HAKIOS_STA_CHANNEL_PASSIVE : 0) | (has->channels[i].flags & IEEE80211_CHAN_RADAR ?
				    HAKIOS_STA_CHANNEL_RADAR : 0));
			}
		}
	}
	return (count);
}

static struct hakios_sta_ops boundary = {
	.power = boundary_power,
	.scan = boundary_scan,
	.heard = boundary_heard,
	.join = boundary_join,
	.leave = boundary_leave,
	.state = boundary_state,
	.channels = boundary_channels,
};

/* ---- from brcmfmac's start to the kernel's station ------------------------------------------------------------ */

/* brcmfmac=7, once brcmf_attach() is through (lx_brcmf_attached()): the station, offered to the kernel */
void
lx_cfg_begin(void)
{
	int error;

	if (!lx_brcmf_may(7, "the station: the interface up, en0 on it, the kernel's to ask")) {
		return;
	}
	c.heard = kzalloc(HEARD_MOST * sizeof(*c.heard), GFP_KERNEL);
	c.heard_at = kzalloc(HEARD_MOST * sizeof(*c.heard_at), GFP_KERNEL);
	c.eapol = kzalloc(EAPOL_MOST * sizeof(*c.eapol), GFP_KERNEL);
	c.work = alloc_workqueue("lx-eapol", 0, 1);
	if (c.heard == NULL || c.heard_at == NULL || c.eapol == NULL || c.work == NULL || c.ndev == NULL || c.wiphy == NULL) {
		lx_tell("lx: no memory for the station, or no interface: none is offered\n");
		return;
	}
	mutex_init(&c.send);
	INIT_WORK(&c.eapol_work, eapol_runs);
	c.scan_over = 1;
	lx_os_net_attach(c.ndev->dev_addr);
	if ((error = station_up()) != 0) {
		return;
	}
	memcpy(boundary.address, c.ndev->dev_addr, ETH_ALEN);
	c.offered = true;
	lx_os_station(&boundary);
	lx_tell("lx: the station is the kernel's to ask: scans%s\n", lx_brcmf_reached(8) ? " and joining" :
	    "; not joining (brcmfmac=8)");
}

/* lx_stop(): the station taken back (when no call of the kernel's is under way), the network left, the interface down */
void
lx_cfg_stop(void)
{
	if (c.offered) {
		lx_os_station(NULL);
		c.offered = false;
	}
	station_down();
}
