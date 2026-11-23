# TREE-7 — sizing of a true single walk

Read at `d462b4ad31` (`OPA-rewrites`). Read-only: nothing was built, run or probed. "grep" marks a claim that rests on
a reader count only; "not read" marks code I did not open. Paths are under `src/` unless they start with `context/`
or `test/`. Short names: `tree.cpp` = `planner/decide/decide_tree.cpp`, `terms.cpp` =
`optimizer/decide/decide_linear_form.cpp`, `compile.cpp` = `planner/decide/decide_tree_compile.cpp`, `tree.hpp` =
`include/duckdb/planner/decide/decide_tree.hpp`.

Standard used to choose (owner's): cleaner code, no bloat, simpler, fewer states that must agree.

## 0. Verdict

- The two readings are two different analyses and cannot be one recursion without making things worse. `Lower`
  collects what an expression **reads** (child nodes, columns, sigma fields, decisions). The term code computes its
  **algebra** (signs, scales, products, like terms). The algebra reader is shared with code that has no tree (§ 2).
- What can and should go is the **join**. The G of a reducer is already the return value of `Lower` on that reducer
  (`tree.cpp:795`, `:740`). The map exists only because `Root` lowers the whole left side in one call
  (`tree.cpp:651`) and throws that association away.
- Recommended form (§ 3, form A): the clause's additive spine is read once (`ReadCanonicalAtoms`), and each atom is
  lowered and split together, the atom carrying its G. No map, no clears, no address identity. Pattern: a compiler's
  syntax-directed translation over one decomposition of the input, with two semantic actions per atom.
- It is a small change (about 16 functions, about 90 lines touched, net size about zero) and removes one assert. The
  gain is in states that must agree, not in line count.
- `design.md` § 3.2.4 should then say "one walk over each clause's atoms; an atom's nodes and terms are made together"
  and stop implying that a reducer body is read once: it is read by `Lower` (reads) and by the splitter (algebra).

## 1. What each walker produces, and who reads it

### `Lower` (`tree.cpp:757-862`) and its callers

| Product | Made at | Read by |
|---|---|---|
| Nodes and edges (G + operand, O, F reads, records, links) | `tree.cpp:865`, `:920`, `:967`, `:1006`, `:722` | everything downstream |
| `signature` | `:478`, `:488`, `:722`, replaced by the binder's at `:543` | descent (`decide_descent.cpp:119,365,421,460,776,1056`), emitter (`decide_model_emitter.cpp:180,245,268,270,798`), `compile.cpp:225` |
| `columns` | `:488` | descent (`:376,455,1143,1285`), `decide_tree_columns.cpp:158,177,231,273`, `compile.cpp:299,464,553` |
| `when` | `Gates` `:513` (by `Resolve`, not `Lower`) | descent `:356,455,1349` |
| `member_tests` | `:531` | `compile.cpp:251-255` only |
| `decision_value` | `:726`, `:822`, `:870` | `compile.cpp:269,389,455`, `decide_tree_columns.cpp:667,743`, `physical_decide_frames.cpp:706` |
| `decisions` | not by `Lower`: a later scan of `body`/`rhs`/`fallback`/`arguments` (`Definitions`, `tree.cpp:1126-1136`) | `decide_descent.cpp:1267`; `Sources` (`tree.cpp:1147`) for `dependencies` and `eval_order` |
| the lowered **expression** | return value | see next table |

Who reads a lowered expression after the builder (grep over `execution/operator/decide/*.cpp`, `compile.cpp`,
`decide_solver_gate.cpp`, `decide_source_provenance.cpp`, `plan_decide.cpp`):

| Field | Readers outside the builder |
|---|---|
| a clause root's `body` | none. Only `Definitions` scans it for decisions |
| a clause root's `rhs` | none. Same scan only (`DecideBody::rhs` at `decide_model_emitter.cpp:517,685,1223` is a program index, a different field) |
| a value check's `body` | `compile.cpp:267` |
| an operand's `body` | `compile.cpp:57` (`DecideTreeMembersOf`: is it a bare read of another G) |
| an operand's `arguments` | `compile.cpp:274` (a data reducer's arguments) |
| a G's `body` (a norm's, `tree.cpp:904,1001`) | none. It is lowered for its side effects on the operand |
| an F's `body` | `Validate` (`tree.cpp:1237`), `ToString` (`:1380`) |
| an O's `body`, `arguments`, `function` | none found (grep). `function` is read on G only (`decide_tree_columns.cpp:440,485,488,743`) |
| an O's `fallback` | `decide_descent.cpp:1429`, as a boolean |
| an O's `over` | descent, 11 sites |

