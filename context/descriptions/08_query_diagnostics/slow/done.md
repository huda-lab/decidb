# Query Diagnostics — Slow (how it works)

> Router terminal: **time_limit** (incumbent → report+gap · no sol → report slow). See `router/README.md`.

"Slow" = a solve that reaches the time limit without a proven optimum. It is **not**
a relax/reformulate engine like infeasible: it reports what the solver already found
and — under `decide_on_timeout` — offers to keep solving on the **same warm solver**
(the search resumes, it never restarts), returning the best-so-far rows when the user
stops. No query is edited or rerun.

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
Helpers (`PeakProcessMemoryBytes`, `FormatMemory`, `FormatDuration`,
`PrintDecideTimeoutReport`) and the terminal split live in
`src/execution/operator/decide/physical_decide.cpp`.

## Relation parity with unbounded / infeasible

The stderr report is a one-shot text block; before this, a timed-out solve left
`decide_diagnostics()` **empty** and its error carried **no pointer** — unlike the
unbounded / infeasible terminals, which stash a structured diagnosis and end their error
with `Details: SELECT * FROM decide_diagnostics();`. Slow now matches them on the paths
where the solve **fails** (ends the query with an error):

- **`BuildTimeoutDiagnostic`** (`physical_decide.cpp`) builds a `state='slow'`
  `DecideDiagnostic` — the same facts as the report, as one model-level EAV block
  (`subject_kind='model'`): `status` (`solution_found` / `no_solution`), and when an
  incumbent exists `best_objective` + `within_percent_of_best` (the `gap` fraction as a
  `%` string, matching the report's "within X%"); `best_possible_objective` (the solver's
  bound) **only when the query has an objective** (`decide_objective` set or a composed
  MIN/MAX objective) — a pure-feasibility DECIDE has no objective, so the bound is a
  trivial 0 and is suppressed; always `elapsed`, `peak_memory`. User voice only — the
  attribute names avoid the forbidden jargon (`best_possible_objective`, not "bound").
- **Two fail exits stash + point:** the `error`-mode exit and the ask/continue
  no-incumbent stop now `StashDecideDiagnostic` and throw via `ThrowDecideDiagnosisReady`
  (headline `DECIDE optimization is slow: <summary> … Details: SELECT * FROM
  decide_diagnostics();`) instead of the pointer-less `ThrowDecideSolveError`. The summary
  names the situation and the next step (reduce input / loosen / `SET
  decide_on_timeout='continue'`), mirroring the unbounded terminal's "name it + prescribe
  the fix" shape.

**Scope — diagnostics-only, `auto`-gated.** The `TIME_LIMIT` terminal is reached only
under `diagnose_decide='auto'`; `off` still routes to `UNDIAGNOSED` → the plain
`ThrowDecideSolveError` (unchanged, no pointer, empty relation). The solve, the
`ask`/`continue`/`error` mode flow, the interactive loop, the report block, and the
success/incumbent-return path are untouched. A **caveated success** (an unproven incumbent
returned as *rows*) still does not populate the relation — that would touch the
success-delivery / `ClearDecideDiagnostic` path, so it is deferred (see `todo.md`). Tests:
`TestSlowDiagnosticsRelation` (path 1 populates + points; path 2 marks `no_solution`;
`off` leaves it empty; both backends).

## Warm continuation (S3) + stop delivery (S4)

