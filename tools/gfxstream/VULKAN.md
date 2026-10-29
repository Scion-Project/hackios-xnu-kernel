# Vulkan in the iOS guest through gfxstream

Goal: Vulkan in iOS under AVF, drawn by the Pixel 10's PowerVR, the way GL ES already is: the
guest encodes calls in gfxstream's wire format, `/dev/vgpu3d` and SCGPU carry the bytes, and the
Scion app's gfxstream decodes them on the phone's GPU. It is the base for metal2vulkan
(Metal on Vulkan).

- **Stage 1 (done, on the Mac):** gfxstream's own guest Vulkan driver, built for macOS, talks to
  the Android emulator's `libgfxstream_backend.dylib` (a real gfxstream host, MoltenVK under it)
  in one process. `userland/src/vk_test.c`, nothing but the Vulkan API, passes every check,
  threads included.
- **Stage 2 (built, not run):** the same library for arm64-apple-ios16 as `libhakios.vk.dylib`
  (install name `/redwood/lib/libhakios.vk.dylib`) with the stream over `/dev/vgpu3d`, and
  `vk_test` for iOS (`userland/build.sh VK_DIR=`).
- **Stage 3:** the app's side is in scion #144 (USE_VK, the features string, capset 3). What is
  left is listed at the end.

Build and run: `tools/gfxstream/mk-vk.sh [mac|ios|all]`; `tools/gfxstream/mk-vk.sh mac run [words
[threads]]` runs vk_test against the emulator. Output goes to `$VK_BUILD` (default
`xnu-deps/build/gfxstream-vk`); a clean build of both is about two minutes and 400 MB.

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
decodes it without complaint. Whether 0884d2d does is unverified until it runs on the phone.

What is ours (`tools/gfxstream/vk/scion/`, built into Mesa's tree as `platform/scion`):

| file | what |
|---|---|
| `scion_vk_transport.{h,c}` | the byte pipe. iOS: an open of `/dev/vgpu3d`. macOS: a stream_renderer context + 1 MiB PIPE_BUFFER resource + `"pipe:opengles"` in this process (as `host_test/host.c` does for GL), every host call one at a time as the phone makes them |
| `ScionStream.{h,cpp}` | gfxstream's `IOStream` over that pipe: writes gathered, sent when full or when a reply is wanted |
| `ScionVirtGpu.cpp` | the `VirtGpuDevice` the driver asks for: caps only (no blobs, no execbuffer, no sync fds) |
| `mkvkexports.py` | the 234 core Vulkan commands (1.0 to 1.4) as exported `vk*` functions: iOS has no loader |

What the patch changes in Mesa (1,250 lines; its header lists them): the transport selection, the
non-direct memory path, a stream desync in the encoder's invalidate, threads (sequence numbers per
stream, a stream per queue, the instance on a process stream), polled waits, opt-in coherence, the
exports and link flags, two darwin build fixes.

Run-time switches (environment of the process using Vulkan):

| variable | what |
|---|---|
| `HAKIOS_VK_COHERENCE=emulate` | host-visible memory shown coherent and kept so by the driver (below); default: shown as the host has it, not coherent |
| `HAKIOS_VK_HOST_WAITS=1` | waits made on the host, blocking, instead of polled (deadlocks on the phone; for comparison) |
| `SCION_VK_BATCHED_DESCRIPTORS=0/1` | the capset's batched descriptor updates, over the host's (or the default, 1, on iOS) |
| Mac harness: `GFXSTREAM_LIB`, `SCION_VK_FLAGS`, `SCION_VK_FEATURES`, `SCION_VK_PARALLEL=1` | the emulator's directory, renderer flags (0x39), features (`VirtioGpuNext:disabled`), and transfers of several streams at once instead of one at a time |

## Result on the Mac (`mk-vk.sh mac run`)

