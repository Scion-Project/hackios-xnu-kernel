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

  3. xnu/files/bsd/sys/hakios.macaddr.h, compiled with the host's cc: the phone's address as Samsung's Android writes it
     in /efs/wifi/.mac.info (brcmfmac=6), and what is refused (all zeroes, a group's address, anything malformed).
  4. wifi/lx/brcmfmac/lx_rsn.h, compiled with the host's cc: an access point's RSN element read, and ours for the
     association request made (brcmfmac=8): CCMP, PSK, the group's CCMP or TKIP; WPA3, enterprise networks, protected
     management frames required, WEP and broken elements refused.
  5. the patched driver again: lx_brcmf_attached() once brcmf_attach() is through, and brcmf_dmi_probe() called (the
     phone's settings) when built with CONFIG_DMI, as tools/mk-brcmfmac.sh builds it.

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


# ---- 3, 4. the phone's address, and the RSN elements -----------------------------------------------------------------
MAC_HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hakios.macaddr.h"
static int bad;
static void good(const char *text, const char *want) {
	uint8_t a[6] = { 0 };
	char got[32];
	if (!hakios_macaddr_parse(text, strlen(text), a)) { printf("refused: \"%s\"\n", text); bad = 1; return; }
	snprintf(got, sizeof(got), "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
	if (strcmp(got, want) != 0) { printf("\"%s\" is %s, not %s\n", text, got, want); bad = 1; }
}
static void refused(const char *text, size_t length) {
	uint8_t a[6] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 };
	if (hakios_macaddr_parse(text, length, a) || a[0] != 0x11) { printf("taken: \"%.*s\"\n", (int)length, text); bad = 1; }
}
#define REFUSED(t) refused(t, strlen(t))
int main(void) {
	good("00:12:34:AB:CD:EF\n", "00:12:34:ab:cd:ef");		/* (Samsung's random one: prefix 00:12:34, capitals) */
	good("f8:04:2e:01:02:03", "f8:04:2e:01:02:03");
	good("  0:12:34:5:6:7 \r\n", "00:12:34:05:06:07");
	good("00-12-34-ab-cd-ef", "00:12:34:ab:cd:ef");
	{
		const char file[] = "00:12:34:ab:cd:ef\n\0\0\0garbage";	/* (a NUL ends it) */
		uint8_t a[6];
		if (!hakios_macaddr_parse(file, sizeof(file) - 1, a) || a[5] != 0xef) { printf("NUL not taken as the end\n"); bad = 1; }
	}
	REFUSED("00:00:00:00:00:00\n");			/* (bcmdhd's "invalid" one) */
	REFUSED("01:12:34:ab:cd:ef");			/* (a group's) */
	REFUSED("00:12:34:ab:cd");
	REFUSED("00:12:34:ab:cd:ef:01");
	REFUSED("00:12:34:ab:cd:eff");
	REFUSED("00:12:34:ab:cd:ef x");
	REFUSED("00::12:34:ab:cd:ef");
	REFUSED("0012.34ab.cdef");
	REFUSED("");
	REFUSED("\n");
	refused("00:12:34:ab:cd:ef", 15);		/* (cut short by the length given: a separator last) */
	return bad;
}
"""

RSN_HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lx_rsn.h"
static int bad;
#define EQ(a, b) do { long long x = (a), y = (b); if (x != y) { printf("%s = %lld, not %lld\n", #a, x, y); bad = 1; } } while (0)
/* a beacon's elements: SSID "home", rates, DS channel 6, then the RSN element given, then a vendor element */
static unsigned beacon(uint8_t *out, const uint8_t *rsn, unsigned rsn_length) {
	static const uint8_t before[] = { 0, 4, 'h', 'o', 'm', 'e', 1, 4, 0x82, 0x84, 0x8b, 0x96, 3, 1, 6 };
	static const uint8_t after[] = { 0xdd, 7, 0x00, 0x50, 0xf2, 0x02, 0x00, 0x01, 0x00 };
	memcpy(out, before, sizeof(before));
	memcpy(out + sizeof(before), rsn, rsn_length);
	memcpy(out + sizeof(before) + rsn_length, after, sizeof(after));
	return (unsigned)(sizeof(before) + rsn_length + sizeof(after));
}
static const char *choose(const uint8_t *rsn, unsigned rsn_length, uint8_t *ours, unsigned *group) {
	uint8_t e[256];
	unsigned n = beacon(e, rsn, rsn_length);
	return lx_rsn_choose(e, n, ours, group);
}
int main(void) {
	/* WPA2-PSK, CCMP for both: the commonest home network */
	static const uint8_t wpa2[] = { 48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0x0c, 0 };
	static const uint8_t want[] = { 48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0 };
	/* WPA/WPA2 mixed: the group's TKIP, pairwise TKIP and CCMP */
	static const uint8_t mixed[] = { 48, 24, 1, 0, 0, 0x0f, 0xac, 2, 2, 0, 0, 0x0f, 0xac, 2, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0 };
	/* WPA2/WPA3 transition: PSK and SAE, MFP capable (not required) */
	static const uint8_t transition[] = { 48, 24, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 2, 0, 0, 0x0f, 0xac, 2, 0, 0x0f, 0xac, 8, 0x80, 0 };
	/* WPA3 only: SAE, MFP required */
	static const uint8_t wpa3[] = { 48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 8, 0xc0, 0 };
	/* PSK, but MFP required */
	static const uint8_t mfpr[] = { 48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0x40, 0 };
	/* enterprise (802.1X) */
	static const uint8_t eap[] = { 48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 1, 0, 0 };
	/* TKIP only */
	static const uint8_t tkip[] = { 48, 20, 1, 0, 0, 0x0f, 0xac, 2, 1, 0, 0, 0x0f, 0xac, 2, 1, 0, 0, 0x0f, 0xac, 2, 0, 0 };
	/* a count that runs past the element's end */
	static const uint8_t broken[] = { 48, 14, 1, 0, 0, 0x0f, 0xac, 4, 3, 0, 0, 0x0f, 0xac, 4, 1, 0 };
	uint8_t ours[LX_RSN_OURS_LENGTH], e[256];
	unsigned group = 0, n;
	const char *why;

	why = choose(wpa2, sizeof(wpa2), ours, &group);
	EQ(why == 0, 1);
	EQ(memcmp(ours, want, sizeof(want)), 0);
	EQ(group, LX_RSN_CCMP);
	why = choose(mixed, sizeof(mixed), ours, &group);
	EQ(why == 0, 1);
	EQ(group, LX_RSN_TKIP);
	EQ(ours[7], 2);
	EQ(ours[13], 4);
	why = choose(transition, sizeof(transition), ours, &group);
	EQ(why == 0, 1);
	EQ(ours[19], 2);
	EQ(choose(wpa3, sizeof(wpa3), ours, &group) != 0, 1);
	EQ(choose(mfpr, sizeof(mfpr), ours, &group) != 0, 1);
	EQ(choose(eap, sizeof(eap), ours, &group) != 0, 1);
	EQ(choose(tkip, sizeof(tkip), ours, &group) != 0, 1);
	EQ(choose(broken, sizeof(broken), ours, &group) != 0, 1);
	/* what kind of network */
	n = beacon(e, wpa2, sizeof(wpa2));
	EQ(lx_rsn_kind(e, n, 0x0411), LX_RSN_WPA2);
	n = beacon(e, (const uint8_t *)"", 0);
	EQ(lx_rsn_kind(e, n, 0x0401), LX_RSN_OPEN);
	EQ(lx_rsn_kind(e, n, 0x0411), LX_RSN_OLDER);	/* (privacy without RSN: WEP or WPA) */
	EQ(lx_rsn_choose(e, n, ours, &group) != 0, 1);
	/* an element whose length runs past the end is not read */
	{
		static const uint8_t cut[] = { 3, 1, 6, 48, 20, 1, 0, 0, 0x0f, 0xac, 4 };
		EQ(lx_rsn_element(cut, sizeof(cut), 3) == cut, 1);
		EQ(lx_rsn_element(cut, sizeof(cut), 48) == 0, 1);
		EQ(lx_rsn_kind(cut, sizeof(cut), 0x0011), LX_RSN_OLDER);
	}
	return bad;
}
"""

for name, harness, include, what in (
        ("macaddr", MAC_HARNESS, "xnu/files/bsd/sys",
         "hakios.macaddr.h: the phone's .mac.info and wifi_mac read; zeroes, a group's address and malformed text refused"),
        ("rsn", RSN_HARNESS, "wifi/lx/brcmfmac",
         "lx_rsn.h: WPA2 CCMP, mixed (the group's TKIP) and WPA2/WPA3 transition taken, with our RSN element; WPA3, "
         "enterprise, MFP required, TKIP only, WEP and broken elements refused")):
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "t.c")
        exe = os.path.join(tmp, "t")
        open(src, "w").write(harness)
        cc = subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I", os.path.join(ROOT, include), src, "-o", exe],
                            capture_output=True, text=True)
        if cc.returncode != 0:
            check("%s compiles on the host: %s" % (name, cc.stderr.strip()), False)
            continue
        run = subprocess.run([exe], capture_output=True, text=True)
        for line in run.stdout.splitlines():
            print("     " + line)
        check(what, run.returncode == 0)

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

