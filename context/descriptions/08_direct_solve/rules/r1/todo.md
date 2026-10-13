# R1 — Continuous piecewise-linear allocation — open work

R1 splits one shared budget among `REAL` decisions by filling the best value per unit of budget first. The class is in
[definition.md](definition.md). How to work and the checks are in `../_harness/todo.md` § How to work any task. The goal
is the full definition; R1-00 settles where the build goes past it or stops short of it.

## R1-00 — Settle R1's scope

- **Decision needed.**
- **Problem.** The definition assumes facts a plan cannot prove before it reads the rows, and leaves some DECIDE shapes
  unstated. Each question below changes what R1-02 to R1-06 build.
  1. **Weight from a column.** The definition requires every weight `w` in `SUM(w*x)` to be strictly positive. A column
     weight (`unit_resource * amount`, the worked example) can be zero or negative on some row, and the plan is fixed
     before the rows are read. Raising an error there is a wrong answer, because the solver answers.
  2. **Bound from a column.** The definition requires finite bounds. A column bound of 1e20 or more, or an infinite
     value, is no bound on the solver, so the plan meets it at run time.
  3. **Shrinking returns.** DECIDE has no piecewise syntax. A user writes pieces as one row each (plain R1) or as several
     decisions in one budget (`SUM(w1*x1 + w2*x2) <= B`, with each `xi` boxed to its piece).
  4. **Budget direction.** The definition admits `<=` and `=`. `>=` and `BETWEEN` are not stated.
  5. **Budget scope.** The definition admits independent groups (`PER`). A `WHEN` on the budget, where rows outside it use
     no budget, is not stated.
- **Options.**
  - Q1: (a) one-price construction for any sign: one price per unit of budget, each row at its upper bound when its value
    beats price × weight, else at its lower; the price comes from one sort by value per weight. A short duality proof,
    and `definition.md` is corrected to match. (b) Constant positive weights only; a column weight goes to the solver.
    No new proof, loses the worked example.
  - Q2: (a) handle no-bound rows in the same plan: a positive-weight row is capped by the budget, any other improving
    row raises unbounded as on the solver. (b) Decline when any bound is a column.
  - Q3: (a) admit several decisions in one budget with per-piece proof of shrinking returns. Needs multi-decision
    facts and a concavity check. (b) One decision only; pieces as rows already work.
  - Q4: (a) admit `>=`, `=` and `BETWEEN` (the one-price construction covers them with a price of either sign).
    (b) `<=` and `=` only.
  - Q5: (a) admit `PER` and a clause `WHEN`. (b) `PER` only.
- **Recommendation.** (a) on every question, which is the most complete R1. Q3 is the largest and the least used; if it
  proves long, it drops to (b) without touching the rest.
- **Done when.** Each answer is written into the task it affects, and `definition.md` states the admitted class.

## R1-02 — The core rule: one decision, one `<=` budget

- **Needs.** R1-00.
- **Problem.** `DECIDE amount(REAL) SUCH THAT amount BETWEEN 0 AND available AND SUM(w*amount) <= B MAXIMIZE
  SUM(v*amount)` runs on the solver.
- **Change.** `r1_rule.cpp`, registered in `direct_registry.cpp`. One row-scoped `REAL` decision; per-row ranges from
  the shared reader (`DirectReadBounds`, `DirectClauseBounds` and `DirectRowBox` in `direct_builder.cpp`; the shape
  check that a bound reads one decision, `ReadsOneDecision`, is still A1's own), positive lower bounds included (the baseline is taken first); one `SUM(w*x) <= B` with a constant or foldable
  `B`; a linear objective (`MAXIMIZE`, `MINIMIZE` or none). Weights and bounds per R1-00 Q1 and Q2. Infeasible when
  the baseline exceeds the budget, unbounded where the solver says so, every row read before one is released.
- **Done when.** `test_direct_r1.py` (three-way table, error table, boundary table, the shape inside parent queries)
  and C++ proof cases pass; `rules/r1/done.md` exists with its stamp; the README status row says R1 works.

## R1-03 — Contract fixture and fuzz generator

- **Needs.** R1-02.
- **Problem.** The standard contract and fuzz suites do not exercise R1.
- **Change.** A `RuleFixture` in `_direct_rule_fixtures.py` with R1's near misses (an `INT` decision, two budgets, a
  coupled objective term); a seeded R1 generator in `test_direct_fuzz.py`.
- **Done when.** Both suites run R1 and pass in all five checks.

## R1-04 — Budget as `=`, `>=` or `BETWEEN`

- **Needs.** R1-02, R1-00 Q4.
- **Problem.** An exact-fill or minimum-spend budget runs on the solver.
- **Change.** Admit the directions R1-00 Q4 chose. An exact fill continues past nonpositive values; a required amount
  outside the range the boxes can reach is infeasible.
- **Done when.** Three-way and error cases for each direction pass, including an unreachable fill.

## R1-05 — Budget per group and under `WHEN`

- **Needs.** R1-02, R1-00 Q5.
- **Problem.** `SUM(w*x) <= B PER region` and a budget clause with `WHEN` run on the solver.
- **Change.** Rank and fill within each group. Under `WHEN`, rows outside the clause use no budget and take their own
  best value in their range. A NULL `PER` key follows the solver.
- **Done when.** Three-way cases for `PER`, `WHEN`, a NULL key and an empty group pass; the fuzz generator draws them.

## R1-06 — Shrinking returns across several decisions

- **Needs.** R1-02, R1-00 Q3.
- **Problem.** A piecewise benefit written as several decisions in one budget runs on the solver.
- **Change.** Admit several `REAL` decisions in one budget when each piece's value per unit of budget does not increase
  from one piece to the next. Each piece becomes a row in the ranking; the pieces of one decision sum back to it.
- **Done when.** Three-way cases with two and three pieces pass, and a non-concave set of pieces stays on the solver.

## R1-07 — Performance table

- **Needs.** R1-02 to R1-06.
- **Problem.** R1's speed against the solver is unmeasured.
- **Change.** Measure the admitted shapes at 5M stored rows against Gurobi and HiGHS in fresh processes, with the same
  method as `../a1/done.md`. Timeouts and solver limits are not wins.
- **Done when.** `done.md` has the table, its date and the commands to reproduce it.
