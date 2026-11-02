/*
 * Darwin-on-hakios: a file of one of the phone's own ext4 partitions, read only, for the drivers that load the user's
 * own firmware at run time instead of from an image: the Galaxy S9's Wi-Fi (drivers/wifi/hakios.brcmfmac.c: the
 * BCM4361's firmware, NVRAM and CLM) and Bluetooth (drivers/bluetooth/hakios.bcm4361_bt.c: its patch), both from the
 * VENDOR partition Android mounts at /vendor. So nothing of the vendor's is in an image, a repository or a download: the
 * phone gives its own files to the kernel that runs on it.
 *
 * The partition is read through hakios_ufs_read_partition() (hakios.ufs.c: through the loader with ufs=1 or 2, or this
 * kernel's own UFS driver), the file system by xnu/files/bsd/sys/hakios.ext4.h (host-tested on images made by mke2fs:
 * tools/ext4-test.py). Nothing is written. A file read is kept for the rest of the run (the drivers keep the pointer, as
 * they keep those of hakios_image_file()), at most PARTITION_FILES of them; asked again, it is not read again.
 *
 * Every read is logged with what was asked, where it came from, its size and how long it took (never its contents):
 *   ext4: VENDOR:/firmware/bcmdhd_sta.bin_b2: 731337 bytes in 84 ms
 *   ext4: VENDOR:/etc/wifi/bcmdhd_sta.bin_b2: not there
 * and, once, what the partition is (`ext4: VENDOR: 4096-byte blocks, 131072 of them, features 0x2c2`), or why it
 * cannot be read (no storage: ufs= not given; no partition of that name; not ext4; a feature not read).
 *
 * What was checked: tools/ext4-test.py (the reader); compiled. Not yet run on the phone.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.ext4.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern void loader_log_line(const char *line);

#define PARTITION_FILES         24

static LCK_GRP_DECLARE(ext4_lock_group, "hakios-ext4");
static LCK_MTX_DECLARE(ext4_lock, &ext4_lock_group);

static struct {
	char partition[24];
	char path[128];
	const uint8_t *data;    /* NULL: asked, and not there (error) */
	uint64_t size;
	int error;
} kept[PARTITION_FILES];
static unsigned kept_count;
static char said_partition[24];

static void
ext4_say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	printf("%s", line);
}

static int
ext4_read(void *context, uint64_t offset, void *dst, uint64_t length)
{
	return hakios_ufs_read_partition((const char *)context, offset, (uint8_t *)dst, length, NULL);
}

static void *
ext4_alloc(uint64_t size)
{
	return kalloc_data((vm_size_t)size, Z_WAITOK);
}

static void
ext4_free(void *p, uint64_t size)
{
	kfree_data(p, (vm_size_t)size);
}

static const char *
ext4_error(int error)
{
	switch (error) {
	case ENXIO: return "the phone's storage is not there (ufs=2 not given, or not yet up)";
	case ENOENT: return "not there";
	case EFTYPE: return "not a file (or the partition is not ext2/3/4)";
	case EFBIG: return "larger than the driver takes";
	case ENOTSUP: return "a feature of the file system this reader does not read";
	case ELOOP: return "too many symbolic links";
	case EXDEV: return "a symbolic link out of the partition";
	case ENOTDIR: return "a part of the path is not a directory";
	case ENOMEM: return "no memory";
	default: return "a read failed, or a structure is not right";
	}
}

/*
 * The file `path` of the ext4 partition `partition`, mounted by Android at `mount` (for absolute symbolic links: "/vendor"
 * for VENDOR), whole, if it is no larger than max bytes: 0 and *data, *size; or an errno, said in the log. The memory is
 * the kernel's for the rest of the run.
 */
int
hakios_partition_file(const char *partition, const char *mount, const char *path, uint64_t max, const uint8_t **data,
    uint64_t *size)
{
	struct hakios_ext4 fs;
	uint64_t start, end, ns;
	uint8_t *buffer = NULL;
	uint64_t length = 0;
	int error;

	*data = NULL;
	*size = 0;
	if (strlen(partition) >= sizeof(kept[0].partition) || strlen(path) >= sizeof(kept[0].path)) {
		return ENAMETOOLONG;
	}
	lck_mtx_lock(&ext4_lock);
	for (unsigned i = 0; i < kept_count; i++) {
		if (strcmp(kept[i].partition, partition) == 0 && strcmp(kept[i].path, path) == 0 && kept[i].error != EFBIG) {
			*data = kept[i].data;
			*size = kept[i].size;
			error = kept[i].error;
			lck_mtx_unlock(&ext4_lock);
			return error;
		}
	}
	clock_get_uptime(&start);
	error = hakios_ext4_open(&fs, ext4_read, (void *)(uintptr_t)partition, mount, ext4_alloc, ext4_free);
	if (error != 0) {
		/* (a missing storage or partition is said per partition, every time: it may come later) */
		ext4_say("ext4: %s: cannot be read: %s (%d)\n", partition, ext4_error(error), error);
		lck_mtx_unlock(&ext4_lock);
		return error;
	}
	if (strcmp(said_partition, partition) != 0) {
		strlcpy(said_partition, partition, sizeof(said_partition));
		ext4_say("ext4: %s: %u-byte blocks, %llu of them, %u groups, features %#x%s\n", partition, fs.block_size,
		    fs.blocks, fs.groups, fs.incompat, (fs.incompat & HAKIOS_EXT4_INCOMPAT_RECOVER) ?
		    " (its journal not replayed: Android did not unmount it cleanly; read as it is)" : "");
	}
	error = hakios_ext4_file(&fs, path, max, &buffer, &length);
	hakios_ext4_close(&fs);
	clock_get_uptime(&end);
	absolutetime_to_nanoseconds(end - start, &ns);
	if (error == 0) {
		ext4_say("ext4: %s:%s: %llu bytes in %llu ms\n", partition, path, length, ns / 1000000);
	} else if (error == EFBIG) {
		ext4_say("ext4: %s:%s: %llu bytes, %s (%llu)\n", partition, path, length, ext4_error(error), max);
	} else {
		ext4_say("ext4: %s:%s: %s (%d)\n", partition, path, ext4_error(error), error);
	}
	if (kept_count < PARTITION_FILES && (error == 0 || error == ENOENT)) {
		strlcpy(kept[kept_count].partition, partition, sizeof(kept[0].partition));
		strlcpy(kept[kept_count].path, path, sizeof(kept[0].path));
		kept[kept_count].data = buffer;
		kept[kept_count].size = length;
		kept[kept_count].error = error;
		kept_count++;
	} else if (error == 0) {
		/* the table is full: the caller still gets it, and keeps it (one more, never freed in a run) */
	}
	lck_mtx_unlock(&ext4_lock);
	*data = buffer;
	*size = length;
	return error;
}
