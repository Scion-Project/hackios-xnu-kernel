/*
 * HakiosGPU: the phone's Adreno GPU, for programs to draw with.
 *
 * Depends on drivers/gpu/adreno/hakios.adreno.c, which wakes the GPU and, with boot-arg
 * adreno_keep=1, keeps it on and starts this service. The userland side is tools/mesa/hakios-drm,
 * a stand-in for libdrm inside Mesa.
 *
 * The interface is Linux's msm DRM one, as Mesa's freedreno uses it. Each request is a call to the
 * user client with the request number as the selector; the request structure is passed in and back.
 * mmap() on the device gives the client memory of the type that is the buffer's handle, as on Linux.
 *
 * Supported: parameters, buffers (create, get GPU address, map, free), command buffer submission,
 * and waiting for a submission to finish. A buffer is wired kernel memory entered into the 3D core's
 * SMMU context. It is zeroed when made and write-combined, so CPU writes go to memory where the GPU
 * reads them, not through this processor's cache. Boot-arg gpu_cached=1 makes buffers ordinary cached
 * memory; they are then pushed out of the cache before each submission (see submit()).
 *
 * Not Linux's: kHakiosSurface (surfaceBuffer()) gives a buffer backed by an IOSurface's memory, so a
 * program can draw into what the display shows, or read frames other programs drew. Linux passes such
 * memory as file descriptors; here the surface is named by its ID. That memory is ordinary cached
 * memory, shared with the other users of the surface.
 *
 * Not implemented: interrupts (waits poll every GPU_ASK_AGAIN_US), fences and buffers as file
 * descriptors, and per-client address spaces. All clients share one address space, so a client's
 * buffers go away with it. A GPU that stops answering is reported (hakios_gpu_report()) and, with
 * boot-arg gpu_recover=1, reset and restarted, which loses the submission that hung; see
 * hakios_gpu_recover() in bsd/dev/hakios.adreno.c. Without the boot-arg it stays stopped, and nothing
 * is waited for again.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <sys/systm.h>

#include "MiniIOSurface.h"

extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);

extern "C" {
/* from drivers/gpu/adreno/hakios.adreno.c; declared in bsd/sys/hakios.input.h, not included here */
bool hakios_gpu_ready(void);
bool hakios_gpu_parameter(uint32_t which, uint64_t * value);
uint64_t hakios_gpu_address(uint64_t size);
void hakios_gpu_address_free(uint64_t address, uint64_t size);
void hakios_gpu_space(uint64_t * used, uint32_t * live, uint64_t * largest, unsigned * holes, unsigned * most,
    uint64_t * lost, uint32_t * refused);
extern uint32_t gpu_va;                 /* boot-arg gpu_va: 0 off, 1 log the address space (see submit()), 2 also guard the addresses */
bool hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void hakios_gpu_mapped(void);
void hakios_gpu_unmap(uint64_t address, uint64_t size);
void hakios_gpu_to_memory(void * start, uint64_t size);
uint32_t hakios_gpu_submit(const uint64_t * addresses, const uint32_t * words, uint32_t count);
extern uint32_t gpu_log;
bool hakios_gpu_done(uint32_t fence);
uint32_t hakios_gpu_last(void);
void hakios_gpu_worked(uint64_t * microseconds, uint32_t * submissions, uint32_t by[7]);
void hakios_gpu_report(const char * why);
bool hakios_gpu_recover(void);
void hakios_gpu_stop(void);

void HakiosGPUStart(void);
void HakiosGPUStop(void);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);
}

/* Linux's include/uapi/drm/msm_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
	kHakiosGPUWorked       = 0x7d,         /* not Linux's: the 3D core's busy time and submission count, see struct hakios_worked */
	kHakiosSurface         = 0x7e,         /* not Linux's: an IOSurface's memory as a buffer */
	kHakiosShow            = 0x7f,         /* not Linux's: copy the caller's pixels to the panel */
};

struct msm_timespec {
	int64_t seconds, nanoseconds;           /* of the continuous clock, which runs during sleep */
};
struct msm_param {
	uint32_t pipe, param;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_new {
	uint64_t size;
	uint32_t flags, handle;
};
struct msm_gem_info {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_cpu_prep {
	uint32_t handle, op;
	struct msm_timespec timeout;
};
struct msm_gem_submit_cmd {
	uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
	uint64_t relocs;
};
struct msm_gem_submit_bo {
	uint32_t flags, handle;
	uint64_t presumed;
};
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence {
	uint32_t fence, flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};
struct msm_gem_madvise {
	uint32_t handle, madv, retained;
};
struct msm_submitqueue {
	uint32_t flags, prio, id;
};
struct msm_submitqueue_query {
	uint64_t data;
	uint32_t id, param, len, pad;
};
struct drm_gem_close {
	uint32_t handle, pad;
};
struct hakios_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* in: kHakiosDescribe for the layout only; out: the buffer's handle */
	uint64_t size;                          /* out: the buffer's size */
	uint32_t offset, rowBytes;              /* out: where the pixels start, and bytes per row */
	uint32_t width, height, pixelFormat, planes;    /* out: the pixel layout */
};
struct hakios_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};
/*
 * kHakiosGPUWorked: the GPU core's own time for a measurement the program makes.
 *
 * A program timing this driver (userland/src/metal_drv_test.m --time) sees only wall time, which
 * includes its own work, this request, the submission, the core's execution and the wait. The
 * kernel times every submission from CP_ALWAYS_ON_COUNTER (19.2 ticks a microsecond, in
 * bsd/dev/hakios.adreno.c) and only logged the result. This request returns those numbers, so the
 * subtraction can be done where both halves are known.
 *
 * Reading resets the counters (hakios_gpu_worked(): "since this was last asked"). Two programs
 * reading at once each get part of the total. submit()'s five-second log line uses the same
 * counters, so a boot in which this request is used has gaps in those log lines.
 */
