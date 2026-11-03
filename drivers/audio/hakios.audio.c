/*
 * /dev/hakios-audio: the sound that Darwin plays (see sys/hakios.audio.h).
 *
 * What is written goes two ways:
 *
 *   to a sink   a driver that plays it: the virtio sound device under QEMU
 *               (hakios.virtio_sound.c). On the phone there is none yet (hakios.speaker.c
 *               is where it begins).
 *
 *   into a recording, with boot-arg rec_sound=N: the first N seconds of sound are also kept
 *               in one piece of memory that is left in place when the kernel returns to the
 *               loader, which hands it to the host ("xl sound"; under QEMU, the monitor's
 *               pmemsave), where tools/sound.py makes a WAV file of it. Only what is played
 *               takes room, the silence in between does not: every write is stored with the
 *               time it was made.
 *
 *                 struct rec_header, then per write: struct rec_block, frames * 4 bytes
 *
 *               With rec_sound_fed=1 as well, the recording has instead what the speakers' guard
 *               let through (hakios.guard.c, on the phone): what reaches the amplifiers. With
 *               rec_sound_mic=1, what the main microphone hears (hakios.mic.c, mic=3), in both
 *               channels.
 *
 * /dev/hakios-mic, the other way: what a microphone hears, for Darwin (sys/hakios.audio.h). The driver that listens
 * (hakios.mic.c on the POCO X5 Pro, hakios.exynos_abox.c on the Galaxy S9) makes it with hakios_audio_input_create(),
 * listens while hakios_audio_input_wanted() says it is open, and puts what it hears in with hakios_audio_input_put();
 * the ring, the reader's distance behind the microphone and the device itself are kept here, the same for both.
 *
 * HAKIOS_AUDIO_GET_ROUTE says whether something is plugged into the headphone jack and whether the sound goes there
 * (hakios.cs47l93.c tells it, hakios_audio_set_route()); the plug-in asks.
 *
 * When the writer has stopped, a line in the log says how long it wrote, at what rate, how much
 * of it was not silence and how loud: the only way to "hear" a machine one cannot listen to.
 * (The audio server writes for as long as its device runs, a second or two beyond the sound.)
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/kalloc.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/hakios.audio.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <vm/vm_kern.h>

extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */

#define CHUNK_FRAMES    1024            /* what one pass of write() moves */
#define QUIET_MS        500             /* no write for this long: the writer has stopped */
#define SILENCE         8               /* samples no larger than this are silence */
#define REC_MAX_SECONDS 1200            /* 220 MB */

struct rec_header {
	char     magic[8];              /* "RWSND001" */
	uint32_t rate, channels;        /* HAKIOS_AUDIO_RATE, HAKIOS_AUDIO_CHANNELS; the samples are 16-bit */
	uint32_t blocks;
	uint32_t reserved;
};
struct rec_block {
	uint64_t nanoseconds;           /* when it was written, since the kernel started */
	uint32_t frames;
	uint32_t reserved;
};

/* { physical address, bytes, kernel address } of the recording, for the loader: osfmk/arm64/loader_return.c */
extern uint64_t loader_sound[3];

static LCK_GRP_DECLARE(audio_lck_grp, "hakios-audio");
static LCK_MTX_DECLARE(audio_lock, &audio_lck_grp);

static hakios_audio_sink_t audio_sink;

static uint8_t *rec_base;
static bool rec_fed;                    /* boot-arg rec_sound_fed=1: what the guard let through is recorded, not what was written */
static bool rec_mic;                    /* boot-arg rec_sound_mic=1: what the microphone hears (hakios.mic.c) */
static uint64_t rec_size, rec_used;
static uint64_t rec_frames, rec_frames_max;

/* the stretch of writing in progress */
static uint64_t stretch_first, stretch_last;    /* mach_absolute_time() of its first and its latest write */
static uint64_t stretch_frames, stretch_latest; /* frames written, and how many of them by the latest write */
static uint64_t stretch_sound, stretch_lost;    /* frames in writes that were not silence; frames the sink did not take */
static uint32_t stretch_peak;                   /* the largest sample, as an absolute value */
static thread_call_t report_call;
static bool report_armed;

