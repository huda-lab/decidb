# Planned design changes

Two changes follow Phases 0 to 4 of the queue: the tree becomes the one representation of a DECIDE clause (Phase 5), and
the solver model is written at the tree's nodes (Phase 6, the deck's step 5). Rulings: [decisions.md](decisions.md).

## 1. What the deck requires

The deck is `context/New_syntax.pdf`; pages 77 to 119 give the algorithm. `repro/deck_example.sql` is its running
example.

| Step | Rule | Pages |
|---|---|---|
| 1. The tree | Nodes are E (a scoped expression: `when`, `per`), G (a reducer: function, `by`) and O (a frame). The operators of the written expression (`<=`, `+`, `*`) are nodes too. An E's signature σ is its `per` key, its direct tuple terms and its direct `by` keys. An inner reducer groups inside its enclosing group | 28-30, 80-81 |
| 2. Top-down | Each joined row is routed from the root once. At an E: test `when`; if it holds, insert σ into the container and pass the row down. At a G: take the `by` key and route into that key's operand | 83-91 |
| 3. Bottom-up | Each node is computed from its children's containers. A G over data folds to a value; a G over decisions composes an expression (`max(x1, x2)`, `x3 + max(x5, x6)`). The root composes one row per σ | 93-97 |
| 4. Frames | An O is filled top-down by `within`, then `over`, then the payload's signature. Bottom-up, `at(previous)` shifts each timeline; the root drops rows that read no value | 99-108 |
| 5. The model | Written at the nodes, bottom-up. A SUM passes its members up. A MAX allocates one variable per group that has more than one member | 115, 119 |

Containers (p. 110): a G is a map from `by` key to its operand E; an E is a vector of signatures for `per row` and
a set of distinct signatures for `per K` and `per ()`; an O is a map from `within` to an ordered map of `over`.

## 2. Where the branch stands

| Step | On the branch |
|---|---|
| 1 | The tree exists, with a fourth node kind, F (ABS, Boolean product, L0, definition). It is built at physical planning, after every rewrite, so the operator nodes are gone and a node's body is stage 05's lowered terms |
| 2 | Built. `Sink` buffers each thread's rows; `Finalize` routes the whole buffer through one descent |
| 3 | Built for data values. A G over decisions holds a helper variable made at planning, not an expression |
| 4 | Built |
| 5 | Not built. Every nested decision reducer becomes a helper variable at planning (`AddValueHelper`); the optimizer chooses which are written into their readers (`ChooseHelperForms`) and stage 06 writes them in (`WriteHelpersIn`). The models still match the deck's on its example |

One clause exists in eight forms between planning and the solver: the bound expression with its marker strings and
side tables on `LogicalDecide`; the tree's nodes; the linear-form records inside the nodes; compiled programs;
the descent's containers; program value columns; the emitter's output; `SolverInput`. The deck has one, the tree.

## 3. Phase 5: the tree is the representation

**Target.** The parser builds the structure and every later stage works on it. The tasks are TREE-1 to TREE-9
(`context/docs/pipeline/todo.md`).

### 3.1 The change in one picture

Today a clause is an ordinary SQL expression, and what makes it a DECIDE clause rides beside it:

```text
parser      fake function calls named by a tag            (5 tags)
binder      one AND expression; `when`/`per` are tagged wrappers; reducers and frames are tagged aggregates
            + tables: keys, frames, constraint_sources
rewrites    more tags, + tables: value links, ABS, products, L0, definitions, boxes   (55 tags in all)
planning    two lowerings read the tags and tables and build the tree, joined by expression address
execution   reads the tree
```

After the phase the structure is built once, by the parser, and each stage transforms it:

```text
parser      declarations, clause records, reducer and frame nodes
binder      the same records and nodes, bound
rewrites    transform the records and nodes; a rewrite's result is a node, a record or a field
planning    one walk lowers the clauses to the tree execution runs
execution   reads that tree, unchanged
```

No stage finds structure by searching an alias or by a child's position. [measured] 55 tag constants, 269 uses
outside their header; execution and model building hold 2 of them.

**One rule for shared things.** A thing with identity that several places read is a record in a table on
`BoundDecide`, and a node names it by a typed index: a key, a frame, a helper's definition. Today the index
travels inside an alias string; after the phase it is a field. The tables stay.

### 3.2 The structure

The deck's grammar (p. 28) is `E ::= [when θ] [per K] : B`, `G ::= agg(E) [by (Γ)]`, `B ::= f(e…, G…)`. A scope E
never stands alone: it is a clause's root or a reducer's operand. A body B is an ordinary expression whose leaves
are columns, decisions, reducers and frames.

