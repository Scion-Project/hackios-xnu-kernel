/* Host test of xnuloader/chooser.h (tools/chooser-test.sh): the key decode, the boot image header, the device tree edit. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../xnuloader/chooser.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* a tiny tree builder: struct block and strings block */
struct tb { uint8_t st[4096], str[512]; uint32_t sn, strn; };

static void tb32(struct tb *t, uint32_t v) { cz_put_be32(t->st + t->sn, v); t->sn += 4; }
static void tbnode(struct tb *t, const char *n) { tb32(t, 1); uint32_t l = strlen(n) + 1; memset(t->st + t->sn, 0, (l + 3) & ~3u); memcpy(t->st + t->sn, n, l); t->sn += (l + 3) & ~3u; }
static void tbprop(struct tb *t, const char *n, const void *d, uint32_t len)
{
	uint32_t off = t->strn;
	memcpy(t->str + t->strn, n, strlen(n) + 1); t->strn += strlen(n) + 1;
	tb32(t, 3); tb32(t, len); tb32(t, off);
	memset(t->st + t->sn, 0, (len + 3) & ~3u); memcpy(t->st + t->sn, d, len); t->sn += (len + 3) & ~3u;
}
static uint32_t tbfinish(struct tb *t, uint8_t *out)
{
	uint32_t so = 56;  /* 40 header + one empty reservation entry (16) */
	memset(out, 0, so);
	cz_put_be32(out, FDT_MAGIC); cz_put_be32(out + 8, so); cz_put_be32(out + 16, 40); cz_put_be32(out + 20, 17);
	cz_put_be32(out + 24, 16); cz_put_be32(out + 32, t->strn); cz_put_be32(out + 36, t->sn);
	memcpy(out + so, t->st, t->sn); cz_put_be32(out + so + t->sn - 0, 0);
	/* (the struct block above has no FDT_END yet: add it) */
	cz_put_be32(out + so + t->sn, FDT_END);
	cz_put_be32(out + 36, t->sn + 4);
	cz_put_be32(out + 12, so + t->sn + 4);
	memcpy(out + so + t->sn + 4, t->str, t->strn);
	cz_put_be32(out + 4, so + t->sn + 4 + t->strn);
	return so + t->sn + 4 + t->strn;
}

/* find a property of /chosen in a tree; returns its length or -1 */
static int chosen_prop(const uint8_t *f, const char *name, const uint8_t **val)
{
	uint32_t so = cz_be32(f + 8), ss = cz_be32(f + 12), n = cz_be32(f + 36), d = 0, i;
	int in = 0;
	for (i = 0; i < n;) {
		uint32_t t = cz_be32(f + so + i);
		if (t == FDT_BEGIN_NODE) { uint32_t l = strlen((const char *)f + so + i + 4) + 1; if (d == 1 && !strcmp((const char *)f + so + i + 4, "chosen")) in = 1; d++; i += 4 + cz_pad4(l); }
		else if (t == FDT_END_NODE) { if (in && d == 2) in = 0; d--; i += 4; }
		else if (t == FDT_PROP) {
			uint32_t len = cz_be32(f + so + i + 4), no = cz_be32(f + so + i + 8);
			if (in && !strcmp((const char *)f + ss + no, name)) { *val = f + so + i + 12; return len; }
			i += 12 + cz_pad4(len);
		} else if (t == FDT_NOP) i += 4; else break;
	}
	return -1;
}