static void
audio_report(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t frames, latest, sound, lost, deadline, since = 0, lasted = 0;
	uint32_t loudest;

	lck_mtx_lock(&audio_lock);
	absolutetime_to_nanoseconds(mach_absolute_time() - stretch_last, &since);
	if (since < QUIET_MS * NSEC_PER_MSEC) {
		/* still writing: look again later */
		clock_interval_to_deadline(2 * QUIET_MS, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(report_call, deadline);
		lck_mtx_unlock(&audio_lock);
		return;
	}
	absolutetime_to_nanoseconds(stretch_last - stretch_first, &lasted);
	frames = stretch_frames;
	latest = stretch_latest;
	sound = stretch_sound;
	lost = stretch_lost;
	loudest = stretch_peak;
	stretch_frames = stretch_sound = stretch_lost = 0;
	stretch_peak = 0;
	report_armed = false;
	lck_mtx_unlock(&audio_lock);

	printf("hakios-audio: %llu.%02llu s written", frames / HAKIOS_AUDIO_RATE,
	    frames % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE);
	if (lasted >= NSEC_PER_SEC) {
		/* what was written before the latest write took the time between the first and the latest */
		printf(" at %llu frames a second", (frames - latest) * NSEC_PER_SEC / lasted);
	}
	printf(", %llu.%02llu s of it sound, peak %u%% of full scale", sound / HAKIOS_AUDIO_RATE,
	    sound % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE, loudest * 100 / 32768);
	if (lost) {
		printf(", %llu ms lost (the sink was full)", lost * 1000 / HAKIOS_AUDIO_RATE);
	}
	printf("\n");
}

/* With audio_lock held. */
static void
audio_record(const int16_t *frames, uint32_t count)
{
	struct rec_header *header = (struct rec_header *)rec_base;
	struct rec_block *block;
	uint64_t bytes = (uint64_t)count * HAKIOS_AUDIO_FRAME;

	if (rec_base == NULL || rec_frames >= rec_frames_max || rec_used + sizeof(*block) + bytes > rec_size) {
		return;
	}
	block = (struct rec_block *)(rec_base + rec_used);
	absolutetime_to_nanoseconds(mach_absolute_time(), &block->nanoseconds);
	block->frames = count;
	block->reserved = 0;
	memcpy(block + 1, frames, bytes);
	rec_used += sizeof(*block) + bytes;
	rec_frames += count;
	header->blocks++;
	loader_sound[1] = rec_used;
	if (rec_frames >= rec_frames_max) {
		printf("hakios-audio: recording finished: %llu s in %u writes\n", rec_frames / HAKIOS_AUDIO_RATE, header->blocks);
	}
}

bool
hakios_audio_recording_fed(void)
{
	return rec_fed && rec_base != NULL && rec_frames < rec_frames_max;
}

bool
hakios_audio_recording_mic(void)
{
	return rec_mic && rec_base != NULL && rec_frames < rec_frames_max;
}

/* From the speakers' feeder: what the guard let through, into the recording (rec_sound_fed=1). */
void
hakios_audio_record_fed(const int16_t *frames, uint32_t count)
{
	lck_mtx_lock(&audio_lock);
	audio_record(frames, count);
	lck_mtx_unlock(&audio_lock);
}

static void
audio_play(const int16_t *frames, uint32_t count)
{
	uint32_t loudest = 0, taken = 0;
	uint64_t deadline;

	for (uint32_t i = 0; i < count * HAKIOS_AUDIO_CHANNELS; i++) {
		uint32_t level = frames[i] < 0 ? -(int32_t)frames[i] : frames[i];

		if (level > loudest) {
			loudest = level;
		}
	}
	lck_mtx_lock(&audio_lock);
	if (!rec_fed && !rec_mic) {
		audio_record(frames, count);
	}
	if (audio_sink != NULL) {
		taken = audio_sink(frames, count);
		stretch_lost += count - taken;
	}
	stretch_last = mach_absolute_time();
	if (stretch_frames == 0) {
		stretch_first = stretch_last;
	}
	stretch_frames += count;
	stretch_latest = count;
	if (loudest > SILENCE) {
		stretch_sound += count;
	}
	if (loudest > stretch_peak) {
		stretch_peak = loudest;
	}
	if (!report_armed && report_call != NULL) {
		report_armed = true;
		clock_interval_to_deadline(2 * QUIET_MS, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(report_call, deadline);
	}
	lck_mtx_unlock(&audio_lock);
}

void
hakios_audio_set_sink(const char *name, hakios_audio_sink_t sink)
{
	lck_mtx_lock(&audio_lock);
	audio_sink = sink;
	lck_mtx_unlock(&audio_lock);
	printf("hakios-audio: sound goes to %s\n", name);
}

static int
audio_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
audio_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
audio_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t frames[CHUNK_FRAMES * HAKIOS_AUDIO_CHANNELS];
	int error = 0;

	while (uio_resid(uio) >= HAKIOS_AUDIO_FRAME) {
		user_ssize_t bytes = uio_resid(uio);

		if (bytes > (user_ssize_t)sizeof(frames)) {
			bytes = sizeof(frames);
		}
		bytes -= bytes % HAKIOS_AUDIO_FRAME;
		error = uiomove((caddr_t)frames, (int)bytes, uio);
		if (error) {
			break;
		}
		audio_play(frames, (uint32_t)(bytes / HAKIOS_AUDIO_FRAME));
	}
	return error;
}

/* the speakers' volume, as Darwin sets it (sys/hakios.audio.h): kept for the sink's driver to look at */
#define HAKIOS_AUDIO_BELOW_MAX  (90 * 8)        /* eighths of a dB: the amplifiers' own lowest volume (speaker.c VOLUME_LOWEST) */
static uint32_t volume_below, volume_generation;
static bool volume_muted;

/* where the sound goes (HAKIOS_AUDIO_GET_ROUTE), as the driver of the jack has said it */
static struct hakios_audio_route route_now;
static bool route_told;                 /* a driver knows a jack here: HAKIOS_AUDIO_GET_ROUTE is answered */

void
hakios_audio_set_route(bool jack, bool headphones)
{
	lck_mtx_lock(&audio_lock);
	route_told = true;
	if (route_now.jack != jack || route_now.headphones != headphones) {
		route_now.jack = jack;
		route_now.headphones = headphones;
		route_now.generation++;
	}
	lck_mtx_unlock(&audio_lock);
	printf("hakios-audio: the jack: %s; the sound goes to the %s\n", jack ? "something plugged in" : "nothing plugged in",
	    headphones ? "headphones" : "speakers");
}

static int
audio_ioctl(__unused dev_t dev, u_long command, caddr_t data, __unused int flags, __unused struct proc *p)
{
	const struct hakios_audio_volume *volume = (const struct hakios_audio_volume *)data;
	uint32_t below;

	if (command == HAKIOS_AUDIO_GET_ROUTE) {
		int error = ENOTTY;     /* no jack known here (every machine but the S9 with cs47l93_hp=1): the plug-in shows none */

		lck_mtx_lock(&audio_lock);
		if (route_told) {
			*(struct hakios_audio_route *)data = route_now;
			error = 0;
		}
		lck_mtx_unlock(&audio_lock);
		return error;
	}
	if (command != HAKIOS_AUDIO_SET_VOLUME) {
		return ENOTTY;
	}
	/*
	 * The amplifiers go no lower than -90 dB (hakios.speaker.c's VOLUME_LOWEST, the "mute volume"): a larger `below'
	 * means nothing there, and from user space (the node is open to all) it would make the sums made with it
	 * (sound_below + below) wrap round to a level far louder than the one asked for.
	 */
	below = volume->below > HAKIOS_AUDIO_BELOW_MAX ? HAKIOS_AUDIO_BELOW_MAX : volume->below;
	lck_mtx_lock(&audio_lock);
	volume_below = below;
	volume_muted = volume->muted != 0;
	volume_generation++;
	lck_mtx_unlock(&audio_lock);
	printf("hakios-audio: the volume: %s%u.%03u dB under the loudest\n", volume->muted ? "muted; " : "", below / 8,
	    below % 8 * 125);
	return 0;
}

bool
hakios_audio_volume(uint32_t *below, bool *muted, uint32_t *generation)
{
	bool known;

	lck_mtx_lock(&audio_lock);
	known = hakios_audio_volume_held(below, muted, generation);
	lck_mtx_unlock(&audio_lock);
	return known;
}

/* The same, for a sink: it is called with audio_lock held (hakios_audio_set_sink). */
bool
hakios_audio_volume_held(uint32_t *below, bool *muted, uint32_t *generation)
{
	*below = volume_below;
	*muted = volume_muted;
	*generation = volume_generation;
	return *generation != 0;
}

static const struct cdevsw audio_cdevsw = {
	.d_open = audio_open, .d_close = audio_close, .d_read = eno_rdwrt, .d_write = audio_write,
	.d_ioctl = audio_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* ---- /dev/hakios-mic ------------------------------------------------------------------------------------------------ */

/*
 * What the microphone hears goes into a ring, and the device gives it to Darwin's audio server: 16-bit samples, one
 * channel, 48 kHz. A read never waits: it gives what has come, keeping the reader about 40 ms behind the microphone,
 * and nothing until that much is there; the audio server makes up the rest with silence. (Moved here from hakios.mic.c,
 * which had it for the POCO's microphone alone, when the Galaxy S9's came: the same numbers.)
 */
#define IN_RING_FRAMES          16384           /* 341 ms; a power of two */
#define IN_KEEP                 1920            /* 40 ms: how far behind the microphone a reader is kept */
#define IN_MOST                 4800            /* 100 ms: more than this waiting, and the oldest is skipped */

static int16_t *in_ring;
static volatile uint64_t in_written;            /* frames put into the ring since the device was made */
static uint64_t in_read;                        /* frames taken from it (one reader: the audio server) */
static bool in_primed;                          /* the reader is IN_KEEP behind; false after it ran dry */
static uint64_t in_skipped, in_dry;             /* frames skipped, reads that found too little, since begin */
static volatile uint32_t in_open;               /* the device is open */

void
hakios_audio_input_put(const int16_t *samples, uint32_t count)
{
	uint64_t at = in_written;

	if (in_ring == NULL) {
		return;
	}
	for (uint32_t i = 0; i < count; i++) {
		in_ring[(at + i) % IN_RING_FRAMES] = samples[i];
	}
	__atomic_store_n(&in_written, at + count, __ATOMIC_RELEASE);
}

bool
hakios_audio_input_wanted(void)
{
	return in_open != 0;
}

void
hakios_audio_input_begin(void)
{
	in_primed = false;
	in_skipped = in_dry = 0;
	in_read = __atomic_load_n(&in_written, __ATOMIC_ACQUIRE);
}

void
hakios_audio_input_counts(uint64_t *skipped, uint64_t *dry)
{
	*skipped = in_skipped;
	*dry = in_dry;
}

static int
input_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint64_t written = __atomic_load_n(&in_written, __ATOMIC_ACQUIRE), avail, want;
	int error = 0;

	if (in_ring == NULL || uio_resid(uio) < 2) {
		return 0;
	}
	want = (uint64_t)uio_resid(uio) / 2;
	avail = written - in_read;
	if (avail > IN_MOST) {
		in_skipped += avail - IN_KEEP;
		in_read = written - IN_KEEP;
		avail = IN_KEEP;
	}
	if (!in_primed) {
		if (avail < IN_KEEP) {
			return 0;
		}
		in_primed = true;
	}
	if (avail < want) {
		in_dry++;
		in_primed = false;
	}
	while (want > 0 && avail > 0 && error == 0) {
		uint64_t at = in_read % IN_RING_FRAMES, n = want < avail ? want : avail;

		n = n < IN_RING_FRAMES - at ? n : IN_RING_FRAMES - at;
		error = uiomove((caddr_t)&in_ring[at], (int)(n * 2), uio);
		in_read += n;
		want -= n;
		avail -= n;
	}
	return error;
}

static int
input_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	in_open = 1;
	return 0;
}

