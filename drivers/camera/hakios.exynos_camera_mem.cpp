/*
 * The IOKit half of hakios.exynos_camera.c: its frame buffer's mapping. CSIS1's WDMA takes 32-bit addresses, and none of
 * this kernel's own memory is below 4 GB on the Galaxy S9 (the loader gives XNU the bank from 0x880000000), so the
 * buffer is physical memory in the lower bank that XNU does not manage: the gap of the phone's device tree above the
 * display's scan-out buffers (from 0xD4000000, hakios.exynos_dpu.c) and below secure_camera (0xE1900000);
 * hakios.exynos_camera.c chooses where and checks the range. It is mapped through an IOMemoryDescriptor of the
 * physical range, never ml_io_map(): io_submap, which ml_io_map() takes from, is only 8 MB, and a few MB of frame there
 * leave the next driver's registers no room (a panic). Write-combined (normal memory, not cached, unaligned reads
 * allowed), as the display maps its buffers there: what the DMA writes is what the processor reads, with no cache
 * maintenance needed.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

extern "C" vm_offset_t hakios_exynos_camera_map(uint64_t pa, uint64_t bytes);

extern "C" vm_offset_t
hakios_exynos_camera_map(uint64_t pa, uint64_t bytes)
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
	/* kept for the rest of the run: the descriptor and its map are never released (one buffer, mapped once) */
	return (vm_offset_t) map->getVirtualAddress();
}
