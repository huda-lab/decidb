# O1 (Monge assignment) — open work

O1 answers a one-to-one matching of two ordered sides by sorting both and pairing equal ranks. The class is in
`definition.md`. How to work is in `../../framework/todo.md` § How to work any task. A task runs only the focused
tests its **Done when** names; the full gate runs once at combine. The standard tests are in
`../../architecture/rules.md` § The standard tests every rule ships.

## Scope rulings (owner, 2026-11-10)

The first version admits only the shape below and declines everything else. A decline is a slower correct answer; a
wrong admit is a wrong answer.

- **Source.** DECIDE's child is a `CROSS_PRODUCT`. Filter pushdown runs before the DECIDE optimizer, so a one-side
  `WHERE` sits inside its side and the pairs stay complete; a filter across both sides makes a join, which declines.
- **Constraints.** Exactly two, each `per K: sum(x) by (K) = 1`, where K is a relation key (`per l`, NULL compared as a
  value) whose columns all come from one side, and the two keys come from different sides. No `when`, no other
  constraint, one row-scoped `BOOL` decision.
- **Objective.** One term: `MINIMIZE k*|A - B|` or `MINIMIZE k*(A - B)^2` with constant `k >= 0`, or `MAXIMIZE` or
  `MINIMIZE k*A*B` with constant `k`. A is a bare numeric column of the left key, B of the right key (either order). No
  `when`.
- **Positions (ruling, owner, 2026-11-10).** Both position columns are declared `NOT NULL` (a `PRIMARY KEY` counts) on
  a table their side scans directly; otherwise the rule declines. `SUM` drops a NULL term (guide § NULL; deck p. 56),
  so an item with a NULL position pairs with anyone at no cost, and rank pairing is no longer optimal. A declared
  constraint holds for every row the plan will read, which column statistics do not promise.
- **Plan.** Group-then-join over the source reader: the distinct classes of each side, ranked by position, joined back
  to the pair rows. A hand plan of this shape ran in 64 ms at 600 × 600, where Gurobi took 1.47 s.
- **Lean.** No new shared helper until a second rule needs it; no case outside the shapes above.

## O1-9 — O1 ranks each side on its own rows

- **Problem.** O1's plan does its work on the pairs, not on the rows of each side. It reads the cross product three
  times, groups all n² pairs into classes twice on the whole row key, joins all n² pairs back to the classes twice, and
  runs a window over all n² pairs to pick one copy of each matched class pair. At 3,200 suppliers per side (10.24M
  pairs) it takes 51 s of CPU and 7.8 s of latency, where a hand-written plan of the same answer takes 47 ms
  (`run.py --compare --sf 0.32 --queries o1`).
- **Cause.** A cross product is never a repeatable chain (`DirectRepeatableSourceChain`), so with three reads the
  source reader materializes all n² pairs, and every stage reads them.
- **Change (architect review, 2026-11-10).** Eager aggregation (Yan–Larson), applied to windows: the work whose keys
  come from one side runs below the cross product, on that side's rows.
  - `Rule::Apply` reads the source once (`source_reads = 1`, `TakeRecomputedSource()`), which is DECIDE's own cross
    product, and builds one `DirectSourceReader` per child, two reads each, so each side makes the existing choice
    between a rescan and an n-row CTE. The cross-product node is reused as the plan's own cross product, never freed
    (the driver holds a reference to it).
  - `Rank`, per side: one window of `ROW_NUMBER`, `RANK` and `DENSE_RANK`, ordered by the position in the side's
    direction, then every other key column; a second window gives the class count as the greatest `DENSE_RANK`.
    `DENSE_RANK` is the class rank; `ROW_NUMBER = RANK` marks one copy of each class.
  - `CheckPairs`: the written coefficient and its `DirectCheckNumber` over the cross product of one read of each side,
    in `DirectAllRowsValid`, gating the left reader (A1's verify-then-stream). The verdict evaluates the coefficient
    even where `DirectCheckNumber` gives no check, so DuckDB's own evaluation error still raises.
  - `Assign`, over the cross product of the two ranked sides: the source columns, then `x` = equal dense ranks and
    both copies first, then the class-count check (`DirectRaiseUnless`) as the validated column.
  - The right reader is finished outermost: the verdict reads the right side's CTE.
  - Dead after the change, deleted with it: `RankClasses`' aggregate and count window, `PlanBuilder::Join`, the pair
    window, the validated coefficient column.
- **Rulings (owner, 2026-11-10).**
  - The every-pair check is the gate over one extra pass of the pairs, not a proof over each side's extreme positions.
  - A row check or bound table under O1 is a `D_ASSERT`: O1's shape makes neither, so a decline would be unreachable.
  - The class count is a second window over the dense rank, not a second sort.
  - `../../architecture/rules.md` names one reader per side as the way a rule reads a cross-product source.
- **Tests to add.** A side that is materialized (a join or `random()` on one side, and on both); duplicate rows with a
  NULL key; -0.0 against 0.0 and a NaN in a key column that is not the position, where window peers and the old
  grouping could differ.
- **Done when.** `run.py --compare --sf 0.32 --queries o1` gives a CPU ratio within 5 of the hand plan; the O1 tests
  pass (`./test/decide/run_tests.sh -n 2 -k "o1_monge or O1 or comparator"`); C++ `unittest "[decidb]"` passes.

## O1-6 — O1's Performance is measured

- **Needs.** O1-9. A quiet machine.
- **Problem.** `done.md` has no Performance section, which every working class has.
- **Change.** A hand plan `benchmark/decide/direct_solve/hand/o1.sql` and an O1 query in the benchmark, measured as
  `../../../performance/benchmarking/direct_solve.md` § Comparing with a hand-written plan describes, against Gurobi and
  HiGHS.
- **Done when.** `done.md` § Performance gives CPU against the hand plan and speed against both solvers.

## O1-7 — `per l.id` on a primary key

- **Needs.** Optional.
- **Problem.** `per l.id` declines, because a column key may have duplicates or NULLs and its position need not be a
  function of it. Users write this spelling naturally.
- **Change.** Admit a column key when its columns are the declared `PRIMARY KEY` of a table its side scans directly,
  so each class is one row and its position is fixed. `DirectIR::not_null` already walks to those scans; the front end
  records the primary-key columns on the same walk. Every other column key declines.
- **Done when.** An oracle case with a primary key answers under `require`; the same query without the key declines;
  `./test/decide/run_tests.sh -n 2 -k o1_monge` passes.
