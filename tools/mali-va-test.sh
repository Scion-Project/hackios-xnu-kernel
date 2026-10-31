#!/bin/sh
# The Galaxy S9 Mali's GPU addresses (drivers/gpu/hakios.mali_va.h, what drivers/gpu/hakios.mali.c gives the buffers of
# Mesa's panfrost) on this machine: tools/mali_va_test.c built against it under the address and undefined-behaviour
# sanitizers, and run. The header is plain C.
# usage: tools/mali-va-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all tools/mali_va_test.c -o "$tmp/mali_va_test"
"$tmp/mali_va_test"
