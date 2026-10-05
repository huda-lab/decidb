# S1 (top-k and cardinality intervals) — open work

What S1 does today is in `done.md`. The class definition is in `definition.md`. Each task is independent. When one
ships, its result moves to `done.md` and the task is deleted from here.

**Shared rule for S1-01 to S1-03.** The solver raises a NULL source-valued bound even on a row whose `WHEN` is false or
whose `PER` key is NULL, and an empty scoped aggregate can raise before that bound error. Every wider admission keeps that
error order. Anything not proved stays on the solver path. These three touch the same `Prove` function; the seeded
differential generator (`_direct_differential.py` via `_fuzz_query`) should be extended to produce each form admitted.

## S1-01 — Numeric source-valued per-row pins (was NEXT-06)

- **Goal.** Admit `x <= pin_col` where `pin_col` is a numeric source column. Boolean-typed source pins and exact zero/one
  pins already work.
- **Code.** Pin handling in `s1_rule.cpp` (`Prove`).
- **Done when.** The bound and error semantics are proved from the complete plan, and tests cover global and grouped use,
  `WHEN` bypass, NULL pin values, and both solvers. Fixed status is never inferred from a sample.

## S1-02 — Throwing source expressions as bounds (was NEXT-07)

- **Goal.** Admit source-only bound expressions that can raise at runtime (a plain narrowing `CAST`). Today only nonthrowing
  ones such as `COALESCE` and `TRY_CAST` are admitted. A multi-term objective whose coefficient can throw also misses;
  handle it here or leave it, but say which.
- **Code.** `DirectMayThrow` (`direct_expression.cpp`) and `Prove` in `s1_rule.cpp`.
- **Done when.** The error and its order match the solver on every row, including rows that bypass the bound, and tests
  cover both solvers.

## S1-03 — Offset count bodies such as `SUM(x + 0)` (was NEXT-08)

- **Goal.** Decide whether `SUM(x + c)` can be admitted as a count with a shifted bound. Bodies whose terms add up to
  exactly one `x` (`SUM(1*x)`, `SUM(2*x - x)`) are admitted; a constant term misses.
- **Code.** `IsUnitContribution` in `s1_rule.cpp`.
- **Done when.** Either an exact proof and tests admit the form, or the near miss is pinned in a test as a permanent solver
  case with a reason.

## S1-04 — Wider keyed application (was NEXT-03)

- **Goal.** Extend past source-column `PER` with matching top-level or aggregate-local `WHEN` to other keyed shapes.
- **Done when.** Component independence and row/entity identity are proved and tested for the new shape. Estimates never
  establish independence. Independently scoped clauses, such as quotas on two overlapping subsets, are a separate class
  (see `../s2/todo.md`), not a wider S1 matcher.

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
