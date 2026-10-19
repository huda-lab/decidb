# R1 — Continuous (fractional) knapsack — open work

R1 splits one shared budget among `REAL` decisions by filling the best value per unit of budget first. The class is in
[definition.md](definition.md). How to work and the checks are in `../../framework/todo.md` § How to work any task. The
goal is the full definition with its § Admitted class.

## R1-02 — The core rule: one decision, one `<=` budget

- **Needs.** `../../framework/todo.md` § H-22 and § H-23.
- **Problem.** `DECIDE amount(REAL) SUCH THAT amount BETWEEN 0 AND available AND SUM(w*amount) <= B MAXIMIZE
  SUM(v*amount)` runs on the solver.
- **Change.** `r1_continuous_knapsack_rule.cpp`, registered in `direct_registry.cpp`. One row-scoped `REAL` decision;
  per-row ranges and the one-decision shape check from the shared bound reader, a linear objective from the shared
  objective reader, positive lower bounds included (the baseline is taken first); one `SUM(w*x) <= B` with a constant or foldable `B`; a
  linear objective (`MAXIMIZE`, `MINIMIZE` or none). A weight of any sign, read per row (`definition.md` § Admitted
  class): one price per unit of budget, each row at its upper bound when its value beats price × weight, else at its
  lower. A bound of magnitude 1e20 or more, constant or per row, is no bound (`definition.md` § Admitted class): the
  price must leave an uncapped row's value no more than price × weight and a floorless row's value no less, and when no
  price does, the plan raises unbounded, in the same pass that finds the price. Tests cover a lone free row, an
  improving pair, and both under `MINIMIZE`. Infeasible when a row's box is empty (the bound reader's empty-box test) or the baseline exceeds the budget, unbounded where the solver
  says so, every row read before one is released.
- **Done when.** `test_direct_r1_continuous_knapsack.py` (differential oracle table, error table, boundary table, the
  shape inside parent queries) and C++ applicability-condition cases pass; `rules/r1_continuous_knapsack/done.md` exists
  with its stamp; the README status row says R1 works.

## R1-03 — Contract fixture and fuzz generator

- **Needs.** R1-02.
- **Problem.** The conformance and fuzz suites do not exercise R1.
- **Change.** A `RuleFixture` in `_direct_rule_fixtures.py` with R1's near misses (an `INT` decision, two budgets, a
  coupled objective term); a seeded R1 generator in `test_direct_fuzz.py`.
- **Done when.** Both suites run R1 and pass in all five checks.

## R1-04 — Budget as `=`, `>=` or `BETWEEN`

- **Needs.** R1-02.
- **Problem.** An exact-fill or minimum-spend budget runs on the solver.
- **Change.** Admit `=`, `>=` and `BETWEEN` (`definition.md` § Admitted class). One walk serves every direction: put each row at its own best
  end and total the spend; when that meets the budget, the budget does not bind; when it is over a limit, pull rows back
  giving up the least value per unit of budget first; when it is under a minimum, push rows up losing the least value
  per unit first (a negative price). `BETWEEN` binds on at most one side. An exact fill continues past nonpositive
  values; a required amount outside the range the boxes can reach is infeasible.
- **Done when.** Three-way and error cases for each direction pass, under `MAXIMIZE` and `MINIMIZE`, including a
  budget that does not bind and an unreachable fill; the minimum-cost cover (`MINIMIZE SUM(c*x)`, `SUM(x) >= d`) is
  among them.

## R1-05 — Budget per group and under `WHEN`

- **Needs.** R1-02.
- **Problem.** `SUM(w*x) <= B PER region` and a budget clause with `WHEN` run on the solver.
- **Change.** Rank and fill within each group, one price per group. A `WHEN` on the budget, written on the clause or
  on the aggregate, and a NULL `PER` key leave a row out of every budget; that row takes its own best end, and an
  uncapped improving one is unbounded (`definition.md` § Admitted class). A `WHEN` that covers no row of a nonempty source raises the
  empty-aggregate error through `DirectGuardEmptyAggregate`, as S1 does.
- **Done when.** Three-way cases for `PER`, `WHEN` in both spellings, `PER` with `WHEN`, a NULL key, a group with no
  covered row and a `WHEN` covering no row pass; the fuzz generator draws them.

## R1-06 — Shrinking returns across several decisions

- **Needs.** R1-02.
- **Problem.** A piecewise benefit written as several decisions in one budget runs on the solver.
- **Change.** First, the concavity check the owner asked for: confirm, by argument and by differential oracle cases with
  increasing piece values, that the solver's model holds no ordering or linking between the decisions of one row, so
  the greedy fill over every (row, decision) item matches it with or without concavity. Bring the result to the owner
  before building. Then admit several `REAL` decisions in one budget, each (row, decision) an item in the ranking with
  its own weight, value and box, each decision answered in its own column. A per-row constraint or objective term that
  links decisions stays on the solver.
- **Done when.** The concavity result is settled with the owner; differential oracle cases with two and three decisions
  pass, concave and not (or, if the check fails, a non-concave set stays on the solver); a linking constraint is a near
  miss.

## R1-07 — Performance table

- **Needs.** R1-02 to R1-06.
- **Problem.** R1's speed against the solver is unmeasured.
- **Change.** Measure the admitted shapes at 5M stored rows against Gurobi and HiGHS in fresh processes, with the same
  method as `../a1_separable_box/done.md`. Timeouts and solver limits are not wins.
- **Done when.** `done.md` has the table, its date and the commands to reproduce it.
