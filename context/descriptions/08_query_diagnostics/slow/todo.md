# Query Diagnostics — Slow (planned)

**Concretized design (2026-07-02).** "Slow" = a solve that reaches
`DECIDB_TIME_LIMIT` without a proven optimum. It is **not** a relax/reformulate
engine like infeasible. It does exactly two things: **report what the solver
already found**, and **offer to keep going on the same warm solver** — no rerun,
no query edit. The objective→constraint "anytime ladder" (old S4) is **dropped**:
modern MIP solvers already tighten their own bound internally, so it was
reinventing the solver. See "Superseded" at the bottom.

Two paths, split on whether a feasible solution was found
(Gurobi `SolCount > 0` / HiGHS `primal_solution_status == 2`):

1. **Solution found.** Tell the user a usable solution exists and how close it is
   to the best possible; offer to keep improving it. On stop, the statement
   **succeeds and returns the best-so-far rows** with a NOTICE that they aren't
   proven best.
2. **No solution yet.** Tell the user nothing was found yet; offer to keep
   searching. On stop, the statement **errors** (the existing timeout error) —
   there is nothing to return.

Both paths report **elapsed solve wall-clock** and **peak process memory**.
Continuation is an **in-operator loop**: the DECIDE physical operator owns the
live solver, so on timeout it prints the status, reads one line from stdin, and
either raises the limit and re-solves the *same* model (warm start is automatic —
the solver never leaves scope) or returns. The checkpoint is the time limit
itself; pressing Ctrl-C to checkpoint *early* is deferred (needs signal handling
that fights DuckDB's own Ctrl-C).

> Both backends honor a shared time limit (300s default, `DECIDB_TIME_LIMIT`
> override) via `ResolveDecideTimeLimit()` in `solver_config.hpp` — see `done.md`.
>
> **S0–S2 shipped (2026-07-02).** The HiGHS `kWarning`-at-timeout bug is fixed, both
> backends populate the incumbent / best-bound / gap fields on `SolverResult` at the
> limit, and the checkpoint status block prints on `TIME_LIMIT` under `auto` (info
> only; the statement still errors after it). Remaining work below is the interactive
> loop (S3), the success-with-notice result on stop (S4), and router wiring (S5). See
> `done.md` for the shipped behavior and `foundations/done.md` for the field table.

## Messages (draft — SQL-user voice, no solver jargon)

Status to stderr; the decision line reads from stdin. "incumbent" / "gap" /
"optimal" are solver words — avoid them.

Path 1 (solution found):
```
DECIDE hit the 300s time limit with a usable solution (not proven best).
  best objective so far: 147235  (within 0.09% of the best possible)
  elapsed 300s · peak memory 1.2 GB
Keep improving it?  [Enter] continue +300s   ·   type s + Enter to stop and take this solution
```

Path 2 (no solution):
```
DECIDE hit the 300s time limit without finding a solution yet.
  elapsed 300s · peak memory 1.2 GB
The problem is very hard (or has no solution). Keep searching?  [Enter] continue +300s   ·   type s + Enter to give up
```

## Checklist

- [x] **S0 · Fix HiGHS `kWarning`-at-timeout** — done (`07_issues/bugs/done.md`, `done.md`)
- [x] **S1 · `SolverResult` timeout fields + populate** — done (`foundations/done.md`, `done.md`)
- [x] **S2 · Checkpoint report (status block + memory + elapsed)** — done (`done.md`)
- [ ] **S3 · In-operator continuation loop (stdin prompt, warm re-solve, fallback)** — deps: S2
- [ ] **S4 · Result on stop (rows+NOTICE vs error)** — deps: S3
- [ ] **S5 · Router R6 wiring + tests** — deps: S4

---

## S3 · In-operator continuation loop (stdin prompt, warm re-solve, fallback)

- **What:** after printing the status, if interactive, read one line; on continue
  raise the limit and re-solve the **same** model (warm), then re-check → loop; on
  stop, break. Keep the solver object alive across iterations inside the operator.
- **Non-interactive fallback (must-have):** if stdin is not a TTY (differential
  tests, benchmarks, C-API), **never prompt** — behave deterministically (return best
  solution if present, else error). Add `PRAGMA decide_on_timeout = ask | take |
  error` to pin behavior regardless of TTY; default `ask` interactive, `take`
  otherwise.
- 🔬 **Probe (implementation):** reading stdin from inside a query operator — the
  operator may run on a worker thread; confirm it doesn't fight the CLI's line editor
  or hang under `.read` / piped input. This is the main risk in the whole feature.
- 🔬 **Probe (implementation):** warm-resume semantics — does re-calling
  `optimize()` / `run()` on the same model resume the search or restart it? If it
  restarts, seed with the saved incumbent (Gurobi `Start` attr / HiGHS `setSolution`)
  so no work is lost.
- **Pointers:** `physical_decide.cpp`; backend `Solve()` (needs a re-entrant / higher
  time-limit path, or a persistent solver handle).
- **Test:** scripted stdin ("s\n" → stop; "\n\n s\n" → two continues then stop) on a
  hard MILP; assert the loop count and that non-TTY never prompts.
- **Done section:** `slow/done.md`.

## S4 · Result on stop (rows + NOTICE vs error)

- **What:** on stop with a solution → the statement **succeeds**, returns the
  best-so-far rows, and emits a NOTICE that they are not proven best. On stop with no
  solution → throw the existing timeout error (no rows).
- **Decision (settled):** a SQL statement can't both error and return rows, so
  "return the rows" = success-with-notice. This is a behavior change: today every
  timeout is an error.
- **Pointers:** `physical_decide.cpp` result-delivery; `SolverResult.solution`.
- **Test:** stop-with-solution returns the incumbent rows + notice; stop-without
  errors. Differential test vs `oracle_solver` on the *continued-to-completion* case
  (must match the true optimum).
- **Done section:** `slow/done.md`.

## S5 · Router R6 wiring + tests

- **What:** wire the router's `time_limit` terminal to this engine (`router/todo.md`
  R6): solution-found vs no-solution split; extend the router unit tests (R7).
