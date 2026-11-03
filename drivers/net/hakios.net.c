/*
 * en0: a network interface that is Ethernet as far as the rest of the kernel and Darwin can tell.
 *
 * Darwin expects its interfaces from IONetworkingFamily, which this kernel does not have; the BSD
 * half of the kernel does not need it. The interface is made with the kernel's own interface
 * KPI, the way bsd/net/if_fake.c makes its "feth": family Ethernet, the stock Ethernet framing
 * and demultiplexing, one function that is given a frame to send and one call to hand a frame
 * that has arrived to the stack. What Darwin's user space makes of an interface that has no
 * IOKit object behind it is another matter: see userland/src/net_test.m.
 *
 * What carries the frames is a link driver's business; sys/hakios.net.h is all that the two
 * know of each other. The first link is QEMU's virtio network device (hakios.virtio_net.c).
 * There can be two interfaces: en0 for the Wi-Fi (or virtio), en1 for the modem's packets
 * through the IPA (hakios.cell.c); each has its link, its state and its thread.
 */
#include <stdarg.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <netinet/in.h>
#include <pexpert/pexpert.h>
#include <sys/kern_event.h>
#include <sys/kpi_mbuf.h>
#include <sys/proc.h>
#include <sys/hakios.net.h>
#include <sys/sockio.h>
#include <sys/systm.h>

extern void HakiosWLANStart(const char *interface, const uint8_t *address);  /* drivers/wifi/hakios.wlan.cpp */

static LCK_GRP_DECLARE(net_lck_grp, "hakios-net");

/* One interface: enN on its link. */
struct net_if {
	unsigned        unit;
	const struct hakios_net_link *link;
	bool            up;             /* the link says frames can pass */
	ifnet_t         ifp;
	lck_mtx_t       tx_lock;        /* one frame at a time to the link */
	/* What has gone through: said once a minute while anything moves. */
	struct {
		uint64_t sent, received, sent_bytes, received_bytes, dropped_out, dropped_in;
	} count;
};
static struct net_if net_ifs[HAKIOS_NET_UNITS];

static inline struct net_if *
net_if_of(ifnet_t ifp)
{
	return ifnet_softc(ifp);
}

/* The interface on unit `unit`, once attached; NULL before. */
static struct net_if *
net_if_unit(unsigned unit)
{
	return unit < HAKIOS_NET_UNITS && net_ifs[unit].ifp != NULL ? &net_ifs[unit] : NULL;
}

/*
 * Boot-arg net_trace=N: a line in the log for each of the first N frames, either way.
 * Boot-arg net_names=1: a line for every name asked of a DNS server and for every TCP connection
 * begun from here -- which is how to see what a system that has just been given a network does
 * with it (up to NAMES_MAX lines).
 */
#define NAMES_MAX       3000
static uint32_t net_trace, net_names;

