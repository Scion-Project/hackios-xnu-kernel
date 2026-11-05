/* The run's log kept across a restart: a raw write to a GPT partition, on a phone only. */

static efi_block_io *
find_log_partition(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	static const char16 want[] = LOG_PARTITION;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		int match = 1;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2) {
			continue;
		}
		for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
			if (info->name[k] != want[k]) {
				match = 0;
				break;
			}
		}
		if (match && !EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			/* (the handle array is pool memory the caller owns; this runs on every log write) */
			BS->free_pool(handles);
			return bio;
		}
	}
	BS->free_pool(handles);
	return NULL;
}

/*
 * The "staged" buffer holds the loader log followed by the kernel log of the
 * last run. It is what gets written to the log partition and what the host
 * fetches over USB (fastboot get_staged).
 */
/* aligned for any firmware's block I/O: U-Boot's wants a buffer aligned to the block size (media->io_align) */
static char out_buf[2 * 1024 * 1024] __attribute__((aligned(0x2000)));
/* the storage's reads for the kernel land here (block_poll), and nothing else may be put in it */
#define BLOCK_BUFFER_SIZE  (4u << 20)
static uint8_t block_buffer[BLOCK_BUFFER_SIZE] __attribute__((aligned(0x2000)));
/*
 * Where the log is read back to compare it (write_logs). Not block_buffer: that is the kernel's
 * mailbox buffer, and a read-back landing in it while the kernel runs on the other core would
 * overwrite data the kernel is still copying out of it.
 */
static uint8_t log_verify[sizeof(out_buf) + 8192] __attribute__((aligned(0x2000)));
static const char *staged_data = out_buf;   /* what "upload" sends: normally the log in out_buf */
static uint64_t staged_len;
/*
 * The kernel's log of the last run, once it is back (write_logs): every later write of the log (flush_log(), the one
 * before the restart among them) keeps it behind the loader's. Before, the restart's flush put the loader's log alone
 * into the RAM log, over the kernel's. run_kernel() forgets it when the next kernel is loaded.
 */
static const char *last_klog;
static uint64_t last_klog_len;

static void
compose_logs(const char *klog, uint64_t klog_len)
{
	static const char sep[] = "\n== KERNEL LOG ==\n", end[] = "\n== END OF LOG ==\n";
	uint64_t n = log_len;

	staged_data = out_buf;
	memset(out_buf, 0, sizeof(out_buf));
	memcpy(out_buf, log_buf, log_len);
	if (klog != NULL) {
		if (klog_len > sizeof(out_buf) - n - 8192) {
			klog_len = sizeof(out_buf) - n - 8192;
		}
		memcpy(out_buf + n, sep, sizeof(sep) - 1);
		n += sizeof(sep) - 1;
		memcpy(out_buf + n, klog, klog_len);
		n += klog_len;
		memcpy(out_buf + n, end, sizeof(end) - 1);
		n += sizeof(end) - 1;
	}
	staged_len = n;
}

static int hc_list_is_uefis(void);
static int hc_log_skips;                /* logs not written this run (said twice at most) */
static void hc_print_state(const char *when);
static void hc_take_back(const char *when);

/*
 * A phone with no log partition (the Galaxy S9: logdump is Qualcomm's) keeps the log in RAM that a restart leaves
 * alone, as one of Linux's persistent RAM buffers (fs/pstore/ram_core.c: "DBGC", the start, the size, the bytes), in a
 * place its own kernel's pstore reads back from: after the next start of Android or TWRP the log is a file in
 * /sys/fs/pstore. What fits: the head of the loader's log and the tail of everything after it.
 */
static uint64_t ram_log_base, ram_log_size;     /* the platform's; 0: none */

