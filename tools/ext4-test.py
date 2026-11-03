#!/usr/bin/env python3
"""Host-side checks of the kernel's read-only ext4 reader (xnu/files/bsd/sys/hakios.ext4.h), without the phone.

usage: tools/ext4-test.py

The reader is compiled with the host's cc into a small program that prints a file of an image by its path. File
systems are made with e2fsprogs' mke2fs -d from a directory laid out like the Galaxy S9's VENDOR partition (made-up
contents: no vendor file is used), in the shapes the reader has to read:

  1. ext4 as Android makes VENDOR: 4 KB blocks, extents, 64bit, flex_bg, metadata_csum, dir_index; a large file in
     several extents, a sparse file (holes read as zeroes), a directory of 600 entries (an htree: e2fsck -D), short, long,
     relative and absolute (/vendor/...) symbolic links, and a link out of the partition refused;
  2. the same with 1 KB blocks and without 64bit (32-byte group descriptors, the superblock in block 1);
  3. ext2 (no extents: the block map, through single and double indirect blocks of 1 KB blocks);
  4. ext4 with inline_data: small files and directories in their inodes;
  5. what is refused: a missing file (ENOENT), a directory (EFTYPE), a file larger than allowed (EFBIG), not ext4.

Every file read is compared with the one it was made from. Skipped (exit status 0, said) without mke2fs.
Exit status 0 when everything holds.
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
failed = []


def check(what, ok):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failed.append(what)


HARNESS = r"""
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "hakios.ext4.h"

static FILE *image;
static int rd(void *c, uint64_t off, void *dst, uint64_t len)
{
	(void)c;
	if (fseeko(image, (off_t)off, SEEK_SET) != 0) return EIO;
	return fread(dst, 1, (size_t)len, image) == len ? 0 : EIO;
}
static void *al(uint64_t n) { return malloc((size_t)n); }
static void fr(void *p, uint64_t n) { (void)n; free(p); }

