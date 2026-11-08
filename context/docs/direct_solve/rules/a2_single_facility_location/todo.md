# A2 (single-facility location) — open work

A2 picks one shared value for many rows: one `per ():` or `per K:` decision placed at the best center of the rows'
observations. The class is in [definition.md](definition.md). How to work and the checks are in
`../../framework/todo.md` § How to work any task; a new rule follows `../../architecture/rules.md` § Adding a rule.

## A2-9 — Data weights on the absolute loss

- **Decision needed.** Whether A2 takes `SUM(w * ABS(c - v))` with `w` read from the data.
- **Problem.** The weighted median is optimal only when every weight is nonnegative. A negative weight makes the loss
  non-convex, the plan is chosen before any weight is read, and the solver path answers such a query (Big-M).
- **Options.**
  1. Keep declining it. Cost: none; weighted medians over data weights stay on the solver path.
  2. Evaluate the loss at every breakpoint inside the interval and at both ends, from running sums of `m` and `m*v`
     per class, and take the smallest (unbounded when the total weight is negative and an end is infinite). It covers
     every sign with no branch, at the cost of two more windows and values that can cancel.
- **Recommendation.** 1, until a query needs it.
- **Done when.** Option 2: oracle cases with mixed-sign weights agree on both backends.

## A2-10 — `ABS` is copied into a square's struct

- **Problem.** `DirectAbsTerm` and `DirectSquareTerm` (`direct_bound_reader.hpp`) hold the same three fields: the
  weight, its names and the linear form inside. A2's `ReadAbsolute` copies the one into the other so that
  `DirectLayoutQuadraticPart` lays the `ABS` out, and `CenteredLoss` says it is "laid out as a square is". Two names
  for one thing, and a copy to cross between them.
- **Change.** One struct for `weight * f(w*x + t)` (for example `DirectWeightedForm`), returned by both readers and
  taken by the layout. `ReadAbsolute` keeps no copy.
- **Done when.** No copy between term types remains; the matrix passes with no answer changed.

## A2-11 — The losses pass their columns as unnamed lists

- **Problem.** Each loss hands its columns between stages as a `vector<ColumnBinding>` read back by position
  (`raw[at++]`, `term.kept ? 1 : 0`, `classes[0]` to `classes[2]`). `SquareLoss` packs its layout's named columns
  into such a list in P1 and unpacks them into `DirectQuadraticColumns` again in P2. A wrong index still compiles.
  `SquareLoss::Finish` also recomputes the clamp that the shared tail computes, for its too-large check.
- **Change.** Each loss keeps its own columns by name from stage to stage (a struct per loss, which the skeleton
  passes through without reading). The too-large check of the answer moves to the shared tail, on the clamped answer
  it already computes, after the loss's own checks, so the unbounded error still comes first.
- **Done when.** No loss reads a column by position, the clamp is computed once, and the matrix passes with no answer
  or error text changed.

## A2-12 — The test grids reach the same branches many times

- **Problem.** A2 has 1,172 tests; N1 has 143 and A1 196. The absolute loss alone has 530. The grids are full cross
  products: both centered losses times 10 row sets times 6 intervals, every spelling on three row sets, every weight
  set in every interval. `test_a2_a_weighted_class_answers_the_ruled_point` and
  `test_a2_an_absolute_class_answers_the_ruled_point` run every case of the oracle tables a second time to check the
  tie point. The repository's rule is that a grid case reaches a branch no other case reaches
  (`.claude/CLAUDE.md` § Testing).
- **Change.** For each grid, name the branches of the plan it is for (the lower and the upper median, a clamp up, down
  or none, the nearest-0 tie, no kept row, a NULL value, a point interval; each spelling once for the IR reading) and
  keep one case per branch, named for it. Check the ruled point inside the oracle test, on the same run, and delete
  the separate ruled-point tests.
- **Needs.** A2-10, A2-11 and H-48, which change the branches.
- **Done when.** Every remaining case names the branch it reaches, removing that branch fails it, and the matrix
  passes.