```
VKTEST instance: Vulkan 1.3.0 offered, 199.6 ms (the stream to the host included)
VKTEST PASS physical device
VKTEST physical device: "Apple M1 Pro" (vendor 0x106b device 0x1b000207, Vulkan 1.2.306, driver 0x000027e3, type 1)
VKTEST memory type 0: heap 0 device-local
VKTEST memory type 1: heap 0 device-local host-visible host-cached
VKTEST memory type 2: heap 0 device-local
VKTEST PASS a compute queue family
VKTEST device and queue (family 0 of 1): 1.3 ms
VKTEST buffers of 262144 words: device in memory type 0, host-visible in type 1 (not coherent)
VKTEST PASS vkCmdFillBuffer + vkCmdCopyBuffer, read back: 262144 words of 0xc0ffee11 (submit and wait 13.69 ms; invalidate 0.89 ms: 1181 MB/s)
VKTEST PASS flushed pattern through device memory and back: 262144 words (flush 0.56 ms: 1886 MB/s; submit and wait 1.11 ms)
VKTEST PASS compute shader: 262144 words of i * 3 + 7 (pipeline 15.8 ms; dispatch, copy and wait 1.02 ms)
VKTEST SKIP coherent memory: no host-visible type is coherent
VKTEST an empty submit and wait: 0.059 ms
VKTEST PASS a waiter on work not yet submitted, while this thread makes things: the fence came, the timeline value came; 56 ms waiting (50 of them before the submit)
VKTEST PASS threads: 4 threads x 8 rounds of pipeline cache, query pool, compute pipeline, dispatch, submit and check, with this thread submitting too: 26 ms
VKTEST OK: 0 failed, 327 ms in all
```

`vk_test 4096 16` (16 threads) passed 10 runs in 10, as did 5 runs in 5 with
`HAKIOS_VK_COHERENCE=emulate`. The device name is MoltenVK's for this Mac's GPU, seen through
gfxstream. The compute shader is SPIR-V embedded as words (its GLSL is in a comment in vk_test.c).
Measured (M1 Pro, in-process, so the transport is memcpys and function calls; the phone adds the
kernel, SCGPU's ring and AVF):

| what | 1 MiB | 16 MiB | 64 MiB |
|---|---|---|---|
| vkCreateInstance, first in the process (brings the whole host up: ANGLE, MoltenVK) | 160-520 ms | | |
| vkCreateDevice | 1-4 ms | | |
| vkFlushMappedMemoryRanges, whole buffer | 0.3-0.6 ms (1.9-3.5 GB/s) | 13 ms (1.3 GB/s) | 20-33 ms (2-3.5 GB/s) |
| vkInvalidateMappedMemoryRanges, whole buffer | 0.4-0.9 ms (1.2-2.7 GB/s) | 19 ms (0.9 GB/s) | 183 ms (366 MB/s) |
| vkCreateComputePipelines (MoltenVK compiles MSL; cached on later runs) | 467 ms first, 5-19 ms after | | |
| empty vkQueueSubmit + polled vkWaitForFences | 0.04-0.1 ms | | |

Large invalidates are slow on the host's side: `VirtioGpuRenderThreadPipe::TransferFromHost` copies
what is left of a reply into a new buffer after every read of a pipe's worth, so one reply of
64 MiB read 1 MiB at a time is ~2 GB of memcpy. Small, exact invalidates are fine; whole blocks are
not.

## The transport: the pipe stream, the same as GL's

A Vulkan stream is the same kind of stream as GL's: on the host, every render thread runs the
Vulkan decoder first on what it reads and the GL/renderControl decoders on the rest
(`RenderThread::main`, when the renderer was started with Vulkan). So the Vulkan opcodes go down a
`"pipe:opengles"` pipe exactly like src/gfx's, and the kernel and SCGPU carry them as they are. No
address-space (ASG) ring is needed.

What selects the transport in the guest: `renderControlGetTransport()` (the driver's stub,
vulkan/stubs/GfxStreamRenderControl.cpp) returns a `GfxStreamTransportType`, and
`GfxStreamConnectionManager::initialize()` makes the stream for it. Upstream Mesa returns
`VIRTIO_GPU_ADDRESS_SPACE` (an ASG ring in a mapped blob: needs blob resources and host-visible
mapping, which our device has neither of). The patch adds `GFXSTREAM_TRANSPORT_SCION` (5): a
`ScionStream` over our pipe, and the flags word skipped when the kernel already sent it (it does,
in `vgpu3d_open`). The renderControl encoder is not used (`noRenderControlEnc = 1`); the context's
capset matters only to ASG render threads.

## Threads

A runtime calls Vulkan from several threads at once: metal2vulkan has a waiter per queue in
`vkWaitSemaphores`/`vkWaitForFences`, a completion thread and pipeline workers besides the main
thread. Mesa's driver gives every thread a stream of its own (`GfxStreamConnectionManager` is
thread-local): one `/dev/vgpu3d` open, one virtio-gpu context, one render thread on the host. As
upstream left it, two threads deadlocked at once. What was found, and what the patch does:

