# Design — verified foundation

This is source-grounded context, **not** a claim that direct solve is shipped.

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
- `LogicalWindow` can supply an unpartitioned `ROW_NUMBER()`. Ordinary SQL
  experiments kept an outer filter above a global window and raised an
  always-true-or-throw validation filter even when `x` was unprojected. These
  observations do **not** prove the generated bound plan's optimizer behavior.

Research has established the intended boundary: an exact relational rewrite
avoids the solver altogether. The mathematical class work is consolidated in
[`decidb_direct_relational_rewrites.docx`](../decidb_direct_relational_rewrites.docx).
Existing pipeline profiling shows where a direct path might save work, but no
integrated direct-path speedup has been measured. No detector, prover, rewriter,
or direct selection is implemented on `master` as of 2026-09-29.
