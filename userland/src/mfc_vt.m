/*
 * mfc_vt: VideoToolbox's decoder directly, every frame's CRC against the list ffmpeg's frames made (FILE.crc): what the block decodes, frame by frame.
 *   mfc_vt plain FILE            all frames, as fast as they go
 *   mfc_vt paced FILE            all frames, at the clip's own rate
 *   mfc_vt two FILE FILE2        two sessions at once, turn about: the second goes to software (mediaserverd's log says which)
 *   mfc_vt seek FILE             60 frames, FinishDelayedFrames, then on from a later key frame
 *   mfc_vt cont FILE             60 frames, FinishDelayedFrames, then on from the very next frame, a key frame or not
 */
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <zlib.h>

struct run {
	const char *name;
	uint32_t *expected;
	int count;
	int shown, right, wrong, dropped, errors, reported;
	double fps;
	VTDecompressionSessionRef session;
	NSMutableArray *samples;
};

static uint32_t
crc_of(CVPixelBufferRef image)
{
	uLong c = crc32(0, NULL, 0);
	size_t height = CVPixelBufferGetHeight(image), width = CVPixelBufferGetWidth(image);

	CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
	for (size_t plane = 0; plane < 2; plane++) {
		const uint8_t *base = CVPixelBufferGetBaseAddressOfPlane(image, plane);
		size_t stride = CVPixelBufferGetBytesPerRowOfPlane(image, plane), rows = plane == 0 ? height : height / 2;

		for (size_t y = 0; y < rows; y++) {
			c = crc32(c, base + y * stride, (uInt)width);
		}
	}
	CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
	return (uint32_t)c;
}

static void
output(void *ref, void *frame, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image, CMTime pts, CMTime duration)
{
	struct run *r = ref;

	(void)frame, (void)duration;
	if (status != noErr) {
		if (r->errors++ < 5) {
			printf("mfc_vt: %s: status %d\n", r->name, (int)status);
		}
		return;
	}
	if (image == NULL || (flags & kVTDecodeInfo_FrameDropped)) {
		r->dropped++;
		return;
	}
	int index = (int)(CMTimeGetSeconds(pts) * r->fps + 0.5);
	uint32_t crc = crc_of(image);

	if (r->shown++ == 0) {
		OSType f = CVPixelBufferGetPixelFormatType(image);

		printf("mfc_vt: %s: the first frame: %zux%zu '%c%c%c%c' IOSurface %d\n", r->name, CVPixelBufferGetWidth(image), CVPixelBufferGetHeight(image), (f >> 24) & 255, (f >> 16) & 255, (f >> 8) & 255, f & 255, CVPixelBufferGetIOSurface(image) != NULL);
		fflush(stdout);
	}
	if (index >= 0 && index < r->count && r->expected[index] == crc) {
		r->right++;
	} else {
		r->wrong++;
		if (r->reported++ < 5) {
			printf("mfc_vt: %s: frame %d: CRC %08x, ffmpeg's %08x\n", r->name, index, crc, index >= 0 && index < r->count ? r->expected[index] : 0);
		}
	}
}

static void
load(struct run *r, const char *path)
{
	NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
	AVURLAsset *asset = [AVURLAsset assetWithURL:url];
	AVAssetTrack *track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
	NSError *error = nil;
	AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
	AVAssetReaderTrackOutput *out = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:nil];
	char crcs[512];
	FILE *f;
	CMSampleBufferRef sample;

	r->name = path;
	r->fps = track.nominalFrameRate;
	r->samples = [NSMutableArray array];
	[reader addOutput:out];
	[reader startReading];
	while ((sample = [out copyNextSampleBuffer]) != NULL) {
		if (CMSampleBufferGetNumSamples(sample) > 0) {
			[r->samples addObject:(__bridge id)sample];
		}
		CFRelease(sample);
	}
	snprintf(crcs, sizeof(crcs), "%s.crc", path);
	r->expected = calloc(100000, 4);
	if ((f = fopen(crcs, "r")) != NULL) {
		unsigned v;

		while (r->count < 100000 && fscanf(f, "%x", &v) == 1) {
			r->expected[r->count++] = v;
		}
		fclose(f);
	}
	CMFormatDescriptionRef format = (__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
	VTDecompressionOutputCallbackRecord callback = { output, r };
	NSDictionary *want = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange), (id)kCVPixelBufferIOSurfacePropertiesKey: @{} };
	OSStatus status = VTDecompressionSessionCreate(NULL, format, NULL, (__bridge CFDictionaryRef)want, &callback, &r->session);
	CFBooleanRef hw = NULL;

	VTSessionCopyProperty(r->session, CFSTR("UsingHardwareAcceleratedVideoDecoder"), NULL, &hw);
	printf("mfc_vt: %s: %lu samples, %.2f fps, %d CRCs; session %d, 'hardware' says %d\n", path, (unsigned long)r->samples.count, r->fps, r->count, (int)status, hw != NULL ? CFBooleanGetValue(hw) : -1);
	fflush(stdout);
}

