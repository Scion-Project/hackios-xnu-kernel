# Metal on the Adreno: the plan, and where it stands

Read `docs/ru/metal.md` (why Metal is not simply missing, and what is on each side of the wall) and `docs/air.md` (the
shader compiler) first. This file is the order of work, with how each step is checked and where.

## The three parts

| Part | What it is | State |
|---|---|---|
| **A. The shader compiler** | Apple's shader bitcode (AIR, in a `.metallib`) into Mesa's NIR, so that ir3 (the compiler behind the OpenGL ES that already runs) makes Adreno code | largely done: 321 of the system's 456 shader modules reach ir3, 54 have run on the phone's GPU (`docs/air.md`, `docs/air-report.md`) |
| **B. The device** | what Metal loads: a bundle with a subclass of Metal's private `_MTLDevice`, and the objects it hands out (buffers, libraries, pipeline states, queues, command buffers, encoders: 53 classes and about 1200 methods in Apple's own) | step 1 ran in QEMU; buffers and libraries (step two, below) are written and checked on a Mac |
| **C. The kernel side** | submitting work to the GPU for a Metal client, with memory and fences | the GPU already has a user client for Mesa (`drivers/gpu/hakios.gpu.cpp`, `HakiosGPUUserClient`); it is not connected to B |

What "real Metal" would be, and why games are out of reach, is argued in `docs/ru/metal.md` section 9.4: that has not
changed. What this plan aims at is the part that is within reach: **Metal clients of the system itself** (the window
server's compositor, CoreImage, the camera's viewfinder) running on the Adreno through B and C, with shaders from A.

**A second route, for a guest with no GPU of its own: Metal on Vulkan** (the last section, "Metal on Vulkan"). The Pixel's
guest under AVF reaches its GPU only through gfxstream, which speaks OpenGL ES and Vulkan, not the Adreno's kernel
interface, so A and C do not apply there. metal2vulkan's runtime gives the whole Metal API on any Vulkan 1.2 driver; with
AdrenoMetal's frontend it is a second build of part B (`METAL=vk`), on gfxstream's guest Vulkan driver on the Pixel and on
Mesa's panvk on the Galaxy S9 later. It runs on three Vulkan drivers of a Mac; on the guest it waits for that driver.

## The order, and how each step is checked

| # | Step | Checked by | Where |
|---|---|---|---|
| 1 | **A device that exists.** `metal=1` makes the (so far empty) `IOAcceleratorES` service name `HakiosMetal`; the bundle makes `HakiosMetalDevice`, a `_MTLDevice` subclass; `MTLCreateSystemDefaultDevice()` returns it | `metal_test` (`RC_JOBS=rc.jobs.metal`) prints the device and `HakiosMetal:` lines say which messages Metal sent that the device did not answer | QEMU, no GPU needed |
| 2 | **Learn what Metal asks first**, from step 1's "not implemented" lines: properties, family support, queue, buffer | the log, in order | QEMU |
| 3 | **Buffers and textures** over the GPU memory the kernel hands out (`hakios.gpu.cpp`: `kMsmGemNew`), `IOSurface`-backed | a program writes a buffer and reads it back | phone |
| 4 | **A library and a compute pipeline**: `newLibraryWithData:` takes a `.metallib`, the function goes through A, the kernel runs | the CPU reference in `tools/air` and then the phone | phone |
| 5 | **Command queue, command buffer, compute encoder, commit and wait**, on fences | a compute kernel dispatched through the Metal API gives the numbers `tools/air/cpuref.py` expects | phone |
| 6 | **Render pipeline and render command encoder** for the three vertex and 271 fragment shaders A already handles | a triangle through Metal | phone |
| 7 | **Present**: a drawable, and the frame reaching the screen without being dropped | the frame counters (below) | phone |

Without a phone, steps 1 and 2 are all that can be done and checked. Steps 3 to 7 each need the GPU.

## Step 1: what is in the tree

- `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`: with boot-arg `metal=1` the `IOAcceleratorES` service gets the
  properties `MetalPluginName` = `HakiosMetal` and `MetalPluginClassName` = `HakiosMetalDevice`. Without the argument
  nothing changes: **the properties stay off by default, on purpose**. With them a program that asks for Metal gets a
  device, and CoreAnimation would pick Metal for the screen; until that device can draw, the screen without it is the
  working one.
- `userland/src/HakiosMetal.m`, `userland/src/HakiosMetal.plist`: the bundle, built into `/redwood/lib/HakiosMetal.bundle` and
  linked at `/System/Library/Extensions/HakiosMetal.bundle` (`userland/build.sh`).
- `userland/src/metal_test.m` and `userland/rc.jobs.metal`: the check that already existed.

