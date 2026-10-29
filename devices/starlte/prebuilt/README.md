# Galaxy S9: prebuilt UEFI

What `mkxnuimg.py` (`UEFI_TARGET=starlte`) and `devices/starlte/mkimage.sh` take when there is no `Mu-Silicium/` built here.

| File | What | Licence |
|---|---|---|
| `SILICIUM_UEFI.fd` | HakiOS's UEFI for the S9 | BSD-2-Clause, BSD-2-Clause-Patent; Carbon Hypervisor GPL-2.0 (below) |
| `BootShim.bin` | Silicium's boot shim with HakiOS's payload copy (`PAYLOAD_BASE=0xF0000000`), `FD_BASE=0x8F000000` | BSD-2-Clause-Patent |
| `mkbootimg.py` | AOSP's `mkbootimg`, as Silicium carries it | Apache-2.0 |
| `ramdisk` | Silicium's 5-byte placeholder | |

The UEFI is [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) at `uefi/BASE` (e51cd877e31c, 2026-10-09)
with `uefi/hakios.patch`, `uefi/files` and `devices/starlte/uefi/files`, built RELEASE in an Ubuntu 24.04 arm64 container:

```sh
docker run --rm -v "$PWD":/repo:ro -v /path/to/work:/work -w /work IMAGE bash -c 'MU=/work/Mu-Silicium bash /repo/uefi/apply.sh starlte'
docker run --rm -v /path/to/work:/work IMAGE bash -c 'cd /work/Mu-Silicium && python3 build_uefi.py -d starlte -r RELEASE'
# then the shim, as mkxnuimg.py builds it (build_uefi.py rebuilds Silicium's own, without the payload copy):
#   make -C Mu-Silicium/BootShim REQUIRES_KERNEL_HEADER=1 FD_BASE=0x8F000000 FD_SIZE=0x200000 \
#        'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE=0xF0000000'
```

IMAGE has Silicium's packages (its `setup_env.sh`, apt) and a venv with `Mu-Silicium/pip-requirements.txt`.

sha256: `SILICIUM_UEFI.fd` 5a46b859118fb325797d141ab492bb187bb23c605ddc44e449ade8d9b33f05cd,
`BootShim.bin` 7445cf6035702aa5b6f98bf20a931a296845f76d3e8334f23737df9395849035. The build is not yet proven
reproducible byte for byte (no `SOURCE_DATE_EPOCH`).

**Carbon Hypervisor.** The image carries `Silicon/Samsung/S5E9810Pkg/Include/Firmware/Hyp.bin` of that Silicium commit:
Project Silicium's [Carbon Hypervisor](https://github.com/Project-Silicium/Carbon-Hypervisor), GPL-2.0, built by Silicium
from that repository's source (its commit before 2026-10-09). It is the EL2 the kernel runs under, and answers PSCI.
