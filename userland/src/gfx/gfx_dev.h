#pragma once
#include "gfx.h"
/* /dev/vgpu3d as a gfx transport: 0 or an errno (gfx_dev.c). */
int gfx_dev_open(struct gfx_transport *t);
