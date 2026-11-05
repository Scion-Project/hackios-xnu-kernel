/* QEMU's PCI host bridge as the device tree's /pci0, for bsd/dev/hakios.pci.c. */

/*                                                                          */
/* From the flattened device tree when the firmware publishes one (UEFI     */
/* configuration table), else QEMU virt's layout. Either way the kernel      */
/* gets plain little-endian values with absolute addresses:                  */
/*   compatible, ecam = <base size>, bus-range = <first last>,              */
/*   pci-ranges = { u32 space, u32 prefetch, u64 pci, u64 cpu, u64 size }..., */
/*   intx-map = { u32 phys.hi, u32 pin, u32 GIC INTID, u32 edge }...,         */
/*   intx-mask = <phys.hi mask, pin mask>, probe-only, firmware-devfn.       */
/* firmware-devfn names the PCI function this loader was started from (its */
/* ESP under QEMU): UEFI keeps using it after the kernel returns, so the     */
/* kernel's drivers leave it alone.                                          */

#define EFI_DTB_TABLE_GUID      { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } }
#define EFI_LOADED_IMAGE_GUID   { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define PCI_MAX_RANGES  4
#define PCI_MAX_INTX    64

struct pci_range_le {
	uint32_t space, prefetch;
	uint64_t pci, cpu, size;
};

struct pci_intx_le {
	uint32_t hi, pin, intid, edge;
};

static struct pci_host {
	int present;
	const char *compatible;
	uint64_t ecam[2];
	uint32_t bus_range[2];
	struct pci_range_le ranges[PCI_MAX_RANGES];
	uint32_t nranges;
	struct pci_intx_le intx[PCI_MAX_INTX];
	uint32_t nintx;
	uint32_t mask[2];
	uint32_t probe_only;
	uint32_t firmware_devfn[4];
	uint32_t nfirmware;
} PCI;

