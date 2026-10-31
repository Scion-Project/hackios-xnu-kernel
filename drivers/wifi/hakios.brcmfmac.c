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
 *                writes memory: DMA), events, cfg80211's wiphy; the chip's interrupt looked at by a thread of the object
 *                every brcmfmac_poll_ms
 *   brcmfmac=5   ... the interrupt by INTx instead: SPI 253 of the GIC, level high, through the root complex's latch
 *                (ELBI's PCIE_IRQ_PULSE, cleared before the driver's handler runs). brcmfmac_poll=1 keeps the thread
 *   brcmfmac=6   ... the phone's own address: boot-arg wifi_mac, else fw/mac.info of the small image (the phone's
 *                /efs/wifi/.mac.info), given to brcmfmac, which sets it in the firmware (cur_etheraddr); without
 *                either, the firmware's own (its NVRAM's or OTP's), as at 4 and 5
 *   brcmfmac=7   ... the station: the interface up (the firmware's "up"), en0 on it and the station given to the
 *                kernel (sys/hakios.sta.h), so that iOS's Wi-Fi settings scan through it; nothing is joined
 *   brcmfmac=8   ... joining: an open network, or WPA2 with a passphrase (the four-way handshake by the object's
 *                lx_wpa.c, the keys given to the firmware), then frames; and the network of fw/network of the small
 *                image (its name on the first line, its passphrase on the second), if there is one, joined at once
 * The way back to the loader (hakios_wifi_stop(), hakios_wifi_quiesce()): the driver taken down in order when it got as far
 * as its interface (lx_stop()), then the radio held in reset and unpowered (hakios_exynos_pcie_quiesce()).
 *
 * Memory the chip reads and writes is shown to it at its physical addresses: the PCIe SysMMU (0x11420000), which
 * Android puts in between, is not touched. Whether the controller's master then reaches memory is the open question
 * step 4 answers.
 *
 * What was checked: compiled, and the object built and linked (tools/mk-brcmfmac.sh); it has not run on the phone.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.exynos_pcie.h>
#include <sys/hakios.input.h>
#include <sys/hakios.lx.h>
#include <sys/hakios.macaddr.h>
#include <sys/hakios.sta.h>
#include <sys/systm.h>

extern void loader_log_line(const char *line);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern kern_return_t thread_terminate(thread_t thread);
extern void thread_set_thread_name(thread_t thread, const char *name);
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern void gic_v3_spi_unmask(uint32_t intid);
extern void gic_v3_spi_detach(uint32_t intid);
extern uint64_t gic_v3_spi_count(uint32_t intid);

static LCK_GRP_DECLARE(brcmfmac_lock_group, "hakios-brcmfmac");
static LCK_MTX_DECLARE(say_lock, &brcmfmac_lock_group);
static uint32_t steps;                  /* brcmfmac=N */
static bool started;                    /* the driver was started (brcmfmac=1 and up) */

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
	loader_log_line(line);
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

/*
 * brcmfmac=6: the phone's own address, which Android keeps in /efs/wifi/.mac.info and bcmdhd sets in the firmware
 * (dhd_custom_cis.c, dhd_set_macaddr_from_file(): "cur_etheraddr"); the firmware's own is its NVRAM's or OTP's. Boot-arg
 * wifi_mac first (as on the POCO), else fw/mac.info of the small image (userland/build.sh puts fw-wifi/mac.info there).
 * The log says where it came from and its first three bytes (the maker's), not the rest.
 */
__attribute__((weak)) bool
hakios_wifi_radio_address(uint8_t *address)
{
	static int said;
	char text[32] = "";
	const uint8_t *file = NULL;
	uint64_t size = 0;
	const char *from;
	bool good;

	if (steps < 6) {
		return false;
	}
	if (PE_parse_boot_arg_str("wifi_mac", text, sizeof(text)) && text[0] != 0) {
		from = "boot-arg wifi_mac";
		good = hakios_macaddr_parse(text, strlen(text), address) != 0;
	} else if ((file = hakios_image_file("fw", "mac.info", &size, NULL)) != NULL) {
		from = "fw/mac.info";
		good = hakios_macaddr_parse((const char *)file, size > 64 ? 64 : (size_t)size, address) != 0;
	} else {
		if (said++ == 0) {
			say("brcmfmac: 6: no address of the phone's (wifi_mac, fw/mac.info: the phone's /efs/wifi/.mac.info): the "
			    "firmware's own is used\n");
		}
		return false;
	}
	if (said++ == 0) {
		if (good) {
			say("brcmfmac: 6: the phone's address, from %s: %02x:%02x:%02x:..:..:..\n", from, address[0], address[1],
			    address[2]);
		} else {
			say("brcmfmac: 6: %s IS NOT AN ADDRESS (six bytes in hexadecimal with colons, not all zeroes, not a group's): "
			    "the firmware's own is used\n", from);
		}
	}
	return good;
}

/*
 * brcmfmac=8: the network to join, fw/network of the small image (fw-wifi/network: its name on the first line, its
 * passphrase on the second; hakios.wifi.c reads the same file on the POCO). Only the lengths are said.
 */
__attribute__((weak)) bool
hakios_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length)
{
	uint64_t size = 0;
	const uint8_t *file = steps >= 8 ? hakios_image_file("fw", "network", &size, NULL) : NULL, *end = file + size, *line = file;
	unsigned lengths[2] = { 0, 0 }, sizes[2] = { *name_length, *passphrase_length };
	uint8_t *where[2] = { name, passphrase };

	if (file == NULL) {
		return false;
	}
	for (unsigned n = 0; n < 2 && line < end; n++) {
		const uint8_t *stop = line;

		while (stop < end && *stop != '\n') {
			stop++;
		}
		lengths[n] = (unsigned)(stop - line) - (stop > line && stop[-1] == '\r' ? 1 : 0);
		if (lengths[n] > sizes[n]) {
			lengths[n] = 0;
		} else {
			memcpy(where[n], line, lengths[n]);
		}
		line = stop + 1;
	}
	say("brcmfmac: 8: fw/network: a name of %u bytes and a passphrase of %u bytes%s\n", lengths[0], lengths[1],
	    lengths[0] == 0 || lengths[0] > 32 || lengths[1] < 8 || lengths[1] > 63 ? ": NOT A NAME OF 1 TO 32 BYTES AND A "
	    "PASSPHRASE OF 8 TO 63" : "");
	if (lengths[0] == 0 || lengths[0] > 32 || lengths[1] < 8 || lengths[1] > 63) {
		return false;
	}
	*name_length = lengths[0];
	*passphrase_length = lengths[1];
	return true;
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

/*
 * ---- the chip's interrupt, brcmfmac=5 (sys/hakios.lx.h's line): INTx, latched by the root complex in ELBI's
 * PCIE_IRQ_PULSE and passed to the GIC as SPI 253, level high (Samsung's device tree: interrupts <0 253 0> and the
 * interrupt-map of every INTx to it, type 4; use-msi "false"). In interrupt context the SPI is masked and this file's
 * thread told; the thread clears the latch (Android's exynos_pcie_irq_handler() does that first too) and calls the
 * object's function, which calls brcmfmac's handlers (the chip's mailbox read and cleared, which ends its INTA) and then
 * lx_os_irq_line_enable(). An INTA asserted again meanwhile is latched again, and fires as soon as the SPI is unmasked.
 */
#define INTX_INTID      (32u + HAKIOS_EXYNOS_PCIE_INTX_SPI)

static struct {
	void (*function)(void *);
	void *argument;
	volatile bool attached;
	bool running;
	uint32_t pending;
	thread_t thread;
	uint64_t latched_not_a;                 /* times the latch held something besides INTA */
} line;
static LCK_MTX_DECLARE(line_lock, &brcmfmac_lock_group);

static void
line_arrived(__unused void *context, uint32_t intid)
{
	gic_v3_spi_mask(intid);
	os_atomic_store(&line.pending, 1, release);
	thread_wakeup((event_t)&line.pending);
}

static void
line_runs(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		for (;;) {
			assert_wait((event_t)&line.pending, THREAD_UNINT);
			if (os_atomic_xchg(&line.pending, 0, acquire) != 0) {
				clear_wait(current_thread(), THREAD_AWAKENED);
				break;
			}
			thread_block(THREAD_CONTINUE_NULL);
		}
		lck_mtx_lock(&line_lock);
		if (!line.attached) {
			lck_mtx_unlock(&line_lock);
			continue;
		}
		line.running = true;
		lck_mtx_unlock(&line_lock);
		if (hakios_exynos_pcie_intx_ack() & ~1u) {
			line.latched_not_a++;
		}
		line.function(line.argument);
		lck_mtx_lock(&line_lock);
		line.running = false;
		lck_mtx_unlock(&line_lock);
		thread_wakeup((event_t)&line.running);
	}
}

