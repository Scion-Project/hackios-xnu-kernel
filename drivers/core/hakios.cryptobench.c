/*
 * sysctl kern.hakios.crypto_bench: the kernel's SHA-256, SHA-1 and AES-128-CBC over a page, with the ARMv8 crypto instructions
 * and in C, in MB/s, on whichever core the reader is on (osfmk/minicrypto/mc_ce.c). Measured when it is read: about 0.6 s of
 * one core; nothing runs unless asked.
 */
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern void mc_ce_bench(char *text, size_t size);

SYSCTL_DECL(_kern_hakios);

static int
crypto_bench SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[160];

	if (req->oldptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, NULL, sizeof(text));
	}
	mc_ce_bench(text, sizeof(text));
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, crypto_bench, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, NULL, 0, crypto_bench, "A",
    "the kernel's SHA and AES over a page, with the ARMv8 instructions and in C, MB/s (measured on reading)");
