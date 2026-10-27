#pragma once
#include <stdint.h>
#include "../../../userland/src/gfx/gfx.h"
int host_init(uint32_t renderer_flags);
int host_open(struct gfx_transport *t);
