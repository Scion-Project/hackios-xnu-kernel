/*
 * Boot chooser (CHOOSER=1, the Galaxy S9 only): Volume Up held at power-on starts Android, anything else starts the
 * kernel in the payload, as it always did. Called from efi_main before anything of the kernel's run is set up.
 *
 * It writes no partition: the only storage calls here are read_blocks (checked by tools/chooser-test.sh), and a failure
 * anywhere on the Android side prints why and returns, so the phone boots the payload as if the chooser were not there.
 *
 * The Android side, as S-Boot does it for a stock phone -- a raw arm64 Image, a ramdisk and a device tree in RAM, the
 * MMU and caches off, x0 = the device tree, x1-x3 = 0 (Documentation/arm64/booting.txt), at EL1 -- and what is verified:
 *   verified on the host (tools/chooser-test.sh): the boot image header parse, the device tree edit, the key decode.
 *   built for the S9 (xnuloader/build.sh CHOOSER=1, clang): compiles and links; not run on a phone.
 *   guesses, none checked on a phone: the three load addresses (Starbite's, below) are free in this UEFI's map; the
 *   device tree in the image (or UEFI's, when the image has none) needs only /chosen set; the kernel starts from UEFI's
 *   state (its other cores, its timer, the GIC) as it does from S-Boot's; Volume Up alone at power-on reaches this
 *   loader (S-Boot's own recovery combo is Volume Up + Bixby + Power, and it runs first).
 */
#include "chooser.h"

#ifndef CHOOSER_PART
#define CHOOSER_PART    "SCIONSTORE"    /* the partition whose first block is an Android boot image */
#endif
#ifndef CHOOSER_WINDOW_MS
#define CHOOSER_WINDOW_MS 1500          /* how long the key is looked for; S-Boot, then UEFI, take seconds to get here */
#endif
/* Starbite's S9 handoff addresses (shim/starlte/xnu_boot.c, boot_android; GPL-2.0-only, the numbers are facts) */
#ifndef CHOOSER_KERNEL_PA
#define CHOOSER_KERNEL_PA 0x92080000ULL
#define CHOOSER_RAMDISK_PA 0xA8000000ULL
#define CHOOSER_FDT_PA   0xAD000000ULL
#endif
#define CHOOSER_FDT_ROOM 0x10000         /* the tree, edited, is built in this much */

static int
chooser_volup_held(void)
{
	int run = 0;

	for (unsigned ms = 0;; ms += 10) {
		if (cz_debounce(&run, cz_volup(*(volatile uint32_t *)CZ_GPA0_DAT))) {
			return 1;
		}
		if (ms >= CHOOSER_WINDOW_MS) {
			return 0;
		}
		BS->stall(10000);
	}
}

/* pages for [pa, pa + bytes): exactly there, or not at all */
static int
chooser_take(uint64_t pa, uint64_t bytes, const char *what)
{
	uint64_t a = pa;

	if (EFI_ERROR(BS->allocate_pages(2 /* AllocateAddress */, 2 /* EfiLoaderData */, (bytes + 0xfff) >> 12, &a))) {
		print("Scion chooser: %s: %x is not free in this firmware's map\n", what, pa);
		return 0;
	}
	return 1;
}

/* read bytes of the partition from its byte offset into pa; the sizes are rounded up to whole blocks */
static int
chooser_read(efi_block_io *bio, uint64_t off, uint64_t bytes, uint64_t pa)
{
	uint64_t bs = bio->media->block_size;
	uint64_t n = (bytes + bs - 1) / bs * bs;

	return !EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, off / bs, n, (void *)(uintptr_t)pa));
}

static efi_block_io *
chooser_find(const char *name)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID, blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	efi_block_io *found = NULL;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count && found == NULL; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char n[40];

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			continue;
		}
		ufs_name(info, n, sizeof(n));
		if (str_eq(n, name) && bio->media->block_size != 0) {
			found = bio;
		}
	}
	BS->free_pool(handles);
	return found;
}

