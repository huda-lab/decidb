# Memory and scaling

Find the point where each workload and backend stops fitting comfortably, and which stage holds the memory when it does. Relate peak RSS to model size — variables, rows and nonzeros are recorded alongside it in the same CSV row. A large-database sweep exists, `benchmark/decide/results/pipeline_profile_large.csv` (written 2026-09-09, 90 rows with `peak_rss_mib`), and is not yet analysed here.
