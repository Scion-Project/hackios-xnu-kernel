#!/bin/bash
# The Galaxy S9's GPU path, its user-space half, checked on a Linux machine without the phone.
#
# Mesa 25.0.7 is built for this machine as tools/mk-mesa.sh builds it for the phone -- the same
# release, every patch of tools/mesa applied, the AIR frontend and libhakiosair in it -- with
# panfrost, softpipe and Mesa's drm-shim added. Then panfrost is run under drm-shim with
# tools/mali-drmshim/hakios_mali_shim.c in place of a GPU: a shim that answers as HakiosMali does
# (drivers/gpu/hakios.mali.c, hakios.mali.cpp): a Mali-G72 (GPU_ID 0x6221) MP18, interface 1.2,
# and GPU addresses from the kernel's own allocator, drivers/gpu/hakios.mali_va.h. Nothing
# executes the jobs; what is checked is everything up to the GPU:
#
#   1. OpenGL ES: userland/src/gl_test.c's triangle (tools/mali-drmshim/gl_drmshim.c) through
#      OSMesa, which opens panfrost the way the phone's libOSMesa does (osmesa_create_gpu_screen of
#      tools/mesa/panfrost-darwin.patch), plus gl_test fill's ten kinds of frame and one blend that
#      needs a blend shader. panfrost compiles the shaders for Bifrost v6 and builds the job chains;
#      PAN_MESA_DEBUG=trace has its own decoder (pandecode) write every chain out, and
#      tools/mali-drmshim/pandecode-check.py judges what it wrote.
#   2. Metal: the tree's own AIR modules (tools/air/*.ll, written by hand, none of them Apple's)
#      through libhakiosair (tools/mesa/air/air_metal.c) onto panfrost: tools/mesa/air's
#      air_metal_test drives them with RWA_SOFTWARE=0, as userland/rc.jobs.malimetal does on the
#      phone, and the chains are judged the same way.
#   3. The reference: the same triangle on softpipe, the phone's own fallback renderer (this build,
#      like the phone's, has no LLVM, so softpipe and not llvmpipe), compared with the picture kept
#      in tools/mali-drmshim/gl_test-1080x2400-softpipe.{png,sha256}.
#   4. The contract of HakiosMali's allocator, from both sides: every buffer the shim gave inside
#      32 MB .. 4 GB and code never across a 16 MB line (the shim's own line at exit), and every
#      pointer in every decoded descriptor inside a buffer it gave, code in an executable one.
#
# What this does NOT prove: that the G72 runs these chains (no GPU here; the pixels panfrost
# "reads back" are not results, which is why only softpipe's picture is kept), that the feature
# registers the shim reports are this phone's (the phone has never been read: the values and why
# each was chosen are in hakios_mali_shim.c, and HAKIOS_MALI_REGS takes the ones step 4 of mali=N
# logs), or anything of the kernel's half (drivers/gpu/hakios.mali.c, the IOKit service, the MMU).
#
#   tools/mali-drmshim-check.sh                  build what is missing, run everything
#   COMPARE=phone.rgba tools/mali-drmshim-check.sh
#                                                also compare a 1080 x 2400 RGBA readback (gl_test's
#                                                drawn[] buffer) with softpipe's picture
#
# Needs: curl, xz, a C compiler, python3 with venv, ninja, bison, flex, libdrm's headers
# (libdrm-dev), llvm-as (for tools/air/*.ll). About 1 GB in build/mali-drmshim. Exit 0: all passed.
set -e
SELF=$(readlink -f "$0")
ROOT=$(cd "$(dirname "$SELF")/.." && pwd)
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
B=${B:-$ROOT/build/mali-drmshim}
SRC=$B/mesa-$VERSION
OUT=$B/out
LLVM_AS=${LLVM_AS:-$(command -v llvm-as || ls /usr/lib/llvm-*/bin/llvm-as 2>/dev/null | tail -1)}
mkdir -p "$B"
cd "$B"

