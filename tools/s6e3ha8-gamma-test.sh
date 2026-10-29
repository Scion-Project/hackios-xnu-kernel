#!/bin/sh
# The Galaxy S9 panel's gamma (drivers/gpu/hakios.s6e3ha8_gamma.h, the calculation drivers/gpu/hakios.s6e3ha8.c sends
# from) on this machine: tools/s6e3ha8_gamma_test.c built against it under the address and undefined-behaviour
# sanitizers, and run. The header is plain C: nothing of the kernel is needed.
# usage: tools/s6e3ha8-gamma-test.sh                         the checks
#        tools/s6e3ha8-gamma-test.sh --rows S0|S1 <C8 hex>   the 74 rows for a panel's MTP, as Android prints them
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all tools/s6e3ha8_gamma_test.c -lm \
	-o "$tmp/s6e3ha8_gamma_test"
"$tmp/s6e3ha8_gamma_test" "$@"
