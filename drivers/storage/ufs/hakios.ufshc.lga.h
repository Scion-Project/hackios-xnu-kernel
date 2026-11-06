/*
 * The Tensor G5's UFS host from nothing (ufs_native_cold=1, off by default): power, reset, reference clock, M-PHY firmware, link startup,
 * and hakios.ufshc.c takes over from NOP OUT. For a phone whose ABL leaves the host down; docs/pixel10-bringup.md has the order, each step's
 * source and its evidence. Facts only from Google's GPL-2.0 kernel, nothing copied: private/google-modules/soc/rdo in GrapheneOS's kernel_pixel,
 * 17-muzel (the flow the factory image's ufs.ko has, by its strings; 6f689be7fa23 is older and sets T_ACTIVATE first),
 * drivers/ufs/ufs-google.c (ufs_google_init, ufs_google_pd_notifier, ufs_google_phy_initialization, ufs_google_link_startup_notify),
 * drivers/ufs/lga/, power-controller/, drivers/rst/google/rst-cpm.c, drivers/clk/google/clk-cpm.c, drivers/mailbox/goog-mba-ctrl.c,
 * drivers/soc/google/goog-mba-cpm-iface.c, drivers/pinctrl/google/; private/devices/google/lga/dts/lga-base.dtsi.
 *
 * Writes to nothing but cold_allow[] and cold_host_allow[]; CPM mails are cold_mails[] and nothing else; the one SMC is the clock gate below.
 * No PMIC mail exists here: Google's kernel does not set these rails either (LDO 1S, 24S, bucks 11S, 13S are google,monitor-only), and
 * UFS_PWR_EN is the pad it drives. The driver stays .read_only; devices/frankel/audit-image.py checks these tables against its own copy.
 */

#define MBA_GLOBAL              0x05250000ULL   /* cpm_mba_syscon: read only here */
#define MBA_GLOBAL_REQ_WORDS    (MBA_GLOBAL + 0x10)
#define MBA_GLOBAL_RESP_WORDS   (MBA_GLOBAL + 0x14)
#define MBA_REQ                 0x05260000ULL   /* cpm_ap_ns_req: the AP's non-secure client; mba-ip-version 2.0: message words at 0x100 */
#define MBA_RESP                0x05261000ULL
#define MBA_TRIG                0x000
#define MBA_CONFIG              0x004
#define MBA_CONFIG_ON           ((1u << 8) | (1u << 16) | (1u << 24))   /* host auto ack, MSG and ACK interrupts: goog_mba_ctrl_startup's */
#define MBA_STATUS              0x008
#define MBA_STATUS_MSG          (1u << 0)
#define MBA_STATUS_ACK          (1u << 8)
#define MBA_OUTSTANDING         0x010
#define MBA_MSG                 0x100
#define MBA_SLOTS_MOST          16u             /* lga-mba.dtsi: 8 request and 4 response channels of 4 words */
#define MBA_TIMEOUT_MS          3000            /* power_controller_main.c, rst-cpm.h */
#define XPORT_INIT              0x100u          /* goog_mba_q_xport.h */
#define XPORT_HEADER(type, seq, src, dst) (1u << 31 | (uint32_t)(type) << 29 | (uint32_t)(seq) << 16 | (uint32_t)(src) << 8 | (dst))
#define XPORT_ONEWAY            0
#define XPORT_REQUEST           1
#define XPORT_RESPONSE          2
#define XPORT_SEQ_MOST          13
#define CPM_LPB                 5
#define CPM_LPCM                8
#define CPM_SRC_POWER           7               /* APC_COMMON_SERVICE_ID_POWER_CONTROLLER: where the LPB's completion comes back to */
#define LPB_HSIO_S              24              /* lpb_service.h */
#define LPB_ON                  3
#define LPB_CMD_OK              0xf
#define LPB_PWRUP_STARTED       0x12
#define LPB_NO_EFFECT           0x13
#define SIP_CLK_CONTROL         0x82000702u     /* clk-cpm.c: the clocks marked use-smc in lga-cpm-clk.dtsi */
#define CLK_LPCM_HSIO_S         6
#define CLK_REF_TCX0            3
#define CLK_AUX                 16

#define PSM_HSIOS               0x3C00B108ULL   /* lga-base.dtsi: read only; state in 4:0, 0x10 on, 0x11 power gated */
#define PSM_UFS_HC              0x3C00D210ULL
#define PSM_UFS_PHY             0x3C00E108ULL
#define TOP                     0x3C410000ULL   /* ufs_top */
#define TOP_ENC_STAT            0x008
#define TOP_ENC_KAT_DONE        (1u << 4)
#define TOP_PWR_CTRL            0x100
#define TOP_SRAM_BYPASS         (3u << 16)
#define TOP_BOOTLOAD_BYPASS     (3u << 18)
#define TOP_EXT_LD_DONE         (1u << 20)
#define TOP_INIT_DONE           (1u << 21)
#define TOP_REXT_CTRL           0x110
#define TOP_REXT_EN             (1u << 6)       /* Google: external mode, its TODO to read it from OTP; not read here */
#define TOP_AWUSER_VC           0x200
#define TOP_PHYCLK_SEL          0x208
#define TOP_CFG_CLKSEL          0x210
#define TOP_MPHY_CFG            0x214
#define TOP_SRAM_APB_MODE       0x510
#define PHY_SRAM                0x3C700000ULL
#define PHY_SRAM_SIZE           0x14000u
#define PAD_RESETB              0x3C590000ULL   /* hsios pinctrl, pad 0; a pad's registers each 0x1000 */
#define PAD_PWR_EN              0x3C598000ULL   /* pad 8 */
#define PAD_REFCLK              0x3C569000ULL   /* hsios_stby pinctrl, pad 1 */
#define PAD_PARAM               0x00
#define PAD_DMUX                0x04
#define PAD_TXDATA              0x08
#define PAD_TX_OE               (1u << 3)
#define PAD_TX_OUT              (1u << 2)
#define PAD_TX_PUPD             3u

