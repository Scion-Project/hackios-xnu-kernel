/*
 * Performance-controller probe (perfctl=1): what user space tells the scheduler about its deadlines.
 *
 * On Apple's phones a closed controller registers sched_perfcontrol_callbacks and sets clocks from them. XNU's
 * half of that interface is open and nothing is registered here, so work_interval_notify(), the call a render
 * thread makes at the end of each frame with its start, finish and deadline, goes nowhere. Whether iOS makes that
 * call on hardware it does not recognise decides whether a governor can work from frame deadlines instead of load.
 *
 * This only counts. Callbacks run on the scheduler's paths, so they touch nothing but atomics; a thread call logs
 * the totals every five seconds, and only when they moved.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.log.h>
#include <sys/sysctl.h>

#define PERFCTL_LOG_SECS        5
#define PERFCTL_NTYPES          16      /* WORK_INTERVAL_TYPE_MASK's field, shifted down */
#define PERFCTL_TYPE_SHIFT      28

static _Atomic uint64_t notifies[PERFCTL_NTYPES];
static _Atomic uint64_t with_deadline, missed, slack_us;
static _Atomic uint64_t ctl_start, ctl_update, ctl_finish;
static _Atomic uint64_t deadlines_passed;

static thread_call_t call;

SYSCTL_DECL(_kern_hakios);
SYSCTL_QUAD(_kern_hakios, OID_AUTO, perfctl_with_deadline, CTLFLAG_RD | CTLFLAG_LOCKED, (uint64_t *)&with_deadline, "");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, perfctl_missed, CTLFLAG_RD | CTLFLAG_LOCKED, (uint64_t *)&missed, "");

static void
on_notify(perfcontrol_state_t state __unused, perfcontrol_work_interval_t wi)
{
	uint64_t ns;

	atomic_fetch_add_explicit(&notifies[(wi->create_flags >> PERFCTL_TYPE_SHIFT) % PERFCTL_NTYPES], 1, memory_order_relaxed);
	if (wi->deadline == 0 || wi->finish == 0) {
		return;
	}
	atomic_fetch_add_explicit(&with_deadline, 1, memory_order_relaxed);
	if (wi->finish > wi->deadline) {
		atomic_fetch_add_explicit(&missed, 1, memory_order_relaxed);
		return;
	}
	absolutetime_to_nanoseconds(wi->deadline - wi->finish, &ns);
	atomic_fetch_add_explicit(&slack_us, ns / NSEC_PER_USEC, memory_order_relaxed);
}

static void
on_ctl(perfcontrol_state_t state __unused, perfcontrol_work_interval_instance_t wi)
{
	_Atomic uint64_t *count = wi->ctl == WORK_INTERVAL_START ? &ctl_start :
	    wi->ctl == WORK_INTERVAL_UPDATE ? &ctl_update : &ctl_finish;

	atomic_fetch_add_explicit(count, 1, memory_order_relaxed);
}

static void
on_deadline_passed(uint64_t deadline __unused)
{
	atomic_fetch_add_explicit(&deadlines_passed, 1, memory_order_relaxed);
}

static void
tick(thread_call_param_t p0 __unused, thread_call_param_t p1 __unused)
{
	static uint64_t last;
	uint64_t total = 0, deadline, met;
	uint64_t ctl = ctl_start + ctl_update + ctl_finish;

	for (unsigned i = 0; i < PERFCTL_NTYPES; i++) {
		total += notifies[i];
	}
	if (total + ctl + deadlines_passed != last) {
		last = total + ctl + deadlines_passed;
		met = with_deadline - missed;
		say("perfctl: notify %llu (with deadline %llu, missed %llu, mean slack %llu us) ctl %llu/%llu/%llu passed %llu\n",
		    total, (uint64_t)with_deadline, (uint64_t)missed, met ? slack_us / met : 0,
		    (uint64_t)ctl_start, (uint64_t)ctl_update, (uint64_t)ctl_finish, (uint64_t)deadlines_passed);
		for (unsigned i = 0; i < PERFCTL_NTYPES; i++) {
			if (notifies[i] != 0) {
				say("perfctl: type %u: %llu\n", i, (uint64_t)notifies[i]);
			}
		}
	}
	clock_interval_to_deadline(PERFCTL_LOG_SECS, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}

static struct sched_perfcontrol_callbacks callbacks = {
	.version = SCHED_PERFCONTROL_CALLBACKS_VERSION_7,
	.work_interval_notify = on_notify,
	.work_interval_ctl = on_ctl,
	.deadline_passed = on_deadline_passed,
};

__startup_func
static void
perfctl_init(void)
{
	uint32_t enabled = 0;

	if (!PE_parse_boot_argn("perfctl", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	sched_perfcontrol_register_callbacks(&callbacks, 0);
	call = thread_call_allocate(tick, NULL);
	tick(NULL, NULL);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, perfctl_init);
