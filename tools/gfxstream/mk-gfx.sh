#!/bin/bash
# Build the gfx OpenGL ES library (src/gfx: gfxstream over /dev/vgpu3d) for iOS, the way
# tools/mk-mesa.sh builds Mesa for the same place, behind src/eagl.c:
#   obj/libredwood-gl.dylib   eagl.c + gfx (install name /redwood/lib/libredwood-gl.dylib)
#   obj/gfx_test              the library end to end (src/gfx_test.c), on /dev/vgpu3d
# Needs an iOS SDK: $SDK, else the theos one userland/build.sh uses, else Xcode's.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
U=$ROOT/userland
OUT=${OUT:-$U/obj}
mkdir -p "$OUT/gfx"
if [ -z "$SDK" ]; then
	SDK=${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk
	[ -d "$SDK" ] || SDK=$(xcrun --sdk iphoneos --show-sdk-path)
fi
CC=${CC:-clang}
CFLAGS="-target arm64-apple-ios16.0 -isysroot $SDK -O2 -Wall -Wno-unused-function"
D=$("$ROOT/tools/gfxstream/fetch.sh")
python3 "$ROOT/tools/gfxstream/mkgfxenc.py" "$D/codegen" "$OUT/gfx/gfx_enc.h" "$OUT/gfx/gfx_enc.c" "$OUT/gfx/gfx_api.c" \
	"$D"/opcodes/*.h
G=$U/src/gfx
GFX="$G/gfx_core.c $G/gfx_gl.c $G/gfx_shared.c $G/gfx_dev.c $OUT/gfx/gfx_enc.c $OUT/gfx/gfx_api.c"
# the gl* functions the library exports, and their prototypes (the Khronos registry), for mkeagl.py
[ -f "$D/gl.xml" ] || curl -sfL -o "$D/gl.xml" \
	https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/${GL_XML_COMMIT:-main}/xml/gl.xml
$CC $CFLAGS -fvisibility=hidden -dynamiclib -I"$G" -I"$OUT/gfx" -o "$OUT/gfx/libgfx-probe.dylib" $GFX \
	-framework CoreFoundation -framework IOSurface -install_name /redwood/lib/libgfx-probe.dylib
nm -gU "$OUT/gfx/libgfx-probe.dylib" | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > "$OUT/gfx/exported-gl.txt"
"$U/mkeagl.py" "$U/src/eagl_slots.txt" "$D/gl.xml" "$OUT/gfx/exported-gl.txt" "$U/src/eagl.c" > "$OUT/eagl_gen.h"
$CC $CFLAGS -fvisibility=hidden -dynamiclib -install_name /redwood/lib/libredwood-gl.dylib -I"$OUT" -I"$G" -I"$OUT/gfx" \
	-framework CoreFoundation -framework IOKit -framework IOSurface -lobjc -o "$OUT/libredwood-gl.dylib" \
	"$U/src/eagl.c" "$G/gfx_redwood.c" $GFX
$CC $CFLAGS -I"$U/src" -I"$G" -I"$OUT/gfx" -o "$OUT/gfx_test" "$U/src/gfx_test.c" "$U/src/gfx_test_dev.c" $GFX \
	-framework CoreFoundation -framework IOSurface
rm -f "$OUT/gfx/libgfx-probe.dylib"
echo "mk-gfx.sh: $OUT/libredwood-gl.dylib ($(wc -c < "$OUT/libredwood-gl.dylib") bytes, $(wc -l < "$OUT/gfx/exported-gl.txt") gl* functions), $OUT/gfx_test"
