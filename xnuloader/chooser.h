/*
 * Boot chooser, pure part (CHOOSER=1; chooser.c is the UEFI part): the Volume Up register bit, the Android boot image
 * header, and the device tree edit that gives the Android kernel its command line and ramdisk. No UEFI, no hardware:
 * tools/chooser-test.sh builds it on the host.
 *
 * Written from the designs, not from anyone's code. Facts used, with their sources:
 *   keys: Starbite (shim/starlte/recovery.c, GPL-2.0-only, read for the register facts only): the ALIVE block's GPA0 data
 *         register at 0x14050024, bit 3 Volume Up, bit 4 Volume Down, bit 6 Bixby; GPA2's at 0x14050064, bit 4 Power; all
 *         active low. Not yet read on a phone by Scion.
 *   Android boot image header, versions 0-2: AOSP's bootimg.h (Apache-2.0, a public format).
 *   flattened device tree: the Devicetree Specification, section 5.
 */
#ifndef XNULOADER_CHOOSER_H
#define XNULOADER_CHOOSER_H

#define CZ_GPA0_DAT     0x14050024UL
#define CZ_VOLUP_BIT    3
#define CZ_DEBOUNCE     3               /* consecutive samples of "pressed" */

/* a pad read as 0 is a key held (active low) */
static int
cz_volup(uint32_t gpa0)
{
	return ((gpa0 >> CZ_VOLUP_BIT) & 1) == 0;
}

/* one sample in; 1 once CZ_DEBOUNCE in a row have been pressed */
static int
cz_debounce(int *run, int pressed)
{
	*run = pressed ? *run + 1 : 0;
	return *run >= CZ_DEBOUNCE;
}

static uint32_t
cz_le32(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t
cz_be32(const uint8_t *p)
{
	return p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}

static void
cz_put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static uint32_t
cz_len(const char *s)
{
	uint32_t n = 0;

	while (s[n]) {
		n++;
	}
	return n;
}

/* ---- Android boot image ---- */

#define CZ_BOOT_HDR_MAX 1664            /* through dtb_addr of version 2 */
#define CZ_KERNEL_MAX   (64u << 20)
#define CZ_RAMDISK_MAX  (128u << 20)
#define CZ_DTB_MAX      (2u << 20)

struct cz_bootimg {
	uint64_t kernel_off, ramdisk_off, dtb_off;      /* from the start of the image */
	uint32_t kernel_size, ramdisk_size, dtb_size;
	uint32_t page_size, version, os_version;
	char cmdline[1536];                             /* cmdline, then extra_cmdline, as one string */
};

/*
 * hdr: the first CZ_BOOT_HDR_MAX bytes; image_max: the bytes there are to read (the partition). NULL when it is fine,
 * else what is wrong. A header_version field above 2 is the older header's dt_size (Samsung's, at the same offset).
 */
static const char *
cz_bootimg_parse(const uint8_t *hdr, uint64_t image_max, struct cz_bootimg *bi)
{
	uint32_t ps, second, hv, rdtbo = 0, n;
	uint64_t off;

	if (memcmp(hdr, "ANDROID!", 8) != 0) {
		return "no ANDROID! magic";
	}
	memset(bi, 0, sizeof(*bi));
	bi->kernel_size = cz_le32(hdr + 8);
	bi->ramdisk_size = cz_le32(hdr + 16);
	second = cz_le32(hdr + 24);
	ps = cz_le32(hdr + 36);
	hv = cz_le32(hdr + 40);
	bi->page_size = ps;
	bi->os_version = cz_le32(hdr + 44);
	if (ps < 2048 || ps > 65536 || (ps & (ps - 1)) != 0) {
		return "page size is not a power of two in 2048..65536";
	}
	if (bi->kernel_size == 0 || bi->kernel_size > CZ_KERNEL_MAX || bi->ramdisk_size > CZ_RAMDISK_MAX) {
		return "kernel or ramdisk size out of range";
	}
	bi->version = hv <= 2 ? hv : 0;
	off = ps;
	bi->kernel_off = off;
	off += ((uint64_t)bi->kernel_size + ps - 1) / ps * ps;
	bi->ramdisk_off = off;
	off += ((uint64_t)bi->ramdisk_size + ps - 1) / ps * ps;
	off += ((uint64_t)second + ps - 1) / ps * ps;
	if (hv > 2) {
		bi->dtb_size = hv;                      /* legacy dt_size, behind second */
		bi->dtb_off = off;
	} else if (hv >= 1) {
		rdtbo = cz_le32(hdr + 1632);
		off += ((uint64_t)rdtbo + ps - 1) / ps * ps;
		if (hv == 2) {
			bi->dtb_size = cz_le32(hdr + 1648);
			bi->dtb_off = off;
		}
	}
	if (bi->dtb_size > CZ_DTB_MAX) {
		return "device tree larger than 2 MiB";
	}
	if (bi->dtb_size != 0 && bi->dtb_off + bi->dtb_size > image_max) {
		return "device tree past the end of the partition";
	}
	if (bi->kernel_off + bi->kernel_size > image_max || bi->ramdisk_off + bi->ramdisk_size > image_max) {
		return "kernel or ramdisk past the end of the partition";
	}
	for (n = 0; n < 512 && hdr[64 + n]; n++) {
		bi->cmdline[n] = (char)hdr[64 + n];
	}
	if (hv <= 2) {
		bi->cmdline[n++] = ' ';
		for (uint32_t m = 0; m < 1024 && hdr[608 + m]; m++) {
			bi->cmdline[n++] = (char)hdr[608 + m];
		}
	}
	bi->cmdline[n] = 0;
	return NULL;
}

/* ---- device tree ---- */

#define FDT_MAGIC       0xd00dfeedu
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

static uint32_t
cz_pad4(uint32_t n)
{
	return (n + 3) & ~3u;
}

static int
cz_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}
	return *a == *b;
}

