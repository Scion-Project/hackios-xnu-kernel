#!/bin/bash
# Build gfxstream's HOST renderer (libgfxstream_backend.so, the one crosvm links) for Android arm64
# with the NDK, for Scion to run in its own process: the app decodes the guest's GL stream itself
# and presents colour buffers straight into its window, instead of crosvm's virtio-gpu, which
# can't show anything to a sideloaded app (avf/DISPLAY.md), and instead of copying every frame
# back into the guest.
#
#   tools/gfxstream/mk-host-android.sh      -> xnu-deps/build/gfxstream-host-android/libgfxstream_backend.so
#
# The source is gfxstream at the commit our encoders are generated from (fetch.sh), cloned into
# xnu-deps/build (Apache-2.0; nothing of it is kept in this repository). android-host.patch
# teaches its CMake about Android, which Soong builds it for but CMake never was: EGL on
# ANativeWindow instead of X11/GLX, VK_USE_PLATFORM_ANDROID_KHR instead of XCB, the AOSP headers
# of third_party/android after the NDK's, no librt/libpthread (bionic's libc has them).
# The result needs nothing but libc, libm, libdl, liblog and libnativewindow: an app can load it.
set -e
COMMIT=${GFXSTREAM_COMMIT:-0884d2d224bfe2e46cbb13b71d68d7c66e2f7dc9}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC=$ROOT/xnu-deps/build/gfxstream-src
OUT=$ROOT/xnu-deps/build/gfxstream-host-android
NDK=${ANDROID_NDK:-$(ls -d "$HOME"/Library/Android/sdk/ndk/* | sort -V | tail -1)}
if [ ! -d "$SRC/.git" ]; then
	git init -q "$SRC"
	git -C "$SRC" remote add origin https://github.com/google/gfxstream.git
fi
# (again whenever android-host.patch changes: the tree is put back to the commit and patched anew)
PATCHED=$(git hash-object "$HERE/android-host.patch")
if [ "$(git -C "$SRC" rev-parse -q --verify HEAD 2>/dev/null)" != "$COMMIT" ] ||
   [ "$(cat "$SRC/.git/hakios-patched" 2>/dev/null)" != "$PATCHED" ]; then
	git -C "$SRC" cat-file -e "$COMMIT^{commit}" 2>/dev/null || git -C "$SRC" fetch -q --depth 1 origin "$COMMIT"
	git -C "$SRC" checkout -q -f "$COMMIT"
	git -C "$SRC" apply "$HERE/android-host.patch"
	echo "$PATCHED" > "$SRC/.git/hakios-patched"
fi
cmake -S "$SRC" -B "$OUT/build" -G Ninja -Wno-dev \
	-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
	-DANDROID_PLATFORM=android-34 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release \
	-DBUILD_STANDALONE=ON -DDEPENDENCY_RESOLUTION=SYSTEM -DENABLE_VKCEREAL_TESTS=OFF > /dev/null
ninja -C "$OUT/build" gfxstream_backend
"$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip | head -1)" -o "$OUT/libgfxstream_backend.so" \
	"$OUT/build/libgfxstream_backend.so"
ls -l "$OUT/libgfxstream_backend.so"