**Pattern.** One structure in two typed phases, a parsed class and a bound class, as DuckDB's `ParsedExpression` and
`Expression` are. A postfix `by` makes a reducer node as a postfix `OVER` makes DuckDB's `WindowExpression`
(the deck draws the same analogy, p. 28).

#### 3.2.1 Parsed (stage 01)

| Thing | Kind | Holds | Replaces |
|---|---|---|---|
| `DecideScope` | struct | `when`; `per` as written: absent, `row`, `()`, or a key list; location (of its `per`, else of its `when`) | `__scope_when__`, `__scope_per__`, `__scope_per_row__` calls; "one child means `per ()`" |
| `DecideDeclaration` | struct | name, type (INT, REAL, BOOL), its `per`, location | a comparison of the name with a string constant (`'integer_variable'`) |
| `DecideClause` | struct | `DecideScope`, body | one item of the AND expression |
| `ParsedDecide` | struct | the declarations, the constraints' clauses, the objective clause with its sense, the source text | five `decide_*` fields of `SelectNode` |
| `DecideReducerExpression` | `ParsedExpression` | the call as SQL parses it, the operand's `DecideScope`, `by` (absent, `()`, or a key list) | `__reducer_by__`, a scope marker as a call's first argument |
| `DecideFrameExpression` | `ParsedExpression` | as today | — |

`SelectNode` holds one `ParsedDecide` (`decide`), null for a query with no DECIDE. A clause has no location of its
own: its body and its scope each have one.

- A reducer node is made only where DECIDE syntax is written (a scope inside the call, a `by`).
  A plain `sum(x)` stays a `FunctionExpression`, and the binder makes a bound reducer of both, because whether a
  name is an aggregate is the catalog's to say.
- The reducer node follows `DecideFrameExpression`, which has every piece: copy, equality, hash, `ToString`,
  generated serialization (`parsed_expression.json`), child enumeration (`parsed_expression_iterator.cpp`). The
  structs have copy, equality, `ToString` and generated serialization (`nodes.json`).
- `ParsedDecide` has one `EnumerateExpressions`, the declarations' keys included. Macro substitution
  (`ParsedExpressionIterator::EnumerateQueryNodeChildren`) goes through it, and `ParsedDecide` has one `Copy` and
  one `Equals`.

#### 3.2.2 Bound (stage 02 onward)

