# Query Diagnostics — Slow (how it works)

> Router terminal: **time_limit** (incumbent → report+gap · no sol → report slow). See `router/README.md`.

"Slow" = a solve stopped before a proven optimum — the wall-clock **time limit** expiring,
or a **user Ctrl-C** on any armed solve (a peer trigger, decoupled from the time limit; see
"Ctrl-C as a peer trigger"). It is **not** a relax/reformulate engine like infeasible: it
reports what the solver already found and — under `decide_on_timeout` — offers to keep solving
on the **same warm solver** (the search resumes, it never restarts), returning the best-so-far
rows when the user stops. No query is edited or rerun.

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
  the `-1e100` / `inf` / `nan` no-incumbent sentinels never leak; `best_bound` / `gap`
  default to **NaN ("unavailable")** and are populated only when a proven bound exists —
  MIP timeouts. On LP/QP timeouts (where Gurobi's `ObjBound` is the ±1e100 sentinel and
  HiGHS's `mip_dual_bound` is a 0 default) they stay NaN, and every report/relation
  writer skips the bound/closeness output when `!std::isfinite`. Field-by-field mapping
  and the "gap is a fraction on both backends" note are in `foundations/done.md`
  (Structured solver result / Time-limit behavior).

## Checkpoint report (S2)

On a `TIME_LIMIT` result, `PhysicalDecide::Finalize` routes through
`RouteSolveResult` to the `TIME_LIMIT` terminal (under `auto`; `off` still gets the
plain static error). The terminal prints a plain-language status block to **stderr**
and then follows the `decide_on_timeout` policy described below: `error` throws the
slow-diagnosis pointer, interactive `ask` can resume or stop, and `continue` resumes
automatically until a final outcome or user interrupt. The status block has two shapes,
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

**Timeout vs. interrupt wording.** The two stop causes share one best-so-far readback but
must not read the same to the user. When the stop was a user Ctrl-C
(`SolverResult::user_interrupted`, Gurobi only), the report opener becomes **"DECIDE stopped
at your request …"** instead of **"DECIDE hit the 300s time limit …"** (both the
solution-found and no-solution shapes); everything else (closeness, elapsed, peak memory) is
identical. HiGHS never sets the flag, so its boundary-only interrupt keeps the time-limit
wording.

Details:

- **Voice.** Obeys the user-output rule — no "incumbent"/"gap"/"bound" solver words.
  The `gap` fraction is rendered as a percentage ("within 0.09%"). When `gap` is NaN
  (no proven bound — LP/QP timeouts), the path-1 line drops the parenthetical and
  prints only "best objective so far: N" — the report never claims closeness it
  cannot prove.
- **Elapsed** is measured by an always-on `Profiler` wrapped around the `SolveModel`
  call (independent of `DECIDB_BENCH`), reported via `FormatDuration`: integer seconds
  when whole (`300s`), one decimal for larger fractional values (`1.5s`), and two
  significant figures for sub-second values so a small limit keeps its digits (`0.05s`,
  `elapsed 0.056s`) instead of rounding up to a misleading `0.1s`.
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
  (`subject_kind='model'`): `stopped_by` (`user_interrupt` / `time_limit`), `status`
  (`solution_found` / `no_solution`), and when an incumbent exists `best_objective` +
  `within_percent_of_best` (the `gap` fraction as a `%` string, matching the report's "within
  X%" — omitted when `gap` is NaN, i.e. no proven bound); `best_possible_objective` (the
  solver's bound) **only when the query has an objective** (`decide_objective` set or a
  composed MIN/MAX objective) **and the bound is finite** — a pure-feasibility DECIDE has no
  objective and an LP/QP timeout has no proven bound (NaN), so both are suppressed; always
  `elapsed`, `peak_memory`.
  User voice only — the attribute names avoid the forbidden jargon (`best_possible_objective`,
  not "bound"). The `summary` also has an interrupt variant ("the solve was still improving when
  you stopped it …" / "you stopped the solve before it found a solution …"), phrased to stay
  coherent after the "DECIDE optimization is slow:" error headline.
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
returned as *rows*) also populates the relation: the stop-with-incumbent path stashes the
same `BuildTimeoutDiagnostic` quality block and flags it (`keep_slow_diagnosis`,
`physical_decide.cpp`) so the success epilogue's blanket `ClearDecideDiagnostic` spares
it — after the rows return, `SELECT * FROM decide_diagnostics()` still answers "how good
is the solution I got" (the stderr caveat is one-shot). Tests:
`TestSlowDiagnosticsRelation` (path 1 populates + points; path 2 marks `no_solution`;
`off` leaves it empty; both backends) and `TestSlowContinuation::
test_caveated_success_populates_relation`.

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

### Ctrl-C as a peer trigger (Gurobi) — first solve + continuation

Ctrl-C is an **independent trigger into the slow branch**, decoupled from the time limit: a
user interrupt on *any* armed solve — the **first (normal) solve**, not just a continuation
chunk after a timeout — cuts it short and routes into the `TIME_LIMIT` terminal with the
best-so-far, instead of aborting the query. On Gurobi it also stops the running solve the
instant it is interrupted (rather than waiting out the chunk).

