# Slow solves — the time limit, and Ctrl-C (how it works)

> Stage 08 (physical execution). **Not a diagnosis.** This used to be a router terminal
> under `07_query_diagnostics/slow/`; batch H moved it here, because a solve that runs
> out of time is ordinary execution behaviour: it happens with or without the `DIAGNOSE`
> prefix, no engine runs, and nothing is diagnosed.

"Slow" = a solve stopped before a proven optimum — the wall-clock **time limit** expiring,
or a **user Ctrl-C** (a peer trigger, decoupled from the time limit; see "Ctrl-C as a peer
trigger"). It reports what the solver already found and offers to keep solving on the
**same warm solver** (the search resumes, it never restarts), returning the best-so-far
rows when the user stops. No query is edited or rerun.

**Who decides what happens next is fixed by who is there to answer, not by a setting.**
`decide_on_timeout` was deleted in batch H along with `diagnose_decide`. The rule is now:

| situation                          | what happens                                                        |
| ---------------------------------- | ------------------------------------------------------------------- |
| clock ran out, at a terminal       | print the report, then offer to keep solving or take the best so far |
| clock ran out, anywhere else       | print the report, then error — nobody can answer the offer            |
| user pressed Ctrl-C                | print the report, hand back the best so far with its caveat — terminal or not; the decision was already made |
| user pressed Ctrl-C, nothing found | print the report, then error — there is nothing to hand back          |

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
  `kOk`) at the limit; `HighsSession::RunAndReadback` throws only on
  `HighsStatus::kError`, letting `kWarning` fall through to the `kTimeLimit →
  TIME_LIMIT` mapping. (Previously the `!= kOk` guard threw an INTERNAL error first —
  bug, since fixed.)
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

On a `TIME_LIMIT` result, `PhysicalDecide::Finalize` handles the stop **before the
router runs at all** — `RouteSolveResult` has no time-limit leaf. It prints a
plain-language status block to **stderr** and then applies the table above. The status
block has two shapes, split on `has_solution`:

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
must not read the same to the user. When the backend attributes the stop to a user Ctrl-C
(`SolverResult::user_interrupted`, Gurobi only), the report opener becomes **"DECIDE stopped
at your request …"** instead of **"DECIDE hit the 300s time limit …"** (both the
solution-found and no-solution shapes); everything else (closeness, elapsed, peak memory) is
identical. HiGHS never sets the flag, so its boundary-only interrupt keeps the time-limit
wording.

**A sibling's failure is neither.** A SELECT can hold several DECIDE clauses — two
subqueries joined side by side — and when one throws, DuckDB stops the remaining
pipelines by setting the very query interrupt Ctrl-C sets (`Executor::PushError`). Both
tests above read that interrupt, so a live sibling would report a cancellation nobody
asked for, printed *above* the error that actually stopped the query and carrying
elapsed/memory figures for a solve that was never the problem. So the very first thing
the `TIME_LIMIT` block does is ask `Executor::HasError()`: when the executor already
holds an error, nothing is printed and that error is re-raised. Only Gurobi ever
reproduced this — HiGHS has no thread-safe terminate, so its solve is never cut short
mid-search.

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

## No relation, by design

A timed-out solve used to stash a `state='slow'` diagnosis and point at
`decide_diagnostics()`. Batch H deleted both: `decide_diagnostics()` is gone, and a slow
solve is not one of the three states `DIAGNOSE` reports (`infeasible` / `unbounded` /
`feasible`). Everything the user learns about a slow solve is in the checkpoint report
printed above the rows — what was found, how close it is to the best possible, elapsed,
peak memory. `BuildTimeoutDiagnostic` and its three stash sites went with it.

Prefixing a slow query with `DIAGNOSE` changes nothing about the timeout: the same
report prints, the same offer is made, and if the solve eventually succeeds the prefix
reports `feasible`.

## Warm continuation (S3) + stop delivery (S4)

The checkpoint report is not a dead end. The operator runs an in-loop offer at each
chunk boundary, whenever there is someone to answer it — `isatty(STDIN_FILENO)`, which
IS the rule now rather than a fallback within a mode:

- **At a terminal** — at each chunk boundary print the report, then prompt: *Enter*
  resumes another chunk on the warm solver, `s` (or EOF) stops and takes the best so far.
- **Anywhere else** (tests, benchmarks, `-c` pipes, the C API) — print the report, then
  error. It never blocks on a prompt no one can answer, which is why the report tests
  (which pipe stdin) see report-then-error.
- **After Ctrl-C** — no prompt at all. The user already said stop, so the best-so-far is
  returned with its caveat and the query **succeeds**, terminal or not. With no
  incumbent, the plain timeout error fires. The interrupt flag is cleared on the way out
  so the rows flow instead of the executor aborting the query.

### Ctrl-C as a peer trigger (Gurobi) — first solve + continuation

Ctrl-C is an **independent trigger into the slow branch**, decoupled from the time limit: a
user interrupt on *any* solve — the **first (normal) solve**, not just a continuation
chunk after a timeout — cuts it short and hands back the best-so-far, instead of aborting
the query. On Gurobi it also stops the running solve the instant it is interrupted
(rather than waiting out the chunk).

