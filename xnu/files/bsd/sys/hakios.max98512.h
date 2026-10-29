/*
 * The Galaxy S9's speaker amplifiers (drivers/audio/hakios.max98512.c): two Maxim MAX98512s on the HSI2C of USI07 of
 * PERIC1, fed by UAIF1 of ABOX over I2S. The driver starts itself (boot-arg max98512=N); what it offers the rest of the
 * kernel is the hook below, through which ABOX's driver turns the amplifiers on once UAIF1's clocks run and off before
 * they stop.
 *
 * The hook is ABOX's (sys/hakios.exynos_abox.h, in its own driver). It is declared here as well, for a kernel built
 * without it: whichever of the two headers comes first declares it and defines HAKIOS_ABOX_AMP_DECLARED, and the other
 * one then leaves it alone. When ABOX's driver is not in the kernel, hakios_abox_set_amp() is a weak stand-in in
 * hakios.max98512.c (see there): XNU's kernel is linked -static, where ld64 refuses an undefined weak reference but takes
 * a weak definition and lets a strong one of the same name win.
 */
#ifndef _SYS_HAKIOS_MAX98512_H_
#define _SYS_HAKIOS_MAX98512_H_

#include <stdint.h>

#ifndef HAKIOS_ABOX_AMP_DECLARED
#define HAKIOS_ABOX_AMP_DECLARED
struct hakios_abox_amp {
	const char *name;
	int (*start)(uint32_t rate, uint32_t slot_bits, uint32_t channels);   /* after UAIF1's clocks run; 0 or an errno */
	void (*stop)(void);                                                    /* before they stop */
};
void hakios_abox_set_amp(const struct hakios_abox_amp *amp);              /* NULL: none */
#endif

/*
 * What the MAX98512 driver's hook asks of its caller: start() and stop() from a thread (they take a lock and talk over
 * I2C), never at once from two; start() takes some tens of milliseconds (both amplifiers reset and set up anew), stop()
 * about 16 (the amplifiers ramp down on UAIF1's clocks, so the clocks must still run until it returns). start() takes
 * 48000 Hz, two channels (I2S: channel 0 the top speaker, 1 the bottom one) of 16, 24 or 32 bits, and refuses the rest.
 * hakios_abox_set_amp() may be called before ABOX's driver has started: it is to keep the pointer, nothing more.
 */

#endif /* _SYS_HAKIOS_MAX98512_H_ */
