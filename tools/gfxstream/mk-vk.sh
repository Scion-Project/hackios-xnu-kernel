#!/bin/bash
# Build Scion's Vulkan library (tools/gfxstream/VULKAN.md): gfxstream's own guest Vulkan driver --
# the encoder (vulkan_enc: VkEncoder, ResourceTracker) and the ICD -- which gfxstream moved into Mesa
# (src/gfxstream/guest), with a platform of ours under it (vk/scion): the Vulkan stream goes down a
# "pipe:opengles" render thread exactly as src/gfx's GL stream does.
#
#   tools/gfxstream/mk-vk.sh [mac|ios|all] [run [words]]
#
#   mac   $B/out-mac: libhakios.vk.dylib and vk_test for the Mac, the stream going to the Android
#         emulator's gfxstream (libgfxstream_backend.dylib, $GFXSTREAM_LIB) in the same process
#         through stream_renderer_* -- a real gfxstream host with MoltenVK under it. `mac run [words]`
#         runs vk_test (userland/src/vk_test.c) there.
#   ios   $B/out-ios: libhakios.vk.dylib (install name /redwood/lib/libhakios.vk.dylib) and vk_test
#         for arm64-apple-ios16, the stream over /dev/vgpu3d.
#
# $B is $VK_BUILD (default xnu-deps/build/gfxstream-vk). Mesa is the release tools/mk-mesa.sh pins,
# from $MESA_TARBALL, else xnu-deps/build/mesa, else archive.mesa3d.org (checked against its SHA-256);
# only the directories this build reads are unpacked (about 60 MB). It is patched with
# vk/mesa-gfxstream-scion.patch and vk/scion is copied in; nothing of Mesa is kept in this
# repository. When the patch changes, the tree is unpacked and patched anew.
# Needs meson, ninja, python3 (Mako goes into a private venv), Xcode's clang, and for ios the
# iPhoneOS 16.5 SDK ($SDK, else $THEOS's, else Xcode's).
set -e
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
B=${VK_BUILD:-$ROOT/xnu-deps/build/gfxstream-vk}
WHAT=${1:-all}
mkdir -p "$B"
cd "$B"

# ---- the source: Mesa's tree, the parts a Vulkan driver needs, patched
PATCHED=$(cat "$HERE/vk/mesa-gfxstream-scion.patch" | shasum -a 256 | cut -d' ' -f1)
M=$B/mesa-$VERSION
if [ ! -d "$M" ] || [ "$(cat "$M/.scion-patched" 2>/dev/null)" != "$PATCHED" ]; then
	T=${MESA_TARBALL:-$ROOT/xnu-deps/build/mesa/mesa-$VERSION.tar.xz}
	if [ ! -f "$T" ]; then
		T=$B/mesa-$VERSION.tar.xz
		[ -f "$T" ] || curl -sL --fail -o "$T" https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	fi
	echo "$SHA256  $T" | shasum -a 256 -c --quiet
	rm -rf "$M" build-mac build-ios
	# (the other drivers and the docs are left in the archive: meson never reads them for this)
	X=mesa-$VERSION/src
	tar xJf "$T" --exclude "mesa-$VERSION/docs" --exclude "$X/intel" --exclude "$X/freedreno" \
		--exclude "$X/nouveau" --exclude "$X/panfrost" --exclude "$X/imagination" --exclude "$X/asahi" \
		--exclude "$X/broadcom" --exclude "$X/etnaviv" --exclude "$X/microsoft" \
		--exclude "$X/gallium/drivers/[a-k]*" --exclude "$X/gallium/drivers/[m-z]*" \
		--exclude "$X/gallium/targets" --exclude "$X/gallium/winsys"
	patch -s --no-backup-if-mismatch -d "$M" -p1 < "$HERE/vk/mesa-gfxstream-scion.patch"
	echo "$PATCHED" > "$M/.scion-patched"
fi
# ours, always the current ones
rm -rf "$M/src/gfxstream/guest/platform/scion"
cp -r "$HERE/vk/scion" "$M/src/gfxstream/guest/platform/scion"
if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging
fi
export PATH=$B/venv/bin:$PATH

# Apple's ar: a GNU ar earlier in PATH writes archives ld64 skips members of
cat > native.ini <<INI
[binaries]
ar = '/usr/bin/ar'
ranlib = '/usr/bin/ranlib'
INI
OPTS="--wrap-mode=nofallback -Dplatforms= -Dgallium-drivers= -Dvulkan-drivers=gfxstream -Dvulkan-layers=
	-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
	-Dosmesa=false -Dllvm=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled
	-Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs=
	-Dtools= -Dbuild-tests=false -Dlmsensors=disabled"
LIBTARGET=src/gfxstream/guest/vulkan/libvulkan_gfxstream.dylib