#define UFSHC_BUSTHRTL          0xc0
#define UFSHC_LP_PG             (3u << 16)      /* LP_PGE, LP_AH8_PGE: the tree enables neither */
#define UFSHC_HCLKDIV           0xf8
#define UFSHC_HCLKDIV_VALUE     0xc8
#define UFSHC_IS_VENDOR         0xe0000000u     /* CIS, CRS, CTES */
#define UIC_DME_SET             0x02
#define UIC_DME_PEER_SET        0x04
#define UIC_TIMEOUT_MS          4000            /* UIC_COMMAND_POLL_TIMEOUT_US */

#define MIB(attr, sel)          ((uint32_t)(attr) << 16 | (sel))
#define PA_T_ACTIVATE           0x15a8
#define PHY_ULPH8               0x8118          /* CBULPH8: PHY power gating as the host's, off in the tree */
#define DME_CFG_UPD             0xd085
#define DME_MPHY_DISABLE        0xd0c1
#define TX_LANE(n)              (n)
#define RX_LANE(n)              (4 + (n))       /* PA_MAXDATALANES + lane: ufshci.h */
#define PHY_ADDR_LSB            0x810e
#define PHY_ADDR_MSB            0x810f
#define PHY_WDATA_LSB           0x8110
#define PHY_WDATA_MSB           0x8111
#define PHY_RDATA_LSB           0x8112
#define PHY_RDATA_MSB           0x8113
#define PHY_WRITESEL            0x8114
#define PHY_TX_FSM              0x41
#define PHY_RX_FSM              0xc1
#define PHY_READY_TIMEOUT_MS    1000
#define PHY_INIT_TIMEOUT_MS     570
#define PHY_FW_ENTRY            "ufsphyfw"      /* mkimage.sh -F: the payload's entry: this runs before there is a root to read a file from */
#define PAYLOAD_PA              0xF0000000ULL   /* xnuloader/loader.c */
#define PAYLOAD_MOST            0x04000000u
#define FDT_MOST                (4u << 20)
#define PHY_CAL_MOST            2048u

/* every physical range this code may write, by the word */
static const struct { uint64_t pa; uint32_t bytes; } cold_allow[] = {
	{ MBA_REQ + MBA_TRIG, 4 }, { MBA_REQ + MBA_CONFIG, 4 }, { MBA_REQ + MBA_STATUS, 4 },
	{ MBA_REQ + MBA_MSG, MBA_SLOTS_MOST * 16 },
	{ MBA_RESP + MBA_CONFIG, 4 }, { MBA_RESP + MBA_STATUS, 4 },
	{ TOP + TOP_PWR_CTRL, 4 }, { TOP + TOP_REXT_CTRL, 4 }, { TOP + TOP_AWUSER_VC, 4 }, { TOP + TOP_PHYCLK_SEL, 4 },
	{ TOP + TOP_CFG_CLKSEL, 4 }, { TOP + TOP_MPHY_CFG, 4 }, { TOP + TOP_SRAM_APB_MODE, 4 },
	{ PHY_SRAM, PHY_SRAM_SIZE },
	{ PAD_RESETB + PAD_DMUX, 4 }, { PAD_RESETB + PAD_TXDATA, 4 }, { PAD_PWR_EN + PAD_DMUX, 4 }, { PAD_PWR_EN + PAD_TXDATA, 4 },
	{ PAD_REFCLK + PAD_PARAM, 4 }, { PAD_REFCLK + PAD_DMUX, 4 }, { PAD_REFCLK + PAD_TXDATA, 4 },
};
static const uint32_t cold_host_allow[] = {
	UFSHC_HCE, UFSHC_IS, UFSHC_BUSTHRTL, UFSHC_HCLKDIV, UFSHC_UICCMD, UFSHC_UICCMDARG1, UFSHC_UICCMDARG2, UFSHC_UICCMDARG3,
};
/* every mail this code may send */
enum { MAIL_LPB_GET, MAIL_LPB_ON, MAIL_PREP_ON, MAIL_UFS_ON, MAIL_PHY_RST_OFF };
static const struct { uint8_t dst, src; uint32_t w[3]; const char *what; } cold_mails[] = {
	[MAIL_LPB_GET] = { CPM_LPB, CPM_SRC_POWER, { 3, LPB_HSIO_S, 0 }, "state of hsio_s" },
	[MAIL_LPB_ON] = { CPM_LPB, CPM_SRC_POWER, { 2, LPB_HSIO_S, 1 }, "hsio_s on" },
	/* REQ_ID 1 (GEN_PD), lpcm sswrp 1 (HSIO_S), subdomain 3 (UFS_PREP) or 0 (UFS), on */
	[MAIL_PREP_ON] = { CPM_LPCM, CPM_SRC_POWER, { 0x01030101, 0, 0 }, "hsio_s_ufs_prep on" },
	[MAIL_UFS_ON] = { CPM_LPCM, CPM_SRC_POWER, { 0x01000101, 0, 0 }, "hsio_s_ufs on" },
	/* req 4 (SET_RST), lpcm 6 (HSIO_S), reset 3 (UFS_PHY), op 1 (deassert) */
	[MAIL_PHY_RST_OFF] = { CPM_LPCM, 0, { 0x01030604, 0, 0 }, "UFS PHY out of reset" },
};
/* the blocks that are read or written, mapped once */
static struct { uint64_t pa; uint32_t bytes; vm_offset_t va; } cold_maps[] = {
	{ MBA_GLOBAL, 0x1000, 0 }, { MBA_REQ, 0x2000, 0 }, { PSM_HSIOS & ~0xfffULL, 0x1000, 0 }, { PSM_UFS_HC & ~0xfffULL, 0x2000, 0 },
	{ TOP, 0x1000, 0 }, { PHY_SRAM, PHY_SRAM_SIZE, 0 },
	{ PAD_RESETB, 0x9000, 0 }, { PAD_REFCLK, 0x1000, 0 },
};
static bool cold_refused;
static uint32_t cold_tx, cold_rx, cold_nreq, cold_nresp, cold_seq;
static enum { COLD_NOTHING, COLD_MAILBOX, COLD_HSIO, COLD_HOST } cold_up;
static const uint8_t *cold_fw;
static uint32_t cold_fw_size;
static const uint8_t *cold_cal;
static uint32_t cold_cal_n;