So for a clause root, `Lower`'s expression result is redundant once terms exist. What is not redundant is its side
effects: nodes, edges, sigma, columns, `decision_value`, and the decisions list. The reverse does not hold: terms
cannot supply sigma (they hold coefficients, not reads, and cover the left side only).

### The term code (`terms.cpp`)

| Product | Read by |
|---|---|
| `DecideConstraint` (`lhs_terms`, `product_groups`, bound, admitted ends) on `compiled.clause` | `compile.cpp:96-102,476-505`; emitter (13 sites, grep); `decide_solver_gate.cpp:76`; `decide_domain_check.cpp`; `decide_source_provenance.cpp:827` |
| `DecideObjective` on `compiled.objective` | `compile.cpp:104,508`; emitter `:1358` |
| `DecideBody::members`, `fallback` | `compile.cpp:113-117,519-526`; emitter `:1547` |
| `DecideTerm::child`, `qualifier_reducer` (`Stamp`) | `compile.cpp:73-81` (`Site`); emitter `:1443,1501` |
| `DecideFrameReads` | `compile.cpp:330,366-433`; `decide_tree_columns.cpp:639-648`; emitter `:203,206`; `physical_decide_frames.cpp:720,729` |

One redundancy inside the term records: `DecideTerm::filter` is the reducer's `when`, which the operand already
holds as gates (`tree.cpp:885`). No execution file evaluates it (grep: no `term.filter` or `group.filter` read under
`execution/` or `decidb/`); its readers are `TermsAreLike` (`terms.cpp:510-513`) and `LowerFrames` (`:395`). With
`avg_scale`, `qualifier_scope_idx` and `avg_group_key` it restates "which G". Not part of this task; noted for § 3 (c).

## 2. The shared splitter and the tree-specific part

Shared, no tree: `DecideTermSplitter` and `DecideSplitTerm` (`planner/decide/decide_term_split.cpp`, 349 lines).
Users (grep): `decide_optimizer.cpp`, `decide_rewrite_abs.cpp`, `decide_rewrite_bilinear.cpp`,
`decide_rewrite_notequal_avg.cpp`, `decide_rewrite_scoped.cpp`, `decide_singleton_row_folding.cpp`,
`decide_tree_definition.cpp`, `direct/direct_ir.cpp`, `direct/direct_bound_reader.cpp`,
`direct/s3_max_cardinality_knapsack_rule.cpp`. Also shared: `ReadCanonicalAtoms`
(`decide_canonicalizer.cpp:146`; users `decide_rewrite_minmax.cpp:59`, `direct_ir.cpp:380`,
`decide_shape_check.cpp:149`, `decide_source_provenance.cpp:737`, `decide_product_form.cpp:134`) and
`DecideMergeLikeCoefficients` (`decide_term_split.cpp:47`).

`DecideLinearFormBuilder` calls the splitter at five places (`terms.cpp:157,308,834,860`, and `:152` for
`FindVariable`). Everything else in the file is its own:

| Part | Lines | Needs the tree? |
|---|---|---|
| Split terms into records (`AppendLinearTerms`, `ReadProductGroup`, `ExtractConstraintTerms`, `ExtractObjectiveBodyTerms`) | `:155-191`, `:832-886` | no |
| Atom dispatch (aggregate, easy extremum, per-row, objective) | `:82-110`, `:754-828`, `:889-975` | no |
| Reducer metadata, scales, filters, AVG (`ApplyAggregateMetadata`, `ApplyAvgGroupKey`, `AttachReducerFilter`, `ApplyScaleToExtracted`) | `:598-685` | no |
| Like-term collection, NULL objective terms, weight signs | `:498-590`, `:524-542`, `:639` | no |
| Frame terms and the frame-column registry | `:200-335` | no |
| Frames left in evaluated expressions, two passes | `:341-474` | no (reads the records it filled) |
| **`Stamp`** | `:717-746` | **yes: the only tree-specific code** (map lookup, `DecideTreeMembersOf`) |