static int
guid_eq(const efi_guid *a, const efi_guid *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

static uint32_t
fdt32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t
fdt_cells(const uint8_t *p, uint32_t cells)
{
	uint64_t v = 0;
	for (uint32_t i = 0; i < cells; i++) {
		v = v << 32 | fdt32(p + 4 * i);
	}
	return v;
}

/* does a "compatible" list (len bytes of NUL-separated strings) contain want? */
static int
compat_has(const char *list, uint32_t len, const char *want)
{
	for (uint32_t at = 0; at < len;) {
		if (str_eq(list + at, want)) {
			return 1;
		}
		at += (uint32_t)str_len(list + at) + 1;
	}
	return 0;
}

#define FDT_MAX_CONTROLLERS 8

/*
 * The FDT, twice: first every interrupt controller (phandle -> #address-cells, #interrupt-cells),
 * then the host bridge and /chosen. Only what the bridge needs is understood.
 */
static int
pci_from_fdt(const uint8_t *fdt)
{
	struct { uint32_t phandle, addr_cells, intr_cells; } ctl[FDT_MAX_CONTROLLERS];
	uint32_t nctl = 0;

	if (fdt32(fdt) != 0xd00dfeed) {
		return 0;
	}
	const uint8_t *st = fdt + fdt32(fdt + 8);
	const char *strs = (const char *)fdt + fdt32(fdt + 12);
	uint32_t st_size = fdt32(fdt + 36);

	for (int pass = 0; pass < 2; pass++) {
		/* the node being read, depth by depth: only the innermost matters */
		uint32_t depth = 0, phandle = 0, addr_cells = 2, intr_cells = 0, is_ctl = 0, is_pci = 0, is_chosen = 0;
		const uint8_t *reg = 0, *ranges = 0, *imap = 0, *imask = 0, *busr = 0;
		uint32_t reg_len = 0, ranges_len = 0, imap_len = 0, imask_len = 0;
		const char *compat = 0;

		for (uint32_t at = 0; at + 4 <= st_size;) {
			uint32_t tok = fdt32(st + at);
			at += 4;
			if (tok == 1) {                 /* BEGIN_NODE name */
				const char *name = (const char *)st + at;
				at += ((uint32_t)str_len(name) + 1 + 3) & ~3u;
				depth++;
				phandle = 0, addr_cells = 2, intr_cells = 0, is_ctl = 0, is_pci = 0;
				is_chosen = depth == 2 && (str_eq(name, "chosen"));
				reg = ranges = imap = imask = busr = 0;
				compat = 0;
			} else if (tok == 3) {          /* PROP len nameoff value */
				uint32_t len = fdt32(st + at), nameoff = fdt32(st + at + 4);
				const uint8_t *v = st + at + 8;
				const char *pn = strs + nameoff;
				at += 8 + ((len + 3) & ~3u);
				if (str_eq(pn, "phandle") && len == 4) {
					phandle = fdt32(v);
				} else if (str_eq(pn, "#address-cells") && len == 4) {
					addr_cells = fdt32(v);
				} else if (str_eq(pn, "#interrupt-cells") && len == 4) {
					intr_cells = fdt32(v);
				} else if (str_eq(pn, "interrupt-controller")) {
					is_ctl = 1;
				} else if (str_eq(pn, "compatible")) {
					if (compat_has((const char *)v, len, "pci-host-ecam-generic")) {
						is_pci = 1, compat = "pci-host-ecam-generic";
					} else if (compat_has((const char *)v, len, "pci-host-cam-generic")) {
						is_pci = 1, compat = "pci-host-cam-generic";
					}
				} else if (str_eq(pn, "reg")) {
					reg = v, reg_len = len;
				} else if (str_eq(pn, "ranges")) {
					ranges = v, ranges_len = len;
				} else if (str_eq(pn, "interrupt-map")) {
					imap = v, imap_len = len;
				} else if (str_eq(pn, "interrupt-map-mask")) {
					imask = v, imask_len = len;
				} else if (str_eq(pn, "bus-range") && len == 8) {
					busr = v;
				} else if (pass == 1 && is_chosen && str_eq(pn, "linux,pci-probe-only") && len == 4) {
					PCI.probe_only = fdt32(v);
				}
			} else if (tok == 2) {          /* END_NODE: the node is complete */
				if (pass == 0 && is_ctl && phandle && nctl < FDT_MAX_CONTROLLERS) {
					ctl[nctl].phandle = phandle;
					ctl[nctl].addr_cells = addr_cells;
					ctl[nctl].intr_cells = intr_cells ? intr_cells : 3;
					nctl++;
				}
				if (pass == 1 && is_pci && !PCI.present && reg && reg_len >= 16) {
					/* the bridge sits under the root: 2 address and 2 size cells */
					PCI.present = 1;
					PCI.compatible = compat;
					PCI.ecam[0] = fdt_cells(reg, 2);
					PCI.ecam[1] = fdt_cells(reg + 8, 2);
					PCI.bus_range[0] = busr ? fdt32(busr) : 0;
					PCI.bus_range[1] = busr ? fdt32(busr + 4) : 0;
					/* ranges: child phys.hi phys.mid phys.lo, parent addr (2), size (2) */
					for (uint32_t o = 0; ranges && o + 28 <= ranges_len && PCI.nranges < PCI_MAX_RANGES; o += 28) {
						uint32_t hi = fdt32(ranges + o);
						struct pci_range_le *r = &PCI.ranges[PCI.nranges++];
						r->space = (hi >> 24) & 3;
						r->prefetch = (hi >> 30) & 1;
						r->pci = fdt_cells(ranges + o + 4, 2);
						r->cpu = fdt_cells(ranges + o + 12, 2);
						r->size = fdt_cells(ranges + o + 20, 2);
					}
					/* interrupt-map: phys.hi mid lo, pin, phandle, parent unit, parent specifier */
					for (uint32_t o = 0; imap && o + 20 <= imap_len && PCI.nintx < PCI_MAX_INTX;) {
						uint32_t ph = fdt32(imap + o + 16), pa = 2, pi = 3;
						for (uint32_t c = 0; c < nctl; c++) {
							if (ctl[c].phandle == ph) {
								pa = ctl[c].addr_cells, pi = ctl[c].intr_cells;
							}
						}
						uint32_t n = 5 + pa + pi;
						if (o + 4 * n > imap_len) {
							break;
						}
						const uint8_t *spec = imap + o + 4 * (5 + pa);
						if (pi >= 2) {
							struct pci_intx_le *e = &PCI.intx[PCI.nintx++];
							uint32_t type = fdt32(spec), num = fdt32(spec + 4);
							e->hi = fdt32(imap + o);
							e->pin = fdt32(imap + o + 12);
							e->intid = type == 1 ? 16 + num : 32 + num;
							e->edge = pi >= 3 ? (fdt32(spec + 8) & 3) != 0 : 0;
						}
						o += 4 * n;
					}
					PCI.mask[0] = imask && imask_len >= 16 ? fdt32(imask) : 0xffff00;
					PCI.mask[1] = imask && imask_len >= 16 ? fdt32(imask + 12) : 7;
				}
				depth--;
				is_chosen = 0;
			} else if (tok == 9) {          /* END */
				break;
			} else if (tok != 4) {          /* anything but NOP: give up */
				break;
			}
		}
	}
	return PCI.present;
}

/*
 * A GICv2 under the root of the FDT (QEMU virt with gic-version=2): its distributor and CPU interface, by the
 * root's #address-cells and #size-cells. 0: none, or a GICv3.
 */
static int
gic_v2_from_fdt(const uint8_t *fdt, uint64_t *dist, uint64_t *cpu)
{
	if (fdt32(fdt) != 0xd00dfeed) {
		return 0;
	}
	const uint8_t *st = fdt + fdt32(fdt + 8);
	const char *strs = (const char *)fdt + fdt32(fdt + 12);
	uint32_t st_size = fdt32(fdt + 36);
	uint32_t depth = 0, root_addr = 2, root_size = 1, is_v2 = 0, reg_len = 0;
	const uint8_t *reg = 0;

	for (uint32_t at = 0; at + 4 <= st_size;) {
		uint32_t tok = fdt32(st + at);
		at += 4;
		if (tok == 1) {                 /* BEGIN_NODE name */
			at += ((uint32_t)str_len((const char *)st + at) + 1 + 3) & ~3u;
			depth++;
			if (depth == 2) {       /* (a child of the GIC's node, QEMU's v2m@, must not forget the GIC's) */
				is_v2 = 0, reg = 0, reg_len = 0;
			}
		} else if (tok == 3) {          /* PROP len nameoff value */
			uint32_t len = fdt32(st + at), nameoff = fdt32(st + at + 4);
			const uint8_t *v = st + at + 8;
			const char *pn = strs + nameoff;
			at += 8 + ((len + 3) & ~3u);
			if (depth == 1 && str_eq(pn, "#address-cells") && len == 4) {
				root_addr = fdt32(v);
			} else if (depth == 1 && str_eq(pn, "#size-cells") && len == 4) {
				root_size = fdt32(v);
			} else if (depth == 2 && str_eq(pn, "compatible")) {
				is_v2 = compat_has((const char *)v, len, "arm,cortex-a15-gic") ||
				    compat_has((const char *)v, len, "arm,gic-400");
			} else if (depth == 2 && str_eq(pn, "reg")) {
				reg = v, reg_len = len;
			}
		} else if (tok == 2) {          /* END_NODE */
			uint32_t one = 4 * (root_addr + root_size);
			if (depth == 2 && is_v2 && reg != 0 && root_addr <= 2 && root_size <= 2 && reg_len >= 2 * one) {
				*dist = fdt_cells(reg, root_addr);
				*cpu = fdt_cells(reg + one, root_addr);
				return 1;
			}
			depth--;
		} else if (tok == 9) {          /* END */
			break;
		}
	}
	return 0;
}

/*
 * A platform with a .compatible (one whose framebuffer does not tell it apart: the Pixel 10's is wherever its boot loader
 * put it) found by the root node's "compatible" in the device tree the firmware publishes (UEFI configuration table;
 * socs/lga/shim publishes the boot loader's). `plat` if none matches.
 */
static const struct platform *
platform_from_fdt(const struct platform *plat)
{
	static const efi_guid dtb_guid = EFI_DTB_TABLE_GUID;

	for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables; i++) {
		const uint8_t *fdt = ST->tables[i].table;

		if (!guid_eq(&ST->tables[i].guid, &dtb_guid) || fdt == NULL || fdt32(fdt) != 0xd00dfeed) {
			continue;
		}
		const uint8_t *st = fdt + fdt32(fdt + 8);
		const char *strs = (const char *)fdt + fdt32(fdt + 12);
		uint32_t st_size = fdt32(fdt + 36), at = 0;

		/* the root node: BEGIN_NODE "", then its properties, up to its first child */
		while (at + 4 <= st_size && fdt32(st + at) == 4) {      /* NOP */
			at += 4;
		}
		if (at + 8 > st_size || fdt32(st + at) != 1) {
			continue;
		}
		at += 4;
		at += ((uint32_t)str_len((const char *)st + at) + 1 + 3) & ~3u;
		while (at + 12 <= st_size && (fdt32(st + at) == 3 || fdt32(st + at) == 4)) {
			if (fdt32(st + at) == 4) {
				at += 4;
				continue;
			}
			uint32_t len = fdt32(st + at + 4), nameoff = fdt32(st + at + 8);
			const char *v = (const char *)st + at + 12;

			if (str_eq(strs + nameoff, "compatible")) {
				for (unsigned p = 0; p < sizeof(platforms) / sizeof(platforms[0]); p++) {
					if (platforms[p]->compatible != NULL && compat_has(v, len, platforms[p]->compatible)) {
						return platforms[p];
					}
				}
				return plat;
			}
			at += 12 + ((len + 3) & ~3u);
		}
	}
	return plat;
}

