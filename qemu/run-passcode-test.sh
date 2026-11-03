#!/bin/bash
# The key store's passcode (xnu/files/iokit/Kernel/MiniAppleKeyStore.cpp) across restarts, in QEMU: userland/src/passcode_test.c
# as process 1 of a tiny APFS root, /private/var an HFS Plus volume on a writable virtio disk (virtio_blk=2), three boots on
# the same volume: 1 sets a passcode and tries the failed-attempt delays, 2 finds it after the restart, unlocks, removes it and
# sets another, 3 boots with passcode_reset=1 and finds none. Each boot ends with "passcode_test: PASS" or FAIL; the kernel's
# "MiniAppleKeyStore: selftest N:" lines say which check.
# usage: qemu/run-passcode-test.sh [HFS-IMAGE]  (default: qemu/passcode-test/hfs.img, made with mkfs.hfsplus, or $MKFS, if absent)
# needs: a kernel built for QEMU in hakios-kernel (or $KERNEL), xnu-clang (xnu-env.sh) or a prebuilt test in $TEST
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
W=qemu/passcode-test
mkdir -p $W/root/dev $W/root/private/var $W/root/sbin
IMG=${1:-$W/hfs.img}
if [ ! -f "$IMG" ]; then
	truncate -s 64M "$IMG"
	"${MKFS:-mkfs.hfsplus}" -s -J -U 0 -G 0 -M 755 -v Data "$IMG" || exit 1
fi
if [ -n "$TEST" ]; then
	install -m 0755 "$TEST" $W/root/sbin/launchd
else
	. ./xnu-env.sh
	xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
		-Wl,-e,__start -Wl,-pagezero_size,0x100000000 -o $W/root/sbin/launchd userland/src/passcode_test.c || exit 1
fi
python3 tools/apfs/mkapfs.py -o $W/root.apfs Root=$W/root >/dev/null || exit 1
for step in 1 2 3; do
	extra=""
	[ $step = 3 ] && extra=" passcode_reset=1"
	xnuloader/mkpayload.py -o qemu/payload.bin kernel=${KERNEL:-hakios-kernel} ramdisk=$W/root.apfs \
		--cmdline "cpus=1 debug=0x8 rd=md0 virtio_blk=2 passcode_selftest=$step$extra" >/dev/null || exit 1
	echo "=== boot $step"
	qemu/run.sh ${QEMU_SECONDS:-240} -global virtio-mmio.force-legacy=false \
		-drive file="$IMG",format=raw,if=none,id=hd -device virtio-blk-device,drive=hd |
		grep -a "passcode_test\|MiniAppleKeyStore\|panic(" | tr -d '\r'
done
