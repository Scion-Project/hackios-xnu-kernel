/*
 * hakios_physmap_wc(): physical memory that XNU does not manage, mapped into the kernel write-combined, once
 * (sys/hakios.physmap.h says when and why). One copy of what the camera's (hakios.exynos_camera.c) and UFS's own mode
 * (hakios.ufshc.c) each had in a file of their own.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

/* (declared for C in sys/hakios.physmap.h, which the IOKit side of the build does not see) */
extern "C" vm_offset_t hakios_physmap_wc(uint64_t pa, uint64_t bytes);

extern "C" vm_offset_t
hakios_physmap_wc(uint64_t pa, uint64_t bytes)
{
	IOMemoryDescriptor * memory;
	IOMemoryMap * map;

	memory = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress) pa, (IOByteCount) bytes, kIODirectionInOut);
	if (memory == NULL) {
		return 0;
	}
	map = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
	if (map == NULL) {
		memory->release();
		return 0;
	}
	/* kept for the rest of the run: the descriptor and its map are never released (mapped once) */
	return (vm_offset_t) map->getVirtualAddress();
}
