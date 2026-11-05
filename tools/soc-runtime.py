#!/usr/bin/env python3
"""Make the kernel's table of every SoC from socs/*/include/, for a kernel that picks its SoC at boot.

  tools/soc-runtime.py            write xnu/files/bsd/sys/hakios.soc_runtime.h and bsd/dev/hakios.soc_runtime.c's table
  tools/soc-runtime.py --check    exit 1 if they are not what the tables make

Each EXYNOS_* an object-like number in a table becomes a field. A number no table of that SoC has is 0: its block is absent, and
a driver sees that before it maps anything (HAKIOS_SOC_HAS). The 9810's function-like macros and brace lists stay what they are:
only its drivers use them, and those drivers do not start on another SoC.
"""
import importlib.util
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NUM = re.compile(r"^#define\s+EXYNOS_([A-Z0-9_]+)\s+([^/\n]+?)\s*(?:/\*.*)?$", re.M)
FN = re.compile(r"^#define\s+EXYNOS_[A-Z0-9_]+\([^)]*\).*$", re.M)
# a brace list over several lines: a number only the 9810's driver takes, compile time on every SoC
BLOCK = re.compile(r"^#define\s+EXYNOS_[A-Z0-9_]+\s+\{\s*\\\n(?:.*\\\n)*.*$", re.M)
HDR = os.path.join(ROOT, "xnu", "files", "bsd", "sys", "hakios.soc_runtime.h")
SRC = os.path.join(ROOT, "drivers", "soc", "exynos", "hakios.soc_runtime.c")
GENERATED = "/* Made by tools/soc-runtime.py from the tables in socs/. Do not edit. */\n"


def tables():
    out = {}
    base = os.path.join(ROOT, "socs")
    for soc in sorted(os.listdir(base)):
        path = os.path.join(base, soc, "include", "hakios.%s_soc.h" % soc)
        if os.path.isfile(path):
            out[soc] = open(path, encoding="utf-8").read()
    return out


