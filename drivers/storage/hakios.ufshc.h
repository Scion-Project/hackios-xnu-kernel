/*
 * The kernel's own UFS driver (hakios.ufshc.c), as hakios.ufs.c uses it.
 *
 * ---- What it is ------------------------------------------------------------------------------
 *
 * One driver for a standard UFS host controller (UFSHCI, JESD223) with what a SoC does
 * differently behind a few hooks (struct ufshc_soc in hakios.ufshc.c):
 *
 *   pci          QEMU's -device ufs (Red Hat 1b36:0013, class 01/09/01), found on the PCI bus; the
 *                one controller this kernel may also bring up from reset. The test bench.
 *   sm7325       the POCO's (Qualcomm, ufshc@1d84000). Nothing of Qualcomm's own registers is
 *                needed for a controller UEFI has brought up.
 *   exynos9810   the Galaxy S9's (Samsung, ufs@11120000; the vendor's HCI registers at
 *                0x11121100). Its differences, from the S9's own kernel and from a mainline port
 *                to the S9+ (both GPL-2.0, facts only; the head of hakios.ufshc.c says which file
 *                each comes from): offsets and lengths in the request descriptor in bytes, not
 *                dwords; PRDT entries of 128 bytes or 16 (the protector's descriptor type, as UEFI
 *                left it: learnt from UEFI's own read), at most 4 KB each; a "nexus type" bit per
 *                slot to be set for a SCSI command and cleared for a query or NOP before the
 *                doorbell; UTRLCLR clears with ones; DMA below 4 GB. Taken as Silicium's UEFI left
 *                it, never reset; its power, reset line, clocks, UniPro and PHY read back first, in
 *                steps (ufs_native_step=, 1 by default: only look).
 *
 * Two ways of having the controller:
 *
 *   shared (ufs_native=1, the POCO)   UEFI keeps the controller and the loader keeps reading
 *                through it (ufs=2); this kernel puts a transfer request list of its own in for
 *                the run, in memory the loader offers, and gives UEFI's back at the end. Read only.
 *   own (ufs_native=4)   this kernel has the controller for the run, and the loader's block
 *                service is not used (no ufs=2): it finds the controller (PCI, or the SoC's
 *                address), takes it over from UEFI as UEFI left it (or, on QEMU only, enables it
 *                from reset), reads each logical unit's GPT itself and makes /dev/ufs_NAME of
 *                every partition, as the loader's service does. Writes go to the one partition
 *                ufs_native_write=NAME names, never to one on the list of those that must not
 *                be written (the S9's EFS, BOOT, RECOVERY, PARAM, ...), and the controller's
 *                own check refuses any write outside that partition's blocks.
 *
 * ---- ShengUfs and this driver, side by side ----------------------------------------------------
 *
 * UniXNU/ShengUfs (BSD-2-Clause, Copyright (c) 2026 blu-sharky; read at commit
 * f2d6a4a72f5e7be7d2c9b78eba16cf373868fa30) is a native XNU UFS driver for the SM8550 ("sheng"):
 * an IOService that adopts the controller UEFI left running and publishes the root image's
 * windows as IOBlockStorageDevices. Compared item by item:
 *
 *   transfer list    ShengUfs: its own list with one slot (slot 0), one command at a time, in a
 *                    16 KB DMA buffer of its own (list, command descriptor, 128 KB data area).
 *                    Here: 32 slots, slot 0 kept for the proof (shared) or the control commands
 *                    (own); reads in flight in up to 31 at once in shared mode, 15 in own mode,
 *                    which uses the even slots only (its descriptors are cached memory: two 32-byte
 *                    descriptors share a 64-byte line). ufs_native_bench=1 measures what that buys.
 *                    Kept ours.
 *   task list        neither uses UTMRL (no task management, no abort).
 *   UPIU building    the same layout: command UPIU at 0 of the command descriptor, response
 *                    UPIU after it, PRDT after that; READ(10) with the LBA and length in the CDB
 *                    and the expected length in the UPIU. ShengUfs has query reads (attributes,
 *                    flags) and NOP OUT; this driver had neither. ADOPTED (own mode): NOP OUT
 *                    before anything else, QUERY READ ATTRIBUTE bCurrentPowerMode, and START STOP
 *                    UNIT (power condition ACTIVE) to the device well-known unit 0xD0 when the
 *                    device was left in power-down -- ShengUfs found the SM8550's UFS left so by
 *                    its UEFI, every LU then answering NOT READY. Added beyond it: QUERY READ
 *                    DESCRIPTOR of each unit (bLUEnable, bBootLunID, bLUWriteProtect, block size
 *                    and count), so the logical units are found rather than assumed.
 *   doorbell         both ring with the list checked to be theirs and the slot free; ShengUfs
 *                    also refuses to touch the command descriptor while any doorbell bit is set
 *                    (one slot). Here the whitelist runs on every command, and what the slot then
 *                    holds is read back and checked again before the doorbell (kept).
 *   completion       both poll; neither enables the controller's interrupt (IE stays 0).
 *                    ShengUfs: a cleared doorbell bit alone is not taken as completion -- the
 *                    descriptor's OCS must also have been written back (not 0xF), for a
 *                    completion so fast the bit is never seen set, and for a controller that
 *                    clears the bit without running the command. ADOPTED. Here: a short spin,
 *                    then sleep, with one poller thread for all sleepers (kept: it keeps the
 *                    processors free while 31 reads are in flight).
 *   interrupt status ShengUfs's fatal set is ULLS | DFES | UTPES | HCFES | SBFES | CEFES (link
 *                    lost and crypto engine included, UIC error not: that one is often
 *                    recoverable); ours was UE | DFES | UTPES | HCFES | SBFES. ADOPTED theirs.
 *                    Before a ring ShengUfs acknowledges UTRCS only, leaving the error bits as
 *                    evidence: ADOPTED in own mode (shared mode writes nothing to IS, as before).
 *   response checks  ShengUfs checks the response UPIU's transaction type, task tag, LUN, that
 *                    there is no extra header segment and the data segment fits, the sense data
 *                    (fixed and descriptor format), and treats an overflow or underflow residual
 *                    with GOOD status as a protocol error. Ours checked type, tag, response and
 *                    status. ADOPTED all of theirs.
 *   SCSI             ShengUfs: READ(10) only, and WRITE(10) (with FUA) into at most two windows
 *                    of LUN 0 that its integration opens once, plus SYNCHRONIZE CACHE(10). Here:
 *                    READ(10) (READ(16) beyond 2^32 blocks), and in own mode WRITE(10/16) into the
 *                    one window of the partition ufs_native_write= names -- the same two-layer
 *                    idea as theirs (the partition layer refuses, and the controller layer
 *                    refuses again whatever lies outside the window, of any LUN), plus
 *                    SYNCHRONIZE CACHE(10) when the partition's device is closed and at the end.
 *   errors           ShengUfs: a timeout or fatal status quarantines the transport for good (no
 *                    cancel, no reset, no retry: the controller may still own the buffer). Ours
 *                    did the same ("the path is off", a slot that never came back is never
 *                    reused). Kept.
 *   takeover         ShengUfs (from UEFI): HCE 1 and HCS's four ready bits set, no transfer or
 *                    task request in flight, no fatal status; IE masked, the list stopped,
 *                    UTRLBA/UTRLBAU written and read back, IS cleared, the list started, its
 *                    ready bit waited for; UEFI's list is not given back. Ours (shared): the same
 *                    checks, IE not touched, and UEFI's list given back at the end and checked by
 *                    the loader. ADOPTED for own mode: the task list's doorbell checked as well,
 *                    IE saved, masked, and put back at the end with UEFI's list -- the loader and
 *                    UEFI go on using the storage after a run.
 *   DMA              ShengUfs: one physically contiguous buffer, refused above 4 GB when CAP's
 *                    64-bit addressing bit is clear; on the SM8550 it also has to give the UFS
 *                    stream a bypass context bank in the SMMU (the hypervisor leaves it faulting).
 *                    ADOPTED the 4 GB rule (and the Exynos 9810's 32-bit DMA mask makes it the
 *                    S9's rule whatever CAP says). The SMMU part is the SM8550's alone: neither
 *                    the POCO's UEFI (the transfers already work there) nor the S9 (no SysMMU in
 *                    front of the UFS in its device tree) needs it.
 *   GPT              ShengUfs reads LUN 0's GPT itself: header signature, revision and CRC,
 *                    entries CRC, usable range, every entry inside it, no two overlapping, no
 *                    name twice. ADOPTED (own mode: the loader's UEFI used to do this for us).
 *   what not         its root-image manifest, SHA-256 policies and IOMedia publication (this port
 *                    mounts APFS from a block device), its display and Wi-Fi SMMU work.
 *
 * No line of ShengUfs is in this file or in hakios.ufshc.c: what is adopted is the behaviour
 * above, written again for this driver. Its licence is kept with the repository's other third
 * parties (THIRD_PARTY_LICENSES.md) because the comparison above describes its code.
 */
