/*
 * The kernel's side of virtio-gpu 3D (bsd/dev/hakios.vgpu3d.c), on the control queue of the
 * virtio-gpu driver (bsd/dev/hakios.vgpu.c, hakios_vgpu_ctrl), which owns the device.
 *
 * /dev/vgpu3d (a cloning node: every open is its own minor): one virtio-gpu context and one
 * gfxstream pipe ("pipe:opengles", a render thread on the host) per open.
 *
 *   write(fd, buf, n)    the bytes go to the render thread (TRANSFER_TO_HOST_3D, 1 MiB at a time)
 *   read(fd, buf, n)     exactly n bytes of its replies (TRANSFER_FROM_HOST_3D)
 *   VGPU3D_IOC_CREATE    a 2D resource of this context (a gfxstream colour buffer whose handle is
 *                        the resource ID: rcBindTexture etc.), backed by kernel memory, width * 4
 *                        bytes a row
 *   VGPU3D_IOC_UPLOAD    a rectangle of user memory (any stride) into it, then to the host
 *   VGPU3D_IOC_DOWNLOAD  a rectangle from the host, then out to user memory
 *   VGPU3D_IOC_UNREF     a resource away (all of them go with the last close)
 *   VGPU3D_IOC_HOST_SHOWS  whether the host shows a surface itself: in an IOSurface ID, out 1 when
 *                        the host is SCGPU's (below) and the display has had that surface in a swap
 *                        (hakios_vgpu3d_display_surface, from MiniIOMobileFramebuffer). The gfx library
 *                        then ends such a frame with rcFBPost -- in the stream, after its draws -- instead
 *                        of copying it back for the display to copy out again.
 *   VGPU3D_IOC_RING_MARK a frame's pace: in a mark this gave before (0: none), out a mark of everything
 *                        written so far. Waits until the app has run the stream ring past the mark in
 *                        (ENOTSUP without SCGPU's ring). The gfx library gives the mark of the frame
 *                        before at the end of each frame the host shows itself: it draws the next frame
 *                        while the app draws this one, but never gets more than a frame ahead.
 *
 * With scgpu.addr= and scgpu.size= (sys/hakios.scgpu.h) the "device" is the Scion app, which runs
 * gfxstream itself: the same commands go through a mailbox in a region of RAM the app maps too, the
 * pipe's buffer comes from that region's pool, and colour buffers get no backing of their own --
 * UPLOAD and DOWNLOAD pass through a staging buffer of the pool instead (SCGPU_CMD_TRANSFER).
 *
 * The user-space side is the gfx library (userland/src/gfx, gfx_dev.c).
 *
 * Boot-arg gpu3d=1 also runs vgpu3d_selftest() once at boot (gpu3d=2: in a thread): the host
 * GPU draws a picture, the guest reads it back and checks it ("GPU3D ..." lines).
 */
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/ioccom.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
#include <sys/hakios.scgpu.h>
#include <sys/hakios.virtio.h>
#include <sys/hakios.vgpu.h>
#include <sys/hakios.vgpu3d.h>

extern uint64_t kvtophys(vm_offset_t va);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);

struct vgpu3d_ioc_create {
	uint32_t width, height, format, bind;   /* format: VIRGL_FORMAT_*, 0 = B8G8R8A8 */
	uint32_t res;                           /* out */
};
struct vgpu3d_ioc_xfer {
	uint32_t res, x, y, w, h;
	uint32_t stride;                        /* of the user memory, bytes */
	uint64_t addr;                          /* user address of pixel (x, y) */
};
#define VGPU3D_IOC_CREATE       _IOWR('G', 1, struct vgpu3d_ioc_create)
#define VGPU3D_IOC_UNREF        _IOW('G', 2, uint32_t)
#define VGPU3D_IOC_UPLOAD       _IOW('G', 3, struct vgpu3d_ioc_xfer)
#define VGPU3D_IOC_DOWNLOAD     _IOW('G', 4, struct vgpu3d_ioc_xfer)
#define VGPU3D_IOC_HOST_SHOWS   _IOWR('G', 5, uint32_t)
#define VGPU3D_IOC_RING_MARK    _IOWR('G', 6, uint64_t)

/*
 * Opens: a GL context is one, and so is every thread of a process that calls Vulkan (gfxstream's
 * guest Vulkan driver has a stream a thread, plus one a queue and one for the process: each its own
 * context, since gfxstream's host has one pipe a context; tools/gfxstream/VULKAN.md).
 */
#define MAX_OPENS               64
#define MAX_RES                 1024            /* CoreAnimation shares hundreds of IOSurfaces */
#define PIPE_SIZE               (1u << 20)
/*
 * With SCGPU's stream ring the pipe's buffer carries only replies (writes go through the ring, in
 * records of at most this): a quarter of it, so that 64 opens take 16 MiB of the app's pool, not 64.
 */
#define RING_PIPE_SIZE          (256u << 10)
#define MAX_ENTRIES             3000            /* ATTACH_BACKING entries: 48 KiB of command */

struct res {
	uint32_t id, width, height;
	vm_offset_t va;
	vm_size_t size;
};

struct vopen {
	bool used, ready;
	lck_mtx_t lock;
	struct vgpu3d g;
	struct res res[MAX_RES];
	uint32_t next_id;
};

static LCK_GRP_DECLARE(vgpu3d_lck_grp, "hakios-vgpu3d");
static LCK_MTX_DECLARE(vgpu3d_opens_lock, &vgpu3d_lck_grp);
static struct vopen *opens[MAX_OPENS];
/*
 * A minor handed out by vgpu3d_clone() and not opened yet: until this time (mach_absolute_time),
 * no other lookup is given it. devfs asks for the minor under its lock but opens the device after
 * dropping it, so two threads opening /dev/vgpu3d at once were given the same minor, and the
 * second open failed with EBUSY (gfxstream's Vulkan opens a stream a thread). devfs gives a minor
 * back only when making the vnode fails; a lookup that never opens (a stat) leaves its minor
 * reserved, hence the time limit.
 */
static uint64_t reserved_until[MAX_OPENS];
#define RESERVE_MS              2000
/*
 * opens[] slots and reserved_until[]: a lock of its own, never held while waiting, since
 * vgpu3d_clone() runs under devfs's lock (and vgpu3d_opens_lock is held while the GPU answers).
 * Taken inside vgpu3d_opens_lock, never the other way round.
 */
static LCK_MTX_DECLARE(vgpu3d_minor_lock, &vgpu3d_lck_grp);
static int vgpu3d_major = -1;
static uint64_t vgpu3d_features;
static uint32_t vgpu3d_gles = 0xffffffff;       /* the context type: unknown yet */
static uint8_t *attach_scratch;                 /* ATTACH_BACKING commands (under vgpu3d_opens_lock) */

/* ---- SCGPU: the Scion app's GPU (sys/hakios.scgpu.h) ------------------------------------ */

#define SCGPU_CHUNK             0x10000u        /* the pool's unit: 64 KiB */
#define SCGPU_MAX_CHUNKS        8192u           /* 512 MiB of pool at most */
#define SCGPU_SPIN_US           300             /* a reply is waited for by spinning this long, */
#define SCGPU_NAP_US            20              /* then in naps of this, */
#define SCGPU_BELL_SPIN_US      60              /* ... or, with the doorbell, this long and then asleep, */
#define SCGPU_BELL_SAFETY_MS    5               /* looking again at least this often (a lost ring) */
#define SCGPU_TIMEOUT_S         10              /* until this (a first shader compile can take a while) */
#define SCGPU_MARK_NAP_US       250             /* a frame's pace (ring_mark) is looked at this often, */
#define SCGPU_MARK_MAX_MS       1000            /* ... and not waited for longer than this */

