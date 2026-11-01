/*
 * A USB device ("gadget") link for phones with a DesignWare DWC3: the kernel's log streamed to a host over a CDC-ACM
 * serial port (Linux /dev/ttyACM0, macOS /dev/cu.usbmodem*, Windows COMn), and a small line console back. For a run that
 * has no serial port and gave back only what fits in pstore (32 KB on the Galaxy S9, read in TWRP), this is the whole
 * log, as it is written, for as long as the run lasts, and the panic text at the end.
 *
 * Device-neutral: the DWC3 in device mode, its event buffer, endpoint 0 and the bulk endpoints are here; the CDC-ACM
 * descriptors and requests are plain C in hakios.usb_acm.h (checked on the build machine: tools/usb-acm-test.sh); what
 * the SoC has to do first (clocks, power, the PHY) is a glue's (hakios.usb_glue.h), today only the Exynos 9810's
 * (hakios.exynos_usb.c, the Galaxy S9). Polled from a thread call, no interrupt; one control transfer at a time, one
 * TRB per transfer (LST set), each in its own cache line of memory mapped uncached.
 *
 * The tree had no USB device code of its own: the POCO's loader serves fastboot through Qualcomm's UEFI USB protocol
 * (xnuloader/loader.c, usb_start()), which the S9's Silicium UEFI does not have (no USB driver in its FD), and nothing
 * of it survives ExitBootServices-less hand-over into the kernel. So this is new, not a port of that.
 *
 * THE STEPS (usb_gadget=N does the first N; each says what it is about to do before it does it):
 *   1  look: the glue reads the SoC's side (PMU isolation, clocks, the PHY's registers, the supplies through ACPM) and,
 *      when the controller has a clock, the DWC3's: its ID (GSNPSID), GCTL, DCTL (RUN_STOP: did S-Boot leave it running?),
 *      DSTS, the event buffer it was given. Reads only.
 *   2  PHY: a controller left running is stopped first (DCTL RUN_STOP clear, DEVCTRLHLT awaited, its events masked),
 *      then the glue's PHY sequence (Exynos 9810: hakios.exynos_usb.c's head, every write read back).
 *   3  core: a device soft reset (DCTL CSFTRST, as dwc3_soft_reset()), whatever the boot left (see below); then the
 *      USB 2.0 PHY interface, GCTL in device mode, bus bursts, the event buffer in this kernel's memory, DCFG high speed,
 *      the device events, endpoint 0 (DEPSTARTCFG, a transfer resource each, DEPCFG) and the first SETUP TRB. The pull-up
 *      stays off: the host sees nothing.
 *   4  enumerate: DCTL RUN_STOP: the host sees a device and asks; endpoint 0 answers (USB 2.0 chapter 9 and CDC's
 *      requests), each request said in the log (the first 64, then every hundredth); SET_CONFIGURATION sets up the bulk
 *      endpoints, nothing is sent on them. The PHY's line state is said every 10 s until a host is seen.
 *   5  ACM: the kernel's log from its first line (the part the kernel keeps of it, then everything new) goes out on
 *      bulk IN, and lines typed on the host come in on bulk OUT to the line console below.
 *
 * Was the controller touched before? Not by UEFI: Silicium's starlte FD has no USB driver (hakios.exynos_usb.c says
 * where that is seen). S-Boot has a download mode and charger detection and may leave it in any state, so step 3
 * always resets it, and step 2 stops it first if it runs; step 1 says what was found. The reset is the one Linux does
 * on every start (dwc3_core_init: dwc3_soft_reset then dwc3_core_soft_reset, both DCTL CSFTRST).
 *
 * THE LINE CONSOLE (step 5). Lines typed on the host, ended by Enter; the device echoes the line ("usb> ...") and
 * answers. The answers and the echo go into the kernel's log (and so to the host); not to the screen.
 *   help              these
 *   stat              the link's counters
 *   uptime            seconds since boot
 *   sysctl NAME       a sysctl's value (an integer or a string)
 *   log               the kernel's log again from its start (what the kernel keeps of it: the first 256 KB)
 *   return            end the run: the data volume saved, the drivers given back, the loader restarts the phone
 * Not a shell: user space is not reached from here.
 *
 * Boot-args:
 *   usb_gadget=<1-5>      the steps (0, or none: nothing)
 *   usb_gadget_soc=<name> the glue (exynos9810, the only one; the default)
 *   usb_gadget_clk=1      the glue may open a clock the boot left shut (Android's mux, divider and gate)
 *   usb_gadget_ldo=1      the glue puts the PHY's supplies into Android's USB mode (S9: S2MPS18 LDO9-14 mode 3, read back)
 *   usb_gadget_fs=1       full speed (12 Mbit/s) instead of high speed
 *   usb_gadget_susphy=1   let the link suspend the 2.0 PHY when idle (GUSB2PHYCFG SUSPHY, as Linux does; off by default)
 *   usb_gadget_poll=<ms>  the poll period while a host is attached (1-50; 2)
 *   usb_gadget_drain=<ms> on the way back to the loader, how long to keep sending the rest of the log (0-5000; 1000)
 *   usb_gadget_crlf=0     send the log's line ends as they are (LF) rather than CR LF
 *
 * Registers and fields: Samsung's kernel fork for the Galaxy S9 (Scion-Project/android_kernel_samsung_exynos9810,
 * lineage-24.0, e5906667068e), drivers/usb/dwc3/core.h and gadget.h, at the lines given next to each below; the
 * sequences after core.c dwc3_core_init (795-1000), dwc3_soft_reset (350-375), dwc3_core_config (140-253), dwc3_phy_setup
 * (637-765), gadget.c __dwc3_gadget_start (2025-2110), dwc3_gadget_set_ep_config (564-646), dwc3_gadget_start_config
 * (533-562), dwc3_gadget_run_stop (1733-1800), dwc3_gadget_reset_interrupt (2888), dwc3_gadget_conndone_interrupt (2998),
 * ep0.c (the control transfer's phases, 39-1160). GPL-2.0: read for facts only, no code taken. The event and TRB
 * layouts are those of core.h:1095-1205 and 676-710.
 *
 * Status: WRITTEN, NOT YET RUN on the phone. QEMU has no DWC3 device model (its DWC3 is the Xilinx boards' host-only
 * one), so the controller side is unchecked; the descriptors and requests are checked on the build machine.
 */
#include <stdarg.h>
#include <stdint.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/kalloc.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>