/* QEMU virt (gic-version=3, highmem): what its FDT says, for firmware that publishes ACPI instead */
static void
pci_qemu_virt(void)
{
	PCI.present = 1;
	PCI.compatible = "pci-host-ecam-generic";
	PCI.ecam[0] = 0x4010000000ULL;
	PCI.ecam[1] = 0x10000000;
	PCI.bus_range[0] = 0;
	PCI.bus_range[1] = 0xff;
	PCI.ranges[0] = (struct pci_range_le){ 2, 0, 0x10000000, 0x10000000, 0x2eff0000 };
	PCI.ranges[1] = (struct pci_range_le){ 3, 1, 0x8000000000ULL, 0x8000000000ULL, 0x8000000000ULL };
	PCI.nranges = 2;
	/* INTA..INTD of slot s: SPI 3 + (s + pin - 1) % 4, level */
	for (uint32_t slot = 0; slot < 4; slot++) {
		for (uint32_t pin = 1; pin <= 4; pin++) {
			PCI.intx[PCI.nintx++] = (struct pci_intx_le){ slot << 11, pin, 32 + 3 + (slot + pin - 1) % 4, 0 };
		}
	}
	PCI.mask[0] = 0x1800;
	PCI.mask[1] = 7;
}

static uint64_t device_path_bytes(const efi_device_path *dp);

