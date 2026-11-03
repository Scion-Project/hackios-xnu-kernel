/*
 * libhakios.bluetoothd.dylib: bluetoothd on the Galaxy S9 (build.sh BT=1, which also puts it into bluetoothd through
 * etc/inject.conf).
 *
 * When an accessory offers iAP over Bluetooth (the Sony WH-1000XM3 does, on an RFCOMM channel), bluetoothd 16.5 opens
 * the "Wireless IAP" profile as soon as the device is paired: it makes a PTY (openpty: "Tried to open PTY with central
 * = 17, peripheral = 18"), hands the peripheral end to the accessory server (com.apple.accessories.transport-server,
 * accessoryd: not on this system, "Could not establish connection to IAP") and from then on writes what the
 * accessory sends over iAP into the central end -- from its stack thread, holding the stack's mutex. Nobody reads the
 * other end, so after a few hundred bytes the PTY is full and that write() sleeps in the kernel ("ptcout") for good;
 * 20 s later bluetoothd's own watchdog finds the mutex still taken ("Stack Mutex try lock failed and watchdog has
 * struck twice -- abort"), bluetoothd exits and the headphones are dropped (phone 1, 3 November, 15:26).
 *
 * bluetoothd itself makes the central end non-blocking in one of its configurations (after openpty: fcntl(F_GETFL),
 * "Flags are 0x%08x", fcntl(F_SETFL, | O_NONBLOCK)), only not in the one this phone gets. So here, in bluetoothd and
 * nowhere else, openpty's central end is always made non-blocking: a write the PTY has no room for fails (EAGAIN) and
 * is dropped instead of stopping the stack. iAP itself goes nowhere (there is no accessory server); A2DP, AVRCP and
 * HFP do not use the PTY.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

static void
say(const char *format, ...)
{
	char line[300];
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
	say("libhakios.bluetoothd: in bluetoothd (pid %d): openpty's central end non-blocking\n", getpid());
}

static int
my_openpty(int *central, int *peripheral, char *name, struct termios *termp, struct winsize *winp)
{
	int r = openpty(central, peripheral, name, termp, winp);
	int saved = errno;

	if (r == 0 && central != NULL) {
		int flags = fcntl(*central, F_GETFL);

		if (flags >= 0 && fcntl(*central, F_SETFL, flags | O_NONBLOCK) == 0) {
			say("libhakios.bluetoothd: PTY %s: central end %d non-blocking (nothing reads the other end here)\n",
			    name != NULL ? name : "?", *central);
		}
	}
	errno = saved;
	return r;
}

__attribute__((used)) static const struct { const void *replacement, *replacee; } interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)my_openpty, (const void *)openpty },
};