static void
trace_frame(const char *way, const uint8_t *frame, size_t length, size_t whole)
{
	static uint32_t said, names;
	const uint8_t *end = frame + length, *ip = frame + ETHER_HDR_LEN, *l4;
	uint16_t type;
	bool ports;

	if (length < ETHER_HDR_LEN + 28 || (said >= net_trace && (!net_names || names >= NAMES_MAX))) {
		return;
	}
	type = (uint16_t)(frame[12] << 8 | frame[13]);
	l4 = ip + 4 * (ip[0] & 0xf);
	ports = type == ETHERTYPE_IP && (ip[9] == IPPROTO_TCP || ip[9] == IPPROTO_UDP) && l4 + 4 <= end;
	if (net_names && names < NAMES_MAX && ports && way[0] == 'o') {
		if (ip[9] == IPPROTO_UDP && (l4[2] << 8 | l4[3]) == 53 && l4 + 8 + 12 < end) {
			char name[128];
			const uint8_t *label = l4 + 8 + 12;
			size_t n = 0;

			/* the question: lengths and labels, up to a zero */
			while (label < end && *label != 0 && *label < 64 && label + 1 + *label < end && n + *label + 2 < sizeof(name)) {
				memcpy(name + n, label + 1, *label);
				n += *label;
				name[n++] = '.';
				label += 1 + *label;
			}
			name[n ? n - 1 : 0] = 0;
			names++;
			printf("net: asks for the name %s (type %u)\n", name, label + 2 < end ? label[1] << 8 | label[2] : 0);
		} else if (ip[9] == IPPROTO_TCP && l4 + 14 <= end && (l4[13] & 0x12) == 0x02) {
			names++;
			printf("net: connects to %u.%u.%u.%u port %u\n", ip[16], ip[17], ip[18], ip[19], l4[2] << 8 | l4[3]);
		}
	}
	if (said >= net_trace) {
		return;
	}
	said++;
	if (type == ETHERTYPE_IP) {
		printf("net: %s %zu bytes: IPv4 %s %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u%s\n", way, whole, ip[9] == IPPROTO_TCP ? "TCP" :
		    ip[9] == IPPROTO_UDP ? "UDP" : ip[9] == IPPROTO_ICMP ? "ICMP" : "protocol?", ip[12], ip[13], ip[14], ip[15],
		    ports ? l4[0] << 8 | l4[1] : 0, ip[16], ip[17], ip[18], ip[19], ports ? l4[2] << 8 | l4[3] : 0,
		    ip[9] == IPPROTO_TCP && ports && l4 + 14 <= end ? (l4[13] & 2 ? " SYN" : l4[13] & 1 ? " FIN" : l4[13] & 4 ? " RST" : "") : "");
	} else {
		printf("net: %s %zu bytes: %s (type %#x) to %02x:%02x:%02x:%02x:%02x:%02x\n", way, whole, type == ETHERTYPE_ARP ? "ARP" :
		    type == ETHERTYPE_IPV6 ? "IPv6" : "?", type, frame[0], frame[1], frame[2], frame[3], frame[4], frame[5]);
	}
}

/* ... of a frame on its way out: the start of it is enough */
static void
trace_packet(mbuf_t packet, size_t length)
{
	uint8_t start[192];
	size_t have = length < sizeof(start) ? length : sizeof(start);

	if ((net_trace || net_names) && mbuf_copydata(packet, 0, have, start) == 0) {
		trace_frame("out", start, have, length);
	}
}

/* ---- the interface -------------------------------------------------------------------------------- */

/* The stack has a frame to send (a chain of mbufs that starts with the Ethernet header). */
static errno_t
net_output(ifnet_t ifp, mbuf_t packet)
{
	struct net_if *n = net_if_of(ifp);
	size_t length = mbuf_pkthdr_len(packet);
	bool sent = false;

	if (length <= HAKIOS_NET_FRAME_MAX && n->up) {
		bpf_tap_out(ifp, DLT_EN10MB, packet, NULL, 0);
		trace_packet(packet, length);
		lck_mtx_lock(&n->tx_lock);
		sent = n->link->transmit(packet, length);
		lck_mtx_unlock(&n->tx_lock);
	}
	if (sent) {
		n->count.sent++;
		n->count.sent_bytes += length;
		ifnet_stat_increment_out(ifp, 1, (uint32_t)length, 0);
	} else {
		n->count.dropped_out++;
		ifnet_stat_increment_out(ifp, 0, 0, 1);
	}
	mbuf_freem(packet);
	return 0;
}

