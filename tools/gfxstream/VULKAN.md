# Vulkan in the iOS guest through gfxstream (stages 1 and 2, 2026-10-29)

Goal: Vulkan in iOS under AVF, drawn by the Pixel 10's PowerVR, the way GL ES already is: the
guest encodes calls in gfxstream's wire format, `/dev/vgpu3d` and SCGPU carry the bytes, and the
Scion app's gfxstream decodes them on the phone's GPU. It is the base for a later Metal-on-Vulkan
layer.

- **Stage 1 (done, on the Mac):** gfxstream's own guest Vulkan driver, built for macOS, talks to
  the Android emulator's `libgfxstream_backend.dylib` (a real gfxstream host, MoltenVK under it)
  in one process. `userland/src/vk_test.c`, nothing but the Vulkan API, passes every check.
- **Stage 2 (built, not run):** the same library for arm64-apple-ios16 as `libhakios.vk.dylib`
  (install name `/redwood/lib/libhakios.vk.dylib`) with the stream over `/dev/vgpu3d`, and
  `vk_test` for iOS. Nothing in the kernel or the app was changed.
- **Stage 3 (to do):** the app and the kernel; see the list at the end.

Build and run: `tools/gfxstream/mk-vk.sh [mac|ios|all]`; `tools/gfxstream/mk-vk.sh mac run [words]`
runs vk_test against the emulator. Output goes to `$VK_BUILD` (default
`xnu-deps/build/gfxstream-vk`); a clean build of both is about a minute and uses about 400 MB.

## Where the driver comes from, and the pin

The gfxstream commit fetch.sh pins (0884d2d, 2026-10-19) **has no guest Vulkan driver**: gfxstream
moved its guest side (vulkan_enc, ResourceTracker, the ICD, the connection manager, the platform
layer) into Mesa, `src/gfxstream/guest`, built on Mesa's common Vulkan runtime. So the guest is
pinned to **Mesa 25.0.7**, the release tools/mk-mesa.sh already pins (same tarball, same SHA-256),
and the host stays what it is: 0884d2d on the phone, the emulator's own build (36.1.9, 13823996) on
the Mac. Mesa's gfxstream files are MIT (SPDX in each file); gfxstream's host is Apache-2.0.
Nothing of either is committed: mk-vk.sh unpacks the parts of Mesa it needs (94 MB of 292),
applies `vk/mesa-gfxstream-scion.patch` (header names upstream and licence) and copies in
`vk/scion/`, ours.

The guest is older than the phone's host. gfxstream hosts keep decoding older guests (every
Android image in the field is one), and the emulator's host, of the same year as Mesa 25.0.7,
decodes it without complaint. Whether 0884d2d does is unverified until stage 3.

