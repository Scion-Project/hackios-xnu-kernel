/*
 * What brcmfmac's files ask of LinuxKPI besides what wifi/lx/src/lx_base.c already gives ath11k: sleeping and
 * spinning by time, a few string functions, firmware asked for in the background, a wiphy and a net_device as
 * plain memory, and debugfs as nothing. Small things each, on the calls of lx_os.h.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/linker.h>
#include <sys/lock.h>
#include <sys/sx.h>
#include <sys/malloc.h>
#include <sys/time.h>

#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/netdevice.h>
#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>

#include <lx_os.h>
#include "lx.h"
#include "lx_brcmf.h"

int linuxkpi_debug;
int linuxkpi_debug_80211;
linker_file_t __this_linker_file;
struct workqueue_struct *system_wq;		/* (made by lx_start()) */

/* ---- time ---------------------------------------------------------------------------------------- */

void
DELAY(int usec)
{
	unsigned long long until = lx_os_nanoseconds() + (unsigned long long)usec * 1000;

	while (lx_os_nanoseconds() < until) {
		__asm__ volatile ("yield");
	}
}

/* msleep() and usleep_range() of linux/delay.h: asleep for that long (a time in 1/2^32 seconds) */
int
pause_sbt(const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags)
{
	static char nothing;			/* (an address nobody wakes) */
	unsigned long long ns = ((unsigned long long)sbt * 1000000000ull) >> 32;

	lx_os_lock();
	lx_os_sleep(&nothing, ns == 0 ? 1 : ns);
	lx_os_unlock();
	return (0);
}

void
add_timer(struct timer_list *timer)
{
	mod_timer(timer, timer->expires);
}

/* (lx_base.c's waits look again every few milliseconds whether woken or not: waking is never wasted) */
bool
linux_waitqueue_active(wait_queue_head_t *wqh)
{
	return (true);
}

/* DEFINE_MUTEX()'s initialisation, which FreeBSD runs at boot; a lock of lx_base.c that is all zeroes is ready as it is */
void
sx_sysinit(const void *arg)
{
	const struct sx_args *args = arg;

	sx_init_flags(args->sa_sx, args->sa_desc, args->sa_flags);
}

/* ---- strings ------------------------------------------------------------------------------------- */

size_t
strnlen(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n] != 0) {
		n++;
	}
	return (n);
}

char *
strchrnul(const char *s, int c)
{
	while (*s != 0 && *s != (char)c) {
		s++;
	}
	return (__DECONST(char *, s));
}

char *
strnstr(const char *s, const char *find, size_t max)
{
	size_t length = strlen(find);

	if (length == 0) {
		return (__DECONST(char *, s));
	}
	for (; max >= length && *s != 0; s++, max--) {
		if (strncmp(s, find, length) == 0) {
			return (__DECONST(char *, s));
		}
	}
	return (NULL);
}

char *
strsep(char **string, const char *delimiters)
{
	char *start = *string, *s;

	if (start == NULL) {
		return (NULL);
	}
	for (s = start; *s != 0; s++) {
		if (strchr(delimiters, *s) != NULL) {
			*s = 0;
			*string = s + 1;
			return (start);
		}
	}
	*string = NULL;
	return (start);
}

u_long
strtoul(const char *s, char **end, int base)
{
	u_long value = 0;

	while (*s == ' ' || *s == '\t') {
		s++;
	}
	if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		base = 16;
	} else if (base == 0) {
		base = s[0] == '0' ? 8 : 10;
	}
	for (;; s++) {
		int digit = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'z' ? *s - 'a' + 10 :
		    *s >= 'A' && *s <= 'Z' ? *s - 'A' + 10 : 99;

		if (digit >= base) {
			break;
		}
		value = value * (u_long)base + (u_long)digit;
	}
	if (end != NULL) {
		*end = __DECONST(char *, s);
	}
	return (value);
}

static char *
vkasprintf(const char *format, va_list list)
{
	char *text;
	va_list again;
	int length;

	va_copy(again, list);
	length = vsnprintf(NULL, 0, format, again);
	va_end(again);
	text = kmalloc(length + 1, GFP_KERNEL);
	if (text != NULL) {
		vsnprintf(text, length + 1, format, list);
	}
	return (text);
}

char *
kasprintf(gfp_t gfp, const char *format, ...)
{
	va_list list;
	char *text;

	va_start(list, format);
	text = vkasprintf(format, list);
	va_end(list);
	return (text);
}

/* (the device's memory is never given back here: nor is this) */
char *
lkpi_devm_kasprintf(struct device *dev, gfp_t gfp, const char *format, ...)
{
	va_list list;
	char *text;

	va_start(list, format);
	text = vkasprintf(format, list);
	va_end(list);
	return (text);
}