#include "hakios.usb_acm.h"
#include "hakios.usb_glue.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void loader_log_line(const char *line);
extern uint32_t loader_log_tap_set(void (*tap)(char c), void (*history)(const char *text, uint32_t len));
extern void loader_return(void);
extern void hakios_var_save_on_return(void);
extern void hakios_adreno_stop(void);
extern void hakios_wifi_stop(void);
extern void hakios_adsp_stop(void);
extern int kernel_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
extern void delay(int usec);
void hakios_usb_gadget_quiesce(void);

SYSCTL_DECL(_kern_hakios);

/* ---- the DWC3's registers (drivers/usb/dwc3/core.h of the BSP, line by line) ---- */
#define GSBUSCFG0               0xc100u         /* :83 */
#define  GSBUSCFG0_INCRBRSTEN   (1u << 0)       /* :161 */
#define  GSBUSCFG0_INCR16BRSTEN (1u << 3)       /* :164 */
#define  GSBUSCFG0_REQINFO      ((2u << 16) | (2u << 20) | (2u << 24) | (2u << 28))    /* :169-172 */
#define GSBUSCFG1               0xc104u         /* :84 */
#define  GSBUSCFG1_BREQLIMIT(n) ((n) << 8)      /* :177 */
#define GCTL                    0xc110u         /* :87 */
#define  GCTL_DSBLCLKGTNG       (1u << 0)       /* :227 */
#define  GCTL_SCALEDOWN_MASK    (3u << 4)       /* :222-223 */
#define  GCTL_SOFITPSYNC        (1u << 10)      /* :207, 221 */
#define  GCTL_PRTCAPDIR_MASK    (3u << 12)      /* :215, 218 */
#define  GCTL_PRTCAP_DEVICE     (2u << 12)      /* :215, 217 */
#define GSTS                    0xc118u         /* :89 */
#define GSNPSID                 0xc120u         /* :91; 0x5533xxxx DWC_usb3, 0x3331xxxx DWC_usb31 (core.c:804-807) */
#define GUCTL                   0xc12cu         /* :94 */
#define  GUCTL_USBHSTINAUTORETRYEN (1u << 14)   /* :231 */
#define  GUCTL_REFCLKPER_MASK   (0x3ffu << 22)  /* :229-230 */
#define  GUCTL_REFCLKPER(n)     ((n) << 22)
#define GHWPARAMS0              0xc140u         /* :99 */
#define GHWPARAMS1              0xc144u         /* :100 */
#define GHWPARAMS3              0xc14cu         /* :102 */
#define GUSB2PHYCFG             0xc200u         /* :118, port 0 */
#define  GUSB2_PHYSOFTRST       (1u << 31)      /* :235 */
#define  GUSB2_U2_FREECLK_EXISTS (1u << 30)     /* :236 */
#define  GUSB2_SUSPHY           (1u << 6)       /* :237 */
#define  GUSB2_ENBLSLPM         (1u << 8)       /* :239 */
#define GUSB3PIPECTL            0xc2c0u         /* :123, port 0 */
#define GEVNTADRLO              0xc400u         /* :128, buffer 0 */
#define GEVNTADRHI              0xc404u         /* :129 */
#define GEVNTSIZ                0xc408u         /* :130 */
#define  GEVNTSIZ_INTMASK       (1u << 31)      /* :281 */
#define GEVNTCOUNT              0xc40cu         /* :131 */
#define  GEVNTCOUNT_MASK        0xfffcu         /* :68 */
#define DCFG                    0xc700u         /* :137 */
#define  DCFG_SPEED_MASK        7u              /* :345 */
#define  DCFG_HIGHSPEED         0u              /* :348 */
#define  DCFG_FULLSPEED         1u              /* :349 */
#define  DCFG_DEVADDR_MASK      (0x7fu << 3)    /* :342-343 */
#define  DCFG_DEVADDR(a)        ((uint32_t)(a) << 3)
#define  DCFG_LPM_CAP           (1u << 22)      /* :356 */
#define DCTL                    0xc704u         /* :138 */
#define  DCTL_RUN_STOP          (1u << 31)      /* :359 */
#define  DCTL_CSFTRST           (1u << 30)      /* :360 */
#define  DCTL_KEEP_CONNECT      (1u << 19)      /* :381 */
#define  DCTL_U1U2_MASK         (0xfu << 9)     /* :386-389: INITU2ENA, ACCEPTU2ENA, INITU1ENA, ACCEPTU1ENA */
#define DEVTEN                  0xc708u         /* :139 */
#define  DEVTEN_DISCONNEVTEN    (1u << 0)       /* :416 */
#define  DEVTEN_USBRSTEN        (1u << 1)       /* :415 */
#define  DEVTEN_CONNECTDONEEN   (1u << 2)       /* :414 */
#define  DEVTEN_ULSTCNGEN       (1u << 3)       /* :413 */
#define DSTS                    0xc70cu         /* :140 */
#define  DSTS_CONNECTSPD        7u              /* :439; 0 high, 1 full (and 3), 4 super (:442-444) */
#define  DSTS_USBLNKST(v)       (((v) >> 18) & 0xfu)   /* :431-432 */
#define  DSTS_DEVCTRLHLT        (1u << 22)      /* :429 */
#define DALEPENA                0xc720u         /* :143; bit n: physical endpoint n (:498) */
#define DEP_BASE(n)             (0xc800u + (n) * 0x10u)        /* :145 */
#define  DEPCMDPAR2             0x00u           /* :146 */
#define  DEPCMDPAR1             0x04u           /* :147 */
#define  DEPCMDPAR0             0x08u           /* :148 */
#define  DEPCMD                 0x0cu           /* :149 */
#define  DEPCMD_PARAM(x)        ((uint32_t)(x) << 16)  /* :475-476 */
#define  DEPCMD_RSC_IDX(x)      (((x) >> 16) & 0x7fu)  /* :477 */
#define  DEPCMD_STATUS(x)       (((x) >> 12) & 0xfu)   /* :478 */
#define  DEPCMD_HIPRI_FORCERM   (1u << 11)      /* :479 */
#define  DEPCMD_CMDACT          (1u << 10)      /* :481 */
#define  CMD_SETEPCONFIG        0x01u           /* :495 */
#define  CMD_SETTRANSFRESOURCE  0x02u           /* :494 */
#define  CMD_SETSTALL           0x04u           /* :489 */
#define  CMD_CLEARSTALL         0x05u           /* :488 */
#define  CMD_STARTTRANSFER      0x06u           /* :487 */
#define  CMD_ENDTRANSFER        0x08u           /* :485 */
#define  CMD_DEPSTARTCFG        0x09u           /* :484 */
/* DEPCFG's parameters (gadget.h) */
#define DEPCFG_EP_TYPE(t)       (((t) & 3u) << 1)              /* gadget.h:44 */
#define DEPCFG_MPS(n)           (((n) & 0x7ffu) << 3)          /* gadget.h:45 */
#define DEPCFG_FIFO_NUMBER(n)   (((n) & 0x1fu) << 17)          /* gadget.h:46 */
#define DEPCFG_ACTION_INIT      (0u << 30)                     /* gadget.h:52 */
#define DEPCFG_ACTION_MODIFY    (2u << 30)                     /* gadget.h:54 */
#define DEPCFG_XFER_COMPLETE_EN (1u << 8)                      /* gadget.h:32 */
#define DEPCFG_XFER_IN_PROGRESS_EN (1u << 9)                   /* gadget.h:33 */
#define DEPCFG_XFER_NOT_READY_EN (1u << 10)                    /* gadget.h:34 */
#define DEPCFG_BINTERVAL_M1(n)  (((n) & 0xffu) << 16)          /* gadget.h:37 */
#define DEPCFG_EP_NUMBER(n)     (((n) & 0x1fu) << 25)          /* gadget.h:39 */
#define DEPXFERCFG_NUM_XFER_RES(n) ((n) & 0xffffu)             /* gadget.h:57 */
/* the TRB (core.h:676-710) */
#define TRB_SIZE_MASK           0x00ffffffu     /* :682 */
#define TRB_SIZE_TRBSTS(s)      (((s) >> 28) & 0xfu)   /* :685; SETUP_PENDING 2 (:689) */
#define TRB_CTRL_HWO            (1u << 0)       /* :693 */
#define TRB_CTRL_LST            (1u << 1)       /* :694 */
#define TRB_CTRL_ISP_IMI        (1u << 10)      /* :698 */
#define TRB_CTRL_IOC            (1u << 11)      /* :699 */
#define TRBCTL_NORMAL           (1u << 4)       /* :697, 703-707 */
#define TRBCTL_CONTROL_SETUP    (2u << 4)
#define TRBCTL_CONTROL_STATUS2  (3u << 4)
#define TRBCTL_CONTROL_STATUS3  (4u << 4)
#define TRBCTL_CONTROL_DATA     (5u << 4)
#define TRBSTS_SETUP_PENDING    2u
/* events (core.h:52-66, 1095-1205): bit 0 clear, an endpoint's: [5:1] the endpoint, [9:6] the event, [15:12] status */
#define DEPEVT_XFERCOMPLETE     1u              /* :1103 */
#define DEPEVT_XFERNOTREADY     3u              /* :1105 */
#define DEPEVT_STATUS_CONTROL_DATA   1u         /* :1150 */
#define DEPEVT_STATUS_CONTROL_STATUS 2u         /* :1151 */
#define DEVT_DISCONNECT         0u              /* :56-59 */
#define DEVT_RESET              1u
#define DEVT_CONNECT_DONE       2u
#define DEVT_LINK_STATUS_CHANGE 3u

