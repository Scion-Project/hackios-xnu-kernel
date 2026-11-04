#!/bin/bash
# The key store's passcode (xnu/files/iokit/Kernel/MiniAppleKeyStore.cpp) across restarts, in QEMU: userland/src/passcode_test.c
# as process 1 of a tiny APFS root, /private/var an HFS Plus volume on a writable virtio disk (virtio_blk=2), one boot after another
# on the same volume. Each boot ends with "passcode_test: PASS" or FAIL; the kernel's "MiniAppleKeyStore: selftest N:" lines say which check.
#
#   old (default)  without keystore_real: 1 sets a passcode and tries the failed-attempt delays, 2 finds it after the restart, unlocks,
#                  removes it and sets another, 3 boots with passcode_reset=1 and finds none.
#   real           keystore_real=1 on a new volume: 10 a new key bag, real wrap and unwrap, a locked keystore cannot open class A, a wrong
#                  code gets no key; 11 the restart, after which tools/keybag-offline.py reads the volume as a thief would; 12 damage
#                  fails closed; 13 and 14 passcode_reset=1 destroys the keys.
#   migrate        the old kind of file first (1), then keystore_real=1: 15 and 16.
#
# usage: qemu/run-passcode-test.sh [old|real|migrate] [HFS-IMAGE]
#   default image: qemu/passcode-test/hfs-MODE.img, made with mkfs.hfsplus, or $MKFS, if absent. Use a new one for each run of real and migrate.
# needs: a kernel built for QEMU in hakios-kernel (or $KERNEL), xnu-clang (xnu-env.sh) or a prebuilt test in $TEST
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
W=qemu/passcode-test
MODE=${1:-old}
case $MODE in
old) STEPS="1 2 3" ;;
real) STEPS="10 11 12 13 14" ;;
migrate) STEPS="1 15 16" ;;
*) echo "usage: qemu/run-passcode-test.sh [old|real|migrate] [HFS-IMAGE]" >&2; exit 1 ;;
esac
mkdir -p $W/root/dev $W/root/private/var $W/root/sbin
IMG=${2:-$W/hfs-$MODE.img}
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
LOG=$(mktemp)
for step in $STEPS; do
	extra=""
	[ "$step" = 3 ] && extra=" passcode_reset=1"
	# QEMU's translated code is slow, so the key derivation is made cheap here: keystore_kdf_kib only counts with passcode_selftest
	[ "$step" -ge 10 ] && extra="$extra keystore_real=1 keystore_kdf_kib=${KDF_KIB:-1024}"
	{ [ "$step" = 13 ] || [ "$step" = 14 ]; } && extra="$extra passcode_reset=1"
	xnuloader/mkpayload.py -o qemu/payload.bin kernel=${KERNEL:-hakios-kernel} ramdisk=$W/root.apfs \
		--cmdline "cpus=1 debug=0x8 rd=md0 virtio_blk=2 passcode_selftest=$step$extra" >/dev/null || exit 1
	echo "=== boot $step"
	qemu/run.sh ${QEMU_SECONDS:-240} -global virtio-mmio.force-legacy=false \
		-drive file="$IMG",format=raw,if=none,id=hd -device virtio-blk-device,drive=hd |
		grep -a "passcode_test\|MiniAppleKeyStore\|panic(" | tr -d '\r' | grep -av "initproc failed to start\|--- panic() called" | tee -a "$LOG"
	# process 1 exits when it is done, and the kernel panics about that: not a failure. After boot 11 the passcode is 999999
	if [ "$step" = 11 ] && [ -z "$NO_OFFLINE" ]; then
		python3 tools/keybag-offline.py "$IMG" || OFFLINE_FAILED=1
	fi
done
if [ -n "$OFFLINE_FAILED" ] || grep -q "selftest [0-9]*: FAIL\|passcode_test: FAIL\|panic(" "$LOG" ||
	[ "$(grep -c '^passcode_test: PASS' "$LOG")" -lt "$(echo $STEPS | wc -w)" ]; then
	echo "passcode test ($MODE): FAILED"; rm -f "$LOG"; exit 1
fi
rm -f "$LOG"
echo "passcode test ($MODE): all boots passed"
exit 0