- **`SolveModelOptions::interrupt_poll`** (`ilp_solver.hpp`) — the operator arms it whenever
  diagnosis is armed (`diagnosis_armed`, in `physical_decide.cpp`), decoupled from
  `decide_on_timeout` / the retained session, reading `context.interrupted`. `SolveModel`
  installs it on the session **before the first `Solve`** (`ilp_solver.cpp`), so the initial
  solve is interruptible. The poll is a session member, so it **persists into every
  `Continue()`** when the session is retained — `continue` keeps its mid-chunk interrupt for
  free.
- **`SolverSession::SetInterruptPoll(std::function<bool()>)`** (`solver_session.hpp`) — the
  install point. In the continuation loop, `ask` **resets it to boundary-only**
  (`SetInterruptPoll({})`, `physical_decide.cpp`): `ask` already stops the user at each prompt,
  and — found empirically — a watcher thread contending with the interactive `getline`
  destabilizes the prompt. (The entry interrupt already fired *before* the loop, so first-solve
  Ctrl-C still routes in; only *within* the `ask` loop is it boundary-only.) `continue` leaves
  the inherited poll in place.
- **`GurobiSession`** (`gurobi_solver.cpp`) — while `optimize()` blocks, a watcher `std::thread`
  polls every 25 ms and calls **`GRBterminate()`** (thread-safe) when the poll trips; the thread
  is joined right after `optimize()` returns. `optimize()` then reports `GRB_INTERRUPTED`, which
  sets `SolverResult::user_interrupted` and maps to `SolverStatus::TIME_LIMIT` (same incumbent
  readback) — so the solve returns its best-so-far exactly like a natural limit stop and the
  existing routing/loop logic is unchanged, while the flag lets the report distinguish "you
  stopped it" from "the clock ran out." Measured latency: ~0.06 s (vs. waiting out the chunk).
- **The rare OPTIMAL-as-Ctrl-C-lands race:** if the watcher terminates just as Gurobi proves
  the optimum, the solve returns `OPTIMAL` with `context.interrupted` still set — which would
  make the executor abort a query that actually succeeded. The operator clears
  `context.interrupted` on the SOLVED path too (guarded on the poll being armed), mirroring the
  clear the `TIME_LIMIT` terminal already does on its incumbent-stop path.
- **Solver-agnostic fallback — HiGHS is boundary-only.** `GRBterminate` is loaded best-effort
  (optional symbol, like `GRBaddqconstr`); if absent the watcher is never spawned. **HiGHS** does
  not override `SetInterruptPoll` (no thread-safe terminate), so it **ignores the poll during a
  solve**: a mid-first-solve Ctrl-C is not seen until the chunk boundary (its own time limit),
  and the stop is then reported as a **time-limit** stop (`user_interrupted` stays false), yet
  the incumbent is still delivered at the boundary. Probed 2026-07-04: a 0.7 s signal into a 2 s
  chunk waits out the chunk, then returns rows with the "time limit" wording.
- **Watcher cost.** Because the poll is armed on every diagnosed solve (default `auto`), each
  such solve now spawns the watcher `std::thread` for the duration of `optimize()` and joins it
  immediately after — negligible (a 25 ms-polling sleeper), and the price of Ctrl-C working on
  any query. Solves with diagnosis `off` are **not** armed and spawn no watcher. Internal
  diagnostic re-solves (`SolvePreparedModel`, used by the `INF_OR_UNBD` probe, unbounded ray
  fallback, and infeasible elastic passes) reuse the same interrupt poll when diagnosis is armed,
  but with their own smaller helper budget (`min(60s, primary solve limit)`).

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

Timeout honesty (`TestNonMipTimeoutHonesty`): a 200k-var LP (both backends) and a
convex QP (Gurobi-only) at `DECIDB_TIME_LIMIT=0.001` assert that no proven-bound output
is fabricated — no "within …% of the best possible" line, no `best_possible_objective` /
`within_percent_of_best` relation rows, and no `1e+100` / `nan` in the output — while a
MIP-timeout regression case asserts the real bound + closeness still appear.

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

First-solve Ctrl-C (`TestSlowFirstSolveInterrupt`): a **generous** `DECIDB_TIME_LIMIT` (20s)
keeps the first boundary far away, so a fast stop after a ~1s SIGINT proves the *first solve
itself* was interrupted mid-solve (a boundary-only path would wait out ~19s). Gurobi-only
assertions: `continue` mode returns the incumbent with the **"stopped at your request"**
wording (not "time limit") + caveat, rc 0, latency < 5s; default (ask→error over a pipe)
reports the interrupt then errors via `decide_diagnostics()` without delivering rows; the
stashed relation records `stopped_by=user_interrupt` + `status=solution_found`. HiGHS is
asserted **boundary-only** (probed 2026-07-04): a mid-first-solve SIGINT is reported as a
time-limit stop (never "stopped at your request") yet still delivers the incumbent at the
boundary — the solver-agnostic fallback.
