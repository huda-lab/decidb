# Archived direct-solve profilers

`benchmark/decide/direct_solve/archive/` holds one-off profilers written while building the direct-solve rules. Each
answered a question about one rule, and the rule's `done.md` cites the measurement it produced, with the exact command.
They are kept so those measurements can be reproduced. For new direct-solve timing, use the
[direct-solve benchmark](direct_solve.md).

All of them build synthetic source tables from `range(n)`, not TPC-H. All run each measured query
in a fresh process, pin `decide_direct_solve='require'` for the direct path (so a query the rule does not take fails)
and `'off'` for the solver path, and read the output back as one aggregate row unless stated otherwise. Each prints
`-h` with its options. All but the S1 API pair use the CLI at `build/release/decidb`.

| Script | Rule | Measures | Cited by |
|---|---|---|---|
| `profile_direct_a1.py` | A1 | direct against the default solver on A1 shapes | [a1 done.md](../../08_direct_solve/rules/a1_separable_box/done.md), [result_adapter.md](../../08_direct_solve/architecture/result_adapter.md) |
| `profile_direct_r1.py` | R1 | direct against Gurobi and HiGHS on R1 shapes | [r1 done.md](../../08_direct_solve/rules/r1_continuous_knapsack/done.md) |
| `profile_r1_floor.py` | R1 | the least R1's plan could cost, and thread scaling | [r1 done.md](../../08_direct_solve/rules/r1_continuous_knapsack/done.md) |
| `profile_r1_plan.py` | R1 | R1's direct plan operator by operator, and the effect of source width | [r1 done.md](../../08_direct_solve/rules/r1_continuous_knapsack/done.md) |
| `profile_direct_s1_api.py` + `.cpp` | S1 | materialized against streamed result delivery through the C++ API | [s1 done.md](../../08_direct_solve/rules/s1_uniform_matroid/done.md), [glossary.md](../../08_direct_solve/architecture/glossary.md) |
| `profile_direct_s1.py` | S1 | end-to-end direct against HiGHS and Gurobi for a global S1, full output materialized | none |

## `profile_direct_a1.py`

Times direct solve against the solver on eight A1 shapes: a REAL quadratic with and without WHEN, an INT linear, a
BOOL bound, a constant problem, a wide unused and a wide used 512-byte payload, and a count-only read. Modes are
`direct` and `solver` (the default backend, not forced). Direct runs `--repeats` times (default 2) and the solver once;
the table shows the median and peak memory. The solver skips the next size once it took over `--solver-budget`
(default 15 s). Databases `a1_<rows>.db` are built in `--db-dir` (required). Prints a Markdown table.

```bash
python3 benchmark/decide/direct_solve/archive/profile_direct_a1.py --rows 100000 1000000 --db-dir /tmp/a1db
```

## `profile_direct_r1.py`

Times direct solve against Gurobi and HiGHS (`DECIDB_FORCE_SOLVER`) on eleven R1 shapes: `<=`, `>=`, `=` and
`BETWEEN` budgets, mixed signs, PER groups (1,000 groups), WHEN, two decisions, a column budget, an aggregate budget
and a count-only read. Budgets bind at 30% of the full spend; `prepare` stores them in `r1_<rows>.budgets.json` beside
each `r1_<rows>.db`. Before timing, direct and Gurobi must agree on each shape's objective at the smallest size within
1e-6 relative, or the script exits non-zero. Timeouts kill the process group and are reported as `timeout`; failures
as `error`. A solver skips the next size once it exceeded `--solver-budget`. Prints a Markdown table.

```bash
python3 benchmark/decide/direct_solve/archive/profile_direct_r1.py --rows 100000 1000000 --db-dir /tmp/r1db
```

## `profile_r1_floor.py`

Uses the databases of `profile_direct_r1.py` and times four methods: `floor`, a hand-written DuckDB query of the
greedy break-item rule (one sort, one running sum, one join back); `scan`, a bare scan returning one value per row;
`direct`, R1's direct plan under each `SET threads` setting; and `gurobi`, split into model construction, solve and
the rest using the `DECIDB_BENCH` lines. Per run it records wall, user and system CPU, peak memory, page faults and
swaps. Repeats of all methods at one size are interleaved round-robin so thermal drift hits all of them alike. The
floor and the direct plan must agree on the `le_positive` objective at every size. Writes `r1_floor.json` (raw
records) and `r1_floor.md` in `--db-dir`, or at `--out`.

```bash
python3 benchmark/decide/direct_solve/archive/profile_r1_floor.py --rows 100000 --db-dir build/r1db_smoke
```

## `profile_r1_plan.py`

Runs R1's direct plan with DuckDB's JSON profiler in detailed mode and reads it back. Per (shape, rows, source
variant, threads) it records latency, CPU time, plan and optimizer phase timings (including the DECIDE pass), peak
temp-directory size sampled every 20 ms, and per operator its CPU time, rows out, output size and output columns.
Source variants for `le_positive` are `stored` (the 13-column table of `--source-dir/r1_<rows>.db`, default
`build/r1db`), `narrow` (only the three columns the query names) and `wide` (20 extra DOUBLE columns). It imports its
shapes and run loop from the two scripts above. Writes `r1_plan_profile.json`, `r1_plan_profile.md` and the raw
profiles to `--out-dir` (default `--db-dir`).

```bash
python3 benchmark/decide/direct_solve/archive/profile_r1_plan.py --rows 100000 --source-dir build/r1db_smoke --db-dir build/r1db_plan_smoke
```

## `profile_direct_s1_api.py` and `profile_direct_s1_api.cpp`

Compares S1 result delivery through the C++ API: `Connection::Query` (materialized) against `Connection::SendQuery`
(streamed). The C++ program runs one query per process; the Python driver sweeps rows, payload width, source kind,
output kind (`full`, `aggregate`, `consume`), mode (`direct`, `highs`, `gurobi`), delivery, cardinality
(`upper`, `lower`, `exact`, `interval`), scope (`global`, `grouped`, `grouped_local_when`), pins and bound kind. With
`--db-dir` the source is built once by a `prepare` run, so each measured run's peak memory is the query's own.
Collection cost is the `full` time minus the `consume` time. Writes `benchmark/decide/results/direct_s1_api_<stamp>.csv`
with a JSON manifest beside it, or to `--output`.

```bash
clang++ -std=c++17 -O2 -I src/include benchmark/decide/direct_solve/archive/profile_direct_s1_api.cpp \
  -L build/release/src -lduckdb -Wl,-rpath,$PWD/build/release/src \
  -o benchmark/decide/results/profile_direct_s1_api
python3 benchmark/decide/direct_solve/archive/profile_direct_s1_api.py --rows 5000000 --db-dir /tmp/s1db --keep-db
```

## `profile_direct_s1.py`

End-to-end comparison for a global S1 (`SUM(x) <= K`, no PER) over a `range()` source, a stored temp table or a stored
join. It sweeps rows, payload width, K as a fraction of the rows, and score pattern (`mixed` or `tied`), on `direct`,
`highs` and `gurobi`, with `SET threads=4`. Each run writes the full DECIDE output with `CREATE TEMP TABLE ... AS`, and
`DECIDB_PROFILE=1` supplies phase timings. A timeout, solver limit or error is recorded, never counted as a win.
Writes `benchmark/decide/results/direct_s1_<stamp>.csv`, a JSON manifest and raw logs, or to `--output`. It imports
`profile_pipeline.py` and `run_benchmarks.py` from `benchmark/decide/`. No current doc cites it.

```bash
python3 benchmark/decide/direct_solve/archive/profile_direct_s1.py --rows 1000 10000
```
