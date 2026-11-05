# gov-replay

Runs the S9 governor's own source on the host against a model, to compare the policies (`gov_policy=` 1, 2 and 3) when no phone is free, and to test the scion governor's safety paths.

- The clock is virtual. `sim.c` includes `hakios.exynos_cpufreq.c` unchanged and stubs the kernel calls under it.
- **Replay:** `mktrace.py` turns the once-a-second `cpufreq_gov:` lines of a console log (after the first 120 s of each boot, duplicates dropped) into work: the busiest core's cycles of that second are split over that second's frames, and finger events are spread through it.
- **Scenarios:** `scen:scroll`, `scen:tap`, `scen:hold` and `scen:two` are synthetic gestures, 40 of each, after idle. `two` is one render thread and a helper: the shape parking is for, so it flatters it.
- **Tests:** `run.sh test` fails if a stale or nonsense temperature does not bring the safe rate and all four M3s back, if 80 C does not end a burst above 1794 MHz at once, if 97 C does not bring the lowest level, or if more than two M3s ever run above 1794 MHz. The same check on the core count and rate runs under every replay.

What it can't tell you:

- Work scales with the clock in the model, with no memory stalls, so it overstates what a faster clock buys.
- The split of work inside a second is invented, because the log has one line a second.
- The energy figure is a proxy: V² times cycles on busy cores, with no leakage or idle power. M3 voltages under 1586 MHz are extrapolated, because the log line that lists them is cut off there.
- ACPM requests take no time in the model, so `raise` reads 0 there.
- The log gives only the busiest core's load, so no replay of it can say how often the other M3s would have been idle. Policy 3 in a replay is an upper bound.
- The temperature model is uncalibrated above 1794 MHz. The phone's reading is the one that counts.

The phone's numbers come from the `cpufreq_gov: stat` line, and they are the ones that count.