/* does not return when it starts Android */
static void
chooser_android(void)
{
	static const efi_guid dtb_guid = EFI_DTB_TABLE_GUID;
	static uint8_t hdr[CZ_BOOT_HDR_MAX + 4096] __attribute__((aligned(4096)));
	struct cz_bootimg bi;
	efi_block_io *bio = chooser_find(CHOOSER_PART);
	const uint8_t *dt = NULL;
	uint32_t dt_size = 0, fdt_size;
	uint64_t bs, part_bytes, ramdisk_end;
	const char *why;

	if (bio == NULL) {
		print("Scion chooser: no partition %s\n", CHOOSER_PART);
		return;
	}
	bs = bio->media->block_size, part_bytes = (bio->media->last_block + 1) * bs;
	if (bs > 4096 || !chooser_read(bio, 0, bs > CZ_BOOT_HDR_MAX ? bs : (CZ_BOOT_HDR_MAX + bs - 1) / bs * bs,
	    (uint64_t)(uintptr_t)hdr)) {
		print("Scion chooser: %s: the header cannot be read\n", CHOOSER_PART);
		return;
	}
	if ((why = cz_bootimg_parse(hdr, part_bytes, &bi)) != NULL) {
		print("Scion chooser: %s: %s\n", CHOOSER_PART, why);
		return;
	}
	if ((READ_SYSREG(CurrentEL) >> 2) != 1) {
		print("Scion chooser: running at EL%d, the handoff is for EL1\n", READ_SYSREG(CurrentEL) >> 2);
		return;
	}
	if (!chooser_take(CHOOSER_KERNEL_PA, bi.kernel_size, "kernel") ||
	    !chooser_take(CHOOSER_RAMDISK_PA, bi.ramdisk_size, "ramdisk") ||
	    !chooser_take(CHOOSER_FDT_PA, CHOOSER_FDT_ROOM + CZ_DTB_MAX, "device tree")) {
		return;
	}
	if (!chooser_read(bio, bi.kernel_off, bi.kernel_size, CHOOSER_KERNEL_PA) ||
	    (bi.ramdisk_size != 0 && !chooser_read(bio, bi.ramdisk_off, bi.ramdisk_size, CHOOSER_RAMDISK_PA))) {
		print("Scion chooser: the kernel or ramdisk cannot be read\n");
		return;
	}
	/* a raw arm64 Image has "ARM\x64" at 56; a gzip is not unpacked here */
	if (cz_le32((const uint8_t *)(uintptr_t)CHOOSER_KERNEL_PA + 56) != 0x644d5241) {
		print("Scion chooser: the kernel is not a raw arm64 Image (gzip is not unpacked here)\n");
		return;
	}
	/* the tree: the image's, else the firmware's (at the end of the DTB table's pointer: its size is in its header) */
	if (bi.dtb_size != 0) {
		if (!chooser_read(bio, bi.dtb_off, bi.dtb_size, CHOOSER_FDT_PA + CHOOSER_FDT_ROOM)) {
			print("Scion chooser: the device tree cannot be read\n");
			return;
		}
		dt = (const uint8_t *)(uintptr_t)(CHOOSER_FDT_PA + CHOOSER_FDT_ROOM), dt_size = bi.dtb_size;
	} else {
		for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables; i++) {
			if (guid_eq(&ST->tables[i].guid, &dtb_guid)) {
				dt = ST->tables[i].table;
				dt_size = cz_be32(dt + 4);
			}
		}
	}
	ramdisk_end = CHOOSER_RAMDISK_PA + bi.ramdisk_size;
	if (dt == NULL || (fdt_size = cz_fdt_set_chosen(dt, dt_size, (uint8_t *)(uintptr_t)CHOOSER_FDT_PA, CHOOSER_FDT_ROOM,
	    bi.cmdline, CHOOSER_RAMDISK_PA, ramdisk_end)) == 0) {
		print("Scion chooser: no usable device tree\n");
		return;
	}
	print("Scion chooser: starting Android: kernel %d, ramdisk %d, tree %d bytes\n", (uint64_t)bi.kernel_size,
	    (uint64_t)bi.ramdisk_size, (uint64_t)fdt_size);
	clean_dcache(CHOOSER_KERNEL_PA, CHOOSER_KERNEL_PA + bi.kernel_size);
	clean_dcache(CHOOSER_RAMDISK_PA, ramdisk_end);
	clean_dcache(CHOOSER_FDT_PA, CHOOSER_FDT_PA + fdt_size);
	__asm__ volatile (
		"msr daifset, #0xf\n"
		"mrs x2, sctlr_el1\n"
		"bic x2, x2, #0x1\n"            /* M */
		"bic x2, x2, #0x4\n"            /* C */
		"bic x2, x2, #0x1000\n"         /* I */
		"msr sctlr_el1, x2\n"
		"isb\n"
		"ic iallu\n"
		"dsb sy\n"
		"isb\n"
		"mov x0, %1\n"
		"mov x1, xzr\n"
		"mov x2, xzr\n"
		"mov x3, xzr\n"
		"br %0\n"
		: : "r" ((uint64_t)CHOOSER_KERNEL_PA), "r" ((uint64_t)CHOOSER_FDT_PA) : "x0", "x1", "x2", "x3");
	__builtin_unreachable();
}

static void
chooser_run(void)
{
	print("\n   S C I O N   hold Volume Up for Android, else iOS\n\n");
	if (chooser_volup_held()) {
		chooser_android();
		print("Scion chooser: Android did not start, booting iOS\n");
	} else {
		print("Scion chooser: iOS\n");
	}
}
