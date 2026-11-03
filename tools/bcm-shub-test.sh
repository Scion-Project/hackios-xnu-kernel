#!/bin/sh
# The Galaxy S9's sensor hub's byte formats (drivers/input/hakios.bcm_shub.h: the SSI frames' status, the transport
# layer's packets and CRC-8, the RPCs, the firmware patch's records, SSP's instructions and data frames; what
# drivers/input/hakios.ssp.c uses) on this machine: tools/bcm_shub_test.c built against it under the address and
# undefined-behaviour sanitizers, and run. With the device repository synced it also reads the S9's firmware through
# (devices/starlte/firmware/sensorhub/bcm_sensorhub_a1.bin); without it those checks are skipped. The header is plain C.
# usage: tools/bcm-shub-test.sh [PATCH]
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all tools/bcm_shub_test.c -o "$tmp/bcm_shub_test"
"$tmp/bcm_shub_test" "${1:-devices/starlte/firmware/sensorhub/bcm_sensorhub_a1.bin}"
