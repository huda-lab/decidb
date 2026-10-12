# S1 (top-k and cardinality intervals) — open work

What S1 does today is in `done.md`; the class is in `definition.md`. How to work and the checks are in
`../_harness/todo.md` § How to work any task. Every widening also meets the exit gate at the end of this file.

## S1-08 — Limits read from a data column through an aggregate

- **Problem.** A count bound's right side admits only plain `COUNT(*)`. The solver also accepts `MIN`, `MAX`, `SUM`, `AVG`
  and `COUNT(col)` of a data column, one value per group (`SUM(x) <= MIN(capacity) PER warehouse`). Those queries take
  the solver path.
- **Decision made.** Admit `MIN`, `MAX` and `COUNT(col)` of any numeric column, and `SUM` and `AVG` of integer columns. Keep
  on the solver: `SUM`/`AVG` of non-integer columns (the solver adds row by row in `DOUBLE`, a window may add in another
  order, and a last-digit difference can flip a floored limit), a `WHEN` on the right-hand aggregate alone, and
  relation-qualified reducers (`SUM(D: cost)`).
- **Solver behavior to match** (`EvaluateRhsReducerPerGroup`, `physical_decide.cpp`): values are read as `DOUBLE`;
  `NaN` raises and infinity is a value; an empty group raises; the reducer follows the clause's `WHEN` and `PER` but not a
  left-side aggregate-local `WHEN`; `AVG` is the `DOUBLE` sum over the count. Confirm on the CLI how a `NULL` in the
  column behaves and that integer sums past 2^53 are already refused, before coding.
- **Change.** The count stage (`CountRows` in `s1_rule.cpp`) adds one window aggregate per admitted reducer over the same
  partitions; `IsPlainCountStar`, `RowCountBoundProblem` and `ReplaceCountStar` widen to the admitted reducers. Reuse
  the existing source-bound `NULL`/`NaN` check and `DirectGuardEmptyAggregate` for the errors.
- **Done when.** Each admitted reducer matches the solver for `<=`, `>=`, `=`, with and without `PER` and `WHEN`, including
  the `NULL`, `NaN`, infinity and empty-group errors; each form kept on the solver has a declined-reason test.

## S1-14 — Queries with no objective

- **Problem.** A constraints-only query (no `MAXIMIZE`/`MINIMIZE`) is declined because the definition requires a linear
  objective (`done.md` § Explicitly out of scope).
- **Change.** Accept a missing objective in `Match` and treat every score as 0, so the plan picks only the rows a lower
  bound requires. Which rows are picked when scores tie is a decision: stop and report if the solver's choice cannot be
  matched or is unspecified.
- **Done when.** Constraints-only queries match the solver's objective value and feasibility under every count bound
  shape, with and without `PER` and `WHEN`.

## S1-15 — Offset or negated count body

- **Problem.** `SUM(x + c)` and `SUM(1 - x)` are declined (`done.md` § Explicitly out of scope). A constant term makes the
  bound depend on the group size (`count + n*c <= B`), and `1 - x` flips its direction. Users can write `SUM(x) >= n - k`
  instead.
- **Change.** Admit a body `a*x + c` with `a = ±1` and a constant `c`: shift the per-group limit by `n*c`, where `n` is
  the group's covered row count, and flip the side when `a = -1`.
- **Done when.** Both forms match the solver for every comparison, with and without `PER` and `WHEN`, including a `c`
  that makes the limit unreachable.

## S1-12 — S1 reads a per-row bound on `x` in its own way, and so does A1

- **Needs.** A third rule that reads per-row bounds (A3, A5 or A6). S1 is heavily tested, so changing its pins has to earn
  its risk.
- **Problem.** S1 decides "which of {0, 1} may this row take" for a per-row bound on `x` with its own code (`PinDomain`,
  `ConstantPinDomain`, `SourceBooleanPinValue`, and the numeric-pin code in `ProvePins`, about 140 lines). A1 decides
  "what interval may this row take" for the same kind of bound with different code (`ClauseBounds` in `a1_rule.cpp`).
  They also treat the solver's reading of a bare constant bound differently: S1 declines some spellings by inspecting
  them, A1 asks the `variable_bound` fact, which S1 never reads.
  Two readings of one rule can drift apart, and a third per-row rule would write a third.
- **Change.** Move A1's per-row bound algebra to a shared builder that yields a lower and an upper bound per row; give S1's
  pins that builder with the domain {0, 1}; keep S1's rank and count logic as it is.
- **Done when.** S1's pin code is a call to the shared builder, S1's suites (three-way, contract, fuzz) pass unchanged, and
  the A1 suites still pass. About 1 day.

## S1-13 — Refresh S1's performance table

- **Problem.** The performance table in `done.md` was measured on 2026-10-05, and `done.md` says the cost of the per-row
  NULL message on clean data is not measured. The 15 to 100 times speedups are part of the reason `auto` is the default.
- **Change.** After `make release`, on a quiet machine with a Gurobi licence, run the sweep under "Reproduce" in `done.md`
  (about 4.5 minutes; it builds the 5,000,000-row and wide databases, about 3 GiB), plus one run with and without NULLs in
  the score column to measure the message cost. Rewrite the table and its date.
- **Done when.** `done.md` has numbers from a run you made, with their date, and says what the NULL message costs. Machine
  load changes timings, so run nothing else meanwhile.

## Exit gate for any S1 widening

An independent proof, behavior tests (the standard tests in `../../architecture/rules.md`), and end-to-end measurement that
does not count timeouts, solver limits or unsupported cases as wins. Finding a relational expression in the catalogue is
not enough.