# the library has nothing undefined but libSystem's and libc++'s, and the weak entry points
check_undefined() {
	local bad
	bad=$(/usr/bin/nm -m "$1" | grep '(undefined)' | grep 'dynamically looked up' | grep -v ' weak ' || true)
	if [ -n "$bad" ]; then
		echo "mk-vk.sh: $1 looks these up at load and would fail without them:" >&2
		echo "$bad" >&2
		exit 1
	fi
}

if [ "$WHAT" = mac ] || [ "$WHAT" = all ]; then
	[ -f build-mac/build.ninja ] || meson setup build-mac "$M" --native-file native.ini --buildtype=debugoptimized \
		$OPTS > meson-mac.log
	ninja -C build-mac $LIBTARGET > ninja-mac.log
	mkdir -p out-mac
	cp build-mac/$LIBTARGET out-mac/libhakios.vk.dylib
	install_name_tool -id @rpath/libhakios.vk.dylib out-mac/libhakios.vk.dylib 2>/dev/null
	codesign -f -s - out-mac/libhakios.vk.dylib 2>/dev/null || true
	check_undefined out-mac/libhakios.vk.dylib
	clang -O2 -g -Wall -I"$M/include" -o out-mac/vk_test "$ROOT/userland/src/vk_test.c" \
		-Lout-mac -lhakios.vk -Wl,-rpath,"$B/out-mac"
	echo "mk-vk.sh: $B/out-mac/libhakios.vk.dylib ($(wc -c < out-mac/libhakios.vk.dylib | tr -d " ") bytes," \
		"$(/usr/bin/nm -gU out-mac/libhakios.vk.dylib | grep -c ' _vk[A-Z]') vk* functions), $B/out-mac/vk_test"
fi

if [ "$WHAT" = ios ] || [ "$WHAT" = all ]; then
	if [ -z "$SDK" ]; then
		SDK=${THEOS:-$ROOT/xnu-deps/theos-mac}/sdks/iPhoneOS16.5.sdk
		[ -d "$SDK" ] || SDK=$(xcrun --sdk iphoneos --show-sdk-path)
	fi
	cat > cross-ios.ini <<INI
[binaries]
c = ['/usr/bin/clang', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
cpp = ['/usr/bin/clang++', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
objc = ['/usr/bin/clang', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
ar = '/usr/bin/ar'
ranlib = '/usr/bin/ranlib'
strip = '/usr/bin/strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
	[ -f build-ios/build.ninja ] || meson setup build-ios "$M" --cross-file cross-ios.ini --native-file native.ini \
		--buildtype=release -Db_ndebug=true $OPTS > meson-ios.log
	ninja -C build-ios $LIBTARGET > ninja-ios.log
	mkdir -p out-ios
	/usr/bin/strip -x -o out-ios/libhakios.vk.dylib build-ios/$LIBTARGET     # (Apple's: a GNU strip in PATH breaks Mach-O)
	install_name_tool -id /redwood/lib/libhakios.vk.dylib out-ios/libhakios.vk.dylib 2>/dev/null
	codesign -f -s - out-ios/libhakios.vk.dylib 2>/dev/null || true
	check_undefined out-ios/libhakios.vk.dylib
	/usr/bin/clang -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -Wall -I"$M/include" -o out-ios/vk_test \
		"$ROOT/userland/src/vk_test.c" out-ios/libhakios.vk.dylib
	echo "mk-vk.sh: $B/out-ios/libhakios.vk.dylib ($(wc -c < out-ios/libhakios.vk.dylib | tr -d " ") bytes," \
		"$(/usr/bin/nm -gU out-ios/libhakios.vk.dylib | grep -c ' _vk[A-Z]') vk* functions), $B/out-ios/vk_test"
fi

if [ "$2" = run ] && [ "$WHAT" = mac ]; then
	# The emulator's gfxstream finds its Vulkan loader and MoltenVK beside the program
	# (<program>/lib64/vulkan) or where these say; vk_test is not in the emulator's directory.
	LIB=${GFXSTREAM_LIB:-$HOME/Library/Android/sdk/emulator/lib64}
	ln -sfn "$LIB" out-mac/lib64
	export GFXSTREAM_LIB=$LIB
	export DYLD_LIBRARY_PATH=$LIB:$LIB/gles_angle
	export ANDROID_EMU_VK_ICD=${ANDROID_EMU_VK_ICD:-moltenvk}
	export ANDROID_EMU_VK_LOADER_PATH=${ANDROID_EMU_VK_LOADER_PATH:-$LIB/vulkan/libvulkan.dylib}
	export VK_DRIVER_FILES=${VK_DRIVER_FILES:-$LIB/vulkan/MoltenVK_icd.json}
	export VK_ICD_FILENAMES=$VK_DRIVER_FILES
	shift 2
	exec out-mac/vk_test "$@"
fi