struct hakios_worked {
	uint64_t microseconds;                  /* the core's own time on the submissions counted here */
	uint32_t submissions;                   /* how many of them it timed; 0 means nothing finished since */
	uint32_t by[7];                         /* ... of which under 0.1, 0.25, 0.5, 1, 2, 4 ms, and longer */
};

#define kHakiosDescribe        0xffffffffu

#define MSM_INFO_GET_OFFSET     0
#define MSM_INFO_GET_IOVA       1
#define MSM_INFO_SET_NAME       2
#define MSM_INFO_SET_IOVA       4
#define MSM_INFO_GET_FLAGS      5
#define MSM_BO_GPU_READONLY     0x00000002
#define MSM_PREP_NOSYNC         0x04
#define MSM_SUBMIT_CMD_BUF      1
#define MSM_SUBMIT_FENCE_FD_OUT 0x20000000

#define GPU_BUFFERS             8192            /* per client */
#define GPU_BUFFER_MAX          (256u << 20)
#define GPU_SUBMIT_CMDS         128
#define GPU_SUBMIT_BOS          8192
#define GPU_PATIENCE_MS         5000            /* longest a wait lasts, however far off its deadline */
#define GPU_ASK_AGAIN_US        200             /* between polls of the GPU during a wait */
#define GPU_PUSH_AGAIN_MS       4               /* minimum interval between cache flushes of an unchanged surface */

static IOLock * gGPULock;                       /* guards the GPU and all buffer state; the hakios_gpu_*() calls are made under it */
static bool gGPUReported;
static bool gGPUCached;                         /* boot-arg gpu_cached=1 */
static bool gGPUSaidSetIova;                    /* the MSM_INFO_SET_IOVA refusal has been logged */

/* Counters for the periodic log line in submit(); reset after each line. */
static struct {
	uint64_t since, flushing, waiting, taking;      /* times of the processor's clock */
	uint64_t flushed;                       /* bytes sent out of the cache */
	uint32_t submissions, commands, buffers, surfaces, waits, over;
	uint32_t made, given_again;             /* buffers made for programs, and kept buffers given again */
	uint64_t making;                        /* time spent making buffers */
} gGPUStat;

/* Waits up to `milliseconds` for the 3D core to finish submission `fence`. Called with gGPULock held;
 * the lock is dropped while polling. Returns false on timeout. */
static bool
gpuWait(uint32_t fence, uint64_t milliseconds)
{
	uint64_t began = mach_absolute_time(), patience;

	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
	for (bool waited = false;; waited = true) {
		if (hakios_gpu_done(fence)) {
			if (waited) {
				gGPUStat.waits++;
				gGPUStat.waiting += mach_absolute_time() - began;
			}
			return true;
		}
		if (mach_absolute_time() - began >= patience || gGPUReported) {
			break;                  /* once the GPU is reported stopped, waits end at once */
		}
		IOLockUnlock(gGPULock);
		delay_for_interval(GPU_ASK_AGAIN_US, NSEC_PER_USEC);
		IOLockLock(gGPULock);
	}
	/*
	 * A wait of a second or more that timed out means the core has stopped answering. Its state is
	 * reported. With boot-arg gpu_recover=1 the core is then reset and restarted, which gives up the
	 * hung submission. If that does not happen, waits end at once for the rest of the run, since the
	 * stopped GPU may still write memory. This wait fails either way.
	 */
	if (milliseconds >= 1000 && !gGPUReported) {
		hakios_gpu_report("no answer");
		gGPUReported = !hakios_gpu_recover();
	}
	return false;
}

bool
MiniIOSurfaceDrawn(MiniIOSurface * surface, uint64_t milliseconds)
{
	uint32_t fence = surface->drawn;
	bool done = true;

	if (surface->maliSeq != 0) {
		/* drawn by the Mali GPU instead (drivers/gpu/mali/hakios.mali.cpp); present when both GPU drivers are built in */
		return HakiosMaliDrawn(surface, milliseconds);
	}
	if (fence != 0 && gGPULock != NULL) {
		IOLockLock(gGPULock);
		done = !hakios_gpu_ready() || gpuWait(fence, milliseconds);
		IOLockUnlock(gGPULock);
	}
	return done;
}

/*
 * Boot-arg touch_lat=1: logs each wait of 1 ms or more as tl: f KIND US at US, for tools/frames.py.
 */
static TUNABLE(uint32_t, gWaitLines, "touch_lat", 0);

static void
slowWait(const char * kind, uint64_t began)
{
	uint64_t now = mach_absolute_time(), ns, at;

	if (!gWaitLines) {
		return;
	}
	absolutetime_to_nanoseconds(now - began, &ns);
	if (ns >= NSEC_PER_MSEC) {
		absolutetime_to_nanoseconds(now, &at);
		hakios_tl("tl: f %s %llu at %llu\n", kind, ns / 1000, at / 1000);
	}
}

/* Milliseconds from now until `when` (a msm_timespec on the continuous clock), capped at
 * GPU_PATIENCE_MS; 0 if now or past. */