/* physical endpoints: 0 ep0 OUT, 1 ep0 IN, 2 EP1 OUT, 3 EP1 IN, 5 EP2 IN */
#define PEP_OUT                 2
#define PEP_IN                  3
#define PEP_NOTIFY              5
#define NPEPS                   6

/* memory the controller reaches: one 32 KB block below 4 GB, mapped uncached */
#define DMA_SIZE                (32 * 1024)
#define EVT_OFF                 0x0000u
#define EVT_SIZE                4096u
#define TRB_OFF(slot)           (0x1000u + (slot) * 64u)       /* one TRB per cache line */
#define EP0_OFF                 0x2000u         /* 512 bytes */
#define OUT_OFF                 0x2400u         /* 512 bytes */
#define IN_OFF                  0x4000u
#define IN_CHUNK                16384u

#define RING_SIZE               (1024u * 1024u)        /* the log on its way out; a power of two */

struct trb {
	uint32_t bpl, bph, size, ctrl;
};

enum { EP0_IDLE, EP0_SETUP, EP0_DATA, EP0_STATUS_WAIT, EP0_STATUS };

static uint32_t steps, poll_ms = 2, drain_ms = 1000, crlf = 1, full_speed, susphy, glue_flags;
static const struct hakios_usb_glue *glue;
static vm_offset_t regs;
static volatile uint8_t *dma;
static uint64_t dma_pa;
static uint32_t ev_pos;
static thread_call_t call;
static int stage;                       /* how far the bring-up got: 0 none ... 5 */
static bool started, connected, high_speed;
static uint32_t busy;                   /* the poll, or the way back to the loader, has the controller */
static bool want_return;                /* the console asked to end the run */

static struct usbacm acm;
static struct usbacm_line cline;
static int ep0_state;
static bool ep0_in, ep0_three;          /* the data stage's direction; a data stage there was */
static uint16_t ep0_out_len;
static int ep0_status_pep = -1, ep0_notready_pep = -1;
static struct { bool active; uint32_t rsc; uint32_t len; } pep[NPEPS];

static char *ring;
static uint32_t ring_head, ring_tail, ring_dropped;
static uint64_t n_resets, n_setups, n_stalls, n_in, n_out, n_events, last_phy_say;
static int usb_speed_v;
static char usb_state[24] = "off";

SYSCTL_NODE(_kern_hakios, OID_AUTO, usb, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "the USB gadget link (drivers/core/hakios.dwc3.c, usb_gadget=N)");
SYSCTL_STRING(_kern_hakios_usb, OID_AUTO, state, CTLFLAG_RD | CTLFLAG_LOCKED, usb_state, 0, "off, phy, core, waiting, default, address, configured");
SYSCTL_QUAD(_kern_hakios_usb, OID_AUTO, bytes_in, CTLFLAG_RD | CTLFLAG_LOCKED, &n_in, "log bytes the host has taken");
SYSCTL_QUAD(_kern_hakios_usb, OID_AUTO, bytes_out, CTLFLAG_RD | CTLFLAG_LOCKED, &n_out, "bytes the host has sent");
SYSCTL_QUAD(_kern_hakios_usb, OID_AUTO, resets, CTLFLAG_RD | CTLFLAG_LOCKED, &n_resets, "bus resets");
SYSCTL_UINT(_kern_hakios_usb, OID_AUTO, dropped, CTLFLAG_RD | CTLFLAG_LOCKED, &ring_dropped, 0, "log bytes dropped: the host did not read fast enough");

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[600];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

/* the console's answers: into the log (and so to the host) as whole lines, not onto the screen */
static void con(const char *format, ...) __printflike(1, 2);

static void
con(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
}

static void
set_state(const char *s)
{
	strlcpy(usb_state, s, sizeof(usb_state));
}

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(regs + off);
}

