#!/bin/sh
# The USB gadget's CDC-ACM descriptors and control requests (drivers/core/hakios.usb_acm.h, what drivers/core/hakios.dwc3.c
# answers a host with) on this machine: tools/usb_acm_test.c built against it under the address and undefined-behaviour
# sanitizers, and run. The header is plain C.
# usage: tools/usb-acm-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all tools/usb_acm_test.c -o "$tmp/usb_acm_test"
"$tmp/usb_acm_test"
