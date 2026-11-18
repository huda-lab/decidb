# Pipeline — open architectural work

Changes that cross every stage. The plan and its rulings are in `context/opa_rewrite/design.md` and
`decisions.md`. How to work and the checks: [`../direct_solve/framework/todo.md`](../direct_solve/framework/todo.md)
§ How to work any task in these todo files.

## ARCH-1 — Design Phase 5: the parser builds the tree

- **Decision needed.** Two choices before the design (`context/opa_rewrite/design.md` § 3):
  1. Order inside the phase. Recommendation: front to back (parser, binder, canonicalizer, rewrites), with an
     adapter that lowers the new bound tree to today's tagged expressions for the stages not yet converted; its
     boundary moves back one stage at a time and it goes at the end (strangler fig). The branch runs throughout.
  2. Shape of the clause list, one AND expression with tagged wrappers today. Recommendation: a
     `vector<DecideClause>` (body, `when`, `per` key, source id, shape), the objective one more clause.
- **Change.** A design that states the five contracts of design.md § 3, the node classes (parsed and bound, as
  DuckDB's `ParsedExpression` and `Expression`), and the tasks of the phase, each filed with an id. It covers: every
  tag of `enums/decide.hpp` replaced by a node, record or field (R20), the tag refusal of BIND-3 deleted with the
  last one; direct rules reading the tree and `DirectIR` deleted (R17); the code design.md § 7 lists for this phase.
  An architect review (`decidb-architect`) before the owner approves it.
- **Done when.** The owner approves the design and the phase's tasks are on the queue.

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
- **Needs.** Phase 5.
- **Done when.** The owner approves the design and the phase's tasks are on the queue.