static inline void
wr(uint32_t off, uint32_t value)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(regs + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline volatile struct trb *
trb_of(int p)
{
	int slot = p == PEP_NOTIFY ? 4 : p;

	return (volatile struct trb *)(dma + TRB_OFF(slot));
}

static inline uint64_t
trb_pa(int p)
{
	return dma_pa + TRB_OFF(p == PEP_NOTIFY ? 4 : p);
}

/* ---- the log's way out: a ring the log fills (under its lock) and the poll empties ---- */

static void
tap(char c)
{
	uint32_t h = ring_head, t = os_atomic_load(&ring_tail, acquire);

	if (h - t >= RING_SIZE) {
		ring_dropped++;
		return;
	}
	ring[h & (RING_SIZE - 1)] = c;
	os_atomic_store(&ring_head, h + 1, release);
}

static void
history(const char *text, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		tap(text[i]);
	}
}

/* ---- endpoint commands ---- */

static int
depcmd(int p, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t *out)
{
	uint32_t base = DEP_BASE((uint32_t)p), v = 0;

	wr(base + DEPCMDPAR2, p2);
	wr(base + DEPCMDPAR1, p1);
	wr(base + DEPCMDPAR0, p0);
	wr(base + DEPCMD, cmd | DEPCMD_CMDACT);
	for (int us = 0; us < 5000; us++) {
		v = rd(base + DEPCMD);
		if (!(v & DEPCMD_CMDACT)) {
			if (out != NULL) {
				*out = v;
			}
			if (DEPCMD_STATUS(v) != 0) {
				say("usb: endpoint %d command %x: status %u\n", p, cmd & 0xf, DEPCMD_STATUS(v));
				return EIO;
			}
			return 0;
		}
		delay(1);
	}
	say("usb: endpoint %d command %x: no answer in 5 ms (DEPCMD %08x)\n", p, cmd & 0xf, v);
	return ETIMEDOUT;
}

static int
start_xfer(int p, uint64_t buf_pa, uint32_t len, uint32_t trbctl)
{
	volatile struct trb *t = trb_of(p);
	uint32_t v = 0;
	uint64_t tp = trb_pa(p);

	t->bpl = (uint32_t)buf_pa;
	t->bph = (uint32_t)(buf_pa >> 32);
	t->size = len & TRB_SIZE_MASK;
	t->ctrl = trbctl | TRB_CTRL_LST | TRB_CTRL_IOC | TRB_CTRL_ISP_IMI | TRB_CTRL_HWO;
	__asm__ volatile ("dsb sy" ::: "memory");
	if (depcmd(p, CMD_STARTTRANSFER, (uint32_t)(tp >> 32), (uint32_t)tp, 0, &v) != 0) {
		return EIO;
	}
	pep[p].active = true;
	pep[p].rsc = DEPCMD_RSC_IDX(v);
	pep[p].len = len;
	return 0;
}

static void
end_xfer(int p)
{
	if (!pep[p].active) {
		return;
	}
	(void)depcmd(p, CMD_ENDTRANSFER | DEPCMD_HIPRI_FORCERM | DEPCMD_PARAM(pep[p].rsc), 0, 0, 0, NULL);
	delay(100);             /* as gadget.c's dwc3_stop_active_transfer waits */
	pep[p].active = false;
}

static int
ep_config(int p, uint32_t type, uint32_t mps, uint32_t action, uint32_t interval_m1)
{
	uint32_t p0 = DEPCFG_EP_TYPE(type) | DEPCFG_MPS(mps) | action;
	uint32_t p1 = DEPCFG_XFER_COMPLETE_EN | DEPCFG_EP_NUMBER((uint32_t)p);

	if (p & 1) {
		p0 |= DEPCFG_FIFO_NUMBER((uint32_t)p >> 1);
	}
	if (type == 0) {
		p1 |= DEPCFG_XFER_NOT_READY_EN;
	} else {
		p1 |= DEPCFG_XFER_IN_PROGRESS_EN;
	}
	if (type == 3) {
		p1 |= DEPCFG_BINTERVAL_M1(interval_m1);
	}
	return depcmd(p, CMD_SETEPCONFIG, p0, p1, 0, NULL);
}

/* ---- endpoint 0 ---- */

static void
ep0_setup_start(void)
{
	ep0_state = EP0_SETUP;
	ep0_status_pep = -1;
	ep0_notready_pep = -1;
	if (start_xfer(0, dma_pa + EP0_OFF, 8, TRBCTL_CONTROL_SETUP) != 0) {
		ep0_state = EP0_IDLE;
	}
}

static void
ep0_stall(void)
{
	n_stalls++;
	(void)depcmd(0, CMD_SETSTALL, 0, 0, 0, NULL);
	ep0_setup_start();
}

static void
ep0_try_status(void)
{
	if (ep0_state != EP0_STATUS_WAIT || ep0_notready_pep != ep0_status_pep) {
		return;
	}
	ep0_state = EP0_STATUS;
	if (start_xfer(ep0_status_pep, dma_pa + EP0_OFF, 0, ep0_three ? TRBCTL_CONTROL_STATUS3 : TRBCTL_CONTROL_STATUS2) != 0) {
		ep0_stall();
	}
}

static void bulk_down(void);
static void bulk_up(void);
static void pump_in(void);

static void
apply(const struct usbacm_reply *r)
{
	if (r->effects & USBACM_EFF_ADDRESS) {
		wr(DCFG, (rd(DCFG) & ~DCFG_DEVADDR_MASK) | DCFG_DEVADDR(r->value));    /* before the status stage (ep0.c:553) */
		say("usb: address %u\n", r->value);
		set_state(r->value ? "address" : "default");
	}
	if (r->effects & USBACM_EFF_CONFIG) {
		bulk_down();
		if (r->value) {
			bulk_up();
			set_state("configured");
			say("usb: configured (%s speed): /dev/ttyACM* on a Linux host, /dev/cu.usbmodem* on a Mac\n", high_speed ? "high" : "full");
		} else {
			set_state("address");
			say("usb: unconfigured\n");
		}
	}
	if (r->effects & (USBACM_EFF_HALT | USBACM_EFF_CLEAR_HALT)) {
		int p = r->ep == USBACM_EP_OUT ? PEP_OUT : r->ep == USBACM_EP_IN ? PEP_IN : PEP_NOTIFY;

		if (r->effects & USBACM_EFF_HALT) {
			end_xfer(p);
			(void)depcmd(p, CMD_SETSTALL, 0, 0, 0, NULL);
		} else {
			(void)depcmd(p, CMD_CLEARSTALL, 0, 0, 0, NULL);
			if (p == PEP_OUT && steps >= 5 && !pep[PEP_OUT].active) {
				(void)start_xfer(PEP_OUT, dma_pa + OUT_OFF, 512, TRBCTL_NORMAL);
			}
		}
	}
	if (r->effects & USBACM_EFF_LINES) {
		say("usb: the host %s the port (DTR %u RTS %u)\n", (acm.lines & 1) ? "opened" : "closed", acm.lines & 1, (acm.lines >> 1) & 1);
	}
}