- **Pointers:** `decide_router.cpp`, `test/common/test_decidb_router.cpp`.
- **Done section:** `router/done.md` ("Terminals: time_limit") + fold tests into "Tests."

---

## Superseded / deferred (out of v1 scope)

- **Old S4 — anytime objective→constraint ladder: dropped.** It replaced
  `maximize f` with a warm-started `f ≥ b` binary search to force early feasible
  solutions. Cut because modern MIP solvers already tighten their own dual bound and
  emit anytime incumbents internally — the ladder duplicated solver behavior for no
  gain. "Continue" is simply more wall-clock on the warm model.
- **Bucket B elastic-as-classifier: deferred.** Running the elastic engine on a
  no-solution timeout to decide "hard vs infeasible" is a nice future refinement, but
  v1 just reports "no solution yet" and offers to continue (per the agreed design).
- **Diverging-bound → unbounded hand-off: deferred.** An unbounded MILP that reaches
  the limit (incumbent + bound running to ∞) could route to `unbounded/`. Rare
  (unbounded is normally caught pre-timeout via ray extraction); revisit if it shows up.
- **Early Ctrl-C checkpoint: deferred.** Interrupting the solve *before* the time
  limit needs a solver-callback interrupt (Gurobi `GRBterminate` / HiGHS
  `setCallback`+`startCallback`, both confirmed to exist) plus coordination with
  DuckDB's own SIGINT handler. v1 checkpoints only at the time limit.
- **No ETA, ever.** MILP exposes no honest time-remaining (gap closes
  non-monotonically; node counts give no fraction-done). Report elapsed + closeness
  as facts; never predict a finish time.
