#!/bin/bash
# Build the driver of the Galaxy S9's Wi-Fi radio (the BCM4361 on PCIe) for our kernel: brcmfmac,
# Broadcom's FullMAC driver as it is in Linux, on the same layer as the POCO's ath11k
# (tools/mk-ath11k.sh says how that layer is made; this is its twin). Three things meet here:
#
#   Linux's brcmfmac      drivers/net/wireless/broadcom/brcm80211/{brcmfmac,brcmutil,include} of the
#                         Linux release mk-ath11k.sh pins (ISC, every file checked below), with
#                         wifi/lx/brcmfmac/patches/*.patch on top: the BCM4361 in its tables;
#   FreeBSD's LinuxKPI    its headers, as mk-ath11k.sh lays them out and patches them;
#   wifi/lx               ours: src/lx_base.c (locks, sleeping, work, memory, firmware) and
#                         brcmfmac/*.c, what this driver needs besides: a struct pci_dev on the
#                         kernel's PCIe root complex (lx_os_pci_*() of sys/hakios.lx.h), the bit of
#                         cfg80211 the files built so far ask for, and lx_start(); CONFIG_DMI makes
#                         brcmf_dmi_probe() lx_board.c's (the phone's settings).
#
# Not all of the driver is built yet: DRIVER below lists the files that are. What they call in
# the files that are not becomes a function that says its name in the log and answers 0
# (out/stubs.txt lists them), unless wifi/lx/brcmfmac has one that does more. No SDIO, no USB,
# no BCDC.
#
# What comes out, in xnu-deps/build/brcmfmac:
#   src/brcm80211/     the driver's files, patched          out/brcmfmac.o  the object for the kernel
#   out/stubs.txt      what is not there yet
# The two archives are mk-ath11k.sh's, taken from xnu-deps/build/ath11k when they are there.
#
# usage: tools/mk-brcmfmac.sh [fetch]      "fetch" stops when the driver's sources are there
set -e
VERSION=7.0.14
SHA256=de9999b784d2293f00d39c62d8f92a08ab8a54bc4e80ffd250a0c09cb07a0f98
FREEBSD=15.1
FREEBSD_SHA256=cf5762da53fd52e1eaf0f9ceee9bf58cbe314c821031d0d9ffa76823185a89e1
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
B=$ROOT/xnu-deps/build/brcmfmac
A=$ROOT/xnu-deps/build/ath11k
BRCM=drivers/net/wireless/broadcom/brcm80211
W=$ROOT/wifi/lx
mkdir -p "$B"
cd "$B"
archive() {		# archive FILE URL SHA256: here, from mk-ath11k.sh's directory or from the network
	[ -f "$1" ] || { [ -f "$A/$1" ] && ln "$A/$1" "$1" 2>/dev/null || cp "$A/$1" "$1"; } 2>/dev/null ||
		curl -sL --fail -o "$1" "$2"
	echo "$3  $1" | sha256sum -c --quiet
}

