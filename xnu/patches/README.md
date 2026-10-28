# Numbered kernel patches

`xnu/apply.sh` applies these in order after `hakios.patch`, `files/` and the drivers (`drivers/`). They keep work on
separate tracks from conflicting in the one big patch:

| Numbers | Track |
|---|---|
| 1000-1099 | platform: AVF/crosvm (`avf/`) |
| 1100-1199 | virtio-pci transport and drivers |
| 1200-1299 | display and input |
| 1300-1399 | GPU 3D: virtio-gpu 3D / gfxstream (`drivers/gpu/hakios.vgpu3d*.c`) |
| 1400-1499 | the Scion app's devices under AVF: camera, sound, flashlight, battery (`avf/CAMERA.md`) |

The patches change Apple's files and upstream's drivers. The AVF guest's own drivers are whole files in `drivers/`
(`drivers/map`, the last block), built by `standard` lines of their subsystem's `conf`: the patches do not touch
`bsd/conf/files`, whose HakiOS block `tools/hakios-config.py kernel` writes. (1301-1303, the virtio GPU's cursor queue,
fast completion and interrupt count, were changes to `hakios.vgpu.c` alone, and are in `drivers/gpu/hakios.vgpu.c` now.)

Make one with `git diff` in the kernel tree against the state after `apply.sh`, and mind that
`xnu/export.sh` folds everything in the tree into `hakios.patch`: export from a tree without
these patches applied (or revert them first), or the change ends up in both places.
