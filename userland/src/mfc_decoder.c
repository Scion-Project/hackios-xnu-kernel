/*
 * VideoToolbox's decoder for H.264 and HEVC on the Exynos 9810's MFC (kernel: bsd/dev/hakios.exynos_mfc.c, /dev/hakios-mfc, boot-arg mfc=5),
 * part of libhakios.media.dylib: VideoToolbox decodes for everybody in mediaserverd (see inject_media.c). Registered only when the
 * kernel has made the device; VideoToolbox's software decoders (H264SW.videocodec, VCPHEVC.videocodec) stay registered behind it. A
 * session that cannot have the hardware (another session has it; the block has failed) is refused when it is created, and VideoToolbox
 * goes on to the software decoder.
 *
 * The private ABI, from the decoders of Darwin userland 16.5 (20F66), read from their code, not assumed:
 *   VTRegisterVideoDecoderWithInfo_(codec, info, create): create(codec, allocator, &object), as H264SW.videocodec's (VCPAVCRegisterDecoder).
 *   CMDerivedObjectCreate_(allocator, &vtable, VTVideoDecoderGetClassID_(), &object); vtable: { 0, &base, &decoder } (H264SW's: its data at
 *   0x213c89950).
 *   base: { version 1, the derived storage's size, equal, invalidate, finalize, copyDebugDescription, copyProperty, setProperty, 0, 0 }.
 *   decoder: { version, startSession, decodeFrame, copySupportedPropertyDictionary, setProperties, copySerializableProperties,
 *   canAcceptFormatDescription, finishDelayedFrames, ... }; H264SW's is version 3 with two later functions, which are left NULL here.
 *   startSession(decoder, session, format); decodeFrame(decoder, frameRefCon, sampleBuffer, flags, &infoFlags);
 *   VTDecoderSessionEmitDecodedFrame_(session, frameRefCon, status, infoFlags, imageBuffer) gives a frame back, in display order; it may
 *   come after decodeFrame has returned, and every frame gets its one emission.
 *
 * The samples are length-prefixed NAL units; the block wants Annex B, so each is rewritten, and the parameter sets from the format go
 * first. Pictures are copied by the kernel into a pixel buffer from VideoToolbox's pool (NV12, video range): one copy of a frame, where
 * the software decoder runs on the processor for all of it. Writing straight into an IOSurface is the next step.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "hakios.mfc.h"

/* VideoToolbox's and CoreMedia's private functions: looked up, none of them is in the SDK's stubs (hakios_mfc_decoder_register) */
static CFTypeID (*VTVideoDecoderGetClassID_)(void);
static void *(*VTVideoDecoderGetCMBaseObject_)(void *decoder);
static OSStatus (*VTDecoderSessionSetPixelBufferAttributes_)(void *session, CFDictionaryRef attributes);
static CVPixelBufferPoolRef (*VTDecoderSessionGetPixelBufferPool_)(void *session);
static OSStatus (*VTDecoderSessionEmitDecodedFrame_)(void *session, void *frame, OSStatus status, uint32_t infoFlags, CVImageBufferRef image);
static OSStatus (*VTRegisterVideoDecoderWithInfo_)(uint32_t codec, CFDictionaryRef info, OSStatus (*create)(uint32_t, CFAllocatorRef, void **));
static OSStatus (*CMDerivedObjectCreate_)(CFAllocatorRef allocator, const void *vtable, CFTypeID classID, void **out);
static void *(*CMBaseObjectGetDerivedStorage_)(void *object);
static const CFStringRef *kVTVideoDecoderList_DecoderID_, *kVTVideoDecoderList_DecoderName_;

#define ERR_COULD_NOT_FIND      (-12906)        /* kVTCouldNotFindVideoDecoderErr */
#define ERR_BAD_DATA            (-12909)        /* kVTVideoDecoderBadDataErr */
#define ERR_MALFUNCTION         (-12911)        /* kVTVideoDecoderMalfunctionErr */
#define PENDING_MAX             64
#define RECENT_FRAMES           6

