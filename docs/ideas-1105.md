# Ideas of 5 November: findings

Findings only; nothing here is built or booted. Where a fact is from memory or a public figure and not checked in this tree, it says so.

## Touch report rate

- **Now:** `drivers/input/touch/hakios.s6sy761.c` polls the chip's IRQ pin every `POLL_MS` = 4 ms (at most 250 polls a second) and reads the event queue (0x60/0x61)
  when the pin is low. The chip's own scan and report rate is whatever its firmware sets; nothing here sets or reads it. It has not been measured:
  `s6sy761_trace=N` prints the first N finger events with a millisecond stamp, and no run log in hand has one (`usb-1105.log` has none).
- **A higher one:** Samsung's GPL `sec_ts` driver for this phone (`drivers/input/touchscreen/sec_ts/sec_ts.h` in the starlte kernel, read for the facts)
  names a command `SET_TS_CMD_SET_REPORT_RATE` = 0x34, beside `SET_TS_CMD_SET_CHARGER_MODE` 0x32 and `SET_TS_CMD_SET_NOISE_MODE` 0x33. The driver
  never sends it, and no value, size or meaning is given anywhere in that source. The same opcode is in the Note 10 (y771) copy, again unused.
- **Decision:** no argument. A write of 0x34 would be a guess at an undocumented register of a chip with unknown firmware. What would settle it: boot with
  `s6sy761_trace=200`, swipe, and read the spacing of the `moved` events. If they come at about 4 ms, the poll is the limit and a shorter `POLL_MS` is safe
  and needs no chip write; if at 8 to 17 ms, the chip is the limit and only the firmware (not ours to write, see the hard rules) could change it.

## Dark mode and a black wallpaper by default

- **Dark:** iOS keeps the appearance in a preference of `com.apple.UIKit` (`UserInterfaceStyleMode`, as far as I know: not checked, no Apple files here
  and nothing booted). A first-boot job could write it with CoreFoundation's public `CFPreferencesSetValue` in the mobile user's domain, as `selftest` runs
  as a job. Not implemented: I cannot confirm the key or its values (which number means dark) without a booted system, and a wrong guess changes nothing visible.
  To settle it: on a booted phone, change the appearance in Settings and diff `/private/var/mobile/Library/Preferences/com.apple.UIKit.plist`.