static void
ep0_complete(int p)
{
	volatile struct trb *t = trb_of(p);
	uint32_t sts = TRB_SIZE_TRBSTS(t->size);
	struct usbacm_reply r;
	uint8_t pkt[8];

	switch (ep0_state) {
	case EP0_SETUP:
		for (int i = 0; i < 8; i++) {
			pkt[i] = dma[EP0_OFF + (uint32_t)i];
		}
		n_setups++;
		usbacm_setup(&acm, pkt, &r);
		if (n_setups <= 64 || n_setups % 100 == 0) {
			static const char *const what[] = { "stall", "status", "in", "out" };
			say("usb: setup %llu: %02x %02x %02x%02x %02x%02x %02x%02x -> %s %u\n", n_setups, pkt[0], pkt[1], pkt[3], pkt[2],
			    pkt[5], pkt[4], pkt[7], pkt[6], what[r.action & 3], r.len);
		}
		apply(&r);
		switch (r.action) {
		case USBACM_STATUS:
			ep0_three = false;
			ep0_status_pep = 1;
			ep0_state = EP0_STATUS_WAIT;
			ep0_try_status();
			break;
		case USBACM_IN:
			for (uint32_t i = 0; i < r.len; i++) {
				dma[EP0_OFF + i] = r.data[i];
			}
			ep0_in = true;
			ep0_three = true;
			ep0_state = EP0_DATA;
			if (start_xfer(1, dma_pa + EP0_OFF, r.len, TRBCTL_CONTROL_DATA) != 0) {
				ep0_stall();
			}
			break;
		case USBACM_OUT:
			ep0_in = false;
			ep0_three = true;
			ep0_out_len = r.len;
			ep0_state = EP0_DATA;
			/* an OUT data stage is received in whole packets (ep0.c __dwc3_ep0_do_control_data) */
			if (start_xfer(0, dma_pa + EP0_OFF, USBACM_EP0_MPS, TRBCTL_CONTROL_DATA) != 0) {
				ep0_stall();
			}
			break;
		default:
			ep0_stall();
			break;
		}
		break;
	case EP0_DATA:
		if (sts == TRBSTS_SETUP_PENDING) {
			ep0_setup_start();
			break;
		}
		if (!ep0_in) {
			uint32_t got = USBACM_EP0_MPS - (t->size & TRB_SIZE_MASK);
			uint8_t data[USBACM_EP0_MPS];

			for (uint32_t i = 0; i < got && i < sizeof(data); i++) {
				data[i] = dma[EP0_OFF + i];
			}
			if (usbacm_out_done(&acm, data, MIN(got, (uint32_t)ep0_out_len), &r) == USBACM_STALL) {
				ep0_stall();
				break;
			}
			apply(&r);
		}
		ep0_status_pep = ep0_in ? 0 : 1;
		ep0_state = EP0_STATUS_WAIT;
		ep0_try_status();
		break;
	case EP0_STATUS:
		ep0_setup_start();
		break;
	default:
		break;
	}
}

static void
ep0_notready(int p, uint32_t status)
{
	if (status == DEPEVT_STATUS_CONTROL_STATUS) {
		ep0_notready_pep = p;
		ep0_try_status();
	}
}

/* ---- the bulk endpoints ---- */

static void
bulk_down(void)
{
	end_xfer(PEP_OUT);
	end_xfer(PEP_IN);
	end_xfer(PEP_NOTIFY);
	wr(DALEPENA, rd(DALEPENA) & ~((1u << PEP_OUT) | (1u << PEP_IN) | (1u << PEP_NOTIFY)));
}

static void
bulk_up(void)
{
	uint32_t mps = high_speed ? USBACM_HS_BULK : USBACM_FS_BULK;

	if (ep_config(PEP_OUT, 2, mps, DEPCFG_ACTION_INIT, 0) != 0 || ep_config(PEP_IN, 2, mps, DEPCFG_ACTION_INIT, 0) != 0 ||
	    ep_config(PEP_NOTIFY, 3, 16, DEPCFG_ACTION_INIT, high_speed ? 8 : 0) != 0) {
		say("usb: the bulk endpoints could not be configured\n");
		return;
	}
	wr(DALEPENA, rd(DALEPENA) | (1u << PEP_OUT) | (1u << PEP_IN) | (1u << PEP_NOTIFY));
	if (steps >= 5) {
		(void)start_xfer(PEP_OUT, dma_pa + OUT_OFF, mps, TRBCTL_NORMAL);
		pump_in();
	}
}

static void
pump_in(void)
{
	uint32_t n = 0, lost;

	if (steps < 5 || acm.config == 0 || pep[PEP_IN].active || (acm.halted & 2)) {
		return;
	}
	lost = os_atomic_xchg(&ring_dropped, 0, relaxed);
	if (lost != 0) {
		n = (uint32_t)snprintf((char *)(uintptr_t)(dma + IN_OFF), 64, "\r\n[usb: %u bytes of the log lost]\r\n", lost);
	}
	uint32_t t = ring_tail, h = os_atomic_load(&ring_head, acquire);
	while (t != h && n + 2 <= IN_CHUNK) {
		char c = ring[t & (RING_SIZE - 1)];

		if (c == '\n' && crlf) {
			dma[IN_OFF + n++] = '\r';
		}
		dma[IN_OFF + n++] = (uint8_t)c;
		t++;
	}
	os_atomic_store(&ring_tail, t, release);
	if (n != 0) {
		(void)start_xfer(PEP_IN, dma_pa + IN_OFF, n, TRBCTL_NORMAL);
	}
}

static void console(char *line);

static void
bulk_complete(int p)
{
	volatile struct trb *t = trb_of(p);
	uint32_t left = t->size & TRB_SIZE_MASK, done = pep[p].len > left ? pep[p].len - left : 0;

	if (p == PEP_IN) {
		n_in += done;
		pump_in();
	} else if (p == PEP_OUT) {
		n_out += done;
		for (uint32_t i = 0; i < done; i++) {
			if (usbacm_line_feed(&cline, dma[OUT_OFF + i])) {
				console(cline.line);
			}
		}
		if (acm.config != 0 && !(acm.halted & 1)) {
			(void)start_xfer(PEP_OUT, dma_pa + OUT_OFF, high_speed ? USBACM_HS_BULK : USBACM_FS_BULK, TRBCTL_NORMAL);
		}
	}
}

