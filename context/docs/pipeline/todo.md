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

## TREE-3 — Clause records

- **Problem.** The constraints are one AND expression. A clause's scope is a tagged wrapper above it
  (`decide_constraints_binder.cpp:216`), its origin a `__source_clause_<i>__` tag beside `constraint_sources`, and
  its shape, easy MIN/MAX and folded bound are tags on the comparison. A rewrite adds a row by wrapping the whole
  tree in a new AND (`logical_decide.cpp:271`). The canonicalizer walks the conjunction at 12 sites and the
  rewrites reach clauses through `EnumerateBodies` (19 calls) and `decide_constraint_walk.hpp`.
- **Change.** `vector<BoundDecideClause>` on `BoundDecide`, the objective one more clause (R22). A clause holds its
  scope (`DecideClauseScope`), body, origin (`DecideRowOrigin`, the `BETWEEN` half, the role of a row a rewrite
  adds and the helper it defines) and its facts: shape, grouping key and variant signature, set once by the
  canonicalizer (R26; `RewriteScopedReducers` keeps its one write to the shape until TREE-6), easy MIN/MAX, folded
  bound. `AddConstraint` appends a record. The norm pass and the AVG pass canonicalize the clauses they change and
  stop sending every constraint through the canonicalizer again (`decide_rewrite_norm_in.cpp:147`,
  `decide_rewrite_notequal_avg.cpp:84`); if a golden moves because a later pass relied on that second
  canonicalization, stop and report which fact it relied on. Bound reduction records the direction and shape it
  relied on, and a debug build checks that record in `RunPass` (`decide_optimizer.cpp:66`). `EXPLAIN` and
  `DIAGNOSE` print from the records. `DecideEnclosingScope` goes.
- **Done when.** Goldens identical, `EXPLAIN` text included; the DIAGNOSE tests, the serializer mode and the direct
  matrix pass; the 14 tags design.md § 3.5 lists for TREE-3 are named nowhere; no `BoundConjunctionExpression`
  joins clauses.

## TREE-4 — Reducer node and value node

- **Problem.** A reducer is a `BoundAggregateExpression` whose alias carries its `by` key, its own `per`, a norm's
  order and bound, and rewrite marks; `norm` is a SUM with a patched return type (`decide_binder.cpp:1232`). A
  flattened subquery or a recorded bound reducer is a leaf told apart by a provenance tag, read as far as stage 08
  (`decide_value_extraction.cpp:324`). DuckDB's statistics pass reads `SKIP_NULL_TAG`
  (`propagate_decide.cpp:8`).
- **Change.** `BoundDecideReducerExpression` and `BoundDecideValueExpression` (design.md § 3.2.2), each with the
  four obligations. The reducer holds its kind, the operand's scope, `arguments`, its `by` key, a function-only
  aggregate (as `DecideBoundAggregate` does, `logical_decide.hpp:226`), its written name, the count scale of a
  rewritten AVG, and its NULL tests apart from the written `when`. The binder builds it
  (`decide_binder.cpp:848`); `DecideReducerGroupKey` and its neighbours read fields.
- **Needs.** TREE-3.
- **Done when.** Goldens identical; the serializer mode passes; `make decide-test-debug` and the debug C++ tests
  report nothing; a C++ test of copy and equality for each class; the 15 tags design.md § 3.5 lists for TREE-4 are
  named nowhere.

## TREE-5 — Frame node

- **Problem.** A frame is a tagged aggregate (`first(e)`, or the range's reducer) whose alias carries an index
  into `frames` (`decide_binder.cpp:1065`), told from an ordinary aggregate by `TryGetDecideFrame` and
  `ContainsDecideFrame` at each site. A data reducer that reads a frame, a frame column and a clause's tightest
  frame bound are three more tags.
- **Change.** `BoundDecideFrameExpression`: a frame index and the payload, with the four obligations. The `frames`
  table stays (R25). The guards become class checks. The tightest bound is a clause field; a data reducer names
  its `frame_aggregates` record by a field.
- **Needs.** TREE-4.
- **Done when.** Goldens identical; the frame tests and the serializer mode pass; `FRAME_TAG_PREFIX`,
  `FRAME_COLUMN_TAG`, `FRAME_AGGREGATE_TAG_PREFIX` and `FRAME_TIGHTEST_BOUND_TAG` are named nowhere.

## TREE-6 — Function nodes and one table of helper definitions

- **Problem.** An ABS, a Boolean product, an L0 count and a nonlinear body are each a tag on the kept call that
  carries an index into its own table (`abs_functions`, `bilinear_function_groups`, `l0_indicator_functions`,
  `definition_functions`), and a lifted reducer is a record in `reducer_value_links`. Five tables are listed by
  hand in `EnumerateExpressions` (`logical_decide.cpp:302`) and registered one by one when the tree is built
  (`decide_tree.cpp:182`).
- **Change.** `BoundDecideFunctionExpression`: kind and the index of its definition. One table of helper
  definitions on `LogicalDecide` owns each helper's body (R25): kind, the helper decision, the body or reducer
  node, and the facts joined over its readers (`push` as one `ExtremumPush`, whether an ABS is exact, the
  innermost-first order). A reader and a copy of a clause hold the index. After this step a clause's shape has one
  writer.
- **Needs.** TREE-5.
- **Done when.** Goldens identical; `EXPLAIN` and DIAGNOSE tests pass; the 9 tags design.md § 3.5 lists for TREE-6
  are named nowhere; one loop enumerates the definitions.

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
- **Needs.** TREE-6.
- **Done when.** Goldens and model dumps identical on both solvers; the debug build reports nothing;
  `context/opa_rewrite/tools/ab_bench.py` against the build before the phase, no workload beyond about 10%;
  `DecideTreeSources` and `retained_function_links` are named nowhere; the tests that reached only a deleted
  cross-check are deleted; the `lizard` figures of `decide_tree.cpp` before and after are in the report.

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