- **Black wallpaper:** the wallpaper here is drawn live by SpringBoard/PaperBoard from the stock Darwin wallpaper (`inject_sb.c` notes that no picture of it exists for the
  switcher). A black one means either writing the wallpaper store (private layout, Apple's format) or hooking the provider in `inject_sb.c`, as the switcher's wash already is.
  The hook is the clean way and is a change in code we own; it needs the booted phone to try.
- **Benefit (an estimate, not measured):** the panel is AMOLED (S6E3HA8), so a pixel's power follows its brightness. Published tests of dark UIs on OLED phones
  (e.g. Purdue 2021 on a Pixel 2/Galaxy class phone, from memory) report single-digit percent display-power savings at 30 to 50 % brightness and
  tens of percent at full brightness, for apps that go to near-black. A mostly-dark home screen and Settings might save a few percent of total phone power in
  use; a fully black wallpaper helps only while the home screen or lock screen is on. Nothing about this has been measured on this phone.

## Heat-aware Low Power Mode

- **What we have:** the kernel's thermal service gives a pressure level 0 to 4 in `kern.hakios.thermal_pressure`; `userland/src/thermald.c` passes it to iOS as
  `com.apple.system.thermalpressurelevel`, which `ProcessInfo.thermalState` reads.
- **The path to LPM:** the state is held by powerd (`_PMLowPowerMode`, private PowerManagement framework). Posting a Darwin notification does not change it:
  something has to ask powerd, through that framework's setter or its XPC service, and powerd may check an entitlement of the caller. Both are Apple-private, and the system image
  is not in this tree to read, so the selector, the service name and the entitlement cannot be confirmed here. A launchd job runs as root and can be signed with entitlements
  (`passcoded.entitlements` is the precedent), so it is plausible; it is not verifiable without the phone.
- **Design when it can be tried:** `thermald` already polls the level once a second. Enter LPM at pressure >= 2 held for 30 s, leave at pressure 0 held for 120 s
  (hysteresis in time, so a level that flaps does not flap the setting), only with an opt-in on the job's command line (not a boot argument), and never touch a mode the user set
  himself (leave only what thermald entered). Also to check first: what LPM changes on this userland (CPU is governed by our own `cpufreq_gov`, not CLPC, so LPM
  would reach apps and SpringBoard's animations and background activity, not the clocks).
- **Not implemented:** an unverified private-API call is not worth a blind PR.

## Jetsam

- **In our logs (`usb-1105.log`):** no jetsam kill line anywhere. Every `memorystatus_update` line reads `limit ... (-1MB F)`: no per-process memory limit is set on any process; compressor
  reports `0 swapfiles` (iOS has no swap; the compressor is all there is); memory is 3090 to 3218 MB with 16 KB pages. One process was `SIGKILLed` by launchd
  (a calendar widget extension), cause not shown in that log.
- **iPhone X profile (public, approximate):** the X has 3 GB and a per-device `JetsamProperties` table (Apple's plist, not here) that sets, per process, a priority band and a hard
  limit. Developers measuring the foreground app's budget on 3 GB iPhones report about 1.4 GB (the often-quoted table gives 1392 MB for the X); app extensions get far less
  (tens of MB; widgets the least). Treat the numbers as public measurements, not Apple's.
- **Comparison:** our 3.09 GB is the X's size, so the X's profile would fit as is, but we apply none of it: with `-1MB` limits an app can grow until system-wide pressure, and
  then the kernel's `memorystatus_available_pages_*` thresholds decide. Storage speed matters differently here than on a phone with swap-like behaviour: with no swap, memory
  pressure costs file-backed refaults (UFS reads) and compressor work, not swap writes.
- **Recommendation (nothing changed):** leave it. If kills or stalls appear under load, first turn on the kernel's memorystatus kill logging to see which band goes, and only then
  decide between loading the X's `JetsamProperties` (needs Apple's file at image build time, never in the repo) and setting per-extension limits. Do not set a foreground limit
  below ~1.2 GB.

## Prebuilt launch (dyld closures) at install time

- **Mechanism in 16.5:** dyld4 (iOS 16) has `PrebuiltLoaderSet`, the successor of dyld3 closures. For OS programs they are in the dyld cache; for others "built as needed and saved to disk"
  (`doc/PrebuiltLoaderSet_Policy.md` in apple-oss-distributions/dyld, tag dyld-1042.1). dyld builds and saves one itself at first launch when the cache has no set for the program.
- **Without Apple's private tools:** the open source dyld has `other-tools/dyld_closure_util.cpp`, whose `-create_closure <prog> -cache_file <cache>` builds the same set with
  `PrebuiltLoaderSet::makeLaunchSet`. It builds on macOS from the open sources plus the cache builder's headers; it needs the device's dyld shared cache as input (from the user's own image,
  never committed). So install-time generation is possible in principle.
- **Why it is not worth doing:** the policy document states that saved sets are not used after a reboot (a security rule: no persistence), so a set made at install is dropped at the next boot
  and dyld builds it again on the first launch, which is the stock behaviour. Gaining anything means defeating that check, which is a change to dyld itself. The first launch of an app already
  pays the cost once per boot; measure it (`DYLD_PRINT_STATISTICS`-style timing, or launch to first frame) before deciding it matters.
- **Not tried:** building `dyld_closure_util`; the tag dyld-1042.1 is my assumption for 16.5 (it is the one matching macOS 13.3/13.4) and was not checked against the cache's dyld.