struct mfc_decoder {
	int fd;
	uint32_t codec;
	void *session;
	int length_size;
	uint8_t *header;
	size_t header_bytes;
	bool opened;
	bool drained;                   /* the block has finished this instance: the next key frame opens it again */
	uint32_t width, height;
	CFTypeRef matrix, primaries, transfer;
	CVPixelBufferRef recent[RECENT_FRAMES]; /* the last frames emitted, kept a little (see emit) */
	unsigned recent_at;
	CVPixelBufferRef spare;
	uint32_t next_tag;
	void *pending[PENDING_MAX];     /* frameRefCons not yet shown, by tag % PENDING_MAX */
	bool pending_set[PENDING_MAX];
	uint8_t *stream;
	size_t stream_bytes;
};

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

static struct mfc_decoder *
storage(void *decoder)
{
	return CMBaseObjectGetDerivedStorage_(VTVideoDecoderGetCMBaseObject_(decoder));
}

static void
append(uint8_t **to, size_t *at, size_t *room, const uint8_t *from, size_t bytes)
{
	static const uint8_t start[4] = { 0, 0, 0, 1 };

	if (*at + bytes + 4 > *room) {
		*room = (*at + bytes + 4) * 2;
		*to = reallocf(*to, *room);
		if (*to == NULL) {
			return;
		}
	}
	memcpy(*to + *at, start, 4);
	memcpy(*to + *at + 4, from, bytes);
	*at += bytes + 4;
}

/*
 * What the block's NV12 buffers cannot hold: more than 8 bits, or other than 4:2:0. HEVC says so in its hvcC (ISO 14496-15: byte 16 the chroma
 * format, 17 and 18 the depths less 8); H.264 by the profile in its first SPS (High and below are 8-bit 4:2:0).
 */
static bool
is_8bit_420(struct mfc_decoder *d, CMVideoFormatDescriptionRef format)
{
	if (d->codec == HAKIOS_MFC_H264) {
		const uint8_t *sps = NULL;
		size_t bytes = 0;

		if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &sps, &bytes, NULL, NULL) != noErr || bytes < 2) {
			return false;
		}
		return sps[1] == 66 || sps[1] == 77 || sps[1] == 88 || sps[1] == 100;
	}
	CFDictionaryRef atoms = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
	CFDataRef hvcc = atoms != NULL && CFGetTypeID(atoms) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(atoms, CFSTR("hvcC")) : NULL;

	if (hvcc == NULL || CFGetTypeID(hvcc) != CFDataGetTypeID() || CFDataGetLength(hvcc) < 19) {
		return false;   /* (not seen: nobody says what it is, so software) */
	}
	const uint8_t *c = CFDataGetBytePtr(hvcc);

	return (c[16] & 3) == 1 && (c[17] & 7) == 0 && (c[18] & 7) == 0;
}

static bool
open_block(struct mfc_decoder *d)
{
	struct hakios_mfc_open request;

	memset(&request, 0, sizeof(request));
	request.codec = d->codec;
	request.header = (uint64_t)(uintptr_t)d->header;
	request.header_bytes = (uint32_t)d->header_bytes;
	if (ioctl(d->fd, HAKIOS_MFC_OPEN, &request) != 0) {
		return false;
	}
	if (request.width > d->width || request.height > d->height) {
		say("hakios: mfc: the stream is %ux%u, its format says %ux%u: software\n", request.width, request.height, d->width, d->height);
		return false;
	}
	d->opened = true;
	d->drained = false;
	return true;
}