- **`SolveModelOptions::interrupt_poll`** (`ilp_solver.hpp`) — the operator arms it
  **unconditionally** (`physical_decide.cpp`), reading `context.interrupted`. Before batch
  H it was armed only under `diagnose_decide='auto'`, so an interrupt on a query the user
  had not opted into aborted instead of returning the best-so-far. `SolveModel`
  installs it on the session **before the first `Solve`** (`ilp_solver.cpp`), so the initial
  solve is interruptible. The poll is a session member, so it **persists into every
  `Continue()`** when the session is retained — `continue` keeps its mid-chunk interrupt for
  free.
- **`SolverSession::SetInterruptPoll(std::function<bool()>)`** (`solver_session.hpp`) — the
  install point. The interactive loop **resets it to boundary-only**
  (`SetInterruptPoll({})`, `physical_decide.cpp`): the prompt already stops the user at each
  boundary, and — found empirically — a watcher thread contending with the interactive
  `getline` destabilizes the prompt. (The entry interrupt already fired *before* the loop,
  so a first-solve Ctrl-C still routes in; only *within* the prompt loop is it
  boundary-only.)
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
  chunk waits out the chunk, then returns rows with the "time limit" wording. The delivery
  works because the operator asks **two** questions, not one: the backend's own attribution
  (`user_interrupted`) OR the query interrupt (`context.interrupted`), which SIGINT sets on
  every backend. Reading only the first would have made Ctrl-C error out on HiGHS.
- **Watcher cost.** Because the poll is now armed on **every** solve, each spawns the watcher
  `std::thread` for the duration of `optimize()` and joins it immediately after — negligible
  (a 25 ms-polling sleeper), and the price of Ctrl-C working on any query. Internal
  diagnostic re-solves (`SolvePreparedModel`, used by the `INF_OR_UNBD` probe, unbounded ray
  fallback, and infeasible elastic passes) reuse the same interrupt poll, with their own
  smaller helper budget
  (`min(60s, primary solve limit)`).

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
`retained_model`); the operator holds it for the loop. A session is the *only* way a
DECIDE query reaches a backend — `GurobiSolver` and `DeterministicNaive` expose
`CreateSession()` and nothing else, so a diagnostic re-solve and a primary solve take the
same path. Backend sessions: `GurobiSession` (`gurobi_solver.cpp`, sets `TimeLimit` on the
live model env via `getenv_model`), `HighsSession` (`deterministic_naive.cpp`).

## Tests

`test/decide/tests/test_query_diagnostics_slow.py` (both backends). Report block
(`TestSlowCheckpointReport`): a 15-D knapsack (x=0 always feasible, optimum not provable
in ~1s) drives path 1; a seeded random market-split (feasibility-hard, no incumbent
within the limit) drives path 2; the report is checked free of solver jargon.

Timeout honesty (`TestNonMipTimeoutHonesty`): a 200k-var LP (both backends) and a
convex QP (Gurobi-only) at `DECIDB_TIME_LIMIT=0.001` assert that no proven-bound output
is fabricated — no "within …% of the best possible" line and no `1e+100` / `nan` in the
output — while a MIP-timeout regression case asserts the real bound + closeness still
appear.

Continuation (`TestSlowContinuation`): the interactive prompt is driven through a
pseudo-terminal (`DecidBCli.execute_interactive`, so `isatty` is true) — `s` stops and
returns incumbent rows + caveat, `\n\ns\n` shows the report repeating with rising
cumulative elapsed, EOF stops. Piped stdin is asserted to never prompt and to
report-then-error even with an incumbent in hand. A SIGINT over piped stdin is asserted
to deliver the incumbent + caveat with rc 0 — the "Ctrl-C needs no terminal" rule — and,
on Gurobi, to do so mid-solve (latency < 3s against a 6s chunk). Continue-to-proven-optimum
correctness is covered by manual/oracle checks (the timing window that both backends
time-out-then-finish quickly is too narrow for a stable CI assertion). Instances are
sized with margin so the asserted branch is stable across machines.

First-solve Ctrl-C (`TestSlowFirstSolveInterrupt`): a **generous** `DECIDB_TIME_LIMIT` (20s)
keeps the first boundary far away, so a fast stop after a ~1s SIGINT proves the *first solve
itself* was interrupted mid-solve (a boundary-only path would wait out ~19s). Gurobi-only
assertions: the incumbent comes back with the **"stopped at your request"** wording (not
"time limit") + caveat, rc 0, latency < 5s; an interrupt on the market-split (nothing found
yet) reports the interrupt and errors, delivering no rows. HiGHS is asserted
**boundary-only** (probed 2026-07-04): a mid-first-solve SIGINT is reported as a
time-limit stop (never "stopped at your request") yet still delivers the incumbent at the
boundary — the solver-agnostic fallback.

Sibling failure (`test_diagnose_trigger.py::TestASiblingFailureIsNotACancellation`,
both backends): a composed query joins a live 599-row knapsack to a tiny infeasible
DECIDE, and asserts the output names the infeasible clause with no "at your request"
wording and no elapsed/peak-memory tail. The healthy side is sized deliberately — with
two rows it finishes before its sibling fails and nothing is ever interrupted.