/* is this property of /chosen one we write ourselves? */
static int
cz_ours(const char *name)
{
	return cz_streq(name, "bootargs") || cz_streq(name, "linux,initrd-start") ||
	    cz_streq(name, "linux,initrd-end");
}

struct cz_out {
	uint8_t *p;
	uint32_t n, max;
	int bad;
};

static void
cz_emit(struct cz_out *o, const void *d, uint32_t len)
{
	if (o->bad || len > o->max - o->n) {
		o->bad = 1;
		return;
	}
	memcpy(o->p + o->n, d, len);
	o->n += len;
}

static void
cz_emit_u32(struct cz_out *o, uint32_t v)
{
	uint8_t b[4];

	cz_put_be32(b, v);
	cz_emit(o, b, 4);
}

static void
cz_emit_prop(struct cz_out *o, uint32_t nameoff, const void *d, uint32_t len)
{
	static const uint8_t zero[4];

	cz_emit_u32(o, FDT_PROP);
	cz_emit_u32(o, len);
	cz_emit_u32(o, nameoff);
	cz_emit(o, d, len);
	cz_emit(o, zero, cz_pad4(len) - len);
}

/*
 * Copy the tree 'in' to 'out' (outmax bytes) with /chosen's bootargs, linux,initrd-start and linux,initrd-end set (the
 * initrd as two cells each: the phone's root has #address-cells = 2; a tree with 1 is not handled and is refused). Other
 * properties of /chosen stay. A tree with no /chosen gets one. Returns the new size, or 0.
 */