/* ---- device events ---- */

static void
on_reset(void)
{
	n_resets++;
	if (n_resets <= 16) {
		say("usb: bus reset %llu\n", n_resets);
	}
	bulk_down();
	for (int p = 2; p < NPEPS; p++) {
		if (p != 4) {
			(void)depcmd(p, CMD_CLEARSTALL, 0, 0, 0, NULL);
		}
	}
	wr(DCFG, rd(DCFG) & ~DCFG_DEVADDR_MASK);
	usbacm_reset(&acm);
	set_state("default");
	if (ep0_state != EP0_SETUP) {
		end_xfer(0);
		end_xfer(1);
		ep0_setup_start();
	}
}

static void
on_connect_done(void)
{
	uint32_t speed = rd(DSTS) & DSTS_CONNECTSPD;

	high_speed = speed == 0;
	acm.high_speed = high_speed;
	connected = true;
	usb_speed_v = (int)speed;
	(void)ep_config(0, 0, USBACM_EP0_MPS, DEPCFG_ACTION_MODIFY, 0);
	(void)ep_config(1, 0, USBACM_EP0_MPS, DEPCFG_ACTION_MODIFY, 0);
	say("usb: connected at %s speed (DSTS %08x)\n", speed == 0 ? "high" : speed == 4 ? "super (unexpected)" : "full", rd(DSTS));
}

static void
events(void)
{
	uint32_t count = rd(GEVNTCOUNT) & GEVNTCOUNT_MASK;

	while (count >= 4) {
		uint32_t ev = *(volatile uint32_t *)(dma + EVT_OFF + ev_pos);

		ev_pos = (ev_pos + 4) % EVT_SIZE;
		count -= 4;
		n_events++;
		if (!(ev & 1)) {
			int p = (int)((ev >> 1) & 0x1f);
			uint32_t type = (ev >> 6) & 0xf, status = (ev >> 12) & 0xf;

			if (p < NPEPS && type == DEPEVT_XFERCOMPLETE) {
				pep[p].active = false;
				if (p <= 1) {
					ep0_complete(p);
				} else {
					bulk_complete(p);
				}
			} else if (p <= 1 && type == DEPEVT_XFERNOTREADY) {
				ep0_notready(p, status & 3);
			}
		} else if (((ev >> 1) & 0x7f) == 0) {
			switch ((ev >> 8) & 0xf) {
			case DEVT_DISCONNECT:
				say("usb: disconnected\n");
				connected = false;
				bulk_down();
				usbacm_reset(&acm);
				set_state("waiting");
				break;
			case DEVT_RESET:
				on_reset();
				break;
			case DEVT_CONNECT_DONE:
				on_connect_done();
				break;
			default:
				break;
			}
		}
		wr(GEVNTCOUNT, 4);
	}
}

/* ---- the line console ---- */

static void
console(char *line)
{
	char *arg = line;

	while (*arg != 0 && *arg != ' ') {
		arg++;
	}
	if (*arg == ' ') {
		*arg++ = 0;
		while (*arg == ' ') {
			arg++;
		}
	}
	con("usb> %s%s%s\n", line, *arg ? " " : "", arg);
	if (strcmp(line, "help") == 0) {
		con("usb: help, stat, uptime, sysctl NAME, log (the kernel's log again from its start), return (end the run)\n");
	} else if (strcmp(line, "stat") == 0) {
		con("usb: %s, %s speed, %llu resets, %llu requests (%llu stalled), %llu bytes sent, %llu received, %u lost, %llu events\n",
		    usb_state, high_speed ? "high" : "full", n_resets, n_setups, n_stalls, n_in, n_out, ring_dropped, n_events);
	} else if (strcmp(line, "uptime") == 0) {
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		con("usb: up %llu.%03llu s\n", ns / NSEC_PER_SEC, (ns / NSEC_PER_MSEC) % 1000);
	} else if (strcmp(line, "sysctl") == 0 && *arg != 0) {
		union { char s[256]; int32_t i; int64_t q; } v;
		size_t len = sizeof(v.s);
		bool text = true;

		memset(&v, 0, sizeof(v));
		if (kernel_sysctlbyname(arg, &v, &len, NULL, 0) != 0) {
			con("usb: sysctl %s: no such name\n", arg);
			return;
		}
		for (size_t i = 0; i < len; i++) {
			if (v.s[i] == 0 && i + 1 == len) {
				break;
			}
			if (v.s[i] < 32 || v.s[i] > 126) {
				text = false;
			}
		}
		if (text && len > 0) {
			v.s[MIN(len, sizeof(v.s) - 1)] = 0;
			con("usb: %s = %s\n", arg, v.s);
		} else if (len == 4) {
			con("usb: %s = %d\n", arg, v.i);
		} else if (len == 8) {
			con("usb: %s = %lld\n", arg, v.q);
		} else {
			con("usb: %s: %zu bytes, not shown\n", arg, len);
		}
	} else if (strcmp(line, "log") == 0) {
		con("usb: the kernel's log from its start:\n");
		(void)loader_log_tap_set(tap, history);
		con("usb: (end of what the kernel keeps; the rest was sent as it came)\n");
	} else if (strcmp(line, "return") == 0) {
		con("usb: ending the run: the loader will restart the phone\n");
		want_return = true;     /* done by tick() once this event is handled and the controller let go */
	} else {
		con("usb: %s: not a command (help)\n", line);
	}
}

/* ---- bring-up ---- */

static void
say_dwc3(const char *when)
{
	say("usb: DWC3 %s: GSNPSID %08x GCTL %08x GSTS %08x GUSB2PHYCFG %08x GUSB3PIPECTL %08x GHWPARAMS0/1/3 %08x %08x %08x\n", when,
	    rd(GSNPSID), rd(GCTL), rd(GSTS), rd(GUSB2PHYCFG), rd(GUSB3PIPECTL), rd(GHWPARAMS0), rd(GHWPARAMS1), rd(GHWPARAMS3));
	say("usb: DWC3 %s: DCFG %08x DCTL %08x (RUN_STOP %u) DSTS %08x (link state %u, halted %u) DEVTEN %08x event buffer %08x%08x size %08x count %08x\n",
	    when, rd(DCFG), rd(DCTL), (rd(DCTL) & DCTL_RUN_STOP) ? 1 : 0, rd(DSTS), DSTS_USBLNKST(rd(DSTS)),
	    (rd(DSTS) & DSTS_DEVCTRLHLT) ? 1 : 0, rd(DEVTEN), rd(GEVNTADRHI), rd(GEVNTADRLO), rd(GEVNTSIZ), rd(GEVNTCOUNT));
}

