#!/usr/bin/env python3
"""
Pack a phone's UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image: for `fastboot boot` on redwood, to flash on starlte (Samsung has no fastboot).

  redwood: kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB
  starlte: kernel = BootShim + UEFI FD + ["XNUP" container], uncompressed and without a DTB (Project Silicium's
           starlte.toml: Samsung's boot loader takes no gzip, and its device tree from the phone's dtb partition)

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see uefi/README.md)
or, when there is none, from devices/<device>/prebuilt/.

usage: mkxnuimg.py [-p payload.bin] [-o out.img]

The device and what it takes are set in the environment:
  UEFI_TARGET   Mu-Silicium device name: redwood (default) or starlte    UEFI_FD, DTB   files to use instead
  FD_BASE, FD_SIZE, PAYLOAD_BASE    addresses from its UEFI port's memory map (defaults: the device's, in DEVICES below)
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')
def env_int(name, default):
    v = os.environ.get(name, '')
    return int(v, 0) if v else default
# what each device's boot loader takes (its Mu-Silicium Resources/Configs/<device>.toml) and where its UEFI image lives
DEVICES = {
    'redwood': dict(fd_base=0x9FD00000, gzip=True, append_dtb=True, dtb_arg=False),
    # (Samsung's boot loader reads the device tree from the phone's own dtb partition: none goes in the image)
    'starlte': dict(fd_base=0x8F000000, gzip=False, append_dtb=False, dtb_arg=False),
}
TARGET = os.environ.get('UEFI_TARGET') or 'redwood'
if TARGET not in DEVICES:
    sys.exit(f'mkxnuimg.py: no packing known for {TARGET} (DEVICES in this file: {", ".join(DEVICES)})')
DEV = DEVICES[TARGET]
PREBUILT = os.path.join(HERE, 'devices', TARGET, 'prebuilt')
FD_BASE, FD_SIZE = env_int('FD_BASE', DEV['fd_base']), env_int('FD_SIZE', 0x200000)
PAYLOAD_BASE, PAYLOAD_MAX = env_int('PAYLOAD_BASE', 0xF0000000), 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output', default=os.path.join(HERE, f'xnu-{TARGET}.img'))
ap.add_argument('--mode', default='RELEASE')
args = ap.parse_args()

fd_path = os.environ.get('UEFI_FD') or os.path.join(MU, f'Build/{TARGET}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path) and os.path.isdir(os.path.join(MU, 'BootShim')):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, f'Resources/DTBs/{TARGET}.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
else:
    parts = {'shim': os.path.join(PREBUILT, 'BootShim.bin'), 'fd': os.path.join(PREBUILT, 'SILICIUM_UEFI.fd'),
             'dtb': os.path.join(PREBUILT, 'device-tree.dtb'), 'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')}
    if os.environ.get('UEFI_FD'): parts['fd'] = os.environ['UEFI_FD']
if not (DEV['append_dtb'] or DEV['dtb_arg']):
    del parts['dtb']
for name, path in parts.items():
    if not os.path.isfile(path):
        hint = ''
        if name == 'fd':
            hint = ("\n  The UEFI image is not in the repository: it carries the phone vendor's own drivers, which are not\n"
                    "  redistributed here. Build it from your own phone's firmware: uefi/README.md")
        sys.exit(f'mkxnuimg.py: {path} not found{hint}')
shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
assert len(fd) == FD_SIZE, hex(len(fd))

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

if os.environ.get('DTB'): parts['dtb'] = os.environ['DTB']
dtb = open(parts['dtb'], 'rb').read() if 'dtb' in parts else b''
kernel = args.output + '.kernel~'
open(kernel, 'wb').write((gzip.compress(blob, 6) if DEV['gzip'] else blob) + (dtb if DEV['append_dtb'] else b''))
try:
    subprocess.check_call([sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
                           '--header_version', '1', '-o', args.output] +
                          (['--dtb', parts['dtb']] if DEV['dtb_arg'] else []))
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