def soc_check():
    spec = importlib.util.spec_from_file_location("soc_check", os.path.join(ROOT, "tools", "soc-check.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(mod)
    return mod


def used():
    """The numbers read at run time: those of the drivers that do not stay on the 9810's table (HAKIOS_SOC_FIXED)."""
    names = set()
    for d, _dirs, files in os.walk(os.path.join(ROOT, "drivers")):
        for name in files:
            text = open(os.path.join(d, name), encoding="utf-8", errors="replace").read()
            if "sys/hakios.exynos_soc.h" in text and "HAKIOS_SOC_FIXED" not in text:
                names |= set(re.findall(r"\bEXYNOS_([A-Z0-9_]+)\b", text))
    return names


def make():
    sc, hd = soc_check(), None
    hd = sc.device_tool()
    reference = sc.macros(sc.REFERENCE)
    tabs = tables()
    want = used() | {"PRODUCT_ID"}
    nums = {}
    for soc, t in tabs.items():
        nums[soc] = {k: v for k, v in NUM.findall(t) if not v.endswith("\\") and k in want}
        verdict = sc.judge(soc, sc.read_soc(hd, soc)[2], sc.macros(soc), reference)
        for block, drivers in sc.DRIVERS.items():
            if not verdict[block][0]:       # a block not built is absent: its driver must see 0, whatever numbers are known
                for _symbol, path in drivers:
                    for m in sc.needs(path, reference):
                        nums[soc].pop(m[len("EXYNOS_"):], None)
    fields = sorted({k for n in nums.values() for k in n})
    keep = FN.findall(tabs["exynos9810"]) + BLOCK.findall(tabs["exynos9810"])
    h = [GENERATED, "#ifndef _SYS_HAKIOS_SOC_RUNTIME_H_", "#define _SYS_HAKIOS_SOC_RUNTIME_H_", "", "#include <sys/types.h>", "",
         "struct hakios_soc {", "\tconst char *name;", "\tuint32_t product_id;"]
    h += ["\tuint32_t %s;" % f.lower() for f in fields if f != "PRODUCT_ID"]
    h += ["};", "", "/* The table of the chip the kernel is on, picked at first use; all zero, every block absent, when no table has its chip id */",
          "extern const struct hakios_soc *hakios_soc_table;", "const struct hakios_soc *hakios_soc_select(void);", "int hakios_soc_is_9810(void);", "",
          "static inline const struct hakios_soc *", "hakios_soc(void)", "{",
          "\treturn __builtin_expect(hakios_soc_table != 0, 1) ? hakios_soc_table : hakios_soc_select();", "}", "",
          "#define HAKIOS_SOC_HAS(name)\t(EXYNOS_##name != 0)", ""]
    h += ["#define EXYNOS_%s\t(hakios_soc()->%s)" % (f, f.lower()) for f in fields if f != "PRODUCT_ID"]
    h += [""] + keep + ["", "#endif /* _SYS_HAKIOS_SOC_RUNTIME_H_ */", ""]
    c = [GENERATED, "#include <sys/param.h>", "#include <sys/systm.h>", "#include <sys/hakios.soc_runtime.h>", "", "extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);", ""]
    known = [s for s in tabs if "PRODUCT_ID" in nums[s]]
    for soc in known:
        c.append("static const struct hakios_soc soc_%s = {" % soc)
        c.append('\t.name = "%s",' % soc)
        c.append("\t.product_id = %s," % nums[soc]["PRODUCT_ID"])
        c += ["\t.%s = (uint32_t)(%s)," % (f.lower(), nums[soc][f]) for f in fields if f in nums[soc] and f != "PRODUCT_ID"]
        c += ["};", ""]
    c += ["static const struct hakios_soc *const socs[] = {"] + ["\t&soc_%s," % s for s in known] + ["};", "",
          "static const struct hakios_soc soc_none = { .name = \"none\" };", "", "const struct hakios_soc *hakios_soc_table;", "",
          "/* The chip id block is at 0x10000000 on every SoC with a table: Linux's exynos-chipid.c, PRO_ID, [31:12] the product */",
          "const struct hakios_soc *", "hakios_soc_select(void)", "{",
          "\tuint32_t id = *(volatile uint32_t *)ml_io_map(0x10000000, 0x1000) & 0xFFFFF000u;", "",
          "\tfor (unsigned i = 0; i < sizeof(socs) / sizeof(socs[0]); i++) {",
          "\t\tif (socs[i]->product_id == id) {", "\t\t\thakios_soc_table = socs[i];", "\t\t\treturn hakios_soc_table;", "\t\t}", "\t}",
          '\tprintf("hakios_soc: chip id 0x%08x is in no table: no SoC driver starts\\n", id);', "\thakios_soc_table = &soc_none;", "\treturn hakios_soc_table;", "}", "", "int", "hakios_soc_is_9810(void)", "{",
          "\tconst struct hakios_soc *soc = hakios_soc_table != 0 ? hakios_soc_table : hakios_soc_select();", "",
          "\treturn soc == &soc_exynos9810;", "}", ""]
    return "\n".join(h), "\n".join(c), known, sorted(set(tabs) - set(known)), len(fields)


def main():
    h, c, known, unknown, n = make()
    if "--check" in sys.argv:
        bad = [p for p, t in ((HDR, h), (SRC, c)) if not os.path.isfile(p) or open(p, encoding="utf-8").read() != t]
        if bad:
            print("soc-runtime: not what the tables make: %s" % ", ".join(os.path.relpath(p, ROOT) for p in bad))
            return 1
    else:
        open(HDR, "w", encoding="utf-8", newline="\n").write(h)
        open(SRC, "w", encoding="utf-8", newline="\n").write(c)
    print("soc-runtime: %d numbers; selectable: %s; no chip id known: %s" % (n, " ".join(known), " ".join(unknown)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
