# S1 (top-k and cardinality intervals) — open work

What S1 does today is in `done.md`. The class definition is in `definition.md`. Each task is independent unless it says
otherwise. When one ships, its result moves to `done.md` and the task is deleted from here. Estimates are rough guesses,
not measurements.

**What is left.** Nothing here is needed to close S1's definition: S1-08 widens what the solver-accepted right-hand side
reads, and S1-04 and S1-05 wait for a decision (see their entries). Batches A (per-row bounds on `x`, out-of-scope tests),
B (scaled objective) and C (row-count bounds) shipped and are in `done.md`.

## S1-08 — Other aggregates on the right-hand side of a count bound

- **Problem.** Only a plain `COUNT(*)` is admitted on the right. The solver also accepts `AVG`, `SUM`, `MIN`, `MAX` and
  `COUNT(col)` of data columns, evaluated to one value per group (`03_expressivity/sql_functions/done.md`, "Reducers as a
  Bound"), and a right-hand aggregate with its own `WHEN`.
- **Checked on the solver 2026-10-05.** `COUNT(*)` follows the clause-level `WHEN` and `PER`; an aggregate-local `WHEN` on the
  left does not narrow it. A trailing `WHEN` on a *bare* right-hand aggregate (`SUM(x) <= COUNT(*) WHEN active`) is
  aggregate-local to that aggregate: the count is over the active rows while the left side sums every row. The same `WHEN`
  on `COUNT(*) / 2` is clause-level. S1 declines the bare form today, so it cannot be misread.
- **Plan.** The count stage (`CountRows` in `s1_rule.cpp`) is one window per count flavor. Another reducer is one more window
  aggregate over the same partitions, with a `FILTER` for the right-hand `WHEN`. The bound expression, its range proof,
  and the per-group limit are reused.
- **Not known.** How the solver treats NULLs inside `SUM`, `AVG`, `MIN` and `MAX`; an empty group; DECIMAL and integer
  typing of `SUM(col)` (the range proof needs a bound on the column, which a count does not); and relation-qualified
  reducers (`SUM(D: cost)`), which stay out. Check each on the solver before coding.
- **Done when.** Each admitted reducer matches the solver for every comparison and scope, the range proof covers its
  value, and the ones left on the solver have a named reason. About 1 day. Worth doing if users write "at most the group's
  average" style bounds; not needed to close S1.

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
