/*
 * SCSND: the sink of /dev/hakios-audio under AVF, the Scion app (sys/hakios.scsnd.h, patch 1401).
 *
 * With scsnd.addr= and scsnd.size= the app has lent a region with a header and a ring of samples.
 * What the audio server writes goes into the ring as it comes, and the app plays it through Android's
 * audio. Never waits (the writer is the audio server's real-time thread): what does not fit is lost,
 * which only happens when the app has stopped taking. iOS's volume is passed on in the header.
 */
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.scsnd.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);

static volatile struct scsnd_header *scsnd;
static uint8_t *scsnd_ring;
static uint32_t scsnd_bytes;
static uint32_t volume_seen = ~0u;

/* Called with /dev/hakios-audio's lock held: one writer at a time. */
static uint32_t
scsnd_play(const int16_t *frames, uint32_t count)
{
	volatile struct scsnd_header *h = scsnd;
	uint32_t below, generation;
	bool muted;

	if (hakios_audio_volume(&below, &muted, &generation) && generation != volume_seen) {
		h->below = below;
		h->muted = muted ? 1 : 0;
		volume_seen = generation;
	}
	uint32_t head = h->head;                /* (ours alone) */
	uint32_t tail = __atomic_load_n((uint32_t *)(uintptr_t)&h->tail, __ATOMIC_ACQUIRE);
	uint32_t used = head - tail, room = used <= scsnd_bytes ? scsnd_bytes - used : 0;
	uint32_t bytes = count * HAKIOS_AUDIO_FRAME;

	if (bytes > room) {
		bytes = room - room % HAKIOS_AUDIO_FRAME;
	}
	uint32_t at = head & (scsnd_bytes - 1);
	uint32_t first = bytes < scsnd_bytes - at ? bytes : scsnd_bytes - at;
	memcpy(scsnd_ring + at, frames, first);
	memcpy(scsnd_ring, (const uint8_t *)frames + first, bytes - first);
	__atomic_store_n((uint32_t *)(uintptr_t)&h->head, head + bytes, __ATOMIC_RELEASE);
	if (bytes < count * HAKIOS_AUDIO_FRAME) {
		h->dropped += count - bytes / HAKIOS_AUDIO_FRAME;
	}
	return bytes / HAKIOS_AUDIO_FRAME;
}

void
hakios_scsnd_init(void)
{
	uint64_t pa = 0, size = 0;
	volatile struct scsnd_header *h;

	if (!PE_parse_boot_argn("scsnd.addr", &pa, sizeof(pa)) || pa == 0) {
		return;
	}
	(void)PE_parse_boot_argn("scsnd.size", &size, sizeof(size));
	if ((pa & PAGE_MASK) || size < 0x10000 || size > (64ull << 20)) {
		printf("scsnd: scsnd.addr=%#llx scsnd.size=%#llx: not a region\n", pa, size);
		return;
	}
	h = (volatile struct scsnd_header *)ml_io_map_unmappable((vm_offset_t)pa, (vm_size_t)round_page(size), VM_WIMG_DEFAULT);
	if (h == NULL || h->magic != SCSND_MAGIC || h->version != SCSND_VERSION) {
		printf("scsnd: no header from the app at %#llx (magic %#x): no sound\n", pa, h != NULL ? h->magic : 0);
		return;
	}
	uint32_t off = h->ring_off, bytes = h->ring_bytes;
	if (off < sizeof(struct scsnd_header) || bytes < 4096 || (bytes & (bytes - 1)) || (uint64_t)off + bytes > size ||
	    h->rate != HAKIOS_AUDIO_RATE || h->channels != HAKIOS_AUDIO_CHANNELS) {
		printf("scsnd: the app's ring (%u bytes at %u, %u Hz, %u channels) is not one to write into\n", bytes, off,
		    h->rate, h->channels);
		return;
	}
	scsnd = h;
	scsnd_ring = (uint8_t *)(uintptr_t)h + off;
	scsnd_bytes = bytes;
	__atomic_store_n((uint32_t *)(uintptr_t)&h->guest_ready, 1, __ATOMIC_RELEASE);
	printf("scsnd: the app's speaker at %#llx: a ring of %u ms\n", pa,
	    bytes / HAKIOS_AUDIO_FRAME * 1000 / HAKIOS_AUDIO_RATE);
	hakios_audio_set_sink("the Scion app (SCSND)", scsnd_play);
}