1. **gfxstream's host has one pipe per virtio-gpu context.** `VirtioGpuContext::Create` makes one
   `VirtioGpuPipe` and every resource attached to the context shares it
   (`VirtioGpuContext::AttachResource`). So the streams of a process can **not** share one context
   with a pipe resource each, as a Linux DRM fd shares one: their bytes would be mixed into one
   render thread (tried: it hangs at once). A Linux guest gets several render threads per context
   only through ASG rings, which need mapped blobs. Every stream stays a context of its own, which
   is what `/dev/vgpu3d` already does (a context an open).
2. **Sequence numbers per stream.** With queue-submit-with-commands (on in every current host), every
   Vulkan command carries a sequence number, and the host's render thread waits until its context's
   counter (`ProcessResources`, per context id) reaches the one before. Upstream numbers commands
   from one counter per process, so a stream's host thread waited for numbers that went to other
   contexts: the deadlock (the sample metal2vulkan took: every thread in `ScionStream::readFully`,
   the render threads in `VkDecoder::decode`). The patch counts per encoder (`VkEncoder::nextSeqno`),
   and an encoder is one stream: each context sees 1, 2, 3, ...
3. **A stream per queue.** With sequence numbers per stream, the host no longer orders one thread's
   commands against another's. What has to stay in order across threads is a queue's work, so
   `getQueueEncoder` gives each VkQueue a stream of its own, used by whichever thread calls
   `vkQueue*` (the spec wants the queue externally synchronized, so it is never used by two at
   once). Submits, their command buffers (sent at submit time) and their descriptor updates (batched,
   committed at submit) go down it in the order the threads made them. Objects made on other threads
   are made before they can be used: their creation waits for the host's reply.
4. **The instance on a process stream.** The host ties a VkInstance to the context it was made on
   and destroys it with that context (`registerProcessCleanupCallback`), and a thread's stream (and
   context) goes when the thread ends. `vkCreateInstance`/`vkDestroyInstance` use one more stream
   that lives as long as the process.
5. **Waits are polled.** On the phone every reply comes through SCGPU's mailbox, and the app has
   one thread (scgpu.c, `serve`) for the mailbox *and* the stream ring: a read there waits for the
   render thread's reply before anything else is done, writes of other threads included. A wait
   made on the host (`vkWaitForFences` with a timeout, `vkWaitSemaphores`, `vkQueueWaitIdle`,
   `vkDeviceWaitIdle`, `vkGetQueryPoolResults` with WAIT) on work another thread has yet to submit
   would block that thread's submit for ever. So the driver polls: a host call with timeout 0, then
   a nap here (20 us, doubling to 2 ms), until done or the caller's timeout. Queue and device idle
   are an empty submit with a fence of the driver's, polled. vk_test's waiter proves it: it waits on
   a fence and a timeline value 50 ms before the main thread makes a buffer (calls that want replies)
   and submits. Polled, it passes; with `HAKIOS_VK_HOST_WAITS=1` both waits time out after their 30 s.