| Thing | Kind | Holds | Replaces |
|---|---|---|---|
| `BoundDecide` | struct | decisions with their scopes, keys, keyed scopes, frames, clauses, the objective clause, sense, constant offset, source text | 13 fields copied one by one from `BoundSelectNode` to `LogicalDecide` |
| `BoundDecideClause` | struct | scope, body (the comparison), origin, and the facts of § 3.3 contract 4 | an AND child, its wrapper tags, `constraint_sources[i]` |
| `BoundDecideReducerExpression` | `Expression` | its `kind` (`DecideReducerKind`: SUM, AVG, MIN, MAX, COUNT, NORM), set where the node is built and read by every reader; the bound aggregate function DuckDB runs for it, with its bind data (none for a NORM, a norm no definition has replaced, which its `norm` field names with its order and bound; the binder binds a norm's body as a scalar and takes its type from the norm's definition). An AVG is an AVG from the binder to the linear form, which scales its terms (R33). A helper definition (`reducer`), a tree G or range O (`aggregate`), a frame (`DecideFrameSpec::reducer`), a value link and a direct part hold the same enum, `children`, `filter` (its `when` and its members' NULL tests, each a `BoundDecideNullTestExpression`), `by` key index, own `per` scope index, written name, and what the rewrites record (`holds_reducer`). A frame is no reducer | a `BoundAggregateExpression` with `__group_key_`, `__qualified_by_`, `__decide_norm_`, `__written_norm_` tags; `norm` as a SUM with a patched return type |
| `BoundDecideFrameExpression` | `Expression` | an index into `frames` and its children: the payload, and the key a collated MIN or MAX range compares. A range's reducer is `DecideFrameSpec::reduce`, the bound aggregate over the children's positions, held once in the frames table; a point has none, and no `first` is bound. The node has no `by`, `per`, norm or filter, so a reader that means a reducer tests the reducer class and one that means a frame tests this one (`TryGetDecideFrame`). `DecideFrameFold` builds the executable aggregate from the two where a frame is lowered (R28) | a `BoundDecideReducerExpression` whose alias carries `__frame_<i>__` |
| `BoundDecide::computed_values` | table of `DecideComputedValue` | a computed value read whole, found by equality with its `value`: whether it is one value for the whole query, and whether it is a flattened subquery. It holds the subquery placeholders only. A recorded reducer over data stores nothing here: its record says it (`DecideBoundAggregate`, whole-query `key`, `IsReadBy`), so the fact does not depend on which clause read the reducer first. A bound that reads a record stays data (`query_wide_bound` is false), so a repair edits it as data. No node wraps the read | `__query_wide_value__`, `__row_varying_subquery__`, `__bound_reducer__` on a leaf |

| `BoundDecideNullGuardExpression` | `Expression` | `guard`, the COALESCE or CASE that reads a NULL coefficient as no term, and `Value()`, the coefficient it guards (a Decorator, R39). The linear form builds it (`DropNullObjectiveTerms`); a reader tests the class (`DecideIsGuarded`) and reads through it (`DecideUnguarded`); the executor runs `guard` | `__null_guard__` on the guard's alias |

- **Existing types are reused.** A clause's scope is today's `DecideClauseScope` (`decide_tree_terms.hpp:107`) and
  its origin is today's `DecideRowOrigin` (`decide_source_info.hpp:203`), held by the clause from binding on.
  `DecideEnclosingScope`, a pair of pointers to wrapper nodes, goes.
- **`BoundDecide` is moved whole**: binder → `LogicalDecide` → physical planning. `LogicalDecide` keeps what is the
  operator's own: `decide_index`, `optimized`, the solver choice, `value_checks`. The objective's texts
  (`written_objective`, `canonical_objective`, `objective_canonical_differs`), `source_columns` and the reducer
  records (`bound_aggregates`) are members of `BoundDecide`, which is the one argument a `DecideCanonicalizer` is
  built from.
- **`constraint_sources` keeps what outlives a rewritten clause.** `between_partner`, `between_upper` and
  `qualifier` are read by diagnostics after a rewrite has replaced the clause, so the table holds them.
  `repair_group_id` is a field of the shared `DecideRowOrigin`, read on provenance rows. The clause facts only the
  optimizer reads (`structural`, `minmax_easy`, `folded_bound`, `folded_clause_rows`,
  `tightest_frame_bound`, `removal_group_id`) are not serialized.
- **A reducer node keeps what the aggregate binding gives.** [read] `FunctionBinder` picks the overload, the return
  type and the argument casts for a decision reducer too (`decide_binder.cpp:848`), a collated MIN or MAX has two
  arguments (`decide_binder.cpp:1122`), and execution folds a data reducer with the bound function and its bind data
  (`decide_tree_columns.cpp:32`). So the node holds `arguments` and a function-only aggregate, as
  `DecideBoundAggregate` does today (`logical_decide.hpp:226`).
- **The frames table stays**, as the key table does. [read] One frame is one O node however many places read it
  (`decide_tree.cpp:1338`), and signatures, helpers and frame columns hold its index.

#### 3.2.3 What the rewrites add

A rewrite changes a node or a record; it leaves no tag.

| Today | After |
|---|---|
| ABS, Boolean product, L0, definition: a tag on the kept call carrying an index into `abs_functions`, `bilinear_function_groups`, `l0_indicator_functions`, `definition_functions` | The kept call, a `BoundFunctionExpression`, holds a typed reference (`definition`: kind and index) into the one table `LogicalDecide::helper_definitions`, whose entries are a class per kind over the base `DecideHelperDefinition` (`Cast<T>()`, virtual `EnumerateExpressions` and `Bodies`); there is no wrapper node, since the kept call is still matched by shape (`context/docs/pipeline/03_logical_plan/done.md`). A product is two kinds: the kept call's expansion (`PRODUCT`) and each Boolean product's helper (`BOOLEAN_PRODUCT`) |
| A reducer replaced by a helper's column, its definition in `reducer_value_links` | Unchanged in shape: the reader holds the helper's column, the `REDUCER_VALUE` definition holds the reducer node once |
| What a row is to the helper it defines (an ABS envelope, an L0 bound or tolerance row, a body definition): a tag on the row | Read from the row's owner: the kind of the definition that states the row and the row's place among its rows (`DecideHelperDefinition::Rows`). A clause holds no role |
| Rows a rewrite adds: appended by wrapping the whole tree in a new AND; role in `__decide_structural_constraint__`, `__defines_<i>__`, `__objective_row__` | A `BoundDecideClause` appended to the vector, its origin saying what it is |
| Shape, easy MIN/MAX, folded bound, tightest frame bound: tags on the comparison | Fields of the clause |

- **A helper is defined once.** [read] One value link is shared by every clause that reads the same reducer, and
  the rewrites copy clauses (`decide_rewrite_minmax.cpp:136`). If each reading node held the body, a later rewrite
  would make one auxiliary per copy. So the definition owns the body, and a reader or a copy holds only its index.
- The five definition tables become one table of helper definitions with a kind, read by one loop where
  `EnumerateExpressions` and the tree's construction have five today.
- Helpers are allocated at planning, in this phase and after it (R45).

#### 3.2.4 The tree execution runs

`DecideTree` and `DecideNode` stay the executor's structure. Physical planning derives them from `BoundDecide` in
three steps, where column bindings are final (`plan_decide.cpp:224`):

1. **One walk over each clause's atoms.** A clause makes its root E, and the walk reads the atoms of its left side
   once (`ReadCanonicalAtoms`), lowers each (`LowerAtoms`), and hands the atom with the G its reducer became to the
   term half (`DecideLoweredAtom`, `DecideTermLowering`), so a term names its G because it was given it. A reducer
   node makes a G and its operand E; a frame index makes its O once; a helper definition makes its F and the rows it
   states. A reducer body has two readings by two readers (R31): what its node reads, which the walk collects, and
   its coefficient algebra, which the term half does with the splitter the rewrites share. A per-row clause's left
   side is split whole, since the splitter keeps a decision-free sum as one constant term. A root stores no lowered
   body: the decisions its atoms read are its `decisions`. The helpers' members are split in a later loop of the
   same construction. The frame columns are the tree's, made as each reader asks for one.
2. **The solver gate** (`RequireDecideSolverSupport`), as today.
3. **`CompileDecideTree`**, today's program compiler, unchanged.

- Gone: the second lowering, the address maps that join the two (`DecideTreeSources`, `retained_function_links`,
  `DecideReducerNodes`),
  and the checks that they agree. [read] 81 internal errors in the three construction files.
- Kept, because they serve more than the join: the term splitter (three rewrites and the direct rules use it),
  like-term collection, and the order of rows the model dumps record.
- `DecideNode` holds no copy of a plan fact and no unread field: the tree holds the helper definitions and a node
  names its own by index. A root's bound, origin and defined helper are read from its clause record
  (`DecideConstraint`), an F's from its definition, a linked G's or a range O's reducer from the definition or the
  frame (`DecideTree::ReducerOf`). A G holds its operand's index (`operand`, `DecideTree::OperandOf`).
- A node is a class per kind over the base `DecideNode`, DuckDB's tagged hierarchy: `DecideScopeNode` (E: `arguments`,
  `merges_classes`, `folded`, `value_check`), `DecideReducerNode` (G: `function`, `aggregate`, `qualifier_scope`,
  `operand`), `DecideFrameNode` (O: `frame`, `over`, `fallback`) and `DecideFunctionNode` (F: `rows`). A reader that
  has tested the kind uses `Cast<T>()`, which raises on another kind; one that reads any node uses `TryCast<T>()`. A
  field of one kind cannot be set on another, so `Validate` holds no cross-kind check.