/* the controller stopped: pull-up off, then its events masked (gadget.c dwc3_gadget_run_stop(, 0), 1733-1800) */
static bool
stop_controller(void)
{
	wr(DCTL, rd(DCTL) & ~DCTL_RUN_STOP);
	for (int i = 0; i < 100; i++) {
		if (rd(DSTS) & DSTS_DEVCTRLHLT) {
			break;
		}
		delay(1000);
	}
	wr(GEVNTSIZ, GEVNTSIZ_INTMASK | rd(GEVNTSIZ));
	return (rd(DSTS) & DSTS_DEVCTRLHLT) != 0;
}

static int
dma_alloc(void)
{
	vm_offset_t va = 0;
	/* any address: the 9810's DWC3 takes 64-bit TRB and event-buffer pointers, and this kernel's RAM is above 4 GB
	 * (0x880000000 on, seen 1 November) */

	if (kmem_alloc_contig(kernel_map, &va, round_page(DMA_SIZE), PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return ENOMEM;
	}
	FlushPoC_DcacheRegion(va, round_page(DMA_SIZE));
	dma_pa = kvtophys(va);
	dma = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)dma_pa, round_page(DMA_SIZE), VM_WIMG_WCOMB);
	if (dma == NULL) {
		return ENOMEM;
	}
	for (uint32_t i = 0; i < DMA_SIZE; i += 4) {
		*(volatile uint32_t *)(dma + i) = 0;
	}
	return 0;
}

/* step 3: the core reset and set up as a device, the pull-up still off */
static int
core_init(void)
{
	uint32_t id = rd(GSNPSID), v;
	int us;

	if ((id & 0xffff0000u) != 0x55330000u && (id & 0xffff0000u) != 0x33310000u) {
		say("usb: 3: GSNPSID %08x is not a DWC3's: stopping\n", id);
		return ENODEV;
	}
	say("usb: 3: device soft reset (DCTL CSFTRST)\n");
	wr(DCTL, DCTL_CSFTRST);
	for (us = 0; us < 500000 && (rd(DCTL) & DCTL_CSFTRST); us += 10) {
		delay(10);
	}
	if (rd(DCTL) & DCTL_CSFTRST) {
		say("usb: 3: the soft reset did not finish in 500 ms (DCTL %08x): the PHY's clock is not there? stopping\n", rd(DCTL));
		return ETIMEDOUT;
	}
	say("usb: 3: reset done in %d us\n", us);

	/* the 2.0 PHY interface: dwc3_phy_setup (core.c:637-765); the dt's snps,dis-u2-freeclk-exists-quirk (core.c:761) */
	v = rd(GUSB2PHYCFG) & ~(GUSB2_U2_FREECLK_EXISTS | GUSB2_ENBLSLPM | GUSB2_SUSPHY);
	if (susphy) {
		v |= GUSB2_SUSPHY;
	}
	wr(GUSB2PHYCFG, v);

	/* GCTL: device mode; SOFITPSYNC for the same quirk (core.c:921) */
	v = rd(GCTL) & ~(GCTL_SCALEDOWN_MASK | GCTL_PRTCAPDIR_MASK);
	wr(GCTL, v | GCTL_PRTCAP_DEVICE | GCTL_SOFITPSYNC);

	/* the bus: dwc3_core_config (core.c:140-186) */
	v = rd(GSBUSCFG0) | GSBUSCFG0_INCRBRSTEN | GSBUSCFG0_INCR16BRSTEN;
	if ((id & 0xffff0000u) == 0x33310000u || id >= 0x5533220au) {
		v |= GSBUSCFG0_REQINFO;
	}
	wr(GSBUSCFG0, v);
	wr(GSBUSCFG1, rd(GSBUSCFG1) | GSBUSCFG1_BREQLIMIT(3));
	wr(GUCTL, (rd(GUCTL) & ~GUCTL_REFCLKPER_MASK) | GUCTL_USBHSTINAUTORETRYEN | GUCTL_REFCLKPER(0x14));

	/* the event buffer (core.c dwc3_event_buffers_setup, 476-495) */
	ev_pos = 0;
	wr(GEVNTADRLO, (uint32_t)(dma_pa + EVT_OFF));
	wr(GEVNTADRHI, (uint32_t)((dma_pa + EVT_OFF) >> 32));
	wr(GEVNTSIZ, EVT_SIZE);
	wr(GEVNTCOUNT, 0);

	/* DCFG: the speed, address 0, no LPM (bcdUSB 2.00) */
	v = rd(DCFG) & ~(DCFG_SPEED_MASK | DCFG_DEVADDR_MASK | DCFG_LPM_CAP);
	wr(DCFG, v | (full_speed ? DCFG_FULLSPEED : DCFG_HIGHSPEED));
	wr(DEVTEN, DEVTEN_DISCONNEVTEN | DEVTEN_USBRSTEN | DEVTEN_CONNECTDONEEN | DEVTEN_ULSTCNGEN);

	/* endpoint 0 (gadget.c __dwc3_gadget_ep_enable -> dwc3_gadget_start_config, 533-562) */
	if (depcmd(0, CMD_DEPSTARTCFG, 0, 0, 0, NULL) != 0) {
		return EIO;
	}
	for (int p = 0; p < NPEPS; p++) {
		if (depcmd(p, CMD_SETTRANSFRESOURCE, DEPXFERCFG_NUM_XFER_RES(1), 0, 0, NULL) != 0) {
			return EIO;
		}
	}
	if (ep_config(0, 0, USBACM_EP0_MPS, DEPCFG_ACTION_INIT, 0) != 0 || ep_config(1, 0, USBACM_EP0_MPS, DEPCFG_ACTION_INIT, 0) != 0) {
		return EIO;
	}
	wr(DALEPENA, 3);
	usbacm_init(&acm);
	ep0_setup_start();
	if (ep0_state != EP0_SETUP) {
		return EIO;
	}
	say_dwc3("set up");
	return 0;
}

static int
run(void)
{
	uint32_t v = rd(DCTL) & ~(DCTL_KEEP_CONNECT | DCTL_U1U2_MASK);

	wr(DCTL, v | DCTL_RUN_STOP);
	for (int i = 0; i < 100; i++) {
		if (!(rd(DSTS) & DSTS_DEVCTRLHLT)) {
			return 0;
		}
		delay(1000);
	}
	say("usb: 4: still halted 100 ms after RUN_STOP (DSTS %08x)\n", rd(DSTS));
	return ETIMEDOUT;
}

