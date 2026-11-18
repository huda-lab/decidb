# Operations — open work

Upkeep of the repository as a whole: releases, packaging and the docs across areas.

## OPS-8 — Docs that cite what does not exist

- **Problem.** `pipeline/architecture.md` lists seven edits to core DuckDB; ten exist (CTE column pruning in
  `remove_unused_columns.cpp`, `LogicalExtensionOperator::PruneUnusedOutputs`, the late-materialization call in
  `optimizer.cpp`). `08_execution/done.md`, `o1_monge_assignment/done.md` and `testing/coverage.md` cite three tests
  that do not exist. The direct-solve `done.md` files stamp 6,430 to 7,288 passed as "the baseline".
- **Change.** Each doc states what exists; one baseline stamp, the latest full run.
- **Done when.** Every test and file the touched docs cite exists (`grep` each one).
