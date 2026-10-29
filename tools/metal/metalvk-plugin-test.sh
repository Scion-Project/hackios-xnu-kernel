#!/bin/bash
# The Metal-on-Vulkan plug-in (userland/src/metalvk/) on this Mac, through the Metal API, on a Vulkan driver of the Mac
# (tools/metal/metalvk_plugin_test.m says what that shows and what it does not). Needs the mac build of libmetal2vulkan.dylib and
# the test's metallib (tools/metal/mk-metal2vulkan.sh mac, which this runs when they are missing). macOS only.
# usage: tools/metal/metalvk-plugin-test.sh [ICD]        (default: lavapipe, /opt/homebrew/lib/libvulkan_lvp.dylib)
# The program is left beside the library, as out/mac/metal_test: gfxstream's Mac driver (tools/gfxstream/mk-vk.sh mac) runs the
# emulator's host in the process, which looks for MoltenVK in <program's directory>/lib64/vulkan (docs/metal-plan.md says how).
set -e -o pipefail
cd "$(dirname "$0")/../.."
[ "$(uname)" = Darwin ] || { echo "metalvk-plugin-test: needs macOS"; exit 0; }
M=xnu-deps/build/metal2vulkan/out/mac
[ -f $M/libmetal2vulkan.dylib ] && [ -f $M/m2v_test.metallib ] || tools/metal/mk-metal2vulkan.sh mac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# the same three sources as the bundle (userland/build.sh METAL=vk), and ARC, which the frontend is written for
W="-O1 -g -Wall -Wno-unused-parameter -Wno-deprecated-declarations"
clang $W -fobjc-arc -std=c++17 -Itools/metal -Iuserland/src/metalvk -c -o "$tmp/vk.o" userland/src/metalvk/HakiosMetalVK.mm
clang $W -fobjc-arc -std=c++17 -Itools/metal -Iuserland/src/metalvk -c -o "$tmp/frontend.o" userland/src/metalvk/NativeBridgeMetalFrontend.mm
clang $W -c -o "$tmp/shown.o" userland/src/hakios_metal_shown.c
clang $W -fobjc-arc -c -o "$tmp/test.o" tools/metal/metalvk_plugin_test.m
clang -o "$M/metal_test" "$tmp/test.o" "$tmp/vk.o" "$tmp/frontend.o" "$tmp/shown.o" -framework Foundation -framework Metal \
	-framework IOSurface -framework IOKit -framework CoreFoundation -lc++ -L$M -lmetal2vulkan -Wl,-rpath,"$PWD/$M"
# gfxstream's Mac driver: with GFXSTREAM_LIB (the emulator's lib64), its host's libraries and MoltenVK as tools/gfxstream/mk-vk.sh
# mac run sets them. Set here because macOS drops DYLD_* variables on the way into /bin/bash.
if [ -n "$GFXSTREAM_LIB" ]; then
	ln -sfn "$GFXSTREAM_LIB" $M/lib64
	export DYLD_LIBRARY_PATH=$GFXSTREAM_LIB:$GFXSTREAM_LIB/gles_angle
	export ANDROID_EMU_VK_ICD=${ANDROID_EMU_VK_ICD:-moltenvk}
	export ANDROID_EMU_VK_LOADER_PATH=${ANDROID_EMU_VK_LOADER_PATH:-$GFXSTREAM_LIB/vulkan/libvulkan.dylib}
	export VK_DRIVER_FILES=${VK_DRIVER_FILES:-$GFXSTREAM_LIB/vulkan/MoltenVK_icd.json}
	export VK_ICD_FILENAMES=$VK_DRIVER_FILES
fi
"$M/metal_test" "${1:-/opt/homebrew/lib/libvulkan_lvp.dylib}" $M/m2v_test.metallib 2>&1 | grep -v '^metal2vulkan: '