static volatile uint32_t *
cold_reg(uint64_t pa)
{
	for (unsigned i = 0; i < sizeof(cold_maps) / sizeof(cold_maps[0]); i++) {
		if (pa >= cold_maps[i].pa && pa < cold_maps[i].pa + cold_maps[i].bytes) {
			return (volatile uint32_t *)(cold_maps[i].va + (vm_offset_t)(pa - cold_maps[i].pa));
		}
	}
	return NULL;
}

static uint32_t
cold_rd(uint64_t pa)
{
	volatile uint32_t *r = cold_reg(pa);

	if (r == NULL) {
		cold_refused = true;
		return 0;
	}
	return *r;
}

/* the one store of this file to anything but the host's registers */
static void
cold_wr(uint64_t pa, uint32_t value)
{
	for (unsigned i = 0; i < sizeof(cold_allow) / sizeof(cold_allow[0]); i++) {
		if (pa >= cold_allow[i].pa && pa + 4 <= cold_allow[i].pa + cold_allow[i].bytes && (pa & 3) == 0) {
			volatile uint32_t *r = cold_reg(pa);

			if (r != NULL) {
				__asm__ volatile ("dsb sy" ::: "memory");
				*r = value;
				__asm__ volatile ("dsb sy" ::: "memory");
				return;
			}
		}
	}
	cold_refused = true;
	hc_say("ufs: cold: REFUSED a write of %08x to %llx: not on the list\n", value, pa);
}

static void
cold_host(uint32_t off, uint32_t value)
{
	for (unsigned i = 0; i < sizeof(cold_host_allow) / sizeof(cold_host_allow[0]); i++) {
		if (off == cold_host_allow[i]) {
			hc_write(off, value);
			return;
		}
	}
	cold_refused = true;
	hc_say("ufs: cold: REFUSED a write of %08x to the host's %x: not on the list\n", value, off);
}

static bool
cold_poll(uint64_t pa, uint32_t mask, uint32_t want, uint32_t ms)
{
	for (uint64_t t = mach_absolute_time(); hc_us_since(t) < ms * 1000ULL && !cold_refused;) {
		if ((cold_rd(pa) & mask) == want) {
			return true;
		}
		delay(10);
	}
	return (cold_rd(pa) & mask) == want;
}

/* what ufs: cold: says when it gives up: nothing is read that the step reached has not powered */
static void
cold_dump(const char *why)
{
	hc_say("ufs: cold: GAVE UP: %s\n", why);
	if (hc_soc->pa != 0 && cold_up >= COLD_MAILBOX) {
		hc_say("ufs: cold: dump: mailbox: request config %08x status %08x outstanding %08x; response config %08x status %08x; tx %u rx %u\n",
		    cold_rd(MBA_REQ + MBA_CONFIG), cold_rd(MBA_REQ + MBA_STATUS), cold_rd(MBA_REQ + MBA_OUTSTANDING),
		    cold_rd(MBA_RESP + MBA_CONFIG), cold_rd(MBA_RESP + MBA_STATUS), cold_tx, cold_rx);
	}
	if (hc_soc->pa != 0 && cold_up >= COLD_HSIO) {
		hc_say("ufs: cold: dump: PSM hsio_s %08x host %08x PHY %08x\n", cold_rd(PSM_HSIOS), cold_rd(PSM_UFS_HC), cold_rd(PSM_UFS_PHY));
		hc_say("ufs: cold: dump: top: ENC_STAT %08x PWR_CTRL %08x REXT %08x AWUSER_VC %08x PHYCLK_SEL %08x CFG_CLKSEL %08x MPHY_CFG %08x APB %08x\n",
		    cold_rd(TOP + TOP_ENC_STAT), cold_rd(TOP + TOP_PWR_CTRL), cold_rd(TOP + TOP_REXT_CTRL), cold_rd(TOP + TOP_AWUSER_VC),
		    cold_rd(TOP + TOP_PHYCLK_SEL), cold_rd(TOP + TOP_CFG_CLKSEL), cold_rd(TOP + TOP_MPHY_CFG), cold_rd(TOP + TOP_SRAM_APB_MODE));
		hc_say("ufs: cold: dump: pads: RESETB %08x %08x %08x PWR_EN %08x %08x %08x REFCLK %08x %08x %08x\n",
		    cold_rd(PAD_RESETB + PAD_PARAM), cold_rd(PAD_RESETB + PAD_DMUX), cold_rd(PAD_RESETB + PAD_TXDATA),
		    cold_rd(PAD_PWR_EN + PAD_PARAM), cold_rd(PAD_PWR_EN + PAD_DMUX), cold_rd(PAD_PWR_EN + PAD_TXDATA),
		    cold_rd(PAD_REFCLK + PAD_PARAM), cold_rd(PAD_REFCLK + PAD_DMUX), cold_rd(PAD_REFCLK + PAD_TXDATA));
	}
	if (cold_up >= COLD_HOST) {
		hc_say("ufs: cold: dump: host: HCE %08x HCS %08x IS %08x IE %08x UICCMD %08x ARG2 %08x ARG3 %08x HCLKDIV %08x BUSTHRTL %08x\n",
		    hc_read(UFSHC_HCE), hc_read(UFSHC_HCS), hc_read(UFSHC_IS), hc_read(UFSHC_IE), hc_read(UFSHC_UICCMD),
		    hc_read(UFSHC_UICCMDARG2), hc_read(UFSHC_UICCMDARG3), hc_read(UFSHC_HCLKDIV), hc_read(UFSHC_BUSTHRTL));
		cold_host(UFSHC_HCE, 0);
	}
}

/* ---- the CPM's mailbox, polled: its interrupts are not wired up here ---- */

