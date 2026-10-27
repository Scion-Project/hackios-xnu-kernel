/* gfx_test's transport on the Mac: the emulator's gfxstream (host.c). */
#include <stdlib.h>
#include "host.h"

int
gfx_test_transport(struct gfx_transport *t)
{
	/* renderer flags: EGL (the emulator's ANGLE), GLES, surfaceless; GFXSTREAM_FLAGS overrides */
	const char *f = getenv("GFXSTREAM_FLAGS");
	if (host_init(f ? (uint32_t)strtoul(f, NULL, 0) : (1u << 0) | (1u << 3) | (1u << 4)) != 0) {
		return -1;
	}
	return host_open(t);
}
