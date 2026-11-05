# SoC repositories

`tools/hakios-device.py sync` clones each SoC repository `devices/manifest.conf` names into `socs/<name>/`; `exynos9810/` is kept here, shared by every phone with that SoC. A new one:
[How the repositories fit](../docs/porting/repositories.md).

Only `exynos9810/` has booted. The others are tables with a compile target: [what each block lacks](../docs/soc-support.md).
