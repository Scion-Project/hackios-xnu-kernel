/*
 * MiniAppleKeyStore: a stand-in for Apple's AppleKeyStore (the kernel side of the keybag,
 * which on real hardware talks to the Secure Enclave), as seen by MobileKeyBag.framework
 * and libaks.
 *
 * What it keeps: whether the device has a passcode, and if so a verifier of it, the lock state,
 * the count of failed attempts and the delay they impose, the grace period, and the opaque
 * "system secret blob" that ManagedConfiguration stores with a passcode (what kind of passcode it is:
 * the lock screen's keypad is chosen from it). With no passcode it answers as it always did: "no
 * passcode, unlocked since boot" (lock state 3, disabled).
 *
 * WHAT THIS IS NOT. There is no Secure Enclave and no hardware key. The passcode gates the user
 * interface -- the lock screen, Settings, MC's checks -- and nothing else: no data is encrypted with
 * it, the keychain's "wrapping" below is a reversible transformation of no secrecy, the data volume is
 * plain HFS Plus that anyone with the flash can read, and the verifier file can be deleted by anyone
 * who can write /private/var (root) or the flash. Treat it as a screen lock, not data protection.
 *
 * The passcode
 *   - Verifier: PBKDF2-HMAC-SHA256 of the passcode's bytes with a random 16-byte salt, 100000
 *     iterations (both kept with it, so they can change later), compared in constant time. The
 *     key derivation is checked against RFC 7914's test vector before a passcode is set or tried;
 *     a kernel whose SHA-256 gives another answer refuses to set one, and treats a stored one as
 *     absent (better open than shut for good).
 *   - Kept in /private/var/hakios-passcode (USERDATA on the phone: kept between boots; in memory on
 *     a RAM boot and in QEMU). The file has two 4 KB slots; a change writes the slot that does not
 *     hold the newest record, with a generation one higher and a SHA-256 over it, and syncs. A torn
 *     write damages only the slot being written, and reading takes the valid slot with the highest
 *     generation. A file that cannot be read or has no valid slot means "no passcode".
 *   - Read on the first call any process makes to the key store: process 1 (userland/src/init.c)
 *     mounts /private/var before it starts launchd, and nothing reaches the key store before launchd.
 *
 * Escape hatch: the boot-argument passcode_reset=1 discards the stored passcode at the first call of
 * the boot (both slots are overwritten with a "no passcode" record), and says so in the log. Flashing
 * a boot image with it is always possible (TWRP or download mode), whatever the lock screen does. The kernel's
 * command line holds 607 characters and what does not fit is cut silently, the end first: leave room for it
 * (docs/passcode.md). Without a boot image, deleting /private/var/hakios-passcode from the data volume does the same.
 *
 * Lock states (MobileKeyBag's, what MKBGetDeviceLockState returns): 0 unlocked, 1 locked, 3 disabled
 * (no passcode). With a passcode the device starts locked and not "unlocked since boot". A lock request
 * locks at once. The grace period ManagedConfiguration sets (Settings' "Require Passcode") is kept and reported but not
 * applied: a longer one asks for the passcode sooner than chosen, never later. Lock assertions are refused while
 * locked (kAKSAssertHold below).
 * Every change is sent to the clients of IOGeneralInterest (MKBEventsRegister: SpringBoard), and the
 * first unlock of the boot as well.
 *
 * Failed attempts: the delays of Apple's Platform Security guide, after the 5th failed attempt 1 minute,
 * the 6th 5 minutes, the 7th and 8th 15 minutes, the 9th and every one after 1 hour. Never more, and never
 * "permanently blocked", never a wipe: the 10th attempt is another hour. Attempts made while a delay runs
 * are refused without being counted. The count and the end of the delay are kept with the verifier, so
 * a restart does not clear them (a delay read back after a restart is cut to its length, if the clock
 * says more is left).
 *
 * The protocol was worked out from the user-space side of Darwin userland 16.5 (MobileKeyBag.framework,
 * SpringBoardFoundation, ManagedConfiguration): selectors, the DER-encoded answers ("SET of SEQUENCE {
 * UTF8String key, INTEGER value }"), and how arguments are passed. The passcode is changed through
 * keybagd's XPC service, which userland/src/passcoded.m stands in for; it reaches this file with the
 * selectors kHKChangeSecret and kHKCopyBlob below, which are this port's own. kHKChangeSecret is for root (passcoded
 * is): Apple's key store has entitlements for this, and this one has no such check. Nothing limits who may try a
 * passcode (kAKSUnlockDevice, kAKSVerifyPassword), so any program can run up the failed attempts and their delay.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/crypto/sha2.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <sys/random.h>
#include <sys/kauth.h>
#include <kern/clock.h>
#include <sys/sysctl.h>

#include "MiniDrivers.h"

extern "C" {
extern vfs_context_t vfs_context_kernel(void);
}

enum {
	kAKSOpenCheck           = 0x00,
	kAKSGetLockState        = 0x07,         /* in: handle; out: state flags */
	kAKSLockDevice          = 0x08,         /* in: handle */
	kAKSUnlockDevice        = 0x09,         /* in: handle, the passcode's address in the caller, its length */
	kAKSWrapKey             = 0x0a,         /* in: handle, class; a key; out: the class, the key wrapped */
	kAKSUnwrapKey           = 0x0b,         /* in: handle, class; a wrapped key; out: the key */
	kAKSUnlockBag           = 0x0c,
	kAKSGetSystem           = 0x0e,         /* out: the handle of the system keybag */
	kAKSGetDeviceState      = 0x11,         /* in: handle; out: DER */
	kAKSGetBagUUID          = 0x17,         /* out: 16 bytes */
	kAKSAssertHold          = 0x1a,
	kAKSAssertDrop          = 0x1b,
	kAKSSetConfiguration    = 0x1e,         /* in: DER, as kAKSGetConfiguration's answer */
	kAKSGetConfiguration    = 0x1f,         /* in: handle; out: DER (a big buffer, so out of line) */
	kAKSLockDeviceNow       = 0x20,
	kAKSGetExtendedState    = 0x23,         /* as kAKSGetDeviceState */
	kAKSGeneration          = 0x28,
	kAKSVerifyPassword      = 0x2a,         /* in: handle; { u32 count; { u32 length; bytes, padded to 4 } ... }, the passcode first */
	kAKSAssertPromote       = 0x64,
	kAKSAssertConsume       = 0x65,
	kAKSNotifyStateSet      = 0x66,         /* "the first-unlock notification has been posted" */
	kAKSNotifyStateIsSet    = 0x67,
	kAKSPrewarm             = 0x73,

	/* this port's own, for userland/src/passcoded.m (the stand-in for keybagd's XPC service) */
	kHKChangeSecret         = 0x100,        /* in: struct HKChangeSecret and its bytes */
	kHKCopyBlob             = 0x101,        /* out: the opaque data stored with the passcode */
	kHKStatus               = 0x102,        /* out: has passcode, lock state, failed attempts, seconds of delay left, generation */
};