static const char *
cold_mba_start(void)
{
	uint32_t req = cold_rd(MBA_GLOBAL_REQ_WORDS), resp = cold_rd(MBA_GLOBAL_RESP_WORDS);

	hc_say("ufs: cold: mailbox: %u request words, %u response words; request status %08x outstanding %08x\n", req, resp,
	    cold_rd(MBA_REQ + MBA_STATUS), cold_rd(MBA_REQ + MBA_OUTSTANDING));
	if (req == 0 || resp == 0 || (req % 4) != 0 || (resp % 4) != 0 || req / 4 > MBA_SLOTS_MOST || resp / 4 > MBA_SLOTS_MOST) {
		return "the mailbox is not laid out as lga-mba.dtsi says";
	}
	cold_nreq = req / 4;
	cold_nresp = resp / 4;
	cold_up = COLD_MAILBOX;
	cold_wr(MBA_REQ + MBA_STATUS, MBA_STATUS_MSG | MBA_STATUS_ACK);
	cold_wr(MBA_RESP + MBA_STATUS, MBA_STATUS_MSG | MBA_STATUS_ACK);
	cold_wr(MBA_REQ + MBA_CONFIG, cold_rd(MBA_REQ + MBA_CONFIG) | MBA_CONFIG_ON);
	cold_wr(MBA_RESP + MBA_CONFIG, cold_rd(MBA_RESP + MBA_CONFIG) | MBA_CONFIG_ON);
	/* the queue's reset, as cpm_iface_init_q_mode at Linux's boot: slot 0, whatever ABL's left; both sides go back to slot 0 */
	cold_wr(MBA_REQ + MBA_MSG, XPORT_INIT);
	cold_wr(MBA_REQ + MBA_MSG + 4, 0);
	cold_wr(MBA_REQ + MBA_MSG + 8, 0);
	cold_wr(MBA_REQ + MBA_MSG + 12, 0);
	cold_wr(MBA_REQ + MBA_TRIG, 1);
	if (!cold_poll(MBA_REQ + MBA_OUTSTANDING, ~0u, 0, MBA_TIMEOUT_MS)) {
		return "the CPM did not take the mailbox's reset";
	}
	cold_wr(MBA_REQ + MBA_STATUS, MBA_STATUS_ACK);
	cold_tx = cold_rx = 0;
	cold_seq = 1;
	return NULL;
}

/* one message from the CPM's side, or false after ms */
static bool
cold_mba_next(uint32_t msg[4], uint32_t ms)
{
	if (!cold_poll(MBA_RESP + MBA_STATUS, MBA_STATUS_MSG, MBA_STATUS_MSG, ms)) {
		return false;
	}
	for (uint32_t i = 0; i < 4; i++) {
		msg[i] = cold_rd(MBA_RESP + MBA_MSG + cold_rx * 16 + i * 4);
	}
	cold_rx = (cold_rx + 1) % cold_nresp;
	cold_wr(MBA_RESP + MBA_STATUS, MBA_STATUS_MSG);
	return true;
}

static const char *
cold_mail(unsigned which, uint32_t resp[3])
{
	uint32_t off = MBA_MSG + cold_tx * 16, msg[4];

	cold_wr(MBA_REQ + off, XPORT_HEADER(XPORT_REQUEST, cold_seq, cold_mails[which].src, cold_mails[which].dst));
	for (uint32_t i = 0; i < 3; i++) {
		cold_wr(MBA_REQ + off + 4 + i * 4, cold_mails[which].w[i]);
	}
	cold_wr(MBA_REQ + MBA_TRIG, 1);
	cold_tx = (cold_tx + 1) % cold_nreq;
	cold_seq = cold_seq == XPORT_SEQ_MOST ? 1 : cold_seq + 1;
	if (!cold_poll(MBA_REQ + MBA_OUTSTANDING, ~0u, 0, MBA_TIMEOUT_MS)) {
		return "the CPM did not take a mail";
	}
	cold_wr(MBA_REQ + MBA_STATUS, MBA_STATUS_ACK);
	for (;;) {
		if (!cold_mba_next(msg, MBA_TIMEOUT_MS)) {
			return "no answer from the CPM";
		}
		if (msg[0] != XPORT_INIT && ((msg[0] >> 29) & 3) == XPORT_RESPONSE) {
			break;
		}
		hc_say("ufs: cold: mailbox: from the CPM, not an answer: %08x %08x %08x %08x\n", msg[0], msg[1], msg[2], msg[3]);
	}
	hc_say("ufs: cold: CPM: %s: %08x %08x %08x\n", cold_mails[which].what, msg[1], msg[2], msg[3]);
	memcpy(resp, &msg[1], 3 * sizeof(uint32_t));
	return NULL;
}