static uint64_t
millisecondsUntil(const struct msm_timespec * when)
{
	uint64_t now;
	int64_t left, end;

	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	/* a time too large for nanoseconds is far ahead (capped) or far behind (0), not a wrapped value */
	if (__builtin_mul_overflow(when->seconds, (int64_t) NSEC_PER_SEC, &end) || __builtin_add_overflow(end, when->nanoseconds, &end)) {
		return when->seconds > 0 && when->nanoseconds >= 0 ? GPU_PATIENCE_MS : 0;
	}
	if (end <= 0) {
		return 0;                       /* the clock never reads negative, so this time is in the past */
	}
	left = end - (int64_t) now;
	if (left <= 0) {
		return 0;
	}
	return (uint64_t) left / NSEC_PER_MSEC > GPU_PATIENCE_MS ? GPU_PATIENCE_MS : (uint64_t) left / NSEC_PER_MSEC;
}

/*
 * A wait whose deadline had already passed when it was asked for, and the GPU was not done. The
 * caller goes on as if the work were done (a picture may be shown half drawn). This usually means
 * the caller's clock is not this one. The first four are logged.
 */
static void
waitOver(const struct msm_timespec * when, uint32_t fence)
{
	static uint32_t told;
	uint64_t now;

	gGPUStat.over++;
	if (told < 4) {
		told++;
		absolutetime_to_nanoseconds(mach_continuous_time(), &now);
		IOLog("HakiosGPU: a wait for submission %u, which is not done, until %lld.%09lld s, asked for at %llu.%09llu s: "
		    "over before it began\n", fence, when->seconds, when->nanoseconds, now / NSEC_PER_SEC, now % NSEC_PER_SEC);
	}
}

class HakiosGPU : public IOService
{
	OSDeclareDefaultStructors(HakiosGPU);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosGPU, IOService);

bool
HakiosGPU::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "HakiosGPUUserClient");
	setName("HakiosGPU");
	registerService();
	return true;
}

void
HakiosGPUStart(void)
{
	IOService * provider = IOService::getPlatform();
	HakiosGPU * gpu;
	uint32_t cached = 0;

	gGPUCached = PE_parse_boot_argn("gpu_cached", &cached, sizeof(cached)) && cached != 0;
	gGPULock = IOLockAlloc();
	gpu = OSTypeAlloc(HakiosGPU);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("HakiosGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("HakiosGPU: the service did not start\n");
	}
	gpu->release();
}

/* Called by hakios.adreno.c when the run ends. Holding gGPULock means no request is in progress
 * when the GPU stops. */
void
HakiosGPUStop(void)
{
	IOLockLock(gGPULock);
	hakios_gpu_stop();
	IOLockUnlock(gGPULock);
}

struct gpu_buffer {
	IOMemoryDescriptor *            memory;         /* NULL: this handle is free */
	uint8_t *                       kernel;         /* kernel address of the memory */
	MiniIOSurface *                 surface;        /* the IOSurface whose memory this is, if not its own */
	uint64_t                        address;        /* GPU address */
	uint64_t                        size;
	uint32_t                        flags;
	uint32_t                        seed;           /* the surface's seed at its last cache flush, */
	uint64_t                        pushed;         /* ... and the time of that flush */
	bool                            seen;           /* mapped by the program; the program may write it from then on */
	uint32_t                        fence;          /* the last submission that names it (0: none has) */
};

/*
 * A buffer freed while the GPU still uses it. Its memory and GPU address stay allocated until the
 * submission finishes; reap() releases them later, so freeing does not wait. (Waiting here cost a
 * window server about five milliseconds a frame: it frees the last frame's surface while the GPU
 * draws this one.)
 */
struct gpu_left {
	IOMemoryDescriptor *            memory;
	MiniIOSurface *                 surface;
	uint64_t                        address, size;
	uint32_t                        fence, flags;
};
#define GPU_LEFT                64

/*
 * A program's buffer that was freed and is no longer used by the GPU. It is kept, with its memory and
 * GPU address, for the next buffer of the same size and flags, saving the cost of making one: zeroing,
 * write-combining, and entering each page into the GPU's address space, which took 2 to 3.5 ms of the
 * window server's frame. A kept buffer is handed out as it is, not cleared, since it was the same
 * program's (Mesa keeps given-back buffers the same way). Limits: GPU_KEPT_BYTES in total, with the
 * longest-kept dropped first, and none kept for longer than GPU_KEPT_SECONDS.
 * Measured with rc.jobs.fps in Search: 8 such textures made instead of 77 (a blur's texture, 1082 x
 * 1226, 6 MB with Mesa's rounding), and window-server frames at 6.8 ms median instead of 9.3-10 ms.
 */
struct gpu_kept {
	IOMemoryDescriptor *            memory;         /* NULL: this place is free */
	uint8_t *                       kernel;
	uint64_t                        address, size, since;
	uint32_t                        flags;
};
#define GPU_KEPT                48
#define GPU_KEPT_BYTES          (160ull << 20)
#define GPU_KEPT_SECONDS        60              /* a buffer not asked for again in so long is let go */
#define GPU_KEEP_FROM           (256u << 10)    /* smaller buffers are not kept */
static TUNABLE(uint32_t, gGPUKeep, "gpu_keep", 1);      /* boot-arg gpu_keep: 0 keeps none (for tests), 1 keeps buffers, 2 also clears them when given again */

class HakiosGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(HakiosGPUUserClient);

	task_t                  fTask;
	struct gpu_buffer *     fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextQueue;
	uint32_t                fNextBuffer;            /* where the search for a free handle starts, so a freed handle is not reused at once */
	uint32_t                fBufferCount;
	uint64_t                fBufferBytes;
	uint32_t                fSubmits;
	uint32_t                fStrangers;             /* requests that named a buffer the client does not have */
	struct gpu_left         fLeft[GPU_LEFT];
	uint32_t                fLeftCount;
	struct gpu_kept         fKept[GPU_KEPT];
	uint64_t                fKeptBytes;

	struct gpu_buffer * buffer(uint32_t handle);
	struct gpu_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle);
	void freeBuffer(struct gpu_buffer * b);
	bool keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags);
	void dropKept(struct gpu_kept * k);
	void reap(void);
	IOReturn gemNew(struct msm_gem_new * request);
	IOReturn surfaceBuffer(struct hakios_surface * request);
	IOReturn gemInfo(struct msm_gem_info * request);
	IOReturn submit(struct msm_gem_submit * request);
	IOReturn show(const struct hakios_show * request);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosGPUUserClient, IOUserClient);

bool
HakiosGPUUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
HakiosGPUUserClient::start(IOService * provider)
{
	if (!hakios_gpu_ready() || !IOUserClient::start(provider)) {
		return false;
	}
	fBuffers = IONewZero(struct gpu_buffer, GPU_BUFFERS);
	return fBuffers != NULL;
}

/*
 * Frees a buffer's handle and memory. Called with gGPULock held. If the GPU still uses the buffer,
 * its memory is kept until the GPU is done (see reap()). If there is no room for that, the memory
 * keeps an extra reference and is never freed, since a GPU that has stopped may still write to it.
 */
void
HakiosGPUUserClient::freeBuffer(struct gpu_buffer * slot)
{
	/*
	 * gpuWait() drops the lock. The buffer is taken out of its handle first and handled as a copy, so a
	 * second close of the same handle cannot free its memory or address twice.
	 */
	struct gpu_buffer saved = *slot;
	struct gpu_buffer * b = &saved;

	slot->memory = NULL;
	slot->surface = NULL;
	fBufferCount--;
	fBufferBytes -= b->size;

	bool busy = hakios_gpu_ready() && b->fence != 0 && !hakios_gpu_done(b->fence);

	if (busy && fLeftCount == GPU_LEFT) {
		busy = !gpuWait(b->fence, 1000);        /* no room in fLeft: wait for this one */
	}
	if (busy && fLeftCount < GPU_LEFT) {
		struct gpu_left * left = &fLeft[fLeftCount++];

		left->memory = b->memory;
		left->surface = b->surface;
		left->address = b->address;
		left->size = b->size;
		left->fence = b->fence;
		left->flags = b->flags;
	} else {
		if (busy) {
			static uint32_t said;

			if (b->surface == NULL) {
				b->memory->retain();
			} else {
				b->surface->memory->retain();
			}
			if (said++ < 8) {
				IOLog("HakiosGPU: the GPU is not done with what it was given: a buffer of %llu bytes stays its own\n", b->size);
			}
		} else if (b->surface == NULL && hakios_gpu_ready() && keep(b->memory, b->kernel, b->address, b->size, b->flags)) {
			b->memory = NULL;               /* the memory now belongs to the kept entry */
		} else if (hakios_gpu_ready()) {
			hakios_gpu_unmap(b->address, b->size);
			hakios_gpu_address_free(b->address, b->size);
		}
		if (b->memory != NULL) {
			b->memory->release();
		}
		if (b->surface != NULL) {
			MiniIOSurfaceDrop(b->surface);
		}
	}
}

/* Releases the fLeft buffers the GPU has finished with. Called with gGPULock held. */
void
HakiosGPUUserClient::reap(void)
{
	for (uint32_t i = 0; i < fLeftCount;) {
		struct gpu_left * left = &fLeft[i];

		if (hakios_gpu_ready() && !hakios_gpu_done(left->fence)) {
			i++;
			continue;
		}
		if (left->surface == NULL && hakios_gpu_ready() &&
		    keep(left->memory, NULL, left->address, left->size, left->flags)) {
			left->memory = NULL;
		} else if (hakios_gpu_ready()) {
			hakios_gpu_unmap(left->address, left->size);
			hakios_gpu_address_free(left->address, left->size);
		}
		if (left->memory != NULL) {
			left->memory->release();
		}
		if (left->surface != NULL) {
			MiniIOSurfaceDrop(left->surface);
		}
		*left = fLeft[--fLeftCount];
	}
}

IOReturn
HakiosGPUUserClient::clientClose(void)
{
	if (fBuffers != NULL) {
		IOLockLock(gGPULock);
		/* the client's last submission may still be using these buffers */
		if (hakios_gpu_ready()) {
			gpuWait(hakios_gpu_last(), 1000);
		}
		for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
			if (fBuffers[i].memory != NULL) {
				freeBuffer(&fBuffers[i]);
			}
		}
		reap();
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			dropKept(&fKept[i]);
		}
		if (fLeftCount != 0) {
			IOLog("HakiosGPU: the GPU is not done with %u buffers of a program that is gone: they stay its own\n", fLeftCount);
		}
		IOLockUnlock(gGPULock);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
HakiosGPUUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

struct gpu_buffer *
HakiosGPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > GPU_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		/* a program naming a freed handle may be drawing into someone else's newer buffer */
		if (handle != 0 && fStrangers++ < 16) {
			IOLog("HakiosGPU: buffer %u is asked for and is not there\n", handle);
		}
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/*
 * Allocates a handle and a GPU address range for `size` bytes of `memory`, from byte `from` on
 * (wired memory, whole pages). Called with gGPULock held. Returns NULL when there is no free handle,
 * address range or mapping.
 */