/* the messages MKBEventsRegister's callback knows (SpringBoardFoundation: 0 first unlock, 1 status changed) */
#define kAKSMessageStatusChanged        0xe3ffd00d
#define kAKSMessageFirstUnlock          0xe3ffd00f

/* state flags ("ss", and kAKSGetLockState's answer): bit 0 locked, bit 1 no passcode, bit 2 unlocked since boot */
#define kAKSStateLocked         0x1
#define kAKSStateNoPasscode     0x2
#define kAKSStateUnlockedOnce   0x4
/* lock state ("sls"): 0 unlocked, 1 locked, 2 locking, 3 disabled (no passcode) */
#define kAKSLockUnlocked        0
#define kAKSLockLocked          1
#define kAKSLockDisabled        3

/* answers that MobileKeyBag turns into its own codes: NotReadable is -3 (a wrong passcode), NotPermitted -2 */
#define kAKSWrongPasscode       kIOReturnNotReadable
#define kAKSBlocked             kIOReturnNotPermitted

#define kHKMaxSecret            256
#define kHKMaxBlob              2048
#define kHKIterations           100000
#define kHKMaxUnlockAttempts    10              /* what "mua" says; nothing here wipes, whatever it is */

struct HKChangeSecret {
	uint32_t magic;                         /* 'HKcs' */
	uint32_t oldLength;
	uint32_t newLength;
	uint32_t blobLength;
	/* then the old passcode, the new one (none: no passcode from now on), the opaque data */
};
#define kHKChangeMagic          0x484b6373

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the record on disk */

#define kHKPath                 "/private/var/hakios-passcode"
#define kHKSlotSize             4096
#define kHKRecordMagic          0x4850434b      /* 'HPCK' */
#define kHKRecordVersion        1

struct HKRecord {
	uint32_t magic;
	uint32_t version;
	uint64_t generation;
	uint8_t  digest[32];                    /* SHA-256 of everything after it, to the end of the blob */
	uint32_t hasPasscode;
	uint32_t iterations;
	uint8_t  salt[16];
	uint8_t  verifier[32];
	uint32_t failedAttempts;
	uint32_t gracePeriod;                   /* seconds */
	uint64_t blockedUntil;                  /* calendar seconds; 0: no delay */
	uint32_t blobLength;
	uint32_t reserved;
	uint8_t  blob[kHKMaxBlob];
};

static_assert(sizeof(HKRecord) <= kHKSlotSize, "a record fits in a slot");

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the state */

static IOLock *         gAKSLock;
static IOService *      gAKSService;
static uint64_t         gAKSLogged[4];  /* selectors already reported */
static bool             gAKSNotifyState;

static struct {
	bool     loaded;                        /* the file has been read (or found absent) */
	bool     kdfGood;                       /* PBKDF2 passed its self-test */
	HKRecord record;                        /* the current record, as in memory */
	uint64_t generation;                    /* the newest generation on disk */
	int      newestSlot;                    /* where it is; -1 none */
	bool     locked;
	bool     unlockedOnce;                  /* since boot */
	uint64_t blockedUntilUptime;            /* uptime, ns; 0: no delay */
} gKS;

static uint64_t
uptimeNanoseconds(void)
{
	uint64_t abs, ns;

	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	return ns;
}

static uint64_t
calendarSeconds(void)
{
	clock_sec_t secs;
	clock_usec_t usecs;

	clock_get_calendar_microtime(&secs, &usecs);
	return (uint64_t) secs;
}

