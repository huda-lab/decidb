# Pipeline — open architectural work

Changes that cross every stage. The plan and its rulings are in `context/opa_rewrite/design.md` and
`decisions.md`. How to work and the checks: [`../direct_solve/framework/todo.md`](../direct_solve/framework/todo.md)
§ How to work any task in these todo files.

## Phase 5 — the parser builds the tree

The design is `context/opa_rewrite/design.md` § 3; the rulings are R17 and R20 to R32 in `decisions.md`. The review
each task answers is in `context/opa_rewrite/review/` (`AUDIT-TREE-1-3.md`, `AUDIT-TREE-4-6.md`,
`TREE-7-single-walk-sizing.md`, `TREE-8-design.md`). The tasks run in the queue's order, one at a time. Rules for
every one of them:

- **A fact has one home and one writer.** A task that gives a fact a typed home deletes every other carrier of it
  in the same change: a string compared by name, a null sentinel, a marker class, a copy on another record, a
  parallel check. No accessor layer stays behind.
- **The better form wins** (R27): cleaner code, no bloat, simpler, fewer states that must agree. A task that finds
  its stated form is not the better one stops and reports before building another.
- **No behaviour changes.** No answer, error, warning, `EXPLAIN` or `DIAGNOSE` text moves. If one must, stop and
  report it.
- **Execution stays vectorized** (R23): no task adds a loop over input rows.
- **The tree is built once** (design.md § 3.2.5): a task adds no pass that rebuilds or copies the clause list.
- **A new `Expression` class owes DuckDB four things** (design.md § 3.3 contract 2): a case in
  `ExpressionIterator::EnumerateChildren`, a case in `LogicalOperatorVisitor::VisitExpression`, `IsFoldable()`
  false, and copy, equality, hash and generated serialization.
- **The design follows the code in the same commit.** A task corrects the cells of design.md § 3 that describe
  what it builds; design.md never records a form the code does not have.
- **Tests** (R32): focused tests in each task's **Done when**; `make decide-test` once at the end of each queue
  group; every other suite in TREE-28.

## TREE-21 — A definition class per kind

- **Problem.** `DecideHelperDefinition` is one struct used six ways, its fields labelled by comment
  (`logical_decide.hpp:280-350`). `EnumerateExpressions` branches on the kind and `EnumerateBodies` has a three-way
  switch (`logical_decide.cpp:332-351,437-466`). `DecideHelperRef::kind` copies the entry's kind.
- **Change.** A base with the head fields and a derived class per kind, reached by `Cast<T>()`, with virtual
  `EnumerateExpressions` and `Bodies`, as `LogicalOperator` does. A field exists only on the kind that has it.
- **Done when.** No reader switches on the kind to pick fields;
  `./test/decide/run_tests.sh -k "abs or product or bilinear or norm or minmax or helper or definition"` and
  `build/release/test/unittest "[decidb]"` pass.

## TREE-22 — A definition owns its rows and its facts

- **Problem.** Every clause holds `helper_row`, `structural` and `defines`, while the tree's `Root(clause, owner)`
  already has the owner (`decide_tree_terms.hpp:163,204`, `decide_tree.cpp:613-616`). An L0 indicator's rows live
  on the definition (`links`); an ABS's and a definition's rows are made at lowering: two paths.
  `signed_helper_vars`, `aux_var_expressions` and `KeyedScopeInfo::value_link` are per-helper facts outside the
  table (`logical_decide.hpp:103,534,542`).
- **Change.** Each definition owns its rows in named members, made in one place for every kind. A row's role is
  "owned by definition i", read from the owner. The three side lists are fields of the definition.
- **Needs.** TREE-21.
- **Done when.** `helper_row` is named nowhere or only on the definition;
  `./test/decide/run_tests.sh -k "abs or norm or l0 or definition or explain or diagnos"` passes; a stored plan
  of a query mixing an ABS, an L0 count and a product pins the order of helper rows (R30).

## TREE-7 — Each atom of a clause is handed over with its node

- **Problem.** Two readers take each clause: `DecideTreeBuilder::Lower` collects what a node reads, and
  `DecideTermLowering` does the coefficient algebra with a splitter ten tree-less callers share. They are joined by
  `reducer_nodes`, a map from a reducer's address to its G (`decide_tree.cpp:426,607,778,794`,
  `decide_linear_form.cpp:717-746`): the same object must be handed over, the map cleared per root, and a miss
  dereferences `end()` in a release build.