int main(int argc, char **argv)
{
	struct hakios_ext4 fs;
	uint8_t *data;
	uint64_t size, max = argc > 4 ? strtoull(argv[4], NULL, 0) : (64u << 20);
	int e;

	image = fopen(argv[1], "rb");
	if (image == NULL) return 100;
	e = hakios_ext4_open(&fs, rd, NULL, argv[3], al, fr);
	if (e != 0) { printf("open %d\n", e); return 0; }
	e = hakios_ext4_file(&fs, argv[2], max, &data, &size);
	if (e != 0) { printf("error %d\n", e); hakios_ext4_close(&fs); return 0; }
	fwrite(data, 1, (size_t)size, stdout);
	fr(data, size);
	hakios_ext4_close(&fs);
	return 0;
}
"""


def tree(top, rng):
    """A made-up VENDOR: its names, not its contents."""
    files = {}

    def put(rel, data):
        path = os.path.join(top, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        files[rel] = data

    put("firmware/bcmdhd_sta.bin_b2", rng.randbytes(731_337))
    put("firmware/nvram.txt_r02a_b2", b"boardtype=0x0\nmacaddr=00:90:4c:00:00:00\n" * 40)
    put("firmware/bcmdhd_clm.blob", rng.randbytes(12_345))
    put("firmware/bcm4361B2_murata.hcd", rng.randbytes(106_000))
    put("etc/wifi/tiny", b"x")
    put("etc/wifi/empty", b"")
    put("etc/wifi/big", rng.randbytes(9 << 20))     # past the double-indirect blocks of 1 KB ext2
    for i in range(600):
        put("etc/many/file-%04d" % i, b"%d\n" % i)
    os.symlink("../../firmware/bcmdhd_sta.bin_b2", os.path.join(top, "etc/wifi/bcmdhd_sta.bin_b2"))
    os.symlink("/vendor/firmware/nvram.txt_r02a_b2", os.path.join(top, "etc/wifi/nvram.txt"))
    os.symlink("/vendor/" + "d/" * 40 + "../" * 40 + "firmware/bcmdhd_clm.blob", os.path.join(top, "etc/wifi/long"))
    os.makedirs(os.path.join(top, "d"), exist_ok=True)
    path = top
    for _ in range(40):
        path = os.path.join(path, "d")
        os.makedirs(path, exist_ok=True)
    os.symlink("/system/etc/hosts", os.path.join(top, "etc/wifi/out"))
    os.symlink("loop2", os.path.join(top, "etc/wifi/loop1"))
    os.symlink("loop1", os.path.join(top, "etc/wifi/loop2"))
    files["etc/wifi/bcmdhd_sta.bin_b2"] = files["firmware/bcmdhd_sta.bin_b2"]
    files["etc/wifi/nvram.txt"] = files["firmware/nvram.txt_r02a_b2"]
    files["etc/wifi/long"] = files["firmware/bcmdhd_clm.blob"]
    return files


def sparse(path):
    with open(path, "wb") as f:
        f.write(b"A" * 5000)
        f.seek(3 << 20)
        f.write(b"B" * 7000)
    with open(path, "rb") as f:
        return f.read()


def main():
    mke2fs = shutil.which("mke2fs") or next((p for p in ("/sbin/mke2fs", "/usr/sbin/mke2fs") if os.path.exists(p)), None)
    if mke2fs is None:
        print("skipped: no mke2fs on this machine (e2fsprogs)")
        return 0
    fsck = shutil.which("e2fsck") or next((p for p in ("/sbin/e2fsck", "/usr/sbin/e2fsck") if os.path.exists(p)), None)
    rng = random.Random(4361)
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "ext4cat")
        src = os.path.join(tmp, "ext4cat.c")
        with open(src, "w") as f:
            f.write(HARNESS)
        cc = subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-I", os.path.join(ROOT, "xnu/files/bsd/sys"), src, "-o", exe],
                            capture_output=True, text=True)
        check("hakios.ext4.h compiles on the host" + (": " + cc.stderr.strip() if cc.returncode else ""), cc.returncode == 0)
        if cc.returncode:
            return 1
        top = os.path.join(tmp, "vendor")
        os.makedirs(top)
        files = tree(top, rng)
        files["etc/sparse"] = sparse(os.path.join(top, "etc/sparse"))

        def cat(image, path, mount="/vendor", max_=None):
            args = [exe, image, path, mount] + ([str(max_)] if max_ is not None else [])
            return subprocess.run(args, capture_output=True).stdout

        shapes = [
            ("ext4, 4 KB, 64bit, metadata_csum (as Android's VENDOR)", "ext4",
             ["-b", "4096", "-O", "64bit,metadata_csum,dir_index,flex_bg,extent,huge_file"], 64),
            ("ext4, 1 KB, 32-byte descriptors", "ext4", ["-b", "1024", "-O", "^64bit,^metadata_csum,dir_index"], 64),
            ("ext2, 1 KB (the block map)", "ext2", ["-b", "1024"], 64),
            ("ext4 with inline_data", "ext4", ["-b", "4096", "-O", "inline_data"], 64),
        ]
        for name, kind, options, mb in shapes:
            image = os.path.join(tmp, "img")
            if os.path.exists(image):
                os.unlink(image)
            mk = subprocess.run([mke2fs, "-q", "-F", "-t", kind] + options + ["-d", top, image, "%dM" % mb],
                                capture_output=True, text=True)
            if mk.returncode != 0:
                check("%s: mke2fs: %s" % (name, mk.stderr.strip()), False)
                continue
            if fsck is not None and "dir_index" in ",".join(options) + kind:
                # mke2fs -d writes directories as lists; e2fsck -D makes the large one an htree, as Android's are
                subprocess.run([fsck, "-fyD", image], capture_output=True)
            bad = [rel for rel, data in files.items() if cat(image, "/" + rel) != data]
            check("%s: %d files read back the same%s" % (name, len(files), "" if not bad else ": NOT " + ", ".join(bad[:5])),
                  not bad)
            check("%s: a path without the leading '/' and with '//'" % name,
                  cat(image, "firmware//bcmdhd_clm.blob") == files["firmware/bcmdhd_clm.blob"])
            check("%s: missing file: ENOENT" % name, cat(image, "/firmware/nothing") == b"error 2\n")
            check("%s: a directory is not a file: EFTYPE" % name, cat(image, "/firmware") == b"error 79\n")
            check("%s: a file larger than allowed: EFBIG" % name, cat(image, "/etc/wifi/big", max_=1 << 20) == b"error 27\n")
            check("%s: a link out of the partition: EXDEV" % name, cat(image, "/etc/wifi/out") == b"error 18\n")
            check("%s: a loop of links: ELOOP" % name, cat(image, "/etc/wifi/loop1") == b"error 62\n")
            check("%s: an absolute link without a mount point: EXDEV" % name,
                  cat(image, "/etc/wifi/nvram.txt", mount="") == b"error 18\n")
            check("%s: a file below a file: ENOTDIR" % name, cat(image, "/etc/wifi/tiny/x") == b"error 20\n")
        notfs = os.path.join(tmp, "zero")
        with open(notfs, "wb") as f:
            f.write(bytes(1 << 20))
        check("not a file system: EFTYPE at open", cat(notfs, "/x") == b"open 79\n")
    print("ext4-test: %s" % ("all good" if not failed else "%d FAILED" % len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