static OSStatus
start_session(void *decoder, void *session, CMVideoFormatDescriptionRef format)
{
	struct mfc_decoder *d = storage(decoder);
	OSStatus (*get)(CMFormatDescriptionRef, size_t, const uint8_t **, size_t *, size_t *, int *) =
	    d->codec == HAKIOS_MFC_H264 ? CMVideoFormatDescriptionGetH264ParameterSetAtIndex : CMVideoFormatDescriptionGetHEVCParameterSetAtIndex;
	size_t count = 0, room = 0;
	const uint8_t *set;
	size_t bytes;

	d->session = session;
	if (get(format, 0, NULL, NULL, &count, &d->length_size) != noErr || count == 0 || d->length_size < 1 || d->length_size > 4) {
		say("hakios: mfc: no parameter sets in the format: not for the hardware\n");
		return ERR_COULD_NOT_FIND;
	}
	if (!is_8bit_420(d, format)) {
		say("hakios: mfc: more than 8 bits, or not 4:2:0: software\n");
		return ERR_COULD_NOT_FIND;
	}
	for (size_t i = 0; i < count; i++) {
		if (get(format, i, &set, &bytes, NULL, NULL) == noErr) {
			append(&d->header, &d->header_bytes, &room, set, bytes);
		}
	}
	CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);

	d->width = size.width;
	d->height = size.height;
	/* the block is asked now, not at the first frame: a session refused here goes on to the software decoder (too large a stream: no memory) */
	if (!open_block(d)) {
		say("hakios: mfc: the block refuses a %dx%d stream: software\n", size.width, size.height);
		return ERR_COULD_NOT_FIND;
	}
	CFBooleanRef full = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_FullRangeVideo);
	int32_t pixel = (full != NULL && CFBooleanGetValue(full)) ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	int32_t width = size.width, height = size.height;

#define KEEP(to, key) do { CFTypeRef v = CMFormatDescriptionGetExtension(format, key); d->to = v != NULL ? CFRetain(v) : NULL; } while (0)
	KEEP(matrix, kCMFormatDescriptionExtension_YCbCrMatrix);
	KEEP(primaries, kCMFormatDescriptionExtension_ColorPrimaries);
	KEEP(transfer, kCMFormatDescriptionExtension_TransferFunction);
#undef KEEP
	CFNumberRef pixel_n = CFNumberCreate(NULL, kCFNumberSInt32Type, &pixel), width_n = CFNumberCreate(NULL, kCFNumberSInt32Type, &width),
	    height_n = CFNumberCreate(NULL, kCFNumberSInt32Type, &height);
	CFDictionaryRef surface = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *keys[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey };
	const void *values[] = { pixel_n, width_n, height_n, surface };
	CFDictionaryRef attributes = CFDictionaryCreate(NULL, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	OSStatus status = VTDecoderSessionSetPixelBufferAttributes_(session, attributes);
	CFRelease(attributes);
	CFRelease(surface);
	CFRelease(pixel_n);
	CFRelease(width_n);
	CFRelease(height_n);
	say("hakios: mfc: a %s session of %dx%d, %zu parameter sets (%d)\n", d->codec == HAKIOS_MFC_H264 ? "H.264" : "HEVC", width, height, count, (int)status);
	return status;
}

/* whatever frame the kernel wrote goes back to VideoToolbox: its frameRefCon by its tag */
static void
emit(struct mfc_decoder *d, uint32_t tag, CVPixelBufferRef image)
{
	uint32_t i = tag % PENDING_MAX;

	if (d->pending_set[i]) {
		d->pending_set[i] = false;
		if (image != NULL) {
			if (d->matrix != NULL) {
				CVBufferSetAttachment(image, kCVImageBufferYCbCrMatrixKey, d->matrix, kCVAttachmentMode_ShouldPropagate);
			}
			if (d->primaries != NULL) {
				CVBufferSetAttachment(image, kCVImageBufferColorPrimariesKey, d->primaries, kCVAttachmentMode_ShouldPropagate);
			}
			if (d->transfer != NULL) {
				CVBufferSetAttachment(image, kCVImageBufferTransferFunctionKey, d->transfer, kCVAttachmentMode_ShouldPropagate);
			}
		}
		VTDecoderSessionEmitDecodedFrame_(d->session, d->pending[i], noErr, image != NULL ? 0 : 2 /* kVTDecodeInfo_FrameDropped */, image);
		/*
		 * The frame crosses to the client's process after this returns, and until the client has it nothing says it is in use: released now, the pool gives it out again and the
		 * next frame overwrites it. (Seen on the phone, mfc_vt at full speed: frame 0 with frame 1's picture, and one frame of the burst at a drain.) So the last few stay held.
		 */
		if (image != NULL) {
			CVPixelBufferRetain(image);
			if (d->recent[d->recent_at] != NULL) {
				CVPixelBufferRelease(d->recent[d->recent_at]);
			}
			d->recent[d->recent_at] = image;
			d->recent_at = (d->recent_at + 1) % RECENT_FRAMES;
		}
	}
}

