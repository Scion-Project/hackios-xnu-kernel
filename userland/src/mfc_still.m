/*
 * mfc_still THREADS ROUNDS FILE...: HEIC stills through ImageIO, the way an application shows one: every picture decoded at once into 8-bit RGBA; a CRC
 * of its pixels (the same with the hardware and without, or the decoder lies), how long it took, how many failed.
 */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

static int rounds, nfiles, failed, done;
static char **files;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *
work(void *arg)
{
	long who = (long)arg;

	for (int r = 0; r < rounds; r++) {
		for (int i = 0; i < nfiles; i++) @autoreleasepool {
			NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:files[i]]];
			CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, NULL);
			NSDictionary *o = @{ (id)kCGImageSourceShouldCacheImmediately: @YES };
			CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
			CGImageRef image = source != NULL ? CGImageSourceCreateImageAtIndex(source, 0, (__bridge CFDictionaryRef)o) : NULL;
			uint32_t crc = 0;
			size_t w = 0, h = 0;

			if (image != NULL) {
				w = CGImageGetWidth(image);
				h = CGImageGetHeight(image);
				uint8_t *pixels = calloc(w * h, 4);
				CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
				CGContextRef context = CGBitmapContextCreate(pixels, w, h, 8, w * 4, space, kCGImageAlphaPremultipliedLast);

				CGContextDrawImage(context, CGRectMake(0, 0, w, h), image);
				crc = (uint32_t)crc32(0, pixels, (uInt)(w * h * 4));
				CGContextRelease(context);
				CGColorSpaceRelease(space);
				free(pixels);
				CGImageRelease(image);
			}
			double ms = (CFAbsoluteTimeGetCurrent() - t0) * 1000;

			pthread_mutex_lock(&lock);
			done++;
			if (image == NULL) {
				failed++;
			}
			printf("mfc_still: [%ld] %s: %zux%zu CRC %08x in %.0f ms\n", who, files[i], w, h, crc, ms);
			fflush(stdout);
			pthread_mutex_unlock(&lock);
			if (source != NULL) {
				CFRelease(source);
			}
		}
	}
	return NULL;
}

int
main(int argc, char **argv)
{
	if (argc < 4) {
		return 1;
	}
	int threads = atoi(argv[1]);
	pthread_t t[16];

	rounds = atoi(argv[2]);
	files = argv + 3;
	nfiles = argc - 3;
	for (long i = 0; i < threads; i++) {
		pthread_create(&t[i], NULL, work, (void *)i);
	}
	for (int i = 0; i < threads; i++) {
		pthread_join(t[i], NULL);
	}
	printf("mfc_still: %d pictures, %d failed\n", done, failed);
	return 0;
}
