# Pipeline — open architectural work

Changes that cross every stage. The plan and its rulings are in `context/opa_rewrite/design.md` and
`decisions.md`. How to work and the checks: [`../direct_solve/framework/todo.md`](../direct_solve/framework/todo.md)
§ How to work any task in these todo files.

## ARCH-2 — Design Phase 6: the model is written at the nodes

- **Decision needed.** Two choices before the design (`context/opa_rewrite/design.md` § 4):
  1. Constructs the deck omits (ABS, products, L0, `<>`, AVG, norms, MIN/MAX beside other terms, quadratic terms).
     Recommendation: a node rule for each, presented as a departure (R-D).
  2. The contract with the model builder: helpers allocated at run time (names in `EXPLAIN` and `DIAGNOSE`
     change), inlining at the node or by a whole-model pass, rows per group or per input row. Recommendation: per
     group, decided at the node, allocated at run time; measure inlining against solver presolve before keeping a
     whole-model pass.
- **Change.** A design for the deck's step 5 with the constraints of design.md § 4, the old and new model building
  compared side by side while both exist, and the tasks of the phase, each filed with an id.
- **Constraint.** Evaluation stays vectorized: a coefficient, a key and a data fold are computed by programs over
  chunks, as today. A node writes model rows per group, never per input row, and no array as long as the input is
  built for the model. Check: `context/opa_rewrite/tools/ab_bench.py`, and `repro/clauses_times_rows.sql` no slower
  in evaluation time or peak memory than the build before the phase.
- **Read first.** `context/opa_rewrite/review/TREE-25-sizing.md` (R37): whether terms are split from the finished
  nodes is decided here, with the model contract.
- **Done when.** The owner approves the design and the phase's tasks are on the queue.