static void
write_ram_log(void)
{
	struct { uint32_t sig, start, size; } *h = (void *)(uintptr_t)ram_log_base;
	uint8_t *data = (uint8_t *)(uintptr_t)ram_log_base + sizeof(*h);
	uint64_t room = ram_log_size - sizeof(*h), head = 3072, n = staged_len;
	static const char cut[] = "\n[...]\n";

	if (ram_log_base == 0 || ram_log_size <= sizeof(*h) + head + sizeof(cut)) {
		return;
	}
	if (n <= room) {
		memcpy(data, staged_data, n);
	} else {
		memcpy(data, staged_data, head);
		memcpy(data + head, cut, sizeof(cut) - 1);
		memcpy(data + head + sizeof(cut) - 1, staged_data + n - (room - head - (sizeof(cut) - 1)),
		    room - head - (sizeof(cut) - 1));
		n = room;
	}
	h->sig = 0x43474244;    /* "DBGC" */
	h->start = 0;
	h->size = (uint32_t)n;
	clean_dcache(ram_log_base, ram_log_base + sizeof(*h) + n);
}

/*
 * The pstore's other half, where the platform has one (ram_dump): Linux's ramoops keeps there one record of the kind a
 * panic leaves (dmesg-ramoops-0 in /sys/fs/pstore), and it keeps it only behind its header line ("====sec.nsec-D",
 * fs/pstore/ram.c, ramoops_read_kmsg_hdr). It gets the kernel's log from its start, where ram_log has the end: twice
 * the log of a run that comes back.
 */
static uint64_t ram_dump_base, ram_dump_size;   /* the platform's; 0: none */

/* At the start: no record of a run before, which this one could be taken for if its kernel never comes back */
static void
clear_ram_dump(void)
{
	if (ram_dump_base != 0 && ram_dump_size >= sizeof(uint32_t)) {
		*(volatile uint32_t *)(uintptr_t)ram_dump_base = 0;
		clean_dcache(ram_dump_base, ram_dump_base + sizeof(uint32_t));
	}
}

static void
write_ram_dump(const char *klog, uint64_t klog_len)
{
	struct { uint32_t sig, start, size; } *h = (void *)(uintptr_t)ram_dump_base;
	uint8_t *data = (uint8_t *)(uintptr_t)ram_dump_base + sizeof(*h);
	static const char hdr[] = "====0.0-D\n== THE KERNEL'S LOG FROM ITS START (its end: pmsg-ramoops-0) ==\n";
	uint64_t room, n;

	/*
	 * Only a kernel's log is written: flush_log() comes here with none after it (round 3 on the S9 lost the record
	 * that way). Last run's record is taken away once, at the start (clear_ram_dump).
	 */
	if (ram_dump_base == 0 || ram_dump_size <= sizeof(*h) + sizeof(hdr) || klog == NULL || klog_len == 0) {
		return;
	}
	room = ram_dump_size - sizeof(*h) - (sizeof(hdr) - 1);
	n = klog_len < room ? klog_len : room;
	memcpy(data, hdr, sizeof(hdr) - 1);
	memcpy(data + sizeof(hdr) - 1, klog, n);
	h->sig = 0x43474244;    /* "DBGC" */
	h->start = 0;
	h->size = (uint32_t)(sizeof(hdr) - 1 + n);
	clean_dcache(ram_dump_base, ram_dump_base + sizeof(*h) + h->size);
}

static void wdt_pet(void);
static uint64_t timer_hz(void);

/*
 * The returned kernel's log on the console, for a machine with no log partition (QEMU, the Galaxy S9). Not into the
 * loader's own log (log_buf, 32 KB): the kernel's is in the staged log already, behind the loader's. On the S9 the
 * console is UEFI's text console on the panel, a character at a time: a run's log of a megabyte or two takes minutes
 * there. Meanwhile the watchdog is fed (it bit in the middle of the echo before: a reset that loses INFORM3, so the
 * phone came back into BOOT, not TWRP), and where the log is in a RAM log already the echo stops after ECHO_SECONDS.
 */
#define ECHO_SECONDS    20

