# S1 (top-k and cardinality intervals) — open work

What S1 does today is in `done.md`. The class definition is in `definition.md`. Each task is independent unless it says
otherwise. When one ships, its result moves to `done.md` and the task is deleted from here. Estimates are rough guesses,
not measurements. How to run the checks is in `../_harness/todo.md`; read its "How to work any task" section first.

The class definition is met. This layer stays lean (`../../architecture/rules.md`), so a deferred task comes off the list
only when there is evidence a user needs it.

## S1-12 — S1 reads a per-row bound on `x` in its own way, and so does A1

- **Not scheduled.** Do not start before a third rule needs per-row bounds (A3, A5, A6): S1 is heavily tested, so changing
  its pins has to earn its risk.
- **Problem.** S1 decides "which of {0, 1} may this row take" for a per-row bound on `x` with its own code (`PinDomain`,
  `ConstantPinDomain`, `SourceBooleanPinValue`, and the numeric-pin code in `ProvePins`, about 140 lines). A1 decides
  "what interval may this row take" for the same kind of bound with different code (`ClauseBounds` in `a1_rule.cpp`).
  They also treat the solver's reading of a bare constant bound differently: S1 declines some spellings by inspecting
  them, A1 asks the `variable_bound` fact, which S1 never reads.
- **Why it matters.** Two readings of one rule can drift apart, and a third per-row rule would write a third.
- **Plan.** Move A1's per-row bound algebra to a shared builder that yields a lower and an upper bound per row; give S1's
  pins that builder with the domain {0, 1}; keep S1's rank and count logic as it is.
- **Done when.** S1's pin code is a call to the shared builder, S1's suites (three-way, contract, fuzz) pass unchanged, and
  the A1 suites still pass. About 1 day.

## S1-13 — Refresh S1's performance table

- **Problem.** The performance table in `done.md` was measured on 2026-10-05, and `done.md` says the cost of the per-row
  NULL message on clean data is not measured. The 15 to 100 times speedups are part of the reason `auto` is the default.
- **Plan.** After `make release`, on a quiet machine with a Gurobi licence, run the sweep under "Reproduce" in `done.md`
  (about 4.5 minutes; it builds the 5,000,000-row and wide databases, about 3 GiB), plus one run with and without NULLs in
  the score column to measure the message cost. Rewrite the table and its date.
- **Done when.** `done.md` has numbers from a run you made, with their date, and says what the NULL message costs. Machine
  load changes timings, so run nothing else meanwhile.

## Deferred

| Task | Why deferred |
|---|---|
| S1-08, other aggregates on the right of a count bound | No evidence anyone writes these. Each reducer needs its NULL, empty-group and typing behavior checked on the solver first, and the solver already handles the shape correctly. |
| S1-04, wider keyed application | No known missing shape: `PER` on an expression is a parser error today. |

### S1-08 — Other aggregates on the right-hand side of a count bound

- **Problem.** Only a plain `COUNT(*)` is admitted on the right. The solver also accepts `AVG`, `SUM`, `MIN`, `MAX` and
  `COUNT(col)` of data columns, evaluated to one value per group (`03_expressivity/sql_functions/done.md`, "Reducers as a
  Bound"), and a right-hand aggregate with its own `WHEN`.
- **Solver behavior to keep in mind.** `COUNT(*)` follows the clause-level `WHEN` and `PER`; an aggregate-local `WHEN` on the
  left does not narrow it. A trailing `WHEN` on a *bare* right-hand aggregate (`SUM(x) <= COUNT(*) WHEN active`) is
  aggregate-local to that aggregate: the count is over the active rows while the left side sums every row. The same `WHEN`
  on `COUNT(*) / 2` is clause-level. S1 declines the bare form today, so it cannot be misread.
- **Plan.** The count stage (`CountRows` in `s1_rule.cpp`) is one window per count flavor. Another reducer is one more window
  aggregate over the same partitions, with a `FILTER` for the right-hand `WHEN`. The bound expression and the per-group
  limit are reused. The bound is evaluated with DuckDB's own operators, so an overflow raises as on the solver path.
- **Not known.** How the solver treats NULLs inside `SUM`, `AVG`, `MIN` and `MAX`; an empty group; DECIMAL and integer
  typing of `SUM(col)`; and relation-qualified reducers (`SUM(D: cost)`), which stay out. Check each on the solver before
  coding.
- **Done when.** Each admitted reducer matches the solver for every comparison and scope, and the ones left on the solver
  have a named reason. About 1 day. Worth doing if users write "at most the group's average" style bounds.

### S1-04 — Wider keyed application

- **Goal.** Extend past source-column `PER` with matching top-level or aggregate-local `WHEN` to other keyed shapes.
- **Done when.** Component independence and row/entity identity are proved and tested for the new shape. Estimates never
  establish independence. Independently scoped clauses, such as quotas on two overlapping subsets, are a separate class
  (see `../s2/todo.md`), not a wider S1 matcher.
- **Status.** Undecided. `PER` on an expression is a parser error today, so there is no S1 shape known to be missing.

## Exit gate for any S1 widening

An independent proof, behavior tests (the standard tests in `../../architecture/rules.md`), and end-to-end measurement that
does not count timeouts, solver limits or unsupported cases as wins. Finding a relational expression in the catalogue is
not enough.