/* ---- saying things ------------------------------------------------------------------------------- */

int
device_printf(device_t dev, const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("brcmfmac: %s", line);
	return (0);
}

int
__lkpi_hexdump_printf(void *argument, const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("%s", line);
	return (0);
}

/* sixteen bytes a line, whatever was asked for */
void
lkpi_hex_dump(int (*print)(void *, const char *, ...), void *argument, const char *level, const char *prefix,
    const int prefix_type, const int rowsize, const int groupsize, const void *buffer, size_t length,
    const bool ascii, const bool trailing)
{
	const uint8_t *b = buffer;

	for (size_t at = 0; at < length; at += 16) {
		char line[16 * 3 + 1];
		size_t n = length - at < 16 ? length - at : 16;

		for (size_t i = 0; i < n; i++) {
			snprintf(line + 3 * i, 4, " %02x", b[at + i]);
		}
		line[3 * n] = 0;
		print(argument, "%s%04zx:%s\n", prefix != NULL ? prefix : "", at, line);
	}
}

/* debugfs: there is none */
struct dentry *
debugfs_create_file(const char *name, umode_t mode, struct dentry *parent, void *data, const struct file_operations *fops)
{
	return (NULL);
}

void
seq_puts(struct seq_file *m, const char *text)
{
}

void
lkpi_seq_printf(struct seq_file *m, const char *format, ...)
{
}

int
simple_attr_open(struct inode *inode, struct file *filp, int (*get)(void *, uint64_t *), int (*set)(void *, uint64_t),
    const char *format)
{
	return (-ENODEV);
}

int
simple_attr_release(struct inode *inode, struct file *filp)
{
	return (0);
}

ssize_t
simple_attr_read(struct file *filp, char __user *buf, size_t length, loff_t *offset)
{
	return (-ENODEV);
}

ssize_t
simple_attr_write(struct file *filp, const char __user *buf, size_t length, loff_t *offset)
{
	return (-ENODEV);
}

/* nothing comes from a user's process here */
int
linux_copyin(const void *uaddr, void *kaddr, size_t length)
{
	return (EFAULT);
}

/* ---- firmware files: asked for in the background, as Linux does ---------------------------------- */

int
linuxkpi_request_firmware(const struct linuxkpi_firmware **out, const char *name, struct device *dev)
{
	*out = NULL;
	return (linuxkpi_firmware_request_nowarn(out, name, dev));
}

struct lx_fw_request {
	char name[96];
	struct device *dev;
	void *context;
	void (*cont)(const struct linuxkpi_firmware *, void *);
};

static void
fw_request_runs(void *argument)
{
	struct lx_fw_request *r = argument;
	const struct linuxkpi_firmware *fw = NULL;

	if (linuxkpi_firmware_request_nowarn(&fw, r->name, r->dev) != 0) {
		fw = NULL;
	}
	r->cont(fw, r->context);
	kfree(r);
}

int
linuxkpi_request_firmware_nowait(struct module *module, bool uevent, const char *name, struct device *dev, gfp_t gfp,
    void *context, void (*cont)(const struct linuxkpi_firmware *, void *))
{
	struct lx_fw_request *r;

	/*
	 * brcmfmac=1: the request is taken and never answered, as for a file that does not come. (Refused, it would have
	 * brcmfmac fail its probe from inside it and say the firmware has crashed.)
	 */
	if (!lx_brcmf_may(2, "the firmware, NVRAM and CLM asked for")) {
		return (0);
	}
	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (r == NULL) {
		return (-ENOMEM);
	}
	strlcpy(r->name, name, sizeof(r->name));
	r->dev = dev;
	r->context = context;
	r->cont = cont;
	if (lx_os_thread("brcmfmac-fw", fw_request_runs, r) != 0) {
		kfree(r);
		return (-ENOMEM);
	}
	return (0);
}

/* ---- cfg80211's wiphy and the network device, as memory: what registers them is not built yet ----- */

struct wiphy *
linuxkpi_wiphy_new(const struct cfg80211_ops *ops, size_t priv_length)
{
	struct wiphy *wiphy = kzalloc(sizeof(*wiphy) + priv_length, GFP_KERNEL);

	if (wiphy != NULL) {
		mutex_init(&wiphy->mtx);
	}
	return (wiphy);
}

void
linuxkpi_wiphy_free(struct wiphy *wiphy)
{
	kfree(wiphy);
}

