# Android beside iOS on the Galaxy S9 (design, nothing run)

> **Nothing on this page has been run on a phone.** The chooser, the partition plan and the write fence below were written on
> a computer and built or tested there only (what was checked, and what was not, is listed at the end). No phone was attached,
> nothing was flashed. Treat every step as an untested plan, and do the first run on a spare phone. (The POCO X5 Pro's dual boot, which has been
> run, is [another page](dual-boot.md).)

Goal: iOS and Android installed on the same S9 (SM-G960F), chosen at power-on, with Android's own `/data`, TWRP and factory reset
working as on a stock phone. Today's Scion install puts iOS's root in `SYSTEM` and its `/private/var` in `USERDATA`; Android has no system
partition. Everything below is off unless asked for.

## The pieces

| Piece | Where | State |
|---|---|---|
| Boot chooser: hold Volume Up for Android, else iOS | `xnuloader/chooser.c`, build with `CHOOSER=1` | compiles and links; pure part host-tested; **not run** |
| Partition plan | scion-installer `tools/gpt-plan.py` | dry run only; host-tested; **no write mode** |
| Write fence by GUID | loader arg `ufs_write_guid=<GUID>` with `ufs_write=1` | compiles; GUID parser host-tested; **not run** |

## The layout

1. `BOOT` holds Scion's boot image, with the chooser built in. `RECOVERY` (TWRP) is **never** used for it.
2. `SYSTEM`, `VENDOR`, `ODM` go back to Android's (public LineageOS images for starlte). `USERDATA` keeps its name and its first LBA and
   is **shrunk**: it is Android's `/data`.
3. New partitions from USERDATA's old tail, 1 MiB aligned, with Scion's own type GUID: `SCIONSTORE` (256 MiB: Android's kernel, ramdisk and
   command line as one Android boot image, starting at its first block), `SCIONROOT` (iOS's system, 6 GiB) and `SCIONVAR` (iOS's `/private/var`).
4. iOS writes only to `SCIONVAR`, found by its unique GUID: `ufs_write=1 ufs_write_guid=<GUID>`, with `ufsroot` and `var_hfs` naming the new
   partitions. Without `ufs_write_guid=` the by-name rule (USERDATA) is unchanged.
5. Never touched: EFS, CPEFS, PARAM, UP_PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA0/1, DTB, DTBO, BOOT, RECOVERY and the boot LUNs.

## The chooser

At power-on S-Boot runs first and its own key combos still win: Volume Up + Bixby + Power is recovery, Volume Down + Bixby + Power is Download mode.
S-Boot then starts `BOOT`, UEFI, and this loader. The loader prints one line, *S C I O N hold Volume Up for Android, else iOS*, and looks at
Volume Up for 1.5 s (`CHOOSER_WINDOW_MS`; S-Boot and UEFI take seconds to get here, so the key is read for a window, not once). Held (three
samples in a row) starts Android. Anything else, or any failure on the Android side, boots the payload exactly as without the chooser.
It writes no partition: its only storage call is a read, and `tools/chooser-test.sh` fails if a write call appears in it.

Android's side reads an Android boot image (header versions 0 to 2) from `SCIONSTORE`, puts the raw arm64 Image, the ramdisk and the device tree at fixed
RAM addresses, gives the tree `bootargs` and the ramdisk range, cleans the data cache, turns the MMU and caches off, and jumps with x0 = the tree,
x1-x3 = 0, at EL1.

Facts used: the key registers (GPA0 data 0x14050024, Volume Up bit 3, active low) and the three addresses (kernel 0x92080000, ramdisk
0xA8000000, tree 0xAD000000) are Starbite's (`shim/starlte`, GPL-2.0-only, read for the numbers only; no code copied). The boot image
header is AOSP's public format. Blazer's chooser has no licence and was a design reference only.

**Guesses, none checked on a phone:** that Volume Up alone reaches the loader and is still held (or pressed) in the window; that the three
addresses are free in this UEFI's memory map (the loader asks for them and gives up if not); that the tree in the image needs only `/chosen` set
(Samsung's S-Boot does more to it); that a Linux 4.9 kernel starts from UEFI's state (cores, timer, interrupt controller) as it does from S-Boot's;
that `SCIONSTORE` is the right place. A gzip'd kernel is refused, not unpacked.

## Order of tests

Each step must pass before the next. 1 to 5 write nothing to a phone.

1. **Read only.** Dump LUN 0's table (`dd` of the first and last 64 blocks), run `sgdisk --verify`, record every partition's GUID, compare with the PIT.
   Run `tools/gpt-plan.py` on the dump. It prints the plan and writes nothing.
2. **Back up, read only:** EFS, CPEFS, PARAM, UP_PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOOT, RECOVERY, and USERDATA's first and last MiB. Check the hashes. Keep the copies off the phone. Never restore one phone's EFS on another (the IMEI).
3. **The key alone**, in a RAM-only or BOOT-only image of the loader built with `CHOOSER=1` and no `SCIONSTORE`: Volume Up must print the Android line and then fall back to iOS. Recovery from a bad BOOT is Download mode.
4. **Android through the loader with no table change:** Lineage's pieces and the three system partitions on a spare S9, the boot image in a spare partition named in `CHOOSER_PART`; Android must reach the lock screen. Mind that the BOOT image header's patch level must stay at or above Android's, or keymaster refuses `/data`'s keys.
5. **Dry-run the plan on a 4096-byte-sector loop device** (`sgdisk -p` and `-v`; not done yet), then apply it on the spare phone from TWRP, by hand.
6. **iOS with `ufs_write_guid=`:** the fence refuses a write anywhere else (the loader logs `REFUSED`).
7. Only then a phone with data on it, with an attended way back.

## Recovery

- Download mode and TWRP are S-Boot's, and are not changed: Volume Down + Bixby + Power, Volume Up + Bixby + Power. Keep `RECOVERY` intact.
- A bad `BOOT`: flash the saved copy from Download mode.
- A bad table: write the old blocks back from the backup (the plan lists the blocks it would touch and their SHA-256 now, to check the backup against), or `sgdisk --load-backup` of the saved table, from TWRP. Never use Odin's Re-Partition, and never write by name past `SYSTEM` once the table differs from the PIT; a BL flash writes PARAM, UP_PARAM and KEYSTORAGE, so back those up first.
- A phone that lost Android's data: shrinking USERDATA invalidates it. Back it up and restore, or factory-reset from recovery.

## What was and was not checked

- **Checked, on a computer:** `tools/chooser-test.sh` (key decode and debounce, boot image header parse, the device tree edit, no storage write in the chooser). `python3 -I tools/test_gpt_plan.py` in scion-installer (the plan, its refusals, nothing written). The loader builds with clang, with `CHOOSER=1` and without; without it the loader's code section is byte-identical to the one before this change.
- **Not checked:** anything on a phone. `sgdisk -v` on the planned table (needs a 4096-byte-sector loop device and root). The whole-kernel build gate. QEMU. Whether the handoff boots Android.