So the tree-specific part of the term code is 30 lines.

## 3. Candidate forms

### What a single recursion would need, and why it is worse

For `Lower` to make terms as it goes, it would have to carry the sign and the pending scale down through `+`, `-`,
`*` and `/`, know which list a term goes to, and do product and frame algebra. That is `WalkAdditiveSpine`, the
splitter and the atom dispatch folded into a function that is already 98 NLOC, CCN 31 (lizard). For the splitter to
make nodes as it goes, the shared splitter would have to raise read events for sigma, the `carried` rule
(`tree.cpp:782`), records, frames and helper reads, and its ten tree-less users would pass a null sink. Both merge
two concerns that change for different reasons. Evidence that they are different analyses: `Lower` covers the right
side, gates' member tests, data reducers, F bodies and frame payloads, none of which have terms; the split covers
only the left side and the objective, and needs no node except in `Stamp`.

### Form A (recommended): one spine, each atom lowered and split with its G in hand

- `Root` reads the left side's atoms once (`ReadCanonicalAtoms`). For each atom it calls `Lower` on the atom's term
  and takes the G from the result: a reducer lowers to a child read, index `input_column_count + G`
  (`tree.cpp:740,795`). The atom and its G go to the term half in one struct. `Stamp` receives the G as a parameter.
- This is your form (a) at the grain where it works: the builder is the walk, and the term half is called with the
  node in hand. It is not "split at the moment `Aggregate` returns", because a reducer's terms need the atom's sign
  and scale, which only the spine knows.
- Lowering atom by atom is the same as lowering the side whole: the spine's own nodes (`+`, `-`, decision casts,
  `decide_canonicalizer.cpp:105-132`) reach `Lower`'s generic branch (`tree.cpp:854-861`), which has no side effect
  of its own, and every other node is an atom. So the atoms cover every leaf, in the same order.
- A scaled atom (`k * SUM`, `SUM / k`) is lowered whole; its G is the lowered child at the reducer's position. Add
  that position to `ScaledAggregateMatch` (it is a local at `decide_canonicalizer.cpp:120`), so no pointer is compared.
- The retained case becomes ordinary. `Lower` on the inlined reducer returns the lowered nest
  (`tree.cpp:774`), which is a child read, and the same "take the G from the result" rule applies. Lines `:775-779`
  go.

Disappears:
- `DecideReducerNodes` (`decide_linear_form.hpp:26`), the member `reducer_nodes` (`tree.cpp:426`), both clears
  (`:221`, `:607`), both writes (`:778`, `:794`), the lookup and its `D_ASSERT` (`terms.cpp:720-721`), the `reducers`
  constructor parameter on both classes (`terms.cpp:73,1002`).
- Three states that must agree today: the term half must be handed the same expression object the builder lowered
  (`tree.cpp:688` passes `*canonical`; passing `clause` would miss the map); the map must be cleared per root; and in
  a release build a miss dereferences `end()` (`terms.cpp:722`), since the guard is a `D_ASSERT`.
- A clause root's `body` as a stored expression (unread, § 1). Its decisions are collected from each lowered piece
  into `node.decisions` at once, in the same order `Definitions` reads them today (body, then `rhs`).

Stays, and why:
- `DecideTermLowering` as a class, in its file. It owns the frame-column registry and the splitter across clauses.
  Folding it into the builder makes one class of about 2,000 lines.
- The two traversals of a reducer body (reads, algebra). Inherent, as above.
- The `Members` loop (`tree.cpp:245-249`). It holds no agreement state (it indexes `definition_nodes`). Moving it to
  `Link` time would change frame-column slot order (§ 4).
