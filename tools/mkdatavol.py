#!/usr/bin/env python3
"""The data volume of Darwin for the phone: an empty HFS Plus volume in darwin past the Darwin volume and the cryptexes,
as an Android sparse image of the whole of darwin that writes that volume and nothing else.

usage: mkdatavol.py OUT.simg [MKFS]      (MKFS: mkfs.hfsplus, default the one in $PATH)

    fastboot flash darwin OUT.simg

The kernel gives that stretch of darwin out as /dev/ufs_darwin_data (drivers/storage/hakios.ufs.c, ufs_write=1) and
init mounts it on /private/var (userland/src/init.c, var_hfs=ufs_darwin_data). Flashing this again empties it.

The image says DONT_CARE for darwin's blocks before the volume -- the Darwin volume and both cryptexes, which the
bootloader therefore leaves as they are -- and, inside the volume, for every block the new volume does not use: what
is in free space does not matter to HFS Plus, and twelve gigabytes of zeros need not go over USB. The blocks it uses
(the volume headers, the allocation bitmap, the catalog and extents trees, the journal) are written whole, zeros
included: which those are is read from the volume's own allocation bitmap.

Case-sensitive (HFSX), as Darwin's own data volume is, and journaled.

The Galaxy S9 has no darwin: Android's USERDATA, given up, is the data volume, all of it, from its first block
(drivers/storage/hakios.ufs.c and xnuloader/loader.c: data_part; init: var_hfs=ufs_USERDATA_data). TWRP has no
fastboot sparse images to flash, so for it:

usage: mkdatavol.py --userdata BLOCKS OUTDIR [MKFS]   (BLOCKS: USERDATA's size in blocks of 4096, as TWRP's
                                                      blockdev --getsize64 /dev/block/by-name/USERDATA / 4096 says)

makes a volume of exactly BLOCKS blocks and writes the blocks it uses, which come in two stretches: the head (the
volume header, the allocation bitmap, the extents and catalog trees, the journal) as OUTDIR/userdata-head.img.gz,
and the end (the alternate volume header) as OUTDIR/userdata-tail.img, with OUTDIR/userdata.txt saying where each
goes and the head's SHA-256 to read back. Nothing else of USERDATA is written: what was in free space does not
matter to HFS Plus. MKFS may be mkfs.hfsplus (Linux, on a file) or macOS's newfs_hfs (on the file attached with
hdiutil as a raw disk, no root needed).
"""
import gzip
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

BS = 4096
DARWIN_BLOCKS = 5242875            # darwin, in blocks of 4096 (README, "The port's partition")
DATA_FIRST = 2042368               # where the volume begins: drivers/storage/hakios.ufs.c, UFS_WRITE_FIRST
MAGIC, RAW, FILL, DONT_CARE = 0xED26FF3A, 0xCAC1, 0xCAC2, 0xCAC3


def chunk(kind, blocks, data_size):
    return struct.pack('<HHII', kind, 0, blocks, 12 + data_size)


def used_blocks(img):
    """The volume's allocation blocks marked in use, from its header and bitmap; the block size must be BS."""
    img.seek(1024)
    vh = img.read(512)
    sig, = struct.unpack('>H', vh[0:2])
    if sig not in (0x482B, 0x4858):                     # H+, HX
        sys.exit('mkdatavol.py: no HFS Plus volume header in what mkfs made')
    block_size, total = struct.unpack('>II', vh[40:48])
    if block_size != BS:
        sys.exit(f'mkdatavol.py: the volume has blocks of {block_size}, not {BS}')
    fork = vh[112:192]                                  # allocationFile: HFSPlusForkData
    extents = [struct.unpack('>II', fork[16 + 8 * i:24 + 8 * i]) for i in range(8)]
    bitmap = bytearray()
    for start, count in extents:
        if count:
            img.seek(start * BS)
            bitmap += img.read(count * BS)
    if len(bitmap) * 8 < total:
        sys.exit('mkdatavol.py: the allocation file is in more pieces than its fork says')
    return total, lambda b: bitmap[b >> 3] & (0x80 >> (b & 7))


def mkfs_file(mkfs, path):
    """An HFSX volume, journaled, its root root's and 0755, the whole of the file PATH."""
    flags = ['-s', '-J', '-b', str(BS), '-U', '0', '-G', '0', '-M', '755', '-v', 'Data']
    if os.path.basename(mkfs) != 'newfs_hfs':
        subprocess.run([mkfs] + flags + [path], check=True)
        return
    # macOS: newfs_hfs takes a disk, so the file is attached as one (a raw image, not mounted)
    out = subprocess.run(['hdiutil', 'attach', '-nomount', '-imagekey', 'diskimage-class=CRawDiskImage', path],
                         check=True, capture_output=True, text=True).stdout.split()
    disk = out[0]
    try:
        subprocess.run([mkfs] + flags + ['/dev/r' + disk[len('/dev/'):]], check=True)
    finally:
        subprocess.run(['hdiutil', 'detach', disk], check=True, capture_output=True)