/* A frame has arrived on unit `unit`. */
void
hakios_net_input_unit(unsigned unit, const uint8_t *frame, size_t length)
{
	struct net_if *n = net_if_unit(unit);
	struct ifnet_stat_increment_param stats = { .packets_in = 1, .bytes_in = (uint32_t)length };
	unsigned int chunks = 1;
	mbuf_t packet = NULL;
	char *header;

	if (n == NULL) {
		return;
	}
	trace_frame("in ", frame, length, length);
	if (length < ETHER_HDR_LEN || length > HAKIOS_NET_FRAME_MAX || mbuf_allocpacket(MBUF_DONTWAIT, length, &chunks, &packet) != 0) {
		n->count.dropped_in++;
		return;
	}
	if (mbuf_copyback(packet, 0, length, frame, MBUF_DONTWAIT) != 0) {
		mbuf_freem(packet);
		n->count.dropped_in++;
		return;
	}
	/* as every Ethernet driver hands a frame up: the header noted and stepped over */
	header = mbuf_data(packet);
	mbuf_pkthdr_setrcvif(packet, n->ifp);
	mbuf_pkthdr_setheader(packet, header);
	mbuf_pkthdr_adjustlen(packet, -ETHER_HDR_LEN);
	mbuf_setdata(packet, header + ETHER_HDR_LEN, length - ETHER_HDR_LEN);
	bpf_tap_in(n->ifp, DLT_EN10MB, packet, header, ETHER_HDR_LEN);
	n->count.received++;
	n->count.received_bytes += length;
	ifnet_input(n->ifp, packet, &stats);
}

void
hakios_net_input(const uint8_t *frame, size_t length)
{
	hakios_net_input_unit(0, frame, length);
}

static unsigned net_ioctls_said;

static errno_t
net_ioctl(ifnet_t ifp, u_long command, void *data)
{
	struct net_if *n = net_if_of(ifp);
	/* (a radio says it is one here too: IO80211.framework lists the interfaces whose media is 802.11) */
	const int media[] = { (n->link->wireless ? IFM_IEEE80211 : IFM_ETHER) | IFM_AUTO };
	struct ifreq *request = data;
	struct ifmediareq *media_request = data;
	user_addr_t list;
	errno_t error = 0;

	if (n->link->wireless && net_ioctls_said < 80) {
		char who[32] = "?";

		/* (while Darwin's Wi-Fi side is being got to talk to this: who asks the interface what) */
		proc_selfname(who, sizeof(who));
		if (strcmp(who, "configd") != 0) {
			net_ioctls_said++;
			printf("net: en0 is asked %#lx by %s\n", command, who);
		}
	}
	switch (command) {
	case SIOCSIFADDR:
		ifnet_set_flags(ifp, IFF_UP, IFF_UP);
		break;
	case SIOCSIFFLAGS:
		/* running whenever it is up: the link is there or it is not, nothing is switched on */
		ifnet_set_flags(ifp, (ifnet_flags(ifp) & IFF_UP) ? IFF_RUNNING : 0, IFF_RUNNING);
		break;
	case SIOCGIFMEDIA32:
	case SIOCGIFMEDIA64:
		/* what configd asks to learn whether the cable is in */
		list = command == SIOCGIFMEDIA64 ? ((struct ifmediareq64 *)media_request)->ifmu_ulist :
		    CAST_USER_ADDR_T(((struct ifmediareq32 *)media_request)->ifmu_ulist);
		media_request->ifm_active = media_request->ifm_current = media[0];
		media_request->ifm_mask = 0;
		media_request->ifm_status = IFM_AVALID | (n->up ? IFM_ACTIVE : 0);
		if (list != USER_ADDR_NULL && media_request->ifm_count > 0) {
			error = copyout(media, list, sizeof(media));
		}
		media_request->ifm_count = 1;
		break;
	case SIOCGIFDEVMTU:
		request->ifr_devmtu.ifdm_current = ifnet_mtu(ifp);
		request->ifr_devmtu.ifdm_max = ETHERMTU;
		request->ifr_devmtu.ifdm_min = IF_MINMTU;
		break;
	case SIOCSIFMTU:
		error = request->ifr_mtu > ETHERMTU || request->ifr_mtu < IF_MINMTU ? EINVAL : ifnet_set_mtu(ifp, request->ifr_mtu);
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;                          /* the link hands us everything anyway */
	default:
		/* what Darwin's Wi-Fi side asks of a radio's interface: drivers/wifi/hakios.apple80211.c */
		error = n->link->wireless ? hakios_apple80211_ioctl(command, data) : EOPNOTSUPP;
		break;
	}
	return error;
}

