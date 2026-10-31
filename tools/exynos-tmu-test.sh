#!/bin/sh
# The Exynos 9810 TMU's register map and calibration arithmetic (drivers/power/hakios.exynos_tmu.h, what
# drivers/power/hakios.exynos_tmu.c reads the Galaxy S9's temperatures with) on this machine: tools/exynos_tmu_test.c
# built against it under the address and undefined-behaviour sanitizers, and run. The header is plain C.
# usage: tools/exynos-tmu-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all tools/exynos_tmu_test.c -o "$tmp/exynos_tmu_test"
"$tmp/exynos_tmu_test"