- Programs, layouts, containers and the three execution files do what they do today.

#### 3.2.5 The tree is built once

| When | What happens | Per query |
|---|---|---|
| Parsing | the nodes are built | once |
| Binding | each parsed node becomes its bound node: names, types and keys resolved | once |
| Rewrites | a rewrite changes the clause or node it applies to, in place | no rebuild |
| Physical planning | the lowering walk derives the executor's tree | once |

- **Three forms, each derived forward once.** They are DuckDB's own three: `ParsedExpression`, `Expression` and the
  executor's `ExpressionState`; a statement, a `LogicalOperator` and a `PhysicalOperator`. No stage reads a later
  form back, and no form is built a second time.
- **A rewrite touches the clause it changes and no other.** A pass that changed a clause canonicalizes that clause.
  No pass moves the whole clause list out and back. [read] Today the norm pass and the AVG pass send every
  constraint through the canonicalizer again (`decide_rewrite_norm_in.cpp:147`,
  `decide_rewrite_notequal_avg.cpp:84`), which copies every wrapper node (`decide_canonicalizer.cpp:1653`), and
  each added row wraps the whole tree in a new AND (`logical_decide.cpp:271`).
- **The executor's tree is not a copy of the bound tree.** It is the deck's tree with its run-time parts: a reducer
  node becomes a G and its operand E, a frame index becomes one O, and each node gets programs over column
  positions, which exist only at physical planning.

### 3.3 The five contracts

1. **Meaning apart from execution state.** `BoundDecide` holds meaning: scopes, keys, decisions, expressions,
   locations. The executor's tree holds execution state: programs, column positions, layouts, slots. The boundary is
   the lowering walk. No planning stage reads the executor's tree.
