#!/usr/bin/env python3
"""What each socs/<soc>/ says of its blocks, checked against the numbers it has and the drivers that use them.

  tools/soc-check.py            exit 1 if a table, a fragment or the matrix disagree (CI)
  tools/soc-check.py --status   print the matrix: a block with no driver for this SoC is named here, and in the kernel it is absent
  tools/soc-check.py --write    remake the matrix in docs/soc-support.md

A block is built for a SoC when its state is `table` and the SoC's header defines every EXYNOS_* of the 9810's that its driver uses.
Every other block's driver has to be off in the SoC's fragment: a missing number is a block that is absent, not one that compiles
with another SoC's address.

States, in soc.conf's `block <name> <state> <sources> <note>`:
  runs       seen working on a phone (the 9810 alone)
  table      the same IP: numbers are all it takes (they may not all be known yet: the matrix counts them)
  code       the same family: some of the driver has to be written
  different  another IP, or the SoC does not have the block
  unknown    not established from the sources read
"""
import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BLOCKS = ("cpu", "acpm", "gic", "timer", "uart", "ufs", "usb", "dpu", "gpu", "mfc", "usi", "pmic", "wdt")
STATES = ("runs", "table", "code", "different", "unknown")
# the block's drivers: (the symbol that builds it, its file)
DRIVERS = {
    "acpm": (("EXYNOS_ACPM", "drivers/soc/exynos/hakios.exynos_acpm.c"),),
    "ufs": (("UFS", "drivers/storage/ufs/hakios.ufshc.c"),),
    "usb": (("EXYNOS_USB", "drivers/usb/hakios.exynos_usb.c"),),
    "dpu": (("EXYNOS_DPU", "drivers/display/dpu/hakios.exynos_dpu.c"),),
    "gpu": (("MALI", "drivers/gpu/mali/hakios.mali.c"),),
    "mfc": (("EXYNOS_MFC", "drivers/media/mfc/hakios.exynos_mfc.c"),),
    "usi": (("EXYNOS_HSI2C", "drivers/soc/exynos/hakios.exynos_hsi2c.c"), ("EXYNOS_SPI", "drivers/soc/exynos/hakios.exynos_spi.c")),
}
# the 9810's phones' own drivers, and the blocks not in the matrix: off for every other SoC
PHONE_ONLY = ("EXYNOS_ABOX", "EXYNOS_CAMERA", "EXYNOS_CPUFREQ", "EXYNOS_IRIS", "EXYNOS_KEYS", "EXYNOS_M3", "EXYNOS_PCIE",
              "EXYNOS_POWEROFF", "EXYNOS_TEE", "EXYNOS_TMU", "BCM4361_BT", "BRCMFMAC", "CS47L93", "MAX77705", "MAX98512",
              "S2MPB02_TORCH", "S2MPS18_RTC", "S6E3HA8", "S6SY761", "SSP")
REFERENCE = "exynos9810"
MACRO = re.compile(r"^#define\s+(EXYNOS_[A-Z0-9_]+)(?:\([^)]*\))?\s+(.*)$", re.M)
SOURCED = re.compile(r"/\*\s*(guess:.*|[A-Z](?::.*)?)\s*\*/\s*$")
BEGIN, END = "<!-- soc-check: matrix -->", "<!-- soc-check: end -->"