static volatile struct scgpu_header *scgpu;     /* NULL: the virtio-gpu device, as before */
static uint8_t *scgpu_va;
static uint64_t scgpu_pa, scgpu_size;
static uint32_t scgpu_chunks;
static uint8_t scgpu_used[SCGPU_MAX_CHUNKS / 8];
static LCK_MTX_DECLARE(scgpu_mailbox_lock, &vgpu3d_lck_grp);
static LCK_MTX_DECLARE(scgpu_pool_lock, &vgpu3d_lck_grp);

/*
 * The doorbell (sys/hakios.scgpu.h): the virtio-input device "scion-scgpu", handed over by the input
 * driver (hakios_scgpu_bell_attach). The app rings by an input event, which comes as an interrupt;
 * the guest rings by a status event, which crosvm writes into the app's socket.
 */
#define BELL_EVENTS             16
#define BELL_STATUS             8
struct bell_event { uint16_t type, code; uint32_t value; };
static vio_dev_t *bell;
static vio_queue_t *bell_eventq, *bell_statusq;
static struct bell_event *bell_events, *bell_status;
static uint64_t bell_events_pa, bell_status_pa;
static unsigned bell_status_next;
static uint64_t bell_rung, bell_heard;
static LCK_MTX_DECLARE(bell_lock, &vgpu3d_lck_grp);     /* the status queue: rung from the mailbox and the ring */
static int scgpu_reply_chan;                    /* (its address: what a waiting thread sleeps on) */

/* After every interrupt of the doorbell: the app rang (each event a ring); the waiter wakes. */
static void
bell_service(__unused void *arg, __unused uint8_t isr)
{
	uint32_t len;
	void *cookie;
	bool any = false;

	while ((cookie = vio_queue_get(bell_eventq, &len)) != NULL) {
		uintptr_t i = (uintptr_t)cookie - 1;
		struct vio_sg sg = { bell_events_pa + i * sizeof(struct bell_event), sizeof(struct bell_event), true };

		vio_queue_add(bell_eventq, &sg, 1, cookie);
		any = true;
		bell_heard++;
	}
	if (any) {
		vio_queue_kick(bell_eventq);
		thread_wakeup((event_t)&scgpu_reply_chan);
	}
}

/* From the virtio-input driver, with the device initialized: the doorbell is ours. */
bool
hakios_scgpu_bell_attach(vio_dev_t *v)
{
	if (vio_queue_setup(v, 0, BELL_EVENTS, &bell_eventq) != 0 || vio_queue_setup(v, 1, BELL_STATUS, &bell_statusq) != 0 ||
	    (bell_events = vio_dma_alloc(BELL_EVENTS * sizeof(struct bell_event), &bell_events_pa)) == NULL ||
	    (bell_status = vio_dma_alloc(BELL_STATUS * sizeof(struct bell_event), &bell_status_pa)) == NULL) {
		printf("vgpu3d: SCGPU: the doorbell could not be set up; polling\n");
		vio_fail(v);
		return false;
	}
	for (uintptr_t i = 0; i < vio_queue_size(bell_eventq); i++) {
		struct vio_sg sg = { bell_events_pa + i * sizeof(struct bell_event), sizeof(struct bell_event), true };

		vio_queue_add(bell_eventq, &sg, 1, (void *)(i + 1));
	}
	vio_intr_enable(v);
	vio_driver_ok(v);
	vio_queue_kick(bell_eventq);
	vio_service_start(v, bell_service, NULL, 4 * NSEC_PER_MSEC);
	bell = v;
	printf("vgpu3d: SCGPU: the doorbell is %s (%s)\n", vio_name(v), vio_intr_active(v) ? "interrupts" : "POLLED");
	return true;
}

/* Ring the app: a status event, which it reads from its socket. */
static void
bell_ring(void)
{
	uint32_t len;

	lck_mtx_lock(&bell_lock);
	while (vio_queue_get(bell_statusq, &len) != NULL) {
	}
	if (vio_queue_num_free(bell_statusq) == 0) {
		lck_mtx_unlock(&bell_lock);
		return;                         /* rings not yet taken: it is awake anyway */
	}
	unsigned i = bell_status_next++ % BELL_STATUS;
	struct vio_sg sg = { bell_status_pa + i * sizeof(struct bell_event), sizeof(struct bell_event), false };

	/* EV_LED: crosvm writes every status event but EV_MSC into the socket */
	bell_status[i] = (struct bell_event){ 0x11, 0, (uint32_t)bell_rung };
	vio_queue_add(bell_statusq, &sg, 1, (void *)(uintptr_t)(i + 1));
	vio_queue_kick(bell_statusq);
	bell_rung++;
	lck_mtx_unlock(&bell_lock);
}

/* ---- the stream ring (sys/hakios.scgpu.h) ---- */

#define SCGPU_RING_SIZE         (4u << 20)
static uint8_t *scgpu_ring;
static uint32_t scgpu_ring_size;
static LCK_MTX_DECLARE(scgpu_ring_lock, &vgpu3d_lck_grp);
static uint64_t ring_waits;

