# S1 (uniform-matroid optimization) — open work

What S1 does today is in `done.md`; the class is in `definition.md`. How to work and the checks are in
`../../framework/todo.md` § How to work any task. Every widening also meets the exit gate at the end of this file.

## S1-16 — Delete S1's own reading of a right-hand aggregate

- **Problem.** The shared right-hand-side rewrite hands S1 each `COUNT`, `MIN`, `MAX`, `SUM` and `AVG` limit as a source
  column, so S1's own code for aggregates on the right of a count bound can no longer run: `ReducerProblem`,
  `RowCountBoundProblem`, `IsSmallIntegerType`, `IsPlainCountStar`, `ReplaceAggregates`, `OnlyWhereCovered` and the reducer
  windows in `CountRows`, about 270 lines, with the `Condition::SourceBound` fields only they use. What S1 still meets is an
  aggregate the rewrite leaves on the solver's fold (an own `WHEN`, a qualified reducer), which S1 declines.
- **Change.** Delete that code. A right-hand aggregate in the IR is a decline with its reason.
- **Done when.** The S1 differential oracle, conformance and fuzz suites pass with no edit to a test file, no S1 code reads
  an aggregate node, and `done.md` states S1's decline for an aggregate with its own `WHEN`.

## S1-17 — Constant terms in the count body

- **Problem.** S1 admits `SUM(x + c)` and `SUM(c - x)` by shifting the limit by `n*c` per group (`ReadCountBody`,
  `MAX_OFFSET`, the `HUGEINT` widening in `AddBoundValidation`, the covered-row count in `CountRows`, about 90 lines).
  The class definition does not ask for it, and `done.md` records a known difference near the top of `HUGEINT`.
- **Change.** The shared canonicalizer rewrites `SUM(x + c) <op> B` into `SUM(x) <op> B - c*COUNT(*)` and
  `SUM(c - x) <op> B` into `SUM(x) <flipped op> c*COUNT(*) - B`, with the `COUNT(*)` filtered by the `SUM`'s own `WHEN`.
  The right-hand-side rewrite evaluates the shift for both paths. S1 deletes its offset code.
- **Done when.** The S1 suites pass, the solver path gives the same answers on these bodies, and `done.md` states how
  the known difference near the top of `HUGEINT` stands.

## S1-12 — S1 reads a per-row bound on `x` in its own way, and so does A1

- **Needs.** `../../framework/todo.md` § H-21.
- **Problem.** S1 decides "which of {0, 1} may this row take" for a per-row bound on `x` with its own code
  (`FixedDomain`, `ConstantFixedDomain`, `SourceBooleanFixedValue`, and the numeric fixed-value code in
  `ProveFixedVariables`, about 140 lines). A1 decides "what interval may this row take" for the same kind of bound
  through the shared bound reader (`DirectReadBounds`, `DirectClauseBounds`). They also treat the solver's reading of a
  bare constant bound differently: S1 declines some spellings by inspecting them, A1 asks the `variable_bound` IR flag,
  which S1 never reads. Two readings of one bound can drift apart.
- **Change.** Give S1's fixed values the shared reader with the domain {0, 1}; keep S1's rank and count logic as it is.
- **Done when.** S1's fixed-value code is a call to the shared reader, S1's suites (differential oracle, conformance,
  fuzz) pass unchanged, and the A1 suites still pass.

## S1-18 — S1 builds its expressions with `ExpressionBuilder`

- **Needs.** `../../framework/todo.md` § H-25, and S1-16 so deleted code is not converted.
- **Problem.** S1 builds every expression by hand (`make_uniq<Bound...>`, 51 times in the rule and 17 in its helpers)
  and never uses `ExpressionBuilder`; `AnyCondition` copies `ExpressionBuilder::Any`. `SourceLimit` takes about 30
  lines for what the builder writes in about 8.
- **Change.** Convert `s1_uniform_matroid_rule.cpp` and `s1_uniform_matroid_helpers.cpp` to the builder and delete
  `AnyCondition`.
- **Done when.** The S1 suites pass with no edit to a test file, and a hand-built expression remains only where the
  builder has no operation for it.

## Exit gate for any S1 widening

An independent proof, behavior tests (the standard tests in `../../architecture/rules.md`), and end-to-end measurement that
does not count timeouts, solver limits or unsupported cases as wins. Finding a relational expression in the catalogue is
not enough.
