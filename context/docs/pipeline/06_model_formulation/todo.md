# Stage 06 — Model formulation: open work

Open work in the linearizations this stage emits. Finished work is in [done.md](done.md).

## FRM-4 — The two-level objective under a factor of unknown sign

- **Problem.** The two-level objective's direction is its sign times its factor's, read at plan time
  (`RewriteMinMaxObjectiveTree`), and the physical layer fixes the outer extremum from it: PATH B states
  `objective = w` at coefficient 1 and carries the factor in the members' coefficients (`linearization_minmax.cpp`).
  A factor whose sign is known only when the query runs, `(SELECT sum(c) FROM s) * max(per k: sum(w * x) by (k))`,
  has no direction, so it is refused with `DECIDE: an aggregate inside an aggregate is not supported yet`
  (`../../language/errors.md` §2). The flat form takes the same factor: the composed pass pins its extremum in both
  directions, so either sign stays exact (`decide_rewrite_minmax.cpp`, "WHY IT STAYS OUTSIDE").
- **Decision needed.** Pin the outer extremum in both directions under a factor of unknown sign, as the composed pass
  pins a flat one (recommended: the codebase's own answer to an unknown sign, exact for either sign); or choose the
  extremum in the physical layer from the evaluated factor; or move the factor onto `w`'s objective coefficient.
- **Done when.** That query answers as the oracle does for a positive and a negative subquery value, its row in
  `errors.md` §2 and its refusal row in `test_error_binder.py::test_objective_shape` go, and
  `./test/decide/run_tests.sh -n 2 -k "per_objective or two_level"` passes.