/* Until the ring has room for `need` more bytes past `head`: spin, then asleep on the doorbell. */
static int
ring_room(uint32_t head, uint32_t need)
{
	volatile uint32_t *tailp = &scgpu->ring_tail;
	uint64_t now, spin_until, give_up;

	if (head - __atomic_load_n((uint32_t *)(uintptr_t)tailp, __ATOMIC_ACQUIRE) + need <= scgpu_ring_size) {
		return 0;
	}
	ring_waits++;
	now = mach_absolute_time();
	nanoseconds_to_absolutetime(SCGPU_BELL_SPIN_US * NSEC_PER_USEC, &spin_until);
	nanoseconds_to_absolutetime(SCGPU_TIMEOUT_S * NSEC_PER_SEC, &give_up);
	spin_until += now;
	give_up += now;
	while (head - __atomic_load_n((uint32_t *)(uintptr_t)tailp, __ATOMIC_ACQUIRE) + need > scgpu_ring_size) {
		now = mach_absolute_time();
		if (now > give_up) {
			printf("vgpu3d: SCGPU: the stream ring stayed full for %d s\n", SCGPU_TIMEOUT_S);
			return ETIMEDOUT;
		}
		if (now <= spin_until) {
			continue;
		}
		if (bell == NULL) {
			delay_for_interval(SCGPU_NAP_US, NSEC_PER_USEC);
			continue;
		}
		uint64_t deadline;
		clock_interval_to_deadline(SCGPU_BELL_SAFETY_MS, kMillisecondScale, &deadline);
		assert_wait_deadline((event_t)&scgpu_reply_chan, THREAD_UNINT, deadline);
		__atomic_store_n((uint32_t *)(uintptr_t)&scgpu->guest_waiting, 1, __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (head - __atomic_load_n((uint32_t *)(uintptr_t)tailp, __ATOMIC_ACQUIRE) + need <= scgpu_ring_size) {
			clear_wait(current_thread(), THREAD_AWAKENED);
		} else {
			thread_block(THREAD_CONTINUE_NULL);
		}
		__atomic_store_n((uint32_t *)(uintptr_t)&scgpu->guest_waiting, 0, __ATOMIC_RELAXED);
	}
	return 0;
}

/*
 * VGPU3D_IOC_RING_MARK: until the app has run the ring past `*mark` (a mark from before; 0: none),
 * then a mark of everything put in so far. A mark is (1 << 32) | ring_head. Not an error to give up
 * (a host busy compiling shaders): the ring's room holds the writer back then. In naps, not on the
 * doorbell: the app rings only once it has run the ring dry, and the frame before is usually done
 * long before that.
 */
static uint64_t mark_waits, mark_us, mark_late;
static uint64_t ring_posted;          /* (scgpu_ring_post) */
static uint64_t ring_uploads, ring_upload_bytes;     /* (scgpu_ring_upload) */

static int
ring_mark(uint64_t *mark)
{
	if (scgpu == NULL || scgpu_ring == NULL) {
		return ENOTSUP;
	}
	volatile uint32_t *tailp = &scgpu->ring_tail, *headp = &scgpu->ring_head;
	if (*mark >> 32 == 1) {
		uint32_t until = (uint32_t)*mark;
		uint64_t began = mach_absolute_time(), give_up;

		nanoseconds_to_absolutetime((uint64_t)SCGPU_MARK_MAX_MS * NSEC_PER_MSEC, &give_up);
		give_up += began;
		/* (a head behind the mark: the ring began again since, with the app) */
		while ((int32_t)(__atomic_load_n((uint32_t *)(uintptr_t)tailp, __ATOMIC_ACQUIRE) - until) < 0 &&
		    (int32_t)(__atomic_load_n((uint32_t *)(uintptr_t)headp, __ATOMIC_ACQUIRE) - until) >= 0) {
			if (mach_absolute_time() > give_up) {
				mark_late++;
				break;
			}
			delay_for_interval(SCGPU_MARK_NAP_US, NSEC_PER_USEC);
		}
		uint64_t waited = mach_absolute_time() - began, ns;
		if (waited != 0) {
			absolutetime_to_nanoseconds(waited, &ns);
			mark_us += ns / NSEC_PER_USEC;
		}
		mark_waits++;
		if (mark_waits == 600 || mark_waits % 20000 == 0) {
			printf("vgpu3d: SCGPU: %llu frames paced, %llu us each on average waiting for the frame before; "
			    "%llu given up on; %llu commands and %llu uploads (%llu KB) posted through the ring, not waited "
			    "for\n", mark_waits, mark_us / mark_waits, mark_late, ring_posted, ring_uploads, ring_upload_bytes >> 10);
		}
	}
	*mark = (1ull << 32) | __atomic_load_n((uint32_t *)(uintptr_t)headp, __ATOMIC_ACQUIRE);
	return 0;
}

/* One command through the mailbox, as hakios_vgpu_ctrl puts one on the control queue. */
static int
scgpu_ctrl(const void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len)
{
	volatile uint32_t *resp_seq = &scgpu->resp_seq;
	uint64_t now, spin_until, give_up;
	uint32_t seq;
	int err = 0;

	if (cmd_len > SCGPU_CMD_MAX || resp_len > SCGPU_RESP_MAX) {
		return E2BIG;
	}
	lck_mtx_lock(&scgpu_mailbox_lock);
	memcpy(scgpu_va + SCGPU_CMD_OFF, cmd, cmd_len);
	scgpu->req_len = cmd_len;
	scgpu->resp_max = resp_len;
	seq = scgpu->req_seq + 1;
	__atomic_store_n((uint32_t *)(uintptr_t)&scgpu->req_seq, seq, __ATOMIC_RELEASE);
	/* the app asleep on the doorbell is rung (a full barrier first: the other half of its host_sleeping) */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (bell != NULL && __atomic_load_n((uint32_t *)(uintptr_t)&scgpu->host_sleeping, __ATOMIC_RELAXED)) {
		bell_ring();
	}
	now = mach_absolute_time();
	nanoseconds_to_absolutetime((bell != NULL ? SCGPU_BELL_SPIN_US : SCGPU_SPIN_US) * NSEC_PER_USEC, &spin_until);
	nanoseconds_to_absolutetime(SCGPU_TIMEOUT_S * NSEC_PER_SEC, &give_up);
	spin_until += now;
	give_up += now;
	while (__atomic_load_n((uint32_t *)(uintptr_t)resp_seq, __ATOMIC_ACQUIRE) != seq) {
		now = mach_absolute_time();
		if (now > give_up) {
			printf("vgpu3d: SCGPU: no reply to command %#x in %d s\n", *(const uint32_t *)cmd, SCGPU_TIMEOUT_S);
			err = ETIMEDOUT;
			break;
		}
		if (now <= spin_until) {
			continue;
		}
		if (bell == NULL) {
			delay_for_interval(SCGPU_NAP_US, NSEC_PER_USEC);
			continue;
		}
		/* asleep until the app rings: say so, then look once more, so one of the two sees the other */
		uint64_t deadline;
		clock_interval_to_deadline(SCGPU_BELL_SAFETY_MS, kMillisecondScale, &deadline);
		assert_wait_deadline((event_t)&scgpu_reply_chan, THREAD_UNINT, deadline);
		__atomic_store_n((uint32_t *)(uintptr_t)&scgpu->guest_waiting, 1, __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (__atomic_load_n((uint32_t *)(uintptr_t)resp_seq, __ATOMIC_ACQUIRE) == seq) {
			clear_wait(current_thread(), THREAD_AWAKENED);
		} else {
			thread_block(THREAD_CONTINUE_NULL);
		}
		__atomic_store_n((uint32_t *)(uintptr_t)&scgpu->guest_waiting, 0, __ATOMIC_RELAXED);
		/* (a ring that found no free status descriptor: the app may still be asleep on it) */
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (__atomic_load_n((uint32_t *)(uintptr_t)&scgpu->host_sleeping, __ATOMIC_RELAXED) &&
		    __atomic_load_n((uint32_t *)(uintptr_t)resp_seq, __ATOMIC_ACQUIRE) != seq) {
			bell_ring();
		}
	}
	if (!err && scgpu->status != 0) {
		err = EIO;
	}
	if (!err && resp_len) {
		uint32_t n = scgpu->resp_len < resp_len ? scgpu->resp_len : resp_len;
		memcpy(resp, scgpu_va + SCGPU_RESP_OFF, n);
		if (n < resp_len) {
			bzero((uint8_t *)resp + n, resp_len - n);
		}
	}
	lck_mtx_unlock(&scgpu_mailbox_lock);
	return err;
}

/* n bytes of the pool, contiguous: the address here, and *pa the guest's for the app. */
static void *
scgpu_alloc(size_t size, uint64_t *pa)
{
	uint32_t n = (uint32_t)((size + SCGPU_CHUNK - 1) / SCGPU_CHUNK), run = 0;

	lck_mtx_lock(&scgpu_pool_lock);
	for (uint32_t i = 0; n && i < scgpu_chunks; i++) {
		if (scgpu_used[i / 8] & (1u << (i % 8))) {
			run = 0;
			continue;
		}
		if (++run == n) {
			uint32_t first = i + 1 - n;
			for (uint32_t k = first; k <= i; k++) {
				scgpu_used[k / 8] |= (uint8_t)(1u << (k % 8));
			}
			lck_mtx_unlock(&scgpu_pool_lock);
			uint64_t off = SCGPU_POOL_OFF + (uint64_t)first * SCGPU_CHUNK;
			*pa = scgpu_pa + off;
			return scgpu_va + off;
		}
	}
	lck_mtx_unlock(&scgpu_pool_lock);
	printf("vgpu3d: SCGPU: the pool has no %zu bytes in one piece\n", size);
	return NULL;
}

static void
scgpu_free(void *va, size_t size)
{
	uint32_t first = (uint32_t)(((uint8_t *)va - scgpu_va - SCGPU_POOL_OFF) / SCGPU_CHUNK);
	uint32_t n = (uint32_t)((size + SCGPU_CHUNK - 1) / SCGPU_CHUNK);

	lck_mtx_lock(&scgpu_pool_lock);
	for (uint32_t k = first; k < first + n && k < scgpu_chunks; k++) {
		scgpu_used[k / 8] &= (uint8_t)~(1u << (k % 8));
	}
	lck_mtx_unlock(&scgpu_pool_lock);
}

/* scgpu.addr= scgpu.size=: the app's region, mapped as ordinary (cached) memory. */
static void
scgpu_attach(void)
{
	uint64_t pa = 0, size = 0;

	if (!PE_parse_boot_argn("scgpu.addr", &pa, sizeof(pa)) || pa == 0) {
		return;
	}
	(void)PE_parse_boot_argn("scgpu.size", &size, sizeof(size));
	if ((pa & PAGE_MASK) || size < SCGPU_POOL_OFF + SCGPU_CHUNK) {
		printf("vgpu3d: scgpu.addr=%#llx scgpu.size=%#llx: not a region\n", pa, size);
		return;
	}
	scgpu_va = (uint8_t *)ml_io_map_unmappable((vm_offset_t)pa, (vm_size_t)round_page(size), VM_WIMG_DEFAULT);
	scgpu = (volatile struct scgpu_header *)scgpu_va;
	if (scgpu == NULL || scgpu->magic != SCGPU_MAGIC || scgpu->version != SCGPU_VERSION) {
		printf("vgpu3d: SCGPU at %#llx: no header from the app (magic %#x)\n", pa, scgpu ? scgpu->magic : 0);
		scgpu = NULL;
		return;
	}
	scgpu_pa = pa;
	scgpu_size = size;
	scgpu_chunks = (uint32_t)((size - SCGPU_POOL_OFF) / SCGPU_CHUNK);
	if (scgpu_chunks > SCGPU_MAX_CHUNKS) {
		scgpu_chunks = SCGPU_MAX_CHUNKS;
	}
	/* the stream ring, from the pool; the app finds it in the header (no ring: every write by mailbox) */
	uint64_t ring_pa;
	scgpu_ring = scgpu->flags & SCGPU_F_RING ? scgpu_alloc(SCGPU_RING_SIZE, &ring_pa) : NULL;
	if (scgpu_ring != NULL) {
		scgpu->ring_off = (uint32_t)(ring_pa - scgpu_pa);
		scgpu->ring_head = scgpu->ring_tail = 0;
		scgpu_ring_size = SCGPU_RING_SIZE;
		__atomic_store_n((uint32_t *)(uintptr_t)&scgpu->ring_size, SCGPU_RING_SIZE, __ATOMIC_RELEASE);
	}
	printf("vgpu3d: SCGPU: the app's GPU at %#llx (%llu MiB, a pool of %u KiB, a stream ring of %u KiB%s), %s\n", pa,
	    size >> 20, scgpu_chunks * (SCGPU_CHUNK >> 10), scgpu_ring_size >> 10,
	    scgpu_ring != NULL && (scgpu->flags & SCGPU_F_RING_CMDS) ? " that takes commands too" : "",
	    scgpu->flags & SCGPU_F_HOST_READY ? "ready" : "NOT READY");
}

/*
 * A command posted into the stream ring (SCGPU_REC_CMD, the app's SCGPU_F_RING_CMDS): run by the app in
 * order with the stream, its reply dropped; the guest waits for nothing but room. ENOTSUP: use the
 * mailbox.
 */
static int
scgpu_ring_post(const void *cmd, uint32_t len)
{
	uint32_t mask = scgpu_ring_size - 1, need = (uint32_t)sizeof(struct scgpu_record) + ((len + 15) & ~15u);
	int err;

	if (scgpu == NULL || scgpu_ring == NULL || !(scgpu->flags & SCGPU_F_RING_CMDS) || len == 0 ||
	    need > scgpu_ring_size / 4) {
		return ENOTSUP;
	}
	lck_mtx_lock(&scgpu_ring_lock);
	uint32_t head = scgpu->ring_head;       /* (ours alone) */
	if ((err = ring_room(head, need)) == 0) {
		struct scgpu_record rec = { 0, 0, len, SCGPU_REC_CMD };
		uint32_t pos = head & mask;
		memcpy(scgpu_ring + pos, &rec, sizeof(rec));
		uint32_t at = (pos + (uint32_t)sizeof(rec)) & mask;
		uint32_t first = len < scgpu_ring_size - at ? len : scgpu_ring_size - at;
		memcpy(scgpu_ring + at, cmd, first);
		memcpy(scgpu_ring, (const uint8_t *)cmd + first, len - first);
		__atomic_store_n((uint32_t *)(uintptr_t)&scgpu->ring_head, head + need, __ATOMIC_RELEASE);
		ring_posted++;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (bell != NULL && __atomic_load_n((uint32_t *)(uintptr_t)&scgpu->host_sleeping, __ATOMIC_RELAXED)) {
			bell_ring();
		}
	}
	lck_mtx_unlock(&scgpu_ring_lock);
	return err;
}

/*
 * An upload posted into the ring (SCGPU_F_RING_CMDS): a SCGPU_CMD_TRANSFER with no staging buffer,
 * its rows right after it in the record, at the resource's pitch, copied in from user memory here.
 * The guest goes on at once; the app uploads in order with the stream. Only up to a quarter of the
 * ring (ENOTSUP for more: the mailbox and a staging buffer, as before).
 */
static int
scgpu_ring_upload(struct vopen *o, struct res *r, struct vgpu3d_ioc_xfer *x)
{
	uint32_t mask = scgpu_ring_size - 1;
	uint64_t row = (uint64_t)x->w * 4, pitch = (uint64_t)r->width * 4;
	uint64_t len = (uint64_t)(x->h - 1) * pitch + row, total = sizeof(struct scgpu_transfer) + len;
	struct scgpu_transfer t;
	int err;

	if (scgpu == NULL || scgpu_ring == NULL || !(scgpu->flags & SCGPU_F_RING_CMDS) ||
	    sizeof(struct scgpu_record) + ((total + 15) & ~15ull) > scgpu_ring_size / 4) {
		return ENOTSUP;
	}
	uint32_t need = (uint32_t)(sizeof(struct scgpu_record) + ((total + 15) & ~15ull));
	bzero(&t, sizeof(t));
	t.type = SCGPU_CMD_TRANSFER;
	t.ctx_id = o->g.ctx_id;
	t.resource_id = r->id;
	t.to_host = 1;
	t.x = x->x;
	t.y = x->y;
	t.w = x->w;
	t.h = x->h;
	t.offset = (uint64_t)x->y * pitch + (uint64_t)x->x * 4;
	t.staging = 0;                          /* (the rows follow) */
	t.staging_len = len;
	lck_mtx_lock(&scgpu_ring_lock);
	uint32_t head = scgpu->ring_head;       /* (ours alone) */
	if ((err = ring_room(head, need)) == 0) {
		struct scgpu_record rec = { 0, 0, (uint32_t)total, SCGPU_REC_CMD };
		uint32_t pos = head & mask;
		memcpy(scgpu_ring + pos, &rec, sizeof(rec));
		uint32_t at = (pos + (uint32_t)sizeof(rec)) & mask;
		uint32_t first = (uint32_t)sizeof(t) < scgpu_ring_size - at ? (uint32_t)sizeof(t) : scgpu_ring_size - at;
		memcpy(scgpu_ring + at, &t, first);
		memcpy(scgpu_ring, (const uint8_t *)&t + first, sizeof(t) - first);
		at = (at + (uint32_t)sizeof(t)) & mask;
		for (uint32_t y = 0; y < x->h && !err; y++) {
			uint32_t to = (at + (uint32_t)(y * pitch)) & mask;
			uint32_t part = row < scgpu_ring_size - to ? (uint32_t)row : scgpu_ring_size - to;
			user_addr_t from = (user_addr_t)(x->addr + (uint64_t)y * x->stride);
			err = copyin(from, scgpu_ring + to, part);
			if (!err && part < row) {
				err = copyin(from + part, scgpu_ring, row - part);
			}
		}
		if (!err) {
			/* (published only whole: a fault half way leaves the ring as it was) */
			__atomic_store_n((uint32_t *)(uintptr_t)&scgpu->ring_head, head + need, __ATOMIC_RELEASE);
			ring_uploads++;
			ring_upload_bytes += len;
			__atomic_thread_fence(__ATOMIC_SEQ_CST);
			if (bell != NULL && __atomic_load_n((uint32_t *)(uintptr_t)&scgpu->host_sleeping, __ATOMIC_RELAXED)) {
				bell_ring();
			}
		}
	}
	lck_mtx_unlock(&scgpu_ring_lock);
	return err;
}

/* ---- the ops ---------------------------------------------------------------------------- */

static int
xnu_ctrl(__unused void *arg, const void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len)
{
	if (scgpu != NULL) {
		return scgpu_ctrl(cmd, cmd_len, resp, resp_len);
	}
	return hakios_vgpu_ctrl(cmd, cmd_len, resp, resp_len);
}

static void *
xnu_dma_alloc(__unused void *arg, size_t size, uint64_t *pa)
{
	vm_offset_t va = 0;
	if (scgpu != NULL) {
		void *p = scgpu_alloc(size, pa);
		if (p != NULL) {
			bzero(p, size);
		}
		return p;
	}
	if (kmem_alloc_contig(kernel_map, &va, round_page(size), PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	*pa = kvtophys(va);
	return (void *)va;
}

/* What xnu_dma_alloc gave. */
static void
xnu_dma_free(void *va, size_t size)
{
	if (scgpu != NULL) {
		scgpu_free(va, size);
	} else {
		kmem_free(kernel_map, (vm_offset_t)va, round_page(size));
	}
}

static void
xnu_log(__unused void *arg, const char *line)
{
	printf("%s\n", line);
}

static uint64_t
xnu_now_ns(__unused void *arg)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

static int
xnu_post(__unused void *arg, const void *cmd, uint32_t cmd_len)
{
	return scgpu_ring_post(cmd, cmd_len);
}

static const struct vgpu3d_ops xnu_ops = { NULL, xnu_ctrl, xnu_dma_alloc, xnu_log, xnu_now_ns, xnu_post };

/*
 * Once: bring the device up through the 2D driver, see that it does 3D, and pick the context
 * type (gfxstream-gles when offered, else the host's default).
 */
static int
vgpu3d_probe(void)
{
	static struct vgpu3d q;
	struct virtio_gpu_ctrl_hdr get = { .type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO };
	static struct virtio_gpu_resp_display_info info;

	if (vgpu3d_gles != 0xffffffff) {
		return vgpu3d_gles == 0xfffffffe ? ENODEV : 0;
	}
	vgpu3d_gles = 0xfffffffe;
	if (scgpu != NULL) {
		/* the app's renderer: its features and capsets are in the header */
		if (!(scgpu->flags & SCGPU_F_HOST_READY)) {
			printf("vgpu3d: SCGPU: the app's renderer is not up\n");
			return ENODEV;
		}
		vgpu3d_features = (uint64_t)scgpu->features_hi << 32 | scgpu->features_lo;
	} else if (hakios_vgpu_ctrl(&get, sizeof(get), &info, sizeof(info)) != 0) {
		printf("vgpu3d: no virtio-gpu device (does the VM have a GpuConfig?)\n");
		return ENODEV;
	} else {
		vgpu3d_features = hakios_vgpu_features();
	}
	q.ops = xnu_ops;
	if (vgpu3d_query(&q, vgpu3d_features, scgpu != NULL && scgpu->num_capsets ? scgpu->num_capsets :
	    VGPU3D_PROBE_CAPSETS) != 0) {
		return ENODEV;
	}
	vgpu3d_gles = vgpu3d_has_capset(&q, VIRTIO_GPU_CAPSET_GFXSTREAM_GLES) ? VIRTIO_GPU_CAPSET_GFXSTREAM_GLES : 0;
	attach_scratch = IOMallocData(sizeof(struct virtio_gpu_resource_attach_backing) +
	    MAX_ENTRIES * sizeof(struct virtio_gpu_mem_entry));
	return attach_scratch ? 0 : ENOMEM;
}

/* ---- resources -------------------------------------------------------------------------- */

static struct res *
res_find(struct vopen *o, uint32_t id)
{
	for (int i = 0; i < MAX_RES; i++) {
		if (o->res[i].id == id && id) {
			return &o->res[i];
		}
	}
	return NULL;
}

static void
res_free(struct vopen *o, struct res *r)
{
	vgpu3d_resource_unref(&o->g, r->id);
	o->g.err = 0;
	if (r->va) {
		kmem_free(kernel_map, r->va, r->size);
	}
	bzero(r, sizeof(*r));
}

/*
 * SCGPU: a resource with no backing of its own (ATTACH_BACKING with no entries); its transfers go
 * through the pool (scgpu_xfer).
 */
static int
scgpu_res_create(struct vopen *o, struct res *r, struct vgpu3d_ioc_create *c)
{
	int err;

	r->id = o->next_id++;
	r->width = c->width;
	r->height = c->height;
	lck_mtx_lock(&vgpu3d_opens_lock);
	err = vgpu3d_resource_create(&o->g, r->id, c->width, c->height,
	    c->format ? c->format : VIRGL_FORMAT_B8G8R8A8_UNORM,
	    c->bind ? c->bind : VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW, NULL, 0, attach_scratch,
	    (uint32_t)(sizeof(struct virtio_gpu_resource_attach_backing) + MAX_ENTRIES * sizeof(struct virtio_gpu_mem_entry)));
	lck_mtx_unlock(&vgpu3d_opens_lock);
	if (err) {
		o->g.err = 0;
		bzero(r, sizeof(*r));
		return EIO;
	}
	c->res = r->id;
	return 0;
}

/* SCGPU: a rectangle between user memory and the host, through a staging buffer of the pool. */
static int
scgpu_xfer(struct vopen *o, struct res *r, struct vgpu3d_ioc_xfer *x, bool upload)
{
	size_t row = (size_t)x->w * 4, pitch = (size_t)r->width * 4;
	size_t len = (size_t)(x->h - 1) * pitch + row;
	struct scgpu_transfer t;
	struct virtio_gpu_ctrl_hdr reply;
	uint64_t pa;
	uint8_t *stage = scgpu_alloc(len, &pa);
	int err = 0;

	if (stage == NULL) {
		return ENOMEM;
	}
	if (upload) {
		for (uint32_t y = 0; y < x->h && !err; y++) {
			err = copyin((user_addr_t)(x->addr + (uint64_t)y * x->stride), stage + y * pitch, row);
		}
	}
	if (!err) {
		bzero(&t, sizeof(t));
		t.type = SCGPU_CMD_TRANSFER;
		t.ctx_id = o->g.ctx_id;
		t.resource_id = r->id;
		t.to_host = upload;
		t.x = x->x;
		t.y = x->y;
		t.w = x->w;
		t.h = x->h;
		t.offset = (uint64_t)x->y * pitch + (uint64_t)x->x * 4;
		t.staging = pa;
		t.staging_len = len;
		if (scgpu_ctrl(&t, sizeof(t), &reply, sizeof(reply)) || reply.type != VIRTIO_GPU_RESP_OK_NODATA) {
			err = EIO;
		}
	}
	if (!upload) {
		for (uint32_t y = 0; y < x->h && !err; y++) {
			err = copyout(stage + y * pitch, (user_addr_t)(x->addr + (uint64_t)y * x->stride), row);
		}
	}
	scgpu_free(stage, len);
	return err;
}

/*
 * The surfaces the display has had in its swaps: CoreAnimation's frame buffers, two or three of them,
 * the same ones every frame. The display's driver tells (any layer of any swap); the gfx library asks.
 */
#define DISPLAY_SURFACES        8
static uint32_t display_surfaces[DISPLAY_SURFACES];
static unsigned display_next;
/*
 * ... and of those, the ones the gfx library has been told the host shows: from then on its frames
 * in them go out by rcFBPost and their memory here is stale, so the display's driver neither waits for
 * them nor copies them to the panel (hakios_vgpu3d_host_owns).
 */
static uint32_t host_owned[DISPLAY_SURFACES];

void
hakios_vgpu3d_display_surface(uint32_t sid)
{
	for (unsigned i = 0; i < DISPLAY_SURFACES; i++) {
		if (__atomic_load_n(&display_surfaces[i], __ATOMIC_RELAXED) == sid) {
			return;
		}
	}
	unsigned slot = __atomic_fetch_add(&display_next, 1, __ATOMIC_RELAXED) % DISPLAY_SURFACES;
	__atomic_store_n(&display_surfaces[slot], sid, __ATOMIC_RELAXED);
}

static int
host_shows(uint32_t *sid)
{
	uint32_t want = *sid;

	*sid = 0;
	for (unsigned i = 0; scgpu != NULL && want != 0 && i < DISPLAY_SURFACES; i++) {
		if (__atomic_load_n(&display_surfaces[i], __ATOMIC_RELAXED) == want) {
			*sid = 1;
		}
	}
	if (*sid) {
		bool known = false;
		for (unsigned i = 0; i < DISPLAY_SURFACES && !known; i++) {
			known = __atomic_load_n(&host_owned[i], __ATOMIC_RELAXED) == want;
		}
		if (!known) {
			for (unsigned i = 0; i < DISPLAY_SURFACES; i++) {
				uint32_t none = 0;
				if (__atomic_compare_exchange_n(&host_owned[i], &none, want, false, __ATOMIC_RELAXED,
				    __ATOMIC_RELAXED)) {
					printf("vgpu3d: SCGPU: surface %u is shown by the host from now on\n", want);
					/* the app may put its renderer's window in front now */
					__atomic_fetch_or((uint32_t *)(uintptr_t)&scgpu->guest_flags, SCGPU_G_PRESENTING,
					    __ATOMIC_RELEASE);
					break;
				}
			}
		}
	}
	return 0;
}

bool
hakios_vgpu3d_host_owns(uint32_t sid)
{
	for (unsigned i = 0; sid != 0 && i < DISPLAY_SURFACES; i++) {
		if (__atomic_load_n(&host_owned[i], __ATOMIC_RELAXED) == sid) {
			return true;
		}
	}
	return false;
}

static int
res_create(struct vopen *o, struct vgpu3d_ioc_create *c)
{
	struct res *r = res_find(o, 0);
	struct virtio_gpu_mem_entry *e;
	uint32_t n = 0;
	int err;

	for (int i = 0; r == NULL && i < MAX_RES; i++) {
		if (o->res[i].id == 0) {
			r = &o->res[i];
		}
	}
	if (r == NULL || c->width == 0 || c->height == 0 || c->width > 8192 || c->height > 8192) {
		return r == NULL ? ENOSPC : EINVAL;
	}
	if (scgpu != NULL) {
		return scgpu_res_create(o, r, c);
	}
	r->size = round_page((vm_size_t)c->width * c->height * 4);
	if (kmem_alloc(kernel_map, &r->va, r->size, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		r->va = 0;
		return ENOMEM;
	}
	lck_mtx_lock(&vgpu3d_opens_lock);
	e = (struct virtio_gpu_mem_entry *)(attach_scratch + sizeof(struct virtio_gpu_resource_attach_backing));
	for (vm_size_t off = 0; off < r->size; off += PAGE_SIZE) {
		uint64_t pa = kvtophys(r->va + off);
		if (n && e[n - 1].addr + e[n - 1].length == pa) {
			e[n - 1].length += PAGE_SIZE;
		} else if (n < MAX_ENTRIES) {
			e[n].addr = pa;
			e[n].length = PAGE_SIZE;
			e[n].padding = 0;
			n++;
		} else {
			lck_mtx_unlock(&vgpu3d_opens_lock);
			kmem_free(kernel_map, r->va, r->size);
			bzero(r, sizeof(*r));
			return E2BIG;
		}
	}
	r->id = o->next_id++;
	r->width = c->width;
	r->height = c->height;
	/* the entries go into the scratch at the place the command wants them */
	struct virtio_gpu_mem_entry *ents = IOMallocData(n * sizeof(*ents));
	if (ents == NULL) {
		lck_mtx_unlock(&vgpu3d_opens_lock);
		kmem_free(kernel_map, r->va, r->size);
		bzero(r, sizeof(*r));
		return ENOMEM;
	}
	memcpy(ents, e, n * sizeof(*ents));
	err = vgpu3d_resource_create(&o->g, r->id, c->width, c->height,
	    c->format ? c->format : VIRGL_FORMAT_B8G8R8A8_UNORM,
	    c->bind ? c->bind : VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW, ents, n, attach_scratch,
	    (uint32_t)(sizeof(struct virtio_gpu_resource_attach_backing) + MAX_ENTRIES * sizeof(*ents)));
	lck_mtx_unlock(&vgpu3d_opens_lock);
	IOFreeData(ents, n * sizeof(*ents));
	if (err) {
		o->g.err = 0;
		kmem_free(kernel_map, r->va, r->size);
		bzero(r, sizeof(*r));
		return EIO;
	}
	c->res = r->id;
	return 0;
}

/* A rectangle between user memory and the backing, and the matching transfer. */
static int
res_xfer(struct vopen *o, struct vgpu3d_ioc_xfer *x, bool upload)
{
	struct res *r = res_find(o, x->res);
	int err = 0;

	if (r == NULL) {
		return ENOENT;
	}
	if (x->w == 0 || x->h == 0 || x->x >= r->width || x->y >= r->height || x->w > r->width - x->x ||
	    x->h > r->height - x->y || x->stride < x->w * 4) {
		return EINVAL;
	}
	if (scgpu != NULL) {
		/* (an upload into the ring, not waited for, when it fits; the mailbox otherwise) */
		int posted = upload ? scgpu_ring_upload(o, r, x) : ENOTSUP;
		return posted == ENOTSUP ? scgpu_xfer(o, r, x, upload) : posted;
	}
	size_t row = (size_t)x->w * 4, pitch = (size_t)r->width * 4;
	uint64_t offset = (uint64_t)x->y * pitch + (uint64_t)x->x * 4;
	uint8_t *base = (uint8_t *)r->va + offset;

	if (upload) {
		for (uint32_t y = 0; y < x->h && !err; y++) {
			err = copyin((user_addr_t)(x->addr + (uint64_t)y * x->stride), base + y * pitch, row);
		}
		if (!err && vgpu3d_transfer_box(&o->g, r->id, true, x->x, x->y, x->w, x->h, offset)) {
			o->g.err = 0;
			err = EIO;
		}
	} else {
		if (vgpu3d_transfer_box(&o->g, r->id, false, x->x, x->y, x->w, x->h, offset)) {
			o->g.err = 0;
			return EIO;
		}
		for (uint32_t y = 0; y < x->h && !err; y++) {
			err = copyout(base + y * pitch, (user_addr_t)(x->addr + (uint64_t)y * x->stride), row);
		}
	}
	return err;
}

/* ---- the device --------------------------------------------------------------------------- */

static int
vgpu3d_clone(__unused dev_t dev, int action)
{
	if (action == DEVFS_CLONE_ALLOC) {
		uint64_t now = mach_absolute_time(), hold;
		int m = -1;

		nanoseconds_to_absolutetime(RESERVE_MS * NSEC_PER_MSEC, &hold);
		lck_mtx_lock(&vgpu3d_minor_lock);
		for (int i = 0; i < MAX_OPENS; i++) {
			if (opens[i] == NULL && reserved_until[i] <= now) {
				reserved_until[i] = now + hold;
				m = i;
				break;
			}
		}
		lck_mtx_unlock(&vgpu3d_minor_lock);
		return m;
	}
	return 0;
}

static int
vgpu3d_dev_open(dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	int m = minor(dev);
	struct vopen *o;
	int err;

	if (m < 0 || m >= MAX_OPENS) {
		return ENXIO;
	}
	lck_mtx_lock(&vgpu3d_opens_lock);
	err = vgpu3d_probe();
	o = err == 0 ? IOMallocZeroData(sizeof(*o)) : NULL;
	if (err == 0 && o == NULL) {
		err = ENOMEM;
	}
	if (err == 0) {
		lck_mtx_lock(&vgpu3d_minor_lock);
		if (opens[m] != NULL) {
			err = EBUSY;
		} else {
			opens[m] = o;
			reserved_until[m] = 0;
		}
		lck_mtx_unlock(&vgpu3d_minor_lock);
		if (err) {
			IOFreeData(o, sizeof(*o));
		}
	}
	lck_mtx_unlock(&vgpu3d_opens_lock);
	if (err) {
		return err;
	}
	lck_mtx_init(&o->lock, &vgpu3d_lck_grp, LCK_ATTR_NULL);
	o->g.ops = xnu_ops;
	o->g.features = vgpu3d_features;
	o->g.ctx_id = (uint32_t)m + 2;                            /* 1: the boot self-test's */
	o->g.next_resource = 0x20000 + ((uint32_t)m << 12);       /* the pipe */
	o->next_id = o->g.next_resource + 1;                      /* resources: 4095 a minor */
	if (vgpu3d_open(&o->g, vgpu3d_gles, scgpu != NULL && scgpu_ring != NULL ? RING_PIPE_SIZE : PIPE_SIZE) != 0) {
		printf("vgpu3d: /dev/vgpu3d minor %d: no pipe (%d)\n", m, o->g.err);
		vgpu3d_close(&o->g);
		lck_mtx_lock(&vgpu3d_opens_lock);
		lck_mtx_lock(&vgpu3d_minor_lock);
		opens[m] = NULL;
		lck_mtx_unlock(&vgpu3d_minor_lock);
		lck_mtx_unlock(&vgpu3d_opens_lock);
		lck_mtx_destroy(&o->lock, &vgpu3d_lck_grp);
		IOFreeData(o, sizeof(*o));
		return EIO;
	}
	o->ready = true;
	o->used = true;
	return 0;
}

static int
vgpu3d_dev_close(dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	int m = minor(dev);
	struct vopen *o = m >= 0 && m < MAX_OPENS ? opens[m] : NULL;

	if (o == NULL) {
		return 0;
	}
	lck_mtx_lock(&o->lock);
	for (int i = 0; i < MAX_RES; i++) {
		if (o->res[i].id) {
			res_free(o, &o->res[i]);
		}
	}
	vgpu3d_close(&o->g);
	if (o->g.pipe_va) {
		xnu_dma_free(o->g.pipe_va, o->g.pipe_size);
	}
	lck_mtx_unlock(&o->lock);
	lck_mtx_lock(&vgpu3d_opens_lock);
	lck_mtx_lock(&vgpu3d_minor_lock);
	opens[m] = NULL;
	lck_mtx_unlock(&vgpu3d_minor_lock);
	lck_mtx_unlock(&vgpu3d_opens_lock);
	lck_mtx_destroy(&o->lock, &vgpu3d_lck_grp);
	IOFreeData(o, sizeof(*o));
	return 0;
}

static struct vopen *
open_of(dev_t dev)
{
	int m = minor(dev);
	struct vopen *o = m >= 0 && m < MAX_OPENS ? opens[m] : NULL;
	return o && o->ready ? o : NULL;
}

/*
 * SCGPU's write: the bytes into the stream ring as records of the context's pipe, a pipe's worth at
 * most each (gfxstream copies a record through the pipe's linear copy), and back at once; the app is
 * rung if it sleeps. Only room is waited for.
 */
static int
scgpu_ring_write(struct vopen *o, struct uio *uio)
{
	uint32_t mask = scgpu_ring_size - 1;
	int err = 0;

	lck_mtx_lock(&scgpu_ring_lock);
	while (uio_resid(uio) > 0 && !err) {
		uint32_t n = uio_resid(uio) > (user_ssize_t)o->g.pipe_size ? o->g.pipe_size : (uint32_t)uio_resid(uio);
		uint32_t need = (uint32_t)sizeof(struct scgpu_record) + ((n + 15) & ~15u);
		uint32_t head = scgpu->ring_head;       /* (ours alone) */

		if ((err = ring_room(head, need)) != 0) {
			break;
		}
		struct scgpu_record rec = { o->g.pipe_res, o->g.ctx_id, n, 0 };
		uint32_t pos = head & mask;             /* 16-aligned in a ring of a multiple of 16: the record never splits */
		memcpy(scgpu_ring + pos, &rec, sizeof(rec));
		uint32_t at = (pos + (uint32_t)sizeof(rec)) & mask;
		uint32_t first = n < scgpu_ring_size - at ? n : scgpu_ring_size - at;
		err = uiomove((caddr_t)(scgpu_ring + at), (int)first, uio);
		if (!err && n > first) {
			err = uiomove((caddr_t)scgpu_ring, (int)(n - first), uio);
		}
		if (err) {
			break;
		}
		__atomic_store_n((uint32_t *)(uintptr_t)&scgpu->ring_head, head + need, __ATOMIC_RELEASE);
		o->g.bytes_out += n;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (bell != NULL && __atomic_load_n((uint32_t *)(uintptr_t)&scgpu->host_sleeping, __ATOMIC_RELAXED)) {
			bell_ring();
		}
	}
	lck_mtx_unlock(&scgpu_ring_lock);
	return err;
}

static int
vgpu3d_dev_write(dev_t dev, struct uio *uio, __unused int flag)
{
	struct vopen *o = open_of(dev);
	int err = 0;

	if (o == NULL) {
		return ENXIO;
	}
	lck_mtx_lock(&o->lock);
	if (scgpu != NULL && scgpu_ring != NULL) {
		err = scgpu_ring_write(o, uio);
		lck_mtx_unlock(&o->lock);
		return err;
	}
	while (uio_resid(uio) > 0 && !err) {
		uint32_t n = uio_resid(uio) > (user_ssize_t)o->g.pipe_size ? o->g.pipe_size : (uint32_t)uio_resid(uio);
		err = uiomove((caddr_t)o->g.pipe_va, (int)n, uio);
		if (!err) {
			o->g.wpos = n;
			if (vgpu3d_flush(&o->g)) {
				err = EIO;
			}
		}
	}
	lck_mtx_unlock(&o->lock);
	return err;
}

static int
vgpu3d_dev_read(dev_t dev, struct uio *uio, __unused int flag)
{
	struct vopen *o = open_of(dev);
	int err = 0;

	if (o == NULL) {
		return ENXIO;
	}
	lck_mtx_lock(&o->lock);
	while (uio_resid(uio) > 0 && !err) {
		uint32_t n = uio_resid(uio) > (user_ssize_t)o->g.pipe_size ? o->g.pipe_size : (uint32_t)uio_resid(uio);
		if (vgpu3d_transfer_box(&o->g, o->g.pipe_res, false, 0, 0, n, 1, 0)) {
			err = EIO;
			break;
		}
		o->g.bytes_in += n;
		err = uiomove((caddr_t)o->g.pipe_va, (int)n, uio);
	}
	lck_mtx_unlock(&o->lock);
	return err;
}

static int
vgpu3d_dev_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused proc_t p)
{
	struct vopen *o = open_of(dev);
	int err;

	if (o == NULL) {
		return ENXIO;
	}
	lck_mtx_lock(&o->lock);
	switch (cmd) {
	case VGPU3D_IOC_CREATE:
		err = res_create(o, (struct vgpu3d_ioc_create *)data);
		break;
	case VGPU3D_IOC_UNREF: {
		struct res *r = res_find(o, *(uint32_t *)data);
		err = r ? 0 : ENOENT;
		if (r) {
			res_free(o, r);
		}
		break;
	}
	case VGPU3D_IOC_UPLOAD:
		err = res_xfer(o, (struct vgpu3d_ioc_xfer *)data, true);
		break;
	case VGPU3D_IOC_DOWNLOAD:
		err = res_xfer(o, (struct vgpu3d_ioc_xfer *)data, false);
		break;
	case VGPU3D_IOC_HOST_SHOWS:
		err = host_shows((uint32_t *)data);
		break;
	case VGPU3D_IOC_RING_MARK:
		err = ring_mark((uint64_t *)data);
		break;
	default:
		err = ENOTTY;
		break;
	}
	lck_mtx_unlock(&o->lock);
	return err;
}

static const struct cdevsw vgpu3d_cdevsw = {
	.d_open = vgpu3d_dev_open, .d_close = vgpu3d_dev_close, .d_read = vgpu3d_dev_read,
	.d_write = vgpu3d_dev_write, .d_ioctl = vgpu3d_dev_ioctl, .d_stop = eno_stop, .d_reset = eno_reset,
	.d_select = eno_select, .d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* ---- the boot self-test --------------------------------------------------------------- */

static void
vgpu3d_selftest_thread(__unused void *param, __unused wait_result_t wr)
{
	static struct vgpu3d g;
	static struct vgpu3d_selftest t;
	char size[24] = "";
	uint32_t w = 828, h = 1792;
	uint64_t pa;

	if (PE_parse_boot_argn("gpu3d_size", size, sizeof(size))) {
		char *x = strchr(size, 'x');
		if (x != NULL) {
			*x = 0;
			w = (uint32_t)strtoul(size, NULL, 10);
			h = (uint32_t)strtoul(x + 1, NULL, 10);
		}
	}
	lck_mtx_lock(&vgpu3d_opens_lock);
	int err = vgpu3d_probe();
	lck_mtx_unlock(&vgpu3d_opens_lock);
	g.ops = xnu_ops;
	g.features = vgpu3d_features;
	g.ctx_id = 1;
	if (err || vgpu3d_open(&g, vgpu3d_gles, PIPE_SIZE)) {
		printf("GPU3D-DONE fail\n");
		return;
	}
	t.width = w;
	t.height = h;
	t.pixels = xnu_dma_alloc(NULL, (size_t)w * h * 4, &pa);
	if (t.pixels == NULL) {
		printf("GPU3D-DONE fail\n");
		return;
	}
	uint64_t t0 = xnu_now_ns(NULL);
	err = vgpu3d_selftest(&g, &t);
	printf("GPU3D kernel selftest_ms=%llu\n", (xnu_now_ns(NULL) - t0) / 1000000);
	printf("GPU3D-DONE %s\n", !err && t.checks_passed == t.checks_total ? "pass" : "fail");
	vgpu3d_close(&g);
}

/* From bsd_init, after devfs: /dev/vgpu3d, and with boot-arg gpu3d=1 (or 2) the self-test. */
void
hakios_vgpu3d_init(void)
{
	uint32_t on = 0;
	thread_t th;

	scgpu_attach();
	vgpu3d_major = cdevsw_add(-1, &vgpu3d_cdevsw);
	if (vgpu3d_major < 0 || devfs_make_node_clone(makedev(vgpu3d_major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL,
	    0666, vgpu3d_clone, "vgpu3d") == NULL) {
		printf("vgpu3d: no /dev/vgpu3d\n");
	}
	if (!PE_parse_boot_argn("gpu3d", &on, sizeof(on)) || !on) {
		return;
	}
	if (on == 1) {
		vgpu3d_selftest_thread(NULL, THREAD_AWAKENED);
	} else if (kernel_thread_start(vgpu3d_selftest_thread, NULL, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
	}
}
