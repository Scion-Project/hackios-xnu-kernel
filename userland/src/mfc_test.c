/*
 * mfc_test FILE [y]: a raw H.264 file (Annex B) through /dev/hakios-mfc: frames, time, the CPU time of it, and the CRC of each frame
 * shown with y. For the console: what the block does with no VideoToolbox around it.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "hakios.mfc.h"

static uint32_t
crc(uint32_t c, const uint8_t *p, size_t n)
{
	c = ~c;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
		}
	}
	return ~c;
}

static double
now(void)
{
	struct timeval t;

	gettimeofday(&t, NULL);
	return t.tv_sec + t.tv_usec / 1e6;
}

/* where the next access unit starts: the next first slice (first_mb 0), or parameter set before one */
static size_t
next_unit(const uint8_t *s, size_t n, size_t at)
{
	int slice_seen = 0;

	for (size_t i = at; i + 4 < n; i++) {
		if (s[i] != 0 || s[i + 1] != 0 || s[i + 2] != 1) {
			continue;
		}
		int type = s[i + 3] & 31;

		if (type == 1 || type == 5) {
			if (slice_seen && (s[i + 4] & 0x80)) {
				return i > at && s[i - 1] == 0 ? i - 1 : i;
			}
			slice_seen = 1;
		} else if (slice_seen && (type == 7 || type == 8 || type == 6 || type == 9)) {
			return i > at && s[i - 1] == 0 ? i - 1 : i;
		}
	}
	return n;
}

int
main(int argc, char **argv)
{
	int fd = open("/dev/hakios-mfc", O_RDWR), show = argc > 2, file;
	struct stat st;
	uint8_t *s, *y, *uv;
	struct hakios_mfc_open o;
	size_t at = 0, end;
	unsigned frames = 0, shown = 0;
	struct rusage a, b;
	double t0;

	if (fd < 0 || argc < 2 || (file = open(argv[1], O_RDONLY)) < 0 || fstat(file, &st) != 0) {
		perror("mfc_test: /dev/hakios-mfc or the file");
		return 1;
	}
	s = malloc((size_t)st.st_size);
	if (read(file, s, (size_t)st.st_size) != st.st_size) {
		perror("read");
		return 1;
	}
	end = next_unit(s, (size_t)st.st_size, 0);
	memset(&o, 0, sizeof(o));
	o.codec = HAKIOS_MFC_H264;
	o.header = (uint64_t)(uintptr_t)s;
	o.header_bytes = (uint32_t)end;
	if (ioctl(fd, HAKIOS_MFC_OPEN, &o) != 0) {
		perror("HAKIOS_MFC_OPEN");
		return 1;
	}
	printf("mfc_test: %ux%u\n", o.width, o.height);
	y = malloc(o.width * o.height);
	uv = malloc(o.width * o.height / 2);
	getrusage(RUSAGE_SELF, &a);
	t0 = now();
	for (;;) {
		struct hakios_mfc_decode d;

		memset(&d, 0, sizeof(d));
		if (at < (size_t)st.st_size) {
			end = next_unit(s, (size_t)st.st_size, at);
			d.stream = (uint64_t)(uintptr_t)(s + at);
			d.bytes = (uint32_t)(end - at);
			at = end;
			d.tag = frames++;
		}
		d.y = (uint64_t)(uintptr_t)y;
		d.uv = (uint64_t)(uintptr_t)uv;
		d.y_stride = d.uv_stride = o.width;
		if (ioctl(fd, HAKIOS_MFC_DECODE, &d) != 0) {
			perror("HAKIOS_MFC_DECODE");
			break;
		}
		if (d.shown) {
			shown++;
			if (show) {
				printf("frame %u: crc %#x\n", d.shown_tag, crc(crc(0, y, o.width * o.height), uv, o.width * o.height / 2));
			}
		}
		if (d.bytes == 0 && (d.finished || !d.shown)) {
			break;
		}
	}
	double seconds = now() - t0;
	getrusage(RUSAGE_SELF, &b);
	double cpu = (b.ru_utime.tv_sec - a.ru_utime.tv_sec) + (b.ru_utime.tv_usec - a.ru_utime.tv_usec) / 1e6 +
	    (b.ru_stime.tv_sec - a.ru_stime.tv_sec) + (b.ru_stime.tv_usec - a.ru_stime.tv_usec) / 1e6;
	printf("mfc_test: %u frames in, %u shown, in %.2f s (%.1f frames/s); CPU %.2f s (%.0f%% of one core)\n", frames, shown, seconds,
	    shown / seconds, cpu, cpu * 100 / seconds);
	return 0;
}