struct net_device *
linuxkpi_alloc_netdev(size_t priv_length, const char *name, uint32_t flags, void (*setup)(struct net_device *))
{
	struct net_device *ndev = kzalloc(sizeof(*ndev) + priv_length, GFP_KERNEL);	/* (drv_priv[] at its end) */

	if (ndev == NULL) {
		return (NULL);
	}
	strlcpy(ndev->name, name, sizeof(ndev->name));
	if (setup != NULL) {
		setup(ndev);
	}
	return (ndev);
}

void
linuxkpi_free_netdev(struct net_device *ndev)
{
	kfree(ndev);
}

/* ---- what FreeBSD's kernel would have, asked for through LinuxKPI's headers --------------------------- */

int cold;					/* (not booting: the kernel is long up when the driver starts) */
struct malloc_type M_KMALLOC[1];

/* (malloc(9) is a macro around this one; what vendor.c takes of it is a buffer's worth, with kfree() back) */
#undef malloc
void *
malloc(size_t size, struct malloc_type *type, int flags)
{
	return (lkpi_kmalloc(size, (flags & M_ZERO) ? GFP_KERNEL | __GFP_ZERO : GFP_KERNEL));
}

void
nanouptime(struct timespec *ts)
{
	unsigned long long ns = lx_os_nanoseconds();

	ts->tv_sec = ns / 1000000000ull;
	ts->tv_nsec = ns % 1000000000ull;
}

void
panic(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	lx_os_panic(line);
}

/* cond_resched(): a moment's sleep, so that a loop that asks for it lets the others run */
void
sched_relinquish(struct thread *td)
{
	static char nothing;

	lx_os_lock();
	lx_os_sleep(&nothing, 1000);
	lx_os_unlock();
}

void
sx_destroy(struct sx *sx)
{
}

int
timer_shutdown_sync(struct timer_list *timer)
{
	return (timer_delete_sync(timer));
}

/* ---- cfg80211, as far as brcmfmac's cfg80211.c reaches before anything above takes it ------------------ */

/*
 * The wiphy registered: nothing takes it yet (lx_80211.c is the POCO's, for mac80211; the S9's station is a later
 * step of stage 2). What the firmware says it can do is put in the log.
 */
int
linuxkpi_80211_wiphy_register(struct wiphy *wiphy)
{
	for (int b = 0; b < NUM_NL80211_BANDS; b++) {
		if (wiphy->bands[b] != NULL) {
			lx_tell("lx: cfg80211: band %d: %d channels, %d rates\n", b, wiphy->bands[b]->n_channels,
			    wiphy->bands[b]->n_bitrates);
		}
	}
	lx_tell("lx: cfg80211: the wiphy is registered; no station takes it yet\n");
	return (0);
}

/* IEEE 802.11-2020, 27.3.23.2 and Annex E: channel numbers to centre frequencies, MHz */
uint32_t
linuxkpi_ieee80211_channel_to_frequency(uint32_t channel, enum nl80211_band band)
{
	switch (band) {
	case NL80211_BAND_2GHZ:
		return (channel == 14 ? 2484 : channel >= 1 && channel <= 13 ? 2407 + 5 * channel : 0);
	case NL80211_BAND_5GHZ:
		return (channel >= 182 && channel <= 196 ? 4000 + 5 * channel : 5000 + 5 * channel);
	case NL80211_BAND_6GHZ:
		return (channel == 2 ? 5935 : 5950 + 5 * channel);
	default:
		return (0);
	}
}

uint32_t
linuxkpi_ieee80211_frequency_to_channel(uint32_t frequency, uint32_t flags)
{
	if (frequency == 2484) {
		return (14);
	}
	if (frequency >= 2412 && frequency < 2484) {
		return ((frequency - 2407) / 5);
	}
	if (frequency >= 4910 && frequency <= 4980) {
		return ((frequency - 4000) / 5);
	}
	if (frequency == 5935) {
		return (2);
	}
	if (frequency > 5950 && frequency <= 7115) {
		return ((frequency - 5950) / 5);
	}
	if (frequency >= 5000 && frequency <= 5925) {
		return ((frequency - 5000) / 5);
	}
	return (0);
}

struct linuxkpi_ieee80211_channel *
linuxkpi_ieee80211_get_channel(struct wiphy *wiphy, uint32_t frequency)
{
	for (int b = 0; b < NUM_NL80211_BANDS; b++) {
		struct ieee80211_supported_band *band = wiphy->bands[b];

		for (int c = 0; band != NULL && c < band->n_channels; c++) {
			if (band->channels[c].center_freq == frequency) {
				return (&band->channels[c]);
			}
		}
	}
	return (NULL);
}
