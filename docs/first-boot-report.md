# First-boot report

The job `com.hakios.selftest` (`userland/src/selftest.c`) writes one plain report 60 s into the first boot. Nothing is sent anywhere.

It holds: `kern.version`, `kern.bootargs` (so whether `fps_log=1` was set), cores (`hw.ncpu`, `hw.activecpu`), the frame buffer's surface
(`kern.hakios.fb_surface`), touch events (`kern.hakios.touch_events`, counted by the S6SY761 driver, so it needs `s6sy761=1` and a finger on the glass),
the Wi-Fi interface `en0`, the UFS error counters, the panic reports found under `/private/var/mobile/Library/Logs/CrashReporter`,
the battery (charge, voltage, temperature) and the TMU's hottest zone with the thermal pressure. A value the kernel does not offer reads `n/a`.

Not in it: the panel's mode and refresh rate (user space has no query for them here; `fps_log=1` puts them in the kernel log), and a panic of the
boot itself, which reboots the phone before the report is made.

## Reading it

- **USB console:** every line is printed to the console, so it is in the log the USB console streams to the host (`/dev/ttyACM0` on Linux,
  `/dev/cu.usbmodem*` on macOS). Filter for `selftest:`; the report is between `selftest: begins` and `selftest: ends`.
- **On a kept volume** (`var_hfs=`, `var_store=1`): `/private/var/scion-selftest.txt`, the same lines without the prefix.

Only the first boot reports: `/private/var/.scion-selftest-done` is written at the end and a later boot ends at once. On a `/private/var` in memory
nothing is kept, so every boot reports. Delete that file to have it report again. Built in, with no boot argument; it adds nothing to the boot line.

Status: compiled; not booted.
