#!/bin/bash
# The Galaxy S9's boot image for one run: the boot shim, HakiOS's UEFI for starlte (prebuilt/ or a Mu-Silicium/ built
# here) and a payload with everything in it -- the loader, the kernel, the small root image, the command line. Samsung's
# boot loader has no fastboot and Silicium's UEFI no USB gadget, so there is no resident loader to send runs to as on the
# POCO: every run is an image flashed to RECOVERY (README.md, "On the phone"). The run ends by itself after SECS seconds
# (loader_return_after=): the loader keeps the log in Samsung's pstore RAM and restarts the phone.
#
#   devices/starlte/mkimage.sh [-o xnu-starlte.img] [-k kernel] [-r root.apfs] [-s SECS] [boot-args...]
#
# The kernel is the S9's own configuration (make starlte_defconfig && make kernel: hakios-kernel); the boot-arguments
# default to that configuration's (tools/hakios-config.py args). The loader is xnuloader/loader.efi, built with the
# iPhone identity from your own IPSW (make loader): the image is yours, never to be published.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
OUT=xnu-starlte.img K=hakios-kernel RD=userland/root.apfs SECS=120
while getopts o:k:r:s: opt; do
	case $opt in
	o) OUT=$OPTARG ;; k) K=$OPTARG ;; r) RD=$OPTARG ;; s) SECS=$OPTARG ;;
	*) sed -n 2,13p "$0"; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
L=xnuloader/loader.efi
for f in "$L" "$K" "$RD"; do
	[ -f "$f" ] || { echo "mkimage.sh: $f is missing (make loader, make kernel, userland/build.sh)" >&2; exit 1; }
done
ARGS="$*"
[ -n "$ARGS" ] || ARGS=$(python3 tools/hakios-config.py args)
P=$(mktemp "${TMPDIR:-/tmp}/starlte-payload.XXXXXX")
trap 'rm -f "$P"' EXIT
python3 xnuloader/mkpayload.py -o "$P" loader.efi="$L" kernel="$K" ramdisk="$RD" \
	--cmdline "loader_return_after=$SECS epoch=$(date +%s) $ARGS" >/dev/null
UEFI_TARGET=starlte python3 mkxnuimg.py -p "$P" -o "$OUT"
echo "mkimage.sh: $OUT: flash it to RECOVERY (heimdall flash --RECOVERY $OUT), start with Vol Up + Bixby + Power"
echo "mkimage.sh: boot-args: $ARGS; the run returns after $SECS s"