struct gpu_buffer *
HakiosGPUUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle)
{
	struct gpu_buffer * b = NULL;
	bool mapped = true;

	for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL || (b->address = hakios_gpu_address(size)) == 0) {
		IOLog("HakiosGPU: no %s left for a buffer of %llu bytes\n", b ? "addresses" : "handles", size);
		return NULL;
	}
	for (uint64_t offset = 0; offset < size && mapped; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && hakios_gpu_map(b->address + offset, phys, flags & MSM_BO_GPU_READONLY);
	}
	hakios_gpu_mapped();
	if (!mapped) {
		hakios_gpu_unmap(b->address, size);
		hakios_gpu_address_free(b->address, size);
		IOLog("HakiosGPU: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	b->size = size;
	b->flags = flags;
	b->seed = 0;
	b->pushed = 0;
	b->seen = false;
	b->fence = 0;
	b->surface = NULL;
	fBufferCount++;
	fBufferBytes += size;
	return b;
}

/* Called with gGPULock held. */
void
HakiosGPUUserClient::dropKept(struct gpu_kept * k)
{
	if (k->memory == NULL) {
		return;
	}
	if (hakios_gpu_ready()) {
		hakios_gpu_unmap(k->address, k->size);
		hakios_gpu_address_free(k->address, k->size);
	}
	k->memory->release();
	k->memory = NULL;
	fKeptBytes -= k->size;
}

/* Called with gGPULock held, and the GPU done with the buffer. Keeps it if it is large enough and
 * fits; the oldest kept buffers are dropped to make room. */
bool
HakiosGPUUserClient::keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags)
{
	struct gpu_kept * place = NULL;

	if (gGPUKeep == 0 || size < GPU_KEEP_FROM || size > GPU_KEPT_BYTES / 4 || memory == NULL ||
	    (kernel == NULL && OSDynamicCast(IOBufferMemoryDescriptor, memory) == NULL)) {
		return false;
	}
	for (;;) {
		struct gpu_kept * oldest = NULL;

		place = NULL;
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			if (fKept[i].memory == NULL) {
				place = place != NULL ? place : &fKept[i];
			} else if (oldest == NULL || fKept[i].since < oldest->since) {
				oldest = &fKept[i];
			}
		}
		if (place != NULL && fKeptBytes + size <= GPU_KEPT_BYTES) {
			break;
		}
		dropKept(oldest);
	}
	place->memory = memory;
	place->kernel = kernel != NULL ? kernel : (uint8_t *) OSDynamicCast(IOBufferMemoryDescriptor, memory)->getBytesNoCopy();
	place->address = address;
	place->size = size;
	place->flags = flags;
	place->since = mach_absolute_time();
	fKeptBytes += size;
	return true;
}

IOReturn
HakiosGPUUserClient::gemNew(struct msm_gem_new * request)
{
	uint64_t size = round_page_64(request->size), began = mach_absolute_time();
	struct gpu_buffer * b;
	IOBufferMemoryDescriptor * memory;

	if (size == 0 || size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/* reuse a kept buffer of the same size and flags: the most recently kept (likeliest still in the caches) */
	IOLockLock(gGPULock);
	struct gpu_kept * found = NULL;
	uint64_t old;

	nanoseconds_to_absolutetime(GPU_KEPT_SECONDS * NSEC_PER_SEC, &old);
	for (uint32_t i = 0; i < GPU_KEPT; i++) {
		struct gpu_kept * k = &fKept[i];

		if (k->memory != NULL && began - k->since > old) {
			dropKept(k);
		}

		if (k->memory != NULL && k->size == size && k->flags == request->flags && (found == NULL || k->since > found->since)) {
			found = k;
		}
	}
	for (uint32_t n = 0; found != NULL && n < GPU_BUFFERS; n++) {
		uint32_t h = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[h].memory == NULL) {
			b = &fBuffers[h];
			bzero(b, sizeof(*b));
			b->memory = found->memory;
			b->kernel = found->kernel;
			b->address = found->address;
			b->size = found->size;
			b->flags = found->flags;
			fNextBuffer = h + 1;
			fBufferCount++;
			fBufferBytes += size;
			request->handle = h + 1;
			found->memory = NULL;
			fKeptBytes -= size;
			gGPUStat.given_again++;
			IOLockUnlock(gGPULock);
			if (gGPUKeep == 2) {
				bzero(b->kernel, (size_t) b->size);
			}
			return kIOReturnSuccess;
		}
	}
	IOLockUnlock(gGPULock);
	/* the new memory is zeroed; making it write-combined pushes the zeroes out of the cache */
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    (gGPUCached ? 0 : kIOMapWriteCombineCache), (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("HakiosGPU: no memory for a buffer of %llu bytes (%u buffers, %llu MB with this client)\n", size, fBufferCount,
		    fBufferBytes >> 20);
		return kIOReturnNoMemory;
	}
	if (gGPUCached) {
		hakios_gpu_to_memory(memory->getBytesNoCopy(), size);
	}

	IOLockLock(gGPULock);
	b = newBuffer(memory, 0, size, request->flags, &request->handle);
	if (b != NULL) {
		b->memory = memory;
		b->kernel = (uint8_t *) memory->getBytesNoCopy();
		gGPUStat.made++;
		gGPUStat.making += mach_absolute_time() - began;
	}
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

/*
 * An IOSurface's memory as a buffer: its pixels, from the page after the surface's shared state
 * (MiniIOSurface.h). The surface is retained while the buffer exists. Asking again for a surface that
 * already has a buffer of this client returns that buffer. The client must not give that buffer back
 * at the same moment. kHakiosDescribe asks for the layout only; no buffer is made.
 */
IOReturn
HakiosGPUUserClient::surfaceBuffer(struct hakios_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct gpu_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size, began = mach_absolute_time();

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL) {
		/* a surface made around a program's own memory: its pixels are not in whole pages, and rows seldom suit the GPU */
		MiniIOSurfaceDrop(s);
		return kIOReturnUnsupported;
	}
	size = round_page_64(s->info.allocSize);
	memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > GPU_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kHakiosDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}

	IOLockLock(gGPULock);
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
	} else if ((b = newBuffer(s->memory, PAGE_SIZE, size, 0, &request->handle)) != NULL) {
		b->memory = memory;
		b->kernel = s->kbase + PAGE_SIZE;
		b->surface = s;
		gGPUStat.surfaces++;
	}
	gGPUStat.taking += mach_absolute_time() - began;
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