- `Frames` and the record pointers (`terms.cpp:985-987`, `:459-474`). The frame columns have a stated layout: the
  term-registered `READ`/`FALLBACK` columns, then `VALUE`/`REDUCED`/`DEFINED` over every record, then each
  `AGGREGATE` "after every frame column" (`terms.cpp:449-455`), then `RecordFallbackColumns`. Doing it per record at
  split time interleaves the slots.
- `DecideTreeMembersOf` (`compile.cpp:55`): the term half still asks which G the terms range over.

Moves between files: nothing moves. `tree.cpp` gains `LowerAtoms` (about 18 lines) and a "node of a lowered read"
helper (about 8). `decide_linear_form.hpp` loses the typedef and gains the atom-with-node struct (about 8).

Size: `tree.cpp` — `Root`, the objective block of `Structure`, `Lower` (two branches shrink), the constructor, two new
helpers. `terms.cpp` — constructor, `Clause`, `Objective`, `AnalyzeObjective`, `ExtractAggregateConstraintTerms`,
`ExtractReducerConstraintTerms`, `ExtractEasyExtremumTerms`, `ExtractAggregateObjectiveTerms`,
`ExtractReducerObjectiveTerms`, `Stamp`, three façade methods. Canonicalizer: one field. About 16 functions, about 90
lines touched, net about zero. One internal check goes (`terms.cpp:721`); none is added (a tuple atom has no G by
design).

Not changed on purpose: the per-row arm keeps splitting the left side whole (`terms.cpp:105`). Splitting it atom by
atom would change its terms: the splitter keeps a decision-free sum as one constant term (`decide_term_split.cpp:269`)
where the spine yields one atom per addend.

### Form B: the term code is the walk and asks the builder for nodes

The term half would call back for each reducer atom. But the scale, the tuple terms and the whole right side also
have reads, so the callback must be "lower this piece" for every piece, in `Lower`'s order. A piece the term code
forgets to hand over loses a sigma field or a column with no error, and merges instances that differ. Today that
cannot happen, because `Lower` takes the side whole. Node numbering would follow the term code's order. Rejected: it
replaces a checked join with an unchecked completeness duty. `DecideTermLowering` would survive with a sink
interface; size about 150 lines.

### Form C: split the terms from the tree, not from the clause (not recommended now)

The bound clause is read once, by `Lower`; a later pass over the nodes splits each root's and each operand's lowered
body. Every term's G is the node the pass stands on. This is the only form in which the bound clause is truly read
once, and it would also remove what the two readings cost downstream: two translators (`Translate`, `compile.cpp:316`
and `TranslateTree`, `:440`), two ways to read a frame (`FrameColumn` `:366`, `FrameRead` `:385`), the `LowerFrames`
passes with their record pointers, and four agreement errors (§ 5: `terms.cpp:370`, `compile.cpp:372,377,398`).

Why not now:
- The shared splitter would need a tree-aware leaf rule (a child read is a helper decision, a product's expansion
  or an opaque value), so its ten other users get a hook they do not use.
- A lowered body no longer holds what a message quotes (`DecideProgram::written`, `compile.cpp:179`) or the kept
  calls and markers the dispatch matches (`TryMatchScaledAggregate`, frame markers).
- Frame-column slots and program order on an O would follow node order. Like terms merge today across two reducers
  with equal filters (`TermsAreLike` does not compare `child`); per-node splitting would stop that.
- Size not established: several hundred lines across `decide_term_split.cpp`, `terms.cpp`, `compile.cpp`, with
  golden and message risk. The emitter's and the formulation's use of the written expressions: not read.

### Keeping today's form

It is honest if the design text is changed to say so, and `terms.cpp:721` should then be an `InternalException`, not
a `D_ASSERT` before a dereference. By the owner's standard it loses to form A: it has three more states that must
agree and one more special case (the retained write), for no less code.

## 4. Order sensitivity (form A)

