/*
 * passcode_test: the kernel key store's passcode (xnu/files/iokit/Kernel/MiniAppleKeyStore.cpp) on an HFS Plus
 * /private/var, in QEMU, across restarts: process 1 of a tiny APFS root (qemu/run-passcode-test.sh). It mounts the
 * volume on /private/var, as init does on the phone, and asks the kernel to run the step of its self-test that the
 * boot-argument passcode_selftest=N names (the kernel logs each check); then says what the kernel answered.
 * A static program with no libc, like hfs_test.c.
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;

static long
sys6(long num, long a, long b, long c, long d, long e, long f)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	long carry;
	__asm__ volatile ("svc #0x80\n cset %1, cs" : "+r" (x0), "=r" (carry) : "r" (x16), "r" (x1), "r" (x2), "r" (x3),
	    "r" (x4), "r" (x5) : "memory", "cc");
	return carry ? -x0 : x0;
}
#define sys3(n, a, b, c) sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

enum { SYS_exit = 1, SYS_write = 4, SYS_open = 5, SYS_sync = 36, SYS_mount = 167, SYS_unmount = 159, SYS_sysctlbyname = 274 };
#define O_RDWR 2

static long console = -1;

static unsigned long
slen(const char *s)
{
	unsigned long n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static void
say(const char *s)
{
	sys3(SYS_write, console, s, slen(s));
}

static void
num(long v)
{
	char b[24];
	int i = 23, neg = v < 0;
	b[i] = 0;
	if (neg) {
		v = -v;
	}
	do {
		b[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	if (neg) {
		b[--i] = '-';
	}
	say(b + i);
}

/* struct hfs_mount_args as user space passes it (bsd/hfs/hfs_mount.h) */
struct hfs_args {
	const char *fspec;
	u32 uid, gid;
	u16 mask, pad;
	u32 encoding;
	int tz[2];
	int flags, journal_tbuffer_size, journal_flags, journal_disable;
};

void
_start(void)
{
	struct hfs_args a = { .fspec = "/dev/rvblk0" };
	static const char name[] = "kern.hakios.passcode_selftest";
	int one = 1, result = 0;
	u64 size = sizeof(result);
	long r;

	console = sys3(SYS_open, "/dev/console", O_RDWR, 0);
	if ((r = sys6(SYS_mount, (long)"hfs", (long)"/private/var", 0, (long)&a, 0, 0)) < 0) {
		say("passcode_test: mount hfs /dev/rvblk0 on /private/var: ");
		num(r);
		say("\n");
		sys3(SYS_exit, 1, 0, 0);
	}
	say("passcode_test: /private/var mounted\n");
	r = sys6(SYS_sysctlbyname, (long)name, sizeof(name) - 1, 0, 0, (long)&one, sizeof(one));
	if (r < 0) {
		say("passcode_test: sysctl write: ");
		num(r);
		say("\n");
	}
	r = sys6(SYS_sysctlbyname, (long)name, sizeof(name) - 1, (long)&result, (long)&size, 0, 0);
	say(result == 1 ? "passcode_test: PASS\n" : "passcode_test: FAIL (");
	if (result != 1) {
		num(result);
		say(")\n");
	}
	sys3(SYS_sync, 0, 0, 0);
	sys3(SYS_unmount, "/private/var", 0, 0);
	sys3(SYS_exit, 0, 0, 0);
}
