# Benchmark methodology

`benchmark/decide/profile_pipeline.py` discovers every `queries/q*.sql` and `queries/p*.sql` file, resolves its `${...}` placeholders through `run_benchmarks.py`'s medium-tier coefficient table, and runs each one as `CREATE TEMP TABLE result AS <query>` under the DuckDB CLI's own `.timer on`. `query_s` is that CLI-reported wall time, not a sum of spans. The backend is forced with `DECIDB_FORCE_SOLVER`; there is no silent fallback to a different backend than the one being measured.

The only correctness signal kept is the row count returned by `SELECT count(*) FROM result`. There is no constraint check and no objective recomputation — a query that runs to completion but returns a wrong answer would not be caught here. This is a deliberate trade against the earlier validation SQL, which existed for a different purpose (proving a rewrite bug was fixed) and was dropped once that job moved to the optimizer's own tests.

`DECIDB_PROFILE=1` makes the engine print one JSON line per span boundary to stderr. Two shapes exist: `begin`/`end` pairs for nested nesting, and pre-summed `aggregate` totals for two spans the engine doesn't log per-chunk (`execution.sink_append`, `execution.output_readback`). Both feed the same per-name totals; a parser that only reads `end` events silently reports these two phases as zero. `counter` events (model size, solver iteration/objective values) are collected separately and are keyed differently between backends — Gurobi's `ObjVal`, HiGHS's `highs.objective` — so the CSV takes whichever the run actually emitted.

Spans are inclusive parent totals. `solve_ms` is the backend's own `gurobi.optimize`/`highs.optimize` span alone; `non_solver_ms` is `query_s * 1000 - solve_ms`, i.e. everything DeciDB does that isn't the solver call itself.

`peak_rss_mib` is `/usr/bin/time -l`'s peak resident set size for the whole process. It is recorded and never enforced — there is no cap, no guard, and no early stop.

Campaigns before the 2026-09-08 `norm`-under-arithmetic fix (`01_pipeline/05_optimizer/done.md`) solved a reduced objective for Q3 and Q5; that data has been discarded rather than kept as a caveat. The current sweep runs against the fixed binary.
