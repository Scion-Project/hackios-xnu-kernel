#!/bin/bash
# Fetch gfxstream's codegen descriptions (Apache-2.0) at a pinned commit for mkgfxenc.py, into
# xnu-deps/build/gfxstream (nothing of gfxstream is kept in this repository).
set -e
COMMIT=${GFXSTREAM_COMMIT:-0884d2d224bfe2e46cbb13b71d68d7c66e2f7dc9}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=$ROOT/xnu-deps/build/gfxstream/$COMMIT
mkdir -p "$D/codegen/gles2" "$D/codegen/renderControl" "$D/opcodes"
URL=https://raw.githubusercontent.com/google/gfxstream/$COMMIT
for f in gles2/gles2.in gles2/gles2.attrib gles2/gles2.types \
	renderControl/renderControl.in renderControl/renderControl.attrib renderControl/renderControl.types; do
	[ -f "$D/codegen/$f" ] || curl -sfL -o "$D/codegen/$f" "$URL/codegen/$f"
done
for f in guest/GLESv2_enc/gl2_opcodes.h guest/renderControl_enc/renderControl_opcodes.h; do
	[ -f "$D/opcodes/$(basename $f)" ] || curl -sfL -o "$D/opcodes/$(basename $f)" "$URL/$f"
done
echo "$D"