IOReturn
HakiosGPUUserClient::gemInfo(struct msm_gem_info * request)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	IOLockLock(gGPULock);
	b = buffer(request->handle);
	if (b == NULL) {
		ret = kIOReturnBadArgument;
	} else if (request->info == MSM_INFO_GET_OFFSET) {
		request->value = (uint64_t) request->handle << 32;      /* mmap uses the handle (clientMemoryForType), not an offset */
	} else if (request->info == MSM_INFO_GET_IOVA) {
		/*
		 * The buffer's GPU address: fixed from when it is made until it is freed, so a caller may read it
		 * once and keep it (freedreno does, in fd_bo_init_common).
		 */
		request->value = b->address;
	} else if (request->info == MSM_INFO_SET_IOVA) {
		/*
		 * Buffer addresses are the kernel's to choose. Every client shares SMMU context 0, so an address one
		 * program picked would also be in every other program's space. Linux refuses VA_START and VA_SIZE
		 * unless the client has page tables of its own (adreno_gpu.c, adreno_get_param), and placing a
		 * buffer is the stronger request of the two. The refusal is logged once, by name.
		 */
		if (!gGPUSaidSetIova) {
			gGPUSaidSetIova = true;
			IOLog("HakiosGPU: a program asked to place buffer %u at %#llx itself: refused. There is one "
			    "page table here for every client, so addresses are the kernel's to give and "
			    "MSM_INFO_GET_IOVA is how to learn them\n", request->handle, request->value);
		}
		ret = kIOReturnUnsupported;
	} else if (request->info == MSM_INFO_GET_FLAGS) {
		request->value = b->flags;
	} else if (request->info != MSM_INFO_SET_NAME) {            /* SET_NAME: a debugging name, not kept */
		ret = kIOReturnUnsupported;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
HakiosGPUUserClient::submit(struct msm_gem_submit * request)
{
	struct msm_gem_submit_bo * bos = NULL;
	struct msm_gem_submit_cmd * cmds = NULL;
	uint64_t addresses[GPU_SUBMIT_CMDS];
	uint32_t words[GPU_SUBMIT_CMDS], count = 0;
	uint64_t began, nanos, again;
	IOReturn ret = kIOReturnBadArgument;

	if (request->nr_cmds == 0 || request->nr_cmds > GPU_SUBMIT_CMDS || request->nr_bos == 0 || request->nr_bos > GPU_SUBMIT_BOS) {
		IOLog("HakiosGPU: a submission of %u command buffers in %u buffers: not taken\n", request->nr_cmds, request->nr_bos);
		return kIOReturnBadArgument;
	}
	bos = IONew(struct msm_gem_submit_bo, request->nr_bos);
	cmds = IONew(struct msm_gem_submit_cmd, request->nr_cmds);
	if (bos == NULL || cmds == NULL) {
		ret = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t) request->bos, bos, request->nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t) request->cmds, cmds, request->nr_cmds * sizeof(*cmds)) != 0) {
		ret = kIOReturnVMError;
		goto out;
	}
	IOLockLock(gGPULock);
	/*
	 * Cache coherence. The GPU reads and writes memory, so what this processor's cache holds of a buffer
	 * is pushed out before a submission that may use it:
	 * - an IOSurface buffer when its seed has changed since the last push (writers are to lock the
	 *   surface, and so is a program that has had the GPU draw into it: userland/src/eagl.c), and in any
	 *   case once every GPU_PUSH_AGAIN_MS, since not every writer changes the seed (an application
	 *   drawing a picture that is on screen meanwhile does not);
	 * - a surface this program has mapped, every time;
	 * - a buffer made here, when gpu_cached=1 and the program has mapped it.
	 * The pushes are followed by a barrier.
	 */
	began = mach_absolute_time();
	nanoseconds_to_absolutetime(GPU_PUSH_AGAIN_MS * NSEC_PER_MSEC, &again);
	for (uint32_t i = 0; i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b == NULL) {
			continue;
		}
		if (b->surface != NULL) {
			uint32_t seed = b->surface->shared()->seed;

			if (seed == b->seed && !b->seen && began - b->pushed < again) {
				continue;
			}
			b->seed = seed;
			b->pushed = began;
		} else if (!b->seen || !gGPUCached) {
			continue;
		}
		hakios_gpu_to_memory(b->kernel, b->size);
		gGPUStat.flushed += b->size;
	}
	hakios_gpu_to_memory(NULL, 0);                 /* barrier after the pushes */
	gGPUStat.flushing += mach_absolute_time() - began;
	gGPUStat.buffers += request->nr_bos;
	for (uint32_t i = 0; i < request->nr_cmds; i++) {
		struct gpu_buffer * b = cmds[i].submit_idx < request->nr_bos ? buffer(bos[cmds[i].submit_idx].handle) : NULL;

		if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;                               /* other command types apply to GPUs shared between address spaces; skipped here */
		}
		if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
		    (uint64_t) cmds[i].submit_offset + cmds[i].size > b->size) {
			IOLog("HakiosGPU: command buffer %u of a submission: buffer %u, offset %u, %u bytes, %u relocations: not taken\n",
			    i, cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
			IOLockUnlock(gGPULock);
			goto out;
		}
		addresses[count] = b->address + cmds[i].submit_offset;
		words[count++] = cmds[i].size / 4;
	}
	request->fence = hakios_gpu_submit(addresses, words, count);
	/*
	 * A zero fence means the ring is full, so the core is not reading it and has stopped. The stop is
	 * reported; with gpu_recover=1 the core is reset and the submission given again, since the ring is
	 * empty after a reset. (A wait of a second or more usually finds a stopped core first; this case is
	 * for programs that submit without waiting.)
	 */
	if (request->fence == 0 && !gGPUReported) {
		hakios_gpu_report("the ring is full");
		gGPUReported = !hakios_gpu_recover();
		if (!gGPUReported) {
			request->fence = hakios_gpu_submit(addresses, words, count);
		}
	}
	if (gpu_log && request->fence != 0) {
		/* with gpu_log: "tl: Q FENCE at US: N buffers, B of 4 MB or more, W words of commands" per submission */
		uint32_t big = 0, all_words = 0;
		uint64_t ns;

		for (uint32_t i = 0; i < request->nr_bos; i++) {
			struct gpu_buffer * b = buffer(bos[i].handle);

			big += b != NULL && b->size >= (4u << 20);
		}
		for (uint32_t i = 0; i < count; i++) {
			all_words += words[i];
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		hakios_tl("tl: Q %u at %llu: %u buffers, %u big, %u words\n", request->fence, ns / 1000, request->nr_bos, big, all_words);
	}
	if (request->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		request->fence_fd = -1;
	}
	/*
	 * Buffers named here are busy until this submission finishes. MiniIOSurfaceDrawn() waits for that
	 * before a surface is shown. freedreno names every buffer for both reading and writing, so the
	 * direction cannot be told; the last submission of a frame names the surface the frame draws into.
	 */
	for (uint32_t i = 0; request->fence != 0 && i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b != NULL) {
			b->fence = request->fence;
			if (b->surface != NULL) {
				b->surface->drawn = request->fence;
			}
		}
	}
	reap();
	if (fSubmits++ < 4) {
		/* the first is printed only if count != 0: addresses[] and words[] are set only below count */
		IOLog("HakiosGPU: submission %u: %u command buffers, the first at %#llx, %u words\n", request->fence, count,
		    count != 0 ? addresses[0] : 0ull, count != 0 ? words[0] : 0u);
	}
	/* once every five seconds of submissions: a summary log, then the counters are reset */
	gGPUStat.submissions++;
	gGPUStat.commands += count;
	if (gGPUStat.since == 0) {
		gGPUStat.since = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - gGPUStat.since, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t flushing, waiting, taking, worked;
		uint32_t timed, by[7];

		absolutetime_to_nanoseconds(gGPUStat.flushing, &flushing);
		absolutetime_to_nanoseconds(gGPUStat.waiting, &waiting);
		absolutetime_to_nanoseconds(gGPUStat.taking, &taking);
		hakios_gpu_worked(&worked, &timed, by);
		uint64_t making;

		absolutetime_to_nanoseconds(gGPUStat.making, &making);
		IOLog("HakiosGPU: buffers: %u made in %llu us each, %u kept ones given again (%llu MB kept)\n", gGPUStat.made,
		    gGPUStat.made ? making / NSEC_PER_USEC / gGPUStat.made : 0, gGPUStat.given_again, fKeptBytes >> 20);
		IOLog("HakiosGPU: %u submissions in %llu ms: %u command buffers and %u buffers named, %llu KB out of the cache "
		    "in %llu us a submission; the GPU worked %llu us on each of %u (%u under 0.1 ms, %u under 0.25, %u under 0.5, "
		    "%u under 1, %u under 2, %u under 4, %u longer; %llu ms in all); %u waits of %llu us, %u over before they "
		    "began; %u IOSurfaces taken as buffers in %llu us each; %u buffers, %llu MB\n",
		    gGPUStat.submissions, nanos / NSEC_PER_MSEC, gGPUStat.commands, gGPUStat.buffers,
		    (gGPUStat.flushed >> 10) / gGPUStat.submissions, flushing / NSEC_PER_USEC / gGPUStat.submissions,
		    timed ? worked / timed : 0, timed, by[0], by[1], by[2], by[3], by[4], by[5], by[6], worked / 1000,
		    gGPUStat.waits, gGPUStat.waits ? waiting / NSEC_PER_USEC / gGPUStat.waits : 0, gGPUStat.over,
		    gGPUStat.surfaces, gGPUStat.surfaces ? taking / NSEC_PER_USEC / gGPUStat.surfaces : 0, fBufferCount,
		    fBufferBytes >> 20);
		if (gpu_va != 0) {
			/*
			 * Address space summary (gpu_va != 0). "lost" and "refused" count the two ways the allocator fails
			 * to hold addresses it was given; argument buffers and heaps depend on it holding them.
			 */
			uint64_t used, largest, lost;
			uint32_t live, refused;
			unsigned holes, most;

			hakios_gpu_space(&used, &live, &largest, &holes, &most, &lost, &refused);
			IOLog("HakiosGPU: addresses: %llu MB in %u buffers, longest run free %llu MB, %u holes "
			    "(%u at most, of 1024), %llu KB lost, %u allocations refused\n", used >> 20, live,
			    largest >> 20, holes, most, lost >> 10, refused);
		}
		bzero(&gGPUStat, sizeof(gGPUStat));
	}
	IOLockUnlock(gGPULock);
	ret = request->fence != 0 ? kIOReturnSuccess : kIOReturnTimeout;
