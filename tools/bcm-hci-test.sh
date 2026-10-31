#!/bin/sh
# The Galaxy S9's Bluetooth byte formats and arithmetic (drivers/bluetooth/hakios.bcm_hci.h: H4 framing, HCI events, Broadcom's
# .hcd patch file, the Exynos UART's divisor; what drivers/bluetooth/hakios.bcm4361_bt.c uses) on this machine:
# tools/bcm_hci_test.c built against it under the address and undefined-behaviour sanitizers, and run. The header is plain C.
# usage: tools/bcm-hci-test.sh
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all tools/bcm_hci_test.c -o "$tmp/bcm_hci_test"
"$tmp/bcm_hci_test"