static OSStatus
decode_unit(struct mfc_decoder *d, const uint8_t *stream, size_t bytes, uint32_t tag, bool *finished)
{
	struct hakios_mfc_decode request;
	OSStatus status = noErr;
	CVPixelBufferRef image = d->spare;

	d->spare = NULL;
	if (image == NULL) {
		CVPixelBufferPoolRef pool = VTDecoderSessionGetPixelBufferPool_(d->session);

		if (pool == NULL || CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &image) != kCVReturnSuccess || image == NULL) {
			return ERR_MALFUNCTION;
		}
	}
	CVPixelBufferLockBaseAddress(image, 0);
	memset(&request, 0, sizeof(request));
	request.stream = (uint64_t)(uintptr_t)stream;
	request.bytes = (uint32_t)bytes;
	request.tag = tag;
	request.y = (uint64_t)(uintptr_t)CVPixelBufferGetBaseAddressOfPlane(image, 0);
	request.uv = (uint64_t)(uintptr_t)CVPixelBufferGetBaseAddressOfPlane(image, 1);
	request.y_stride = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(image, 0);
	request.uv_stride = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(image, 1);
	request.room_width = (uint32_t)CVPixelBufferGetWidth(image);
	request.room_height = (uint32_t)CVPixelBufferGetHeight(image);
	if (ioctl(d->fd, HAKIOS_MFC_DECODE, &request) != 0) {
		/* EBADMSG: this frame's data; the rest (the block has died, the stream outgrew its buffers): the session cannot go on, and VideoToolbox's client makes a new one, in software if need be */
		status = errno == EBADMSG ? ERR_BAD_DATA : ERR_MALFUNCTION;
		if (status == ERR_MALFUNCTION) {
			say("hakios: mfc: a decode fails: %d\n", errno);
		}
	}
	CVPixelBufferUnlockBaseAddress(image, 0);
	if (finished != NULL) {
		*finished = request.finished != 0;
	}
	if (status == noErr && request.shown) {
		emit(d, request.shown_tag, image);
		CVPixelBufferRelease(image);
	} else {
		d->spare = image;       /* nothing written into it: the next frame's */
	}
	return status;
}