def userdata(blocks, outdir, mkfs):
    """The S9's USERDATA: the volume's two used stretches, for dd from TWRP."""
    if blocks < 262144:
        sys.exit('mkdatavol.py: USERDATA has to be at least 1 GiB (262144 blocks): the kernel and the loader refuse less')
    os.makedirs(outdir, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=outdir) as tmp:
        path = os.path.join(tmp, 'data.hfs')
        with open(path, 'wb') as f:
            f.truncate(blocks * BS)                     # sparse: only what mkfs writes takes room
        mkfs_file(mkfs, path)
        with open(path, 'rb') as img:
            total, used = used_blocks(img)
            if total != blocks:
                sys.exit(f'mkdatavol.py: the volume has {total} blocks, USERDATA has {blocks}')
            # the head: up to the last used block of the first half; the tail: from the first used block of the second
            head = max(b for b in range(0, total // 2) if used(b)) + 1
            tail = min(b for b in range(total // 2, total) if used(b))
            if any(used(b) for b in range(head, tail)):
                sys.exit('mkdatavol.py: the volume uses blocks between its head and its end: not two stretches')
            img.seek(0)
            data = img.read(head * BS)
            with gzip.open(os.path.join(outdir, 'userdata-head.img.gz'), 'wb', 9) as f:
                f.write(data)
            img.seek(tail * BS)
            with open(os.path.join(outdir, 'userdata-tail.img'), 'wb') as f:
                f.write(img.read((total - tail) * BS))
    digest = hashlib.sha256(data).hexdigest()
    note = (f'USERDATA: {blocks} blocks of {BS} ({blocks * BS} bytes), an HFSX volume, journaled, the whole of it\n'
            f'head: blocks 0..{head - 1} from userdata-head.img.gz ({head * BS} bytes), sha256 {digest}\n'
            f'tail: blocks {tail}..{total - 1} from userdata-tail.img (seek={tail})\n')
    with open(os.path.join(outdir, 'userdata.txt'), 'w') as f:
        f.write(note)
    print(note, end='')


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == '--userdata':
        if len(sys.argv) not in (4, 5):
            sys.exit(__doc__)
        userdata(int(sys.argv[2]), sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else 'mkfs.hfsplus')
        return
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    out = sys.argv[1]
    mkfs = sys.argv[2] if len(sys.argv) > 2 else 'mkfs.hfsplus'
    blocks = DARWIN_BLOCKS - DATA_FIRST
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'data.hfs')
        with open(path, 'wb') as f:
            f.truncate(blocks * BS)
        # its root is /private/var: root's, 0755, as on an iPhone
        subprocess.run([mkfs, '-s', '-J', '-b', str(BS), '-U', '0', '-G', '0', '-M', '755', '-v', 'Data', path],
                       check=True)
        with open(path, 'rb') as img:
            total, used = used_blocks(img)
            if total != blocks:
                sys.exit(f'mkdatavol.py: the volume has {total} blocks, darwin has {blocks} for it')
            runs = []                                   # [kind, first, count], in order
            for b in range(total):
                kind = RAW if used(b) else DONT_CARE
                if runs and runs[-1][0] == kind and runs[-1][1] + runs[-1][2] == b:
                    runs[-1][2] += 1
                else:
                    runs.append([kind, b, 1])
            chunks, written = [chunk(DONT_CARE, DATA_FIRST, 0)], 0
            for kind, first, count in runs:
                if kind == DONT_CARE:
                    chunks.append(chunk(DONT_CARE, count, 0))
                    continue
                img.seek(first * BS)
                data = img.read(count * BS)
                # zeros in a used stretch go as FILL, which costs nothing to send; everything else as RAW
                at = 0
                while at < count:
                    zero = data[at * BS:(at + 1) * BS] == bytes(BS)
                    end = at
                    while end < count and (data[end * BS:(end + 1) * BS] == bytes(BS)) == zero:
                        end += 1
                    if zero:
                        chunks.append(chunk(FILL, end - at, 4) + bytes(4))
                    else:
                        chunks.append(chunk(RAW, end - at, (end - at) * BS) + data[at * BS:end * BS])
                    written += end - at
                    at = end
    with open(out, 'wb') as f:
        f.write(struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, BS, DARWIN_BLOCKS, len(chunks), 0))
        for c in chunks:
            f.write(c)
    print(f'{out}: {os.path.getsize(out)} bytes; the volume: {blocks} blocks ({blocks * BS >> 20} MB) from darwin '
          f'block {DATA_FIRST}, {written} of them written, the rest of darwin left as it is')


if __name__ == '__main__':
    main()