2. **DuckDB can see every expression.** A new `Expression` class owes DuckDB four things:

   | Obligation | Where | Why |
   |---|---|---|
   | A case in `ExpressionIterator::EnumerateChildren` | `expression_iterator.cpp:96` | every generic walk reaches its children, as `BoundWindowExpression`'s |
   | A case in `LogicalOperatorVisitor::VisitExpression` | `logical_operator_visitor.cpp:185` | [read] an unknown class is an internal error there, and column binding and pruning go through it |
   | `IsFoldable()` false | `expression.cpp:83` | [read] the default is "all children foldable", so `sum(1)` would read as a constant; DECIDE code asks it |
   | Copy, equality, hash, serialization | `expression.json` | contract 3 |

   The NULL guard is the exception to the third row: it is a Decorator, foldable exactly when the expression it
   holds is, which is the default.

   `BoundDecide` has one `EnumerateExpressions`: decisions, keys, frames, clauses, helper definitions.
   `LogicalDecide`'s adds its own `value_checks`. [read] Four DuckDB passes read it (column
   binding, column pruning, binding replacement, statistics), and the binder's `PlanSubqueries` lists the same
   fields by hand; it uses the one function. The DECIDE optimizer runs before join ordering and column pruning
   (`optimizer.cpp:147`), so nodes hold column bindings throughout planning. The debug build's
   `ColumnBindingResolver::Verify` after every pass is the check.