/* Tell whoever listens for kernel events (configd does) that the link is up, or down. */
static void
net_link_event(ifnet_t ifp, uint32_t code)
{
	_Alignas(struct kern_event_msg) char message[sizeof(struct kern_event_msg) + sizeof(struct net_event_data)] = { 0 };
	struct kern_event_msg *header = (struct kern_event_msg *)message;
	struct net_event_data *about = (struct net_event_data *)(header + 1);

	header->total_size = sizeof(message);
	header->vendor_code = KEV_VENDOR_APPLE;
	header->kev_class = KEV_NETWORK_CLASS;
	header->kev_subclass = KEV_DL_SUBCLASS;
	header->event_code = code;
	about->if_family = ifnet_family(ifp);
	about->if_unit = ifnet_unit(ifp);
	strlcpy(about->if_name, ifnet_name(ifp), IFNAMSIZ);
	ifnet_event(ifp, header);
}

void
hakios_net_link_state_unit(unsigned unit, bool up)
{
	struct net_if *n = net_if_unit(unit);

	if (n == NULL || up == n->up) {
		return;
	}
	n->up = up;
	printf("net: en%u's link (%s) is %s\n", n->unit, n->link->name, up ? "up" : "down");
	net_link_event(n->ifp, up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF);
}

void
hakios_net_link_state(bool up)
{
	hakios_net_link_state_unit(0, up);
}

/* The interface's own thread: for a link that wants to be asked, and for the minute's line in the log. */
static void
net_thread(void *parameter, __unused wait_result_t result)
{
	struct net_if *n = parameter;
	uint64_t said = 0, total;
	unsigned quiet = 0;

	for (unsigned tick = 1;; tick++) {
		quiet = n->link->poll != NULL && n->link->poll() ? 0 : quiet + 1;
		/* about once a minute, if anything has moved since */
		total = n->count.sent + n->count.received + n->count.dropped_out + n->count.dropped_in;
		if (tick % 8192 == 0 && total != said) {
			said = total;
			printf("net: en%u so far %llu frames sent (%llu bytes), %llu received (%llu bytes); dropped: %llu out, %llu in\n",
			    n->unit, n->count.sent, n->count.sent_bytes, n->count.received, n->count.received_bytes,
			    n->count.dropped_out, n->count.dropped_in);
		}
		delay_for_interval(quiet < 100 ? 2 : 10, NSEC_PER_MSEC);
	}
}