| Order | Why it is preserved |
|---|---|
| Node numbering | `Lower` is still the only node maker and sees the same leaves in the same order (spine nodes have no side effect; atoms are every other node, left to right, as `EnumerateChildren` visits them). The right side, gates and helpers are untouched. `test/decide/tests/test_one_pass_tree.py:76` compares a tree dump byte for byte, so a slip shows |
| Sigma and `columns` order | Same calls to `Signature` and `Column` in the same order, for the same reason |
| `decisions`, `dependencies`, `eval_order` | Decisions of the lowered pieces are appended in spine order, which is the order the scan of the whole `body` yields; `rhs` follows as today. `Definitions` then runs unchanged |
| Row order in the model | Roots are made and numbered as today (`compile.cpp:122-137`); terms are produced by the same functions in the same atom order |
| Like-term collection | Same lists, same order, same `CollectLikeTerms` calls (`terms.cpp:107,116`) |
| Frame-column slots | Registered by the same calls in the same order: clauses at `Root` time, the objective, then `Members`, then `Frames`. None of these loops moves |

Risks to check with the tree-dump probes during the cut:
- A spine node that is itself a kept call. `Lower` tests `DecideDefinitionOf` before anything else
  (`tree.cpp:760`); `WalkAdditiveSpine` does not. If a root-level `+` or `-` could be named a definition, the two
  would disagree. `NameDefinition` is called on a reducer's body (`decide_tree_definition.cpp:333`), not on a root's
  spine; the bilinear pass's naming sites: not read. Safe rule: the builder treats a named call as an atom.
- `AnalyzeObjective` has a bare-reducer arm (`terms.cpp:951-965`) beside the atom arm. Keep both; merging them is a
  separate question (whether a bare objective reducer is always named `sum`: not read).

Check of the earlier note (`TREE-7-design.md`):
- "Placeholders are appended last, so clauses-then-definition-rows is the clause list's own order": confirmed.
  `AddClause` has three callers: `decide_rewrite_minmax.cpp:83` and `decide_rewrite_norm_in.cpp:254`, both run before
  `PrepareDefinitionFunctions` (`decide_optimizer.cpp:128,134,136`), and `decide_tree_definition.cpp:340`, which is
  it. Nothing else appends to `bound.clauses` (`logical_decide.cpp:262` is the only `push_back`).
- "Split at `Root` time; no second loop": true for clauses (`tree.cpp:688`). The helpers' members and the frame
  passes are still later loops (`tree.cpp:245-250`); the note's "Stays" list says so.
- "No copy is retained": confirmed (`canonical_envelope` is a local, `tree.cpp:620`).
- The retained branch: the note does not describe it. It is a second writer of the map with a silent path
  (`tree.cpp:776-779`: no entry when the lowered nest is not a child read). By `Lower`'s branches a non-frame reducer
  always lowers to a child read, so the path is unreachable; not probed.
- The note's probe results (`probe7.sql`, `probe8.sql`): not re-run.

## 5. Internal errors and asserts

Classes: (i) exists because two readings must agree; (ii) guards a plan index or another stage's contract; (iii) a
structural invariant of the tree.

`tree.cpp` (30 lines match: 29 `InternalException`, 1 `D_ASSERT`):

| Line | What | Class |
|---|---|---|
| 61, 64 | clause has no shape / VALUE shape | ii |
| 198 | definition placeholder finds no definition | ii |
| 277 | L0 indicator has not 3 links | ii |
| 372, 386 | helper has no scope, label or valid keyed scope | ii |
| 457 | source column not in the child's bindings | ii |
| 556 | signature index out of range | ii |
| 618 | owned row given no owner | iii |
| 772 | retained expression index out of range | ii |
| 1008 | frame index out of range | ii |
| 1167 | dependency cycle | iii |
| 1213, 1218 | post-order covers each node once | iii |
| 1225 | objective node with no direction | ii |
| 1247 | F node: helper, body, row count, row ownership | iii (re-checks what the builder just wrote) |
| 1262 | F node's `per`/`key` match its helper's scope | iii (recomputes `FunctionNode`, `tree.cpp:381-394`) |
| 1266 | key index out of range | ii |
| 1273 | value-link ownership, with two cross-kind tests | iii |
| 1277 | qualifier only on a G, and in range | iii + ii |
| 1282, 1287 | child is a dependency; dependency runs first | iii |
| 1299, 1304, 1309 | carried, program and signature columns are source columns | iii |
| 1319, 1329 | hole target; program within layout | iii |
| 1340, 1346 | `function_nodes` matches the F kinds | iii |
| 1352 | root is an E or F marked root | iii |

