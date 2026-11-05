#!/bin/bash
# The S9 CPU governor's real source (drivers/power/exynos/hakios.exynos_cpufreq.c) run on the host against a model of the M3s, for
# comparing gov_policy=1 and gov_policy=2 before a phone is free. Not a phone: see README.md.
#   tools/gov-replay/run.sh CONSOLE.log          the log's cpufreq_gov lines replayed, both policies
#   tools/gov-replay/run.sh scen:scroll|tap|hold synthetic gestures, both policies
set -e
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd); tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp"/stub/{kern,libkern,mach,machine,pexpert,sys}
for h in kern/clock.h kern/thread.h libkern/libkern.h mach/mach_time.h mach/mach_types.h machine/machine_routines.h \
    pexpert/pexpert.h sys/hakios.exynos_acpm.h sys/param.h sys/systm.h sys/sysctl.h sys/errno.h; do
	echo '#include "simstub.h"' > "$tmp/stub/$h"
done
clang -O1 -w -I "$here" -I "$tmp/stub" -I "$root/drivers/power" -o "$tmp/sim" "$here/sim.c" -lm
input=$1; shift
case "$input" in
scen:*) trace=$input ;;
*) python3 "$here/mktrace.py" "$input" > "$tmp/trace.txt"; trace=$tmp/trace.txt ;;
esac
for policy in 1 2; do "$tmp/sim" "$policy" "$trace" "$@"; done