static bool
bring_up(void)
{
	int e;

	say("usb: steps 1-%u (1 look, 2 PHY, 3 core, 4 enumerate, 5 log and console) on %s's glue\n", steps, glue->name);
	say("usb: 1: reading what the boot left\n");
	bool readable = glue->look(say, glue_flags);
	regs = ml_io_map((vm_offset_t)glue->dwc3_pa, glue->dwc3_size);
	if (regs == 0) {
		say("usb: could not map the DWC3 at %#llx; off\n", glue->dwc3_pa);
		return false;
	}
	if (readable) {
		say_dwc3("as the boot left it");
	}
	stage = 1;
	if (steps < 2) {
		return false;
	}

	if (readable && (rd(DCTL) & DCTL_RUN_STOP)) {
		say("usb: 2: the controller was left running (DCTL %08x): stopping it first\n", rd(DCTL));
		say("usb: 2: %s\n", stop_controller() ? "halted" : "NOT halted after 100 ms; going on");
	}
	say("usb: 2: the PHY (%s)\n", glue->name);
	if ((e = glue->phy_on(say, glue_flags)) != 0) {
		say("usb: 2: the PHY did not come up (%d); off\n", e);
		return false;
	}
	stage = 2;
	set_state("phy");
	if (steps < 3) {
		return false;
	}

	say("usb: 3: memory for the controller (32 KB, uncached)\n");
	if (dma_alloc() != 0) {
		say("usb: 3: no memory for the controller; off\n");
		return false;
	}
	say("usb: 3: at %#llx; resetting the core and setting it up as a device (%s speed), pull-up off\n", dma_pa,
	    full_speed ? "full" : "high");
	if ((e = core_init()) != 0) {
		say("usb: 3: failed (%d); off\n", e);
		return false;
	}
	stage = 3;
	set_state("core");
	if (steps < 4) {
		return false;
	}

	if (steps >= 5) {
		ring = kalloc_data(RING_SIZE, Z_WAITOK | Z_ZERO);
		if (ring == NULL) {
			say("usb: 5: no memory for the log's ring; going on without it (step 4)\n");
			steps = 4;
		} else {
			say("usb: 5: the kernel's log goes out on bulk IN from its first line; lines from the host to the console\n");
			(void)loader_log_tap_set(tap, history);
		}
	}
	say("usb: 4: pull-up on (DCTL RUN_STOP): a host should see %04x:%04x \"%s\"\n", USBACM_VID, USBACM_PID, "HakiOS kernel log and console");
	if (run() != 0) {
		return false;
	}
	stage = steps >= 5 ? 5 : 4;
	set_state("waiting");
	return true;
}

static void
tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	if (!os_atomic_cmpxchg(&busy, 0, 1, acquire)) {
		goto again;
	}
	if (!started) {
		started = true;
		if (!bring_up()) {
			os_atomic_store(&busy, 0, release);
			return;
		}
	}
	events();
	pump_in();
	if (!connected && glue->phy_state != NULL) {
		uint64_t now = mach_absolute_time(), ns;

		absolutetime_to_nanoseconds(now - last_phy_say, &ns);
		if (last_phy_say == 0 || ns > 10 * NSEC_PER_SEC) {
			char st[120];

			glue->phy_state(st, sizeof(st));
			say("usb: waiting for a host: %s; DSTS %08x\n", st, rd(DSTS));
			last_phy_say = now;
		}
	}
	os_atomic_store(&busy, 0, release);
	if (want_return) {
		hakios_var_save_on_return();
		hakios_adreno_stop();
		hakios_wifi_stop();
		hakios_adsp_stop();
		loader_return();
	}
again:
	clock_interval_to_deadline(connected ? poll_ms : 20, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}

/*
 * On the way back to the loader (loader_return(), the other processors already stopped, the panic text appended): what
 * is left of the log sent while the host takes it, for up to usb_gadget_drain ms; then the pull-up off, the events
 * masked, the PHY powered down. Nothing of this kernel's memory is used by the controller after it.
 */
void
hakios_usb_gadget_quiesce(void)
{
	if (stage < 3 || regs == 0) {
		return;
	}
	for (int i = 0; i < 1000 && !os_atomic_cmpxchg(&busy, 0, 1, acquire); i++) {
		delay(100);     /* the poll is mid-way; after 100 ms it is taken to be gone with its processor */
	}
	if (stage >= 5 && acm.config != 0) {
		for (uint32_t ms = 0; ms < drain_ms; ms++) {
			events();
			pump_in();
			if (!pep[PEP_IN].active && os_atomic_load(&ring_head, acquire) == ring_tail) {
				break;
			}
			delay(1000);
		}
	}
	(void)loader_log_tap_set(NULL, NULL);
	bulk_down();
	(void)stop_controller();
	if (glue->phy_off != NULL) {
		glue->phy_off();
	}
	stage = 0;
}

__startup_func
static void
usb_gadget_init(void)
{
	char soc[32] = "exynos9810";
	uint32_t value = 0;

	if (!PE_parse_boot_argn("usb_gadget", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	steps = steps > 5 ? 5 : steps;
	(void)PE_parse_boot_argn("usb_gadget_soc", soc, sizeof(soc));
	if (PE_parse_boot_argn("usb_gadget_clk", &value, sizeof(value)) && value) {
		glue_flags |= USB_GLUE_CLK;
	}
	if (PE_parse_boot_argn("usb_gadget_ldo", &value, sizeof(value)) && value) {
		glue_flags |= USB_GLUE_LDO;
	}
	(void)PE_parse_boot_argn("usb_gadget_fs", &full_speed, sizeof(full_speed));
	(void)PE_parse_boot_argn("usb_gadget_susphy", &susphy, sizeof(susphy));
	(void)PE_parse_boot_argn("usb_gadget_crlf", &crlf, sizeof(crlf));
	if (PE_parse_boot_argn("usb_gadget_poll", &value, sizeof(value)) && value >= 1 && value <= 50) {
		poll_ms = value;
	}
	if (PE_parse_boot_argn("usb_gadget_drain", &value, sizeof(value)) && value <= 5000) {
		drain_ms = value;
	}
	if (strcmp(soc, "exynos9810") == 0) {
		glue = hakios_usb_glue_exynos9810();
	}
	if (glue == NULL) {
		say("usb: usb_gadget=%u, but no glue for \"%s\" in this kernel; off\n", steps, soc);
		return;
	}
	call = thread_call_allocate(tick, NULL);
	thread_call_enter(call);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, usb_gadget_init);
