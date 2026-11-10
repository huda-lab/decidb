# Stage 06 — Model formulation: open work

Open work in the linearizations this stage emits. Finished work is in [done.md](done.md).

## FRM-2 — A composed extremum's box keeps its finite end

- **Problem.** `ComposedTermRange` (`linearization_minmax.cpp:905`) boxes the auxiliary of a MIN/MAX term in a composed
  objective or constraint. When a decision of a row is open at either end, it marks both ends of the box open
  (`MarkUnbounded`), whatever the sign of the decision's coefficient. The flat and two-level paths walk each end on its
  own (`saved_row_range`, :547, then `AuxRange::CoverRowSided`): a row `c * v` with `c > 0` and `v` bounded below
  keeps its finite floor `c * lb`. One term gets a box on one path and a free auxiliary on the other.
- **Change.** `ComposedTermRange` reads each end through the same signed walk, so one walk sizes every MIN/MAX
  auxiliary.
- **Done when.** A composed MIN/MAX term over a decision bounded on one side dumps its auxiliary with that finite end
  (`DeciDBCli.dump_model`), as the flat path does for the same term, and answers as the oracle does;
  `./test/decide/run_tests.sh -n 2 -k "minmax or extremum"` passes.
