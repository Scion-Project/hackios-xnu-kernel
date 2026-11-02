#!/usr/bin/env python3
"""A picture of the phone's screen over the USB console (drivers/core/hakios.dwc3.c, usb_gadget=5).

usage: tools/usb-shot.py [-p PORT] [-n N] [-o FILE.png] [--log FILE] [--timeout S] [--raw FILE]

Sends "shot N" to the kernel's line console and reads the answer from the same serial port: the log up to the
command, then a framed picture,

    SHOT <width> <height> <stride> rgb888 <bytes>\\n   <bytes of pixels, rows top to bottom, R G B>   END\\n

and writes it as a PNG (zlib only, no PIL). N (1-16, default 4) keeps every Nth pixel of every Nth row of the panel
(the Galaxy S9's 1440x2960 at N=4 is 360x740, 0.8 MB, some 0.1 s at high speed; N=1 is the whole panel, 12.8 MB).

  -p PORT      the serial port (default: the first /dev/cu.usbmodem* on a Mac, /dev/ttyACM* on Linux)
  -o FILE      the PNG (default shot-<date>-<time>.png)
  --log FILE   append the log text read before and after the picture to FILE (to stand in for a log reader)
  --raw FILE   also write the raw pixels as they came
  --timeout S  give up after S seconds without the picture (default 20)

Only one program may read the port at a time: a second reader (cat, screen, ~/s9/usblog.sh) takes half of the
bytes and the picture comes out broken. Stop it first; --log keeps its file going while this runs. The kernel holds
its log back while the picture goes out, so the picture is never mixed with log text.

Exit status: 0 a PNG written, 1 the kernel said the shot failed (no framebuffer...), 2 nothing usable came back.
"""
import argparse
import glob
import os
import select
import struct
import sys
import termios
import time
import tty
import zlib


def find_port():
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    return None


def png(path, width, height, rgb):
    """an 8-bit RGB PNG: IHDR, one IDAT (each row with filter 0), IEND"""
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)

    stride = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


class Port:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        self.saved = termios.tcgetattr(self.fd)
        tty.setraw(self.fd)                     # no echo, no CR/LF translation: the pixels pass as they are
        attrs = termios.tcgetattr(self.fd)
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.buf = bytearray()

    def close(self):
        try:
            termios.tcsetattr(self.fd, termios.TCSANOW, self.saved)
        finally:
            os.close(self.fd)

    def write(self, data):
        os.write(self.fd, data)

    def fill(self, deadline):
        """more bytes into the buffer; False when the deadline passed with nothing"""
        left = deadline - time.time()
        if left <= 0:
            return False
        ready, _, _ = select.select([self.fd], [], [], min(left, 0.5))
        if ready:
            data = os.read(self.fd, 1 << 16)
            if data:
                self.buf += data
        return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("-n", type=int, default=4)
    ap.add_argument("-o", "--out")
    ap.add_argument("--log")
    ap.add_argument("--raw")
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()
    if not 1 <= args.n <= 16:
        ap.error("N is from 1 to 16")
    path = args.port or find_port()
    if path is None:
        print("usb-shot: no /dev/cu.usbmodem* or /dev/ttyACM*: is the phone up with usb_gadget=5?", file=sys.stderr)
        return 2
    out = args.out or time.strftime("shot-%Y%m%d-%H%M%S.png")
    log = open(args.log, "ab") if args.log else None
    port = Port(path)
    try:
        port.write(b"\r")                       # anything half typed ends here (an empty line is ignored)
        port.write(b"shot %d\r" % args.n)
        deadline = time.time() + args.timeout
        header = None
        # the log until the header line; a "usb: shot failed" line is the kernel's no
        while header is None:
            while b"\n" in port.buf:
                line, _, rest = port.buf.partition(b"\n")
                text = bytes(line).rstrip(b"\r")
                if text.startswith(b"SHOT "):
                    header = text
                    port.buf = bytearray(rest)
                    break
                port.buf = bytearray(rest)
                if log:
                    log.write(text + b"\n")
                if b"usb: shot failed" in text:
                    print(text.decode("ascii", "replace"), file=sys.stderr)
                    return 1
                if b"usb: shot" in text or b"usb> shot" in text:
                    print(text.decode("ascii", "replace"), file=sys.stderr)
            if header is None and not port.fill(deadline):
                print("usb-shot: no SHOT header in %.0f s (is another program reading %s?)" % (args.timeout, path),
                      file=sys.stderr)
                return 2
        fields = header.split()
        if len(fields) != 6 or fields[4] != b"rgb888":
            print("usb-shot: a header not understood: %r" % header, file=sys.stderr)
            return 2
        width, height, stride, size = int(fields[1]), int(fields[2]), int(fields[3]), int(fields[5])
        if stride != width * 3 or size != stride * height:
            print("usb-shot: sizes that do not add up: %r" % header, file=sys.stderr)
            return 2
        started = time.time()
        while len(port.buf) < size + 4:
            if not port.fill(deadline + size / 1e6):
                print("usb-shot: %d of %d bytes came" % (len(port.buf), size), file=sys.stderr)
                return 2
        pixels = bytes(port.buf[:size])
        trailer = bytes(port.buf[size:size + 4])
        took = time.time() - started
        if trailer != b"END\n":
            print("usb-shot: no END after the pixels (%r): the picture may be shifted (another reader?)" % trailer,
                  file=sys.stderr)
        png(out, width, height, pixels)
        if args.raw:
            with open(args.raw, "wb") as f:
                f.write(pixels)
        # the log held back during the picture, and the kernel's "shot sent" line
        end = time.time() + 1.0
        while time.time() < end:
            port.fill(end)
        tail = bytes(port.buf[size + 4:])
        if log:
            log.write(tail.replace(b"\r", b""))
        for line in tail.split(b"\n"):
            if b"usb: shot" in line:
                print(line.rstrip(b"\r").decode("ascii", "replace"), file=sys.stderr)
        print("usb-shot: %s: %dx%d (%d bytes in %.2f s)" % (out, width, height, size, took))
        return 0 if trailer == b"END\n" else 2
    finally:
        port.close()
        if log:
            log.close()


if __name__ == "__main__":
    sys.exit(main())