int
lx_os_irq_line_attach(void (*function)(void *), void *argument)
{
	kern_return_t kr;

	if (!hakios_exynos_pcie_ready() || function == NULL) {
		return ENXIO;
	}
	if (line.thread == THREAD_NULL) {
		if (kernel_thread_start(line_runs, NULL, &line.thread) != KERN_SUCCESS) {
			line.thread = THREAD_NULL;
			return ENOMEM;
		}
		thread_set_thread_name(line.thread, "brcmfmac-intx");
	}
	say("brcmfmac: 5: THE GIC'S DISTRIBUTOR IS WRITTEN for the chip's interrupt: SPI %u (INTID %u) level-triggered, to "
	    "this processor; then the root complex's INTA to INTD enabled (ELBI +0x00C)\n", HAKIOS_EXYNOS_PCIE_INTX_SPI, INTX_INTID);
	lck_mtx_lock(&line_lock);
	line.function = function;
	line.argument = argument;
	line.attached = true;
	lck_mtx_unlock(&line_lock);
	if ((kr = gic_v3_spi_attach(INTX_INTID, false, line_arrived, NULL)) != KERN_SUCCESS) {
		lck_mtx_lock(&line_lock);
		line.attached = false;
		lck_mtx_unlock(&line_lock);
		say("brcmfmac: 5: the GIC refuses SPI %u (%d)\n", HAKIOS_EXYNOS_PCIE_INTX_SPI, kr);
		return kr == KERN_NAME_EXISTS ? EBUSY : EINVAL;
	}
	gic_v3_spi_mask(INTX_INTID);
	hakios_exynos_pcie_intx(true);
	return 0;
}

