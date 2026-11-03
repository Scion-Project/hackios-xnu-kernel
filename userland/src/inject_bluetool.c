/*
 * libhakios.bluetool.dylib: BlueTool on the Galaxy S9 (build.sh BT=1, which also puts it into BlueTool through
 * etc/inject.conf).
 *
 * BlueTool 16.5 is the tool bluetoothd asks, over XPC (com.apple.BlueTool), to run the "boot" and then the "init"
 * script that bring an iPhone's Bluetooth chip up; bluetoothd starts its stack only when both have succeeded. Which
 * script BlueTool runs it decides in runCommandForCurrentConfig: the platform (MobileGestalt's ProductHash), the chip
 * (on a UART, what the chip answers to Read Local Version) and the module's maker (WifiVendor) pick one of its
 * built-in sequences, which load Apple's firmware into Apple's modules -- none of which this phone has (here: the
 * platform fd079b29..., chip "BCM", vendor "ALL": no match, so "Error - no firmware for this platform" and a failed
 * boot). Only where MobileGestalt's ReleaseType is "Vendor" or "Internal" does it first look for
 * /etc/bluetool/<HW model>.<script>.script, then /etc/bluetool/<platform>.<script>.script, and run that instead.
 *
 * The S9's chip needs no sequence of Apple's: the kernel has powered it, patched it and raised it to 4 Mbaud (bt=7,
 * drivers/bluetooth/hakios.bcm4361_bt.c). So, in BlueTool and nowhere else:
 *   - ReleaseType is "Vendor" (every other answer is MobileGestalt's own), so that BlueTool runs the S9's scripts;
 *   - /etc/bluetool/ is the small image's /redwood/etc/bluetool/ (the system volume has no such directory), where
 *     D22AP.boot.script and D22AP.init.script (bluetool/ of this directory: D22AP is the HW model MobileGestalt gives)
 *     open the chip through the kernel's com.apple.uart.bluetooth and ask its version.
 * Seen on phone 2, 3 November: "_isVendorInstall = 1", "HW Model there is override script file
 * /etc/bluetool/D22AP.boot.script", "boot script attempt 1 of 1: result 0", the same for init, then bluetoothd's
 * "Stack: STACK_BUSY -> STACK_READY".
 */
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FROM "/etc/bluetool/"
#define TO   "/redwood/etc/bluetool/"

extern CFTypeRef MGCopyAnswer(CFStringRef key, CFDictionaryRef options);

static void
say(const char *format, ...)
{
	char line[600];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > 0) {
		write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
	}
}

__attribute__((constructor)) static void
hello(void)
{
	say("libhakios.bluetool: in BlueTool (pid %d): ReleaseType Vendor, /etc/bluetool/ -> %s\n", getpid(), TO);
}

/* /etc/bluetool/X (or /private/etc/bluetool/X) -> /redwood/etc/bluetool/X; anything else as it is */
static const char *
moved(const char *path, char *buf, size_t cap)
{
	const char *rest = NULL;

	if (path != NULL && strncmp(path, FROM, sizeof(FROM) - 1) == 0) {
		rest = path + sizeof(FROM) - 1;
	} else if (path != NULL && strncmp(path, "/private" FROM, sizeof("/private" FROM) - 1) == 0) {
		rest = path + sizeof("/private" FROM) - 1;
	}
	if (rest == NULL) {
		return path;
	}
	snprintf(buf, cap, "%s%s", TO, rest);
	return buf;
}

static int my_stat(const char *path, struct stat *st) { char b[1024]; return stat(moved(path, b, sizeof(b)), st); }
static int my_lstat(const char *path, struct stat *st) { char b[1024]; return lstat(moved(path, b, sizeof(b)), st); }
static int my_access(const char *path, int mode) { char b[1024]; return access(moved(path, b, sizeof(b)), mode); }
static FILE *
my_fopen(const char *path, const char *mode)
{
	char b[1024];

	return fopen(moved(path, b, sizeof(b)), mode);
}

static int
my_open(const char *path, int flags, ...)
{
	char b[1024];
	int mode = 0;

	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	return open(moved(path, b, sizeof(b)), flags, mode);
}

static CFTypeRef
my_MGCopyAnswer(CFStringRef key, CFDictionaryRef options)
{
	if (key != NULL && CFEqual(key, CFSTR("ReleaseType"))) {
		return CFRetain(CFSTR("Vendor"));
	}
	return MGCopyAnswer(key, options);
}

__attribute__((used)) static const struct { const void *replacement, *replacee; } interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)my_stat, (const void *)stat },
	{ (const void *)my_lstat, (const void *)lstat },
	{ (const void *)my_access, (const void *)access },
	{ (const void *)my_fopen, (const void *)fopen },
	{ (const void *)my_open, (const void *)open },
	{ (const void *)my_MGCopyAnswer, (const void *)MGCopyAnswer },
};
