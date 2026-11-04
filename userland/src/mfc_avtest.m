/*
 * mfc_avtest FILE [SECONDS]: a movie played through AVPlayer the way an application plays one (the decoding is in mediaserverd, VideoToolbox's),
 * every frame taken as a pixel buffer. Says how many frames came, their format, and the CPU time mediaserverd and this program spent: run with
 * mfc= and without for hardware against software.
 */
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>
#include <mach/mach_time.h>

extern int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);
extern int proc_listallpids(void *buffer, int size);
extern int proc_name(int pid, void *buffer, uint32_t size);

static uint64_t
cpu_ns(pid_t pid)
{
	struct rusage_info_v2 info;

	if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *)&info) != 0) {
		return 0;
	}
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);      /* (rusage_info's times are mach ticks: 26 MHz on the phone) */
	}
	return (info.ri_user_time + info.ri_system_time) * base.numer / base.denom;
}

/* zlib's CRC of the visible picture, luma then chroma, as ffmpeg -f rawvideo -pix_fmt nv12 lays it out */
static uint32_t
frame_crc(CVPixelBufferRef image)
{
	uint32_t c = 0xFFFFFFFFu;
	size_t height = CVPixelBufferGetHeight(image), width = CVPixelBufferGetWidth(image);

	CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
	for (size_t plane = 0; plane < 2; plane++) {
		const uint8_t *base_address = CVPixelBufferGetBaseAddressOfPlane(image, plane);
		size_t stride = CVPixelBufferGetBytesPerRowOfPlane(image, plane), rows = plane == 0 ? height : height / 2;

		for (size_t y = 0; y < rows; y++) {
			const uint8_t *p = base_address + y * stride;

			for (size_t x = 0; x < width; x++) {
				c ^= p[x];
				for (int k = 0; k < 8; k++) {
					c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
				}
			}
		}
	}
	CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
	return ~c;
}

static pid_t
find_pid(const char *name)
{
	static int pids[4096];
	int n = proc_listallpids(pids, sizeof(pids));

	for (int i = 0; i < n; i++) {
		char got[64];

		if (proc_name(pids[i], got, sizeof(got)) > 0 && strcmp(got, name) == 0) {
			return pids[i];
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 2) {
			fprintf(stderr, "usage: mfc_avtest FILE [SECONDS]\n");
			return 1;
		}
		int seconds = argc > 2 ? atoi(argv[2]) : 20;
		NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
		AVPlayerItem *item = [AVPlayerItem playerItemWithURL:url];
		AVPlayerItemVideoOutput *output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:@{
			(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
		}];
		AVPlayer *player = [AVPlayer playerWithPlayerItem:item];
		pid_t media = find_pid("mediaserverd");
		uint64_t m0 = 0, s0 = 0, frames = 0, crc_frames = 0;
		size_t width = 0, height = 0;
		OSType format = 0;
		double begun = 0;

		[item addOutput:output];
		player.actionAtItemEnd = AVPlayerActionAtItemEndNone;
		player.muted = YES;
		[player play];
		printf("mfc_avtest: %s, mediaserverd is %d\n", argv[1], media);
		fflush(stdout);
		for (int tick = 0; tick < seconds * 100; tick++) {
			CMTime when = [output itemTimeForHostTime:CACurrentMediaTime()];

			if ([output hasNewPixelBufferForItemTime:when]) {
				CVPixelBufferRef image = [output copyPixelBufferForItemTime:when itemTimeForDisplay:NULL];

				if (image != NULL) {
					if (frames == 0) {
						width = CVPixelBufferGetWidth(image);
						height = CVPixelBufferGetHeight(image);
						format = CVPixelBufferGetPixelFormatType(image);
						begun = CACurrentMediaTime();
						m0 = cpu_ns(media);
						s0 = cpu_ns(getpid());
						printf("mfc_avtest: the first frame: %zux%zu, '%c%c%c%c', CRC %#x\n", width, height, (format >> 24) & 255, (format >> 16) & 255,
						    (format >> 8) & 255, format & 255, frame_crc(image));
						fflush(stdout);
					}
					frames++;
					CVPixelBufferRelease(image);
				}
			}
			if (CMTimeCompare(item.currentTime, item.duration) >= 0 && CMTIME_IS_NUMERIC(item.duration)) {
				[item seekToTime:kCMTimeZero completionHandler:nil];
			}
			[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
			if (tick % 500 == 499) {
				printf("mfc_avtest: %llu frames so far; status %ld, rate %.1f, error %s\n", frames, (long)item.status, player.rate,
				    item.error ? item.error.localizedDescription.UTF8String : "none");
				fflush(stdout);
			}
		}
		double elapsed = CACurrentMediaTime() - begun;
		double media_s = (cpu_ns(media) - m0) / 1e9, self_s = (cpu_ns(getpid()) - s0) / 1e9;

		printf("mfc_avtest: %llu frames in %.1f s (%.1f frames/s), %zux%zu; mediaserverd CPU %.2f s (%.0f%% of a core), this program's %.2f s (%.0f%%)\n",
		    frames, elapsed, frames / elapsed, width, height, media_s, media_s * 100 / elapsed, self_s, self_s * 100 / elapsed);
		(void)crc_frames;
		fflush(stdout);
	}
	return 0;
}