static void
echo_kernel_log(const char *klog, uint64_t klog_len)
{
	static const char cut[] = "\n[... the rest is in the RAM log: not shown here ...]";
	uint64_t started = READ_SYSREG(CNTVCT_EL0), limit = ram_log_base != 0 ? ECHO_SECONDS * timer_hz() : 0;
	char16 w[3] = { 0, 0, 0 };

	print("\n== KERNEL LOG ==\n");
	if (ST->con_out != NULL && !screen_quiet) {
		for (uint64_t i = 0; i < klog_len; i++) {
			if ((i & 1023) == 0) {
				wdt_pet();
				if (limit != 0 && READ_SYSREG(CNTVCT_EL0) - started >= limit) {
					for (const char *c = cut; *c; c++) {
						w[0] = (char16)*c, w[1] = 0;
						ST->con_out->output_string(ST->con_out, w);
					}
					print("\n(the console's echo of the kernel's log stopped after %d s, at byte %d of %d)",
					    (uint64_t)ECHO_SECONDS, i, klog_len);
					break;
				}
			}
			if (klog[i] == '\n') {
				w[0] = '\r', w[1] = '\n';
			} else {
				w[0] = (char16)klog[i], w[1] = 0;
			}
			ST->con_out->output_string(ST->con_out, w);
		}
		wdt_pet();
	}
	print("\n== END OF LOG ==\n");
}

static void
write_logs_echo(const char *klog, uint64_t klog_len, int echo)
{
	efi_block_io *bio = find_log_partition();

	compose_logs(klog, klog_len);
	write_ram_log();
	write_ram_dump(klog, klog_len);
	if (bio == NULL) {
		/* No log partition (QEMU, the S9): echo the returned kernel log on the console instead. */
		if (klog != NULL && echo) {
			echo_kernel_log(klog, klog_len);
			/* and the log again with the lines the echo added, the kernel's still behind them */
			compose_logs(klog, klog_len);
			write_ram_log();
		}
		return;
	}
	uint64_t bs = bio->media->block_size;
	uint64_t len = (staged_len + 1 + bs - 1) / bs * bs;
	efi_status ws, rs = 0;
	int same = 0;

	/* only through UEFI's own transfer list (see hc_take_back) */
	if (!hc_list_is_uefis()) {
		if (hc_log_skips++ < 2) {
			hc_print_state("log not written");
			print("log: NOT WRITTEN to logdump: UEFI's transfer list is not the one in the controller\n");
		}
		compose_logs(klog, klog_len);
		return;
	}
	ws = bio->write_blocks(bio, bio->media->media_id, 0, len, out_buf);

	if (!EFI_ERROR(ws)) {
		bio->flush_blocks(bio);
		/*
		 * And read back: that the log is in the partition is what a run of the kernel's own UFS
		 * driver must leave possible (the controller is shared with this one). The line goes into
		 * the copy the host fetches over USB, not into the partition, which already has the rest.
		 */
		if (len <= sizeof(log_verify)) {
			rs = bio->read_blocks(bio, bio->media->media_id, 0, len, log_verify);
			flush_dcache((uint64_t)(uintptr_t)log_verify, (uint64_t)(uintptr_t)log_verify + len);
			same = !EFI_ERROR(rs) && memcmp(log_verify, out_buf, len) == 0;
		}
	}
	print("log: %d bytes written to logdump (%x), read back %s (%x)\n", len, ws,
	    EFI_ERROR(ws) ? "not tried" : same ? "the same" : "DIFFERENT", rs);
	compose_logs(klog, klog_len);
}

/* the log, and a kernel's that came back (kept for the writes after it: last_klog) */
static void
write_logs(const char *klog, uint64_t klog_len)
{
	if (klog != NULL) {
		last_klog = klog;
		last_klog_len = klog_len;
	}
	write_logs_echo(klog, klog_len, 1);
}

/* the loader's log as it is now, the last run's kernel log (if there is one) behind it, not echoed again */
static void
flush_log(void)
{
	write_logs_echo(last_klog, last_klog_len, 0);
}
