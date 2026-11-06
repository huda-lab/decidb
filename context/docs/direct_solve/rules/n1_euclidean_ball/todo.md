# N1 (Euclidean ball) — open work

N1 keeps one sum of squares inside a limit by scaling the direction or the target onto the ball. The class is in
[definition.md](definition.md); what N1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task.

## N1-1 — The radius is `sqrt(B) / sqrt(q)`, not `sqrt(B / q)`

- **Problem.** P3 computes the ball as `B / q` (`ComputeTargets`, `result.limit`). A data-valued limit has no range
  check, so `B / q` overflows to infinity when `q < 1` and `B` is near the top of a DOUBLE, and P4's no-limit test
  (`limit == +inf` in `ComputeLengths`) raises **unbounded** on a bounded problem. Repro, under `require`:
  `SUM(1e-10 * POWER(x, 2)) <= l MAXIMIZE SUM(s * x)` over `(3, 1e300), (4, 1e300)`; the answer is about
  `(6e154, 8e154)`. A second overflow: `value * (sqrt(B) / sqrt(h))` in `Answer` reaches infinity when `h` is subnormal
  and `B` is above about 1e293. The constant-limit path avoids the first only by declining
  (`ProveLimitValue`, `std::isfinite(condition.limit / condition.square_factor)`).
- **Change.** Carry the radius `sqrt(B) / sqrt(q)` (`sqrt(q)` is a plan constant). Test the user's own `B` for `+inf` in
  the no-limit check. Compute the scale as `(value / sqrt(h)) * radius`, whose first factor is at most 1. With a square,
  compare `sqrt(h) > radius`. Delete the `B / q` finite decline and its C++ decline case. A reordering only: no window,
  no rescaling (owner's precision rule: free arithmetic changes only).
- **Done when.** The repro answers on `require` and matches the oracle where the oracle can solve it; a case with `h`
  subnormal and `B = 1e300` stays inside the ball; `run_tests.sh -k n1` and the C++ `[decidb]` decline cases pass; the
  deleted decline has no remaining test.

## N1-2 — A data limit through the shared group-bound path

- **Problem.** N1 builds its own "tightest limit of the group" (`ProveLimitValue` reads
  `clause.group_bound->inputs[0].invalid` directly; `PlanBuilder::Build` hand-builds the `W1` min window). S1 and R1
  read a data limit through `DirectGroupBoundIsReadable`, `DirectTightestBound` and a group sink. The duplicate reaches
  into the group bound's internals, and N1 declines `<= SUM(budget) by (k)` and `<= 0.5 * SUM(l)`, which S1 and R1 admit
  (`done.md` § What it does not admit; a conformance near miss). N2 would copy N1's version.
- **Change.** Read the limit as R1's `ProveDataLimit` does. Move S1's window sink (`RankWindowSink`) to
  `direct_plan_fragments` as the shared window form of `DirectGroupSink`, and have S1 and N1 use it. Read the limit and
  its NaN message from `DirectGroupValues`. Remove `Condition::limit_invalid`.
- **Done when.** `<= SUM(budget) by (k)` and `<= 0.5 * SUM(l)` answer under `require` and match the oracle (new oracle
  cases); the near miss that declined them moves to the "applies" forms; S1's tests pass unchanged; the decline row in
  `done.md` is removed.

## N1-3 — `EXPLAIN` names both forms N1 answers

- **Problem.** `Rule::Summary` returns `min/max SUM(c*x), SUM(x^2) <= B` for every query N1 takes, including a
  projection (`MINIMIZE SUM(POWER(x - s, 2))`), which it does not describe.
- **Change.** One fixed text that covers both: `SUM(x^2) <= B: linear objective or nearest point to a target`.
- **Done when.** The conformance `EXPLAIN` test reads the new text.

## N1-4 — Two smells in `PlanBuilder` and the square-coverage check

- **Problem.** `PlanBuilder::Build` is `const` but writes the `mutable key_types` member. `SquareCoversEveryLinearTerm`
  detects a NULL guard by comparing `&DecideUnguarded(*e)` with `e.get()`, and its correctness depends, unstated, on the
  guard wrapping every coefficient of the term, `w` included, when `t` is nullable.
- **Change.** Carry the key types in `Carried` (or make `Build` non-const). Add `DecideIsGuarded(expr)` beside
  `DecideUnguarded` (`decide_source_provenance.hpp`) and use it. Comment the dependency on the guard's reach.
- **Done when.** No behavior change; `run_tests.sh -k n1` and C++ `[decidb]` pass.

## N1-5 — Guide §10 teaches N1's shape

- **Problem.** The N1 bullet in `language/guide.md` § 10 says "decisions" (N1 takes one), does not say that any other
  bound loses the rule even when it never binds (`x <= 50`), leaves `by` and `per` out of the grouping sentence, does not
  mention a constant factor or `norm(x, 2)`, and does not say a square objective must be a convex `MINIMIZE`. It does not
  name the rule as `EXPLAIN` prints it.
- **Change.** A short example (scores 3 and 4 under `<= 25`), one query that keeps the rule and one that loses it, the
  rule id `N1_EUCLIDEAN_BALL` beside the bullet. One sentence that a direct answer is exact where the solver's carries its
  tolerance (3 and 4 against 3.0005 and 3.9996).
- **Done when.** Every query in the section runs as the text says under `require` (or declines where it says so).

## N1-6 — Benchmark sizes past `sf=1`

- **Problem.** `benchmark/decide/direct_solve/run.py --queries n1 n1_groups` has run up to `sf=1` only.
- **Change.** Run the larger sizes and record them in `done.md` § Performance. The owner starts these runs.
- **Done when.** The table in `done.md` holds the larger sizes with their agreement check.