static OSStatus
decode_frame(void *decoder, void *frame, CMSampleBufferRef sample, uint32_t flags, uint32_t *info)
{
	struct mfc_decoder *d = storage(decoder);
	CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
	size_t total = block != NULL ? CMBlockBufferGetDataLength(block) : 0, at = 0, out = 0, room = d->stream_bytes;
	uint8_t *source = malloc(total + 1);
	OSStatus status;

	(void)flags;
	if (info != NULL) {
		*info = 1;              /* kVTDecodeInfo_Asynchronous: a frame may come after this returns */
	}
	if (source == NULL || CMBlockBufferCopyDataBytes(block, 0, total, source) != kCMBlockBufferNoErr) {
		free(source);
		VTDecoderSessionEmitDecodedFrame_(d->session, frame, ERR_BAD_DATA, 0, NULL);
		return noErr;
	}
	while (at + (size_t)d->length_size <= total) {
		size_t unit = 0;

		for (int i = 0; i < d->length_size; i++) {
			unit = (unit << 8) | source[at + (size_t)i];
		}
		at += (size_t)d->length_size;
		if (unit > total - at) {
			break;
		}
		append(&d->stream, &out, &room, source + at, unit);
		at += unit;
	}
	d->stream_bytes = room;
	free(source);
	if (d->stream == NULL) {
		VTDecoderSessionEmitDecodedFrame_(d->session, frame, ERR_MALFUNCTION, 0, NULL);
		return noErr;
	}
	if (d->drained) {
		/*
		 * After FinishDelayedFrames (a seek, the end of a loop) the firmware's instance has finished: a new one starts at a key frame. What comes before
		 * that one has lost its references, and is dropped.
		 */
		CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
		CFDictionaryRef first = attachments != NULL && CFArrayGetCount(attachments) > 0 ? CFArrayGetValueAtIndex(attachments, 0) : NULL;
		CFBooleanRef not_sync = first != NULL ? CFDictionaryGetValue(first, kCMSampleAttachmentKey_NotSync) : NULL;

		if (not_sync != NULL && CFBooleanGetValue(not_sync)) {
			VTDecoderSessionEmitDecodedFrame_(d->session, frame, noErr, 2 /* kVTDecodeInfo_FrameDropped */, NULL);
			return noErr;
		}
		if (!open_block(d)) {
			say("hakios: mfc: the block would not open again after a drain\n");
			VTDecoderSessionEmitDecodedFrame_(d->session, frame, ERR_MALFUNCTION, 0, NULL);
			return noErr;
		}
	}
	uint32_t tag = d->next_tag++ & 0x7FFFFFFF;

	if (d->pending_set[tag % PENDING_MAX]) {
		/* the kernel shows every frame within its few buffers: this cannot be, but VideoToolbox must hear about each */
		VTDecoderSessionEmitDecodedFrame_(d->session, d->pending[tag % PENDING_MAX], ERR_BAD_DATA, 0, NULL);
	}
	d->pending[tag % PENDING_MAX] = frame;
	d->pending_set[tag % PENDING_MAX] = true;
	status = decode_unit(d, d->stream, out, tag, NULL);
	if (status == ERR_MALFUNCTION) {
		uint32_t i = tag % PENDING_MAX;

		d->pending_set[i] = false;
		VTDecoderSessionEmitDecodedFrame_(d->session, frame, ERR_MALFUNCTION, 0, NULL);
	} else if (status != noErr) {
		emit(d, tag, NULL);     /* (emits with no image and the dropped flag) */
	}
	return noErr;
}

static OSStatus
finish_delayed(void *decoder)
{
	struct mfc_decoder *d = storage(decoder);
	bool finished = false;

	for (int i = 0; d->opened && !finished && i < PENDING_MAX; i++) {
		if (decode_unit(d, NULL, 0, 0, &finished) != noErr) {
			break;
		}
	}
	d->drained = d->opened;
	/* what the block never showed comes back as dropped */
	for (int i = 0; i < PENDING_MAX; i++) {
		if (d->pending_set[i]) {
			d->pending_set[i] = false;
			VTDecoderSessionEmitDecodedFrame_(d->session, d->pending[i], noErr, 2 /* kVTDecodeInfo_FrameDropped */, NULL);
		}
	}
	return noErr;
}

static Boolean
can_accept(void *decoder, CMFormatDescriptionRef format)
{
	(void)decoder, (void)format;
	return false;           /* a new format is a new session (the block's instance is sized for one stream) */
}

static OSStatus
copy_supported(void *decoder, CFDictionaryRef *out)
{
	(void)decoder;
	*out = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	return noErr;
}

static void
finalize(void *object)
{
	struct mfc_decoder *d = CMBaseObjectGetDerivedStorage_(object);

	if (d->fd >= 0) {
		close(d->fd);   /* the kernel closes the instance */
	}
	for (unsigned i = 0; i < RECENT_FRAMES; i++) {
		if (d->recent[i] != NULL) {
			CVPixelBufferRelease(d->recent[i]);
		}
	}
	free(d->header);
	free(d->stream);
	if (d->matrix != NULL) {
		CFRelease(d->matrix);
	}
	if (d->primaries != NULL) {
		CFRelease(d->primaries);
	}
	if (d->transfer != NULL) {
		CFRelease(d->transfer);
	}
	if (d->spare != NULL) {
		CVPixelBufferRelease(d->spare);
	}
}

static const struct {
	uint64_t version;
	size_t storage;
	void *equal, *invalidate;
	void (*finalize)(void *);
	void *copy_description, *copy_property, *set_property, *barrier, *protocol;
} base_class = { 1, sizeof(struct mfc_decoder), NULL, NULL, finalize, NULL, NULL, NULL, NULL, NULL };

