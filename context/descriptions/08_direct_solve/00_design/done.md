# Design — verified foundation

This is source-grounded context, **not** a claim that direct solve is shipped.

BASE-01: the prototype documentation checkpoint is on branch
`direct-solve-prototype`, forked from recorded `master` SHA
`721d49ba43d10fd4e9e15572508f10a873970c3b`; the first rule uses current
DECIDE syntax. The [baseline corpus](../03_correctness/baseline.md) records
current solver outputs and near misses before any feature code.

DES-01 through DES-07 are resolved as **first-build design decisions**, not as
implementation acceptance: use an explicit output-slot boundary; retain a
mandatory blocking validation dependency; keep parent work outside the global
rank; carry one structured decision record into physical explanation; expose
`off`/`auto`/`require`; use the finite DOUBLE score and integral capacity
contract; and begin implementation with the checks in [todo.md](todo.md).
The [experimental record](experiments.md) includes a failing naive boundary,
the repaired 28-assertion temporary probe, and current solver behavior. The
spike code was removed after gathering evidence, so only the baseline and
design docs are changed at this stage.

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
  covered selected parent/runtime cases, but the full production pass audit
  remains open as DES-08 and DES-09.

Research has established the intended boundary: an exact relational rewrite
avoids the solver altogether. The mathematical class work is consolidated in
[`decidb_direct_relational_rewrites.docx`](../decidb_direct_relational_rewrites.docx).
Existing pipeline profiling shows where a direct path might save work, but no
integrated direct-path speedup has been measured. No detector, prover, rewriter,
or direct selection is implemented on this branch as of 2026-09-30.