out:
	if (bos != NULL) {
		IODelete(bos, struct msm_gem_submit_bo, request->nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct msm_gem_submit_cmd, request->nr_cmds);
	}
	return ret;
}

/* Copies the caller's pixels (32 bits each, blue first) to the panel, for tests. */
IOReturn
HakiosGPUUserClient::show(const struct hakios_show * request)
{
	uint64_t size = (uint64_t) request->rowBytes * request->height;
	uint8_t * pixels;
	IOReturn ret = kIOReturnSuccess;

	if (request->width == 0 || request->width > 4096 || request->height == 0 || request->height > 4096 ||
	    request->rowBytes < 4 * request->width) {
		return kIOReturnBadArgument;
	}
	/* rowBytes comes from the caller; without this limit it could request terabytes of wired memory */
	if (size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	pixels = (uint8_t *) IOMallocData(size);
	if (pixels == NULL) {
		return kIOReturnNoMemory;
	}
	if (copyin((user_addr_t) request->pixels, pixels, size) != 0) {
		ret = kIOReturnVMError;
	} else {
		MiniIOMobileFramebufferShow(pixels, request->width, request->height, request->rowBytes);
	}
	IOFreeData(pixels, size);
	return ret;
}

IOReturn
HakiosGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gGPULock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		b->seen = true;
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
HakiosGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct msm_param param;
		struct msm_gem_new gemNew;
		struct msm_gem_info gemInfo;
		struct msm_gem_cpu_prep cpuPrep;
		struct msm_gem_submit submit;
		struct msm_wait_fence waitFence;
		struct msm_gem_madvise madvise;
		struct msm_submitqueue queue;
		struct msm_submitqueue_query query;
		struct drm_gem_close close;
		struct hakios_surface surface;
		struct hakios_show show;
		struct hakios_worked worked;
	} r;
	size_t size = args->structureInputSize;
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;
	uint64_t zero = 0, patience, waitBegan;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!hakios_gpu_ready() && selector != kHakiosShow) {
		return kIOReturnNotReady;                       /* the GPU has been stopped: the run is ending */
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	switch (selector) {
	case kMsmGetParam:
		ret = hakios_gpu_parameter(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case kMsmSetParam:                              /* SET_PARAM names the program, for Linux's lists of GPU users; the other two cases need nothing */
	case kMsmGemCpuFini:
	case kMsmSubmitQueueClose:
		break;
	case kMsmGemNew:
		ret = gemNew(&r.gemNew);
		break;
	case kMsmGemInfo:
		ret = gemInfo(&r.gemInfo);
		break;
	case kMsmGemCpuPrep:                            /* is the GPU done with the buffers: waits for the last submission, up to the caller's timeout;
		 * MSM_PREP_NOSYNC only checks */
		waitBegan = mach_absolute_time();
		IOLockLock(gGPULock);
		patience = (r.cpuPrep.op & MSM_PREP_NOSYNC) ? 0 : millisecondsUntil(&r.cpuPrep.timeout);
		if (!gpuWait(hakios_gpu_last(), patience)) {
			ret = kIOReturnBusy;
			if (patience == 0 && !(r.cpuPrep.op & MSM_PREP_NOSYNC)) {
				waitOver(&r.cpuPrep.timeout, hakios_gpu_last());
			}
		}
		IOLockUnlock(gGPULock);
		slowWait("cpu-prep", waitBegan);
		break;
	case kMsmGemSubmit:
		ret = submit(&r.submit);
		break;
	case kMsmWaitFence:
		waitBegan = mach_absolute_time();
		IOLockLock(gGPULock);
		/* a fence never issued is never done: refuse it, or waiting would make gpuWait() report the GPU
		 * stopped for every client (fences compare with wraparound) */
		if ((int32_t)(r.waitFence.fence - hakios_gpu_last()) > 0) {
			IOLockUnlock(gGPULock);
			ret = kIOReturnBadArgument;
			break;
		}
		patience = millisecondsUntil(&r.waitFence.timeout);
		if (!gpuWait(r.waitFence.fence, patience)) {
			ret = kIOReturnTimeout;
			if (patience == 0) {
				waitOver(&r.waitFence.timeout, r.waitFence.fence);
			}
		}
		IOLockUnlock(gGPULock);
		slowWait("fence", waitBegan);
		break;
	case kMsmGemMadvise:
		r.madvise.retained = 1;
		break;
	case kMsmSubmitQueueNew:
		r.queue.id = ++fNextQueue;
		break;
	case kMsmSubmitQueueQuery:                      /* faults on this client's commands: always reported as zero here */
		if (r.query.data != 0 && r.query.len >= sizeof(zero)) {
			copyout(&zero, (user_addr_t) r.query.data, sizeof(zero));
		}
		break;
	case kDrmGemClose:
		IOLockLock(gGPULock);
		b = buffer(r.close.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			freeBuffer(b);
			reap();
		}
		IOLockUnlock(gGPULock);
		break;
	case kHakiosGPUWorked:
		/*
		 * Called under gGPULock: hakios_gpu_worked() (via gpu_times()) reads the scratch page and advances the
		 * counters that submit() also advances. The output is copied back at the end of this function. A
		 * caller passing fewer bytes than struct hakios_worked gets only that prefix; the rest is lost, since
		 * the read resets the counters.
		 */
		IOLockLock(gGPULock);
		hakios_gpu_worked(&r.worked.microseconds, &r.worked.submissions, r.worked.by);
		IOLockUnlock(gGPULock);
		break;
	case kHakiosSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kHakiosShow:
		ret = show(&r.show);
		break;
	default:
		IOLog("HakiosGPU: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t) size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}