# The sources: the release with EVERY patch of tools/mesa, in the order tools/mk-mesa.sh applies
# them. The two Darwin ones (freedreno-darwin, panfrost-darwin) are applied too: what they change
# outside #ifdef __APPLE__ and host_machine.system() == 'darwin' -- OSMesa's choice of GPU -- is
# what the GL test goes through. Then three changes for this machine only, none of them in a file
# the phone builds differently:
#   - osmesa_surface_resource() calls hakios_drm_surface(), which exists only in tools/mesa/hakios-drm
#     (Darwin); here it is fenced off with __APPLE__ (no IOSurfaces on Linux)
#   - glibc 2.43's call_once clash with src/c11/threads.h, as tools/air/mk-host.sh fixes it
#   - the shim, as one more library beside Mesa's own panfrost_noop_drm_shim
PATCHES=$(cat "$ROOT"/tools/mesa/*.patch "$SELF" | sha256sum | cut -d' ' -f1)
if [ ! -d "$SRC" ] || [ "$(cat "$SRC/.drmshim-patches" 2>/dev/null)" != "$PATCHES" ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf "$SRC" build
	tar xJf mesa-$VERSION.tar.xz
	for p in "$ROOT"/tools/mesa/*.patch; do patch -s --no-backup-if-mismatch -d "$SRC" -p1 < "$p"; done
	cp -r "$ROOT/tools/mesa/hakios-drm" "$SRC/src/hakios-drm"
	python3 - "$SRC" <<-'PY'
		import sys
		root = sys.argv[1]
		p = root + '/src/gallium/targets/osmesa/osmesa_target.c'
		s = open(p).read()
		old = "{\n#ifdef OSMESA_GPU\n   struct hakios_drm_surface"
		assert old in s, 'osmesa_target.c is not as panfrost-darwin.patch leaves it'
		open(p, 'w').write(s.replace(old, "{\n#if defined(OSMESA_GPU) && defined(__APPLE__)\n   struct hakios_drm_surface"))
		p = root + '/src/c11/threads.h'
		s = open(p).read()
		old = "typedef pthread_once_t  once_flag;\n#  define ONCE_FLAG_INIT PTHREAD_ONCE_INIT\n"
		new = ("#  if defined(__GLIBC__) && __GLIBC_USE(ISOC23) && !defined(__cplusplus)\n"
		       "#    define C11_LIBC_HAS_CALL_ONCE 1\n#  else\n"
		       "typedef pthread_once_t  once_flag;\n#    define ONCE_FLAG_INIT PTHREAD_ONCE_INIT\n#  endif\n")
		if old in s:
		    open(p, 'w').write(s.replace(old, new))
		p = root + '/src/c11/impl/threads_posix.c'
		s = open(p).read()
		old = "call_once(once_flag *flag, void (*func)(void))\n{\n    pthread_once(flag, func);\n}\n"
		if old in s and 'C11_LIBC_HAS_CALL_ONCE' not in s:
		    i = s.index(old)
		    j = s.rfind('\n\n', 0, i)
		    s = (s[:j] + "\n\n#ifndef C11_LIBC_HAS_CALL_ONCE" + s[j:i + len(old)] + "#endif\n" + s[i + len(old):])
		    open(p, 'w').write(s)
		with open(root + '/src/panfrost/drm-shim/meson.build', 'a') as f:
		    f.write("\n# HakiOS: the Galaxy S9's HakiosMali, for tools/mali-drmshim-check.sh\n"
		            "libhakios_mali_drm_shim = shared_library(\n"
		            "  'hakios_mali_drm_shim',\n  'hakios/hakios_mali_shim.c',\n"
		            "  include_directories: [inc_include, inc_src, include_directories('hakios')],\n"
		            "  dependencies: dep_drm_shim,\n  gnu_symbol_visibility : 'hidden',\n)\n")
	PY
	# the frontend, the shim and the allocator as links: no copy to forget to refresh
	rm -rf "$SRC/src/compiler/air"
	ln -s "$ROOT/tools/mesa/air" "$SRC/src/compiler/air"
	mkdir -p "$SRC/src/panfrost/drm-shim/hakios"
	ln -s "$ROOT/tools/mali-drmshim/hakios_mali_shim.c" "$SRC/src/panfrost/drm-shim/hakios/hakios_mali_shim.c"
	ln -s "$ROOT/drivers/gpu/hakios.mali_va.h" "$SRC/src/panfrost/drm-shim/hakios/hakios.mali_va.h"
	echo "$PATCHES" > "$SRC/.drmshim-patches"
fi

if [ ! -x venv/bin/meson ]; then
	python3 -m venv venv
	venv/bin/pip -q install meson mako pyyaml packaging
fi
export PATH=$B/venv/bin:$PATH

# tools/mk-mesa.sh's options, but for this machine, with the assertions on (debugoptimized: a
# broken invariant aborts here instead of drawing wrongly on the phone), drm-shim and panfrost's
# tools, and nothing of Darwin's
[ -f build/build.ninja ] || meson setup build "$SRC" --buildtype=debugoptimized \
	-Dplatforms= -Dgallium-drivers=softpipe,freedreno,panfrost -Dfreedreno-kmds=msm -Dvulkan-drivers= \
	-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled \
	-Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled \
	-Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= \
	-Dtools=drm-shim,panfrost -Dgallium-vdpau=disabled -Dgallium-va=disabled -Dgallium-xa=disabled \
	-Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C build src/gallium/targets/osmesa/libOSMesa.so.8.0.0 src/panfrost/drm-shim/libhakios_mali_drm_shim.so \
	src/panfrost/drm-shim/libpanfrost_noop_drm_shim.so src/gallium/targets/hakiosair/air_metal_test > ninja.log

rm -rf "$OUT"
mkdir -p "$OUT/gl" "$OUT/air"
cc -O1 -Wall -o "$OUT/gl_drmshim" "$ROOT/tools/mali-drmshim/gl_drmshim.c" -I"$SRC/include" \
	-L"$B/build/src/gallium/targets/osmesa" -lOSMesa
export LD_LIBRARY_PATH=$B/build/src/gallium/targets/osmesa:$B/build/src/mapi/shared-glapi
SHIM=$B/build/src/panfrost/drm-shim/libhakios_mali_drm_shim.so
AMT=$B/build/src/gallium/targets/hakiosair/air_metal_test
CHECK=$ROOT/tools/mali-drmshim/pandecode-check.py
FAILED=0
fail() { echo "mali-drmshim-check: FAIL: $*"; FAILED=$((FAILED + 1)); }

# Under the shim, with the decoder writing every chain into DIR.
on_mali() {
	local dir=$1
	shift
	mkdir -p "$dir"
	RWA_SOFTWARE=0 HAKIOS_MALI_SHIM_TRACE=$dir/shim.trace PAN_MESA_DEBUG=trace \
		PANDECODE_DUMP_FILE=$dir/pandecode.dump LD_PRELOAD=$SHIM "$@" > "$dir/stdout.log" 2> "$dir/stderr.log"
}
judge() {
	local dir=$1 what=$2
	grep -h '^hakios-mali-shim:' "$dir/stderr.log" | sed "s/^/$what: /"
	grep -q '^hakios-mali-shim: contract kept' "$dir/stderr.log" || fail "$what: the shim's contract line"
	python3 "$CHECK" "$dir" "$dir/shim.trace" "$dir/stderr.log" | sed "s/^/$what: /"
	python3 "$CHECK" "$dir" "$dir/shim.trace" "$dir/stderr.log" > /dev/null || fail "$what: pandecode-check"
}

echo "== OpenGL ES on panfrost, as the G72"
if on_mali "$OUT/gl/mali" "$OUT/gl_drmshim" 1080 2400 "$OUT/gl/mali/readback.rgba" fill; then
	grep '^gl_drmshim: Mesa' "$OUT/gl/mali/stdout.log"
	grep -q 'Mali-G72 (Panfrost)' "$OUT/gl/mali/stdout.log" || fail "GL: not panfrost as a Mali-G72"
	grep -q 'one with a blend shader; error 0' "$OUT/gl/mali/stdout.log" || fail "GL: a GL error"
else
	fail "GL on panfrost exited $?"
fi
judge "$OUT/gl/mali" GL

echo "== OpenGL ES on softpipe: the picture a phone run is compared with"
OSMESA_SOFTWARE=1 "$OUT/gl_drmshim" 1080 2400 "$OUT/gl/softpipe.rgba" > "$OUT/gl/softpipe.log"
grep -v '^gl_drmshim: Mesa' "$OUT/gl/softpipe.log" | cut -c1-200
python3 "$ROOT/tools/mali-drmshim/image.py" png "$OUT/gl/softpipe.rgba" 1080 2400 "$OUT/gl/softpipe.png"
want=$(cut -d' ' -f1 "$ROOT/tools/mali-drmshim/gl_test-1080x2400-softpipe.sha256")
got=$(sha256sum < "$OUT/gl/softpipe.rgba" | cut -d' ' -f1)
if [ "$want" = "$got" ]; then
	echo "softpipe: the picture is the one kept (sha256 $got)"
else
	fail "softpipe drew a picture other than the one kept ($got)"
fi
if [ -n "${COMPARE:-}" ]; then
	python3 "$ROOT/tools/mali-drmshim/image.py" compare "$COMPARE" "$OUT/gl/softpipe.rgba" 1080 2400 || true
fi

echo "== Metal: the tree's AIR modules through libhakiosair onto panfrost"
for ll in "$ROOT"/tools/air/*.ll; do
	"$LLVM_AS" -o "$OUT/air/$(basename "$ll" .ll).bc" "$ll"
done
cd "$OUT/air"
# what is asked of each, and what of it means anything without a GPU: the pipeline made (the
# shader compiled for v6) and the chains built. The pixels and buffers read back do not.
run_metal() {
	local name=$1
	shift
	set +e
	on_mali "$name" "$AMT" "$@"
	local rc=$?
	RWA_SOFTWARE=1 "$AMT" "$@" > "$name/softpipe.log" 2>&1
	local soft=$?
	set -e
	local chains
	chains=$(sed -n 's/.* \([0-9]*\) chains submitted.*/\1/p' "$name/stderr.log")
	if grep -q 'Assertion\|rwa_device_open:\|would not make' "$name/stdout.log" "$name/stderr.log" || [ "${chains:-0}" = 0 ]; then
		fail "Metal $name: not compiled, or no chain (exit $rc): $(grep -h 'Assertion\|rwa_\|would not' "$name/stdout.log" "$name/stderr.log" | head -2)"
	else
		echo "Metal $name: compiled for the G72, ${chains} chains built (exit $rc; what it read back is not a result); softpipe exit $soft"
	fi
	judge "$name" "Metal $name"
}
run_metal phiconst phiconst.bc
run_metal fragpos --draw fragpos.bc fragPos 125
run_metal argbuf --draw-argbuf argbuf_set.bc argbuf_direct.bc

echo
if [ "$FAILED" = 0 ]; then
	echo "mali-drmshim-check: PASS (everything in $OUT)"
else
	echo "mali-drmshim-check: $FAILED FAILED (everything in $OUT)"
	exit 1
fi