void
lx_os_irq_line_enable(void)
{
	if (line.attached) {
		gic_v3_spi_unmask(INTX_INTID);
	}
}

void
lx_os_irq_line_detach(void)
{
	if (!line.attached) {
		return;
	}
	hakios_exynos_pcie_intx(false);
	gic_v3_spi_detach(INTX_INTID);
	lck_mtx_lock(&line_lock);
	line.attached = false;
	while (line.running && current_thread() != line.thread) {
		lck_mtx_sleep(&line_lock, LCK_SLEEP_DEFAULT, (event_t)&line.running, THREAD_UNINT);
	}
	lck_mtx_unlock(&line_lock);
	say("brcmfmac: the chip's interrupt let go: %llu arrived; the latch held more than INTA %llu times\n",
	    gic_v3_spi_count(INTX_INTID), line.latched_not_a);
}

unsigned long long
lx_os_irq_line_count(void)
{
	return line.attached ? gic_v3_spi_count(INTX_INTID) : 0;
}

/*
 * ---- the way back to the loader. hakios_wifi_stop(): where a run ends in order (loader_return_after, the host's "xl
 * stop", too hot, the battery low), on a thread: the driver taken down in order (lx_stop(): the network left, the
 * interface down, brcmfmac's removal, which resets the chip) if it got as far as its interface, then the radio held in
 * reset and unpowered (hakios_exynos_pcie_quiesce(), the same that loader_return() calls). hakios_wifi_quiesce(): on
 * every way back, after a panic too, with one processor left and interrupts off: no lock and no sleep, only SPI 253
 * masked; loader_return() calls hakios_exynos_pcie_quiesce() after it for the rest. Weak: a kernel with the WCN6750's
 * driver too has that driver's (it does not start this one).
 */
