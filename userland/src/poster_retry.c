/*
 * The wallpaper's second chance: once the start-up rush is over, PosterBoard is restarted if none of its posters
 * survived it.
 *
 * The wallpaper on Darwin userland 16 is drawn by a poster, an extension process (CollectionsPoster for the stock
 * wallpaper, GradientPosterExtension, EmojiPosterExtension, ...) that PosterBoard starts and SpringBoard hosts.
 * launchd gives such an extension five seconds from its spawn to check in, and kills it otherwise:
 *
 *   (pid/106/com.apple.WallpaperKit.CollectionsPoster [113]) <Notice>: service state: SIGKILLed
 *   ... exited with exit reason (namespace: 7 code: 0x3) - XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT
 *
 * PosterBoard starts each poster once and does not try again. On the POCO (eight cores, the big one at 2.4 GHz)
 * the posters check in well inside the five seconds. The Galaxy S9 runs on two Cortex-A55s (cpus=2 bootcpu=2:
 * core 1 and the M3s cannot be started yet, devices/starlte/README.md), and PosterBoard is started in the middle
 * of SpringBoard's rush, about a hundred processes in half a minute with the window server drawing at the same
 * time. In five of the six boots of 1 November (runs/usb-1101.log) every poster PosterBoard started was killed at
 * exactly 5.0 s, so the lock screen and the home screen had no wallpaper. A minute later the same extensions
 * check in in well under a second: SpringBoard's fifth CollectionsPoster of the last boot checked in after 4.3 s,
 * and the widget extensions chronod tried again after the rush checked in in 0.1 to 4.6 s.
 *
 * So this waits for the rush to pass (PosterBoard up for a while, and the load average down), counts the poster
 * processes (posters() below says why it takes two), and when PosterBoard's are not there, stops PosterBoard. launchd starts it again on the next request for it --
 * SpringBoard's, since it hosts the wallpaper -- and the posters it starts then have the processors to
 * themselves. It does that at most three times, and does nothing at all where the posters are already up, which
 * is every boot of a phone with the cores to spare.
 *
 * Boot-arg poster_retry=0 turns it off. poster_retry=2 also restarts chronod, WidgetKit's host, once, when the
 * rush is over, so that every widget asks its extension for a timeline again (its first requests met the same
 * five seconds); off by default, because it has not been seen to be needed.
 *
 *   usage: poster_retry [SECONDS]      give up after SECONDS (default 900)
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* libproc's, which the iPhoneOS SDK does not declare (as in cputop.c) */
#define PROC_ALL_PIDS   1
int proc_listpids(unsigned type, unsigned typeinfo, void *buffer, int buffersize);
int proc_name(int pid, void *buffer, unsigned buffersize);

#define MAX_PROCS       2048
#define LOOK            5       /* seconds between looks */
#define SETTLE          60      /* PosterBoard up for this long before anything is judged */
#define QUIET           1.5     /* the load average below which the rush is over (two cores) */
#define QUIET_WAIT      180     /* ... but no longer than this is waited for it */
#define RECOVER         90      /* after a restart, how long the posters are given to come up */
#define TRIES           3

static int pids[MAX_PROCS];

/* The process called exactly NAME, or 0. */
static int
find(const char *name)
{
	int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids));
	char here[256];

	for (int i = 0; bytes > 0 && i < bytes / (int)sizeof(int); i++) {
		if (pids[i] > 0 && proc_name(pids[i], here, sizeof(here)) > 0 && strcmp(here, name) == 0) {
			return pids[i];
		}
	}
	return 0;
}

/*
 * How many poster processes there are, their names and numbers into LIST. proc_name gives the executable's name:
 * "CollectionsPoster", "GradientPosterExtension", "PhotosPosterProvider", "EmojiPosterExtension" ...
 * PosterBoard starts all of its posters together (eight of them, and all eight were still up seventeen minutes
 * later in the one boot of 1 November where they came up), while SpringBoard starts only the one of the current
 * wallpaper for its own use: in the last boot of that day SpringBoard's fifth CollectionsPoster survived and every
 * one of PosterBoard's was dead. So one poster says nothing; two or more are PosterBoard's.
 */