- **Change.** R31, `review/TREE-7-single-walk-sizing.md` form A: `Root` and the objective read the additive spine
  once (`ReadCanonicalAtoms`), lower each atom, and hand the atom with its G to the term half; `Stamp` takes the G.
  The map, its clears and writes, the retained special case (`decide_tree.cpp:775-779`) and a clause root's stored
  `body` go. A kept call is an atom. The per-row arm splits the left side whole.
- **Done when.** `DecideReducerNodes` is named nowhere;
  `./test/decide/run_tests.sh -k "one_pass or like_term or frame or minmax or objective or bilinear"` passes; the
  probes of `review/tree7-probes/` and `review/tree6-probes/` are byte-identical; design.md § 3.2.4 says "one walk
  over each clause's atoms" and names the two readings of a reducer body.

## TREE-23 — `DecideNode` holds no copy; a G names its operand

- **Problem.** A node still copies facts: a root's `body` and `rhs`, `defined_var`, an F's `origin` and `text`, a
  linked node's `aggregate`, an O's `function` and `arguments` (grep evidence for the last two). A G's operand is
  found by scanning its children (`DecideTreeOperand`), with two internal errors.
- **Change.** Each field with no reader outside the builder goes; each copy is read from the definition or the
  clause record. A G holds its operand's index. A field judged dead on grep evidence is confirmed by a probe
  before it is deleted.
- **Needs.** TREE-7, TREE-22.
- **Done when.** `DecideTreeOperand` is named nowhere;
  `./test/decide/run_tests.sh -k "one_pass or frame or abs or minmax or helper or explain"` and
  `build/release/test/unittest "[decidb]"` pass; the probes are byte-identical.

## TREE-24 — `DecideNode` is a class per kind

- **Problem.** One struct serves four kinds; a field of one kind can be set on another, and
  `DecideTree::Validate` checks that it is not.
- **Change.** DuckDB's tagged hierarchy: a base with the common fields and `DecideScopeNode`, `DecideReducerNode`,
  `DecideFrameNode`, `DecideFunctionNode`, reached by `Cast<T>()` at sites that already test the kind. `Validate`'s
  cross-kind checks go. The execution files change field access only.
- **Needs.** TREE-23.
- **Done when.** `Validate` holds no cross-kind check; the `lizard` figures of `decide_tree.cpp` are reported;
  `./test/decide/run_tests.sh -k "one_pass or frame or abs or minmax or helper or explain or diagnos"` and
  `build/release/test/unittest "[decidb]"` pass; design.md § 3.2.4 describes the classes.

## TREE-25 — Terms split from the finished nodes

- **Decision needed.** After TREE-7 a reducer body is still read twice (reads, then algebra). One form reads a
  clause once: the term half splits the lowered nodes instead of the clause. It would remove the four internal
  errors of the frame registry's two dialects (`decide_linear_form.cpp:370`, `decide_tree_compile.cpp:372,377,398`),
  the `Translate`/`TranslateTree` pair and `LowerFrames`. It needs a tree-aware leaf in the shared splitter and
  touches message text and frame-column slots. It is not sized.
- **Options.** (1) Build it. (2) Keep two readings, stated in design.md § 3.2.4 as two analyses.
- **Recommendation.** Size it first: the task opens with a note (what goes, what the shared splitter gains, each
  order that moves) and the owner rules on it.
- **Needs.** TREE-24.
- **Done when.** The owner rules on the note; under option 1 the note's own Done-when holds.

## TREE-26 — The parser's reducer node holds its call; one AND splitter

- **Problem.** `DecideReducerExpression` wraps a whole parsed `call` and prints by splicing text after the first
  `(` (`decide_reducer_expression.cpp:13-17`); binders unwrap `.call` and cast it at five sites
  (`decide_binder.cpp:162,336-341,652,862`). An unscoped `a AND b` is split by the transformer
  (`transform_select_node.cpp:144-151`) and by the binder (`decide_constraints_binder.cpp:108-128`).
- **Change.** The node holds a `FunctionExpression` by type, as `WindowExpression` holds its fields, and prints its
  scope from the fields. The binder's splitter is the one splitter.
- **Done when.** No binder casts a reducer's call; `./test/decide/run_tests.sh -k "parse or syntax or when or
  between or scope"` and `build/release/test/unittest "[decidb]"` pass.

