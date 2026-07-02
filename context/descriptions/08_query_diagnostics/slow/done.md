# Query Diagnostics — Slow (how it works)

> Router terminal: **time_limit** (incumbent → report+gap · no sol → report slow). See `router/README.md`.

"Slow" = a solve that reaches the time limit without a proven optimum. It is **not**
a relax/reformulate engine like infeasible: it reports what the solver already found.
The interactive "keep going" continuation loop (S3) and returning the incumbent rows
on stop (S4) are still pending — see `todo.md`. Today a timeout still **errors** after
printing the report.

**Shared time limit (both backends).** The per-solve wall-clock cap is resolved in
one solver-agnostic place — `ResolveDecideTimeLimit()` in
`src/include/duckdb/decidb/solver_config.hpp` — so Gurobi and HiGHS honor the same
value and cannot drift. It returns `DECIDE_DEFAULT_TIME_LIMIT_SECONDS` (300s) unless
the user overrides it via the global `DECIDB_TIME_LIMIT` env var (seconds, double;
non-positive / unparseable values are ignored). Gurobi applies it as the `TimeLimit`
parameter (`gurobi_solver.cpp`); HiGHS applies it as the `time_limit` option
(`deterministic_naive.cpp`).

## Timeout produces a diagnosable result (S0 + S1)

Both backends now return `SolverStatus::TIME_LIMIT` on a limit-stop and populate the
incumbent fields on `SolverResult`:

- **S0 — HiGHS mapping fixed.** `highs.run()` returns `HighsStatus::kWarning` (not
  `kOk`) at the limit; `DeterministicNaive::Solve` now throws only on
  `HighsStatus::kError`, letting `kWarning` fall through to the `kTimeLimit →
  TIME_LIMIT` mapping. (Previously the `!= kOk` guard threw an INTERNAL error first —
  bug in `07_issues/bugs/done.md`.)
- **S1 — incumbent fields.** At `GRB_TIME_LIMIT` / `kTimeLimit` the backends fill
  `has_solution`, `best_bound`, `gap`, and (when `has_solution`) the incumbent
  `solution` vector + `objective_value`. `has_solution` gates the incumbent reads so
  the `-1e100` / `inf` / `nan` no-incumbent sentinels never leak; `best_bound` is always
  read. Field-by-field mapping and the "gap is a fraction on both backends" note are in
  `foundations/done.md` (Structured solver result / Time-limit behavior).

## Checkpoint report (S2)

On a `TIME_LIMIT` result, `PhysicalDecide::Finalize` routes through
`RouteSolveResult` to the `TIME_LIMIT` terminal (under `auto`; `off` still gets the
plain static error). The terminal prints a plain-language status block to **stderr**
and then — until S3/S4 land — falls through to the existing timeout error. Two shapes,
split on `has_solution`:

- **Solution found (path 1):**
  ```
  DECIDE hit the 300s time limit with a usable solution (not proven best).
    best objective so far: 147235  (within 0.09% of the best possible)
    elapsed 300s · peak memory 1.2 GB
  ```
- **No solution yet (path 2):**
  ```
  DECIDE hit the 300s time limit without finding a solution yet.
    elapsed 300s · peak memory 1.2 GB
  ```

Details:

- **Voice.** Obeys the user-output rule — no "incumbent"/"gap"/"bound" solver words.
  The `gap` fraction is rendered as a percentage ("within 0.09%").
- **Elapsed** is measured by an always-on `Profiler` wrapped around the `SolveModel`
  call (independent of `DECIDB_BENCH`), reported via `FormatDuration` (integer seconds
  when whole, one decimal otherwise).
- **Peak memory** is whole-process peak RSS via `getrusage(RUSAGE_SELF).ru_maxrss`,
  normalized (bytes on macOS, KiB on Linux) and formatted by `FormatMemory`. Honest
  caveat: whole-process, not solve-only — it is the number that answers "will
  continuing risk running out of RAM."
- **Scope.** S2 prints the *informational* block only. The interactive "keep improving
  it? [Enter] continue" prompt line is deliberately omitted until the S3 continuation
  loop can actually read it; printing an inert prompt would mislead.

Helpers (`PeakProcessMemoryBytes`, `FormatMemory`, `FormatDuration`,
`PrintDecideTimeoutReport`) and the terminal split live in
`src/execution/operator/decide/physical_decide.cpp`.

## Tests

`test/decide/tests/test_query_diagnostics_slow.py` (both backends): a 15-D knapsack
(x=0 always feasible, optimum not provable in ~1s) drives the path-1 block; a seeded
random market-split (feasibility-hard, no incumbent within the limit) drives path 2;
and `off` is asserted to suppress the report. The instances are sized with margin so
the asserted branch is stable across machines; the report is also checked to be free
of solver jargon.

