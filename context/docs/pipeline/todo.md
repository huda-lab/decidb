# Pipeline — open architectural work

Changes that cross every stage. The plan and its rulings are in `context/opa_rewrite/design.md` and
`decisions.md`. How to work and the checks: [`../direct_solve/framework/todo.md`](../direct_solve/framework/todo.md)
§ How to work any task in these todo files.

## Phase 5 — the parser builds the tree (TREE-1 to TREE-9)

The design is `context/opa_rewrite/design.md` § 3; the rulings are R20 to R26 in `decisions.md`. The tasks run in
order, one at a time. Rules for every one of them:

- **Three moves, each its own commit, goldens identical after each:** (a) every reader of the carrier goes through
  an accessor; (b) the accessor reads a field, and the binder or rewrite writes it; (c) the tags are deleted.
- **No behaviour changes.** No answer, error, warning, `EXPLAIN` or `DIAGNOSE` text moves. If one must, stop and
  report it.
- **Execution stays vectorized** (R23): no task adds a loop over input rows.
- **The tree is built once** (design.md § 3.2.5): a task adds no pass that rebuilds or copies the clause list, and
  a rewrite touches only the clause it changes.
- **A home marked † in design.md § 3.5 is a proposal.** Confirm it in move (a), or stop and name another before
  move (b). No tag is kept for display.
- **A new `Expression` class owes DuckDB four things** (design.md § 3.3 contract 2): a case in
  `ExpressionIterator::EnumerateChildren`, a case in `LogicalOperatorVisitor::VisitExpression`, `IsFoldable()`
  false, and copy, equality, hash and generated serialization.

## TREE-7 — One lowering walk; `DecideNode` by kind

- **Problem.** Physical planning lowers each clause twice (`DecideTreeConstruction`, `LowerDecideTree`,
  `physical_plan/plan_decide.cpp:233`) and joins the two by expression address (`DecideTreeSources`,
  `retained_function_links`), with 81 internal errors checking that they agree. `DecideNode` is one struct of 60
  fields for four kinds, many of them copies of plan facts, and `DecideTree::Validate` checks that one kind's
  fields are unset on the others.
- **Change.** design.md § 3.2.4: one walk over the clauses and the helper definitions builds each node and its
  terms together; the solver gate and `CompileDecideTree` follow as today. The term splitter, like-term
  collection, the frame-column registry and the order of rows stay. `DecideNode` loses its copies and unread
  fields, then is split by kind. The three execution files change field access only.
- **Done when.** Goldens and model dumps identical on both solvers; the debug build reports nothing;
  `context/opa_rewrite/tools/ab_bench.py` against the build before the phase, no workload beyond about 10%;
  `DecideTreeSources` and `retained_function_links` are named nowhere; the tests that reached only a deleted
  cross-check are deleted; `FRAME_COLUMN_TAG` is named nowhere (design.md § 3.5: the walk makes the frame-column
  references and says which of them print as written); the `lizard` figures of `decide_tree.cpp` before and after
  are in the report.

## TREE-8 — Direct rules read `BoundDecide`

- **Decision needed.** The task opens with a short design the owner approves: which shared readers
  (`DirectRead*`) take a clause record, and which records of `DirectIR` are a view worth keeping. `DirectIR` is
  named about 130 times across the rules (`direct_rule.hpp:215`), and its front end reuses the solver path's
  readers (`direct_ir.cpp:56`, `:380`, `:430`, `:456`). Recommendation: the rules keep their shared readers, which
  take clause records and nodes; the front end and the copied records go.
- **Change.** R17: each rule reads the clauses and nodes; `DirectIR` and `DirectFrontEnd` go; `NULL_GUARD_TAG`'s
  second writer (`direct_ir.cpp:258`) goes with them.
- **Needs.** TREE-7.
- **Done when.** `test/decide/run_direct_matrix.sh` passes; goldens identical; `DirectIR` and `NULL_GUARD_TAG`
  are named nowhere.

## TREE-9 — The tag machinery goes

- **Problem.** With no tag written, the code that finds, strips and refuses tags has no reader.
- **Change.** Delete the tag section of `enums/decide.hpp` (the constants, `HasDecideTag`, `AddDecideTag`, the
  parsers, `DECIDE_TAGS`), tag stripping at renders, and the refusal of a user name that holds a tag (BIND-3,
  `decide_binder.cpp:49`) with its tests. The seven helper name prefixes stay as display names. Rewrite the stage
  docs (01 to 05, 08's planning part, `code_structure.md`, `glossary`) for the tree, in the present tense.
- **Needs.** TREE-8.
- **Done when.** `enums/decide.hpp` holds no tag constant; a decision named like a former tag is accepted and
  solves; the full matrix and a debug run pass; design.md § 3.9 holds.

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
- **Needs.** Phase 5.
- **Done when.** The owner approves the design and the phase's tasks are on the queue.
