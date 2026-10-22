/* gfx_test's transport on iOS: /dev/vgpu3d (gfx_dev.c). */
#include <stdio.h>
#include <string.h>
#include "gfx/gfx_dev.h"

int
gfx_test_transport(struct gfx_transport *t)
{
	memset(t, 0, sizeof(*t));
	int err = gfx_dev_open(t);
	if (err) {
		printf("GFXTEST /dev/vgpu3d: %s\n", strerror(err));
	}
	return err;
}