BPATCHES=$(cat "$W"/brcmfmac/patches/*.patch 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ "$(cat src/.release 2>/dev/null)" != "$VERSION $SHA256 $BPATCHES" ]; then
	archive linux-$VERSION.tar.xz https://cdn.kernel.org/pub/linux/kernel/v${VERSION%%.*}.x/linux-$VERSION.tar.xz $SHA256
	rm -rf src unpacked
	mkdir unpacked
	tar xJf linux-$VERSION.tar.xz -C unpacked --strip-components=1 linux-$VERSION/$BRCM/brcmfmac \
		linux-$VERSION/$BRCM/brcmutil linux-$VERSION/$BRCM/include
	# every file must say itself that it may be used so: the licence is checked, not assumed
	for f in $(find unpacked/$BRCM -name '*.[ch]'); do
		case "$(head -3 "$f")" in
		*"SPDX-License-Identifier: ISC"*) ;;
		*) echo "mk-brcmfmac.sh: $f does not carry the licence expected" >&2; exit 1 ;;
		esac
	done
	mkdir src
	mv unpacked/$BRCM src/brcm80211
	rm -rf unpacked
	for p in "$W"/brcmfmac/patches/*.patch; do
		[ ! -f "$p" ] || patch -s --no-backup-if-mismatch -d src/brcm80211 -p1 < "$p"
	done
	echo "$VERSION $SHA256 $BPATCHES" > src/.release
fi
[ "$1" = fetch ] && { echo "mk-brcmfmac.sh: brcmfmac of Linux $VERSION: $B/src/brcm80211"; exit 0; }

# --- FreeBSD's LinuxKPI, laid out and patched as mk-ath11k.sh does it (and netlink/, which cfg80211.c's vendor
# commands reach through LinuxKPI's net/netlink.h)
FSYS="compat/linuxkpi sys arm64/include net net80211 netinet netinet6 netlink vm kern tools libkern ddb
	security fs/pseudofs crypto opencrypto geom bsm isa cam rpc xdr x86/include ofed/include
	dev/pci dev/ofw dev/fdt dev/acpica dev/usb dev/iicbus dev/backlight
	contrib/ck/include contrib/dev/acpica/include contrib/libnv"
PATCHES=$( (echo $FSYS; cat "$W"/patches/*.patch 2>/dev/null) | sha256sum | cut -d' ' -f1)
if [ "$(cat freebsd/.release 2>/dev/null)" != "$FREEBSD $FREEBSD_SHA256 $PATCHES" ]; then
	archive freebsd-$FREEBSD-src.txz https://download.freebsd.org/releases/amd64/$FREEBSD-RELEASE/src.txz $FREEBSD_SHA256
	rm -rf freebsd gen obj
	mkdir freebsd
	tar xJf freebsd-$FREEBSD-src.txz -C freebsd --strip-components=2 usr/src/COPYRIGHT \
		$(for d in $FSYS; do echo usr/src/sys/$d; done)
	for p in "$W"/patches/*.patch; do
		[ -f "$p" ] && patch -s --no-backup-if-mismatch -d freebsd -p1 < "$p"
	done
	echo "$FREEBSD $FREEBSD_SHA256 $PATCHES" > freebsd/.release
fi
S=$B/freebsd/sys
if [ ! -d gen ]; then
	mkdir gen
	(
		cd gen
		ln -s $S/arm64/include machine
		for m in kern/device_if.m kern/bus_if.m dev/pci/pci_if.m dev/pci/pci_iov_if.m dev/pci/pcib_if.m \
			dev/iicbus/iicbus_if.m dev/iicbus/iicbb_if.m dev/backlight/backlight_if.m dev/usb/usb_if.m \
			dev/ofw/ofw_bus_if.m; do
			awk -f $S/tools/makeobjops.awk $S/$m -h
		done
		for o in h p q; do awk -f $S/tools/vnode_if.awk $S/kern/vnode_if.src -$o; done
		for o in $(grep -rhoE "opt_[a-z0-9_]+\.h" $S | sort -u) lkpi_iic_if.h; do : > $o; done
		printf '#define INET 1\n#define INET6 1\n' > opt_inet.h
		printf '#define INET6 1\n' > opt_inet6.h
		printf '#define SMP 1\n' > opt_global.h
	)
fi

# --- compile: the driver's files that are built so far, and ours
DRIVER="brcmfmac/pcie brcmfmac/chip brcmfmac/firmware brcmfmac/common brcmfmac/core brcmfmac/proto
	brcmfmac/commonring brcmfmac/flowring brcmfmac/msgbuf brcmfmac/fwil brcmfmac/fweh brcmfmac/feature
	brcmfmac/fwvid brcmfmac/xtlv brcmfmac/wcc/core brcmfmac/cyw/core brcmfmac/bca/core brcmfmac/cfg80211
	brcmfmac/p2p brcmfmac/btcoex brcmfmac/vendor brcmfmac/pno brcmutil/utils brcmutil/d11"
OURS=$W/src/lx_base.c
for f in "$W"/brcmfmac/*.c; do [ ! -f "$f" ] || OURS="$OURS $f"; done
CC="clang -target arm64-apple-ios16.5"
LD=$ROOT/xnu-deps/host/bin/ld64
CFLAGS="-mkernel -mgeneral-regs-only -fno-builtin -fno-common -ffreestanding -fno-strict-aliasing
	-funsigned-char -nostdinc -std=gnu17 -O2 -g -w
	-D__FreeBSD__=15 -D_KERNEL -DKLD_MODULE -DHAVE_KERNEL_OPTION_HEADERS -include opt_global.h
	-I$W/include -Igen -I$S -I$S/contrib/ck/include
	-I$S/compat/linuxkpi/common/include -I$S/compat/linuxkpi/dummy/include
	-include $S/compat/linuxkpi/common/include/linux/kconfig.h -DLINUXKPI_VERSION=70000"
D=src/brcm80211
DFLAGS="-I$D/brcmfmac -I$D/include -include $W/brcmfmac/lx_prelude.h -I$W/brcmfmac
	-DCONFIG_BRCMFMAC_PCIE -DCONFIG_BRCMFMAC_PROTO_MSGBUF -DCONFIG_BRCMDBG -DDEBUG -DCONFIG_DMI -DKBUILD_MODNAME=\"brcmfmac\""
source "$ROOT/xnu-env.sh"
[ -f "$XNU_SRC/bsd/sys/hakios.lx.h" ] || { echo "mk-brcmfmac.sh: the kernel tree has no bsd/sys/hakios.lx.h" >&2; exit 1; }
DFLAGS="$DFLAGS -DLX_HAKIOS_LX_H=\"$XNU_SRC/bsd/sys/hakios.lx.h\""
mkdir -p obj out
rm -f obj/*.err
compile() {		# compile SOURCE OBJECT [FLAGS]: only when the object is older than anything it was made of
	local src=$1 obj=$2
	shift 2
	if [ -f "$obj" ] && [ -f "$obj.d" ] && [ -z "$(find $(sed -e 's/^[^:]*://' -e 's/\\$//' "$obj.d") "$0" -newer "$obj" 2>/dev/null | head -1)" ]; then
		return
	fi
	$CC $CFLAGS "$@" -MMD -MF "$obj.d" -c "$src" -o "$obj" 2> "$obj.err" || rm -f "$obj"
}
name() { echo "$1" | tr / _; }
for f in $DRIVER; do compile $D/$f.c obj/$(name $f).o $DFLAGS & done
for f in $OURS; do compile "$f" obj/lx_$(basename "$f" .c).o $DFLAGS -I$W/src & done
wait
bad=
for f in obj/*.err; do
	[ -s "$f" ] && [ ! -f "${f%.err}" ] && { echo "--- $(basename "$f" .o.err)"; grep -m6 -A3 "error:" "$f"; bad=1; }
done
[ -z "$bad" ] || exit 1
OBJECTS=$(for f in $DRIVER; do echo obj/$(name $f).o; done; for f in $OURS; do echo obj/lx_$(basename "$f" .c).o; done)

# --- one object. What it still wants is a call of the kernel's (hakios.lx.h; the C library
# functions XNU has under the same names), a function of a part of the driver not built yet (a
# stub that says its name and answers 0), or a mistake, which ends the build.
KERNEL="memcmp memcpy memmove memset strcmp strncmp strlen strchr strrchr strlcpy strlcat bzero
	snprintf vsnprintf __stack_chk_fail __stack_chk_guard"
$LD -r -arch arm64 $OBJECTS -o out/all.o 2> out/ld.err || { cat out/ld.err; exit 1; }
NM=$(command -v llvm-nm || xcrun -f llvm-nm)
$NM -u --format=just-symbols out/all.o | sed 's/^_//' | sort > out/undefined.txt
: > out/stubs.txt
: > out/stubs.s
missing=
for s in $(cat out/undefined.txt); do
	case " $(echo $KERNEL) " in *" $s "*) continue ;; esac
	case $s in
	lx_os_*|section\$*) continue ;;
	brcmf_*|brcmu_*) ;;
	*) echo "mk-brcmfmac.sh: nothing defines $s" >&2; missing=1; continue ;;
	esac
	echo $s >> out/stubs.txt
	printf '\t.globl _%s\n_%s:\n\tadrp x0, 1f@PAGE\n\tadd x0, x0, 1f@PAGEOFF\n\tb _lx_stub\n\t.cstring\n1:\t.asciz "%s"\n\t.text\n\t.p2align 2\n' \
		$s $s $s >> out/stubs.s
done
[ -z "$missing" ] || exit 1
$CC -c out/stubs.s -o out/stubs.o
printf '_lx_prepare\n_lx_start\n_lx_stop\n_lx_transmit\n' > out/exported.txt
$LD -r -arch arm64 -exported_symbols_list out/exported.txt out/all.o out/stubs.o -o out/brcmfmac.o 2> out/ld.err ||
	{ cat out/ld.err; exit 1; }
rm -f out/all.o
SIZE=$(command -v llvm-size || xcrun -f llvm-size)
echo "mk-brcmfmac.sh: out/brcmfmac.o: $($SIZE out/brcmfmac.o | awk 'NR == 2 { print $1 + $2 }') bytes of code and data from" \
	"$(echo $DRIVER | wc -w) of the driver's files; $(wc -l < out/stubs.txt) functions of the others are stubs (out/stubs.txt);" \
	"asks the kernel for $($NM -u --format=just-symbols out/brcmfmac.o | grep -vc 'section\$') names"
