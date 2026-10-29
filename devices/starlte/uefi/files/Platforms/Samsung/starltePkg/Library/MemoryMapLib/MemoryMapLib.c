// Project Silicium's Mu-Silicium, Platforms/Samsung/starltePkg/Library/MemoryMapLib/MemoryMapLib.c at the commit in uefi/BASE
// (BSD-2-Clause, Mu-Silicium's licence), with the region HakiOS's payload lives in.
#include <Library/MemoryMapLib.h>

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions
  {"UEFI FD",            0x8F000000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI Stack",         0x8F200000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"DXE Heap",           0x8F240000, 0x03C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"Display Reserved",   0xCC000000, 0x01043000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},
  {"Secure Camera",      0xE1900000, 0x01E00000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK},

  // HakiOS: where the boot shim copies the "XNUP" payload (the kernel, the root image, the command line) and the loader finds
  // it. It is the modem's region in Samsung's device tree (modem_if, 0xF0000000 + 0x9200000), unused here: Android's cbd loads
  // the modem's image there, the boot loader does not, and no modem is started under HakiOS.
  {"XNU Payload",        0xF0000000, 0x04000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK},
  // HakiOS: Samsung's pstore (exynos-snapshot's log_pstore), where the loader keeps its log across a restart for Android or
  // TWRP to read back (xnuloader/loader.c, write_ram_log): UEFI must not put anything of its own there.
  {"Samsung pstore",     0xFED10000, 0x00008000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK},

  // iRAM Regions
  {"IRAM",               0x02020000, 0x00018000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   DEVICE},
  {"NS_IRAM",            0x02038000, 0x00021000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},

  // Register Regions
  {"Chip Info",          0x10000000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICD",               0x10101000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICC",               0x10102000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICH",               0x10104000, 0x00002000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICV",               0x10106000, 0x00002000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SYSREG FSYS0",       0x11010000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFS HCI",            0x11120000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFS UniPro",         0x11130000, 0x00008000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PMU",                0x14060000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Clock Controller",   0x1A240000, 0x00002000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Decon",              0x16030000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