static uint32_t
cz_fdt_set_chosen(const uint8_t *in, uint32_t insize, uint8_t *out, uint32_t outmax, const char *bootargs,
    uint64_t initrd_start, uint64_t initrd_end)
{
	uint32_t total, so, ss, sizes, sizest, ver, i, end, depth = 0, in_chosen = 0, root_seen = 0;
	uint32_t o_args, o_s, o_e, strings_used;
	struct cz_out o = { out, 0, outmax, 0 };
	uint8_t cells[2][8];
	const uint8_t *st;

	if (insize < 40 || cz_be32(in) != FDT_MAGIC) {
		return 0;
	}
	total = cz_be32(in + 4), so = cz_be32(in + 8), ss = cz_be32(in + 12);
	ver = cz_be32(in + 20), sizes = cz_be32(in + 32), sizest = cz_be32(in + 36);
	if (ver < 17 || total > insize || so < 40 || so > ss || (uint64_t)so + sizest > ss ||
	    (uint64_t)ss + sizes > total) {
		return 0;
	}
	cz_emit(&o, in, so);                    /* header and the memory reservations */
	st = in + so;
	end = sizest;
	/* the names go on the end of the strings: the three, one after the other */
	o_args = sizes, o_s = o_args + 9, o_e = o_s + 19;
	strings_used = o_e + 17;
	for (i = 0; i < end;) {
		uint32_t tok = cz_be32(st + i), len, nameoff, nl;

		if (i + 4 > end) {
			return 0;
		}
		if (tok == FDT_BEGIN_NODE) {
			nl = cz_len((const char *)st + i + 4) + 1;
			if (i + 4 + cz_pad4(nl) > end) {
				return 0;
			}
			cz_emit(&o, st + i, 4 + cz_pad4(nl));
			if (depth == 0) {
				root_seen = 1;
			}
			if ((depth == 1 && cz_streq((const char *)st + i + 4, "chosen")) || depth == 0) {
				/* /chosen exists: ours go first inside it; none yet: put one in as the root's first child */
				int have_chosen = 0;
				uint32_t j = i + 4 + cz_pad4(nl), d = 1;

				if (depth == 0) {
					/* does the root have a chosen at all? a scan of its children */
					for (; j < end; ) {
						uint32_t t = cz_be32(st + j);

						if (t == FDT_BEGIN_NODE) {
							uint32_t l = cz_len((const char *)st + j + 4) + 1;

							if (d == 1 && cz_streq((const char *)st + j + 4, "chosen")) {
								have_chosen = 1;
							}
							d++, j += 4 + cz_pad4(l);
						} else if (t == FDT_END_NODE) {
							if (--d == 0) {
								break;
							}
							j += 4;
						} else if (t == FDT_PROP) {
							j += 12 + cz_pad4(cz_be32(st + j + 4));
						} else {
							j += 4;
						}
					}
					if (have_chosen) {
						depth++;
						i += 4 + cz_pad4(nl);
						continue;
					}
					cz_emit_u32(&o, FDT_BEGIN_NODE);
					cz_emit(&o, "chosen\0\0", 8);
				}
				cz_emit_prop(&o, o_args, bootargs, cz_len(bootargs) + 1);
				memset(cells, 0, sizeof(cells));
				cz_put_be32(cells[0], (uint32_t)(initrd_start >> 32)), cz_put_be32(cells[0] + 4, (uint32_t)initrd_start);
				cz_put_be32(cells[1], (uint32_t)(initrd_end >> 32)), cz_put_be32(cells[1] + 4, (uint32_t)initrd_end);
				cz_emit_prop(&o, o_s, cells[0], 8);
				cz_emit_prop(&o, o_e, cells[1], 8);
				if (depth == 0) {
					cz_emit_u32(&o, FDT_END_NODE);
				} else {
					in_chosen = 1;
				}
			}
			depth++;
			i += 4 + cz_pad4(nl);
		} else if (tok == FDT_END_NODE) {
			if (depth == 0) {
				return 0;
			}
			if (in_chosen && depth == 2) {
				in_chosen = 0;
			}
			depth--;
			cz_emit_u32(&o, FDT_END_NODE);
			i += 4;
		} else if (tok == FDT_PROP) {
			if (i + 12 > end) {
				return 0;
			}
			len = cz_be32(st + i + 4), nameoff = cz_be32(st + i + 8);
			if (len > end || i + 12 + cz_pad4(len) > end || nameoff >= sizes) {
				return 0;
			}
			if (!(in_chosen && depth == 2 && cz_ours((const char *)in + ss + nameoff))) {
				cz_emit(&o, st + i, 12 + cz_pad4(len));
			}
			i += 12 + cz_pad4(len);
		} else if (tok == FDT_NOP) {
			i += 4;
		} else if (tok == FDT_END) {
			break;
		} else {
			return 0;
		}
	}
	if (depth != 0 || !root_seen) {
		return 0;
	}
	cz_emit_u32(&o, FDT_END);
	/* the strings: the old ones, then the three names */
	{
		uint32_t struct_size = o.n - so;

		cz_emit(&o, in + ss, sizes);
		cz_emit(&o, "bootargs\0linux,initrd-start\0linux,initrd-end\0", 9 + 19 + 17);
		if (o.bad) {
			return 0;
		}
		cz_put_be32(out + 4, o.n);
		cz_put_be32(out + 12, so + struct_size);
		cz_put_be32(out + 32, strings_used);
		cz_put_be32(out + 36, struct_size);
	}
	return o.n;
}

#endif /* XNULOADER_CHOOSER_H */
