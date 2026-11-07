#!/bin/sh
# The boot chooser's pure part (xnuloader/chooser.h) on this machine, and its promise: no partition is written.
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined tools/chooser_test.c -o "$tmp/chooser_test"
"$tmp/chooser_test"
# the chooser reads storage and never writes it: no write, flush or erase call in either file
if grep -n -E 'write_blocks|flush_blocks|write_blocks_ex|flush_log|block_write|ufs_write' xnuloader/chooser.c xnuloader/chooser.h; then
	echo "chooser-test: a storage write in the chooser"; exit 1
fi
echo "chooser-test: no storage write in chooser.c or chooser.h"