# ---- 5. the station's hooks in the patched driver ------------------------------------------------------------------
setup = pcie[pcie.rindex("static void brcmf_pcie_setup(struct device *dev, int ret,\n"):]  # (the definition)
setup = setup[:setup.index("\nfail:")]
check("pcie.c: lx_brcmf_attached() once, in brcmf_pcie_setup(), after brcmf_attach() has come through",
      pcie.count("lx_brcmf_attached(dev);") == 1 and "lx_brcmf_attached(dev);" in setup
      and setup.index("ret = brcmf_attach(") < setup.index("lx_brcmf_attached(dev);"))
common_h = open(os.path.join(B, "brcmfmac/common.h")).read()
common_c = open(os.path.join(B, "brcmfmac/common.c")).read()
mk = open(os.path.join(ROOT, "tools/mk-brcmfmac.sh")).read()
check("brcmf_get_module_param() calls brcmf_dmi_probe(), declared (not an empty inline) with CONFIG_DMI, which "
      "mk-brcmfmac.sh sets; lx_board.c defines it",
      "brcmf_dmi_probe(settings, chip, chiprev);" in common_c
      and re.search(r"#ifdef CONFIG_DMI\s*\nvoid brcmf_dmi_probe\(", common_h) is not None and "-DCONFIG_DMI" in mk
      and "\nbrcmf_dmi_probe(struct brcmf_mp_device *settings" in open(os.path.join(ROOT, "wifi/lx/brcmfmac/lx_board.c")).read())

print("brcmfmac-test: %s" % ("all good" if not failed else "%d FAILED" % len(failed)))
sys.exit(1 if failed else 0)