What is ours (`tools/gfxstream/vk/scion/`, built into Mesa's tree as `platform/scion`):

| file | what |
|---|---|
| `scion_vk_transport.{h,c}` | the byte pipe. iOS: an open of `/dev/vgpu3d`. macOS: a stream_renderer context + 1 MiB PIPE_BUFFER resource + `"pipe:opengles"` in this process (as `host_test/host.c` does for GL) |
| `ScionStream.{h,cpp}` | gfxstream's `IOStream` over that pipe: writes gathered, sent when full or when a reply is wanted |
| `ScionVirtGpu.cpp` | the `VirtGpuDevice` the driver asks for: caps only (no blobs, no execbuffer, no sync fds) |
| `mkvkexports.py` | the 234 core Vulkan commands (1.0 to 1.4) as exported `vk*` functions: iOS has no loader |

What the patch changes in Mesa: the transport selection, the non-direct memory path (below), a
stream desync in the encoder's invalidate (below), the exports and link flags, two darwin build
fixes. 281 lines.

## Stage 1 result (the Mac, `mk-vk.sh mac run`)

```
VKTEST instance: Vulkan 1.3.0 offered, 218.9 ms (the stream to the host included)
VKTEST PASS physical device
VKTEST physical device: "Apple M1 Pro" (vendor 0x106b device 0x1b000207, Vulkan 1.2.306, driver 0x000027e3, type 1)
VKTEST memory type 0: heap 0 device-local
VKTEST memory type 1: heap 0 device-local host-visible host-cached
VKTEST memory type 2: heap 0 device-local
VKTEST PASS a compute queue family
VKTEST device and queue (family 0 of 1): 1.6 ms
VKTEST buffers of 262144 words: device in memory type 0, host-visible in type 1 (not coherent)
VKTEST PASS vkCmdFillBuffer + vkCmdCopyBuffer, read back: 262144 words of 0xc0ffee11 (submit and wait 13.88 ms; invalidate 0.84 ms: 1251 MB/s)
VKTEST PASS flushed pattern through device memory and back: 262144 words (flush 0.41 ms: 2545 MB/s; submit and wait 0.57 ms)
VKTEST PASS compute shader: 262144 words of i * 3 + 7 (pipeline 7.5 ms; dispatch, copy and wait 0.82 ms)
VKTEST an empty submit and wait: 0.033 ms
VKTEST OK: 0 failed, 253 ms in all
```

The device name is MoltenVK's for this Mac's GPU, seen through gfxstream. The compute shader is
SPIR-V embedded as words (its GLSL is in a comment in vk_test.c). Measured (M1 Pro, in-process,
so the transport is memcpys and function calls; the phone adds the kernel, SCGPU's ring and AVF):

| what | 1 MiB buffers | 16 MiB buffers |
|---|---|---|
| vkCreateInstance, first in the process (brings the whole host up: ANGLE, MoltenVK) | 160-520 ms | 250 ms |
| vkCreateDevice | 1-4 ms | 1.4 ms |
| first submit + wait (fill + copy) | 6-19 ms | 15 ms |
| vkFlushMappedMemoryRanges, whole buffer | 0.4-0.5 ms (2.0-2.5 GB/s) | 13 ms (1.3 GB/s) |
| vkInvalidateMappedMemoryRanges, whole buffer | 0.6-1.3 ms (0.8-1.7 GB/s) | 19 ms (0.9 GB/s) |
| vkCreateComputePipelines (MoltenVK compiles MSL; cached on later runs) | 467 ms first, 7-19 ms after | |
| empty vkQueueSubmit + vkWaitForFences | 0.03-1.3 ms (varies run to run) | |

## The transport: the pipe stream, the same as GL's

A Vulkan stream is the same kind of stream as GL's: on the host, every render thread runs the
Vulkan decoder first on what it reads and the GL/renderControl decoders on the rest
(`RenderThread::main` in host/render_thread.cpp, when the renderer was started with Vulkan). So the
Vulkan opcodes go down a `"pipe:opengles"` pipe exactly like src/gfx's, and **our kernel and SCGPU
carry them unchanged**. No address-space (ASG) ring is needed.

What selects the transport in the guest: `renderControlGetTransport()` (the driver's stub,
vulkan/stubs/GfxStreamRenderControl.cpp) returns a `GfxStreamTransportType`, and
`GfxStreamConnectionManager::initialize()` makes the stream for it. Upstream Mesa returns
`VIRTIO_GPU_ADDRESS_SPACE` (an ASG ring in a mapped blob: needs blob resources and host-visible
mapping, which our device has neither of). `VIRTIO_GPU_PIPE` exists too (`VirtioGpuPipeStream`, the
same protocol over DRM TRANSFER ioctls). The patch adds `GFXSTREAM_TRANSPORT_SCION` (5): a
`ScionStream` over our pipe, and the flags word skipped when the kernel already sent it (it does,
in `vgpu3d_open`).

Other facts that matter:
- The renderControl encoder is not used: the Vulkan capset says `noRenderControlEnc = 1`, and
  versioning comes from the capset instead of rcGetGLString.
- The context's capset (`context_init`) matters only to ASG render threads (`mCapsetId`). A pipe on
  the kernel's GLES-capset context decodes Vulkan just the same. The harness creates its contexts
  with capset 3 and that works as well.
- Every Vulkan command carries a sequence number from one process-wide counter (Mesa's
  `ResourceTracker::nextSeqno`). The host orders them per **virtio-gpu context** (its
  `ProcessResources`, keyed by the context id). A process with several streams on several contexts
  deadlocks the host's render threads. See stage 3, kernel.
- Mesa's driver opens one stream **per thread** (`GfxStreamConnectionManager` is thread-local),
  so each Vulkan-calling thread is one more `/dev/vgpu3d` open.

## Memory: the non-direct mode, and what it costs

Our transport cannot map host GPU memory into the guest: no blob resources, no host-visible
window. gfxstream has a mode for that, from before blobs existed. The patch turns it on when the
device says `kParamHostVisible = 0` (ours does):

- `ResourceTracker::usingDirectMapping()` returns false (upstream Mesa returns true always).
- vkAllocateMemory of a host-visible type: the host allocates as usual, and the guest keeps a
  page-aligned **copy** (malloc) that vkMapMemory hands out. vkMapMemory/vkUnmapMemory never reach
  the host. vkFreeMemory frees the copy.
- `vkFlushMappedMemoryRanges` sends the range's bytes after the packet; the host copies them into
  its own mapping (it maps every host-visible allocation itself) and calls the real flush.
  `vkInvalidateMappedMemoryRanges` gets the host's bytes back after the reply. That is gfxstream's
  generated code on both sides (cereal encoder.py / decoder.py, `!usingDirectMapping()`).
- **The host must agree.** Its `usingDirectMapping()` is `GlDirectMem || VirtioGpuNext`.
  stream_renderer enables VirtioGpuNext by default, so the renderer must be started with the
  feature string `VirtioGpuNext:disabled` (`STREAM_RENDERER_PARAM_RENDERER_FEATURES`, 11). If the
  two sides disagree, the first flush puts the stream out of step.

Fixed on the way: for `VK_WHOLE_SIZE` the host sends the rest of **its** allocation, which in this
mode it rounds up to a host page (16 KiB on Apple, 4 KiB on Android). The guest read only what it
had mapped and left the rest in the stream. The next replies then read those bytes, and the next
vkAllocateCommandBuffers got handle 0. The encoder now reads the size the host announces, keeps
what fits and skips the rest. It also clamps `VK_WHOLE_SIZE` with an offset to the rest of the
memory: upstream sent the whole allocation from `offset`, past the end.

What it costs:
- Every flush and every invalidate is a round trip and a copy of the range, both ways in the
  guest (to the stream buffer, then through the pipe). On the Mac that is 1-2 GB/s. On the phone
  it is also bounded by `/dev/vgpu3d`'s write/read and SCGPU's ring.
- Guest memory doubles for host-visible allocations (the copy here, the real one on the host).
- **No coherent memory.** With VirtioGpuNext off, the host strips `HOST_COHERENT` from every
  type it shows the guest (vk_emulated_physical_device_memory.cpp: "Coherent memory in the guest
  requires one of these features"). So the guest sees host-visible types that are not coherent,
  and an application must flush and invalidate. vk_test does. The Vulkan spec requires at least
  one host-visible + coherent type, so applications that insist on one will not find it. Our own
  Metal-on-Vulkan layer can flush explicitly.
- **0884d2d (the phone's host) also has `VulkanEnsureCachedCoherentMemoryAvailable`, enabled by
  default.** It aborts (`GFXSTREAM_FATAL`) when no coherent type is left. The phone's features must
  therefore also say `VulkanEnsureCachedCoherentMemoryAvailable:disabled`. The emulator's host is
  older, has no such feature and rejects the name, so the Mac harness must not pass it.

Coherent memory could be emulated in the guest later (not done):
- Mark host-visible types coherent again in the guest, and keep a second copy: what was last
  synchronised.
- Before every vkQueueSubmit[2], flush only the 4 KiB chunks where the copy differs from that
  snapshot (what the CPU wrote).
- After every successful wait (fences, queue/device idle, fence status), invalidate into a scratch
  buffer and merge: chunks the CPU has not touched take the host's bytes; chunks it has touched
  keep the CPU's.
- That is correct for the usual patterns (staging upload, readback, rings of persistently mapped
  memory). Its price is a memcmp of all mapped memory per submit, and a full transfer of all
  mapped memory per wait.

## Host flags, features and capset

- Renderer flags: **`STREAM_RENDERER_FLAGS_USE_VK_BIT` (1 << 5)** must be set. In this gfxstream,
  Vulkan is opt-in: `Vulkan = rendererFlags & USE_VK_BIT`. There is no NO_VK flag any more, so not
  passing a NO_VK flag does not enable Vulkan. The harness uses `USE_EGL | SURFACELESS | USE_GLES
  | USE_VK` = 0x39. GLES stays on (`GuestVulkanOnly` is only set when GLES is off).
- Features (param 11, a string `"Name:enabled|disabled,..."`): `VirtioGpuNext:disabled` (both
  hosts), plus `VulkanEnsureCachedCoherentMemoryAvailable:disabled` on 0884d2d. GlDirectMem is
  already off under stream_renderer.
- Capset `GFXSTREAM_VULKAN` (3), `struct vulkanCapset` (host/virtgpu_gfxstream_protocol.h), as
  `VirtioGpuFrontend::fillCaps` fills it:
  - protocolVersion 1, ringSize 12288, bufferSize 1 MiB (ASG only);
  - colorBufferMemoryIndex: the host's colour-buffer memory type;
  - deferredMapping 1 (when Vulkan is on), blobAlignment = the host's page size;
  - noRenderControlEnc 1;
  - vulkanBatchedDescriptorSetUpdate 1 (feature on by default);
  - alwaysBlob and externalSync 0 unless built with the unstable options;
  - virglSupportedFormats: a bitmask;
  - hasTraceAsyncCommand and hasSetMetadataCommand 1 (0884d2d; Mesa 25.0.7's struct ends before
    these two).

  The guest (`ScionVirtGpuDevice`) reads it where it can (the Mac: `stream_renderer_fill_caps`).
  On iOS there is no ioctl for it yet, so it uses the same values with colorBufferMemoryIndex
  "ask the host". Either way it then overrides what a pipe cannot do: deferredMapping 0,
  alwaysBlob 0, externalSync 0, kParamResourceBlob/HostVisible/CreateGuestHandle/FencePassing 0.

Mac harness only: the emulator's gfxstream finds the Vulkan loader and MoltenVK beside the
program. mk-vk.sh sets `ANDROID_EMU_VK_LOADER_PATH`, `VK_DRIVER_FILES`/`VK_ICD_FILENAMES` and
`ANDROID_EMU_VK_ICD=moltenvk`; without the last, the host's instance does not enumerate
portability drivers and finds none.

## What is stubbed or absent

- `VirtGpuDevice`: createBlob/createResource/importBlob return null, execBuffer -ENOTSUP. That
  means no external memory (AHB, dmabuf, colour-buffer import/export) and no Vulkan images bound
  to gfxstream colour buffers yet.
- Sync: no sync fds (`ScionSyncHelper` fails every call). Exportable fences and semaphores are
  out; ordinary fences and semaphores work (vk_test waits on fences).
- WSI: Mesa's WSI with no platforms, so no VkSurfaceKHR and no swapchain. Presenting will go
  through colour buffers/IOSurfaces like GL, later.
- Images, render passes and graphics pipelines were not exercised: vk_test is buffers and
  compute only.
- The library resolves about 2,400 weak entry points of Mesa's dispatch tables at load, through
  flat-namespace lookups (`-undefined dynamic_lookup`; ld warns it is deprecated on iOS). On ELF
  they resolve to NULL at link. They cost a little at load, and mk-vk.sh checks that nothing
  non-weak is left undefined.

## What is built for iOS

`mk-vk.sh ios`:
- `out-ios/libhakios.vk.dylib`: 3.2 MB stripped, arm64, minos 16.0, needs only libSystem and
  libc++, exports 234 `vk*` functions plus the three `vk_icd*`, install name
  `/redwood/lib/libhakios.vk.dylib`.
- `out-ios/vk_test`, linked against it.

Both are compile- and link-checked against the iPhoneOS 16.5 SDK. Neither has run: the Pixel is
not connected. userland/build.sh does not package them yet: copy them to `/redwood/lib` and
`/redwood/bin`.

## Stage 3: what the phone path needs

**Scion app** (`app/src/main/cpp/scgpu.c`):
1. Renderer flags `SR_FLAGS_USE_EGL | SR_FLAGS_SURFACELESS | SR_FLAGS_USE_GLES` **| (1 << 5)**
   (USE_VK).
2. Add the parameter `{ 11 /* RENDERER_FEATURES */, (uint64_t)(uintptr_t)"VirtioGpuNext:disabled,VulkanEnsureCachedCoherentMemoryAvailable:disabled" }`.
   The host library is 0884d2d, which knows both names; an unknown name fails stream_renderer_init.
3. Capsets: `S.h->num_capsets = 2`, and `CMD_GET_CAPSET_INFO` index 1 answers
   `CAPSET_GFXSTREAM_VULKAN` (3) with `get_cap_set(3, ...)`. `CMD_GET_CAPSET` is already generic,
   and `fill_caps(3)` needs the renderer up with Vulkan, which (1) gives. The GLES capset must
   stay index 0: the kernel looks for it by id, so the order is free, but keep it.
4. Check that the Android host library (mk-host-android.sh) loads `libvulkan.so` and picks the
   PowerVR. Its Vulkan server is already compiled in. With Vulkan on, the renderer also brings
   up its VkEmulation, so check that the GL path is unchanged: SpringBoard fps (eagl
   `glframes=1`) before and after. On Android, `VirtioGpuNext:disabled` only drops an extension
   name from rcGetGLString, which src/gfx never asks for.

**Kernel** (`drivers/gpu/hakios.vgpu3d*.c`):
1. Nothing is needed for a single-threaded Vulkan client. A Vulkan stream is a pipe like GL's:
   same CTX_CREATE (context_init GLES or 0), same PIPE_BUFFER resource with VIRGL_BIND_CUSTOM,
   same `"pipe:opengles"` and flags word, same write/read.
2. **Multi-threaded clients:** all opens of one process must share one virtio-gpu context: one
   context id per process, a pipe resource per open, CTX_DESTROY when the process's last open
   closes. Linux works this way: one DRM fd, one context, many pipes. Otherwise the host orders
   one process's commands per context and deadlocks (see the transport section). The
   alternative is in the library: one stream per process. But Mesa's encoder takes no lock when
   queue-submit-with-commands is on, so that needs a lock around every encoder.
3. Optional, `VGPU3D_IOC_CAPSET` (id, buffer, size): the capset bytes, which vgpu3d_query can
   fetch for id 3 as it does for 8. scion_vk_transport_capset would then use the host's own
   colorBufferMemoryIndex and descriptor flag instead of the defaults.

**Userland:** package `libhakios.vk.dylib` and `vk_test` in userland/build.sh (as GL's
mk-gfx.sh outputs are). The Vulkan headers come from the same Mesa tree (`include/vulkan`).

## Unverified

- Anything on the phone: the iOS library has never been loaded or run. That includes dyld on
  iOS 16 resolving the flat-namespace weak imports to NULL, `/dev/vgpu3d` and SCGPU carrying a
  Vulkan stream, flush/invalidate throughput there, and the PowerVR's memory types and limits
  through gfxstream.
- The phone's host at 0884d2d decoding the Mesa 25.0.7 guest. Only the emulator's host was used.
- The features string and USE_VK on the Android host library, and their effect on the GL path.
- Several Vulkan threads (see stage 3, kernel 2). Images, graphics, render passes, descriptor
  sets of more than one storage buffer, query pools.
