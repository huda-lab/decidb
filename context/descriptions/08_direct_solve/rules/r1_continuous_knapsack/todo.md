# R1 — Continuous (fractional) knapsack — open work

R1 splits one shared budget among `REAL` decisions by filling the best value per unit of budget first. The class is in
[definition.md](definition.md). How to work and the checks are in `../../framework/todo.md` § How to work any task. The
goal is the full definition with its § Admitted class.

## R1-02 — The core rule: one decision, one `<=` budget

- **Problem.** `DECIDE amount(REAL) SUCH THAT amount BETWEEN 0 AND available AND SUM(w*amount) <= B MAXIMIZE
  SUM(v*amount)` runs on the solver.
- **Change.** `r1_continuous_knapsack_rule.cpp`, registered in `direct_registry.cpp`, built as `definition.md` § Relational construction describes. One
  row-scoped `REAL` decision; per-row ranges and the one-decision shape check from the shared bound reader, positive
  lower bounds included (the baseline is taken first); one `SUM(w*x) <= B` with a constant or foldable `B`; a linear
  objective (`MAXIMIZE`, `MINIMIZE` or none). The budget's terms and the objective's come from one shared reader: rename
  `DirectReadLinearObjective` to a reader of any linear `DirectPart`. A weight of any sign, read per row: each row at
  its upper bound when its value beats price × weight, else at its lower. A bound of magnitude 1e20 or more, constant
  or per row, is no bound; when no price leaves every uncapped row's value no more than price × weight and every
  floorless row's no less, the plan raises unbounded in the same pass that finds the price. Infeasible when a row's box
  is empty (the bound reader's empty-box test) or the baseline exceeds the budget; every row read before one is
  released. R1 is disjoint from S1 (a `BOOL` count bound) and A1 (no aggregate constraint) by requiring a `REAL`
  decision under exactly one `SUM` constraint. The assignment check uses the tolerances of the S1 and A1 oracle tests.
- **Done when.** `test_direct_r1_continuous_knapsack.py` passes: the differential oracle table (with a lone free row,
  an improving pair, and both under `MINIMIZE`), the error table, the boundary table and the shape inside parent
  queries. C++ cases cover each decline branch, and R1 against S1 and A1 in the rule driver. `test_direct_user_facing.py`
  compares the full text of R1's errors under `require` and `off`. `rules/r1_continuous_knapsack/done.md` exists with
  its stamp; the README status row says R1 works.

## R1-03 — Conformance fixture

- **Needs.** R1-02.
- **Problem.** The conformance suite does not exercise R1.
- **Change.** A `RuleFixture` in `_direct_rule_fixtures.py` with R1's near misses: an `INT` decision, two budgets, a
  coupled objective term, a budget from a column.
- **Done when.** The conformance suite runs R1 and passes in all five checks.

## R1-04 — Budget as `=`, `>=` or `BETWEEN`

- **Needs.** R1-02.
- **Problem.** An exact-fill or minimum-spend budget runs on the solver.
- **Change.** Admit `=`, `>=` and `BETWEEN` through the search in `definition.md` § Relational construction: each row
  starts at its own best end; over a limit, rows pull back losing the least value per unit of budget first; under a
  minimum, rows push up losing the least value per unit first (a negative price). `BETWEEN` binds on at most one side.
  An exact fill continues past nonpositive values; a required amount outside the range the boxes can reach is
  infeasible.
- **Done when.** Three-way and error cases for each direction pass, under `MAXIMIZE` and `MINIMIZE`, including a
  budget that does not bind and an unreachable fill; the minimum-cost cover (`MINIMIZE SUM(c*x)`, `SUM(x) >= d`) is
  among them.

## R1-05 — Budget per group and under `WHEN`

- **Needs.** R1-02.
- **Problem.** `SUM(w*x) <= B PER region` and a budget clause with `WHEN` run on the solver.
- **Change.** Rank and fill within each group, one price per group, with the shared scope projection. A `WHEN` on the
  budget, written on the clause or on the aggregate, and a NULL `PER` key leave a row out of every budget; that row
  takes its own best end, and an uncapped improving one is unbounded (`definition.md` § Admitted class). A `WHEN` that
  covers no row of a nonempty source raises the empty-aggregate error through `DirectGuardEmptyAggregate`, as S1 does.
- **Done when.** Three-way cases for `PER`, `WHEN` in both spellings, `PER` with `WHEN`, a NULL key, a group with no
  covered row and a `WHEN` covering no row pass.

## R1-06 — Several decisions in one budget

- **Needs.** R1-02.
- **Problem.** A piecewise benefit written as several `REAL` decisions in one budget runs on the solver.
- **Change.** Admit several `REAL` decisions in one budget, each (row, decision) an item in the ranking with its own
  weight, value and box, each decision answered in its own column (`definition.md` § Correctness). A per-row
  constraint or objective term that links decisions stays on the solver.
- **Done when.** Differential oracle cases with two and three decisions pass, with shrinking and with growing piece
  values; a linking constraint is a near miss in the fixture.

## R1-10 — Budget from a column or an aggregate

- **Needs.** R1-05.
- **Problem.** A budget read from the data runs on the solver: a source column or expression (`SUM(w*x) <= budget`),
  or an aggregate of the source (`<= 0.5*SUM(cost)`, which the right-hand-side rewrite turns into a per-row window
  value). S1 admits both for its count bound.
- **Change.** Read the budget per group through the shared bound reduction (`DirectValidateBounds`), as S1 reads its data-valued
  bound: an upper budget takes the group's MIN, a lower one its MAX, an equality both, which must agree. A NULL or NaN
  budget raises the solver's error. Add the shape to `definition.md` § Admitted class.
- **Done when.** Three-way cases pass for a column budget, global and per group, an aggregate budget, and a budget
  under `WHEN`; the error table holds an equality budget that differs inside a group and NULL and NaN budgets; the
  fixture's column-budget near miss becomes an applying form.

## R1-07 — Performance table

- **Needs.** R1-02 to R1-06, R1-10.
- **Problem.** R1's speed against the solver is unmeasured.
- **Change.** Measure the admitted shapes at 5M stored rows against Gurobi and HiGHS in fresh processes, with the same
  method as `../a1_separable_box/done.md`. Timeouts and solver limits are not wins.
- **Done when.** `done.md` has the table, its date and the commands to reproduce it.
