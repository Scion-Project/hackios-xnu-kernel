#!/usr/bin/env python3
"""Take a seed of the data volume out of a store: the newest good slot of var_store's store, as one file.

usage: tools/var-seed.py STORE OUT [--store-at BYTES] [--slot-bytes BYTES]

STORE is a disk image that var_store saved /private/var into (userland/src/var_store.c): by default the AVF
layout, a whole second virtio disk (/dev/rrvblk1) whose two halves are the two slots; --store-at and
--slot-bytes give another (the POCO's partition darwin: --store-at 8589934592 --slot-bytes 2147483648). OUT
is that slot alone: its header block, then the archive, padded to whole blocks. userland/build.sh SEED=OUT
puts it into the small image as share/var.seed, and init unpacks it over the template with boot-arg var_seed=1;
or OUT is a read-only disk of its own, var_seed=DEV (rvblk1, say). Every boot then starts from the volume that
boot left, without a store to write into (the idea of UniXNU's RamBlock seed).

Both checksums are checked here, the header's and the archive's, so a torn slot never becomes a seed.
"""
import argparse
import os
import struct
import sys
import zlib

BLOCK = 4096
BUFFER = 4 << 20
HEAD = struct.Struct("<8sIIQQQQII")         # magic, version, slot, generation, bytes, entries, seconds, crc, head_crc
MAGIC = b"RDWVAR\0\0"


def head_at(f, offset):
    f.seek(offset)
    raw = f.read(HEAD.size)
    if len(raw) < HEAD.size:
        return None
    magic, version, slot, generation, nbytes, entries, seconds, crc, head_crc = HEAD.unpack(raw)
    if magic != MAGIC or version != 1:
        return None
    if zlib.crc32(raw[:-4] + b"\0\0\0\0") != head_crc:
        return None
    return dict(slot=slot, generation=generation, bytes=nbytes, entries=entries, seconds=seconds, crc=crc, raw=raw)


def archive_crc(f, offset, nbytes):
    f.seek(offset)
    crc, left = 0, nbytes
    while left:
        piece = f.read(min(left, BUFFER))
        if not piece:
            return None
        crc = zlib.crc32(piece, crc)
        left -= len(piece)
    return crc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("store")
    ap.add_argument("out")
    ap.add_argument("--store-at", type=int, default=0)
    ap.add_argument("--slot-bytes", type=int, default=0, help="default: half the image, rounded down to 4 MB")
    a = ap.parse_args()
    size = os.path.getsize(a.store)
    slot_bytes = a.slot_bytes or (size // 2) & ~(BUFFER - 1)
    best = None
    with open(a.store, "rb") as f:
        for slot in range(2):
            at = a.store_at + slot * slot_bytes
            h = head_at(f, at)
            if h is None or h["slot"] != slot or h["bytes"] > slot_bytes - BLOCK:
                print("var-seed: slot %d: no good header" % slot)
                continue
            if archive_crc(f, at + BLOCK, h["bytes"]) != h["crc"]:
                print("var-seed: slot %d: generation %d, the archive is not what its header says" % (slot, h["generation"]))
                continue
            print("var-seed: slot %d: generation %d, %d entries, %d bytes, written at %d" %
                  (slot, h["generation"], h["entries"], h["bytes"], h["seconds"]))
            if best is None or h["generation"] > best[1]["generation"]:
                best = (at, h)
        if best is None:
            sys.exit("var-seed: %s holds no good slot" % a.store)
        at, h = best
        f.seek(at)
        head_block = f.read(BLOCK)
        with open(a.out + ".new", "wb") as o:
            o.write(head_block)
            f.seek(at + BLOCK)
            left = h["bytes"]
            while left:
                piece = f.read(min(left, BUFFER))
                o.write(piece)
                left -= len(piece)
            # to whole blocks: the same file can then be a disk of its own (var_seed=DEV)
            o.write(bytes(-(BLOCK + h["bytes"]) % BLOCK))
        os.replace(a.out + ".new", a.out)
    print("var-seed: %s: generation %d of slot %d, %d bytes" % (a.out, h["generation"], h["slot"], os.path.getsize(a.out)))


if __name__ == "__main__":
    main()