static void
feed(struct run *r, NSUInteger i)
{
	CMSampleBufferRef sample = (__bridge CMSampleBufferRef)r->samples[i];
	VTDecodeInfoFlags info = 0;
	OSStatus status = VTDecompressionSessionDecodeFrame(r->session, sample, kVTDecodeFrame_EnableAsynchronousDecompression, NULL, &info);

	if (status != noErr && r->errors++ < 5) {
		printf("mfc_vt: %s: decode of %lu: %d\n", r->name, (unsigned long)i, (int)status);
	}
}

static bool
is_key(struct run *r, NSUInteger i)
{
	CFArrayRef a = CMSampleBufferGetSampleAttachmentsArray((__bridge CMSampleBufferRef)r->samples[i], false);
	CFDictionaryRef d = a != NULL && CFArrayGetCount(a) > 0 ? CFArrayGetValueAtIndex(a, 0) : NULL;
	CFBooleanRef n = d != NULL ? CFDictionaryGetValue(d, kCMSampleAttachmentKey_NotSync) : NULL;

	return n == NULL || !CFBooleanGetValue(n);
}

static void
summary(struct run *r, double seconds)
{
	VTDecompressionSessionFinishDelayedFrames(r->session);
	VTDecompressionSessionWaitForAsynchronousFrames(r->session);
	printf("mfc_vt: %s: %d frames shown, %d right, %d WRONG, %d dropped, %d errors, of %lu samples, %.2f s\n", r->name, r->shown, r->right, r->wrong, r->dropped, r->errors, (unsigned long)r->samples.count, seconds);
	fflush(stdout);
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 3) {
			return 1;
		}
		static struct run a, b;
		load(&a, argv[2]);
		double t0 = CACurrentMediaTime();

		if (strcmp(argv[1], "plain") == 0 || strcmp(argv[1], "paced") == 0) {
			bool paced = argv[1][0] == 'p' && argv[1][1] == 'a';

			for (NSUInteger i = 0; i < a.samples.count; i++) {
				feed(&a, i);
				if (paced) {
					usleep((useconds_t)(1000000 / a.fps));
				}
			}
		} else if (strcmp(argv[1], "two") == 0 && argc > 3) {
			load(&b, argv[3]);
			for (NSUInteger i = 0; i < a.samples.count || i < b.samples.count; i++) {
				if (i < a.samples.count) {
					feed(&a, i);
				}
				if (i < b.samples.count) {
					feed(&b, i);
				}
			}
			summary(&b, CACurrentMediaTime() - t0);
			VTDecompressionSessionInvalidate(b.session);
		} else {
			bool cont = strcmp(argv[1], "cont") == 0;
			NSUInteger next = 60;

			for (NSUInteger i = 0; i < 60 && i < a.samples.count; i++) {
				feed(&a, i);
			}
			VTDecompressionSessionFinishDelayedFrames(a.session);
			VTDecompressionSessionWaitForAsynchronousFrames(a.session);
			printf("mfc_vt: after 60: %d shown, %d right, %d wrong, %d dropped\n", a.shown, a.right, a.wrong, a.dropped);
			fflush(stdout);
			if (!cont) {
				for (next = 120; next < a.samples.count && !is_key(&a, next); next++) {
				}
				printf("mfc_vt: on from the key frame at sample %lu\n", (unsigned long)next);
			}
			for (NSUInteger i = next; i < a.samples.count; i++) {
				feed(&a, i);
			}
		}
		summary(&a, CACurrentMediaTime() - t0);
		VTDecompressionSessionInvalidate(a.session);
	}
	return 0;
}