/* (on the last close) */
static int
input_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	in_open = 0;
	return 0;
}

static const struct cdevsw input_cdevsw = {
	.d_open = input_open, .d_close = input_close, .d_read = input_read, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

bool
hakios_audio_input_create(const char *source)
{
	static bool made;
	int major;

	if (made) {
		return true;
	}
	in_ring = kalloc_data(IN_RING_FRAMES * sizeof(int16_t), Z_WAITOK | Z_ZERO);
	major = cdevsw_add(-1, &input_cdevsw);
	if (in_ring == NULL || major < 0 ||
	    devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-mic") == NULL) {
		printf("hakios-audio: /dev/hakios-mic cannot be made\n");
		return false;
	}
	made = true;
	printf("hakios-audio: /dev/hakios-mic: %s\n", source);
	return true;
}

static void
audio_recording_init(void)
{
	struct rec_header *header;
	uint32_t seconds = 0, fed = 0;
	vm_offset_t memory = 0;
	uint64_t size;

	if (!PE_parse_boot_argn("rec_sound", &seconds, sizeof(seconds)) || seconds == 0) {
		return;
	}
	if (seconds > REC_MAX_SECONDS) {
		seconds = REC_MAX_SECONDS;
	}
	/* the samples, and a block header for every write of ten milliseconds or more */
	size = sizeof(*header) + (uint64_t)seconds * (HAKIOS_AUDIO_RATE * HAKIOS_AUDIO_FRAME + 100 * sizeof(struct rec_block));
	size = round_page(size);
	/* one piece of physical memory: the loader will send it over USB as it lies */
	if (kmem_alloc_contig(kernel_map, &memory, size, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("hakios-audio: no memory for a recording of %u s (%llu MB)\n", seconds, size >> 20);
		return;
	}
	header = (struct rec_header *)memory;
	memcpy(header->magic, "RWSND001", 8);
	header->rate = HAKIOS_AUDIO_RATE;
	header->channels = HAKIOS_AUDIO_CHANNELS;
	rec_size = size;
	rec_used = sizeof(*header);
	rec_frames_max = (uint64_t)seconds * HAKIOS_AUDIO_RATE;
	rec_base = (uint8_t *)memory;
	loader_sound[0] = kvtophys(memory);
	loader_sound[1] = rec_used;
	loader_sound[2] = (uint64_t)memory;
	PE_parse_boot_argn("rec_sound_fed", &fed, sizeof(fed));
	rec_fed = fed != 0;
	fed = 0;
	PE_parse_boot_argn("rec_sound_mic", &fed, sizeof(fed));
	rec_mic = fed != 0;
	printf("hakios-audio: recording the first %u s of sound%s, into %llu MB at %#llx\n", seconds,
	    rec_fed ? " as the speakers' guard lets it through" : rec_mic ? " the microphone hears (in both channels)" : "", size >> 20,
	    loader_sound[0]);
}

void
hakios_audio_init(void)
{
	int major = cdevsw_add(-1, &audio_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-audio") == NULL) {
		printf("hakios-audio: cannot create the device\n");
		return;
	}
	report_call = thread_call_allocate(audio_report, NULL);
	audio_recording_init();
	hakios_virtio_sound_init();
	hakios_speaker_init();
	hakios_haptic_init();
	hakios_torch_init();
	hakios_camera_init();
}
