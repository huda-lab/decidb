# Performance — open work

Open measurement questions, by stage of the solver path. Propose an optimization only where a measured cost supports
it. What is measured is in [findings.md](findings.md).

## PERF-1 — Methodology

Quantify measurement overhead: an instrumented run against an uninstrumented one, isolated from solver logs and stack sampling. None of the current sweep does this.

Decide whether to add a lightweight correctness check: the profiler checks row counts only, and even a per-query objective sanity bound would catch a fast wrong answer, which the row-count-only check cannot.

Repeat the sweep on a second host, since every number so far comes from one machine. The large-tier sweep exists on this machine: `benchmark/decide/results/pipeline_profile_large.csv` (written 2026-09-09; 90 rows: 65 `ok`, 10 `solver_limit`, 7 `process_timeout`, 6 `unsupported`, 2 `skipped_memory`), with raw spans in `pipeline_profile_raw_large/` and the console log in `sweep_large.log`. No finding cites it yet; `../issues/bugs.md` cites its raw logs for the HiGHS time-limit bug.

## PERF-2 — Planning

Quantify first-use backend discovery versus parsing, binding and rewriting. Distinguish fresh-process costs from repeated-query costs before proposing caching or environment reuse. Do not optimize sub-millisecond noise.

## PERF-3 — Input and extraction

Rank repeated scans and coefficient/RHS work by workload and clause. Compare Q8's keyed-decision mapping, Q1's low-cardinality `per` classes and P4's order-key classes. Propose batching/caching only where supported by measured cost.

`profile_pipeline.py` puts `execution.scope_checks` and `execution.evaluate_value_links` in no stage column, so their time shows only inside `non_solver_ms`. The scope checks (`../language/guide.md` §7.3) run before constraint evaluation on every query with a `when` or a `per` key, one pass over the kept rows per check. Add both spans to the `extraction` stage (each stage names parent spans only, so no time is counted twice), then rank the checks against constraint evaluation on Q1, Q6, Q8 and P4. Record the result in `findings.md`.

## PERF-4 — Model construction

Relate model time and boundary memory jumps to actual row/nonzero expansion and simultaneously live representations. Rank the expensive formulation families. Preserve provenance, diagnosis and backend contracts in any later reduction.

**A helper decision's box, per instance.** A helper decision of the value rewrite (`../language/guide.md` §6.4) has one instance per `by` group but one column box: `DeriveReducerValueBounds` (`src/decidb/formulation/linearization_reducer_value.cpp`) boxes it by the cover of every group's reach, and a `MIN`/`MAX` helper's closing Big-M reads that box end against its own group's far end. When groups reach very different ranges, every instance gets the widest group's box, so the narrow groups' Big-M rows are looser than their data needs and the LP relaxation is weaker. A box per instance would tighten it, but the model contract keeps one bound pair per decision (`SolverInput::lower_bounds` / `upper_bounds`), so it needs per-instance bounds in `../pipeline/06_model_formulation`. Measure first: a `max(e) by (g)` helper pinned from both sides beside a per-row decision (`x + 1 = max(x * v) by (g)`), two groups whose reach differs by orders of magnitude, node count and solve time with the shared box against a hand-boxed model. Record the finding in `findings.md`.

## PERF-5 — Backend loading

Quantify Gurobi environment and per-row API overhead before considering reuse/bulk loading. For HiGHS, isolate coordinate arrays, compressed packing and backend conversion time/memory before proposing ownership changes.

## PERF-6 — Solver execution

Run scale ladders and repetitions on both backends. Use separate native-log/stack-sample runs for slow cases. Extend solve budgets when that can resolve a concrete question. Do not equate timeouts with infeasibility or extrapolate small-model internal behavior. Keep algorithm/parameter changes deferred.

## PERF-7 — Output and cleanup

Quantify readback, temporary-result writing and destruction after fast solves. Preserve full row production and the keyed-decision mapping. Investigate expensive residual time rather than assigning it to a guessed phase.

## PERF-8 — Memory and scaling

Find the point where each workload and backend stops fitting comfortably, and which stage holds the memory when it does. Relate peak RSS to model size — variables, rows and nonzeros are recorded alongside it in the same CSV row. A large-database sweep exists, `benchmark/decide/results/pipeline_profile_large.csv` (written 2026-09-09, 90 rows with `peak_rss_mib`), and is not yet analysed here.
