# Memory and scaling

Every run records its peak resident set size, taken from `/usr/bin/time -l`, as the `peak_rss_mib` column. It is recorded and never acted on: a large number is a finding, not a reason to stop a run.

The profiler carries no memory guard, cap, or early stop. An earlier version rejected any run during which the machine-wide `vm_stat` swapin counter moved — a since-boot counter covering every process on the host, not the run being measured — and it discarded valid measurements as a result. That guard, and the small-prefix data it was gating, are both gone; nothing here should be read as a claim about swap.

The medium-tier sweep (`results/pipeline_profile_medium.csv`) peaks at 6,525 MiB, on Q10/HiGHS at 2.5M variables; the median across all 90 runs is 668 MiB. The machine has 18 GiB, so headroom was never in question at this scale — but this is the real number, and it should be the one quoted, not a figure from the deleted small-prefix runs.
