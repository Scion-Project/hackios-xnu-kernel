# gov-replay

Runs the S9 governor's own source on the host against a model, to compare `gov_policy=1` and `gov_policy=2` when no phone is free.

- The clock is virtual. `sim.c` includes `hakios.exynos_cpufreq.c` unchanged and stubs the kernel calls under it.
- **Replay:** `mktrace.py` turns the once-a-second `cpufreq_gov:` lines of a console log (after the first 120 s of each boot, duplicates dropped) into work: the busiest core's cycles of that second are split over that second's frames, and finger events are spread through it.
- **Scenarios:** `scen:scroll`, `scen:tap` and `scen:hold` are synthetic gestures, 40 of each, after idle.

What it can't tell you:

- Work scales with the clock in the model, with no memory stalls, so it overstates what a faster clock buys.
- The split of work inside a second is invented, because the log has one line a second.
- The energy figure is a proxy: V² times cycles on busy cores, with no leakage or idle power. M3 voltages under 1586 MHz are extrapolated, because the log line that lists them is cut off there.
- ACPM requests take no time in the model, so `raise` reads 0 there.

The phone's numbers come from the `cpufreq_gov: stat` line, and they are the ones that count.