static int
posters(char *list, size_t size)
{
	int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids)), count = 0;
	size_t used = 0;
	char here[256];

	list[0] = '\0';
	for (int i = 0; bytes > 0 && i < bytes / (int)sizeof(int); i++) {
		if (pids[i] <= 0 || proc_name(pids[i], here, sizeof(here)) <= 0 ||
		    strstr(here, "Poster") == NULL || strcmp(here, "PosterBoard") == 0) {
			continue;
		}
		count++;
		if (used < size) {
			used += (size_t)snprintf(list + used, size - used, " %s[%d]", here, pids[i]);
		}
	}
	return count;
}

static double
load(void)
{
	double avg[3] = { 0, 0, 0 };

	return getloadavg(avg, 3) > 0 ? avg[0] : 0;
}

/* SIGTERM, then SIGKILL if it is still there three seconds later */
static void
stop(int pid, const char *name)
{
	kill(pid, SIGTERM);
	for (int i = 0; i < 15; i++) {
		usleep(200000);
		if (kill(pid, 0) != 0 && errno == ESRCH) {
			return;
		}
	}
	printf("poster_retry: %s[%d] did not leave on SIGTERM; SIGKILL\n", name, pid);
	kill(pid, SIGKILL);
}

int
main(int argc, char **argv)
{
	char args[1024] = "", list[512];
	size_t size = sizeof(args) - 1;
	int limit = argc > 1 ? atoi(argv[1]) : 900, mode = 1, board, count, tries = 0, chronod_done = 0;
	int not_before = -1, quiet_by = -1;      /* when PosterBoard is first judged; when the load stops mattering */
	const char *at;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "poster_retry=")) != NULL) {
		mode = atoi(at + strlen("poster_retry="));
	}
	if (mode == 0) {
		printf("poster_retry: off (poster_retry=0)\n");
		return 0;
	}
	for (int waited = 0; waited < limit; sleep(LOOK), waited += LOOK) {
		if ((board = find("PosterBoard")) == 0) {
			continue;
		}
		if (not_before < 0) {
			not_before = waited + SETTLE;
			quiet_by = not_before + QUIET_WAIT;
		}
		if (waited < not_before || (load() >= QUIET && waited < quiet_by)) {
			continue;
		}
		if (mode == 2 && !chronod_done) {
			int chronod = find("chronod");

			chronod_done = 1;
			if (chronod != 0) {
				printf("poster_retry: chronod[%d] restarted, for the widgets' timelines (poster_retry=2)\n", chronod);
				stop(chronod, "chronod");
			}
		}
		if ((count = posters(list, sizeof(list))) >= 2) {
			printf("poster_retry: PosterBoard[%d]'s posters are up after %d s (load %.2f):%s%s\n", board, waited,
			    load(), list, tries ? "" : "; nothing to do");
			return 0;
		}
		if (tries == TRIES) {
			printf("poster_retry: still no posters of PosterBoard's after %d restarts (%d:%s); giving up\n", TRIES,
			    count, count ? list : " none");
			return 1;
		}
		tries++;
		printf("poster_retry: PosterBoard[%d] has none of its posters %d s into the run (load %.2f; %d:%s): they were killed "
		    "at launchd's 5 s check-in deadline; restarting it (%d of %d)\n", board, waited, load(), count,
		    count ? list : " none", tries, TRIES);
		stop(board, "PosterBoard");
		not_before = waited + RECOVER;
		quiet_by = not_before + QUIET_WAIT / 3;
	}
	printf("poster_retry: no PosterBoard within %d s; nothing done\n", limit);
	return 0;
}