static const struct {
	uint64_t version;
	OSStatus (*start)(void *, void *, CMVideoFormatDescriptionRef);
	OSStatus (*decode)(void *, void *, CMSampleBufferRef, uint32_t, uint32_t *);
	OSStatus (*copy_supported)(void *, CFDictionaryRef *);
	void *set_properties, *copy_serializable;
	Boolean (*can_accept)(void *, CMFormatDescriptionRef);
	OSStatus (*finish)(void *);
	void *later[4];
} decoder_class = { 3, start_session, decode_frame, copy_supported, NULL, NULL, can_accept, finish_delayed, { NULL, NULL, NULL, NULL } };

static const struct { void *zero; const void *base, *derived; } vtable = { NULL, &base_class, &decoder_class };

static OSStatus
create(uint32_t codec, CFAllocatorRef allocator, void **out)
{
	int fd = open("/dev/hakios-mfc", O_RDWR);
	void *object = NULL;
	OSStatus status;

	if (fd < 0) {
		return ERR_COULD_NOT_FIND;      /* VideoToolbox goes on to the software decoder */
	}
	status = CMDerivedObjectCreate_(allocator, &vtable, VTVideoDecoderGetClassID_(), &object);
	if (status != noErr) {
		close(fd);
		return status;
	}
	struct mfc_decoder *d = CMBaseObjectGetDerivedStorage_(object);

	memset(d, 0, sizeof(*d));
	d->fd = fd;
	d->codec = codec == 'avc1' ? HAKIOS_MFC_H264 : HAKIOS_MFC_HEVC;
	*out = object;
	return noErr;
}

void
hakios_mfc_decoder_register(void)
{
	static const struct { uint32_t codec; CFStringRef id, name; } codecs[] = {
		{ 'avc1', CFSTR("com.apple.videotoolbox.videodecoder.avc.hakios.mfc"), CFSTR("H.264 (Exynos MFC)") },
		{ 'hvc1', CFSTR("com.apple.videotoolbox.videodecoder.hevc.hakios.mfc"), CFSTR("HEVC (Exynos MFC)") },
	};
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
	void *cm = dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_NOW | RTLD_GLOBAL);

	if (access("/dev/hakios-mfc", F_OK) != 0 || vt == NULL || cm == NULL) {
		return;
	}
#define LOOK(from, name) (name##_ = dlsym(from, #name))
	if (!LOOK(vt, VTVideoDecoderGetClassID) || !LOOK(vt, VTVideoDecoderGetCMBaseObject) || !LOOK(vt, VTDecoderSessionSetPixelBufferAttributes) ||
	    !LOOK(vt, VTDecoderSessionGetPixelBufferPool) || !LOOK(vt, VTDecoderSessionEmitDecodedFrame) || !LOOK(vt, VTRegisterVideoDecoderWithInfo) ||
	    !LOOK(cm, CMDerivedObjectCreate) || !LOOK(cm, CMBaseObjectGetDerivedStorage) || !LOOK(vt, kVTVideoDecoderList_DecoderID) ||
	    !LOOK(vt, kVTVideoDecoderList_DecoderName)) {
		say("hakios: mfc: something of VideoToolbox's is not where it was: the hardware decoder is not registered\n");
		return;
	}
	for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
		const void *keys[] = { *kVTVideoDecoderList_DecoderID_, *kVTVideoDecoderList_DecoderName_ };
		const void *values[] = { codecs[i].id, codecs[i].name };
		CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		OSStatus status = VTRegisterVideoDecoderWithInfo_(codecs[i].codec, info, create);

		CFRelease(info);
		say("hakios: mediaserverd: the MFC's decoder registered for '%c%c%c%c' (%d)\n", (codecs[i].codec >> 24) & 0xFF,
		    (codecs[i].codec >> 16) & 0xFF, (codecs[i].codec >> 8) & 0xFF, codecs[i].codec & 0xFF, (int)status);
	}
}