The Mac harness makes every host call one at a time, as the phone's serve thread does, so what
deadlocks there deadlocks here (`SCION_VK_PARALLEL=1` lifts that for transfers). That also matters
for the emulator's frontend itself, which is not safe for contexts made from several threads at once
(before the harness serialized its calls, 16-thread runs crashed now and then: "failed to attach
resource ... context not found").

What it costs: a stream (an open, a context, a render thread, 256 KiB of the SCGPU pool) per thread
that has called Vulkan, plus one per queue and one for the process. The kernel now has room for 64
opens (`MAX_OPENS`; it was 32) and, with SCGPU's ring, gives each a 256 KiB pipe buffer instead of
1 MiB (writes go through the ring in records of at most that; the buffer carries only replies), so 64
opens take 16 MiB of the app's 64 MiB region.

## Memory: the non-direct mode, and coherence

Our transport cannot map host GPU memory into the guest: no blob resources, no host-visible
window. gfxstream has a mode for that, from before blobs existed. The patch turns it on when the
device says `kParamHostVisible = 0` (ours does):

- `ResourceTracker::usingDirectMapping()` returns false (upstream Mesa returns true always).
- vkAllocateMemory of a host-visible type: the host allocates as usual, and the guest keeps a
  page-aligned **copy** that vkMapMemory hands out. vkMapMemory/vkUnmapMemory never reach the host.
- `vkFlushMappedMemoryRanges` sends the range's bytes after the packet; the host copies them into
  its own mapping and calls the real flush. `vkInvalidateMappedMemoryRanges` gets the host's bytes
  back after the reply. That is gfxstream's generated code on both sides.
- **The host must agree.** Its `usingDirectMapping()` is `GlDirectMem || VirtioGpuNext`, so the
  renderer runs with `VirtioGpuNext:disabled`. Then it strips `HOST_COHERENT` from every type it
  shows the guest, and 0884d2d also needs `VulkanEnsureCachedCoherentMemoryAvailable:disabled` (it
  aborts when no coherent type is left; the emulator's older host does not know the name).
- Fixed on the way: for `VK_WHOLE_SIZE` the host sends the rest of **its** allocation, rounded up to
  a host page, and the guest read only what it had mapped; the stream went out of step. The encoder
  reads what the host announces and keeps what fits.

**By default the driver shows the host's host-visible types as they are: not coherent.** The
application flushes and invalidates what it touches. metal2vulkan does exactly that when no
host-visible type is coherent (its m2v_test flushed 1.0 MB and invalidated 55 KB in all, against
2.15 GB with blanket emulation). The spec's minimum of one coherent host-visible type is not met.

**`HAKIOS_VK_COHERENCE=emulate`** is for a client that does not flush or invalidate: every
host-visible type is shown coherent, and the driver keeps it so:
- an emulated-coherent allocation has a second copy: its bytes as the host last had them;
- before every vkQueueSubmit[2], the 4 KiB chunks of mapped allocations where the guest's copy
  differs from it (what the CPU wrote) are flushed, in runs: a memcmp of what is mapped;
- after a wait that saw work complete (a fence, a timeline value that moved, queue or device idle),
  and only if something was submitted since, the ranges the device may have written (buffers and
  images bound with a usage that lets it write) are invalidated into a merge: bytes the CPU has not
  touched since the last sync take the host's, the CPU's stay and go out with the next submit;
- vkMapMemory pulls what an unmapped allocation missed, vkUnmapMemory flushes it; one sync at a time
  (a pull applied after a later one would put older bytes back).

What it costs, measured with 64 MiB mapped (an empty submit and its wait, average of 10):

| 64 MiB mapped, bound... | written here per submit | submit | wait |
|---|---|---|---|
| read-only (TRANSFER_SRC, VERTEX) | 0 / 4 KiB / 1 MiB | 2.3 / 2.4 / 2.7 ms | 0.12-0.16 ms |
| writable (TRANSFER_DST, STORAGE) | 0 / 4 KiB / 1 MiB | 2.4 / 2.5 / 2.7 ms | 220 ms |

The submit side is cheap when little changed (the scan is ~2.3 ms per 64 MiB on the M1 Pro); the
wait side pulls every writable-bound byte after every completed wait, at the host's slow large-read
rate. It is correct and opt-in, not fast. With `METAL2VULKAN_HOST_SYNC=always` metal2vulkan issues
its exact ranges on coherent types too; those reach the host as ordinary flushes and invalidates
(and keep the emulation's copy right), so the default, honest mode is the one to use with it.

## Host flags, features and capset

- Renderer flags: **`STREAM_RENDERER_FLAGS_USE_VK_BIT` (1 << 5)**: Vulkan is opt-in in this
  gfxstream. The harness uses `USE_EGL | SURFACELESS | USE_GLES | USE_VK` = 0x39.
- Features (param 11, `"Name:enabled|disabled,..."`): `VirtioGpuNext:disabled` (both hosts), plus
  `VulkanEnsureCachedCoherentMemoryAvailable:disabled` on 0884d2d. `VulkanQueueSubmitWithCommands`
  stays on (the guest numbers its commands per stream).
- Capset `GFXSTREAM_VULKAN` (3), as `VirtioGpuFrontend::fillCaps` fills it: protocolVersion 1;
  colorBufferMemoryIndex; deferredMapping 1; noRenderControlEnc 1; vulkanBatchedDescriptorSetUpdate
  1; blobAlignment the host's page. The guest reads it where it can (the Mac:
  `stream_renderer_fill_caps`); on iOS there is no ioctl for it yet, so it uses the same values. It
  then overrides what a pipe cannot do: deferredMapping 0, alwaysBlob 0, externalSync 0,
  kParamResourceBlob/HostVisible/CreateGuestHandle/FencePassing 0.

Mac harness only: the emulator's gfxstream finds the Vulkan loader and MoltenVK beside the
program. mk-vk.sh sets `ANDROID_EMU_VK_LOADER_PATH`, `VK_DRIVER_FILES`/`VK_ICD_FILENAMES` and
`ANDROID_EMU_VK_ICD=moltenvk`; without the last, the host's instance does not enumerate
portability drivers and finds none.

## What is stubbed or absent

- `VirtGpuDevice`: createBlob/createResource/importBlob return null, execBuffer -ENOTSUP: no
  external memory (AHB, dmabuf, colour-buffer import/export) and no images bound to gfxstream
  colour buffers yet.
- Sync: no sync fds. Exportable fences and semaphores are out; ordinary fences and binary and
  timeline semaphores work.
- WSI: no VkSurfaceKHR and no swapchain. Presenting will go through colour buffers/IOSurfaces.
- Images, render passes and graphics pipelines were not exercised: vk_test is buffers, compute,
  fences, timeline semaphores and threads.
- The library resolves about 2,400 weak entry points of Mesa's dispatch tables at load, through
  flat-namespace lookups (`-undefined dynamic_lookup`; ld warns it is deprecated on iOS). mk-vk.sh
  checks that nothing non-weak is left undefined.
- With emulated coherence, images bound with vkBindImageMemory2 are not tracked as writable.

## What is built for iOS

`mk-vk.sh ios`: `out-ios/libhakios.vk.dylib` (3.3 MB stripped, arm64, minos 16.0, needs only
libSystem and libc++, 234 `vk*` exports plus the three `vk_icd*`) and `out-ios/vk_test`, both
compile- and link-checked against the iPhoneOS 16.5 SDK. `userland/build.sh VK_DIR=out-ios` puts
them in the image; rc.jobs runs vk_test once.

## What the phone path still needs

**Kernel** (this change): `MAX_OPENS` 64 and a 256 KiB pipe buffer with SCGPU's ring (above). A
context per open stays: that is what gfxstream's host needs. Optional later: `VGPU3D_IOC_CAPSET`
(the capset bytes, which vgpu3d_query can fetch for id 3 as it does for 8), so the library uses the
host's colorBufferMemoryIndex and descriptor flag instead of the defaults.

**Scion app** (scion #144 has USE_VK, the features string and capset 3): the serve thread answers a
read only once the render thread has replied, and runs nothing else meanwhile, neither the ring nor
other processes' mailbox commands. Polled waits keep that from deadlocking, but a long host call
(a pipeline compile: hundreds of ms on a first run) stalls every guest process's GPU work, GL
compositing included, for as long. A read that answers "not yet" and is retried, or a thread per
reading context, would lift it.

**Userland**: nothing more for vk_test; metal2vulkan links `libhakios.vk.dylib`.

## Unverified

- Anything on the phone: the iOS library has never been loaded or run. That includes dyld on
  iOS 16 resolving the flat-namespace weak imports to NULL, `/dev/vgpu3d` and SCGPU carrying the
  streams of several threads, the kernel's 64 opens and 256 KiB pipes, flush/invalidate throughput
  there, and the PowerVR's memory types and limits through gfxstream.
- The phone's host at 0884d2d decoding the Mesa 25.0.7 guest; only the emulator's host was used.
- The kernel change is built (`make-kernel.sh`'s judge: only the kext symbol-set checks failed) but
  not booted.
- Images, graphics, render passes, query results, several queues.
