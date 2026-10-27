# Pipeline profiler

`benchmark/decide/profile_pipeline.py` runs every benchmark query on Gurobi and on HiGHS and records where the time
goes inside the solver path: parsing, rewriting, input, coefficient extraction, model build, backend load, solve and
readback, plus model size and peak memory. Use it to find which stage of the pipeline is slow or holds the memory.
The findings it produces live in the stage folders of [`../README.md`](../README.md).

## Running

```bash
python3 benchmark/decide/profile_pipeline.py
```

It takes no options. Its constants sit at the top of the file:

| Constant | Value | Meaning |
|---|---|---|
| `DATABASE` | `"large"` | which `benchmark/decide/databases/{DATABASE}.db` to run on, and which coefficient tier fills the placeholders |
| `BACKENDS` | `gurobi`, `highs` | each run forces one with `DECIDB_FORCE_SOLVER` |
| `REPEATS` | 3 | passes over the whole query set |
| `SOLVER_SECONDS` | 300 | the solver's own time limit (`DECIDB_TIME_LIMIT`); reaching it is a result |
| `PROCESS_SECONDS` | 420 | hard stop for one process, so one query cannot hang the sweep |
| `SKIP` | `(Q10, highs)` | Q10 on HiGHS at `large` runs out of memory and is killed from outside, so it is recorded as `skipped_memory` instead of run |

The databases come from `make decide-bench-setup` ([tpch_suite.md](tpch_suite.md)). The profiler resumes: rows already
in the CSV are kept and their runs are not repeated. Delete the CSV to start over.

## Queries

The profiler discovers every `queries/q*.sql` and `queries/p*.sql`, ordered Q1 to Q11 then P1 to P4, and fills their
`${...}` placeholders through `run_benchmarks.py`'s coefficient table for the `DATABASE` tier. The Q queries are the
[TPC-H suite](tpch_suite.md#query-set). The P queries are simple shapes over `lineitem` that isolate one model size
effect each:

| Query | Shape |
|---|---|
| P1 | bounded LP: each row pushed to its own upper bound |
| P2 | separable convex QP: no constraint couples the rows |
| P3 | P2 plus one aggregate budget that couples every decision |
| P4 | Boolean ILP with exactly one row per `l_orderkey` (a high-cardinality `per`: `per l_orderkey: sum(keep) by (l_orderkey) = 1`) |

P4 has the shape direct solve answers, but a forced backend keeps every query on the solver path, so the profiler
measures the solver on it.

## How a run is measured

Each (query, backend, repeat) is one fresh `decidb` process on the database, opened `-readonly`, under
`/usr/bin/time -l`. The script runs the query as `CREATE TEMP TABLE result AS <query>` under the CLI's `.timer on`,
then `SELECT count(*) FROM result`. CTAS makes the engine produce every output row without formatting millions of them
for a terminal. `query_s` is the CLI's reported wall time for that statement, not a sum of spans.

The child environment drops every inherited `DECIDB_*` variable, then sets `DECIDB_FORCE_SOLVER`,
`DECIDB_TIME_LIMIT`, `DECIDB_PROFILE=1` and `DECIDB_BENCH=1`. A run cannot fall back to a backend other than the one
being measured.

`DECIDB_PROFILE=1` makes the engine print one JSON line per span event to stderr. The script prints
`DECIDB_PROFILE_QUERY_BEGIN` and `DECIDB_PROFILE_QUERY_END` around the measured statement and parses only what lies
between them, so setup and the row count are excluded. There are three event kinds:

- `begin` / `end` pairs for nested spans.
- `aggregate`: one pre-summed total for spans the engine does not log per chunk (`execution.sink_append`,
  `execution.output_readback`). These add into the same per-name totals as `end` events; a parser that reads only `end`
  events reports those two phases as zero.
- `counter`: model size and solver figures. Gurobi and HiGHS name them differently (`ObjVal` against
  `highs.objective`), so each CSV column takes whichever name the run emitted.

Spans are inclusive parent totals. Each CSV stage column sums only parent spans, so no time is counted twice:

| Column | Spans |
|---|---|
| `parse_bind_ms` | `frontend.parse`, `frontend.bind_select` |
| `canonicalize_ms` | `canonicalize.comparison`, `canonicalize.objective` |
| `rewrite_ms` | `optimizer.decide` |
| `plan_ms` | `frontend.physical_decide` |
| `input_ms` | `execution.assemble_input`, `execution.sink_append`, `execution.sink_combine` |
| `extraction_ms` | `execution.evaluate_constraints`, `execution.evaluate_objective`, `execution.evaluate_composed`, `execution.entity_mapping`, `execution.formulate` |
| `model_build_ms` | `solver.build_neutral_model` |
| `backend_load_ms` | `gurobi.load`, `highs.load` |
| `solve_ms` | `gurobi.optimize`, `highs.optimize` |
| `readback_ms` | `gurobi.status_and_readback`, `highs.status_and_readback`, `execution.output_readback`, `solver.postsolve`, `solver.cleanup` |

`non_solver_ms` is `query_s * 1000 - solve_ms`: everything DeciDB does that is not the solver call itself.

Counter columns: `variables`, `linear_rows`, `linear_nonzeros`, `quadratic_rows`, `objective`, `iterations`, `nodes`,
`mip_gap`.

`peak_rss_mib` is the whole process's peak resident set size from `/usr/bin/time -l`. It is recorded and never acted
on: there is no memory guard, cap or early stop.

`status` is one of `ok`; `unsupported` (the backend refused the model, matched on the error text); `solver_limit`
(the solver hit its time limit); `error`; `process_timeout`; `skipped_memory` (listed in `SKIP`).

## Correctness signal

The only correctness signal is the row count of `result` (the `rows` column). There is no constraint check and no
objective recomputation, so a query that finishes with a wrong answer is not caught here. Correctness belongs to the
oracle suite and the optimizer's own tests. Keep only data from binaries that lower `norm` under arithmetic ([`../../pipeline/05_optimizer/done.md`](../../pipeline/05_optimizer/done.md)); a binary without it solves a reduced objective for Q3 and Q5.

## Output

- `benchmark/decide/results/pipeline_profile_{DATABASE}.csv`: one row per (query, backend, repeat), rewritten after
  every run.
- `benchmark/decide/results/pipeline_profile_raw_{DATABASE}/{query}_{backend}_{repeat}/`: `stderr.log`,
  `stdout.log`, the parsed `profile.json` and `rows.json` for each run.

Both are gitignored; keep the files a finding cites.

## Tests

`test/decide/tests/test_pipeline_profile.py` checks query discovery and order, that every query resolves its
placeholders, span summing and counter reading, the folding of `aggregate` events, that events outside the query
markers are ignored, and that no span belongs to two stages and every stage and counter has a CSV column.
