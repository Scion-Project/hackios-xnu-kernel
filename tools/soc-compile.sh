#!/bin/bash
# tools/soc-compile.sh [soc ...]: link a kernel with each SoC's table, one after another in this tree, after the tree's first
# build. A compile target: nothing here says a kernel runs on the SoC. Its defconfig is QEMU's with the blocks soc-check says
# are built; tools/soc-check.py --status names the rest.
#   JOBS=3 tools/soc-compile.sh           every socs/*/ with `block` lines and a <soc>_defconfig
cd "$(dirname "$0")/.." || exit 1
socs=("$@")
if [ ${#socs[@]} -eq 0 ]; then
	for d in socs/*/configs/*_defconfig; do
		s=$(basename "$d" _defconfig)
		grep -q '^[[:space:]]*block' "socs/$s/soc.conf" 2>/dev/null && socs+=("$s")
	done
fi
[ ${#socs[@]} -gt 0 ] || { echo "soc-compile: no SoC has a defconfig" >&2; exit 1; }
fail=0
for s in "${socs[@]}"; do
	python3 tools/hakios-config.py defconfig "${s}_defconfig" >/dev/null || { echo "soc-compile: $s: refused"; fail=$((fail + 1)); continue; }
	if tools/make-kernel.sh -j"${JOBS:-8}" >"soc-compile-$s.log" 2>&1 && grep -q "make-kernel: built" "soc-compile-$s.log"; then
		echo "soc-compile: $s: linked ($(grep -E '^#define HAKIOS_DRIVERS_COUNT' xnu-src/xnu-8796.121.2/bsd/dev/hakios.drivers.h | awk '{print $3}') drivers)"
	else
		echo "soc-compile: $s: FAILED, soc-compile-$s.log"
		grep -E ' error:' "soc-compile-$s.log" | head -5
		fail=$((fail + 1))
	fi
done
echo "soc-compile: ${#socs[@]} SoCs, $fail failed"
exit $((fail != 0))