bool
hakios_net_attach(const struct hakios_net_link *link)
{
	unsigned unit = link->unit;
	struct net_if *n = unit < HAKIOS_NET_UNITS ? &net_ifs[unit] : NULL;
	char uniqueid[16];
	struct ifnet_init_eparams init = {
		.ver = IFNET_INIT_CURRENT_VERSION,
		.len = sizeof(init),
		/*
		 * The simple model: every frame straight to net_output(). And no Skywalk "nexus" made for
		 * the interface by itself: sockets are all that is asked of it so far.
		 */
		.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO,
		.uniqueid = uniqueid,
		.uniqueid_len = 11,
		.name = "en",
		.unit = unit,
		.softc = n,
		.family = IFNET_FAMILY_ETHERNET,
		.subfamily = link->wireless ? IFNET_SUBFAMILY_WIFI : IFNET_SUBFAMILY_ANY,
		.type = IFT_ETHER,
		.output = net_output,
		.demux = ether_demux,
		.add_proto = ether_add_proto,
		.del_proto = ether_del_proto,
		.check_multi = ether_check_multi,
		.framer_extended = ether_frameout_extended,
		.ioctl = net_ioctl,
		.broadcast_addr = etherbroadcastaddr,
		.broadcast_len = ETHER_ADDR_LEN,
	};
	const uint8_t *address = link->address;
	ifnet_t ifp = NULL;
	thread_t thread;
	errno_t error;

	if (n == NULL) {
		printf("net: there is no unit %u for %s\n", unit, link->name);
		return false;
	}
	if (n->link != NULL) {
		printf("net: en%u is on %s already; %s is not attached\n", unit, n->link->name, link->name);
		return false;
	}
	snprintf(uniqueid, sizeof(uniqueid), "hakios-en%u", unit);
	n->unit = unit;
	n->link = link;
	n->up = link->up;
	lck_mtx_init(&n->tx_lock, &net_lck_grp, LCK_ATTR_NULL);
	error = ifnet_allocate_extended(&init, &ifp);
	if (error != 0) {
		printf("net: the interface en%u cannot be made: error %d\n", unit, error);
		n->link = NULL;
		return false;
	}
	ifnet_set_capabilities_enabled(ifp, 0, -1);
	ifnet_set_addrlen(ifp, ETHER_ADDR_LEN);
	ifnet_set_baudrate(ifp, 100 * 1000 * 1000);
	ifnet_set_mtu(ifp, ETHERMTU);
	ifnet_set_flags(ifp, IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX, 0xffff);
	ifnet_set_hdrlen(ifp, ETHER_HDR_LEN);
	ifnet_set_offload(ifp, 0);
	error = ifnet_attach(ifp, NULL);
	if (error != 0) {
		printf("net: the interface en%u cannot be attached: error %d\n", unit, error);
		ifnet_release(ifp);
		n->link = NULL;
		return false;
	}
	ifnet_set_lladdr(ifp, address, ETHER_ADDR_LEN);
	bpfattach(ifp, DLT_EN10MB, ETHER_HDR_LEN);
	n->ifp = ifp;
	if (n->up) {
		net_link_event(ifp, KEV_DL_LINK_ON);
	}
	if (link->wireless) {
		/* (a radio's address is the phone's own: not for the log; and Darwin's Wi-Fi side finds the interface by a service) */
		printf("net: en%u attached on %s (a radio), link %s\n", unit, link->name, n->up ? "up" : "down");
		HakiosWLANStart(uniqueid + 7, address);
	} else {
		printf("net: en%u attached on %s, address %02x:%02x:%02x:%02x:%02x:%02x, link %s\n", unit, link->name, address[0],
		    address[1], address[2], address[3], address[4], address[5], n->up ? "up" : "down");
	}
	if (kernel_thread_start(net_thread, n, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	return true;
}

void
hakios_net_init(void)
{
	PE_parse_boot_argn("net_trace", &net_trace, sizeof(net_trace));
	PE_parse_boot_argn("net_names", &net_names, sizeof(net_names));
	hakios_virtio_net_init();
}

/*
 * ---- the USB console's "wifi ip" and "wifi ping A.B.C.D" (drivers/core/hakios.dwc3.c) ------------------------------
 * What en0 was given (configd's DHCP, once the radio has joined and said its link is up), and whether the network
 * answers: four ICMP echo requests from an ICMP datagram socket of the kernel's own (the kind ping uses without root),
 * a second for each answer. With no address, the default route's gateway.
 */
#include <net/route.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <sys/kpi_socket.h>
#include <sys/socket.h>

extern void loader_log_line(const char *line);
extern kern_return_t thread_terminate(thread_t thread);

static void
console_say(const char *format, ...)
{
	char line[200];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	loader_log_line(line);
}

static void
ipv4_text(struct in_addr a, char *text, size_t room)
{
	const uint8_t *b = (const uint8_t *)&a.s_addr;

	snprintf(text, room, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* en0's first IPv4 address and mask; false without */
static bool
en0_ipv4(struct in_addr *address, struct in_addr *mask)
{
	ifnet_t ifp = NULL;
	ifaddr_t *list = NULL;
	bool found = false;

	if (ifnet_find_by_name("en0", &ifp) != 0 || ifp == NULL) {
		return false;
	}
	if (ifnet_get_address_list_family(ifp, &list, AF_INET) == 0 && list != NULL) {
		for (unsigned i = 0; list[i] != NULL && !found; i++) {
			struct sockaddr_in a = { 0 }, m = { 0 };

			if (ifaddr_address(list[i], (struct sockaddr *)&a, sizeof(a)) == 0 && a.sin_family == AF_INET) {
				*address = a.sin_addr;
				mask->s_addr = ifaddr_netmask(list[i], (struct sockaddr *)&m, sizeof(m)) == 0 ? m.sin_addr.s_addr : 0;
				found = true;
			}
		}
		ifnet_free_address_list(list);
	}
	ifnet_release(ifp);
	return found;
}

/* the default route's gateway, if there is one */
static bool
default_gateway(struct in_addr *gateway, char *interface, size_t room)
{
	struct sockaddr_in any = { .sin_len = sizeof(any), .sin_family = AF_INET };
	struct rtentry *rt = rtalloc1((struct sockaddr *)&any, 0, 0);
	bool found = false;

	if (rt == NULL) {
		return false;
	}
	RT_LOCK(rt);
	if ((rt->rt_flags & RTF_GATEWAY) != 0 && rt->rt_gateway != NULL && rt->rt_gateway->sa_family == AF_INET) {
		*gateway = ((struct sockaddr_in *)(void *)rt->rt_gateway)->sin_addr;
		if (rt->rt_ifp != NULL) {
			snprintf(interface, room, "%s%u", ifnet_name(rt->rt_ifp), ifnet_unit(rt->rt_ifp));
		} else {
			snprintf(interface, room, "?");
		}
		found = true;
	}
	RT_UNLOCK(rt);
	rtfree(rt);
	return found;
}

void
hakios_net_console_ip(void)
{
	struct in_addr address, mask, gateway;
	char a[16], m[16], g[16], via[IFNAMSIZ + 4];
	struct net_if *n = net_if_unit(0);

	if (n == NULL) {
		console_say("usb: wifi ip: there is no en0 (the radio's driver did not get to brcmfmac=7)\n");
		return;
	}
	if (!en0_ipv4(&address, &mask)) {
		console_say("usb: wifi ip: en0's link is %s; no IPv4 address yet (configd's DHCP, once the link is up)\n",
		    n->up ? "up" : "down");
		return;
	}
	ipv4_text(address, a, sizeof(a));
	ipv4_text(mask, m, sizeof(m));
	if (default_gateway(&gateway, via, sizeof(via))) {
		ipv4_text(gateway, g, sizeof(g));
	} else {
		snprintf(g, sizeof(g), "none");
		via[0] = 0;
	}
	console_say("usb: wifi ip: en0's link is %s; address %s, mask %s; the default route's gateway %s%s%s\n",
	    n->up ? "up" : "down", a, m, g, via[0] ? " on " : "", via);
}

static uint16_t
icmp_sum(const uint8_t *p, size_t length)
{
	uint32_t sum = 0;

	for (size_t i = 0; i + 1 < length; i += 2) {
		sum += (uint32_t)(p[i] << 8 | p[i + 1]);
	}
	if (length & 1) {
		sum += (uint32_t)p[length - 1] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	return htons((uint16_t)~sum);
}

static volatile bool console_pinging;

static void
ping_runs(void *parameter, __unused wait_result_t result)
{
	struct in_addr to = { .s_addr = (in_addr_t)(uintptr_t)parameter };
	struct sockaddr_in where = { .sin_len = sizeof(where), .sin_family = AF_INET, .sin_addr = to };
	struct timeval wait = { .tv_sec = 1, .tv_usec = 0 };
	unsigned answered = 0, seq;
	socket_t so = NULL;
	char t[16];
	errno_t error;

	ipv4_text(to, t, sizeof(t));
	if ((error = sock_socket_internal(PF_INET, SOCK_DGRAM, IPPROTO_ICMP, NULL, NULL, &so)) != 0) {
		console_say("usb: wifi ping %s: no ICMP socket: %d\n", t, error);
		goto out;
	}
	(void)sock_setsockopt(so, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
	for (seq = 1; seq <= 4; seq++) {
		uint8_t packet[64], reply[256];
		struct icmp *icp = (struct icmp *)(void *)packet;
		struct iovec iov = { .iov_base = packet, .iov_len = sizeof(packet) };
		struct msghdr msg = { .msg_name = &where, .msg_namelen = sizeof(where), .msg_iov = &iov, .msg_iovlen = 1 };
		uint64_t t0 = mach_absolute_time(), ns;
		size_t done = 0;
		bool got = false;

		bzero(packet, sizeof(packet));
		icp->icmp_type = ICMP_ECHO;
		icp->icmp_id = htons(0x4853);
		icp->icmp_seq = htons((uint16_t)seq);
		for (size_t i = 8; i < sizeof(packet); i++) {
			packet[i] = (uint8_t)i;
		}
		icp->icmp_cksum = icmp_sum(packet, sizeof(packet));
		if ((error = sock_send(so, &msg, 0, &done)) != 0) {
			console_say("usb: wifi ping %s: %u: not sent: %d%s\n", t, seq, error,
			    error == ENETUNREACH || error == EHOSTUNREACH ? " (no route: no address yet?)" : "");
			delay_for_interval(1000, NSEC_PER_MSEC);
			continue;
		}
		do {
			struct iovec in = { .iov_base = reply, .iov_len = sizeof(reply) };
			struct msghdr m = { .msg_iov = &in, .msg_iovlen = 1 };
			size_t hl;

			done = 0;
			if (sock_receive(so, &m, 0, &done) != 0) {
				break;  /* the second is up */
			}
			hl = done > 0 && (reply[0] >> 4) == 4 ? (size_t)(reply[0] & 15) * 4 : 0;
			if (done >= hl + 8) {
				const struct icmp *r = (const struct icmp *)(const void *)(reply + hl);

				if (r->icmp_type == ICMP_ECHOREPLY && r->icmp_id == htons(0x4853) && r->icmp_seq == htons((uint16_t)seq)) {
					absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
					console_say("usb: wifi ping %s: %u: %zu bytes back in %llu.%03llu ms\n", t, seq, done - hl,
					    ns / NSEC_PER_MSEC, (ns / NSEC_PER_USEC) % 1000);
					got = true;
					answered++;
				}
			}
			absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		} while (!got && ns < NSEC_PER_SEC);
		if (!got) {
			console_say("usb: wifi ping %s: %u: no answer in 1 s\n", t, seq);
		} else {
			delay_for_interval(200, NSEC_PER_MSEC);
		}
	}
	console_say("usb: wifi ping %s: %u of 4 answered\n", t, answered);
	sock_close(so);
out:
	console_pinging = false;
	thread_terminate(current_thread());
}

void
hakios_net_console_ping(const char *arg)
{
	struct in_addr to = { 0 };
	unsigned part[4] = { 0 }, n = 0;
	char via[IFNAMSIZ + 4];
	thread_t thread;
	const char *p;

	for (p = arg; *p != 0 && n < 4; p++) {
		if (*p >= '0' && *p <= '9') {
			part[n] = part[n] * 10 + (unsigned)(*p - '0');
			if (part[n] > 255) {
				break;
			}
		} else if (*p == '.' && p > arg && p[-1] != '.') {
			n++;
		} else {
			break;
		}
	}
	if (*arg == 0) {
		if (!default_gateway(&to, via, sizeof(via))) {
			console_say("usb: wifi ping: no default route yet (wifi ip); or give an address: wifi ping A.B.C.D\n");
			return;
		}
	} else if (*p != 0 || n != 3) {
		console_say("usb: wifi ping %s: an IPv4 address A.B.C.D is wanted\n", arg);
		return;
	} else {
		uint8_t *b = (uint8_t *)&to.s_addr;

		for (unsigned i = 0; i < 4; i++) {
			b[i] = (uint8_t)part[i];
		}
	}
	if (console_pinging) {
		console_say("usb: wifi ping: one is under way\n");
		return;
	}
	console_pinging = true;
	if (kernel_thread_start(ping_runs, (void *)(uintptr_t)to.s_addr, &thread) != KERN_SUCCESS) {
		console_pinging = false;
		console_say("usb: wifi ping: no thread for it\n");
		return;
	}
	thread_set_thread_name(thread, "net-console-ping");
	thread_deallocate(thread);
}
