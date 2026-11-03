/*
 * The IOKit half of hakios.ufshc.c: the mapping of own mode's memory on the Galaxy S9. The controller is kept to
 * 32-bit addresses, and none of this kernel's own memory is below 4 GB on the phone (the loader gives XNU the bank
 * from 0x880000000), so the list, the command descriptors and the slots' buffers are physical memory of the lower
 * bank that XNU does not manage, in the device tree's gap below secure_camera (hakios.ufshc.c, UFSHC_S9_MEM_PA, says
 * where and why there). Mapped through an IOMemoryDescriptor of the physical range, never ml_io_map(): io_submap,
 * which ml_io_map() takes from, is only 8 MB. Write-combined (normal memory, not cached), as the camera's and the
 * display's buffers are: what the controller writes is what the processor reads.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

extern "C" vm_offset_t hakios_ufshc_map(uint64_t pa, uint64_t bytes);

extern "C" vm_offset_t
hakios_ufshc_map(uint64_t pa, uint64_t bytes)
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