The checkpoint report is not a dead end. Under `diagnose_decide='auto'` the TIME_LIMIT
terminal runs an in-operator loop that, per the `decide_on_timeout` pragma, either
stops or resumes the **live** solver for another chunk. `off` is a master mute (the
router never routes TIME_LIMIT to this terminal when diagnosis isn't armed), so it
still gives the plain static error — `decide_on_timeout` only takes effect under `auto`.

**The pragma `decide_on_timeout` (default `ask`)** — registered next to `diagnose_decide`
in `RegisterDecideDiagnosticOptions`, read via `GetDecideOnTimeoutMode`:

- **`ask`** — at each chunk boundary print the report, then, at an interactive terminal,
  prompt: *Enter* resumes another chunk on the warm solver, `s` (or EOF) stops. When
  stdin is **not** a terminal (`isatty(STDIN_FILENO)` false — tests, benchmarks, `-c`
  pipes, the C-API), `ask` **falls back to `error`**: it never blocks on a prompt no one
  can answer. This fallback is why the existing report tests (which pipe stdin) still see
  report-then-error.
- **`error`** — print the report, then error. Never returns the incumbent, even when one
  exists (distinct from an `ask` stop). This is today's report-then-error behavior made
  explicit, and the non-TTY `ask` fallback target.
- **`continue`** — auto-resume every chunk with no prompt until the solver finishes on
  its own (proven optimum → success rows; a definitive infeasible/unbounded → error).
  The query-interrupt flag (`ClientContext::interrupted`, set by Ctrl-C) is polled at
  each chunk boundary **and, on Gurobi, mid-chunk** (see below). On a break the interrupt
  is cleared so the incumbent rows still flow instead of aborting the query.

### Mid-chunk Ctrl-C (Gurobi)

Boundary-only polling means a Ctrl-C waits out the rest of the current chunk (up to a full
`ResolveDecideTimeLimit()` late). To cut a running chunk short the instant it is interrupted:

- **`SolverSession::SetInterruptPoll(std::function<bool()>)`** (`solver_session.hpp`) — the
  operator installs a poll reading `context.interrupted` once, right before the continuation
  loop, **scoped to `continue`** (`physical_decide.cpp`). `ask` is left untouched: it already
  stops the user at each prompt, and — found empirically — a watcher thread contending with the
  interactive `getline` destabilizes the prompt, so `ask` keeps the boundary-only path.
- **`GurobiSession`** (`gurobi_solver.cpp`) — while `optimize()` blocks, a watcher `std::thread`
  polls every 25 ms and calls **`GRBterminate()`** (thread-safe) when the poll trips; the thread
  is joined right after `optimize()` returns. `optimize()` then reports `GRB_INTERRUPTED`, which
  is mapped to `SolverStatus::TIME_LIMIT` (same incumbent readback) — so the chunk returns its
  best-so-far exactly like a natural limit stop, and the existing boundary check breaks and
  delivers the rows with **zero operator-logic change** beyond installing the poll. Measured
  latency: ~0.06 s (vs. waiting out the chunk).
- **`GRBterminate` is loaded best-effort** (optional symbol, like `GRBaddqconstr`); if absent
  the watcher is never spawned and behavior degrades to boundary-only. **HiGHS** does not
  override `SetInterruptPoll` (no thread-safe terminate), so it stays boundary-only — the
  solver-agnostic fallback. The **first chunk** (inside `SolveModel`, before the poll is
  installed) is not yet interruptible; only continuation chunks are (`todo.md`).
- **No regression surface:** the poll is installed only on the `continue` retained session, so
  every other solve — the first chunk, `ask`, `error`, normal solves, and all diagnostic
  re-solves (`SolvePreparedModel`) — spawns no watcher and runs byte-for-byte as before.

**Every continue restarts the timer.** Each chunk is a fresh `ResolveDecideTimeLimit()`
budget; the operator tracks *cumulative* elapsed and re-prints the report every boundary
(elapsed climbs 1s → 2s → 3s …). Both backends were probed to honor a **per-call** limit
and to **resume** (not restart) the MIP search after a time-limit stop, so `Continue()`
just re-runs with a fresh chunk — no incumbent re-seeding.

**Stop delivery (S4).** On a stop with an incumbent (`ask` `s` / `continue` Ctrl-C) or a
proven optimum, control falls through to the shared success stores
(`gstate.ilp_solution = move(solve_result.solution)` → `GetData` streams rows). A
stop-with-incumbent-but-not-proven-optimum additionally prints a plain **stderr** caveat
("returning the best solution found so far — NOT proven the best possible"); there is no
live SQL NOTICE channel (`Connection::warning_cb` is a dormant stub), so the caveat rides
on stderr with the report. A stop with **no** incumbent throws the existing timeout error.

**Warm-resumable solver session.** The continuation needs the live solver to survive
across chunks, so the single-shot backend `Solve(model)` was refactored into a
`SolverSession` (`src/include/duckdb/decidb/solver_session.hpp`): `Load(model)` builds the
backend handle once (now a member — Gurobi env+model / the `Highs` object), and
`RunAndReadback(chunk_seconds)` sets the per-chunk limit and (re-)optimizes. `Solve()` =
`Load` + one `RunAndReadback`; `Continue()` = another `RunAndReadback` on the same warm
handle. `SolveModel` threads the first-chunk limit via `SolveModelOptions.time_limit_seconds`
and hands the live session back through a `retained_session` out-param (mirroring
`retained_model`); the operator holds it for the loop. The old static `GurobiSolver::Solve`
/ `DeterministicNaive::Solve` remain as thin wrappers, so every diagnostic re-solve is
untouched. Backend sessions: `GurobiSession` (`gurobi_solver.cpp`, sets `TimeLimit` on the
live model env via `getenv_model`), `HighsSession` (`deterministic_naive.cpp`).

## Tests

`test/decide/tests/test_query_diagnostics_slow.py` (both backends). Report block
(`TestSlowCheckpointReport`): a 15-D knapsack (x=0 always feasible, optimum not provable
in ~1s) drives path 1; a seeded random market-split (feasibility-hard, no incumbent
within the limit) drives path 2; `off` is asserted to suppress the report; the report is
checked free of solver jargon.

Continuation (`TestSlowContinuation`): interactive `ask` is driven through a
pseudo-terminal (`DecidBCli.execute_interactive`, so `isatty` is true) — `s` stops and
returns incumbent rows + caveat, `\n\ns\n` shows the report repeating with rising
cumulative elapsed, EOF stops. `ask` over piped stdin is asserted to never prompt and to
error; `error` mode reports-then-errors even with an incumbent; `continue` is bounded by a
SIGINT (asserting it looped and then returned rows + caveat, rc 0) rather than a fragile
"finish in N chunks"; `off` + `continue` is asserted to stay a plain error with no report.
Continue-to-proven-optimum correctness is covered by manual/oracle checks (the timing
window that both backends time-out-then-finish quickly is too narrow for a stable CI
assertion). Instances are sized with margin so the asserted branch is stable across
machines.

