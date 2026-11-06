#!/usr/bin/env python3
"""A disk image with a GUID partition table of 4096-byte blocks (a UFS logical unit's), for QEMU's -device ufs-lu.

usage: mkgpt.py OUT NAME:MIB[:FILE] ...

Partitions follow one another from 1 MiB, each on a 1 MiB boundary, in the order given; FILE, if given, is copied to the start
of the partition (and must fit). The image is a sparse file as large as the partitions and both tables need. The GUIDs are made
from the names, so the same arguments make the same image. Nothing here is a phone's: names and sizes are the test's.
"""
import struct
import sys
import uuid
import zlib

BLOCK = 4096
MIB = 1 << 20
ENTRIES, ENTRY_SIZE = 128, 128
TABLE_BLOCKS = ENTRIES * ENTRY_SIZE // BLOCK
DATA = uuid.UUID('0FC63DAF-8483-4772-8E79-3D69D8477DE4')


def header(current, backup, first, last, disk, table_lba, table_crc):
    h = struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0, current, backup, first, last, disk.bytes_le, table_lba,
                    ENTRIES, ENTRY_SIZE, table_crc)
    h = h[:16] + struct.pack('<I', zlib.crc32(h) & 0xffffffff) + h[20:]
    return h.ljust(BLOCK, b'\0')


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    out, specs = argv[1], [s.split(':') for s in argv[2:]]
    parts, lba = [], MIB // BLOCK
    for spec in specs:
        name, mib = spec[0], int(spec[1])
        blocks = mib * MIB // BLOCK
        parts.append((name, lba, blocks, spec[2] if len(spec) > 2 else None))
        lba += blocks
    total = lba + MIB // BLOCK
    first, last = 2 + TABLE_BLOCKS, total - 2 - TABLE_BLOCKS
    disk = uuid.uuid5(uuid.NAMESPACE_URL, 'mkgpt:' + ' '.join(argv[2:]))
    table = bytearray(ENTRIES * ENTRY_SIZE)
    for i, (name, start, blocks, _) in enumerate(parts):
        table[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE] = struct.pack('<16s16sQQQ72s', DATA.bytes_le, uuid.uuid5(disk, name).bytes_le,
                                                                  start, start + blocks - 1, 0, name.encode('utf-16-le'))
    crc = zlib.crc32(table) & 0xffffffff
    with open(out, 'wb') as f:
        f.truncate(total * BLOCK)
        mbr = bytearray(BLOCK)
        mbr[446:462] = struct.pack('<BBBBBBBBII', 0, 0, 2, 0, 0xee, 0xff, 0xff, 0xff, 1, min(total - 1, 0xffffffff))
        mbr[510:512] = b'\x55\xaa'
        f.write(mbr)
        f.write(header(1, total - 1, first, last, disk, 2, crc))
        f.write(table)
        f.seek((total - 1 - TABLE_BLOCKS) * BLOCK)
        f.write(table)
        f.write(header(total - 1, 1, first, last, disk, total - 1 - TABLE_BLOCKS, crc))
        for name, start, blocks, path in parts:
            if path is None:
                continue
            f.seek(start * BLOCK)
            with open(path, 'rb') as src:
                n = 0
                while chunk := src.read(8 * MIB):
                    n += len(chunk)
                    if n > blocks * BLOCK:
                        sys.exit('mkgpt: %s does not fit in %s' % (path, name))
                    f.write(chunk)
    print('mkgpt: %s: %d blocks of %d, %d partitions' % (out, total, BLOCK, len(parts)))


main(sys.argv)