`terms.cpp` (15: 12 + 3 asserts):

| Line | What | Class |
|---|---|---|
| 48, 172, 181 | opaque, non-linear or non-product term from the splitter | ii |
| 223 | payload and `else` types differ | ii |
| 261, 265, 285 | frame marker, range over decisions, non-linear frame term | ii |
| 370 | a frame over decisions left for the frame pass | i (the split and the frame pass) |
| 704 | data column beside a product in the objective | ii |
| **721** | **reducer has no entry in the map** | **i** |
| 764 | scale is a decision | ii |
| 790, 933 | reducer not rewritten to SUM | ii |
| 816 | easy extremum without its reducer | ii |
| 820 | easy extremum starts with empty lists | iii |

`compile.cpp` (12):

| Line | What | Class |
|---|---|---|
| 45, 50 | a G has two or no operands | iii |
| 141 | folded-bound count against FoldSingletonRows | ii |
| 341, 343 | coefficient reads a decision / unresolved column | ii |
| 372, 377 | a frame column's G or O was not made by `Lower` | i |
| 398 | an O read in tree terms has no registered column | i |
| 447 | a tree-terms expression reads a decision | ii |
| 461 | a child with no data value read as data | iii |
| 469 | a column the node does not carry | iii |
| 621 | unknown program role | iii |

What goes:
- Form A: `terms.cpp:721`. One. The others of class (i) belong to the frame registry's two dialects and go only
  with form C.
- The node split (§ 6): `compile.cpp:45,50` (a G names its operand), the cross-kind halves of `tree.cpp:1273,1277`,
  and the "only an F has rows" part of `:1247`.
- Nothing else. 57 lines, 5 of class (i): the construction is already mostly guards of other stages.

## 6. `DecideNode` by kind

Fields (`tree.hpp:126-203`), with the kinds that use them and readers outside the builder, `Validate` and `ToString`:

| Field | Kind(s) | Outside readers | Copy of a plan fact |
|---|---|---|---|
| `kind`, `parent`, `children`, `dependencies`, `text`, `when`, `per`, `key`, `signature`, `decisions`, `columns`, `decision_value`, `holds_rows_in_no_class`, `compiled`, `programs`, `tests`, `program_columns`, `holes` | all | many | `text` of an F is `aux_var_expressions`' label (`tree.cpp:380`) |
| `member_tests` | E, G | `compile.cpp:251` | |
| `body` | value check, operand, F, O, G (norm) | `compile.cpp:57,267` | |
| `rhs` | clause root | none | |
| `arguments` | operand; O | `compile.cpp:274` (operand) | |
| `function` | G; O | G: 6 sites; O: none (grep) | |
| `aggregate` | G; O | emitter `:1548,1566,1610`, all on a linked node | linked G: `definition.agg_name` (`tree.cpp:942`); O: `frames[frame].reducer` (`:1023`) |
| `qualifier_scope` | G (set on a linked O too, `tree.cpp:931`, which `Validate:1272` then forbids unless INVALID) | emitter `:322` | `reducer.per` |
| `over`, `fallback`, `frame` | O | descent 11 + 1; `frame` 6 | `frame` is the index, not a copy |
| `rows` | F | emitter `:405,550,610`; provenance `:826` | |
| `definition` | F, linked G, linked O | through `Linked`, `Definition`, `ValueVar` (about 10) | index, not a copy |
| `origin` | clause root; F | root: provenance `:828`; F: none | root: `compiled.clause->origin` (`tree.cpp:45,631`); F: `definition.origin` (`:266,272,281`) |
| `defined_var` | clause root; F | none | root: `compiled.clause->defined_var` (`:52,633`); F: `definitions[definition]->helper` (`:379`) |
| `root`, `hidden`, `objective` | E, F | none (`Validate`, `ToString`, builder) | `objective` restates `compiled.objective` |
| `folded`, `merges_classes`, `value_check` | clause root | 3, 3, 2 | `folded` is `clause.folded_bound` |

