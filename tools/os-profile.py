#!/usr/bin/env python3
"""The OS profile a build is for: profiles/<name>.conf, chosen by OS_PROFILE (default ios-16.5).

  tools/os-profile.py get KEY     a key's value (xnu_tag, xnu_commit, ...); fails if the profile is not implemented
  tools/os-profile.py require     fails with the reason if the profile is not implemented
  tools/os-profile.py list        every profile and its status
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = "ios-16.5"
KEYS = {"status", "arch", "darwin", "xnu_tag", "xnu_commit", "patches", "adt", "userland"}


def read(name):
    path = os.path.join(ROOT, "profiles", name + ".conf")
    if not os.path.isfile(path):
        sys.exit("os-profile: no profile %s (tools/os-profile.py list)" % name)
    fields = {}
    for line in open(path, encoding="utf-8"):
        if line.startswith("\t"):
            key, _, value = line.strip().partition(" ")
            if key not in KEYS:
                sys.exit("os-profile: %s: unknown key %s" % (path, key))
            fields[key] = value.strip()
    missing = KEYS - set(fields)
    if missing:
        sys.exit("os-profile: %s lacks %s" % (path, ", ".join(sorted(missing))))
    return fields


def require(name, fields):
    if fields["status"] != "real":
        sys.exit("os-profile: %s is %s: this tree has no kernel for %s (%s). Only ios-16.5 builds; docs/modular-kernel.md has what is missing."
                 % (name, fields["status"], fields["xnu_tag"], fields["arch"]))


def main():
    name = os.environ.get("OS_PROFILE") or DEFAULT
    if not re.fullmatch(r"[a-z0-9.-]+", name):
        sys.exit("os-profile: bad name %r" % name)
    command = sys.argv[1] if len(sys.argv) > 1 else "list"
    if command == "list":
        for conf in sorted(os.listdir(os.path.join(ROOT, "profiles"))):
            print("%-10s %s" % (conf[:-5], read(conf[:-5])["status"]))
    elif command == "require":
        require(name, read(name))
    elif command == "get" and len(sys.argv) == 3:
        fields = read(name)
        require(name, fields)
        print(fields[sys.argv[2]])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
