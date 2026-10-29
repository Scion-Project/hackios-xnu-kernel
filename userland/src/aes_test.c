/*
 * aes_test: IOAESAccelerator from user space, the way iOS's own clients call it
 * (xnu/files/iokit/Kernel/MiniIOAESAccelerator.cpp).
 *
 *   /redwood/bin/aes_test
 *
 * Finds the service by class, opens it and round-trips AES-CBC through selector 1 with the
 * 0x58-byte request that libFDR, MobileBackup, online-auth-agent and softwareupdated use:
 *
 *   - NIST SP 800-38A F.2.1-F.2.6: AES-128, -192 and -256 in CBC, four blocks each way, and the
 *     chaining value handed back (the last cipher block) checked too
 *   - 64 KB of a pattern encrypted in two calls, the second chained on the IV the first returned,
 *     against one call over the whole; then decrypted in place and compared with the pattern
 *   - what must be refused: a key handle of the AES engine (0x839, libFDR's), a length that is not
 *     a multiple of 16, a missing buffer, a selector that does not exist
 *
 * One line per check, then "aes_test: N of M passed"; exit status 0 only when all passed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

struct request {                        /* the clients' structure: 0x58 bytes in and out */
	uint64_t cleartext;             /* 0x00 */
	uint64_t ciphertext;            /* 0x08 */
	uint32_t size;                  /* 0x10 */
	uint8_t  iv[16];                /* 0x14 */
	uint32_t mode;                  /* 0x24 0 encrypt, 1 decrypt */
	uint32_t bits;                  /* 0x28 */
	uint8_t  key[32];               /* 0x2c */
	uint32_t key_handle;            /* 0x4c */
	uint32_t reserved[2];           /* 0x50 */
};
_Static_assert(sizeof(struct request) == 0x58, "the request is 0x58 bytes");

/*
 * The plain text again, alone in a page of this program's constant data that nothing touches before the
 * driver reads it: a page that is not in memory yet. (The first driver read such a page as zeros.)
 */
static const struct {
	uint8_t before[16384];
	uint8_t text[64];
	uint8_t after[16384 - 64];
} far __attribute__((aligned(16384))) = { { 1 }, {
	0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
	0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
	0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
	0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10 }, { 1 } };

static int passed, total;

static void
check(int ok, const char *what)
{
	total++;
	passed += ok != 0;
	printf("aes_test: [%s] %s\n", ok ? " ok " : "FAIL", what);
}

static void
hex(const char *what, const uint8_t *p, size_t n)
{
	printf("aes_test:   %s ", what);
	for (size_t i = 0; i < n; i++) {
		printf("%02x", p[i]);
	}
	printf("\n");
}

/* One call of selector 1; *out_iv gets the chaining value handed back. */
static kern_return_t
perform(io_connect_t conn, int decrypt, const uint8_t *key, uint32_t bits, uint32_t handle, const uint8_t iv[16],
    void *clear, void *cipher, uint32_t size, uint8_t out_iv[16])
{
	struct request r;
	size_t out_size = sizeof(r);
	kern_return_t kr;

	memset(&r, 0, sizeof(r));
	r.cleartext = (uint64_t)(uintptr_t)clear;
	r.ciphertext = (uint64_t)(uintptr_t)cipher;
	r.size = size;
	memcpy(r.iv, iv, 16);
	r.mode = decrypt ? 1 : 0;
	r.bits = bits;
	if (key != NULL) {
		memcpy(r.key, key, bits / 8);
	}
	r.key_handle = handle;
	kr = IOConnectCallStructMethod(conn, 1, &r, sizeof(r), &r, &out_size);
	if (kr == KERN_SUCCESS && out_iv != NULL) {
		memcpy(out_iv, r.iv, 16);
	}
	return kr;
}

