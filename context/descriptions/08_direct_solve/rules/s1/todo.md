# S1 (top-k and cardinality intervals) — open work

What S1 does today is in `done.md`. The class definition is in `definition.md`. Each task is independent unless it says
otherwise. When one ships, its result moves to `done.md` and the task is deleted from here. Estimates are rough guesses,
not measurements.

**Suggested batches.** C: S1-07. S1-04 and S1-05 are not part of closing S1 and wait for a decision (see their entries).
Batches A (per-row bounds on `x`, out-of-scope tests) and B (scaled objective) shipped and are in `done.md`.

**Shared rule for S1-07.** The solver raises a NULL source-valued bound even on a row whose `WHEN` is false or
whose `PER` key is NULL, and an empty scoped aggregate can raise before that bound error. Every wider admission keeps that
error order. Anything not proved stays on the solver path. It touches the `Prove` function; the seeded differential
generator (`_direct_differential.py` via `_fuzz_query`) is extended to produce each form admitted.

## S1-07 — Bounds that count rows, such as `SUM(x) <= COUNT(*) / 2`

- **Meaning, checked on the solver 2026-10-05.** `COUNT(*)` counts the rows in scope: after the clause-level `WHEN`, within
  each `PER` group. A group of 5 rows with 3 active, bound `COUNT(*) / 2`: `PER dept` gives a cap of 2; `WHEN active PER dept`
  gives a cap of 1 (3 / 2 = 1.5), and the 2 inactive rows skip the bound and are picked if their score is positive.
- **Not yet checked.** `COUNT(*)` when the `WHEN` is aggregate-local (`SUM(x) FILTER ...`); other aggregates on the right
  (`AVG(cap)`, `SUM(col)`); and how the facts layer holds an aggregate in `DirectConstraintFact::rhs`. Read that first, since
  it decides the estimate.
- **Goal.** Admit a bound that is a decision-free expression of `COUNT(*)` and constants (`COUNT(*) / 2`, `COUNT(*) * 0.1`,
  `COUNT(*) - 1`). Other aggregates stay on the solver until checked the same way.
- **Plan.** The plan already counts the eligible rows per group in a window. Feed that count into the bound expression and
  reuse the per-group source-bound path (`SourceLimit`: rounding, strict and fractional limits, NULL checks).
- **Code.** `ProveBounds` in `s1_rule.cpp`; `DirectIsSourceOnlyNumeric` and `DirectFiniteFoldableDouble`
  (`direct_expression.cpp`); `DirectValidateBounds` (`direct_builder.cpp`).
- **Test.** Global, `PER`, `WHEN` and `WHEN` with `PER`; fractions that round both ways; an empty scope (the empty-aggregate
  error comes first); both solvers; oracle; fuzz generator.
- **Done when.** Results and errors match the solver, and the bounds that stay a miss have a named reason. About 1 day.

## S1-04 — Wider keyed application (was NEXT-03)

- **Goal.** Extend past source-column `PER` with matching top-level or aggregate-local `WHEN` to other keyed shapes.
- **Done when.** Component independence and row/entity identity are proved and tested for the new shape. Estimates never
  establish independence. Independently scoped clauses, such as quotas on two overlapping subsets, are a separate class
  (see `../s2/todo.md`), not a wider S1 matcher.
- **Status.** Undecided. `PER` on an expression is a parser error today, so there is no S1 shape known to be missing.

## S1-05 — Why returning wide rows is slow (was PERF-WIDE)

- **Problem.** On 5M rows with a 512-byte payload returned in full, the direct plan runs in about 0.5 s but the whole query
  takes about 2 s. The extra time is result collection: roughly three times what Gurobi spends. This limits the wide-row
  win to 1.4 to 1.8 times and is why 100,000 wide rows at trivial capacity were about 5% slower than Gurobi.
- **Already measured.** `EXPLAIN ANALYZE` of the window shows 1.74 s of operator time for the wide plan against 0.42 s
  pruned, summed over threads; the window's sort buffers hold about 2.8 GiB. Streaming a wide direct result cut the total
  from 5.6 s to 3.4 s in an earlier sweep.
- **Observation 2026-10-05.** The rank's sort-buffer counter now reads 5,626 MiB for the wide plan and 391 MiB for the
  pruned one, about double the 2,804 and 191 MiB recorded earlier, while peak process memory is unchanged (2.8 to 3.6 GiB).
  Check whether the counter or the plan changed before using it as evidence.
- **Not known.** Which step costs the time. Do not choose a fix until one measurement isolates the cause.
- **Experiments, in order.**
  1. `EXPLAIN ANALYZE` the full and the aggregate-only query on the same database; compare per-operator time.
  2. Hold rows at 5M and vary payload width (0, 64, 256, 512 bytes); plot collection cost against width.
  3. Run the same shape in plain DuckDB with no DECIDE (`ROW_NUMBER() OVER (ORDER BY score DESC)` plus the `CASE`). If it
     is just as slow, the cause is DuckDB's window operator.
  4. Vary threads (1, 4, 11). A cost that does not shrink with threads points at a serial gather.
  5. Diagnostic only: rank a narrow `(key, row id)` table and join back to the wide source.
- **Done when.** The cause is stated with the measurement that isolates it, and one decision is recorded: change the plan
  shape or accept the cost. Update the weak-spots table in `done.md`.

## Exit gate for any S1 widening

An independent proof, behavior tests (the standard tests in `../../architecture/rules.md`), and end-to-end measurement that
does not count timeouts, solver limits or unsupported cases as wins. Finding a relational expression in the catalogue is
not enough.
