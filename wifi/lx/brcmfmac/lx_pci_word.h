/*
 * A byte, half-word or word of PCI configuration space, in and out of the 32-bit word the root complex reads and
 * writes (lx_pci.c). Plain arithmetic, also compiled on the host by tools/brcmfmac-test.py.
 */
#ifndef _LX_PCI_WORD_H_
#define _LX_PCI_WORD_H_

/* what a read of width bytes at reg finds in word, the word of reg & ~3 */
static inline uint32_t
lx_pci_word_get(uint32_t word, unsigned reg, int width)
{
	unsigned shift = (reg & 3) * 8;

	return (width >= 4 ? word : (word >> shift) & ((1u << (8 * width)) - 1));
}

/*
 * The word to write for a write of width bytes of value at reg, word being what is there now. The status half of
 * the command word (offset 4) is written as 0, which changes nothing there: its bits are cleared by writing 1.
 */
static inline uint32_t
lx_pci_word_put(uint32_t word, unsigned reg, uint32_t value, int width)
{
	unsigned shift = (reg & 3) * 8;
	uint32_t mask;

	if (width >= 4) {
		return (value);
	}
	mask = ((1u << (8 * width)) - 1) << shift;
	if ((reg & ~3u) == 4) {
		word &= 0xffff;
	}
	return ((word & ~mask) | ((value << shift) & mask));
}

#endif