/* The PCI function the loader came from (its device path's PCI node on the root bus), if any. */
static void
pci_firmware_devices(void)
{
	static const efi_guid li_guid = EFI_LOADED_IMAGE_GUID, dp_guid = EFI_DEVICE_PATH_GUID;
	struct { uint32_t revision; efi_handle parent; void *st; efi_handle device; } *li;
	const uint8_t *p;

	if (EFI_ERROR(BS->handle_protocol(IMAGE, &li_guid, (void **)&li)) || li->device == NULL ||
	    EFI_ERROR(BS->handle_protocol(li->device, &dp_guid, (void **)&p))) {
		return;
	}
	for (uint64_t n = 0, total = device_path_bytes((const efi_device_path *)p), pcis = 0; n < total;) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;
		if (p[n] == 1 && p[n + 1] == 1 && len >= 6) {   /* hardware, PCI: function, device */
			if (pcis++ == 0 && PCI.nfirmware < 4) {
				PCI.firmware_devfn[PCI.nfirmware++] = (uint32_t)p[n + 5] << 3 | p[n + 4];
			} else {
				PCI.nfirmware = 0;      /* behind a bridge: bus unknown, say nothing */
			}
		}
		n += len;
	}
}

static void
pci_probe(const struct platform *plat)
{
	static const efi_guid dtb_guid = EFI_DTB_TABLE_GUID;
	const char *from = "none";

	for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables; i++) {
		if (guid_eq(&ST->tables[i].guid, &dtb_guid) && pci_from_fdt(ST->tables[i].table)) {
			from = "the firmware's device tree";
			break;
		}
	}
	if (!PCI.present && (plat == &platform_qemu_virt || plat == &platform_qemu_virt_gicv2)) {
		pci_qemu_virt();
		from = "QEMU virt's layout";
	}
	if (!PCI.present) {
		return;
	}
	pci_firmware_devices();
	print("pci: %s at %x size %x from %s, %d windows, %d INTx entries%s", PCI.compatible, PCI.ecam[0], PCI.ecam[1],
	    from, (uint64_t)PCI.nranges, (uint64_t)PCI.nintx, PCI.probe_only ? ", probe only" : "");
	for (uint32_t i = 0; i < PCI.nfirmware; i++) {
		print(", firmware's %x.%x", (uint64_t)(PCI.firmware_devfn[i] >> 3), (uint64_t)(PCI.firmware_devfn[i] & 7));
	}
	print("\n");
}

static void
dt_pci_node(struct dt_writer *w)
{
	if (!PCI.present) {
		return;
	}
	dt_begin(w, "pci0");
	dt_prop_str(w, "compatible", PCI.compatible);
	dt_prop(w, "ecam", PCI.ecam, sizeof(PCI.ecam));
	dt_prop(w, "bus-range", PCI.bus_range, sizeof(PCI.bus_range));
	dt_prop(w, "pci-ranges", PCI.ranges, PCI.nranges * sizeof(PCI.ranges[0]));
	dt_prop(w, "intx-map", PCI.intx, PCI.nintx * sizeof(PCI.intx[0]));
	dt_prop(w, "intx-mask", PCI.mask, sizeof(PCI.mask));
	dt_prop_u32(w, "probe-only", PCI.probe_only);
	if (PCI.nfirmware) {
		dt_prop(w, "firmware-devfn", PCI.firmware_devfn, PCI.nfirmware * sizeof(PCI.firmware_devfn[0]));
	}
	dt_end(w);
}
