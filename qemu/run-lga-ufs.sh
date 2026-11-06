#!/bin/bash
# The Pixel 10's whole route in QEMU, with the system image read from a UFS disk as it would be from the phone's userdata:
# the boot shim at EL2, xnuloader ("qemu-lga"), the frankel_ufs kernel, the small root in RAM (ALONE=1 RC=rc.launchd), and a
# QEMU UFS device (read only, on the host side too) whose first logical unit holds a test GPT with the system image in `userdata`.
# The kernel's own driver takes the controller (ufs_native_soc=lga-qemu: the lga entry's quirks, read only), init mounts
# /dev/ufs_userdata read only and makes it the root (system_apfs=ufs_userdata), and Darwin goes on from there, CPU-rendered.
#
#   qemu/run-lga-ufs.sh [SECS] [boot-args...]
#
# env: KERNEL  a kernel built with `make frankel_ufs_defconfig` (default: hakios-kernel)
#      ROOT_IMG  userland/root.apfs from `ALONE=1 RC=rc.launchd REST=1 userland/build.sh`
#      SYSTEM    the system image for `userdata` (userland/system.apfs); never committed, never leaves the machine
#      OUT       where the disk, logs and the shim's files go (default: out-lga-ufs beside this script)
#      QEMU_CPU  max (PAC, BTI, MTE, SVE, SME as QEMU offers them; cortex-a710 is its nearest Armv9 core), QEMU_MEM (7G: XNU then gets the
#                4.2 GB block above the payload, as the phone's window is)
#      UFS_FW    with ufs_native_cold=1: goes into the payload as the M-PHY firmware (a test: it reads the file, QEMU's host has no PHY)
#      QEMU_MONITOR  a Unix socket for the monitor: qemu/fbshot.py takes a picture of the framebuffer (in RAM at FB_BASE) through it
# The frame kicker is on (lga_kick=30) against RAM standing where the display controller would be (FAKE_DPU, with the DC9x00's chip
# id and command mode in it): the log shows the writes, and a counter that never moves, as nothing counts frames there.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
OUT=${OUT:-$HERE/out-lga-ufs}
SECS=${1:-120}
[ $# -gt 0 ] && shift
SYSTEM=${SYSTEM:-$TOP/userland/system.apfs}
export KERNEL=${KERNEL:-$TOP/hakios-kernel} ROOT_IMG=${ROOT_IMG:-$TOP/userland/root.apfs}
FB_BASE=${FB_BASE:-0x80000000} FAKE_DPU=${FAKE_DPU:-0x90000000}      # (both below the block XNU is given)
[ -f "$SYSTEM" ] || { echo "run-lga-ufs.sh: $SYSTEM is missing (PREBUILT=1 REST=1 userland/build.sh)" >&2; exit 1; }
mkdir -p "$OUT"
DISK=$OUT/ufs-lun0.img
if [ ! -f "$DISK" ] || [ "$SYSTEM" -nt "$DISK" ]; then
	MIB=$(( ($(python3 -c 'import os, sys; print(os.path.getsize(sys.argv[1]))' "$SYSTEM") + (1 << 20) - 1) / (1 << 20) + 16 ))
	python3 "$TOP/tools/mkgpt.py" "$DISK" bootloader_a:4 bootloader_b:4 modem_a:2 modem_b:2 boot_a:4 boot_b:4 init_boot_a:2 \
		init_boot_b:2 vendor_boot_a:4 vendor_boot_b:4 dtbo_a:1 dtbo_b:1 vbmeta_a:1 vbmeta_b:1 super:16 metadata:4 \
		userdata:$MIB:"$SYSTEM"
fi
DPU() { printf -- "-device loader,addr=%#x,data=%#x,data-len=4 " $((FAKE_DPU + $1)) "$2"; }
export QEMU_EXTRA="-drive file=$DISK,if=none,id=lu0,format=raw,readonly=on -device ufs,id=ufs0 -device ufs-lu,drive=lu0,bus=ufs0,lun=0 \
$(DPU 0x20 0x9400) $(DPU 0xb6008 2)"
[ -z "${UFS_FW:-}" ] || export PAYLOAD_EXTRA="ufsphyfw=$UFS_FW"
export QEMU_CPU=${QEMU_CPU:-max} QEMU_MEM=${QEMU_MEM:-7G} OUT SHIM_ARGS="scion.fb=$FB_BASE,1080,2424,4320"
exec "$TOP/socs/lga/shim/qemu-test.sh" "$SECS" ufs_native=4 ufs_native_soc=lga-qemu ufs_native_ro=1 system_apfs=ufs_userdata \
	cpus=8 bootcpu=7 core0=2 small_cores_after=0 lga_kick=30 lga_dpu=$FAKE_DPU "$@"
