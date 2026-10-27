/*
 * SCGPU: the GPU of the Scion app, for an XNU guest under the Android Virtualization Framework.
 *
 * crosvm's virtio-gpu draws with the phone's GPU through gfxstream, but it can't show a picture to
 * a sideloaded app (avf/DISPLAY.md), so every frame had to come back into guest memory and go out
 * again through the shared framebuffer (SCFB): a readback of the GPU's picture for every frame, then
 * two copies. Here the app runs gfxstream's host renderer itself (tools/gfxstream/mk-host-android.sh)
 * and presents the guest's colour buffers straight into its window: nothing comes back.
 *
 * The guest still speaks virtio-gpu to it -- the same commands /dev/vgpu3d sends to crosvm
 * (bsd/dev/hakios.vgpu3d.c), the same gfxstream pipe on top -- but through a region of guest RAM the
 * app lends and maps too, as SCFB and SCCAM are lent (a CustomMemoryBackingFile). The kernel learns
 * where from scgpu.addr= and scgpu.size=; avfboot keeps the region out of XNU's memory.
 *
 *   +0           struct scgpu_header (one page)
 *   +4 KiB       the command: one virtio-gpu control command, as it would go on the control queue
 *   +64 KiB      its reply
 *   +128 KiB...  the pool: memory the guest hands out and names by guest physical address, as it would
 *                to a virtio device (ATTACH_BACKING of the pipe buffers, SCGPU_CMD_TRANSFER's staging);
 *                the app reaches it at the same offset in its own mapping
 *
 * One command at a time, as on a control queue: the guest writes the command and req_len, then bumps
 * req_seq (a release); the app sees it (an acquire), runs it, writes the reply, resp_len and status,
 * then sets resp_seq = req_seq (a release). Both sides poll for now.
 *
 * Commands are virtio-gpu's (virtio 1.2, 5.7.6), with these meanings at the app:
 *   GET_CAPSET_INFO, GET_CAPSET            gfxstream's capsets (stream_renderer_get_cap_set/fill_caps)
 *   CTX_CREATE, CTX_DESTROY                stream_renderer_context_create/destroy
 *   RESOURCE_CREATE_3D, RESOURCE_UNREF     stream_renderer_resource_create/unref
 *   RESOURCE_ATTACH_BACKING                the entries are in the pool (none: no backing)
 *   CTX_ATTACH_RESOURCE, CTX_DETACH_RESOURCE
 *   TRANSFER_TO_HOST_3D, TRANSFER_FROM_HOST_3D   on the attached backing (the pipe)
 *   RESOURCE_FLUSH                         present the resource (a colour buffer) in the app's window:
 *                                          stream_renderer_flush; SET_SCANOUT is not needed
 * and one of its own:
 *   SCGPU_CMD_TRANSFER                     a transfer through a staging buffer in the pool instead of
 *                                          a backing attached for good, so that the hundreds of
 *                                          colour buffers CoreAnimation shares cost no guest memory
 */
#ifndef _SYS_HAKIOS_SCGPU_H_
#define _SYS_HAKIOS_SCGPU_H_

#include <stdint.h>

#define SCGPU_MAGIC             0x50474353u     /* "SCGP" */
#define SCGPU_VERSION           1u
#define SCGPU_CMD_OFF           0x1000u
#define SCGPU_CMD_MAX           (0x10000u - SCGPU_CMD_OFF)
#define SCGPU_RESP_OFF          0x10000u
#define SCGPU_RESP_MAX          0x1000u
#define SCGPU_POOL_OFF          0x20000u

/* the app's renderer is up and answers the mailbox */
#define SCGPU_F_HOST_READY      (1u << 0)
/*
 * guest_flags: the guest has begun to show frames through the host (rcFBPost): the app can put the
 * renderer's window in front from now on (before, the shared framebuffer had the picture)
 */
#define SCGPU_G_PRESENTING      (1u << 0)

/* SCGPU_CMD_TRANSFER: outside virtio-gpu's command numbers */
#define SCGPU_CMD_TRANSFER      0x0f00u

struct scgpu_header {
	uint32_t magic, version;
	uint64_t size;                  /* of the whole region */
	uint32_t flags;                 /* SCGPU_F_*, set by the app */
	uint32_t features_lo, features_hi;      /* the virtio-gpu features the "device" offers */
	uint32_t num_capsets;
	/* the mailbox */
	uint32_t req_seq;               /* the guest's, bumped after the command is written */
	uint32_t req_len;               /* bytes at SCGPU_CMD_OFF */
	uint32_t resp_max;              /* bytes the guest takes at SCGPU_RESP_OFF */
	uint32_t resp_seq;              /* the app's: = req_seq once the reply is there */
	uint32_t resp_len;
	int32_t status;                 /* 0, or a negative errno: the app could not run the command */
	/* counters, for the logs of both sides */
	uint64_t commands, presents, host_busy_ns;
	uint32_t guest_flags;           /* SCGPU_G_*, set by the guest */
	uint32_t reserved[15];
};

/* struct virtio_gpu_transfer_host_3d's twin, with the staging buffer instead of the backing */
struct scgpu_transfer {
	uint32_t type, flags;           /* struct virtio_gpu_ctrl_hdr, type = SCGPU_CMD_TRANSFER */
	uint64_t fence_id;
	uint32_t ctx_id;
	uint8_t ring_idx, padding[3];
	uint32_t resource_id;
	uint32_t to_host;               /* 1: guest to host, 0: host to guest */
	uint32_t x, y, w, h;            /* the box, in pixels */
	uint64_t offset;                /* of pixel (x, y) in the resource's rows (width * 4 bytes each) */
	uint64_t staging;               /* guest physical address in the pool: pixel (x, y), then the */
	uint64_t staging_len;           /* rows at the resource's pitch, (h - 1) * pitch + w * 4 bytes */
};

#endif /* _SYS_HAKIOS_SCGPU_H_ */