#ifndef _HAKIOS_UFSHC_H_
#define _HAKIOS_UFSHC_H_

#include <stdbool.h>
#include <stdint.h>

/* what the loader's page says about it (struct loader_block's native sections): shared mode */
struct ufshc_loader {
	uint64_t hc_pa;                         /* the controller's registers; 0: not offered */
	uint64_t area_pa;                       /* the loader's memory for this kernel's list and command descriptors */
	uint32_t area_size, ucd_offset, ucd_size, slots;
	uint32_t uefi_lo, uefi_hi;              /* UEFI's list */
	volatile uint32_t *take_back;           /* the loader's: it takes the controller back, send nothing more */
	volatile uint32_t *list_in;             /* this kernel's: its list is in */
	uint32_t *switched_in, *switched_out;
	volatile uint32_t *cnr_at_switch;       /* this kernel's: UTRLCNR when its list went in */
};

/* a logical unit, as own mode found it */
struct ufshc_unit {
	bool present;                           /* enabled and answering */
	uint8_t boot_lun_id;                    /* bBootLunID: 0 for none, 1 or 2 for a boot LU */
	uint8_t write_protect;                  /* bLUWriteProtect */
	uint32_t block_size;
	uint64_t blocks;
};

#define UFSHC_LUNS              8               /* the general logical units, 0-7 */

/* shared mode (the POCO) */
bool ufshc_probe(const struct ufshc_loader *loader, bool only);
int ufshc_switch_in(bool prove);
int ufshc_switch_out(const char *why);
bool ufshc_list_in(void);

/* own mode */
bool ufshc_own_probe(void);
bool ufshc_own(void);
bool ufshc_own_stopped(void);
const struct ufshc_unit *ufshc_unit(uint8_t lun);
int ufshc_open_window(uint8_t lun, uint64_t first_lba, uint64_t blocks);
int ufshc_write(uint8_t lun, uint64_t offset, const uint8_t *src, uint64_t len);
int ufshc_sync(void);
int ufshc_selftest_refusal(uint8_t lun, uint64_t lba);

/* both */
bool ufshc_available(void);
void ufshc_set_luns(uint32_t mask);
int ufshc_read(uint8_t lun, uint64_t offset, uint8_t *dst, uint64_t len);
void ufshc_report(const char *when);
void hakios_ufs_quiesce(void);

#endif
