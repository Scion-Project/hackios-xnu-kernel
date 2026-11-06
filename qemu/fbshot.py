#!/usr/bin/env python3
"""A picture of a framebuffer in the guest's RAM, through QEMU's monitor (a Unix socket: QEMU_MONITOR= of the lga scripts).

usage: fbshot.py SOCKET OUT.png [BASE [WIDTH HEIGHT [STRIDE_BYTES]]]     defaults: 0x100000000 1080 2424 4320 (XRGB8888)
"""
import os
import socket
import struct
import sys
import tempfile
import time
import zlib


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    sock, out = argv[1], argv[2]
    base = int(argv[3], 0) if len(argv) > 3 else 0x100000000
    w, h = (int(argv[4]), int(argv[5])) if len(argv) > 5 else (1080, 2424)
    stride = int(argv[6]) if len(argv) > 6 else w * 4
    raw = tempfile.mktemp(prefix='fbshot-')
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    s.settimeout(30)
    s.recv(4096)
    s.sendall(('pmemsave %#x %d "%s"\n' % (base, stride * h, raw)).encode())
    for _ in range(300):
        if os.path.exists(raw) and os.path.getsize(raw) >= stride * h:
            break
        time.sleep(0.2)
    else:
        sys.exit('fbshot: QEMU wrote no %d bytes to %s' % (stride * h, raw))
    time.sleep(0.5)
    data = open(raw, 'rb').read()
    os.unlink(raw)
    rows = []
    for y in range(h):
        line = data[y * stride:y * stride + w * 4]
        px = bytearray(w * 3)
        px[0::3], px[1::3], px[2::3] = line[2::4], line[1::4], line[0::4]
        rows.append(b'\0' + bytes(px))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
    open(out, 'wb').write(png)
    print('fbshot: %s: %dx%d' % (out, w, h))


main(sys.argv)
