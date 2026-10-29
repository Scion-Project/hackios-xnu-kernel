# Numbered kernel patches

`xnu/apply.sh` applies these in order after `hakios.patch`, `files/` and the drivers (`drivers/`). They keep work on
separate tracks from conflicting in the one big patch:

| Numbers | Track |
|---|---|
| 1000-1099 | platform: AVF/crosvm (`avf/`) |
| 1100-1199 | virtio-pci transport and drivers |
| 1200-1299 | display and input |
| 1300-1399 | GPU 3D: virtio-gpu 3D / gfxstream (`drivers/gpu/hakios.vgpu3d*.c`) |
| 1400-1499 | the Scion app's devices under AVF: camera, sound, flashlight (`avf/CAMERA.md`) |
| 1500-1599 | other phones of this fork (the Galaxy S9: `devices/starlte`) where they touch upstream's or Apple's files |

The patches change Apple's files and upstream's drivers. The AVF guest's own drivers are whole files in `drivers/`
(`drivers/map`, the last block), built by `standard` lines of their subsystem's `conf`: the patches do not touch
`bsd/conf/files`, whose HakiOS block `tools/hakios-config.py kernel` writes. (1301-1303, the virtio GPU's cursor queue,
fast completion and interrupt count, were changes to `hakios.vgpu.c` alone, and are in `drivers/gpu/hakios.vgpu.c` now;
1403, which started the AVF battery from `hakios.input.c`, is a line of that file now.)

**A file one of these patches touches is changed by a patch here, never in place** (`grep '^+++ b/' xnu/patches/*.patch`
lists them): these patches apply on top of it, and an edit in place breaks their context (#59 broke 1200 so; 1500
carries that change now). After any change here, `XNU_URL=file://$PWD/xnu-src/xnu-8796.121.2 bash xnu/apply.sh /tmp/t` must end in
"N numbered patches".

Make one with `git diff` in the kernel tree against the state after `apply.sh`, and mind that
`xnu/export.sh` folds everything in the tree into `hakios.patch`: export from a tree without
these patches applied (or revert them first), or the change ends up in both places.