int main(void)
{
	static uint8_t in[8192], out[8192];
	struct tb t;
	const uint8_t *v;
	uint32_t n, sz;
	uint32_t cell = 0;

	/* keys: bit 3 low is Volume Up held */
	CHECK(cz_volup(0xfffffff7) && !cz_volup(0xffffffff) && !cz_volup(0xffffffef));
	{ int run = 0; CHECK(!cz_debounce(&run, 1)); CHECK(!cz_debounce(&run, 1)); CHECK(!cz_debounce(&run, 0)); CHECK(!cz_debounce(&run, 1));
	  CHECK(!cz_debounce(&run, 1)); CHECK(cz_debounce(&run, 1)); }

	/* a tree with a /chosen that has bootargs (to be replaced) and stdout-path (kept) */
	memset(&t, 0, sizeof(t));
	tbnode(&t, ""); tbprop(&t, "#address-cells", "\0\0\0\2", 4);
	tbnode(&t, "chosen"); tbprop(&t, "bootargs", "old\0", 4); tbprop(&t, "stdout-path", "serial0", 8);
	tb32(&t, 2); tbnode(&t, "memory"); tbprop(&t, "device_type", "memory", 7); tb32(&t, 2); tb32(&t, 2);
	sz = tbfinish(&t, in);
	n = cz_fdt_set_chosen(in, sz, out, sizeof(out), "console=ttySAC0 androidboot.x=1", 0xA8000000ULL, 0xA8100000ULL);
	CHECK(n != 0 && n == cz_be32(out + 4));
	CHECK(chosen_prop(out, "bootargs", &v) == 32 && !strcmp((const char *)v, "console=ttySAC0 androidboot.x=1"));
	CHECK(chosen_prop(out, "linux,initrd-start", &v) == 8 && cz_be32(v) == 0 && cz_be32(v + 4) == 0xA8000000u);
	CHECK(chosen_prop(out, "linux,initrd-end", &v) == 8 && cz_be32(v + 4) == 0xA8100000u);
	CHECK(chosen_prop(out, "stdout-path", &v) == 8 && !strcmp((const char *)v, "serial0"));
	/* old bootargs gone: exactly one */
	{ int c = 0; for (uint32_t i = 0; i + 4 <= n; i++) if (!memcmp(out + i, "old", 4)) c++; CHECK(c == 0); }
	/* running it again on its own output is stable */
	{ static uint8_t out2[8192]; uint32_t m = cz_fdt_set_chosen(out, n, out2, sizeof(out2), "x", 1, 2);
	  CHECK(m != 0 && chosen_prop(out2, "bootargs", &v) == 2 && cz_be32(v + 0) == 'x' << 24); (void)cell; }
	/* too small an output is refused, not overrun */
	CHECK(cz_fdt_set_chosen(in, sz, out, sz + 8, "console=ttySAC0", 1, 2) == 0);

	/* no /chosen: made */
	memset(&t, 0, sizeof(t));
	tbnode(&t, ""); tbnode(&t, "memory"); tbprop(&t, "device_type", "memory", 7); tb32(&t, 2); tb32(&t, 2);
	sz = tbfinish(&t, in);
	n = cz_fdt_set_chosen(in, sz, out, sizeof(out), "a b", 0x100, 0x200);
	CHECK(n != 0 && chosen_prop(out, "bootargs", &v) == 4 && chosen_prop(out, "linux,initrd-end", &v) == 8);

	/* not a tree */
	memset(in, 0, 64);
	CHECK(cz_fdt_set_chosen(in, 64, out, sizeof(out), "a", 1, 2) == 0);

	/* boot image: header version 2, 4096-byte pages */
	{
		static uint8_t h[CZ_BOOT_HDR_MAX];
		struct cz_bootimg bi;
		memset(h, 0, sizeof(h)); memcpy(h, "ANDROID!", 8);
		h[8] = 0x00; h[9] = 0x30; /* kernel 0x3000 */
		h[16] = 0x10; h[17] = 0x10; /* ramdisk 0x1010 */
		h[36 + 1] = 0x10; /* page 4096 */
		h[40] = 2; strcpy((char *)h + 64, "console=ttySAC0"); strcpy((char *)h + 608, "androidboot.hardware=samsungexynos9810");
		h[1648 + 1] = 0x08; /* dtb 0x800 */
		CHECK(cz_bootimg_parse(h, 1 << 20, &bi) == NULL);
		CHECK(bi.kernel_off == 4096 && bi.ramdisk_off == 4096 + 0x3000 && bi.dtb_off == 4096 + 0x3000 + 0x2000 && bi.dtb_size == 0x800);
		CHECK(!strcmp(bi.cmdline, "console=ttySAC0 androidboot.hardware=samsungexynos9810"));
		CHECK(cz_bootimg_parse(h, 0x4000, &bi) != NULL);        /* past the end of the partition */
		h[36 + 1] = 0x0f; CHECK(cz_bootimg_parse(h, 1 << 20, &bi) != NULL);   /* page size 3840 */
		h[36 + 1] = 0x10; h[0] = 'X'; CHECK(cz_bootimg_parse(h, 1 << 20, &bi) != NULL);
		h[0] = 'A'; h[40] = 0; h[41] = 0x04;                    /* legacy dt_size 0x400 behind second */
		CHECK(cz_bootimg_parse(h, 1 << 20, &bi) == NULL && bi.dtb_size == 0x400 && bi.dtb_off == 4096 + 0x3000 + 0x2000);
	}
	printf(fails ? "chooser-test: %d FAILED\n" : "chooser-test: all passed\n", fails);
	return fails != 0;
}