int
main(void)
{
	static const uint8_t plain[64] = {
		0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
		0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
		0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
		0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10
	};
	static const uint8_t iv0[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
	static const struct {
		const char *name;
		uint32_t bits;
		uint8_t key[32];
		uint8_t cipher[64];
	} vectors[] = {
		{ "F.2.1/F.2.2 CBC-AES128", 128,
		  { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c },
		  { 0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46, 0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d,
		    0x50, 0x86, 0xcb, 0x9b, 0x50, 0x72, 0x19, 0xee, 0x95, 0xdb, 0x11, 0x3a, 0x91, 0x76, 0x78, 0xb2,
		    0x73, 0xbe, 0xd6, 0xb8, 0xe3, 0xc1, 0x74, 0x3b, 0x71, 0x16, 0xe6, 0x9e, 0x22, 0x22, 0x95, 0x16,
		    0x3f, 0xf1, 0xca, 0xa1, 0x68, 0x1f, 0xac, 0x09, 0x12, 0x0e, 0xca, 0x30, 0x75, 0x86, 0xe1, 0xa7 } },
		{ "F.2.3/F.2.4 CBC-AES192", 192,
		  { 0x8e, 0x73, 0xb0, 0xf7, 0xda, 0x0e, 0x64, 0x52, 0xc8, 0x10, 0xf3, 0x2b, 0x80, 0x90, 0x79, 0xe5,
		    0x62, 0xf8, 0xea, 0xd2, 0x52, 0x2c, 0x6b, 0x7b },
		  { 0x4f, 0x02, 0x1d, 0xb2, 0x43, 0xbc, 0x63, 0x3d, 0x71, 0x78, 0x18, 0x3a, 0x9f, 0xa0, 0x71, 0xe8,
		    0xb4, 0xd9, 0xad, 0xa9, 0xad, 0x7d, 0xed, 0xf4, 0xe5, 0xe7, 0x38, 0x76, 0x3f, 0x69, 0x14, 0x5a,
		    0x57, 0x1b, 0x24, 0x20, 0x12, 0xfb, 0x7a, 0xe0, 0x7f, 0xa9, 0xba, 0xac, 0x3d, 0xf1, 0x02, 0xe0,
		    0x08, 0xb0, 0xe2, 0x79, 0x88, 0x59, 0x88, 0x81, 0xd9, 0x20, 0xa9, 0xe6, 0x4f, 0x56, 0x15, 0xcd } },
		{ "F.2.5/F.2.6 CBC-AES256", 256,
		  { 0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe, 0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
		    0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7, 0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4 },
		  { 0xf5, 0x8c, 0x4c, 0x04, 0xd6, 0xe5, 0xf1, 0xba, 0x77, 0x9e, 0xab, 0xfb, 0x5f, 0x7b, 0xfb, 0xd6,
		    0x9c, 0xfc, 0x4e, 0x96, 0x7e, 0xdb, 0x80, 0x8d, 0x67, 0x9f, 0x77, 0x7b, 0xc6, 0x70, 0x2c, 0x7d,
		    0x39, 0xf2, 0x33, 0x69, 0xa9, 0xd9, 0xba, 0xcf, 0xa5, 0x30, 0xe2, 0x63, 0x04, 0x23, 0x14, 0x61,
		    0xb2, 0xeb, 0x05, 0xe2, 0xc3, 0x9b, 0xe9, 0xfc, 0xda, 0x6c, 0x19, 0x07, 0x8c, 0x6a, 0x9d, 0x1b } },
	};
	char line[160];
	uint8_t out[64], back[64], iv[16];
	io_service_t service;
	io_connect_t conn = IO_OBJECT_NULL;
	kern_return_t kr;
	CFTypeRef soft;

	service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAESAccelerator"));
	check(service != IO_OBJECT_NULL, "IOServiceMatching(\"IOAESAccelerator\") finds the service");
	if (service == IO_OBJECT_NULL) {
		printf("aes_test: %d of %d passed\n", passed, total);
		return 1;
	}
	soft = IORegistryEntryCreateCFProperty(service, CFSTR("NativeAESSoftwareKeysOnly"), kCFAllocatorDefault, 0);
	check(soft == kCFBooleanTrue, "it says NativeAESSoftwareKeysOnly");
	if (soft != NULL) {
		CFRelease(soft);
	}
	/* what OTATaskingAgent reads off it */
	soft = IORegistryEntryCreateCFProperty(service, CFSTR("CrashReporter-ID"), kCFAllocatorDefault, 0);
	check(soft != NULL && CFGetTypeID(soft) == CFDataGetTypeID() && CFDataGetLength(soft) == 20,
	    "it has a CrashReporter-ID of 20 bytes");
	if (soft != NULL) {
		CFRelease(soft);
	}
	kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
	IOObjectRelease(service);
	snprintf(line, sizeof(line), "IOServiceOpen: 0x%x", kr);
	check(kr == KERN_SUCCESS, line);
	if (kr != KERN_SUCCESS) {
		printf("aes_test: %d of %d passed\n", passed, total);
		return 1;
	}

	memset(out, 0, sizeof(out));
	kr = perform(conn, 0, vectors[0].key, 128, 0, iv0, (void *)far.text, out, 64, iv);
	snprintf(line, sizeof(line), "CBC-AES128 encrypt from a page not yet in memory: 0x%x, cipher text as NIST's", kr);
	check(kr == KERN_SUCCESS && memcmp(out, vectors[0].cipher, 64) == 0, line);

	for (size_t v = 0; v < sizeof(vectors) / sizeof(vectors[0]); v++) {
		memset(out, 0, sizeof(out));
		kr = perform(conn, 0, vectors[v].key, vectors[v].bits, 0, iv0, (void *)plain, out, 64, iv);
		snprintf(line, sizeof(line), "%s encrypt: 0x%x, cipher text as NIST's, IV handed back = last block", vectors[v].name, kr);
		check(kr == KERN_SUCCESS && memcmp(out, vectors[v].cipher, 64) == 0 && memcmp(iv, vectors[v].cipher + 48, 16) == 0, line);
		if (memcmp(out, vectors[v].cipher, 64) != 0) {
			hex("got     ", out, 64);
			hex("expected", vectors[v].cipher, 64);
		}
		memset(back, 0, sizeof(back));
		kr = perform(conn, 1, vectors[v].key, vectors[v].bits, 0, iv0, back, (void *)vectors[v].cipher, 64, iv);
		snprintf(line, sizeof(line), "%s decrypt: 0x%x, plain text as NIST's", vectors[v].name, kr);
		check(kr == KERN_SUCCESS && memcmp(back, plain, 64) == 0 && memcmp(iv, vectors[v].cipher + 48, 16) == 0, line);
	}

	/* 64 KB: one call against two chained ones, then back in place */
	{
		const uint32_t n = 65536, half = 16384 + 48;            /* the split off any 4 KB boundary of the driver's */
		uint8_t *text = malloc(n), *whole = malloc(n), *parts = malloc(n);
		uint8_t mid[16], end1[16], end2[16];
		int ok;

		for (uint32_t i = 0; i < n; i++) {
			text[i] = (uint8_t)(i * 31 + (i >> 8));
		}
		ok = perform(conn, 0, vectors[2].key, 256, 0, iv0, text, whole, n, end1) == KERN_SUCCESS &&
		    perform(conn, 0, vectors[2].key, 256, 0, iv0, text, parts, half, mid) == KERN_SUCCESS &&
		    perform(conn, 0, vectors[2].key, 256, 0, mid, text + half, parts + half, n - half, end2) == KERN_SUCCESS;
		check(ok && memcmp(whole, parts, n) == 0 && memcmp(end1, end2, 16) == 0 && memcmp(mid, parts + half - 16, 16) == 0,
		    "64 KB AES-256: two calls chained on the returned IV = one call");
		ok = perform(conn, 1, vectors[2].key, 256, 0, iv0, whole, whole, n, NULL) == KERN_SUCCESS;
		check(ok && memcmp(whole, text, n) == 0, "64 KB decrypted in place = the plain text");
		free(text);
		free(whole);
		free(parts);
	}

	/* what must be refused */
	kr = perform(conn, 0, NULL, 128, 0x839, iv0, (void *)plain, out, 64, NULL);
	snprintf(line, sizeof(line), "hardware key 0x839 (libFDR's): 0x%x, expected kIOReturnUnsupported 0xe00002c7", kr);
	check(kr == (kern_return_t)0xe00002c7, line);
	kr = perform(conn, 0, vectors[0].key, 128, 0, iv0, (void *)plain, out, 15, NULL);
	snprintf(line, sizeof(line), "15 bytes: 0x%x, expected kIOReturnBadArgument 0xe00002c2", kr);
	check(kr == (kern_return_t)0xe00002c2, line);
	kr = perform(conn, 0, vectors[0].key, 100, 0, iv0, (void *)plain, out, 16, NULL);
	snprintf(line, sizeof(line), "a 100-bit key: 0x%x, expected kIOReturnBadArgument 0xe00002c2", kr);
	check(kr == (kern_return_t)0xe00002c2, line);
	kr = perform(conn, 0, vectors[0].key, 128, 0, iv0, NULL, out, 16, NULL);
	snprintf(line, sizeof(line), "no plain text buffer: 0x%x, expected kIOReturnBadArgument 0xe00002c2", kr);
	check(kr == (kern_return_t)0xe00002c2, line);
	{
		size_t none = 0;

		kr = IOConnectCallStructMethod(conn, 7, NULL, 0, NULL, &none);
		snprintf(line, sizeof(line), "selector 7: 0x%x, expected kIOReturnUnsupported 0xe00002c7", kr);
		check(kr == (kern_return_t)0xe00002c7, line);
	}

	IOServiceClose(conn);
	printf("aes_test: %d of %d passed\n", passed, total);
	return passed == total ? 0 : 1;
}