__attribute__((weak)) void
hakios_wifi_stop(void)
{
	static bool stopped;

	if (stopped) {
		return;
	}
	stopped = true;
	if (started) {
		say("brcmfmac: the run ends in order: the driver taken down (lx_stop)\n");
		hakios_lx_stop();
	}
	hakios_exynos_pcie_quiesce();
}

__attribute__((weak)) void
hakios_wifi_quiesce(void)
{
	if (line.attached) {
		gic_v3_spi_mask(INTX_INTID);
	}
}

#ifdef HAKIOS_WIFI_STA
/*
 * brcmfmac=8 with fw/network: the network joined the way anybody joins one, by asking the station (sys/hakios.sta.h),
 * as soon as the object has offered it; and again whenever the radio is turned on, until iOS asks for another.
 */
static void
join_given(__unused void *parameter, __unused wait_result_t result)
{
	static struct hakios_sta_join what;
	struct hakios_sta_state state;
	unsigned name_length = sizeof(what.ssid), passphrase_length = sizeof(what.passphrase), waited;
	int error;

	bzero(&what, sizeof(what));
	if (!hakios_wifi_network(what.ssid, &name_length, what.passphrase, &passphrase_length)) {
		say("brcmfmac: 8: no network given to join at the start (fw-wifi/network): the station waits to be asked\n");
		thread_terminate(current_thread());
	}
	what.ssid_length = (uint8_t)name_length;
	what.passphrase_length = (uint8_t)passphrase_length;
	what.security = HAKIOS_STA_WPA2_PSK;
	for (waited = 0; !hakios_sta_present() && waited < 1200; waited++) {
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	if (!hakios_sta_present()) {
		say("brcmfmac: 8: no station was offered in 120 s: the network of fw/network is not joined\n");
		bzero(&what, sizeof(what));
		thread_terminate(current_thread());
	}
	say("brcmfmac: 8: the station is there, %u.%u s after the driver began: the network of fw/network is asked for\n",
	    waited / 10, waited % 10);
	error = hakios_sta_ask_join_standing(&what);
	bzero(&what, sizeof(what));
	for (waited = 0; error == 0 && hakios_sta_busy() && waited < 600; waited++) {
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	hakios_sta_state(&state);
	say("brcmfmac: 8: %s (%d, %d)\n", state.phase == HAKIOS_STA_JOINED ? "THE NETWORK OF FW/NETWORK IS JOINED" :
	    "THE NETWORK OF FW/NETWORK IS NOT JOINED", error, state.last_error);
	thread_terminate(current_thread());
}
#endif

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
		say("brcmfmac: the device is handed over but the driver is not started (brcmfmac=N, 1 to 8)\n");
		return;
	}
	say("brcmfmac: brcmfmac=%u: Linux's brcmfmac on the BCM4361, step by step (devices/starlte/WIFI.md)\n", steps);
	if ((error = hakios_lx_prepare()) != 0) {
		say("brcmfmac: the driver's preparation fails: %d\n", error);
		return;
	}
	started = true;
	if (steps >= 7) {
		/* en0 made now on the phone's address, if it has one: iOS's Wi-Fi side looks for it early, and once */
		hakios_lx_net_early();
	}
	if (steps >= 8) {
#ifdef HAKIOS_WIFI_STA
		thread_t thread;

		if (kernel_thread_start(join_given, NULL, &thread) == KERN_SUCCESS) {
			thread_set_thread_name(thread, "brcmfmac-join");
			thread_deallocate(thread);
		}
#else
		say("brcmfmac: 8: this kernel has no station layer (WIFI_STA): nothing above the driver asks it to join\n");
#endif
	}
	error = hakios_lx_start();
	say("brcmfmac: the driver's start returns %d\n", error);
}
