#!/bin/bash
# Build gfx_test for the phone against gfxstream's host renderer built for Android
# (tools/gfxstream/mk-host-android.sh), and run it there from adb shell:
#   tools/gfxstream/host_test/build-android.sh [frames]
# It needs no VM: the guest's GL library (src/gfx) talks to libgfxstream_backend.so in the same
# process, the way Scion's host renderer will, on the phone's own GPU.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/../../..
OUT=$HERE/out-android
mkdir -p "$OUT"
NDK=${ANDROID_NDK:-$(ls -d "$HOME"/Library/Android/sdk/ndk/* | sort -V | tail -1)}
CC=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android34-clang | head -1)
D=$("$ROOT/tools/gfxstream/fetch.sh")
python3 "$ROOT/tools/gfxstream/mkgfxenc.py" "$D/codegen" "$OUT/gfx_enc.h" "$OUT/gfx_enc.c" "$OUT/gfx_api.c" "$D"/opcodes/*.h
G=$ROOT/userland/src/gfx
"$CC" -O2 -g -Wall -Wno-unused-function -fvisibility=hidden -I"$G" -I"$OUT" -o "$OUT/gfx_test" \
	"$ROOT/userland/src/gfx_test.c" "$G/gfx_core.c" "$G/gfx_gl.c" "$G/gfx_shared.c" "$OUT/gfx_enc.c" "$OUT/gfx_api.c" \
	"$HERE/host.c" "$HERE/main_host.c" -ldl
LIB=${GFXSTREAM_HOST_LIB:-$ROOT/xnu-deps/build/gfxstream-host-android/libgfxstream_backend.so}
DEV=/data/local/tmp/gfxstream-test
adb shell mkdir -p $DEV
adb push -q "$OUT/gfx_test" $DEV/
adb push -q "$LIB" $DEV/libgfxstream_backend.so
adb shell "cd $DEV && ./gfx_test $*"
