#!/bin/bash
# Build gfx_test for the Mac against the Android emulator's gfxstream and run it:
#   tools/gfxstream/host_test/build.sh [frames]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/../../..
OUT=$HERE/out
mkdir -p "$OUT"
D=$("$ROOT/tools/gfxstream/fetch.sh")
python3 "$ROOT/tools/gfxstream/mkgfxenc.py" "$D/codegen" "$OUT/gfx_enc.h" "$OUT/gfx_enc.c" "$OUT/gfx_api.c" "$D"/opcodes/*.h
G=$ROOT/userland/src/gfx
clang -O2 -g -Wall -Wno-unused-function -fvisibility=hidden -I"$G" -I"$OUT" -o "$OUT/gfx_test" \
	"$ROOT/userland/src/gfx_test.c" "$G/gfx_core.c" "$G/gfx_gl.c" "$OUT/gfx_enc.c" "$OUT/gfx_api.c" \
	"$HERE/host.c" "$HERE/main_host.c"
LIB=${GFXSTREAM_LIB:-$HOME/Library/Android/sdk/emulator/lib64}
DYLD_LIBRARY_PATH=$LIB:$LIB/gles_angle "$OUT/gfx_test" "$@"
