# Stage 06 — Model formulation: open work

Open work in the linearizations this stage emits. Finished work is in [done.md](done.md).

## FRM-2 — A composed extremum's box keeps its finite end

- **Problem.** `ComposedTermRange` (`linearization_minmax.cpp:905`) boxes the auxiliary of a MIN/MAX term in a composed
  objective or constraint. When a decision of a row is open at either end, it marks both ends of the box open
  (`MarkUnbounded`), whatever the sign of the decision's coefficient. The flat and two-level paths walk each end on its
  own (`saved_row_range`, :547, then `AuxRange::CoverRowSided`): a row `c * v` with `c > 0` and `v` bounded below
  keeps its finite floor `c * lb`. One term gets a box on one path and a free auxiliary on the other.
- **Decision needed.** One walk meets an end whose decision is bounded but whose product overflows a DOUBLE
  (`b = 1e308` on every row, `y <= 5`, direct solve off), and two tests read that end apart.
  `reach_of_an_extremum_member` (`test_invalid_value_message.py`) has the composed `SUM(x) + MAX(b * y) <= 10` name
  its clause as too large for a DOUBLE: the composed walk folds the infinite end into the box with
  `AuxRange::CoverRow`, whose `D_ASSERT` forbids an infinity, so only a release build reaches the hand-off check that
  names the clause. `test_a_model_of_finite_huge_numbers_keeps_the_solvers_verdict` has the flat
  `MINIMIZE MAX(b * y)` keep the solver's verdict (the number-range warning, then HiGHS's refusal to load the range):
  the flat walk reads the overflowed end as open and leaves it unboxed.
  - (a) An overflowed end stays open, as the flat walk, the constraint side's walk (`DecideRowSignedRange`) and the
    native ABS arm read it. A box is DeciDB's own tightening and no Big-M, so the model holds only the user's numbers,
    and the warning and the solver's verdict follow; the composed test expects the solver's verdict. A Big-M needs
    both ends and still refuses (`ThrowUnboundedBigMNaming`). Recommended: one reading everywhere, no new code.
  - (b) An overflowed end is a number past a DOUBLE and names its clause on every path. The walk reports an open end
    as a flag (branch `worktree-agent-a59e21263e31e0817`, `818ade4be7`), the flat test takes an example whose
    numbers are all finite, and `DecideRowSignedRange` takes the same flags: there, Gurobi warns and solves where
    HiGHS names the clause, for `MAX(b * y) >= 1` (a native MIN/MAX box) and `MINIMIZE SUM(ABS(b * y))` (the native
    ABS arm's box).
- **Change.** `ComposedTermRange` reads each end through the same signed walk, so one walk sizes every MIN/MAX
  auxiliary.
- **Done when.** A composed MIN/MAX term over a decision bounded on one side dumps its auxiliary with that finite end
  (`DeciDBCli.dump_model`), as the flat path does for the same term, and answers as the oracle does;
  `./test/decide/run_tests.sh -n 2 -k "minmax or extremum"` passes.

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