## TREE-8 — Direct rules read `BoundDecide`

- **Decision needed.** The approved design is `review/TREE-8-design.md`; its section "TREE-8 — form" raises three
  points the owner rules on before any code.
- **Options.**
  1. *The NULL guard's home.* A field beside the coefficient does not hold: like-term collection merges two guarded
     coefficients into one, and five readers have only the expression. (a) A typed home that is not a marker class
     (the form to be proposed). (b) A decorator class around the guarded value.
  2. *The decline tests* (`test_decidb_direct_decline.cpp`, 22 cases, about 76 edits of the IR). (a) Edits move to
     the operator; the 41 that set a derived part are settled one at a time, and an arm no tree can reach is
     deleted with its case. (b) A way to hand a rule edited parts, kept for tests.
  3. *Three whole-query facts* (a frame present, a keyed scope with a signature, a non-finite objective offset).
     (a) Each in the reader that meets it. (b) A two-line whole-query test in the driver.
- **Recommendation.** 1a, 2a, 3a.
- **Change.** R17: each rule reads the clauses and nodes; `DirectIR` and `DirectFrontEnd` go; `NULL_GUARD_TAG` and
  its two writers go.
- **Needs.** TREE-20.
- **Done when.** `DirectIR` and `NULL_GUARD_TAG` are named nowhere; `./test/decide/run_tests.sh -k "direct"` and
  `build/release/test/unittest "[decidb]"` pass; no rule fires or declines on a query where it did the other.

## TREE-9 — The tag machinery goes

- **Problem.** With no tag written, the code that finds, strips and refuses tags has no reader.
- **Change.** Delete the tag section of `enums/decide.hpp` (the constants, `HasDecideTag`, `AddDecideTag`, the
  parsers, `DECIDE_TAGS`), tag stripping at renders, `RebuiltAlias` and the splicing after it
  (`decide_product_form.cpp:386-415`), and the refusal of a user name that holds a tag (BIND-3,
  `decide_binder.cpp:49`) with its tests. The seven helper name prefixes stay as display names.
- **Needs.** TREE-8.
- **Done when.** `enums/decide.hpp` holds no tag constant; a decision named like a former tag is accepted and
  solves (`./test/decide/run_tests.sh -k "decision_name or tag"`).

## TREE-27 — The design and the stage docs state the system

- **Problem.** design.md § 3 records forms the code does not have or no longer has (the reducer's kind, the frame
  class, the value node, the NULL tests' home, the second shape writer, `constraint_sources` as deleted, the node
  as one struct). The stage docs describe carriers by tag.
- **Change.** design.md § 3 and § 3.5 match the code cell by cell; § 3.9's acceptance list is checked line by
  line. The stage docs (01 to 05, 08's planning part, `code_structure.md`, the glossary) describe the tree in the
  present tense. `direct_solve/architecture/ir.md` is the readers' contract.
- **Needs.** TREE-9, TREE-25.
- **Done when.** Every name design.md § 3 gives is found by `grep` in `src/`; § 3.9 holds line by line.

## TREE-28 — The test pass for the phase

- **Problem.** Since the phase's first commit the full suite has run on some commits only, and several behaviours
  the phase touches have no test that names them.
- **Change.** Tests first, each reaching the branch it names and failing without it: `when c: (a and b)` binds one
  constraint per comparison; a decision named like a former tag solves; a frame column and a child's result print
  as written in a physical plan; the stored plan of the mixed-helper query; a norm with a non-numeric body raises
  a norm's error; `sum(x)` and `sum(x) by (g)` are not merged. Then every gate, once: `make decide-test`,
  `DECIDB_VERIFY_SERIALIZER=1 make decide-test`, `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`,
  `DECIDB_FORCE_SOLVER=highs make decide-test`, `build/release/test/unittest "[decidb]"`, `make debug` with
  `build/debug/test/unittest "[decidb]"` and `make decide-test-debug` on a quiet machine,
  `test/decide/run_direct_matrix.sh`, `make format-check`, and `context/opa_rewrite/tools/ab_bench.py` against the
  build of the commit before the phase.
- **Needs.** TREE-27.
- **Done when.** Every gate passes with goldens and model dumps identical on both solvers; no workload of the
  benchmark is beyond about 10%; the `lizard` figures of the three construction files before and after are in the
  report.

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