def device_tool():
    spec = importlib.util.spec_from_file_location("hakios_device", os.path.join(ROOT, "tools", "hakios-device.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(mod)
    return mod


def header(soc):
    path = os.path.join(ROOT, "socs", soc, "include", "hakios.%s_soc.h" % soc)
    return path, open(path, encoding="utf-8").read() if os.path.isfile(path) else ""


def macros(soc):
    return {m.group(1): m.group(2) for m in MACRO.finditer(header(soc)[1])}


def needs(path, reference):
    text = open(os.path.join(ROOT, path), encoding="utf-8").read()
    return sorted(set(re.findall(r"\bEXYNOS_[A-Z0-9_]+\b", text)) & set(reference))


def read_soc(hd, soc):
    fields = hd.describe("soc", soc)
    sources = {}
    for value, _n in fields.get("source", []):
        key, _, what = value.partition(" ")
        sources[key] = what.strip()
    blocks = {}
    for value, n in fields.get("block", []):
        parts = value.split(None, 3)
        if len(parts) < 4:
            raise SystemExit("socs/%s/soc.conf:%d: block <name> <state> <sources> <note>" % (soc, n))
        blocks[parts[0]] = dict(state=parts[1], src=parts[2], note=parts[3])
    return fields, sources, blocks


def judge(soc, blocks, have, reference):
    """{block: (built?, numbers it has, numbers it needs, missing)} for the blocks with a driver."""
    out = {}
    for block, drivers in DRIVERS.items():
        need = sorted({m for _sym, path in drivers for m in needs(path, reference)})
        missing = [m for m in need if m not in have]
        state = blocks.get(block, {}).get("state")
        out[block] = (state in ("table", "runs") and not missing, len(need) - len(missing), len(need), missing)
    return out


def check_soc(soc, fields, sources, blocks, have, verdict):
    errors = []
    for block in BLOCKS:
        if block not in blocks:
            errors.append("%s: block %s is not said" % (soc, block))
    for block, b in blocks.items():
        if block not in BLOCKS:
            errors.append("%s: block %s is not one of the matrix's" % (soc, block))
        if b["state"] not in STATES:
            errors.append("%s: block %s: state %s" % (soc, block, b["state"]))
        if b["state"] == "runs" and soc != REFERENCE:
            errors.append("%s: block %s: nothing but the %s has run" % (soc, block, REFERENCE))
        if b["state"] != "unknown" and soc != REFERENCE and not all(k in sources for k in b["src"].split(",")):
            errors.append("%s: block %s: source %s is not one of soc.conf's %s" % (soc, block, b["src"], sorted(sources)))
    path, text = header(soc)
    if soc == REFERENCE or not text:
        return errors
    for line in text.splitlines():
        if line.startswith("#define EXYNOS_"):
            m = SOURCED.search(line)
            if not m:
                errors.append("%s: no source on: %s" % (os.path.relpath(path, ROOT), line))
            elif not m.group(1).startswith("guess:") and m.group(1)[0] not in sources:
                errors.append("%s: source %s is not in soc.conf: %s" % (os.path.relpath(path, ROOT), m.group(1)[0], line))
    frag = open(os.path.join(ROOT, "socs", soc, "configs", soc + ".config"), encoding="utf-8").read()
    off = set(re.findall(r"^# CONFIG_(\S+) is not set", frag, re.M))
    for block, (built, _h, _n, _m) in verdict.items():
        for symbol, _path in DRIVERS[block]:
            name = "HAKIOS_" + symbol
            if built and name in off:
                errors.append("%s: %s is off in the fragment, and %s has its numbers" % (soc, name, block))
            if not built and name not in off:
                errors.append("%s: %s must be off in the fragment: %s is not built" % (soc, name, block))
    for symbol in PHONE_ONLY:
        if "HAKIOS_" + symbol not in off:
            errors.append("%s: HAKIOS_%s must be off in the fragment" % (soc, symbol))
    return errors


def compile_header(soc):
    """The table as C: every address fits the 32 bits the blocks are in. Skipped, and said so, with no C compiler."""
    cc = shutil.which("cc")
    if not header(soc)[1]:
        return []
    if not cc:
        print("soc-check: %s: no cc, the header was not compiled" % soc)
        return []
    path, text = header(soc)
    lines = ["#include \"%s\"" % path]
    for name in MACRO.findall(text):
        if name[0].endswith("_PA"):
            lines.append("_Static_assert((unsigned long long)(%s) <= 0xFFFFFFFFull, \"%s\");" % (name[0], name[0]))
    r = subprocess.run([cc, "-fsyntax-only", "-Wall", "-Werror", "-x", "c", "-"], input="\n".join(lines) + "\n", text=True, capture_output=True)
    return ["%s: %s" % (soc, r.stderr.strip())] if r.returncode else []


def cell(block, b, verdict):
    state = {"runs": "runs", "table": "table only", "code": "needs code", "different": "different IP", "unknown": "unknown"}[b["state"]]
    if block in verdict and b["state"] == "table" and verdict[block][2]:
        built, have, need, _missing = verdict[block]
        return "%s, %d/%d%s" % (state, have, need, ", built" if built else "")
    if block in verdict and b["state"] == "table":
        return "%s, built" % state
    return state


def matrix(rows):
    out = ["| SoC | " + " | ".join(BLOCKS) + " |", "|---|" + "---|" * len(BLOCKS)]
    for soc, _fields, _sources, blocks, verdict in rows:
        out.append("| `%s` | %s |" % (soc, " | ".join(cell(b, blocks.get(b, dict(state="unknown")), verdict) for b in BLOCKS)))
    return "\n".join(out)


def notes(rows):
    out = []
    for soc, _fields, sources, blocks, _verdict in rows:
        if soc == REFERENCE:
            continue
        out.append("\n### `%s`: %s, %s\n" % (soc, _fields["title"][-1][0], _fields["gpu"][-1][0]))
        out.append("| Block | State | Source | Why |\n|---|---|---|---|")
        for b in BLOCKS:
            out.append("| %s | %s | %s | %s |" % (b, blocks[b]["state"], blocks[b]["src"], blocks[b]["note"]))
        out.append("")
        out.extend("- %s: %s" % kv for kv in sources.items())
    return "\n".join(out)


def status(rows):
    for soc, _fields, _sources, blocks, verdict in rows:
        built = [b for b in verdict if verdict[b][0]]
        print("%-11s built: %-22s" % (soc, " ".join(built) or "(none)"))
        for b in BLOCKS:
            state = blocks[b]["state"]
            if b in verdict and not verdict[b][0]:
                print("  not implemented: %-5s %s%s" % (b, state, ", numbers %d/%d" % verdict[b][1:3] if state == "table" else ""))
            elif b not in verdict and state != "runs" and state != "table":
                print("  not implemented: %-5s %s" % (b, state))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()
    hd = device_tool()
    reference = macros(REFERENCE)
    names = sorted(n for n in os.listdir(os.path.join(ROOT, "socs"))
                   if os.path.isfile(os.path.join(ROOT, "socs", n, "soc.conf")) and hd.describe("soc", n).get("block"))
    absent = [n for kind, n, f, _l in hd.manifest() if kind == "soc" and n not in names and hd.describe("soc", n) is None]
    rows, errors = [], []
    for soc in names:
        fields, sources, blocks = read_soc(hd, soc)
        have = macros(soc)
        verdict = judge(soc, blocks, have, reference)
        errors += check_soc(soc, fields, sources, blocks, have, verdict)
        errors += compile_header(soc)
        rows.append((soc, fields, sources, blocks, verdict))
    if a.status:
        status(rows)
        return 0
    doc = os.path.join(ROOT, "docs", "soc-support.md")
    text = open(doc, encoding="utf-8").read()
    head, rest = text.split(BEGIN)
    _old, tail = rest.split(END)
    made = "%s%s\n%s\n\n%s\n%s%s" % (head, BEGIN, matrix(rows), notes(rows), END, tail)
    if absent:
        print("soc-check: %s not checked out: the matrix was neither compared nor written" % " ".join(absent))
    elif a.write:
        open(doc, "w", encoding="utf-8", newline="\n").write(made)
    elif made != text:
        errors.append("docs/soc-support.md: not what soc.conf makes: run tools/soc-check.py --write")
    if errors:
        print("\n".join("soc-check: " + e for e in errors))
        return 1
    print("soc-check: %d SoCs, %d built blocks" % (len(rows), sum(v[0] for r in rows for v in r[4].values())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