## What steps 1 and 2 found (QEMU, 27 October 2026, `metal=1`, a clean image)

Run in a loop of rebuild-image, boot, read the log; each pass gave the next requirement:

1. **Metal found the service, loaded the bundle and made the class**: `HakiosMetal: the class HakiosMetalDevice is registered`
   in `backboardd` and `mediaserverd`, then `a device for the accelerator port N (made)`: `-initWithAcceleratorPort:` was
   called and `_MTLDevice`'s own `-init` returned an object. So the discovery chain of `docs/ru/metal.md` section 1 is as described.
2. `MTLAddDevice` then **asserted that the class conforms to the protocol `MTLDevice`**, and, once it did, **to the private
   protocol `MTLDeviceSPI`** (neither is declared by `_MTLDevice` itself, so the class declares them).
3. With both declared, the first messages sent to the device, in this order: **`-featureProfile`**, **`-maxBufferLength`**,
   and then `backboardd` tries to **create a command queue** (`[Metal] Failed to create MTLCommandQueue`) and exits. The
   window server restarts and fails the same way (launchd: "service has crashed N times in a row"). That is what a Metal
   device that exists and cannot do anything does to the system, and the reason `metal=1` is off by default.

4. **A command queue** is now made (`HakiosMetalQueue`, a subclass of `_MTLCommandQueue`; it queues nothing). Metal then
   sends it `-setCompletionQueue:`, which is not in `MTLCommandQueue` but in the private protocol `MTLCommandQueueSPI`: found
   by taking every protocol of the process whose name begins with `MTLCommandQueue`. The window server then asks for
   `-setGPUPriority:` and `-newDepthStencilStateWithDescriptor:`.
5. **The objects a device hands out** are stub classes made from a table in `HakiosMetal.m` (depth-stencil state, sampler
   state, texture, render and compute pipeline states, event, fence): a subclass of Metal's private base where the system has
   one, the protocol family declared, every other message logged once and answered with zero. Textures and events have no
   private base class on this system (NSObject is used).
6. **The next wall**, with those in place: CoreAnimation's Metal renderer stops with
   `Assertion failed: (metallib_url != nil), function MetalContext, file ogl-metal.mm, line 3308.` It needs the URL of a
   `.metallib` (the system has `AppleMetalGLRenderer.bundle/default.metallib`, 274 shader modules, which the AIR work translates)
   and will then ask the device for a library from it, which is where the shader compiler (part A) is first needed for real.
   Where CoreAnimation takes that URL from is not known yet; the log shows the device was asked for nothing before it.

7. **Who is shown the device**, then, is a boot-arg level (the plug-in reads `sysctl kern.bootargs`): `metal=1` only the program
   `metal_test`, `metal=2` every process but the window server, `metal=3` every process. With the window server shown the device
   it stops on the missing metallib (QuartzCore.framework has a `default.pipelinelib` that names a `default.metallib` which is
   not in this image), so it cannot run on Metal here, and it has to be left on its OpenGL ES path. With `mediaserverd` shown it, it
   ends and starts again. At `metal=1` the window server, SpringBoard and mediaserverd were each logged as not shown the device
   and none of them ended in a QEMU run (the result of `metal_test` itself is not in this file yet).

So the road from here is the library (step 4 of the order below: `newLibraryWithURL:` / `…Data:` through the AIR compiler), then
buffers and textures with real memory, and the command encoders; none of that can be tried without the GPU.

## Step two: buffers, libraries, functions (27 October 2026)

What the plug-in now makes, in `userland/src/HakiosMetal.m`:

- **Buffers**: `-newBufferWithLength:options:`, `-newBufferWithBytes:length:options:`, `-newBufferWithBytesNoCopy:length:options:deallocator:`
  give a `HakiosMetalBuffer` (a subclass of `_MTLResource`, declaring `MTLBuffer`, `MTLResource` and their SPI protocols) over
  memory of the process: zeroed pages, the caller's own for `...NoCopy` (its deallocator called when the buffer goes). `-contents`,
  `-length`, the storage, cache and hazard modes of its options, `-allocatedSize`, a label. `-gpuAddress` is 0: the GPU's own
  buffers (`hakios.gpu.cpp`, `kMsmGemNew`, mapped into the GPU's address space) come with the GPU, and since the Adreno shares
  the system's memory, a shared-storage buffer's bytes can stay where they are when they do.
- **Libraries**: `-newLibraryWithURL:error:`, `...WithFile:`, `...WithData:`, `-newDefaultLibrary` and `...WithBundle:` read the
  `.metallib` with `userland/src/hakios_metallib.c` (the reading of `tools/metallib.py` in C, every offset checked; the file is
  the program's to give), and give a `HakiosMetalLibrary` (on `_MTLLibrary`) with `-functionNames`. A file that is not a metallib
  is an `MTLLibraryErrorDomain` error that says why.
- **Functions**: `-newFunctionWithName:` gives a `HakiosMetalFunction` (on `_MTLFunction`) with its name, its `MTLFunctionType`
  from the container's TYPE tag, and `-hakiosBitcode`, its AIR module unwrapped: what the shader compiler (part A) takes.
  Function constants are accepted and said not to be applied.

How it was checked, and what that does not show:

- `tools/metallib-test.sh DIR`: the C reading and `tools/metallib.py` agree on every function of every `.metallib` of the iOS 16.5
  system (the five MTLB files; the five runtime libraries of `AGXMetalA11.bundle` are another format, and both refuse them) and of
  macOS's; a file made by the test and then cut short at every length, and with each offset and length bent outside it, is refused
  every time, under the address and undefined-behaviour sanitizers. One correction came out of it: the header's second word at
  0x20 is the function list's length after its count, not its end (`tools/metallib.py` calls it `funcs_end` and does not use it).
- `tools/metal-plugin-test.sh DIR`: the plug-in built for macOS, its device made the way Metal makes it, on the Mac's own
  `_MTLDevice`, `_MTLResource`, `_MTLLibrary`, `_MTLFunction`: buffers written, read back, copied, refused over `maxBufferLength`,
  freed (and the caller's deallocator called); the five iOS libraries read (36, 135, 11, 274 and 0 functions; the last is
  SafariSharedUI's, a header and nothing else), every function made by its name with its type, every module LLVM bitcode. All
  under the address sanitizer. **macOS's Metal is not iOS 16.5's**: this shows the plug-in's own logic, not that iOS's Metal is
  content with these objects.
- Built for iOS 16.5 (`-Wall -Wextra`, no warnings). Not yet run on iOS: `metal_test` asks "what the device makes" in a process of
  its own (`RC_JOBS=rc.jobs.metal`, `metal=1`), and its line is the result that goes here.

Still nothing is drawn or computed: a pipeline made of these functions is the empty stand-in of step one. The next step is the
compiler behind `-newComputePipelineStateWithFunction:error:` (part A: the function's bitcode through NIR to ir3), and then the
command buffer and compute encoder on the GPU's buffers (part C), which need the phone.

## What UniXNU's AdrenoMetal showed, and what was taken

[UniXNU](https://github.com/UniXNU) runs iPadOS 17 on a Xiaomi Pad 6S Pro (SM8550, Adreno 740), and its
[AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) (MIT) composes the system UI on the GPU. Read on 27 October 2026; the facts
below are written here in our own words and code, with the source named where it is used. (Since 29 October the Metal-on-Vulkan
build of the plug-in, the last section, does carry code of theirs: the frontend and the bridge, copied under MIT with their notice.)

- **Their route is Metal on Vulkan**: the plug-in's device is a subclass of `_MTLDevice` (as ours), and every Metal object is
  handed by a bridge of theirs (`bridge/NativeMetalObjects.rs`) to metal2vulkan's runtime (steelbrain's, LGPL), which runs it on
  Turnip, Mesa's Vulkan driver for Adreno, over the kernel's GPU interface. Ours is AIR through NIR to ir3 directly (part A), on the same Mesa. Turnip supports the A6xx
  generation our Adreno 642L belongs to, so their route is open to this port as well; which of the two is less work from here is
  the question of the next step, and it is not decided here.
- **Universal metallibs.** Newer systems ship framework libraries as a fat container (0xCAFEBABE) of an AIR64 slice and a binary
  for Apple's GPUs (QuartzCore's `default.metallib` on iPadOS 17). `hakios_metallib_thin()` takes the AIR64 slice, as Metal does;
  `tools/metallib-test.sh` and `tools/metal-plugin-test.sh` check it. iOS 16.5's libraries are all thin.
- **CoreImage's limits.** `CIMetalDeviceGetLimits` reads `dedicatedMemorySize` and `sharedMemorySize`, which `_MTLDevice` does
  not answer; without them every `CIContext` throws (iconservicesagent ended over symbol glyphs there). The device now answers
  them, `isLowPower`, `isRemovable`, `isHeadless`, and `supportsFeatureSet:` (NO: the base reads Apple's tables).
- **Clients by a file.** A file beside the bundle names the processes shown the device. `HakiosMetal.clients` does the same here,
  on top of `metal=N`.
- **The window server needs QuartzCore's own `default.metallib`.** Their system image has it; iOS 16.5's (this port's) does not,
  which is the wall of step 7 above. It is not something the plug-in can supply: the library is Apple's.

## Frame skips

A dropped frame is not a Metal problem, and Metal would not cure it: the screen of this port is drawn today by
CoreAnimation's OpenGL ES renderer through Mesa (`gpu=1`), and its pace depends on the submit path, the fences and the
present. What to measure first, on the phone, is already counted: `gGPUStat` in `drivers/gpu/hakios.gpu.cpp` (submissions,
waits, what each cost, every five seconds). Whether the framebuffer stand-in (`MiniIOMobileFramebuffer.cpp`) counts
frames and drops is not known to me: it records when the first frame was shown and keeps a small copy of frames
(`record()`), and a counter of presented and dropped frames would be the first thing to add. A frame skip that is not seen
in those numbers is in the compositor, not the driver. This needs a stable GPU run on the phone first; the
`gpu=1` hang being bisected (see `docs/driver-review/README.md`) comes before any of it.

## What is not claimed

Nothing of the Adreno route draws. No Metal client has used this device for anything: the first one that tried (the window
server) asked for a command queue and stopped. Everything above was seen in QEMU; none of it was run on the phone. (The
Metal-on-Vulkan route below has drawn and computed on a Mac, and on nothing else yet.)

## Metal on Vulkan (29 October 2026): metal2vulkan, AdrenoMetal's frontend, Scion's Vulkan driver

Why. On the Pixel the guest has no GPU of its own: it reaches the host's through gfxstream (`tools/gfxstream/`, the scion
repository's `docs/GPU.md`), which speaks OpenGL ES and Vulkan. Parts A and C above are the Adreno's (ir3, KGSL-like submits),
and do not apply. [metal2vulkan](https://github.com/steelbrain/metal2vulkan) is the Metal API on any Vulkan 1.2 device, with its
own AIR-to-SPIR-V translator (no LLVM at run time), and AdrenoMetal (previous section) is a complete Metal plug-in over it. Put
together and pointed at a Vulkan driver of ours, that is a second build of part B: `userland/build.sh METAL=vk`.

### The pieces

| Piece | From | Licence | Where |
|---|---|---|---|
| the runtime, the translator, the AIR reader (`metal2vulkan-runtime`, `metal2vulkan`, `metal2vulkan-air` 0.2.0) | steelbrain/metal2vulkan at `720ef1ce9ae2`, with AdrenoMetal's 16 patches to it | GPL-2.0-or-later OR LGPL-3.0-or-later | fetched (a sparse checkout of the three crates) and patched into `xnu-deps/build/metal2vulkan/src` by `tools/metal/mk-metal2vulkan.sh`; never committed; built only into `libmetal2vulkan.dylib` |
| the C-ABI bridge over the runtime: `NativeMetalObjects.rs/.h`, `NativeMetalBridge.rs/.h` (`nmb_*`, 132 functions) | AdrenoMetal at `ce5e19e33e05` | MIT, NativeMetal contributors | `tools/metal/bridge/`, copied with the notice; our changes marked `Scion:` |
| `icd.rs` (the driver by path), `build.rs`, `Cargo.toml` | ours | MIT | `tools/metal/bridge/` |
| the Metal frontend: `NativeBridgeMetalFrontend.mm/.h` (a dynamic subclass of `_MTLDevice`, a class per Metal object) | AdrenoMetal | MIT | `userland/src/metalvk/`, copied with the notice; changes marked `Scion:` |
| the plug-in's entry: `HakiosMetalVK.mm` (the class Metal takes, the driver, the cache, the device) | ours | MIT | `userland/src/metalvk/` |
| the checks: `m2v_test.c` + `m2v_test.metal`, `metalvk_plugin_test.m`, the self-test's shaders | ours | MIT | `tools/metal/` |

**Why that revision.** AdrenoMetal's bridge and patches were made against metal2vulkan as it was around 14 October; metal2vulkan's
history was landed again on 26 October. Matching the patches' base blobs against every commit of the new history, `720ef1ce9ae2`
is the newest where 37 of the 38 files the patches touch are byte for byte their bases (the 38th, `texture.rs`, takes its hunks
cleanly); all 16 apply. At metal2vulkan's master of 29 October 13 of the 16 do not apply, and the bridge would have to follow the
runtime's API (it uses at least one patched-in API, `PaddedReadsPolicy::AliasWrites`). So the patches apply and the bridge needs
them; moving the pin forward is a port of the bridge (below, "what is needed").

**One dylib, replaceable.** The runtime is LGPL (or GPL): it is only ever in `/redwood/lib/libmetal2vulkan.dylib`, a library of
its own that the bundle links by install name, so a user can rebuild and replace it (`tools/metal/mk-metal2vulkan.sh ios`, from
the pinned source). No line of metal2vulkan is in any MIT or APSL file here; our changes to it are patch files in
`tools/metal/m2v/`, under its licence and saying so in their headers (one: host memory that is not coherent, below).

### How the plug-in reaches the Vulkan driver

1. The kernel's `metal=1` names `HakiosMetal`/`HakiosMetalDevice` (unchanged, step 1 above). The bundle's constructor registers
   `HakiosMetalDevice` on `_MTLDevice` if this process is shown the device (`metal=N`, `HakiosMetal.clients`: the same rule as
   the other build, now in `userland/src/hakios_metal_shown.c` for both).
2. Metal sends `-initWithAcceleratorPort:`. The plug-in picks the driver: `HAKIOS_VK_ICD` in the environment, else the first
   line of `HakiosMetal.icd` beside the bundle (`METAL_ICD=PATH` at build time writes it), else
   **`/redwood/lib/libhakios.vk.dylib`**, the gfxstream driver's install name. No file there: no device, Metal has no plug-in,
   nothing changes for anyone. It picks a writable shader cache (the user's cache directory, its temp directory, `/tmp`,
   `/private/var/tmp`, each `/m2v`), turns off Apple's IOSurface compression for the process (a layout of Apple's GPUs), and calls
   the frontend's `native_bridge_metal_open(driver, cache, registryID, missing)`.
3. The bridge's `nmb_device_open` hands the path to `icd.rs`, which `dlopen`s it, takes `vk_icdGetInstanceProcAddr` (else
   `vkGetInstanceProcAddr`), negotiates the loader-ICD interface (version 5) if the driver exports
   `vk_icdNegotiateLoaderICDInterfaceVersion`, and keeps the entry. There is no Vulkan loader on iOS, and the runtime opens a
   *loader* by path (ash's `Entry::load_from`, which wants `vkGetInstanceProcAddr`): so `libmetal2vulkan.dylib` exports a
   `vkGetInstanceProcAddr` of its own that forwards to the driver's entry, and the runtime is told to open the library itself.
   No layers, no dispatch table written into the driver's objects: every call goes straight to the driver, as a driver is used
   without a loader. One driver per process.
4. The runtime makes the device (Vulkan 1.2 and timeline semaphores are its floor), and `-initWithAcceleratorPort:` returns the
   frontend's `NativeBridgeMetalDevice` in place of the object Metal allocated (as AdrenoMetal's plug-in does). The log says
   `HakiosMetal: a device for the accelerator port N: <name>, on <driver> (<entry>, ICD interface 5), shaders cached in ...`; every
   selector the frontend lacks is `HakiosMetal: asked and not implemented: -[Class selector]`, once.

### What ran, on this Mac (an M1 Pro, macOS 27)

`tools/metal/mk-metal2vulkan.sh mac` builds the library for macOS, compiles `tools/metal/m2v_test.metal` with Xcode's Metal
compiler (`xcrun metal`, then `metallib`) and runs `m2v_test`, which goes through the same C ABI the plug-in uses. On lavapipe
(`/opt/homebrew/lib/libvulkan_lvp.dylib`, Mesa 26.2.5, a CPU Vulkan 1.4), the exact output (the runtime's own notices, lines
beginning `metal2vulkan:`, left out):

```
ok    self-test: compute 0 of 1000 wrong, render 0 of 64 wrong, import 0 of 4096 wrong
      device: llvmpipe (LLVM 23.1.3, 128 bits); driver /opt/homebrew/lib/libvulkan_lvp.dylib (vk_icdGetInstanceProcAddr, ICD interface 5); runtime metal2vulkan-720ef1ce9ae2-d91d85dac5dae0d1
      library: 5 functions from out/mac/m2v_test.metallib (18978 bytes)
ok    axpb: 4096 floats, 0 wrong (out[1234] = 3564.0, the CPU says 3564.0)
ok    collatz: 4096 numbers, 0 wrong (the longest: 237 steps from 3711)
ok    group_sum: 65536 numbers in 256 groups, total 134183984 (the CPU: 134183984), 0 group sums wrong
ok    triangle: 64x64 RGBA8, 1062 pixels inside and 2619 outside judged (415 on edges not), 0 wrong
m2v_test: everything ran and every number is the CPU's
```

The self-test is AdrenoMetal's (`nmb_selftest`) on our own shaders (`tools/metal/m2v_selftest.metal`): 1000 floats through a
kernel, a quad into an 8x8 target read back by a blit, and the same quad into 64x64 host pages imported without a copy. The other
three kernels exercise loops and branches, threadgroup memory with barriers, and a device atomic; the triangle is compared pixel
by pixel with the coverage and the interpolated colour its corners give (red bottom left, green bottom right, blue at the top, as
predicted). MoltenVK (`libMoltenVK.dylib` 1.4.2, on the M1 Pro's GPU) and KosmicKrisp (Mesa's Vulkan on Metal) give the same
lines, `ok` every one.

`tools/metal/metalvk-plugin-test.sh` then builds the plug-in's own three sources for macOS, makes the device the way Metal makes
it (`[[HakiosMetalDevice alloc] initWithAcceleratorPort:]`) and uses **the Metal API**: a library from the metallib's URL,
compute pipelines, a queue, compute, render and blit encoders, a completion handler, and a texture over an IOSurface, rendered
into and read from the surface's own memory. On lavapipe:

```
ok    the plug-in registered HakiosMetalDevice, a subclass of _MTLDevice
ok    -initWithAcceleratorPort: gives the frontend's device (NativeBridgeMetalDevice, on _MTLDevice): llvmpipe (LLVM 23.1.3, 128 bits)
ok    maxBufferLength 2147483648, unified memory
ok    newLibraryWithURL: axpb collatz group_sum triangle_vertex triangle_fragment ((null))
ok    newComputePipelineStateWithFunction: axpb ((null))
ok    axpb through the Metal API: 0 of 4096 floats wrong
ok    the completion handler ran, with the status completed
ok    group_sum through the Metal API: 134183984 (the CPU: 134183984)
ok    newRenderPipelineStateWithDescriptor: RGBA8 ((null))
ok    the triangle through the Metal API: 1062 pixels inside judged, 0 wrong
ok    newTextureWithDescriptor:iosurface:plane: a 128x96 BGRA8 texture over the surface (512 bytes a row)
ok    the triangle read from the IOSurface's own memory: 3407 pixels inside judged, 0 wrong
metalvk_plugin_test: the plug-in did everything asked, every number the CPU's
```

On MoltenVK (`tools/metal/metalvk-plugin-test.sh /opt/homebrew/lib/libMoltenVK.dylib`) every line is `ok` as well.

macOS's Metal is not iOS 16.5's: this shows the frontend, the bridge and the runtime doing what the Metal API asks on a real Vulkan
driver from the plug-in's own entry. It does not show that iOS's `MTLAddDevice` takes the device; `metal_test` on the guest says that.

### On gfxstream's guest Vulkan driver (its Mac twin, 29 October)

The driver Scion's guest will use (`libhakios.vk.dylib`, branch `gpu/gfxstream-vulkan`, `tools/gfxstream/VULKAN.md` there) has a
Mac build linked against the Android emulator's gfxstream host, MoltenVK under it: the same guest code and the same wire, in one
process. `m2v_test` on it (with `mk-vk.sh mac run`'s environment, and the emulator's `lib64` linked beside the program, where the host
looks for MoltenVK):

1. **Coherent memory.** The runtime (this pin, and metal2vulkan's master as well) puts Metal's `Shared`, `Managed` and `WriteCombined`
   memory, and its own host-visible blocks (64 MiB, mapped whole and kept mapped), only in a type that is
   `HOST_VISIBLE | HOST_COHERENT`, and never flushes or invalidates a mapped range. gfxstream's driver, which keeps a copy of each
   mapping in the guest (it cannot map the host's memory), lists none: `vkCreateDevice` succeeds, the runtime's first allocation finds
   no type, and the device is refused, `device: OutOfMemory(Device)`. No Vulkan 1.3 feature is wanted: the runtime's instance is 1.2,
   and it took the 1.2 device.
2. **The runtime now flushes and invalidates itself** (`tools/metal/m2v/0001-host-memory-not-coherent.patch`, under metal2vulkan's
   licence, applied after AdrenoMetal's 16): with no coherent host-visible type it takes the host-visible ones, each placement in
   whole `nonCoherentAtomSize` atoms; before each batch it flushes the command buffer's upload arena, and at its first batch every
   program buffer its commands use; as it completes, before its status is read, it invalidates the arena and the buffers its tracked
   uses write (and those it uses untracked), not those the GPU only reads, so the host's writes to a ring of per-frame data meanwhile
   are kept; its own buffers written once by the host (zeros, bytes, texture staging, fill patterns, resident tables, tile maps) are
   flushed after the write, and what a host step or `getBytes` reads of the GPU's writes is invalidated first. Not covered: an
   indirect command buffer's records and a visible function table's words (written through pointers the host keeps).
   `METAL2VULKAN_HOST_SYNC=always` takes the same steps on coherent memory too.
3. **Checked without the driver, on a driver made to behave like it.** `HAKIOS_VK_INCOHERENT=1` (`tools/metal/bridge/incoherent.rs`)
   makes lavapipe what gfxstream's driver is: no `HOST_COHERENT`, every mapping a copy the bytes leave and reach only by an explicit
   flush or invalidate, no host image copy. There every missing flush is a wrong number. `m2v_test` and the plug-in's Metal-API test
   (now also `replaceRegion:` and `getBytes:` of a `Shared` texture, through staging buffers) give every number right with nothing
   between the runtime and the driver; without the patch the device is refused, as on gfxstream. What crossed, for all of `m2v_test`:

   | | flushed | invalidated |
   |---|---|---|
   | the patched runtime alone | 1.0 MB in 34 calls | 55 KB in 6 calls |
   | coherence emulated between them (`coherence.rs`, the runtime seeing coherent memory) | 0.35 MB in 7 calls | 2.15 GB in 34 calls |

   The emulation invalidates every mapping (64 MiB blocks) after every wait; the runtime, knowing each command buffer's resources,
   moves what they hold. On lavapipe and MoltenVK as they are (coherent) every check passes as before.
4. **`tools/metal/bridge/coherence.rs`**, that emulation, is now off unless `HAKIOS_VK_COHERENCE=emulate` (for the two paths the patch
   does not cover). It works (checked the same way, after a fix: the runtime takes its device's functions before it asks for the
   memory types, so the wrappers are always installed and idle until it decides), but marking the types coherent turns the
   runtime's own flushing off. The same holds for coherence emulated in the driver: for this runtime the driver had better list its
   types as they are, not coherent; an emulating driver that follows explicit ranges could use `METAL2VULKAN_HOST_SYNC=always`.
5. **Then it deadlocks, every run.** The runtime calls Vulkan from several threads (the caller's, its pipeline workers, its
   completion threads); gfxstream's driver opens one stream per thread, and in this harness each stream is a virtio-gpu context of its
   own, while the host orders one process's commands per context. A sample of the hang: the main thread in `vkCreateQueryPool` (the
   GPU-timing pool made at a commit), two `metal2vulkan-pipelines` workers in `vkCreatePipelineCache`, all three in
   `ScionStream::readFully`, the host's render threads in `VkDecoder::decode`. `VULKAN.md` names the cure (stage 3, kernel 2): one
   context per process, a pipe per open, in the Mac transport and in the kernel's `/dev/vgpu3d`. A single-threaded run of the runtime
   is not a knob away: besides the pipeline pool (which already runs a piece on the caller's thread where it cannot start one), each
   `VkQueue` has a waiter thread in `vkWaitSemaphores`/`vkWaitForFences`, and a completion thread reads timestamps.

So nothing has yet been computed through gfxstream; what stands between is the driver's threading (one context per process).

### What is built for the guest

- `xnu-deps/build/metal2vulkan/out/ios/libmetal2vulkan.dylib`: arm64, iOS (`LC_BUILD_VERSION` platform 2, minos 16.0), install name
  `/redwood/lib/libmetal2vulkan.dylib`, 17.7 MB stripped, linking only libSystem and libiconv; built by rustup's Rust 1.98.1
  (metal2vulkan's own pin) for `aarch64-apple-ios`, kept apart from the machine's Rust in `xnu-deps/rust` (`HAKIOS_RUST`).
- `HakiosMetal.bundle` with `METAL=vk`: `HakiosMetalVK.mm` and the frontend (ARC, C++17) and `hakios_metal_shown.c`, compiled
  against the iPhoneOS 16.5 SDK (with `-Wall -Wextra` the only warnings are three deprecations in AdrenoMetal's code) and linked
  against the dylib, every symbol resolved; minos 16.0.
- `/redwood/bin/m2v_test` and `/redwood/share/m2v_test.metallib`: the Mac's test for the guest; `RC_JOBS=rc.jobs.metalvk` runs it
  on `/redwood/lib/libhakios.vk.dylib`, then `metal_test`.

None of it has run on iOS: there is no Vulkan driver in the guest yet.

### What is stubbed or missing, plainly

- **The driver.** gfxstream's guest Vulkan driver (`libhakios.vk.dylib`, branch `gpu/gfxstream-vulkan`) is built for iOS and runs
  on a Mac against the emulator's host, but has not run in the guest, and the runtime does not get through it yet (the section
  above: its threads; coherent memory the patched runtime now does without). Until then this route has computed and drawn on the
  Mac's own drivers only, and on lavapipe made to behave like gfxstream's.
- **IOSurfaces, and with them drawables, need the driver to import host memory.** A texture over an IOSurface is a no-copy buffer
  over the surface's pages (the frontend's `newTextureWithDescriptor:iosurface:plane:`), and at this pin the runtime makes one only
  through `VK_EXT_external_memory_host` (or dma-bufs on Linux); without it the texture is refused, logged
  (`native-metal: surface import refused`), and the caller gets nil. CAMetalLayer's drawables and every surface CoreAnimation hands
  Metal are such textures. gfxstream's guest driver has no external memory (its `VULKAN.md`), so on it every IOSurface texture is
  refused today. metal2vulkan's master (after 26 October) has *copied host buffers*: where a driver cannot import the pages, the
  no-copy object is a copy of them, compared and synced at each commit. That needs the pin moved, or a copy of the same kind in the
  bridge (a texture of its own, filled from the surface before a command buffer reads it and written back after one writes it).
- **Present.** The frontend has no drawable or present code of its own (AdrenoMetal's scanout client, `GpuScanout*`, is the
  tablet's display hand-off and is not taken). A CAMetalLayer presents through CoreAnimation, which composites the drawable's
  IOSurface: so it is the point above, and then the window server's own path to `MiniIOMobileFramebuffer`.
- **The window server still cannot run on Metal** on iOS 16.5: QuartzCore's `default.metallib` is not in the image (step 7 above).
  This route does not change that; its clients are CoreImage, RenderBox, the camera's views, applications.
- **Not implemented by the frontend**, each refused with its reason: argument buffers and indirect command buffers,
  `newTextureWithBytesNoCopy:`, Apple's texture layouts and pipeline libraries (Apple GPU binaries), protected content, shared events
  across processes. GPU priorities are not applied.
- **Device answers the runtime has not got** (logged once at creation, left at `_MTLDevice`'s 0/NO): `gpuCoreCount`,
  `maxVertexAmplificationCount`, `maxComputeThreadgroupMemory`, `isLargeMRTSupported`, `supportsComputeCompressedTextureWrite`,
  `supportsConditionalLoadStore`.
- **Changes to AdrenoMetal's code** (each marked `Scion:`): the `_MTLDevice` size check of iPadOS 17.7.11 (536 bytes) is only
  logged, since iOS 16.5's is not known to match and the subclass adds no instance variables; the frame-trace writer (a thread
  writing under backboardd's caches every 0.4 s) runs only with `HAKIOS_METAL_TRACE` set; the driver is opened through `icd.rs`.
  The frontend still flushes the CPU caches of a command's surfaces at commit (`sys_dcache_flush`), which the tablet's GPU needed
  and which costs only time here. Shader priming (their `PrimeShaders.h`) is not taken; its counters stay zero.
- **One object kept per process.** `-initWithAcceleratorPort:` returns another object than the one Metal allocated; that one, a
  `_MTLDevice` whose `-init` never ran, is deliberately not released.
- **Building** needs Xcode's Metal toolchain (`xcodebuild -downloadComponent MetalToolchain`, 839 MB, once) for the shaders, rustup's
  toolchain (0.6 GB in `xnu-deps/rust`, installed by the script when missing), and about 0.4 GB of intermediates per platform,
  removed after the build unless `KEEP=1`.

### What is needed once the gfxstream driver lands

1. The driver at `/redwood/lib/libhakios.vk.dylib` (or anywhere, with `METAL_ICD=PATH`), exporting `vk_icdGetInstanceProcAddr` or
   `vkGetInstanceProcAddr`, giving a Vulkan 1.2 device with `timelineSemaphore` and a queue family with graphics and compute.
   gfxstream's has all of that. What it still needs for this runtime: **one virtio-gpu context per process** (its Mac transport
   and the kernel's `/dev/vgpu3d`), since the runtime calls Vulkan from several threads. Coherent memory it does not need (the
   patch above). Then `m2v_test` on its Mac twin is the check to run first (the section above says how).
2. An image with `METAL=vk` and whatever the driver needs of the kernel (gfxstream's `/dev/vgpu3d` path), booted with `metal=1`
   and `RC_JOBS=rc.jobs.metalvk`: `m2v_test`'s lines are the first result, the same lines as the Mac's above; `metal_test` the
   second (does iOS 16.5's Metal take the device?).
3. IOSurfaces (the point above), one of: the driver offering `VK_EXT_external_memory_host` over guest pages; IOSurfaces allocated
   from memory the driver can import (gfxstream's blob resources) and the runtime taught that kind of import; or the pin moved to a
   metal2vulkan with copied host buffers, which means porting the bridge to the runtime's newer API and checking AdrenoMetal's
   patches again (they will have moved upstream or need redoing).
4. Then `metal=2` for clients other than the window server (CoreImage, RenderBox), reading the `asked and not implemented` lines
   as the next list, as steps 1 and 2 above did.

On the Galaxy S9 the same build would point at Mesa's panvk (`METAL_ICD`); the runtime's floor (Vulkan 1.2, timeline semaphores)
is the question to ask of panvk on the Mali G72.
