# Archived direct-solve profilers

`benchmark/decide/direct_solve/archive/` holds one-off profilers written while building the direct-solve rules. Each
answered a question about one rule. They are kept so the measurements the docs below cite can be reproduced. For new
direct-solve timing, use the [direct-solve benchmark](direct_solve.md).

All of them build synthetic source tables from `range(n)`, not TPC-H. All run each measured query
in a fresh process, pin `decide_direct_solve='require'` for the direct path (so a query the rule does not take fails)
and `'off'` for the solver path, and read the output back as one aggregate row unless stated otherwise. Each prints
`-h` with its options. All but the S1 API pair use the CLI at `build/release/decidb`.

| Script | Rule | Measures | Cited by |
|---|---|---|---|
| `profile_direct_a1.py` | A1 | direct against the default solver on A1 shapes | [result_adapter.md](../../direct_solve/architecture/result_adapter.md), [findings.md](../findings.md) |
| `profile_direct_s1_api.py` + `.cpp` | S1 | materialized against streamed result delivery through the C++ API | [glossary.md](../../glossary.md) |
| `profile_direct_s1.py` | S1 | end-to-end direct against HiGHS and Gurobi for a global S1, full output materialized; `profile_direct_s1_api.py` imports its phase list | none |

## `profile_direct_a1.py`

Times direct solve against the solver on eight A1 shapes: a REAL quadratic with and without WHEN, an INT linear, a
BOOL bound, a constant problem, a wide unused and a wide used 512-byte payload, and a count-only read. Modes are
`direct` and `solver` (the default backend, not forced). Direct runs `--repeats` times (default 2) and the solver once;
the table shows the median and peak memory. The solver skips the next size once it took over `--solver-budget`
(default 15 s). Databases `a1_<rows>.db` are built in `--db-dir` (required). Prints a Markdown table.

```bash
python3 benchmark/decide/direct_solve/archive/profile_direct_a1.py --rows 100000 1000000 --db-dir /tmp/a1db
```

## `profile_direct_s1_api.py` and `profile_direct_s1_api.cpp`

Compares S1 result delivery through the C++ API: `Connection::Query` (materialized) against `Connection::SendQuery`
(streamed). The C++ program runs one query per process; the Python driver sweeps rows, payload width, source kind,
output kind (`full`, `aggregate`, `consume`), mode (`direct`, `highs`, `gurobi`), delivery, cardinality
(`upper`, `lower`, `exact`, `interval`), scope (`global`, `grouped`, `grouped_local_when`), pins and bound kind. With
`--db-dir` the source is built once by a `prepare` run, so each measured run's peak memory is the query's own.
Collection cost is the `full` time minus the `consume` time. Writes `benchmark/decide/results/direct_solve/direct_s1_api_<stamp>.csv`
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
Writes `benchmark/decide/results/direct_solve/direct_s1_<stamp>.csv`, a JSON manifest and raw logs, or to `--output`. It imports
`profile_pipeline.py` and `run_benchmarks.py` from `benchmark/decide/`. No current doc cites it; it stays because
`profile_direct_s1_api.py` imports its `PHASES`.

```bash
python3 benchmark/decide/direct_solve/archive/profile_direct_s1.py --rows 1000 10000
```
