/*
 * The Pixel 10's frame kicker (lga_kick=HZ): starts one frame of the display controller's output 0 every period, so that what
 * the kernel and CoreAnimation draw into the buffer the boot loader left reaches the command-mode panel. The panel keeps its own
 * copy of the picture and a frame is only ever sent when the controller is told to send one (docs/pixel10-bringup.md in this
 * repository: facts 3 and 4); the splash stays on the glass whatever is written to the buffer.
 *
 * What is written, from Google's vsi-drm driver for the DC9x00 (private/google-modules/display/vsi-drm/verisilicon/9x00/
 * vs_dc_hw.c: dc_hw_start_trigger, dc_hw_sw_sof_trigger; vs_dc_reg_be.h at 6f689be7fa23; GPL-2.0, facts only): OUTPUT0_START
 * (0xB6000) = 1, then OUTPUT0_SW_CONFIG (0xB6004) = 1, "software panel start, ready". Whether these two alone start a frame
 * with the layers, mux and DSC as ABL left them is not shown by anything read: the frame counter DEBUG_DSI0_FRAME_CNT
 * (0xB6434) is read back before and after, and a counter that does not move is said so. A frame is started only when the
 * last one has moved the counter (or has had a few periods to).
 *
 * Nothing is written unless the chip id (0x20) is the DC9x00's 0x9400 and output 0 is in command mode (0xB6008 bit 1).
 * lga_dpu=BASE puts the controller somewhere else, for a test with RAM where it would be. Built into the Pixel 10's kernel
 * alone (configs/lga.config), and off unless lga_kick= asks. Status: written, run in QEMU against RAM, not seen on a phone.
 */

#include <stdint.h>
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.log.h>
#include <sys/hakios.xnu.h>

#define LGA_DPU_PA              0x0EE00000ULL           /* sswrp_dpu 0x0EC00000 + dc9x00@200000 */
#define LGA_DPU_SIZE            0xB8000                 /* through the frame counter */
#define DPU_CHIP_ID             0x000020                /* 0x9400 */
#define DPU_CHIP_REV            0x000024
#define DPU_CHIP_CUSTOMER       0x000030
#define DPU_PRODUCT_ID          0x0000A8
#define DPU_OUT0_START          0x0B6000
#define DPU_OUT0_SW_CONFIG      0x0B6004
#define DPU_OUT0_MODE           0x0B6008                /* bit 1: 1 command mode */
#define DPU_OUT0_CLK_EN         0x0B60B4
#define DPU_DSI0_FRAME_CNT      0x0B6434
#define DPU_CHIP_ID_DC9X00      0x9400
#define KICK_LOGGED             8                       /* kicks whose writes are each logged */
#define KICK_STALL_TICKS        4                       /* periods without a frame after which the next kick goes anyway */
#define KICK_REPORT_SECS        5

static vm_offset_t dpu;
static thread_call_t call;
static uint32_t period_ms, kicks, last_frames, stalled, reported_kicks, reported_frames;
static uint64_t reported_at;

static uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(dpu + off);
}

static void
wr(uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(dpu + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
tick(thread_call_param_t a __unused, thread_call_param_t b __unused)
{
	uint32_t frames = rd(DPU_DSI0_FRAME_CNT);
	uint64_t now = mach_absolute_time(), ns;
	uint64_t deadline;

	stalled = (kicks != 0 && frames == last_frames) ? stalled + 1 : 0;
	if (kicks == 0 || frames != last_frames || stalled >= KICK_STALL_TICKS) {
		wr(DPU_OUT0_START, 1);
		wr(DPU_OUT0_SW_CONFIG, 1);
		if (kicks < KICK_LOGGED) {
			say("lga_kick: kick %u: wrote %x <- 1, %x <- 1; frame counter %u before, %u after\n", kicks + 1,
			    DPU_OUT0_START, DPU_OUT0_SW_CONFIG, frames, rd(DPU_DSI0_FRAME_CNT));
		}
		kicks++;
		stalled = 0;
	}
	last_frames = frames;
	absolutetime_to_nanoseconds(now - reported_at, &ns);
	if (ns >= (uint64_t)KICK_REPORT_SECS * NSEC_PER_SEC) {
		if (kicks != reported_kicks) {
			say("lga_kick: %u kicks (+%u), frame counter %u (+%u in %llu s)%s\n", kicks, kicks - reported_kicks, frames,
			    frames - reported_frames, ns / NSEC_PER_SEC, frames == reported_frames ? ": IT DID NOT MOVE" : "");
		}
		reported_at = now;
		reported_kicks = kicks;
		reported_frames = frames;
	}
	clock_interval_to_deadline(period_ms, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}

__startup_func
static void
lga_kick_init(void)
{
	uint32_t hz = 0;
	uint64_t base = LGA_DPU_PA;
	uint32_t id;

	if (!PE_parse_boot_argn("lga_kick", &hz, sizeof(hz)) || hz == 0) {
		return;
	}
	PE_parse_boot_argn("lga_dpu", &base, sizeof(base));
	if (hz > 120) {
		hz = 120;
	}
	dpu = ml_io_map((vm_offset_t)base, LGA_DPU_SIZE);
	if (dpu == 0) {
		say("lga_kick: no mapping of %llx: nothing started\n", base);
		return;
	}
	id = rd(DPU_CHIP_ID);
	say("lga_kick: %llx: chip %x rev %x customer %x product %x; out0 start %x sw_config %x mode %x clk_en %x; frames %u\n", base,
	    id, rd(DPU_CHIP_REV), rd(DPU_CHIP_CUSTOMER), rd(DPU_PRODUCT_ID), rd(DPU_OUT0_START), rd(DPU_OUT0_SW_CONFIG),
	    rd(DPU_OUT0_MODE), rd(DPU_OUT0_CLK_EN), rd(DPU_DSI0_FRAME_CNT));
	if (id != DPU_CHIP_ID_DC9X00) {
		say("lga_kick: chip id is not the DC9x00's %x: nothing written\n", DPU_CHIP_ID_DC9X00);
		return;
	}
	if ((rd(DPU_OUT0_MODE) & 2) == 0) {
		say("lga_kick: output 0 is not in command mode: nothing written\n");
		return;
	}
	period_ms = 1000 / hz;
	reported_at = mach_absolute_time();
	call = thread_call_allocate(tick, NULL);
	say("lga_kick: a frame of output 0 every %u ms\n", period_ms);
	tick(NULL, NULL);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, lga_kick_init);
