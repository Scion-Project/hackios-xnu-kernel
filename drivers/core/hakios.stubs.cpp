/*
 * The IOKit half of drivers/core/hakios.stubs.c: what the rest of the kernel calls in a driver that is left out of it,
 * for the calls that are C++ (docs/drivers-config.md).
 */
#include "MiniIOSurface.h"

#ifndef HAKIOS_MALI
/* the display's question about the Mali's jobs (MiniIOSurfaceDrawn()): with no Mali driver, none draws into a surface */
bool
HakiosMaliDrawn(__unused MiniIOSurface * surface, __unused uint64_t milliseconds)
{
	return true;
}
#endif

#ifndef HAKIOS_ADRENO
/*
 * the display's driver asks whether the GPU is done with a surface before it shows it: with no Adreno, the Mali's
 * answer (drivers/gpu/hakios.mali.cpp, or "done" above when it is left out too)
 */
bool
MiniIOSurfaceDrawn(MiniIOSurface * surface, uint64_t milliseconds)
{
	return HakiosMaliDrawn(surface, milliseconds);
}
#endif