Step 1, delete (no hierarchy needed): a clause root's `body` (after form A) and `rhs`; an F's `origin`; `defined_var`
on both (the builder reads the clause's and the definition's); `origin` on a root (one read moves to the clause
record); an O's `function` and `arguments` if the grep holds; a linked node's `aggregate` (read the definition's or
the frame's). About 8 fields or uses, no cast anywhere. This is pure gain and I recommend it whatever is decided on
the hierarchy.

Step 2, the hierarchy (DuckDB's tagged base with `Cast<T>()`, as `Expression`): base `DecideNode` with the common
fields and `definition`; `DecideScopeNode` (E: `folded`, `merges_classes`, `value_check`, `arguments`, `body`),
`DecideReducerNode` (G: `function`, `aggregate`, `qualifier_scope`, and a new `operand` index), `DecideFrameNode`
(O: `frame`, `over`, `fallback`, `body`), `DecideFunctionNode` (F: `rows`, `body`).

Read sites that change after step 1 (grep, approximate): `decide_descent.cpp` 14 (`over` 11, `fallback` 1,
`merges_classes` 2); `decide_model_emitter.cpp` 8 (`rows` 3, `frame` 3, `qualifier_scope` 1, `merges_classes` 1);
`decide_tree_columns.cpp` 6 (`function` 4, `frame` 1, `value_check` 1); `physical_decide_frames.cpp` 1;
`compile.cpp` 9; `decide_solver_gate.cpp` 1; `decide_source_provenance.cpp` 1. About 40, plus the six node-making
functions of the builder. Seven `DecideTreeOperand` calls become a field read and its scan with two errors goes.

Judgement by the owner's standard: the hierarchy is the better structure, by a modest margin. It makes "a field set
on the wrong kind" unrepresentable (today `Validate` checks three such cases and cannot check the rest), and it
removes the operand scan. Its cost is about 40 mechanical casts and no new state. E stays a small union (root-only
flags beside operand-only fields); splitting E further is not worth a new kind.

Conflict to settle: `design.md` § 3.2.4 (`context/opa_rewrite/design.md:162-165`) says the node "stays one struct"
and gives a reason; the queue row (`:246`) and this task say "by kind". One of the two texts must change.

Order: after the walk change. Form A makes a root's `body` unread, and step 1 decides which fields survive;
splitting first would move fields the next cut deletes.

## 7. Recommendation and cuts

Each cut ends green on its own, with the tree-dump probes of `tree6-probes/` and `tree7-probes/` byte-identical.

1. **Atom carries its G (form A).** `ScaledAggregateMatch` gets the reducer's child position; `Root` and the
   objective lower by atoms and hand the atoms with their nodes to the term half; `Stamp` takes the G; the map, its
   clears and writes, and the retained special case go. Focused tests: the tree and one-pass tests, nested and
   scoped reducers, easy extremum, objective.
2. **Unread and copied node fields (§ 6 step 1).** One field at a time, grep first.
3. **A G names its operand.** One field, set in `Aggregate`, `Link`, `Record`; `DecideTreeOperand` becomes a read;
   `compile.cpp:45,50` go. Can be folded into cut 4 if the hierarchy is taken.
4. **`DecideNode` by kind (§ 6 step 2)**, if the owner settles the conflict that way.
5. **Docs.** `design.md` § 3.2.4 wording (one walk over each clause's atoms; the two readings of a reducer body named
   as reads and algebra; the frame passes named as kept), the "Gone" list, and the node sentence.

Not recommended in this task: form C; moving `Members` to `Link` time; per-record frame lowering.

## 8. Owner's decisions

- Form A, or today's form with the design text corrected and `terms.cpp:721` made a real error.
- Whether form C goes on the queue as its own item (it is the only form that reads the clause once and removes the
  two-dialect errors, but it is unsized and touches messages and frame slots).
- One struct or a hierarchy for `DecideNode` (the design text and the queue row disagree).
- Whether an O's `function`/`arguments` and an F's `origin` may be deleted on grep evidence, or need a probe first.