/* The delay after the n-th failed attempt, in seconds (Apple Platform Security, "Passcodes"). */
static uint32_t
delayAfter(uint32_t failed)
{
	if (failed < 5) {
		return 0;
	}
	switch (failed) {
	case 5:  return 60;
	case 6:  return 5 * 60;
	case 7:
	case 8:  return 15 * 60;
	default: return 60 * 60;
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * PBKDF2-HMAC-SHA256 */

struct HMACKey {
	SHA256_CTX inner, outer;                /* after the padded key */
};

static void
hmacKey(HMACKey * k, const uint8_t * key, size_t keyLength)
{
	uint8_t block[64], hashed[32];

	if (keyLength > sizeof(block)) {
		SHA256_CTX c;
		SHA256_Init(&c);
		SHA256_Update(&c, key, keyLength);
		SHA256_Final(hashed, &c);
		key = hashed;
		keyLength = sizeof(hashed);
	}
	memset(block, 0, sizeof(block));
	memcpy(block, key, keyLength);
	for (size_t i = 0; i < sizeof(block); i++) {
		block[i] ^= 0x36;
	}
	SHA256_Init(&k->inner);
	SHA256_Update(&k->inner, block, sizeof(block));
	for (size_t i = 0; i < sizeof(block); i++) {
		block[i] ^= 0x36 ^ 0x5c;
	}
	SHA256_Init(&k->outer);
	SHA256_Update(&k->outer, block, sizeof(block));
	memset(block, 0, sizeof(block));
	memset(hashed, 0, sizeof(hashed));
}

static void
hmac(const HMACKey * k, const uint8_t * a, size_t aLength, const uint8_t * b, size_t bLength, uint8_t out[32])
{
	SHA256_CTX c = k->inner;
	uint8_t inner[32];

	SHA256_Update(&c, a, aLength);
	if (bLength) {
		SHA256_Update(&c, b, bLength);
	}
	SHA256_Final(inner, &c);
	c = k->outer;
	SHA256_Update(&c, inner, sizeof(inner));
	SHA256_Final(out, &c);
}

static void
pbkdf2(const uint8_t * password, size_t passwordLength, const uint8_t * salt, size_t saltLength, uint32_t iterations,
    uint8_t * out, size_t outLength)
{
	HMACKey k;

	hmacKey(&k, password, passwordLength);
	for (uint32_t block = 1; outLength > 0; block++) {
		uint8_t count[4] = { (uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t) block };
		uint8_t u[32], t[32];
		size_t n = outLength < 32 ? outLength : 32;

		hmac(&k, salt, saltLength, count, sizeof(count), u);
		memcpy(t, u, sizeof(t));
		for (uint32_t i = 1; i < iterations; i++) {
			hmac(&k, u, sizeof(u), NULL, 0, u);
			for (size_t j = 0; j < sizeof(t); j++) {
				t[j] ^= u[j];
			}
		}
		memcpy(out, t, n);
		out += n;
		outLength -= n;
	}
	memset(&k, 0, sizeof(k));
}

/* RFC 7914 section 11: PBKDF2-HMAC-SHA256 (P="passwd", S="salt", c=1), the first 32 of its 64 bytes */
static bool
pbkdf2SelfTest(void)
{
	static const uint8_t expected[32] = {
		0x55, 0xac, 0x04, 0x6e, 0x56, 0xe3, 0x08, 0x9f, 0xec, 0x16, 0x91, 0xc2, 0x25, 0x44, 0xb6, 0x05,
		0xf9, 0x41, 0x85, 0x21, 0x6d, 0xde, 0x04, 0x65, 0xe6, 0x8b, 0x9d, 0x57, 0xc2, 0x0d, 0xac, 0xbc,
	};
	uint8_t out[32];

	pbkdf2((const uint8_t *) "passwd", 6, (const uint8_t *) "salt", 4, 1, out, sizeof(out));
	return memcmp(out, expected, sizeof(out)) == 0;
}

static bool
sameBytes(const uint8_t * a, const uint8_t * b, size_t length)
{
	uint8_t d = 0;

	for (size_t i = 0; i < length; i++) {
		d |= a[i] ^ b[i];
	}
	return d == 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the file */

static void
recordDigest(const HKRecord * r, uint8_t out[32])
{
	SHA256_CTX c;
	const uint8_t * from = (const uint8_t *) &r->hasPasscode;
	size_t length = __builtin_offsetof(HKRecord, blob) - __builtin_offsetof(HKRecord, hasPasscode) + (r->blobLength <= kHKMaxBlob ? r->blobLength : 0);

	SHA256_Init(&c);
	SHA256_Update(&c, from, length);
	SHA256_Final(out, &c);
}

static bool
recordValid(const HKRecord * r)
{
	uint8_t digest[32];

	if (r->magic != kHKRecordMagic || r->version != kHKRecordVersion || r->blobLength > kHKMaxBlob) {
		return false;
	}
	recordDigest(r, digest);
	if (!sameBytes(digest, r->digest, sizeof(digest))) {
		return false;
	}
	if (r->hasPasscode && (r->iterations == 0 || r->iterations > 10000000)) {
		return false;
	}
	return true;
}

static int
fileIO(enum uio_rw rw, int slot, HKRecord * r)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int resid = 0, error;
	int fmode = rw == UIO_READ ? FREAD : (FREAD | FWRITE | O_CREAT);

	error = vnode_open(kHKPath, fmode, 0600, VNODE_LOOKUP_NOFOLLOW, &vp, ctx);
	if (error) {
		return error;
	}
	error = vn_rdwr(rw, vp, (caddr_t) r, (int) sizeof(*r), (off_t) slot * kHKSlotSize, UIO_SYSSPACE,
	    rw == UIO_WRITE ? IO_SYNC | IO_NOAUTH : IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	if (error == 0 && resid != 0) {
		error = rw == UIO_READ ? ENOENT : EIO;      /* a short slot: never written */
	}
	if (error == 0 && rw == UIO_WRITE) {
		error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
	}
	vnode_close(vp, fmode, ctx);
	return error;
}

/* Write r as the next generation into the slot that does not hold the newest one. */
static int
persist(HKRecord * r)
{
	int slot = gKS.newestSlot == 0 ? 1 : 0;
	int error;

	r->magic = kHKRecordMagic;
	r->version = kHKRecordVersion;
	r->generation = gKS.generation + 1;
	recordDigest(r, r->digest);
	error = fileIO(UIO_WRITE, slot, r);
	if (error) {
		printf("MiniAppleKeyStore: cannot write %s slot %d (error %d)\n", kHKPath, slot, error);
		return error;
	}
	gKS.generation = r->generation;
	gKS.newestSlot = slot;
	return 0;
}

static void
setBlockedUntil(HKRecord * r, uint32_t seconds)
{
	if (seconds == 0) {
		r->blockedUntil = 0;
		gKS.blockedUntilUptime = 0;
	} else {
		r->blockedUntil = calendarSeconds() + seconds;
		gKS.blockedUntilUptime = uptimeNanoseconds() + (uint64_t) seconds * NSEC_PER_SEC;
	}
}

static uint32_t
secondsBlocked(void)
{
	uint64_t now = uptimeNanoseconds();

	if (gKS.blockedUntilUptime == 0 || now >= gKS.blockedUntilUptime) {
		return 0;
	}
	return (uint32_t) ((gKS.blockedUntilUptime - now + NSEC_PER_SEC - 1) / NSEC_PER_SEC);
}

static void
noPasscode(HKRecord * r)
{
	uint32_t grace = r->gracePeriod;

	memset(r, 0, sizeof(*r));
	r->gracePeriod = grace;
}

/* The first call of the boot: read the record (with the key store's lock held). */
static void
load(void)
{
	HKRecord * slots[2] = { NULL, NULL };
	uint32_t reset = 0;
	int error[2] = { 0, 0 };

	gKS.loaded = true;
	gKS.newestSlot = -1;
	gKS.unlockedOnce = true;
	gKS.kdfGood = pbkdf2SelfTest();
	if (!gKS.kdfGood) {
		printf("MiniAppleKeyStore: PBKDF2-HMAC-SHA256 fails its test vector: no passcode can be set or used\n");
	}
	for (int i = 0; i < 2; i++) {
		slots[i] = IONew(HKRecord, 1);
		if (slots[i] == NULL) {
			error[i] = ENOMEM;
			continue;
		}
		memset(slots[i], 0, sizeof(HKRecord));
		error[i] = fileIO(UIO_READ, i, slots[i]);
		if (error[i] == 0 && !recordValid(slots[i])) {
			error[i] = EINVAL;
		}
		if (error[i] == 0 && slots[i]->generation >= gKS.generation) {
			gKS.generation = slots[i]->generation;
			gKS.newestSlot = i;
		}
	}
	if (gKS.newestSlot >= 0) {
		gKS.record = *slots[gKS.newestSlot];
	}
	for (int i = 0; i < 2; i++) {
		if (slots[i] != NULL) {
			memset(slots[i], 0, sizeof(HKRecord));
			IODelete(slots[i], HKRecord, 1);
		}
	}

	if (PE_parse_boot_argn("passcode_reset", &reset, sizeof(reset)) && reset != 0) {
		bool had = gKS.newestSlot >= 0 && gKS.record.hasPasscode;

		noPasscode(&gKS.record);
		/* both slots, so that no older record with a passcode is left to come back */
		for (int i = 0; i < 2; i++) {
			if (persist(&gKS.record) != 0) {
				break;
			}
		}
		printf("MiniAppleKeyStore: passcode_reset=%u: %s; %s now holds no passcode\n", reset,
		    had ? "the stored passcode is discarded" : "there was no passcode", kHKPath);
	}

	if (gKS.newestSlot < 0) {
		printf("MiniAppleKeyStore: %s: %s (slot errors %d, %d): no passcode\n", kHKPath,
		    error[0] == ENOENT && error[1] == ENOENT ? "absent" : "no valid record", error[0], error[1]);
		noPasscode(&gKS.record);
		return;
	}
	if (gKS.record.hasPasscode && !gKS.kdfGood) {
		printf("MiniAppleKeyStore: the stored passcode is ignored (the key derivation is broken): no passcode\n");
		noPasscode(&gKS.record);
		return;
	}
	if (gKS.record.hasPasscode) {
		uint64_t now = calendarSeconds();
		uint32_t delay = 0;

		gKS.locked = true;
		gKS.unlockedOnce = false;
		if (gKS.record.blockedUntil > now) {
			uint64_t left = gKS.record.blockedUntil - now;
			uint32_t most = delayAfter(gKS.record.failedAttempts);

			delay = left > most ? most : (uint32_t) left;       /* a clock that went back does not lengthen it */
			gKS.blockedUntilUptime = uptimeNanoseconds() + (uint64_t) delay * NSEC_PER_SEC;
		}
		printf("MiniAppleKeyStore: passcode set (generation %llu, slot %d), locked; %u failed attempts, %u s of delay left\n",
		    gKS.generation, gKS.newestSlot, gKS.record.failedAttempts, delay);
	} else {
		printf("MiniAppleKeyStore: %s generation %llu: no passcode\n", kHKPath, gKS.generation);
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the state machine (lock held) */

static uint32_t
stateFlags(void)
{
	uint32_t flags = 0;

	if (!gKS.record.hasPasscode) {
		return kAKSStateNoPasscode | kAKSStateUnlockedOnce;
	}
	if (gKS.locked) {
		flags |= kAKSStateLocked;
	}
	if (gKS.unlockedOnce) {
		flags |= kAKSStateUnlockedOnce;
	}
	return flags;
}

static uint32_t
lockState(void)
{
	if (!gKS.record.hasPasscode) {
		return kAKSLockDisabled;
	}
	return gKS.locked ? kAKSLockLocked : kAKSLockUnlocked;
}

/* A passcode try. *changed says whether the lock state changed, *first whether this is the first unlock. */
static IOReturn
tryPasscode(const uint8_t * passcode, size_t length, bool unlock, bool * changed, bool * first)
{
	uint8_t derived[32];
	bool good;

	if (!gKS.record.hasPasscode) {
		return kIOReturnSuccess;
	}
	if (secondsBlocked() > 0) {
		return kAKSBlocked;
	}
	if (length == 0 || !gKS.kdfGood) {
		return kAKSWrongPasscode;                        /* not counted: nothing was tried */
	}
	pbkdf2(passcode, length, gKS.record.salt, sizeof(gKS.record.salt), gKS.record.iterations, derived, sizeof(derived));
	good = sameBytes(derived, gKS.record.verifier, sizeof(derived));
	memset(derived, 0, sizeof(derived));

	if (!good) {
		uint32_t delay;

		if (gKS.record.failedAttempts < 1000) {
			gKS.record.failedAttempts++;
		}
		delay = delayAfter(gKS.record.failedAttempts);
		setBlockedUntil(&gKS.record, delay);
		persist(&gKS.record);                            /* if it cannot be written, memory still counts it */
		printf("MiniAppleKeyStore: wrong passcode (%u failed attempts; %u s before the next)\n", gKS.record.failedAttempts, delay);
		*changed = true;                                 /* the failed count and the delay are state too */
		return kAKSWrongPasscode;
	}
	if (gKS.record.failedAttempts != 0 || gKS.record.blockedUntil != 0) {
		gKS.record.failedAttempts = 0;
		setBlockedUntil(&gKS.record, 0);
		persist(&gKS.record);
		*changed = true;
	}
	if (unlock) {
		if (gKS.locked) {
			gKS.locked = false;
			*changed = true;
			printf("MiniAppleKeyStore: unlocked with the passcode\n");
		}
		if (!gKS.unlockedOnce) {
			gKS.unlockedOnce = true;
			*first = true;
		}
	}
	return kIOReturnSuccess;
}

static IOReturn
changeSecret(const uint8_t * oldSecret, size_t oldLength, const uint8_t * newSecret, size_t newLength,
    const uint8_t * blob, size_t blobLength, bool * changed, bool * first)
{
	HKRecord * next;
	IOReturn ret;
	int error;

	if (newLength > 0 && !gKS.kdfGood) {
		return kIOReturnUnsupported;
	}
	if (gKS.record.hasPasscode) {
		ret = tryPasscode(oldSecret, oldLength, false, changed, first);
		if (ret != kIOReturnSuccess) {
			return ret;
		}
	}
	next = IONew(HKRecord, 1);
	if (next == NULL) {
		return kIOReturnNoMemory;
	}
	*next = gKS.record;
	if (newLength == 0) {
		noPasscode(next);
	} else {
		next->hasPasscode = 1;
		next->iterations = kHKIterations;
		read_random(next->salt, sizeof(next->salt));
		pbkdf2(newSecret, newLength, next->salt, sizeof(next->salt), next->iterations, next->verifier, sizeof(next->verifier));
		next->failedAttempts = 0;
		next->blockedUntil = 0;
	}
	next->blobLength = (uint32_t) blobLength;
	memset(next->blob, 0, sizeof(next->blob));
	if (blobLength) {
		memcpy(next->blob, blob, blobLength);
	}
	error = persist(next);
	if (error) {
		/* nothing changes unless it is kept: a passcode that would be gone at the next start is not set */
		memset(next, 0, sizeof(*next));
		IODelete(next, HKRecord, 1);
		return kIOReturnIOError;
	}
	gKS.record = *next;
	memset(next, 0, sizeof(*next));
	IODelete(next, HKRecord, 1);
	gKS.blockedUntilUptime = 0;
	gKS.locked = false;
	if (!gKS.unlockedOnce) {
		gKS.unlockedOnce = true;
		*first = true;
	}
	*changed = true;
	printf("MiniAppleKeyStore: passcode %s (generation %llu)\n", newLength ? "set" : "removed", gKS.generation);
	return kIOReturnSuccess;
}

static void
announce(bool changed, bool first)
{
	if (gAKSService == NULL) {
		return;
	}
	if (changed || first) {
		gAKSService->messageClients(kAKSMessageStatusChanged);
	}
	if (first) {
		gAKSService->messageClients(kAKSMessageFirstUnlock);
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * DER */

/* Append SEQUENCE { UTF8String key, INTEGER value }; returns the new length. */
static size_t
derPut(uint8_t * buf, size_t length, const char * key, uint64_t value)
{
	size_t keyLength = strlen(key);
	uint8_t bytes[9];
	size_t n = 0;

	/* INTEGER: big-endian, the fewest bytes, a leading zero when the top bit is set (it is not negative) */
	for (int shift = 56; shift >= 0; shift -= 8) {
		uint8_t b = (uint8_t)(value >> shift);
		if (n == 0 && b == 0 && shift != 0) {
			continue;
		}
		if (n == 0 && (b & 0x80)) {
			bytes[n++] = 0;
		}
		bytes[n++] = b;
	}
	buf[length++] = 0x30;
	buf[length++] = (uint8_t) (keyLength + n + 4);
	buf[length++] = 0x0c;
	buf[length++] = (uint8_t) keyLength;
	memcpy(buf + length, key, keyLength);
	length += keyLength;
	buf[length++] = 0x02;
	buf[length++] = (uint8_t) n;
	memcpy(buf + length, bytes, n);
	return length + n;
}

/* The SET's header, once its contents (written from buf + 4) are known; returns the whole length. */
static size_t
derSet(uint8_t * buf, size_t length)
{
	size_t body = length - 4;

	if (body < 128) {
		buf[0] = 0x31;
		buf[1] = (uint8_t) body;
		memmove(buf + 2, buf + 4, body);
		return body + 2;
	}
	buf[0] = 0x31;
	buf[1] = 0x82;
	buf[2] = (uint8_t) (body >> 8);
	buf[3] = (uint8_t) body;
	return body + 4;
}

/* The answer to "device state" (sls is the lock state, ss the flags, sb the seconds of delay left, sfa the failed attempts). */
static size_t
deviceState(uint8_t * buf)
{
	size_t length = 4;

	length = derPut(buf, length, "sb", secondsBlocked());
	length = derPut(buf, length, "sr", 0);
	length = derPut(buf, length, "ss", stateFlags());
	length = derPut(buf, length, "sas", 0);
	length = derPut(buf, length, "sfa", gKS.record.hasPasscode ? gKS.record.failedAttempts : 0);
	length = derPut(buf, length, "sgs", 0);
	length = derPut(buf, length, "sls", lockState());
	length = derPut(buf, length, "sgpe", gKS.record.hasPasscode && gKS.record.gracePeriod != 0);
	length = derPut(buf, length, "srcd", 0);
	return derSet(buf, length);
}

static size_t
configuration(uint8_t * buf)
{
	size_t length = 4;

	length = derPut(buf, length, "bd", 0);
	length = derPut(buf, length, "gp", gKS.record.gracePeriod);
	length = derPut(buf, length, "mua", kHKMaxUnlockAttempts);
	return derSet(buf, length);
}

/* One tag-length-value; returns false if it does not fit. */
static bool
derNext(const uint8_t ** p, const uint8_t * end, uint8_t * tag, const uint8_t ** value, size_t * length)
{
	const uint8_t * q = *p;
	size_t n;

	if (end - q < 2) {
		return false;
	}
	*tag = *q++;
	n = *q++;
	if (n & 0x80) {
		size_t bytes = n & 0x7f;
		if (bytes == 0 || bytes > 2 || (size_t)(end - q) < bytes) {
			return false;
		}
		n = 0;
		while (bytes--) {
			n = (n << 8) | *q++;
		}
	}
	if ((size_t)(end - q) < n) {
		return false;
	}
	*value = q;
	*length = n;
	*p = q + n;
	return true;
}

/* kAKSSetConfiguration: the grace period ("gp") is the one thing kept; the rest is noted. */
static void
setConfiguration(const uint8_t * der, size_t size)
{
	const uint8_t * p = der, * end = der + size, * value;
	uint8_t tag;
	size_t length;

	if (!derNext(&p, end, &tag, &value, &length) || (tag != 0x31 && tag != 0x30)) {
		return;
	}
	p = value;
	end = value + length;
	while (p < end) {
		const uint8_t * item, * key, * number, * q;
		size_t itemLength, keyLength, numberLength;
		uint8_t t1, t2;
		uint64_t v = 0;

		if (!derNext(&p, end, &tag, &item, &itemLength) || tag != 0x30) {
			return;
		}
		q = item;
		if (!derNext(&q, item + itemLength, &t1, &key, &keyLength) || t1 != 0x0c ||
		    !derNext(&q, item + itemLength, &t2, &number, &numberLength) || t2 != 0x02 || numberLength > 8) {
			continue;
		}
		for (size_t i = 0; i < numberLength; i++) {
			v = (v << 8) | number[i];
		}
		if (keyLength == 2 && memcmp(key, "gp", 2) == 0) {
			if (gKS.record.gracePeriod != (uint32_t) v) {
				gKS.record.gracePeriod = v > 4 * 3600 ? 4 * 3600 : (uint32_t) v;
				persist(&gKS.record);
				printf("MiniAppleKeyStore: grace period %u s\n", gKS.record.gracePeriod);
			}
		} else {
			printf("MiniAppleKeyStore: configuration %.*s = %llu (not kept)\n", (int) keyLength, key, v);
		}
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the self-test (QEMU) */

/*
 * kern.hakios.passcode_selftest: written 1 by qemu/run-passcode-test.sh's process 1 once /private/var is mounted,
 * with the boot-argument passcode_selftest=N saying which boot of the sequence this is. Off (EPERM) without the
 * boot-argument: it sets and removes passcodes. Each check logs "MiniAppleKeyStore: selftest N: ..."; reading the
 * sysctl afterwards gives 1 if every check passed, 2 if one failed.
 *   1  no passcode yet; set 123456; lock; four wrong tries (no delay), a fifth (a minute's delay); the right one is
 *      refused during the delay and not counted; the delay cleared (the test's own shortcut), the right one unlocks;
 *      a change with a wrong old passcode is refused; changed to abcdef with new metadata. Left set.
 *   2  after a restart: set, locked, not unlocked since boot, the metadata kept; a wrong try counts one; abcdef
 *      unlocks (the first unlock); removed. Then set 999999 and left set, for 3.
 *   3  with passcode_reset=1 as well: no passcode.
 */
static int gSelftestResult;

#define CHECK(what, cond) do { bool ok_ = (cond); printf("MiniAppleKeyStore: selftest %u: %s %s\n", phase, ok_ ? "ok  " : "FAIL", what); \
	if (!ok_) { failed = true; } } while (0)

static bool
selftest(uint32_t phase)
{
	bool failed = false, changed = false, first = false;
	const uint8_t * none = (const uint8_t *) "";

	if (!gKS.loaded) {
		load();
	}
	switch (phase) {
	case 1:
		CHECK("PBKDF2's test vector", gKS.kdfGood);
		CHECK("no passcode at the start", !gKS.record.hasPasscode && lockState() == kAKSLockDisabled);
		CHECK("set 123456", changeSecret(none, 0, (const uint8_t *) "123456", 6, (const uint8_t *) "BLOB1", 5, &changed, &first) == kIOReturnSuccess);
		CHECK("set and unlocked", gKS.record.hasPasscode && lockState() == kAKSLockUnlocked);
		gKS.locked = true;
		CHECK("locked", lockState() == kAKSLockLocked && (stateFlags() & kAKSStateLocked));
		for (int i = 1; i <= 4; i++) {
			CHECK("a wrong try is refused", tryPasscode((const uint8_t *) "000000", 6, true, &changed, &first) == kAKSWrongPasscode);
		}
		CHECK("four failed attempts, no delay", gKS.record.failedAttempts == 4 && secondsBlocked() == 0);
		CHECK("a fifth wrong try is refused", tryPasscode((const uint8_t *) "000000", 6, true, &changed, &first) == kAKSWrongPasscode);
		CHECK("five failed attempts, about a minute's delay", gKS.record.failedAttempts == 5 && secondsBlocked() > 55 && secondsBlocked() <= 60);
		CHECK("the right one during the delay is refused", tryPasscode((const uint8_t *) "123456", 6, true, &changed, &first) == kAKSBlocked);
		CHECK("... and not counted", gKS.record.failedAttempts == 5 && lockState() == kAKSLockLocked);
		gKS.blockedUntilUptime = 0;
		CHECK("the right one unlocks", tryPasscode((const uint8_t *) "123456", 6, true, &changed, &first) == kIOReturnSuccess);
		CHECK("unlocked, no failed attempts", lockState() == kAKSLockUnlocked && gKS.record.failedAttempts == 0 && gKS.record.blockedUntil == 0);
		CHECK("a change with a wrong old passcode is refused", changeSecret((const uint8_t *) "111111", 6, (const uint8_t *) "abcdef", 6,
		    (const uint8_t *) "BLOB2", 5, &changed, &first) == kAKSWrongPasscode);
		CHECK("changed to abcdef", changeSecret((const uint8_t *) "123456", 6, (const uint8_t *) "abcdef", 6,
		    (const uint8_t *) "BLOB2", 5, &changed, &first) == kIOReturnSuccess);
		CHECK("the old one no longer verifies", tryPasscode((const uint8_t *) "123456", 6, false, &changed, &first) == kAKSWrongPasscode);
		CHECK("the new one verifies", tryPasscode((const uint8_t *) "abcdef", 6, false, &changed, &first) == kIOReturnSuccess);
		break;
	case 2:
		CHECK("set after the restart", gKS.record.hasPasscode);
		CHECK("locked, not unlocked since boot", lockState() == kAKSLockLocked && !(stateFlags() & kAKSStateUnlockedOnce));
		CHECK("the metadata kept", gKS.record.blobLength == 5 && memcmp(gKS.record.blob, "BLOB2", 5) == 0);
		CHECK("a wrong try is refused", tryPasscode((const uint8_t *) "123456", 6, true, &changed, &first) == kAKSWrongPasscode);
		CHECK("one failed attempt", gKS.record.failedAttempts == 1);
		CHECK("abcdef unlocks, the first unlock", tryPasscode((const uint8_t *) "abcdef", 6, true, &changed, &first) == kIOReturnSuccess && first);
		CHECK("unlocked since boot", lockState() == kAKSLockUnlocked && (stateFlags() & kAKSStateUnlockedOnce));
		CHECK("removed", changeSecret((const uint8_t *) "abcdef", 6, none, 0, none, 0, &changed, &first) == kIOReturnSuccess);
		CHECK("no passcode", lockState() == kAKSLockDisabled && !gKS.record.hasPasscode);
		CHECK("set 999999 for the reset", changeSecret(none, 0, (const uint8_t *) "999999", 6, none, 0, &changed, &first) == kIOReturnSuccess);
		break;
	case 3:
		CHECK("passcode_reset=1 left no passcode", !gKS.record.hasPasscode && lockState() == kAKSLockDisabled);
		break;
	default:
		CHECK("a known phase", false);
		break;
	}
	printf("MiniAppleKeyStore: selftest %u: %s (generation %llu)\n", phase, failed ? "FAILED" : "ALL OK", gKS.generation);
	return !failed;
}

static int
selftestSysctl(__unused struct sysctl_oid * oidp, __unused void * arg1, __unused int arg2, struct sysctl_req * req)
{
	uint32_t phase = 0;
	int value = gSelftestResult, error = SYSCTL_OUT(req, &value, sizeof(value));

	if (error != 0 || req->newptr == 0) {
		return error;
	}
	if ((error = SYSCTL_IN(req, &value, sizeof(value))) != 0) {
		return error;
	}
	if (!PE_parse_boot_argn("passcode_selftest", &phase, sizeof(phase)) || phase == 0 || gAKSLock == NULL) {
		return EPERM;
	}
	IOLockLock(gAKSLock);
	gSelftestResult = selftest(phase) ? 1 : 2;
	IOLockUnlock(gAKSLock);
	return 0;
}
/* kern.hakios.passcode: the state, for the USB console ("sysctl kern.hakios.passcode") and for logs; nothing secret */
static int
stateSysctl(__unused struct sysctl_oid * oidp, __unused void * arg1, __unused int arg2, struct sysctl_req * req)
{
	char line[128];

	if (gAKSLock == NULL) {
		return ENOENT;
	}
	IOLockLock(gAKSLock);
	if (!gKS.loaded) {
		snprintf(line, sizeof(line), "not read yet (nothing has asked the key store)");
	} else {
		snprintf(line, sizeof(line), "passcode %s, lock state %u, %s, %u failed attempts, %u s of delay, grace %u s, generation %llu",
		    gKS.record.hasPasscode ? "set" : "none", lockState(), gKS.unlockedOnce ? "unlocked since boot" : "not unlocked since boot",
		    gKS.record.failedAttempts, secondsBlocked(), gKS.record.gracePeriod, gKS.generation);
	}
	IOLockUnlock(gAKSLock);
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}
SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, passcode, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    stateSysctl, "A", "the key store's passcode state (MiniAppleKeyStore.cpp)");
SYSCTL_PROC(_kern_hakios, OID_AUTO, passcode_selftest, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    selftestSysctl, "I", "the key store's passcode self-test (QEMU; needs the boot-argument passcode_selftest=N)");

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the service and its user client */

class AppleKeyStore : public IOService
{
	OSDeclareDefaultStructors(AppleKeyStore);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStore, IOService);

bool
AppleKeyStore::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "AppleKeyStoreUserClient");
	setName("AppleKeyStore");
	registerService();
	return true;
}

class AppleKeyStoreUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleKeyStoreUserClient);

	static IOReturn answer(IOExternalMethodArguments * args, const uint8_t * bytes, size_t length);
	static IOReturn input(IOExternalMethodArguments * args, uint8_t * buf, size_t size, size_t * length);

public:
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStoreUserClient, IOUserClient);

IOReturn
AppleKeyStoreUserClient::clientClose(void)
{
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

/* keybagd asks to be told about keys to back up and bags to save; there never are any. */
IOReturn
AppleKeyStoreUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	releaseNotificationPort(port);
	return kIOReturnSuccess;
}

/*
 * kAKSWrapKey and kAKSUnwrapKey. A wrapped key is 8 bytes longer than the key, as with AES key
 * wrap (RFC 3394), which is what callers size their buffers for: a mark, the class, and the key
 * mixed with a fixed pattern. Not encryption: there is no secret here to encrypt with, and the
 * passcode is not one (see the head of this file).
 */
static const uint8_t kWrapMark[4] = { 'H', 'K', 'w', 'r' };

static void
wrapMix(uint8_t * bytes, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		bytes[i] ^= (uint8_t)(0x5a + 37 * i);
	}
}

/* A structure answer; a big buffer on the other side arrives as a memory descriptor. */
IOReturn
AppleKeyStoreUserClient::answer(IOExternalMethodArguments * args, const uint8_t * bytes, size_t length)
{
	if (args->structureOutputDescriptor != NULL) {
		IOMemoryDescriptor * out = args->structureOutputDescriptor;
		IOReturn ret;

		if (out->getLength() < length) {
			return kIOReturnNoSpace;
		}
		ret = out->prepare();
		if (ret == kIOReturnSuccess) {
			out->writeBytes(0, bytes, length);
			out->complete();
			args->structureOutputDescriptorSize = (uint32_t) length;
		}
		return ret;
	}
	if (args->structureOutput == NULL || args->structureOutputSize < length) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, bytes, length);
	args->structureOutputSize = (uint32_t) length;
	return kIOReturnSuccess;
}

/* The structure input, inline or (a big one) as a memory descriptor, copied into buf. */
IOReturn
AppleKeyStoreUserClient::input(IOExternalMethodArguments * args, uint8_t * buf, size_t size, size_t * length)
{
	if (args->structureInputDescriptor != NULL) {
		IOMemoryDescriptor * in = args->structureInputDescriptor;
		size_t n = in->getLength();
		IOReturn ret;

		if (n > size) {
			return kIOReturnBadArgument;
		}
		ret = in->prepare();
		if (ret != kIOReturnSuccess) {
			return ret;
		}
		*length = in->readBytes(0, buf, n);
		in->complete();
		return *length == n ? kIOReturnSuccess : kIOReturnIOError;
	}
	if (args->structureInput == NULL || args->structureInputSize > size) {
		return kIOReturnBadArgument;
	}
	memcpy(buf, args->structureInput, args->structureInputSize);
	*length = args->structureInputSize;
	return kIOReturnSuccess;
}

IOReturn
AppleKeyStoreUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	static const uint8_t bagUUID[16] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64, 0x00, 0x6b, 0x65, 0x79, 0x62, 0x61, 0x67, 0x00, 0x01 };
	uint8_t der[256];
	uint64_t * out = args->scalarOutput;
	bool changed = false, first = false;
	IOReturn ret = kIOReturnUnsupported;
	bool handled = true;

	IOLockLock(gAKSLock);
	if (!gKS.loaded) {
		load();
	}

	switch (selector) {
	case kAKSOpenCheck:
	case kAKSUnlockBag:
	case kAKSAssertDrop:
	case kAKSPrewarm:
		ret = kIOReturnSuccess;
		break;

	case kAKSAssertHold:
	case kAKSAssertPromote:
	case kAKSAssertConsume:
		/*
		 * A lock assertion ("keep the device unlocked") can only be taken while it is unlocked. SpringBoard takes one
		 * to ask "is the passcode needed?" (SBFUserAuthenticationController, "Checking if passcode locked"): granted
		 * while locked, it lets the lock screen in without its keypad. Holding one does not keep the device from
		 * locking here.
		 */
		if (gKS.record.hasPasscode && gKS.locked) {
			ret = kAKSBlocked;
		} else {
			ret = kIOReturnSuccess;
		}
		break;

	case kAKSLockDevice:
	case kAKSLockDeviceNow:
		/* at once, whatever the grace period (see the head of this file) */
		if (gKS.record.hasPasscode && !gKS.locked) {
			char procname[32] = "?";

			gKS.locked = true;
			changed = true;
			proc_selfname(procname, sizeof(procname));
			printf("MiniAppleKeyStore: locked (%s[%d], %s)\n", procname, proc_selfpid(), selector == kAKSLockDeviceNow ? "now" : "lock");
		}
		ret = kIOReturnSuccess;
		break;

	case kAKSUnlockDevice: {
		uint8_t passcode[kHKMaxSecret];
		size_t length = 0;

		if (args->scalarInputCount >= 3 && args->scalarInput[1] != 0 && args->scalarInput[2] != 0) {
			length = (size_t) args->scalarInput[2];
			if (length > sizeof(passcode)) {
				ret = kIOReturnBadArgument;
				break;
			}
			if (copyin((user_addr_t) args->scalarInput[1], passcode, length) != 0) {
				ret = kIOReturnBadArgument;
				break;
			}
		} else if (args->structureInput != NULL && args->structureInputSize <= sizeof(passcode)) {
			length = args->structureInputSize;
			memcpy(passcode, args->structureInput, length);
		}
		ret = tryPasscode(passcode, length, true, &changed, &first);
		memset(passcode, 0, sizeof(passcode));
		break;
	}

	case kAKSVerifyPassword: {
		size_t size = 8 + kHKMaxSecret + 1024, length = 0;
		uint8_t * buf = IONew(uint8_t, size);
		uint32_t count = 0, passcodeLength = 0;

		if (buf == NULL) {
			ret = kIOReturnNoMemory;
			break;
		}
		ret = input(args, buf, size, &length);
		if (ret == kIOReturnSuccess && length >= 8) {
			memcpy(&count, buf, 4);
			memcpy(&passcodeLength, buf + 4, 4);
		}
		if (ret == kIOReturnSuccess && (length < 8 || count < 1 || passcodeLength > kHKMaxSecret || passcodeLength > length - 8)) {
			ret = kIOReturnBadArgument;
		}
		if (ret == kIOReturnSuccess) {
			ret = tryPasscode(buf + 8, passcodeLength, false, &changed, &first);
		}
		memset(buf, 0, size);
		IODelete(buf, uint8_t, size);
		break;
	}

	case kAKSGetLockState:
		if (args->scalarOutputCount < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		out[0] = stateFlags();
		args->scalarOutputCount = 1;
		ret = kIOReturnSuccess;
		break;

	case kAKSGetSystem:
	case kAKSGeneration:
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		ret = kIOReturnSuccess;
		break;

	case kAKSGetDeviceState:
	case kAKSGetExtendedState:
		ret = answer(args, der, deviceState(der));
		break;

	case kAKSGetConfiguration:
		ret = answer(args, der, configuration(der));
		break;

	case kAKSSetConfiguration: {
		uint8_t buf[256];
		size_t length = 0;

		ret = input(args, buf, sizeof(buf), &length);
		if (ret == kIOReturnSuccess) {
			setConfiguration(buf, length);
		}
		break;
	}

	case kAKSGetBagUUID:
		ret = answer(args, bagUUID, sizeof(bagUUID));
		break;

	case kAKSNotifyStateSet:
		gAKSNotifyState = true;
		ret = kIOReturnSuccess;
		break;

	case kAKSNotifyStateIsSet:
		if (args->scalarOutputCount < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		out[0] = gAKSNotifyState;
		args->scalarOutputCount = 1;
		ret = kIOReturnSuccess;
		break;

	case kAKSWrapKey: {
		uint8_t wrapped[8 + 64];
		size_t length = args->structureInputSize;

		if (args->structureInput == NULL || length == 0 || length > 64 || args->scalarInputCount < 2) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(wrapped, kWrapMark, sizeof(kWrapMark));
		wrapped[4] = (uint8_t)args->scalarInput[1];
		wrapped[5] = wrapped[6] = wrapped[7] = 0;
		memcpy(wrapped + 8, args->structureInput, length);
		wrapMix(wrapped + 8, length);
		if (args->scalarOutputCount >= 1) {
			out[0] = args->scalarInput[1];
			args->scalarOutputCount = 1;
		}
		ret = answer(args, wrapped, 8 + length);
		break;
	}

	case kAKSUnwrapKey: {
		uint8_t key[64];
		const uint8_t * wrapped = (const uint8_t *)args->structureInput;
		size_t length = args->structureInputSize;

		if (wrapped == NULL || length <= 8 || length > 8 + sizeof(key) || memcmp(wrapped, kWrapMark, sizeof(kWrapMark)) != 0) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(key, wrapped + 8, length - 8);
		wrapMix(key, length - 8);
		ret = answer(args, key, length - 8);
		break;
	}

	case kHKChangeSecret: {
		uint8_t * buf;
		size_t size = sizeof(HKChangeSecret) + 2 * kHKMaxSecret + kHKMaxBlob, length = 0;
		HKChangeSecret head = {};

		if (kauth_getuid() != 0) {
			ret = kIOReturnNotPrivileged;
			break;
		}
		buf = IONew(uint8_t, size);
		if (buf == NULL) {
			ret = kIOReturnNoMemory;
			break;
		}
		ret = input(args, buf, size, &length);
		if (ret == kIOReturnSuccess) {
			if (length >= sizeof(head)) {
				memcpy(&head, buf, sizeof(head));
			}
			if (length < sizeof(head) || head.magic != kHKChangeMagic || head.oldLength > kHKMaxSecret ||
			    head.newLength > kHKMaxSecret || head.blobLength > kHKMaxBlob ||
			    sizeof(head) + head.oldLength + head.newLength + head.blobLength != length) {
				ret = kIOReturnBadArgument;
			} else {
				const uint8_t * p = buf + sizeof(head);
				ret = changeSecret(p, head.oldLength, p + head.oldLength, head.newLength,
				    p + head.oldLength + head.newLength, head.blobLength, &changed, &first);
			}
		}
		memset(buf, 0, size);
		IODelete(buf, uint8_t, size);
		break;
	}

	case kHKCopyBlob:
		if (!gKS.record.hasPasscode || gKS.record.blobLength == 0) {
			ret = kIOReturnNotFound;
			break;
		}
		ret = answer(args, gKS.record.blob, gKS.record.blobLength);
		break;

	case kHKStatus:
		if (args->scalarOutputCount < 5) {
			ret = kIOReturnBadArgument;
			break;
		}
		out[0] = gKS.record.hasPasscode;
		out[1] = lockState();
		out[2] = gKS.record.failedAttempts;
		out[3] = secondsBlocked();
		out[4] = gKS.generation;
		args->scalarOutputCount = 5;
		ret = kIOReturnSuccess;
		break;

	default:
		handled = false;
		break;
	}
	IOLockUnlock(gAKSLock);

	announce(changed, first);
	if (handled) {
		return ret;
	}

	if (selector < 256 && !(gAKSLogged[selector / 64] & (1ULL << (selector % 64)))) {
		char procname[32] = "?";

		gAKSLogged[selector / 64] |= 1ULL << (selector % 64);
		proc_selfname(procname, sizeof(procname));
		printf("MiniAppleKeyStore: %s[%d] selector 0x%x not implemented (scalars %u in, %u out; structure %u in, %u out)\n",
		    procname, proc_selfpid(), selector, args->scalarInputCount, args->scalarOutputCount, args->structureInputSize,
		    args->structureOutputSize);
	}
	return kIOReturnUnsupported;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void
MiniAppleKeyStoreStart(void)
{
	/* user space looks for it by class, and by the path IOService:/IOResources/AppleKeyStore */
	IOService * resources = IOService::getResourceService();
	AppleKeyStore * store = OSTypeAlloc(AppleKeyStore);

	gAKSLock = IOLockAlloc();
	if (gAKSLock == NULL || resources == NULL || store == NULL || !store->init() || !store->attach(resources)) {
		OSSafeReleaseNULL(store);
		printf("MiniAppleKeyStore: cannot create the service\n");
		return;
	}
	if (!store->start(resources)) {
		store->detach(resources);
		store->release();
		return;
	}
	gAKSService = store;            /* the registry holds it; so does this */
}
