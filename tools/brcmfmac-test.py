#!/usr/bin/env python3
"""Host-side checks of the Galaxy S9's brcmfmac (tools/mk-brcmfmac.sh, wifi/lx/brcmfmac), without the phone.

usage: tools/brcmfmac-test.py

  1. wifi/lx/brcmfmac/lx_pci_word.h, compiled with the host's cc: a byte, half-word or word of PCI configuration
     space read out of, and written into, the 32-bit word the root complex moves; the command word's status half
     never written with ones.
  2. brcmfmac as tools/mk-brcmfmac.sh fetches and patches it (it is fetched first if it is not there): for the
     chip IDs the BCM4361 can say (0x4347, 0x4361) and every chip revision, the firmware name brcmfmac's table
     (pcie.c, brcmf_pcie_fwnames) gives, the first line that matches as brcmf_fw_alloc_request() takes it; the RAM
     base chip.c gives them; the PCI ID in the driver's device table.

Exit status 0 when everything holds.
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
failed = []


def check(what, ok):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failed.append(what)


# ---- 1. configuration words ---------------------------------------------------------------------------------------
HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include "lx_pci_word.h"
static int bad;
#define EQ(a, b) do { unsigned long long x = (a), y = (b); if (x != y) { printf("%s = %#llx, not %#llx\n", #a, x, y); bad = 1; } } while (0)
int main(void) {
	uint32_t w = 0x12345678;
	EQ(lx_pci_word_get(w, 0, 4), 0x12345678);
	EQ(lx_pci_word_get(w, 0, 2), 0x5678);
	EQ(lx_pci_word_get(w, 2, 2), 0x1234);
	EQ(lx_pci_word_get(w, 1, 1), 0x56);
	EQ(lx_pci_word_get(w, 3, 1), 0x12);
	EQ(lx_pci_word_get(w, 0x83, 1), 0x12);			/* (only reg & 3 matters here) */
	EQ(lx_pci_word_put(w, 0x10, 0xdeadbeef, 4), 0xdeadbeef);
	EQ(lx_pci_word_put(w, 0x10, 0xbeef, 2), 0x1234beef);
	EQ(lx_pci_word_put(w, 0x12, 0xbeef, 2), 0xbeef5678);
	EQ(lx_pci_word_put(w, 0x11, 0xab, 1), 0x1234ab78);
	EQ(lx_pci_word_put(w, 0x13, 0x1ab, 1), 0xab345678);	/* (a byte is a byte) */
	/* the command word: status bits set (they are cleared by writing 1) are written as 0 */
	EQ(lx_pci_word_put(0xf8100002, 4, 0x0006, 2), 0x00000006);
	EQ(lx_pci_word_put(0xf8100002, 4, 0x04, 1), 0x00000004);
	EQ(lx_pci_word_put(0xf8100002, 5, 0x01, 1), 0x00000102);
	/* ... but a whole word is written as it is given */
	EQ(lx_pci_word_put(0xf8100002, 4, 0xf8100006, 4), 0xf8100006);
	return bad;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = os.path.join(tmp, "t.c")
    exe = os.path.join(tmp, "t")
    open(src, "w").write(HARNESS)
    cc = subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I", os.path.join(ROOT, "wifi/lx/brcmfmac"), src, "-o", exe],
                        capture_output=True, text=True)
    if cc.returncode != 0:
        check("lx_pci_word.h compiles on the host: " + cc.stderr.strip(), False)
    else:
        run = subprocess.run([exe], capture_output=True, text=True)
        for line in run.stdout.splitlines():
            print("     " + line)
        check("lx_pci_word.h: reads and writes of 1, 2 and 4 bytes; the command word's status half", run.returncode == 0)

# ---- 2. the patched driver -----------------------------------------------------------------------------------------
B = os.path.join(ROOT, "xnu-deps/build/brcmfmac/src/brcm80211")
fetch = subprocess.run([os.path.join(ROOT, "tools/mk-brcmfmac.sh"), "fetch"], capture_output=True, text=True)
if fetch.returncode != 0 or not os.path.isdir(B):
    check("tools/mk-brcmfmac.sh fetch: the driver's sources, patched: " + (fetch.stderr.strip() or fetch.stdout.strip()), False)
    sys.exit(1)
check("tools/mk-brcmfmac.sh fetch: Linux's brcmfmac, every file ISC, wifi/lx/brcmfmac/patches applied", True)
pcie = open(os.path.join(B, "brcmfmac/pcie.c")).read()
chip = open(os.path.join(B, "brcmfmac/chip.c")).read()
firmware = open(os.path.join(B, "brcmfmac/firmware.c")).read()
ids = open(os.path.join(B, "include/brcm_hw_ids.h")).read()

defines = {m.group(1): int(m.group(2), 0) for m in re.finditer(r"#define\s+(BRCM_\w+)\s+(0x[0-9a-fA-F]+|\d+)", ids)}
names = {m.group(1): m.group(2) for m in re.finditer(r'BRCMF_FW_(?:CLM_)?DEF\((\w+),\s*"([^"]+)"\)', pcie)}
table = pcie[pcie.index("brcmf_pcie_fwnames[] = {"):]
table = table[:table.index("};")]
entries = [(defines.get(c, None), int(mask, 16), n) for c, mask, n in
           re.findall(r"BRCMF_FW_ENTRY\((\w+),\s*(0x[0-9A-Fa-f]+),\s*(\w+)\)", table)]

# brcmf_fw_alloc_request(): the first line whose chip is the chip's and whose mask has the revision's bit
check("firmware.c takes the first line of the table that matches (chip, and the revision's bit in the mask)",
      re.search(r"if \(mapping_table\[i\]\.chipid == chip &&\s*mapping_table\[i\]\.revmask & BIT\(chiprev\)\)\s*break;",
                firmware) is not None)


def firmware_for(chipid, rev):
    for c, mask, n in entries:
        if c == chipid and mask & (1 << rev):
            return names.get(n)
    return None


for chipid in (0x4347, 0x4361):
    got = {rev: firmware_for(chipid, rev) for rev in range(32)}
    want = {rev: None if rev < 3 else "brcmfmac4361b0-pcie" if rev < 5 else "brcmfmac4361b2-pcie" for rev in range(32)}
    check("chip %#x: revisions 0-2 no firmware (A0, A1), 3-4 brcmfmac4361b0-pcie (B0, B1), 5-31 brcmfmac4361b2-pcie (B2...)"
          % chipid, got == want)
    if got != want:
        for rev in range(32):
            if got[rev] != want[rev]:
                print("     revision %d: %s, not %s" % (rev, got[rev], want[rev]))

rambase = chip[chip.index("static u32 brcmf_chip_tcm_rambase"):]
rambase = rambase[:rambase.index("\n}\n")]
for name in ("BRCM_CC_4347_CHIP_ID", "BRCM_CC_4361_CHIP_ID"):
    # the case's own return is the first "return" after it
    at = rambase.find("case %s:" % name)
    check("chip.c: %s has a RAM base of 0x170000 (CR4_4347_RAM_BASE)" % name,
          at >= 0 and re.match(r"[\s\S]*?return\s+(0x[0-9a-f]+);", rambase[at:]).group(1) == "0x170000")
check("brcm_hw_ids.h: BRCM_PCIE_4361_DEVICE_ID is 0x441f, BRCM_CC_4347/4361_CHIP_ID 0x4347/0x4361",
      defines.get("BRCM_PCIE_4361_DEVICE_ID") == 0x441f and defines.get("BRCM_CC_4347_CHIP_ID") == 0x4347
      and defines.get("BRCM_CC_4361_CHIP_ID") == 0x4361)
check("pcie.c: the device table has BRCM_PCIE_4361_DEVICE_ID (vendor 0x14e4, class 0x0280xx)",
      "BRCMF_PCIE_DEVICE(BRCM_PCIE_4361_DEVICE_ID, WCC)" in pcie)
check("pcie.c: brcmfmac=N stops it before the firmware goes to the chip (3) and before brcmf_attach() (4)",
      pcie.count("lx_brcmf_may(3,") == 1 and pcie.count("lx_brcmf_may(4,") == 1)

print("brcmfmac-test: %s" % ("all good" if not failed else "%d FAILED" % len(failed)))
sys.exit(1 if failed else 0)
