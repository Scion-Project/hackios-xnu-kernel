# gfxstream GLES encoder for eagl.c: status (paused 2026-10-22)

Goal: CoreAnimation in backboardd composites on the Pixel's PowerVR. The chain is
eagl.c (src/eagl.c) → a GLES library that **encodes** calls in gfxstream's wire format →
`/dev/vgpu3d` (kernel, hackios-xnu-kernel `redwood_vgpu3d*`) → virtio-gpu pipe → gfxstream in
AVF's crosvm → PowerVR. The design is in scion `docs/GPU.md`.

## Done
- `mkgfxenc.py`: generates C encoders for every renderControl and GLES 2/3 entry from
  gfxstream's own codegen descriptions, fetched at a pinned commit by `fetch.sh` (Apache-2.0;
  nothing of gfxstream is committed). It makes 441 encoders. Its opcodes are checked against
  gfxstream's `gl2_opcodes.h` and `renderControl_opcodes.h` and all match. The wire layout is
  the same as the hand-written encoders that drew 7/7 on the phone (kernel self-test).

## Found
- The macOS Android emulator ships `libgfxstream_backend.dylib`
  (`~/Library/Android/sdk/emulator/lib64`) with the `stream_renderer_*` API. That is the same
  frontend AVF's crosvm uses, so the encoder can be tested on the Mac against a real gfxstream
  host without the phone: context, pipe resource, `transfer_write/read_iov`.
- The 2D driver (patch 1202, `vgpu_setup`) sets up only queue 0. crosvm activates virtio-gpu
  only with **both** queues, so on AVF it needs the cursor queue too. That fix goes in a
  13xx patch or to agent D.
- gfxstream ignores the transfer stride (rows are `width*bpp`), so IOSurfaces with padded
  `bytesPerRow` need a tight bounce buffer in the kernel and a strided copyin/copyout.
- CoreAnimation uses 62 GL entry points (`src/eagl_slots.txt`, '*'), all client-side arrays
  and no VBOs.

## Next
1. `src/gfx/`: the stream (`gfx_begin`/`gfx_read`, a growable buffer), `gfx_gl.c` (exported
   `gl*`), and hand-written client state.
   - Client state: `glVertexAttribPointer` client arrays, then `glVertexAttribPointerData`
     at draw time. `glDrawElements` sends the min..max index range plus
     `glDrawElementsData`. `glShaderSource` becomes `glShaderString`.
   - Pixels: tight packing for `glTexImage2D`/`glTexSubImage2D`/`glReadPixels`, with host
     pack/unpack alignment 1.
   - The rest: `glGetString` through `rcGetGLString`, `glFinish` as `glFinishRoundTrip`,
     and syncs through `gl*SyncAEMU`.
2. A macOS test host over `libgfxstream_backend.dylib`, running gl_test.c (a triangle) and
   reading it back.
3. Kernel: `/dev/vgpu3d`, a devfs clone node with one virtio-gpu context and pipe per open.
   `write`/`read` carry the stream. ioctls: `CREATE` (a colour-buffer resource with a
   kernel-backed tight buffer), `UPLOAD`/`DOWNLOAD` (rect, user pointer, user stride) and
   `UNREF`. All of it goes over `redwood_vgpu_ctrl`.
4. `gfx_redwood.c`: `redwood_gl.h` for eagl.c.
   - Contexts: `rcChooseConfig`/`rcCreateContext`/`rcMakeCurrent` with a 16×16 window
     surface.
   - `redwood_gl_surface_texture`: IOSurface → resource → `rcBindTexture`. Upload when the
     surface seed changes (`glBindTexture`); download render targets at frame end
     (`glFlush`/`glFinish`), so the IOMFB swap copies the GPU's frame into SCFB.
5. Scion's AVF backend: a `GpuConfig` (backend gfxstream, context `gfxstream-gles`, egl, gles,
   surfaceless). Then measure SpringBoard fps (eagl `glframes=1`) before and after.