/* lga-cpm-clk.dtsi: use-smc. The same call Google's kernel makes: on only, and only these two gates */
static const char *
cold_clock_on(uint32_t clock)
{
	register uint64_t x0 __asm__("x0") = SIP_CLK_CONTROL;
	register uint64_t x1 __asm__("x1") = CLK_LPCM_HSIO_S;
	register uint64_t x2 __asm__("x2") = clock == CLK_AUX ? CLK_AUX : CLK_REF_TCX0;
	register uint64_t x3 __asm__("x3") = 1;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
		      "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	hc_say("ufs: cold: clock %u on (EL3's SIP call): a0 %llx a1 %llx\n", clock, x0, x1);
	return x0 == 0 && x1 == 0 ? NULL : "EL3 refused the clock";
}

/* a pad as its function in lga-pinctrl.dtsi: mux bit, driven, no pull; TXDATA says the rest */
static void
cold_pad(uint64_t pad, uint32_t dmux, bool out)
{
	uint32_t tx = (cold_rd(pad + PAD_TXDATA) & ~(PAD_TX_PUPD | PAD_TX_OUT)) | PAD_TX_OE | (out ? PAD_TX_OUT : 0);

	if (cold_rd(pad + PAD_DMUX) != dmux) {
		cold_wr(pad + PAD_DMUX, dmux);
	}
	cold_wr(pad + PAD_TXDATA, tx);
}

/* ---- power, up to the device out of reset: ufs_google_init, then the notifier's PRE_ON and ON ---- */

static const char *
lga_cold_power(bool *phy_up)
{
	uint32_t r[3], pwr;
	const char *no;

	for (unsigned i = 0; i < sizeof(cold_maps) / sizeof(cold_maps[0]); i++) {
		cold_maps[i].va = ml_io_map(cold_maps[i].pa, cold_maps[i].bytes);
		if (cold_maps[i].va == 0) {
			return "a block could not be mapped";
		}
	}
	hc_say("ufs: cold: 1 the CPM's mailbox (%llx)\n", MBA_REQ);
	if ((no = cold_mba_start()) != NULL || (no = cold_mail(MAIL_LPB_GET, r)) != NULL) {
		return no;
	}
	if (r[1] != LPB_ON) {
		uint32_t msg[4] = { 0 };
		bool on = false;

		if ((no = cold_mail(MAIL_LPB_ON, r)) != NULL) {
			return no;
		}
		if (r[0] != LPB_CMD_OK || (r[1] != LPB_CMD_OK && r[1] != LPB_PWRUP_STARTED && r[1] != LPB_NO_EFFECT)) {
			return "the CPM refused to power hsio_s";
		}
		on = r[1] != LPB_PWRUP_STARTED;
		while (!on && cold_mba_next(msg, MBA_TIMEOUT_MS)) {
			hc_say("ufs: cold: mailbox: %08x %08x %08x %08x\n", msg[0], msg[1], msg[2], msg[3]);
			on = ((msg[0] >> 29) & 3) == XPORT_ONEWAY && msg[1] == LPB_HSIO_S && msg[2] == 0;
		}
		if (!on) {
			return "hsio_s did not come on";
		}
	}
	/* on-at-init in the tree: at Linux's boot these are assumed; a request where it is on already has no effect (ufs_google_init's comment) */
	hc_say("ufs: cold: 2 power domains\n");
	if ((no = cold_mail(MAIL_PREP_ON, r)) != NULL) {
		return no;
	}
	if (r[0] != 0) {
		return "the CPM refused hsio_s_ufs_prep";
	}
	cold_up = COLD_HSIO;
	pwr = cold_rd(TOP + TOP_PWR_CTRL);
	*phy_up = (pwr & (TOP_EXT_LD_DONE | TOP_INIT_DONE)) == (TOP_EXT_LD_DONE | TOP_INIT_DONE);
	hc_say("ufs: cold: 3 PSM hsio_s %08x host %08x PHY %08x; PWR_CTRL %08x: %s; pads' TXDATA: RESETB %08x PWR_EN %08x REFCLK %08x\n",
	    cold_rd(PSM_HSIOS), cold_rd(PSM_UFS_HC), cold_rd(PSM_UFS_PHY), pwr, *phy_up ?
	    "ABL's firmware is in and the PHY started: as Linux at its boot, no PHY init" :
	    "the PHY is cold: the firmware and the calibration go in", cold_rd(PAD_RESETB + PAD_TXDATA), cold_rd(PAD_PWR_EN + PAD_TXDATA),
	    cold_rd(PAD_REFCLK + PAD_TXDATA));
	if (!*phy_up) {
		/* PRE_ON: while the PSM is in PREP: mode 2 (google,phy-patching-mode), SRAM not bypassed, boot load bypassed */
		cold_wr(TOP + TOP_PWR_CTRL, (pwr & ~TOP_SRAM_BYPASS) | TOP_BOOTLOAD_BYPASS);
		cold_wr(TOP + TOP_PHYCLK_SEL, cold_rd(TOP + TOP_PHYCLK_SEL) | 2);
		cold_wr(TOP + TOP_MPHY_CFG, cold_rd(TOP + TOP_MPHY_CFG) | 1);
		cold_wr(TOP + TOP_CFG_CLKSEL, cold_rd(TOP + TOP_CFG_CLKSEL) | 2);
		cold_wr(TOP + TOP_REXT_CTRL, cold_rd(TOP + TOP_REXT_CTRL) & ~TOP_REXT_EN);
	}
	if ((no = cold_mail(MAIL_UFS_ON, r)) != NULL) {
		return no;
	}
	if (r[0] != 0) {
		return "the CPM refused hsio_s_ufs";
	}
	cold_up = COLD_HOST;
	if (!cold_poll(TOP + TOP_ENC_STAT, TOP_ENC_KAT_DONE, TOP_ENC_KAT_DONE, 1)) {
		return "the host's crypto self-test (FIPS 140 KAT) did not finish";
	}
	hc_say("ufs: cold: 4 ufs_pd on, the crypto self-test done, ENC_STAT %08x; device reset, VCC and VCCQ's enable, reference clock\n",
	    cold_rd(TOP + TOP_ENC_STAT));
	/* refclk_on: drive-strength 1; the sequence of the notifier's ON, with one difference: UFS_PWR_EN only ever goes high */
	cold_wr(PAD_REFCLK + PAD_PARAM, (cold_rd(PAD_REFCLK + PAD_PARAM) & ~7u) | 1);
	cold_pad(PAD_REFCLK, 1u << 1, false);
	cold_pad(PAD_RESETB, 1u << 0, false);
	cold_pad(PAD_PWR_EN, 1u << 0, true);
	delay(2000);
	if ((no = cold_clock_on(CLK_REF_TCX0)) != NULL) {
		return no;
	}
	delay(310);
	cold_pad(PAD_RESETB, 1u << 0, true);
	cold_host(UFSHC_IS, hc_read(UFSHC_IS) & UFSHC_IS_VENDOR);
	return cold_refused ? "a write off the list" : NULL;
}

/* ---- the host: enabled, the PHY started, the link: the core's hba enable and ufs_google_link_startup_notify ---- */

static const char *
cold_uic(uint32_t cmd, uint32_t sel, uint32_t value, uint32_t *out)
{
	uint32_t result;

	if (!hc_poll(UFSHC_HCS, HCS_UCRDY, HCS_UCRDY, UIC_TIMEOUT_MS)) {
		return "the host is not ready for a UIC command";
	}
	cold_host(UFSHC_IS, IS_UCCS);
	cold_host(UFSHC_UICCMDARG1, sel);
	cold_host(UFSHC_UICCMDARG2, 0);
	cold_host(UFSHC_UICCMDARG3, value);
	cold_host(UFSHC_UICCMD, cmd);
	if (!hc_poll(UFSHC_IS, IS_UCCS, IS_UCCS, UIC_TIMEOUT_MS)) {
		return "no completion of a UIC command";
	}
	result = hc_read(UFSHC_UICCMDARG2) & 0xff;
	if (out != NULL) {
		*out = hc_read(UFSHC_UICCMDARG3);
	}
	cold_host(UFSHC_IS, IS_UCCS);
	/* QEMU's host takes a link startup and no attribute: the sequence is logged there and goes on */
	if (result != 0 && hc_soc->pa == 0) {
		return NULL;
	}
	if (result != 0) {
		hc_say("ufs: cold: UIC %02x of %08x (%x): result %u\n", cmd, sel, value, result);
		return "a UIC command failed";
	}
	return NULL;
}

/* a list of (attribute, value) local sets */
static const char *
cold_dme_seq(const uint32_t (*seq)[2], unsigned n)
{
	const char *no = NULL;

	for (unsigned i = 0; i < n && no == NULL; i++) {
		no = cold_uic(UIC_DME_SET, seq[i][0], seq[i][1], NULL);
	}
	return no;
}

static const char *
cold_phy_write(uint32_t addr, uint32_t data)
{
	const uint32_t seq[][2] = {
		{ MIB(PHY_ADDR_LSB, 0), addr & 0xff }, { MIB(PHY_ADDR_MSB, 0), addr >> 8 }, { MIB(PHY_WDATA_LSB, 0), data & 0xff },
		{ MIB(PHY_WDATA_MSB, 0), data >> 8 }, { MIB(PHY_WRITESEL, 0), 1 },
	};

	return cold_dme_seq(seq, sizeof(seq) / sizeof(seq[0]));
}

static const char *
cold_phy_read(uint32_t addr, uint32_t *data)
{
	const uint32_t seq[][2] = { { MIB(PHY_ADDR_LSB, 0), addr & 0xff }, { MIB(PHY_ADDR_MSB, 0), addr >> 8 }, { MIB(PHY_WRITESEL, 0), 0 } };
	uint32_t lo = 0, hi = 0;
	const char *no = cold_dme_seq(seq, sizeof(seq) / sizeof(seq[0]));

	if (no == NULL) {
		no = cold_uic(UIC_DME_GET, MIB(PHY_RDATA_LSB, 0), 0, &lo);
	}
	if (no == NULL) {
		no = cold_uic(UIC_DME_GET, MIB(PHY_RDATA_MSB, 0), 0, &hi);
	}
	*data = (lo & 0xff) | (hi & 0xff) << 8;
	return no;
}

/* phy-cal-data (u32 addr in the low half, data in the high, big-endian in the tree), then ufs_google_set_done_bits's: rate B's */
static const char *
cold_phy_calibrate(void)
{
	static const uint32_t done[][2] = { { 0x3128, 0xc }, { 0x3328, 0xc }, { 0x3101, 0x2 }, { 0x3301, 0x2 }, { 0x3103, 0x60 }, { 0x3303, 0x60 } };
	const char *no = NULL;

	if (cold_cal == NULL) {
		hc_say("ufs: cold: no phy-cal-data in ABL's tree: no calibration\n");
		return NULL;
	}
	for (uint32_t i = 0; i < cold_cal_n && no == NULL; i++) {
		const uint8_t *c = cold_cal + i * 4;

		no = cold_phy_write((uint32_t)c[2] << 8 | c[3], (uint32_t)c[0] << 8 | c[1]);
	}
	if (no == NULL) {
		no = cold_phy_write(0x017c, 0x2);       /* RAWCMN_DIG_AON_CMNCAL_MPLL_STATUS */
	}
	for (unsigned i = 0; i < sizeof(done) / sizeof(done[0]) && no == NULL; i++) {
		uint32_t v = 0;

		no = cold_phy_read(done[i][0], &v);
		if (no == NULL) {
			no = cold_phy_write(done[i][0], v | done[i][1]);
		}
	}
	hc_say("ufs: cold: calibration: %u values and the done bits: %s\n", cold_cal_n, no == NULL ? "written" : no);
	return no;
}

static void
cold_phy_firmware(void)
{
	cold_wr(TOP + TOP_SRAM_APB_MODE, (cold_rd(TOP + TOP_SRAM_APB_MODE) & ~1u) | 1);
	delay(1);
	for (uint32_t i = 0; i < cold_fw_size / 4; i++) {
		uint32_t w;

		memcpy(&w, cold_fw + i * 4, 4);
		cold_wr(PHY_SRAM + i * 4ULL, w);
	}
	cold_wr(TOP + TOP_SRAM_APB_MODE, cold_rd(TOP + TOP_SRAM_APB_MODE) & ~1u);
	delay(1);
	cold_wr(TOP + TOP_PWR_CTRL, cold_rd(TOP + TOP_PWR_CTRL) | TOP_EXT_LD_DONE);
	hc_say("ufs: cold: M-PHY firmware: %u bytes into its SRAM, external load done; PWR_CTRL %08x\n", cold_fw_size, cold_rd(TOP + TOP_PWR_CTRL));
}

/* ufs_lga_plat_poll_phy_ready: both TX lanes' FSM reads 1, both RX lanes' has a bit of 2:0 */
static const char *
cold_phy_ready(void)
{
	static const uint32_t what[4][3] = { { PHY_TX_FSM, TX_LANE(0), 0 }, { PHY_TX_FSM, TX_LANE(1), 0 }, { PHY_RX_FSM, RX_LANE(0), 1 }, { PHY_RX_FSM, RX_LANE(1), 1 } };

	for (unsigned i = 0; i < 4; i++) {
		uint32_t v = 0;
		const char *no = NULL;
		uint64_t t = mach_absolute_time();
		bool ready = false;

		while (!ready && hc_us_since(t) < PHY_READY_TIMEOUT_MS * 1000ULL) {
			if ((no = cold_uic(UIC_DME_GET, MIB(what[i][0], what[i][1]), 0, &v)) != NULL) {
				return no;
			}
			ready = what[i][2] ? (v & 7) != 0 : v == 1;
			if (!ready) {
				delay(2000);
			}
		}
		if (!ready) {
			hc_say("ufs: cold: M-PHY FSM %x sel %u: %x\n", what[i][0], what[i][1], v);
			return "the M-PHY did not come ready";
		}
	}
	return NULL;
}

static const char *
lga_cold_host(bool phy_up)
{
	static const uint32_t ulph8[][2] = { { MIB(PHY_ULPH8, 0), 0 }, { MIB(DME_CFG_UPD, 0), 1 } };
	static const uint32_t rmmi[][2] = {
		{ MIB(0x8101, 0), 0x5 }, { MIB(0x8115, 0), 0x1 },       /* CBMPHYBOOTCFG, CBATTR2APBCTRL */
		{ MIB(0x8019, RX_LANE(0)), 0x15 }, { MIB(0x8019, RX_LANE(1)), 0x15 },   /* RX_CDR_ACTIVE_LATENCY_ADJUST */
		{ MIB(0x8013, RX_LANE(0)), 0x1 }, { MIB(0x8013, RX_LANE(1)), 0x1 },     /* RX_ASYNC_FILTER */
	};
	static const uint32_t tx_eq[][2] = {
		{ MIB(0x8016, TX_LANE(0)), 0 }, { MIB(0x8016, TX_LANE(1)), 0 }, { MIB(DME_CFG_UPD, 0), 1 },     /* TX_EQ_PRE_DE: 0 before the PHY's reset goes */
	};
	static const uint32_t cport[][3] = {
		{ UIC_DME_SET, 0x4020, 0 }, { UIC_DME_SET, 0x3000, 0 }, { UIC_DME_SET, 0x3001, 1 }, { UIC_DME_SET, 0x4021, 1 },
		{ UIC_DME_SET, 0x4022, 0 }, { UIC_DME_SET, 0x4023, 0 }, { UIC_DME_SET, 0x4025, 6 }, { UIC_DME_SET, 0x402b, 1 },
		{ UIC_DME_SET, 0x4020, 1 },
		{ UIC_DME_PEER_SET, 0x4020, 0 }, { UIC_DME_PEER_SET, 0x3000, 1 }, { UIC_DME_PEER_SET, 0x3001, 1 }, { UIC_DME_PEER_SET, 0x4021, 0 },
		{ UIC_DME_PEER_SET, 0x4022, 0 }, { UIC_DME_PEER_SET, 0x4023, 0 }, { UIC_DME_PEER_SET, 0x4025, 6 }, { UIC_DME_PEER_SET, 0x402b, 1 },
		{ UIC_DME_PEER_SET, 0x4020, 1 },
	};
	bool real = hc_soc->pa != 0, up = false;
	uint32_t r[3], t = 0;
	const char *no;

	cold_up = COLD_HOST;
	cold_host(UFSHC_HCE, 0);
	(void)hc_poll(UFSHC_HCE, 1, 0, 10);
	cold_host(UFSHC_HCE, 1);
	if (!hc_poll(UFSHC_HCE, 1, 1, 100)) {
		return "HCE did not read 1";
	}
	hc_say("ufs: cold: 5 host enabled: CAP %08x VER %08x HCS %08x\n", hc_read(UFSHC_CAP), hc_read(UFSHC_VER), hc_read(UFSHC_HCS));
	cold_host(UFSHC_HCLKDIV, UFSHC_HCLKDIV_VALUE);
	if (real) {
		cold_wr(TOP + TOP_AWUSER_VC, 1 | 1u << 8);
	}
	if ((no = cold_dme_seq(ulph8, sizeof(ulph8) / sizeof(ulph8[0]))) != NULL) {
		return no;
	}
	if (phy_up) {
		hc_say("ufs: cold: 6 the PHY left as it is\n");
	} else {
		if ((no = cold_dme_seq(rmmi, sizeof(rmmi) / sizeof(rmmi[0]))) != NULL ||
		    (no = cold_dme_seq(tx_eq, sizeof(tx_eq) / sizeof(tx_eq[0]))) != NULL) {
			return no;
		}
		hc_say("ufs: cold: 6 PHY attributes set; the PHY out of reset\n");
		if (real) {
			if ((no = cold_mail(MAIL_PHY_RST_OFF, r)) != NULL) {
				return no;
			}
			if (r[0] != 0) {
				return "the CPM refused the PHY's reset";
			}
			if (!cold_poll(TOP + TOP_PWR_CTRL, TOP_INIT_DONE, TOP_INIT_DONE, PHY_INIT_TIMEOUT_MS)) {
				return "the PHY's SRAM did not say its init is done";
			}
		}
		if ((no = cold_phy_calibrate()) != NULL) {
			return no;
		}
		if (real) {
			cold_phy_firmware();
		}
		if ((no = cold_uic(UIC_DME_SET, MIB(DME_MPHY_DISABLE, 0), 0, NULL)) != NULL) {
			return no;
		}
		if (real && (no = cold_phy_ready()) != NULL) {
			return no;
		}
		hc_say("ufs: cold: 7 M-PHY enabled and ready\n");
	}
	hc_say("ufs: cold: link startup\n");
	for (int tries = 1; tries <= 3 && !up; tries++) {
		no = cold_uic(UIC_LINK_STARTUP, 0, 0, NULL);
		up = no == NULL && hc_poll(UFSHC_HCS, HCS_DP, HCS_DP, 500);
		hc_say("ufs: cold: DME_LINKSTARTUP %d: %s, HCS %08x IS %08x\n", tries, up ? "a device is there" : no != NULL ? no : "no device",
		    hc_read(UFSHC_HCS), hc_read(UFSHC_IS));
	}
	if (!up) {
		return "the link did not start";
	}
	/* Synopsys's T_ACTIVATE + 1 (Google's comment), the C-port, power gating off */
	if ((no = cold_uic(UIC_DME_GET, MIB(PA_T_ACTIVATE, 0), 0, &t)) != NULL ||
	    (no = cold_uic(UIC_DME_SET, MIB(PA_T_ACTIVATE, 0), t + 1, NULL)) != NULL) {
		return no;
	}
	for (unsigned i = 0; i < sizeof(cport) / sizeof(cport[0]); i++) {
		if ((no = cold_uic(cport[i][0], MIB(cport[i][1], 0), cport[i][2], NULL)) != NULL) {
			return no;
		}
	}
	cold_host(UFSHC_BUSTHRTL, hc_read(UFSHC_BUSTHRTL) & ~UFSHC_LP_PG);
	if (real && (no = cold_clock_on(CLK_AUX)) != NULL) {
		return no;
	}
	if (!hc_poll(UFSHC_HCS, HCS_UTRLRDY, HCS_UTRLRDY, 500)) {
		return "the transfer list did not come ready";
	}
	cold_host(UFSHC_IS, hc_read(UFSHC_IS));
	hc_say("ufs: cold: 8 link up, T_ACTIVATE %u, C-port set: HCS %08x; this driver goes on from NOP OUT\n", t + 1, hc_read(UFSHC_HCS));
	return cold_refused ? "a write off the list" : NULL;
}

/* ---- what the steps need, before any of them touches anything ---- */

static uint32_t
cold_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* the payload's entry: as mkpayload.py lays it out; in RAM XNU's window stays clear of */
static const char *
cold_find_firmware(void)
{
	const uint8_t *p = (const uint8_t *)ml_io_map_unmappable(PAYLOAD_PA, PAYLOAD_MOST, VM_WIMG_DEFAULT);
	uint32_t size = 0, at = 8, end, magic;

	if (p == NULL) {
		return "the payload could not be mapped";
	}
	memcpy(&magic, p, 4);
	memcpy(&size, p + 4, 4);
	if (magic != 0x50554E58u || size > PAYLOAD_MOST - 8) {
		return "no payload at 0xF0000000";
	}
	end = size + 8;
	while (at + 24 <= end) {
		uint32_t n;

		memcpy(&n, p + at + 16, 4);
		if (p[at] == 0 || n > end - at - 24) {
			break;
		}
		if (strncmp((const char *)p + at, PHY_FW_ENTRY, 16) == 0) {
			if (n == 0 || (n & 3) != 0 || n > PHY_SRAM_SIZE) {
				return "the M-PHY firmware's size is not one the SRAM takes";
			}
			cold_fw = p + at + 24;
			cold_fw_size = n;
			hc_say("ufs: cold: M-PHY firmware: %u bytes in the payload, first word %02x%02x%02x%02x\n", n, cold_fw[3], cold_fw[2], cold_fw[1], cold_fw[0]);
			return NULL;
		}
		at += 24 + ((n + 15) & ~15u);
	}
	return "no " PHY_FW_ENTRY " in the payload (mkimage.sh -F)";
}

/* phy-cal-data of the host's node in ABL's tree: required by Google's driver, set by no dts in its trees or in vendor_boot: ABL's, from OTP */
static void
cold_find_calibration(void)
{
	uint64_t pa = 0;
	const uint8_t *fdt, *s, *names;
	uint32_t total, end, off = 0;
	bool ours = false;

	if (!PE_parse_boot_argn("abl_fdt", &pa, sizeof(pa)) || pa == 0) {
		hc_say("ufs: cold: no abl_fdt= from the loader\n");
		return;
	}
	fdt = (const uint8_t *)ml_io_map_unmappable((vm_offset_t)(pa & ~(uint64_t)PAGE_MASK), FDT_MOST + PAGE_SIZE, VM_WIMG_DEFAULT) + (pa & PAGE_MASK);
	total = cold_be32(fdt + 4);
	if (cold_be32(fdt) != 0xd00dfeedu || total > FDT_MOST) {
		hc_say("ufs: cold: ABL's tree at %llx: magic %08x, %u bytes: not read\n", pa, cold_be32(fdt), total);
		return;
	}
	s = fdt + cold_be32(fdt + 8);
	names = fdt + cold_be32(fdt + 12);
	end = cold_be32(fdt + 36);
	while (off + 4 <= end) {
		uint32_t token = cold_be32(s + off);

		off += 4;
		if (token == 1) {          /* BEGIN_NODE */
			const char *name = (const char *)s + off;

			ours = strncmp(name, "ufs@3c400000", 13) == 0;
			off = (off + (uint32_t)strlen(name) + 1 + 3) & ~3u;
		} else if (token == 3) {   /* PROP */
			uint32_t len = cold_be32(s + off);

			if (ours && strcmp((const char *)names + cold_be32(s + off + 4), "phy-cal-data") == 0 && (len & 3) == 0 && len / 4 <= PHY_CAL_MOST) {
				cold_cal = s + off + 8;
				cold_cal_n = len / 4;
				hc_say("ufs: cold: ABL's tree at %llx has phy-cal-data: %u values\n", pa, cold_cal_n);
				return;
			}
			off = (off + 8 + len + 3) & ~3u;
		} else if (token == 2 || token == 4) {
			continue;
		} else {
			break;
		}
	}
	hc_say("ufs: cold: ABL's tree at %llx has no phy-cal-data\n", pa);
}

/* ufs_native_cold=1: the host as hakios.ufshc.c finds a host ABL left up; NULL, or why not, with what the registers say */
static const char *
lga_cold(void)
{
	uint32_t ro = 0;
	bool phy_up = false;
	const char *no;

	PE_parse_boot_argn("ufs_native_ro", &ro, sizeof(ro));
	if (!hc_soc->read_only || ro != 1) {
		return "ufs_native_cold needs ufs_native_ro=1 and a host marked read only";
	}
	hc_say("ufs: cold: start (%s); nothing on this path writes to storage\n", hc_soc->name);
	if ((no = cold_find_firmware()) != NULL) {
		cold_dump(no);
		return no;
	}
	cold_find_calibration();
	no = hc_soc->pa != 0 ? lga_cold_power(&phy_up) : NULL;
	if (no == NULL) {
		no = lga_cold_host(phy_up);
	}
	if (no != NULL) {
		cold_dump(no);
	}
	return no;
}
