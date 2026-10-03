# Design — verified foundation

This records the source-grounded foundation and the implemented S1
prototype. The broader direct-solve gate is still open.

BASE-01: the prototype documentation checkpoint is on branch
`direct-solve-prototype`, forked from recorded `master` SHA
`721d49ba43d10fd4e9e15572508f10a873970c3b`; the first rule uses current
DECIDE syntax. The [baseline corpus](../03_correctness/baseline.md) records
solver outputs and near misses measured before feature code.

DES-01 through DES-07 are resolved as **first-build design decisions**, not as
implementation acceptance: use an explicit output-slot boundary; retain a
mandatory blocking validation dependency; keep parent work outside the
proved rank scope; carry one structured decision record into physical
explanation; expose `off`/`auto`/`require`; use the finite DOUBLE score and
integral capacity contract; and begin implementation with the checks in
[todo.md](todo.md).
The [experimental record](experiments.md) includes a failing naive boundary,
the repaired 28-assertion temporary probe, and current solver behavior. The
spike code was removed after gathering evidence. The current production path
uses the same explicit dependency idea in `LogicalDirectSolveResult`.

- The DECIDE optimizer runs after filter pushdown and before join ordering,
  unused-column removal, column-lifetime analysis, TopN, and later passes
  (`src/optimizer/optimizer.cpp`).
- `DecideOptimizer::OptimizeDecide` marks `LogicalDecide` optimized and selects a
  backend before solver-specific rewrites (`src/optimizer/decide/decide_optimizer.cpp`).
  A direct attempt must happen before that method's solver work.
- The prepared linear form is solver-path state produced after the early
  optimizer seam (`src/optimizer/decide/decide_linear_form.cpp` and
  `src/execution/physical_plan/plan_decide.cpp`). It is not the direct detector's
  input.
- `LogicalDecide` exposes child bindings plus decision bindings
  (`src/planner/operator/decide/logical_decide.cpp`). A final ordinary projection
  owns new bindings, so replacing the node needs an explicit external contract.
- Declared `BOOL` decisions have a 0/1 domain and SQL `INTEGER` output
  (`src/planner/expression_binder/decide/decide_declarations_binder.cpp`).
- Existing source has a session-setting registration/lookup pattern
  (`src/decidb/diagnostics/decide_diagnostic.cpp`) and a test-only
  `DECIDB_FORCE_SOLVER` override (`src/decidb/solver/ilp_solver.cpp`).
- `LogicalWindow` can supply an unpartitioned `ROW_NUMBER()`. A temporary
  generated bound-plan probe passed optimizer, serializer verification, and
  physical execution after explicit child-output dependencies were added. It
  covered selected parent/runtime cases. The later production pass and
  validation checks are recorded below as DES-08 and DES-09.

The production boundary is a logical extension lowered to an ordinary physical
projection. The first rule builds typed score evaluation, a NULL/non-finite
guard, a global or scoped `ROW_NUMBER`, and a full 0/1 output. The mathematical class
work remains in [`decidb_direct_relational_rewrites.docx`](../decidb_direct_relational_rewrites.docx).
The [large-scale report](../04_performance/s1_large_scale.md) now measures
integrated direct and solver paths through five million rows.

DES-08: the [current built-in optimizer audit](optimizer_audit.md) traces CTE
filter movement, join order, unused columns, both lifetime runs, limit/TopN,
late materialization, empty-result pullup, statistics/compression, and join
filter pushdown through the result boundary. Parent filter, join, aggregate,
materialized CTE, nested, empty-input, serializer, and physical output tests
exercise the contract. The rank remains a dependency when `x` is unprojected.

DES-09: the first rule keeps a hidden rank live even when capacity is
zero or the parent does not read `x`. Permanent 5,000-row regressions check a
late NULL, NaN, and infinity under outer `LIMIT 1`, `ORDER BY ... LIMIT 1`,
`COUNT(*)`, and a parent filter. A deterministic throwing cast is checked under
`LIMIT 1`, `COUNT(*)`, and a parent filter. Input `WHERE` removes an invalid
row; outer `LIMIT 0` skips execution.

DES-10: the decision record appears in optimized logical and default physical
`EXPLAIN` and in `EXPLAIN ANALYZE` profiling. Prepared plans keep the selected
mode when the session setting changes; altering a temporary source table
triggers a real rebind and picks up the current mode. `require` errors name the
rejected condition before model work.

DES-11: the shared extension boundary has an opt-in unused-output hook. The
direct result map carries an exact permission for each output: stored columns,
constants, and passthrough aliases through projections, filters, or inner
comparison joins may be skipped when unreferenced; other
computed source expressions stay live. The mandatory rank suffix is never
pruned. A direct/solver `error('payload boom')` regression proves why this
distinction is needed. The pruned plan retains bindings and passes serializer
verification; [API measurements](../04_performance/s1_api_phase.md#selective-output-pruning)
record the five-million-row stored-payload and joined-source gains.