3. **Copy, equality, serialization.** Parsed classes: as `DecideFrameExpression`. Bound classes and records:
   generated from `expression.json` and `nodes.json`. [read] Only a plan the DECIDE optimizer has not run on is
   serialized (`SupportSerialization` returns `!optimized`), so what the binder and the canonicalizer build is
   serialized, and what the rewrites add (a kept call's reference, helper definitions) is not, as
   `BoundSubqueryExpression` is not. A prepared statement keeps its physical plan and so its executor tree.
4. **What each rewrite invalidates.** Three kinds of fact, each with one rule.

   | Kind | Facts | Rule |
   |---|---|---|
   | A fact of the canonical clause | shape (value or aggregate), its grouping key | a function of the canonical body and the scope with one writer, `ClassifyClause`, which the canonicalizing boundary calls each time it canonicalizes a clause, a function-owned row or the objective (R29). No reader recomputes it. The variant signature is set by the binder and stored |
   | A fact joined over a helper's readers | the sides a MIN or MAX helper needs (`push`), whether an ABS is exact, the innermost-first order of definitions | stored on the definition; a rewrite that adds a reader widens it |
   | Computed when asked | degree | never stored, as today |

   One transformation relies on a fact a later pass can change: bound reduction reads the comparison's direction and
   shape before the scoped-reducer pass runs (`decide_rewrite_scoped.cpp:565` states the hazard). It records what it
   relied on, on the clause, and a debug build checks that record after each pass, in the one place every pass runs
   (`RunPass`, `decide_optimizer.cpp:66`). The scoped-reducer pass ends the record on a clause whose every reducer
   becomes a helper: the helper is one instance per group, so each row states it against the bound its group read.
   Nothing else is verified this way.
5. **An error is raised where it is found.** Every node and record carries its query location, so the expression in
   hand has one and nothing is built to supply it.

### 3.4 The steps

One carrier at a time across every stage (branch by abstraction). Each step has three moves, each its own commit
with goldens identical: (a) every reader goes through an accessor; (b) the accessor reads a field, and the binder
or rewrite writes it; (c) the tags are deleted. [read] Much of (a) exists: `EnumerateBodies` (19 calls) hands each
pass a clause's scope, and a reducer's `by` field and `TryGetDecideFrame` give a reducer's key and a frame node's index.

The steps run in order, one at a time: each touches files the next one touches.

| Task | Carrier | Stages touched | Deletes |
|---|---|---|---|
| TREE-1 | Parser nodes (§ 3.2.1) | 01, 02 | 4 parser tags, `decide_parsed_markers`, the string-typed declarator |
| TREE-2 | `BoundDecide`, moved whole | 02, 03, planning | the field-by-field copies at each hop |
| TREE-3 | Clause records | 02–05, planning, 08's readers, direct solve, EXPLAIN, DIAGNOSE | the AND tree, its wrappers, `constraint_sources`, 14 tags |
| TREE-4 | Reducer node and value node | 02–05, DuckDB's statistics pass, planning, 08's readers, direct solve | 15 tags; `norm` as a patched SUM |
| TREE-5 | Frame field | 02–05, planning | 3 tags |
| TREE-6 | A typed reference on the kept call and one table of helper definitions (§ 3.2.3) | 05, planning | 9 tags; the five definition tables |
| TREE-7 | One lowering walk; `DecideNode` by kind (§ 3.2.4) | planning, 08 (field access only) | the second lowering, the address join |
| TREE-8 | Direct rules read the bound operator (R17); the NULL guard node (R39) | direct solve, 05 | `DirectIR`, its front end and the driver's whole-query gate; the last tag |
| TREE-9 | The tag machinery goes (R20) | all | the tag section of `enums/decide.hpp`, `DECIDE_TAGS`, the refusal of a name that holds a tag (BIND-3), tag stripping at renders; docs |

**TREE-8's form** is § 5: a rule reads `LogicalDecide` and calls the shared readers on demand
(`context/docs/direct_solve/architecture/readers.md`).

**Checks.** Each commit: `make decide-test`, goldens identical. TREE-2 to TREE-4 add the serializer mode. TREE-4 is
the first step with a new `Expression` class under DuckDB's passes, so it also runs the debug build. TREE-7 runs
the debug build and `ab_bench.py`. TREE-8 runs the direct matrix. The phase ends with the full matrix and a debug
run.

**Tests at each boundary** (the phase's accepted-when):

| Boundary | Test |
|---|---|
| Parser | each new class: parse, print, parse again; copy, equality, serialization round trip (C++) |
| Binder | serializer mode over the suite; a C++ test of copy and equality for each bound class |
| Rewrites | goldens; the debug check of contract 4 |
| Direct solve against its fallback | the direct matrix (auto and off) |
| Parallel execution | `test_descent_merge.py`, unchanged |

### 3.5 The tag checklist (R20)

Every tag constant, its home, and the step after which it is gone. A tag with two carriers is listed under both and
goes with the later one.

| Gone after | Tags | Home |
|---|---|---|
| TREE-1 | `SCOPE_WHEN`, `SCOPE_PER`, `SCOPE_PER_ROW`, `REDUCER_BY` | `DecideScope`, `DecideReducerExpression` |
| TREE-3 | `WHEN_FILTER`, `PER_CLASS`, `PER_KEY_`, `SIGNATURE_` | the clause's scope |
| TREE-3 | `SOURCE_CLAUSE_`, `REMOVAL_GROUP_`, `BETWEEN_LOWER`, `BETWEEN_UPPER`, `STRUCTURAL_CONSTRAINT`, `DEFINES_`, `OBJECTIVE_ROW` | the clause's origin |
| TREE-3 | `MINMAX_EASY_REWRITE`, `SINGLETON_ROW_BOUND`, `SINGLETON_ROW_CLAUSE_ROWS` | clause fields |
| TREE-4 | `VALUE_REWRITE`, `GROUP_KEY_` | on a comparison: clause fields (TREE-3). On a reducer: reducer fields |
| TREE-4 | `QUALIFIED_REDUCER_SCOPE_`, `NORM_MARKER_`, `WRITTEN_NORM_`, `AVG_REWRITE`, `SKIP_NULL` | reducer fields: the operand's `per`, `norm` (a marker's order and bound, a definition's written order), the count scale. A NULL test is its own node in the reducer's filter, `BoundDecideNullTestExpression` |
| TREE-4 | `TREE_RETAINED_EXPRESSION_`, `TREE_REDUCER_SCOPE_`, `TREE_REDUCER_KEY_` | none: the nest stays as written and the linear form reads it in place (R35); `FrameAggregate::per` on the record; none (it had no writer) |
| TREE-4 | `QUERY_WIDE_VALUE`, `ROW_VARYING_SUBQUERY`, `BOUND_REDUCER` | an entry of `BoundDecide::computed_values`; the guard around a recorded reducer's value holds a `BoundDecideNullTestExpression` |
| TREE-4 | `QUERY_WIDE_BOUND` | a clause field |
| TREE-4 | `WRITTEN_NAME_` | on a reducer: a field. On a scalar call (`@`, `**`): a display field `written_name` on `BoundFunctionExpression`, beside `is_operator`. The field stays (R36): the binder spells each function one way, and a node canonicalization builds has no text to cut a name from |
| TREE-5 | `FRAME_`, `FRAME_TIGHTEST_BOUND` | the frame node's `frame` field; the clause field `tightest_frame_bound` |
| TREE-5 | `FRAME_AGGREGATE_` | the data reducer's `frame_aggregate` field, naming its `frame_aggregates` record; the lowered column holds it as `DecideFrameColumn::aggregate` |
| TREE-6 | `ABS_FUNCTION_`, `ABS_FUNCTION_NEG_`, `BILINEAR_FUNCTION_GROUP_`, `L0_INDICATOR_FUNCTION_`, `DEFINITION_FUNCTION_`, `BODY_DEFINITION`, `L0_AUTO_BOUND`, `L0_WRITTEN_BOUND`, `L0_TOLERANCE` | on a kept call: `BoundFunctionExpression::definition`, a typed reference to its entry of `helper_definitions`. On a row: read from the row's owner (`DecideHelperDefinition::Rows`); an L0 bound is automatic or written by its definition's `auto_m` |
| TREE-7 | `FRAME_COLUMN` | nothing: the render of a physical plan reads the source's column count (`DecideSourceNames::source_columns`), and a reference past it, a column the tree computes, prints its name as written |
| TREE-8 | `NULL_GUARD` | the node `BoundDecideNullGuardExpression` (R39) |
| TREE-9 | the seven helper name prefixes (`ABS_AUX_NAME_PREFIX`, `VALUE_LINK_NAME_PREFIX`, …) | none: no code reads a helper's origin from its name. They are display names, and a user's decision or column may carry one |

### 3.6 What Phase 6 needs from this phase

- A helper's definition holds its reducer node, its kind and its `push` as one value, not two booleans.
- A reducer several readers share is one definition; the lowering walk lists its readers.
- The executor's tree is visited children first (`eval_order`), and a G finds its members per group in its container.
- Model rows are not written here. The emitter and the linear-form records keep their shape until Phase 6.

### 3.7 Choices in this design

Decided by the evidence, stated so they can be reversed:

- A parsed reducer node only where DECIDE syntax is written (§ 3.2.1).
- Existing types are reused for a clause's scope and origin (§ 3.2.2).
- Helpers stay allocated at planning in this phase (§ 3.2.3).
- The steps run one at a time, with no parallel lanes (§ 3.4).
- Serialization field ids of `LogicalDecide` are reassigned; a serialized DECIDE plan does not outlive its process.

Ruled by the owner (`decisions.md` R21 to R26):

1. **The executor's input.** `DecideTree` stays the executor's structure, derived by one lowering walk (§ 3.2.4).
   It keeps execution as it is, and it is DuckDB's own split between an expression and its executor state. The
   executor reading the bound nodes directly would rewrite the three execution files in the same phase.
2. **A scope is a struct, not a node** (§ 3.2.1, § 3.2.2). [read] Every place a scope is written fits: a declarator
   takes only `per`, a frame's payload takes none, the objective takes the same pair, and the rows a rewrite
   generates need only `when` and `per`. A clause has exactly one scope by construction. The executor's tree has
   an E node either way.
3. **Shared things are tables named by typed index** (§ 3.1, § 3.2.3): keys, frames, and one table of helper
   definitions that owns each helper's body. A reading node owning its body would make a helper per copy of a
   clause.
4. **A clause's shape has one writer, the canonicalizing boundary, and one debug check guards bound reduction**
   (contract 4, R29). No reader computes the shape.

### 3.8 Risks

| Risk | Where it is met | Guard |
|---|---|---|
| A DuckDB pass does not reach an expression inside a new class, or folds a reducer as a constant | TREE-4 | the four obligations of contract 2; the debug binding verifier after every pass |
| A wide step breaks many files at once | TREE-3, TREE-4 | move (a) is a refactor with no change in behaviour and lands first |
| EXPLAIN or DIAGNOSE text moves | TREE-3 | both print from the clause records in the same step; their tests are in the step's gate |
| The direct rules' move is larger than one task | TREE-8 | its own design first |

### 3.9 Accepted when

- scopes, reducers and frames originate in parsing, and binding and every rewrite use them;
- evaluation and model generation do not reconstruct them from helper lists or marker strings;
- no DECIDE tag constant remains in `enums/decide.hpp` (R20), and the refusal of user names that contain a tag
  goes with the last one;
- behaviour tests exist at each boundary (§ 3.4);
- execution is vectorized as before: `tools/ab_bench.py` against the build before the phase, no workload beyond
  about 10%.

## 4. Phase 6: the model is written at the nodes

The design, its measurements and the tasks: [review/ARCH-2-design.md](review/ARCH-2-design.md). Rulings R42 to R50.

- **Three owners (R42).** The optimizer chooses the form of every lowering and stamps it on the plan: a helper's
  form, an AVG under `<>` kept as sums. A node repeats that form once per group and decides nothing. Stage 06
  supplies what needs the data's numbers, through two entry points: `PrepareModel` settles the column box once for
  a query, and `FormulateModel` supplies what needs a box (Big-M constants, a `<>` disjunction, a hard MIN or MAX)
  and stays a pure function of the records and a box, re-runnable for `DIAGNOSE` without reading data. Pattern:
  instruction selection, emission, legalization.
- **The node rule (R43, R44).** A SUM or AVG passes its members up when every reader takes a linear expression and
  it is in a nest of sums or each instance has one reading row by construction; otherwise it is a variable with a
  definition row per group. A MIN or MAX is a variable with a link per group, and a group of one member is that
  member. The optimizer sets the form on the helper's definition (`decide_helper_form.cpp`): the rule lists the
  readers that take a linear expression, and any other place the plan names the helper keeps its column. How a MIN
  or MAX is pinned depends on which way its readers push it (`ExtremumPush`).
- **Helpers stay declared at planning (R45).** Their names in `EXPLAIN` and `DIAGNOSE` do not change.
- **A record per row a clause emits, not per input row.** The emitter writes each clause's rows as records
  (`ClauseRecords`) straight into the solver input. A term is a decision, an instance and a coefficient, and stage
  06 finds its column from the instance alone; a helper holds its class for the rows that read it. A decision read
  through a frame is a term on that decision at the instance of the row the frame selects.
  [measured] `repro/clauses_times_rows.sql`, 2 million rows: 1 clause spends 29 ms writing the model
  (`execution.tree_model`) at a peak of 0.99 GB, 40 clauses 32 ms at 1.03 GB; 40 clauses that each read a helper,
  38 ms at 1.27 GB, the difference in memory being the solver's own.
- **The planner reads a clause twice**: the tree walk builds nodes and the term half builds terms, one extraction
  for a clause and the objective (`DecideTermLists`). The two stay apart (R50): the splitter has ten users and nine
  never see a node. What they shared twice, the frame columns, the tree owns (`DecideFrameReads::Slot`), and a frame
  over data is read from its O when a term's coefficient is compiled.
- **Shared with Phase 5:** routing, containers, the key map, data programs and folds, frames, the no-value rule
  (`DecideAbsence`), plan-time helper allocation, the linearizers of stage 06.

**Departures from the deck (R-D).** Formulation has three owners where the deck's node does all of it; a lone SUM
is written into its readers by structure, not by a count; AVG, COUNT, ABS, products, L0, norms, `<>` and quadratic
terms each have a node rule the deck does not give.

## 5. Direct solve and the tree

A direct rule is an optimizer rule over the bound operator (R17): `CheckCondition` receives `LogicalDecide`, as a
DuckDB rule receives the `LogicalOperator` it matches, and never the executor's tree. It reads the operator's own
fields and calls the shared readers when it needs their reading: `DirectReadClauses`, `DirectReadObjective`,
`DirectReadKey`, and `DirectSourceFacts` for the source plan. The analysis is conservative: a reader that cannot read
a clause returns false and the rule declines (R41), so a whole-query fact is refused by the reader that meets it and
the driver holds no gate. A rule that fires lowers to ordinary relational operators and reads its bound values from
SQL side tables; a miss leaves the solver path's meaning unchanged.

## 6. Tests and docs

- **Keep** every test judged by the independent oracle, and the regression tests of fixed defects. They survive an
  internal rewrite.
- **Cut** a test when the code it alone reaches is deleted, in the same change. A test of dump format or of exact
  model text does not survive Phase 6; move a query worth keeping into the golden corpus.
- **Add** an oracle test for each defect task; for Phase 5, behaviour tests at each
  boundary the tree crosses; for Phase 6, the model comparison between old and new model building.
- **The rule** (`.claude/CLAUDE.md`): each decision point gets a test that reaches it and fails if the branch is
  removed.
- **Docs.** Each phase rewrites `context/docs` for the stages it touches, in the present tense. When Phase 6 is
  done, this file's content lives in the stage docs and this folder goes.

## 7. What Phases 5 and 6 replace

Cleaning this code before the phase that replaces it is work done twice.

| What the review found | Replaced by |
|---|---|
| Each clause was lowered twice at planning, the two joined by expression address; 82 internal cross-checks | Phase 5: the rewrites produce the tree |
| One AND expression for all clauses, with `when` and `per` as tagged conjunctions | Phase 5: the clause record (ARCH-1) |
| `DecideNode` is one struct of 67 fields; `DecideTree::Validate` has complexity 142 | Phase 5: nodes split by kind |
| The emitter scatters node values into arrays as long as the input; evaluation scales with clauses times rows (`repro/clauses_times_rows.sql`) | Phase 6 |
| Helpers the plan writes in are created at planning and removed by `WriteHelpersIn`, with a second column layout | Phase 6 |
