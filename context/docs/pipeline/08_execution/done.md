# Stage 08 — Physical execution and readback

Runs the relational input, evaluates every prepared coefficient and grouping
against the materialized data, invokes the solver contract, and projects the
solution back onto rows. It implements no rewrites, no backend-specific
formulation, and no algebra: the constraints and objective arrive already
flattened into terms.

`PhysicalDecide` is a **blocking** operator: the optimal value of any one decision
depends on the whole dataset, so it must consume its entire input before it can
produce a row.

**Key source files**: `src/execution/operator/decide/physical_decide.cpp` (`Finalize`, the assembly, formulation,
solve and readback), `decide_descent.cpp` (top-down), `decide_tree_columns.cpp` (bottom-up), `decide_model_emitter.cpp`
(what stage 06 reads) and `decide_value_extraction.cpp` (the value checks and the blocks the emitter writes through);
at plan time `src/planner/decide/decide_tree.cpp` and `decide_tree_compile.cpp` (the tree and its programs).
**Header**: `src/include/duckdb/execution/operator/decide/physical_decide.hpp`

`Finalize` itself is a short orchestrator; the phases below are private methods
on `PhysicalDecide`, in call order:

| Method | Phase |
|---|---|
| `EvaluateTree` | 1–2 — the evaluation (§1a): the tree's descent, which read each chunk as `Sink` received it (`DecideDescent`), each node's programs over its entries (`DecideTreeColumns`), and the model emitter (`DecideModelEmitter`), which writes the key scopes' classes, the clauses, the objective, the composed clauses and the helper decisions' members as stage 06 reads them |
| `BuildSolverInput` | 3a — assembles `SolverInput`, settles the column box, builds the `VarIndexer` |
| `FormulateModel` | 3b — derives the constants and emits the rows, against a given box |
| `FormulateElasticModel` | 3b again, under `DIAGNOSE` only — the same pass against the widened box a repair searches |
| `FinalizeSolveResult` | 3 — calls `SolveModel`, routes the terminal (`SOLVED`/`UNBOUNDED`/`INFEASIBLE`/`TIME_LIMIT`), and finishes `gstate` |

`EvaluateTree` fills one `EvaluatedClauses`: everything read off the data that a later phase still needs. It is named
for the phase that fills it rather than for one consumer, because it carries objective terms and composed-MIN/MAX
*constraint* clauses alike.

The blocks the emitter writes through are shared functions (`decide_value_extraction.hpp`), one copy of each rule and
its message: grouping a clause's kept rows by its shape (`GroupRowsByShape`), skipping a group whose reducer reads no
row, counting a term outside the reducers once (`CountTupleTermsOnce`), the AVG-denominator division (`ScaleAvgRows`,
`ScaleAvgTerms`), a square's scale (`ScaleSquaredGroup`) and the tightest frame bound. `EvaluatedConstraint::
BilinearTerm` (`solver_input.hpp`) is the one bilinear-term struct; there is no separate objective-side copy.

---

## 1. Order of operations

| # | Where | What |
|---|---|---|
| 1 | `GetGlobalSinkState` → `DecideGlobalSinkState` ctor | Read the folded variable box and alias the prepared linear form |
| 2 | `Sink` → `DecideDescent` | Materialize every surviving tuple into the thread's `ColumnDataCollection`, and read it top-down as it arrives: each node's `when` parts, keys and σ fields on the rows that reach it, into the thread's containers (§1a) |
| 2a | `Combine` → `DecideDescent::Merge` | Place the thread's rows after those placed before, and merge its descent into the operator's at them |
| 3 | `Finalize` → `EvaluateTree` | Settle the containers (`DecideDescent::Finish`: ranks, timelines) |
| 4 | `EvaluateTree` → `DecideTreeColumns` | Bottom-up: the frames' timelines and columns (§4b), the data checks, each node's programs over its entries, the data reducers' folds |
| 5 | `EvaluateTree` → `DecideModelEmitter` | The key scopes' classes (§4), the objective produced once (§4a), then the clauses, the objective, the composed clauses and the helper decisions' members (§5) |
| 6 | `Finalize` → `BuildSolverInput` → `FormulateModel` | Assemble `SolverInput` and settle the box, then linearize against it (stage 06) |
| 7 | `Finalize` → `FinalizeSolveResult` | `SolverModel::Build` → `SolveModel` (stage 07) |
| 8 | `GetData` | Project solution values back onto rows |

The sink state is constructed before the first `Sink` call, so step 1 sees no
data — which is fine, because it derives nothing. Flattening already happened at
plan time (stage 05).

Read consistency follows from step 2: the optimization runs on the snapshot the
query saw. Concurrent modifications cannot affect a running solve.

---

## 1a. The two passes (deck §3, pp. 77-119)

The deck evaluates a query over its annotated tree in two passes: a top-down construction reads the joined rows once
and fills each node's container with what the node reads on a row, and a bottom-up evaluation computes each node from
its children's containers. Stage 08 does the same:

| Deck | Stage 08 |
|---|---|
| The annotated tree: each node with what it reads per row (pp. 81, 99) | `DecideTree`, built at plan time (below): the E, G and O nodes, each with its `when` parts, key, σ, carried columns and programs |
| Top-down construction: one pass over the rows (pp. 83-91, 100-105) | `DecideDescent`, in `Sink`, each chunk read as it arrives, a window at a time, by a descent per thread merged in `Combine` |
| A node's container (p. 110) | `DecideNodeContainer`: its entries as columns, its slots found in a `DecideKeyMap` |
| Bottom-up evaluation (pp. 93-97, 106-108) | `DecideTreeColumns`: each node's programs over its entries, children before parents; then `DecideModelEmitter` writes what stage 06 reads |

**A row's error is kept until a read reaches the row**, a departure SQL forces: a `when` decides which rows a node
reads, so a read of a row the `when` drops raises nothing (`when t.w > 0: x <= ln(t.w)` where `w` is 0,
`language/guide.md` §6.3), and a read that raises holds its error in the evaluation's ledger at the rank of the phase
that reads it. The least is raised once every phase has run, in the order `language/guide.md` §7.3 gives. An
interrupt, an out-of-memory error and a fatal error stop the evaluation where they arise.

Each row is read once, as it arrives, and the evaluation scans the buffered input no more (`execution.input_scans` 0,
`DECIDB_PROFILE`). Each thread reads its rows into a descent of its own, as a hash aggregate builds a table per
thread, and `Combine` merges it into the operator's (below). The merges run one after another under the sink's lock
and copy every entry (`execution.descent_merge`): over 4 million stored rows, the query reached `Finalize` after
0.23 s on 8 threads, 0.17 s of it merging, against 0.31 s on one, in a query whose model and solve took 3.7 s. A merge in parallel, as a hash aggregate finalizes its partitions, is not built.
The unbounded
diagnosis's column groupings (§7b) explain the solved model after the evaluation, and read the named input columns in
a scan of their own.

---

### How the tree replaced the flat evaluator

The literal one-pass plan (`.claude/local/literal_one_pass_plan.md`, §7) built the deck's evaluator beside the one
stage 08 had, which evaluated each clause over the rows of a one-pass store ("flat"): Branch by Abstraction with a whole
Parallel Run. A switch picked one, and under `verify` both ran on every query and what each would hand stage 06 was
compared, an INTERNAL error on any difference. The suite passed `verify` with no difference at descent chunks 1, 7 and
2,048; the whole suite's output with the tree alone differed from flat's only as recorded below (G5); the tree's
model goldens were byte-identical; and its cost came within the plan's bound (G6). Then the tree became the default,
and flat, the switch and `verify` were deleted. The tests that took flat's run as their oracle read its output from a
store recorded before the deletion (`tests/_expected.py`).

**The evaluator.** Every query is evaluated by the tree and formulated through stage 06. Where a rule below says what
flat did, it names the behaviour the tree keeps from the evaluator it replaced, which the suite pins (its stored
references, `tests/_expected.py`, were recorded from flat's runs):

- *The tree* (`decide_tree.hpp`, `DecideTreeConstruction`): built at plan time in two halves around stage 05. Its
  nodes come from the clause list, the link and record tables and the frames, with the deck's three node classes, E
  (`SCOPE`), G (`REDUCER`) and O (`FRAME`). An E's σ is the binder's signature of its clause; a generated NULL test is
  a member test that carries its columns and adds no σ field. A root in the grouped shape (the aggregate shape, an
  easy or a composed extremum) with no `per` is a class of its `by` key, as flat's grouped rows are
  (`language/guide.md` §6.4). A root's text is its clause as written. Stage 05 then lowers the clauses once, for
  flat, and names on each reducer's terms the G they range over (`DecideTerm::child`); the second half
  (`decide_tree_compile.cpp`) binds each clause root to stage 05's clause, copies stage 05's terms into the node's
  body, finds the node each term is computed on, and compiles each coefficient, bound, admitted end and member test
  into a program over that node's layout, by flat's own rules (`ToChunkReferences`): its carried columns, then the
  program columns its programs add (which the descent carries on each entry), then an admitted end's inputs a and r. A
  root takes the error rank of the stage-05 list its body comes from, in the order flat evaluates them (a folded
  bound's singleton row follows every constraint, and flat evaluates it under DIAGNOSE alone). A read of a data reducer
  is a hole of its reader's layout (`DecideHole`), and so is a read of a frame, its O's column at the reader's row
  (`DecideHole::column`): a point's value or a range's over data where Lower read the O itself, the column stage 05
  read otherwise, and a reducer's over frames its G's fold. What a frame's columns read, its payload, its `else` and a
  range's argument, are programs on its O (`DecideFrameColumnPrograms`), in stage 05's terms. A child a data
  expression reads that has no data value is an internal error.
- *Top-down* (`decide_descent.hpp`): each thread's descent reads each chunk as `Sink` receives it, and numbers its
  rows by their places in the thread's buffer; the buffer is never scanned to evaluate a row (`execution.input_scans`
  0, `execution.descent_rows` the rows read). `Combine` places the thread's buffer after the rows placed before
  (`ColumnDataCollection::Combine`) and merges its descent into the operator's at them (`DecideDescent::Merge`), the
  first thread's descent becoming the operator's: its rows are rebased; its keyed classes are found again by layout
  and sort key, its slots by key -- and enclosing slot, a G's or O's before an E's -- in the operator's dictionaries,
  a set E's entries by slot, signature, carried columns and reads (`DecideNodeContainer::reads`), with a decision's
  instance and a read rebased as the classes, slots and rows are; a collated key's spellings fold into the least; and
  its errors take their ranks at their rebased rows (`DecideErrorLedger::Rebase`). Each is found again in the order
  the thread made it, which is its rows', so the merged containers number as one descent over every row would. An O
  two reducers' operands read takes its rows in the slots of both, which no enclosing slot tells apart: its entries
  keep none once merged, which no reader reads. `DECIDB_DESCENT_SPLIT=n` merges every `n` rows as a thread of its own
  would (`test_descent_merge.py`; the whole suite passes under it, but for the inputs of tens of thousands of rows a
  descent per row cannot read within their time limits). It is a Composite traversal vectorized by selection, as
  DuckDB's filters hand a selection vector on: a window of the input at a time, a node's `when` parts, keys and σ fields
  execute only on the rows that reach it (`Reach`: the rows, and the enclosing slot each arrives in), and each key's
  rows find their slots in a `DecideKeyMap` (`decide_key_map.hpp`). That is DuckDB's GROUP BY hash table
  (`GroupedAggregateHashTable`) with no aggregate and one register per group, which holds the group's slot, probed a
  vector at a time: NULL is equal to NULL, and a collated column compares under its collation. The nodes of one key
  outside an enclosing slot share its dictionary (a Flyweight, `SharedDictionary`), each numbering its own slots in the
  order its rows reach them. A key is evaluated once on a row of a window, by the first node that reads it there, and
  every later read slices those values (`KeyReads`); a row a read raised on is not held, so each node reading it raises
  at its own rank. `DecideTree::Validate` checks at plan time that the descent reads of a row only its nodes' own `when`
  parts, keys and bare input columns: a carried column, a program column and a σ value are each an input column. A
  node's entries are columns (`DecideNodeContainer`): per entry its row, its enclosing slot, its slot and, for a set E
  alone, its instance, and its values in `DecideColumns`, chunks of 2,048 rows held in memory, so that a window of
  entries is one chunk an executor reads in place. An entry that stands for its row alone holds no decision's instance
  (`DecideEntryLayout::signature`), which its row gives; a node per row keeps no slot arrays, since its entries are its
  slots; a constant program column is held once. A G's or O's groups lie in no enclosing slot, because the rewrites
  already make a reducer's key its whole path key (`language/guide.md` §5.5): only a `by` opens a level, and an E's
  classes lie within the G or O they feed. A collated key folds each read's spelling into its group's least, one
  dictionary per key across every node, and a position takes its rows' least spelling when the descent finishes (D69).
  Each slot of a collated key also keeps its own least spelling over the rows its node holds in it
  (`DecideNodeGroups::spellings`), which flat names a variant's class by. A set E keeps one entry per distinct slot, σ,
  carried columns and reads, found by one probe of its entries' `DecideKeyMap`, at least one per variant; its instances
  are its distinct (slot, σ). It records each row it keeps with the entry standing for it
  (`DecideNodeContainer::routed`): a pair per kept row, which the objective's root, a grouped or class root and a
  relation-qualified reducer's operand read. A grouped root under `per K:` holds a kept row in no class of K too, in a
  slot of its own (`DecideNodeGroups::in_a_class`), since the row still lies in a group whose row reads it. So does an O
  an anchor whose `within` column is NULL: it lies in no timeline (D75), and its slot is no timeline's. So does a
  relation-qualified reducer's operand hold a row its `when` keeps in no class of its `per`: flat computes the reducer's
  terms there and its qualifier keeps no value of the row, which still lies in its group (`language/guide.md` §5.3). A G
  holds a row its key leaves in no group (a NULL in a written column, `language/guide.md` §5.2) in a slot of its own
  too, with no entry, so the row reads no value of the G; the row still reaches the G's operand, which flat computes on
  every row the G reads (a clause's terms on every kept row, a side plan a data reducer's arguments), so an error there
  is raised. The objective's G's hold no such slot: flat reads the objective's terms in place, on the rows of each group
  alone (D81). A group's rank is flat's group number: NULL sorts after every value, as `ORDER BY` places it.
- *Arms*: the model emitter has an arm for every shape stage 06 accepts, keyed on what it reads of the clauses and the
  query: a clause with a model row per kept row, or per variant of a class of its `per K:`, the folded bounds' rows a
  repair model emits, the declared decisions' classes and a relation-qualified reducer's, the objective's linear terms,
  a clause in the aggregate shape, a `<>` clause, a row defining an auxiliary, the classes a diagnosis reports a row
  stands for, an easy and a hard MIN or MAX, a MIN or MAX objective, a composed clause and the objective's composed
  terms, a clause's products and squares, the objective's squares and products, a product the optimizer makes an
  auxiliary of (a McCormick link, an AND of two BOOLEANs), a helper decision of a reducer the value rewrite lifted, a
  frame's reads and a range over decisions' helper, and a tightest frame bound. Every query runs `EvaluateTree` and then
  flat's tail (`BuildSolverInput`, `FormulateModel`, the solve).
- *Bottom-up* (`decide_tree_columns.hpp`): partial evaluation, as DuckDB's executor runs a projection, with no
  expression interpreted per entry. Each node's programs run over its entries, one executor per window of 2,048 entries,
  with the one pass's per-row error capture (`DecideEvaluateWindow`), and are read through flat's `ExtractDoubleColumn`,
  so a value and its message are flat's; a coefficient computed from inputs names the input that is not finite at the
  failing entry. A node's member tests decide which of its entries are read: an entry whose test is false is no member
  (a test is `X IS NOT NULL`, NULL only where it raised, and such an entry is no member either), its other programs are
  read on the members alone, so a value or an error elsewhere is never read (flat's read rows). An objective term is
  read only on the entries that read an instance of its decision as well (`ReadEntries`, D84). A G over data, a data
  reducer, folds its operand's members, each variant of a class once at its first row and nothing of a row in no class
  (`FirstOfEachInstance`), with DuckDB's aggregate functions over a dense array of states, one per slot, as
  `PerfectAggregateHashTable` lays out its groups (`DecideAggregateStates`); a norm folds its desugared reducer's (a sum
  of squares, of absolute values); a NaN among its values, or in a norm's body, is the data check's to refuse, as flat's
  is, since the check ranks first. A reducer over frames, which no check reads, refuses one itself, a norm's in its body
  (`DecideBody::norm_body`, since SIGN reads a NaN as 0), but a count, which counts it; it folds once every frame's
  columns are, before any keyed mapping, in its column's order (FRAMES). A node reads a data reducer's value as a hole
  of its layout, after its carried and program columns: the fold in the slot its row lies in, NULL where the row reaches
  none. A data check's message is a program on its hidden root, raised at its rank before any clause is read
  (`CheckData`); the check stands for a side plan's, so a window of it that raises raises the error its whole evaluation
  meets first, as DuckDB's vectorized evaluation of the side plan's chunk does. A frame is read once the descent is
  finished, before any program that reads it (`ReadFrames`): its map from its O's timelines and positions (`FrameMap`),
  each anchor's position or window selected by flat's own block (`SelectDecideFramePeers`), then what its windows skip
  as NULL and its columns, through flat's blocks too (`SkipDecideFrameNullReads`, `ComputeDecideFrameColumns`), each
  expression they read a program on the O over its anchors (`DecideFrameRead`). A column holds a value per row with the
  error its row carries, which a program reading it there raises at its own rank, as flat's read of the row raises it
  (`CarryFrameErrors`), so an error no reader reaches is never raised. An O's programs that read its own frame's
  columns, a range over decisions' `else` terms, are computed after them (`FramePrograms`). What a frame refuses ranks
  where flat refuses it: its range's limit after the reads of its rows, then its partition (D75), frame after frame, in
  FRAMES; a position of two payloads it reads (D73, the descent's `DecideDescentPosition::conflict`) in FRAME_PEERS,
  named by its timeline's and its order key's least spellings (D69). Flat names a D73 timeline, and a D75 class and its
  timelines, by the spellings at their rows instead, so under a collation whose rows spell a timeline differently the
  two raised different messages; which is right is open (D12), and
  `test_a_collated_timeline_is_named_by_each_evaluators_spelling` holds both. Its maps are stage 06's
  (`DecideGlobalSinkState::frame_maps`), which reads a frame read's rows from them. A node is computed when it is first
  read, a hole's G before the node that reads it, rather than in an `eval_order` walk. The descent's errors and the
  programs' go to one ledger, in flat's order inside a clause or the objective (`DecideBlock`): the descent's reads that
  route the rows (a `when`, a key that is no grouped clause's `by` key), in the order it reads them, then the blocks,
  the term filters, the terms, the grouping by the `by` key, the bound and admitted ends, the products, the squares,
  each square whole before the next, in a part of its own (`DecideErrorRank::part`), its factor first. A program's error
  ranks under its owner's rank by its block, then flat's window of the input's rows, which the failing entry's row
  places (not the window of entries, since a `when` keeps fewer entries than rows), execution before value, then the
  program's group and place in its block (`DecideErrorRank::group`, `position`), then the row. A definition's programs
  rank where flat read them, twice: where the auxiliary's absence is resolved (`DecideModelEmitter::ReadAbsence`),
  auxiliary by auxiliary in index order (`DecideErrorRank::step`) and each one's definitions in their order, the bound
  and then the coefficients together, a window at a time (`NullEntries`), which raises an error their evaluation meets
  (`DecideNode::value_rank`); and where its clause is evaluated among the clauses, which checks their numbers
  (`DecideNode::clause_rank`). A reducer's filter is read as flat reads it: after the clause's or the objective's
  `when`, before every term, window by window, by the first term its node holds (`DecideNode::filter_group`), the
  reducer's own `when`, which the descent reads, then its member tests; a clause reads its terms' filters, then its
  products', then its squares', and the objective its products' after its other terms' coefficients, in a part of their
  own with the products' coefficients (`DecideNode::filter_part`). A grouped clause's root and its G's of the `by` key
  read the key in the grouping block (`DecideNode::key_block`), element by element, after the terms: a row the key
  raises on, or leaves in no group, still reaches the terms, whose errors rank first. Flat reads a composed clause's
  terms one after another, each over every row before the next, after every term's filter: a composed term's programs
  rank in its part (`DecideErrorRank::part`, the term's place, which ranks before the block). It reads those filters
  window by window, term by term, each with the clause's `when` in front of its own (`ScopeComposedTerms`): the clause's
  `when` is read in the filter block at the terms' rank, in group 0, and each term's filter, its `when` and member
  tests, in the group after its predecessor's (`PartComposed`), so a term's error in an earlier window ranks before the
  `when`'s in a later one. The objective's `when` is read so too where its composed terms alone carry it, flat's
  objective having no term to read it with (`LogicalDecide::retained_objective_when`), unless its scope check reads it
  first (D72). A value error is found by reading its window entry by entry.
- *Formulation* (`decide_model_emitter.hpp`): the model emitter, an Adapter onto stage 06, writes what
  `PhysicalDecide::Evaluate` writes for the model, gstate's evaluated clauses and the keyed mappings, in flat's order,
  through flat's own blocks (`InitializeEvaluatedConstraint`, `DescribeBound`, `SkipNullInstances`, `CountInstances`),
  so the tree and flat formulate through one code path. A clause in the aggregate shape keeps the rows its root holds,
  groups them by the slots of its G of the `by` key, numbered by value, and emits, skips and counts them through flat's
  own blocks (`GroupRowsByShape`, `SkipGroupsWithAnEmptyReducer`, `CountTupleTermsOncePerGroup`, `ScaleAvgTerms`): a
  reducer's term is read on the members of its G, a kept row in no group among them, where a NULL or a decision with no
  instance leaves its group no value, and a term beside the reducers on every kept row. Its groups are named by their
  least spelling (`DecideGroupKeyValues`). A keyed decision with no class at a row, or a NULL coefficient or bound,
  skips its instance as flat skips it, and the skipped instances are reported unless a
  clause or the objective raised. An objective term in a reducer is read on the members of the G it ranges over, its
  operand's entries its member tests keep, so a reducer that keeps none drops its terms (D84); a term outside every
  reducer is read on each row the objective keeps and counted once (`CountTupleTermsOnce`). Either is dropped where its
  decision has no instance, as flat drops it (D84). A relation-qualified reducer reads the first entry of each instance
  of its operand, its first row, however many spellings split the class into entries, so the tree's columns are already
  what flat's de-duplication keeps, and AVG then divides them as flat does (`ScaleObjectiveAvg`; by the G's slots for an
  AVG read in place, D81). An objective with a second variant over its kept rows raises flat's message (D72): both
  evaluators build it with `DecideObjectiveTwiceMessage`, flat reading the signature's fields from the rows and the tree
  from its root's entries (`ObjectiveDifference`), and a frame field's reason with `DecideFrameDifference`: two
  positions of one timeline, two timelines (which D75 refuses first), or two values its `else` reads, a NULL one value
  as flat groups it (the tree compares the reads as `Identity` does). A clause in the per-row shape under `per K:` emits
  one model row per variant of each class of K among its kept rows, its root's instances, numbered as flat numbers them
  (by value, its class's and then its fields', or by first row where a field reads a decision's instance or a frame's
  keys) and placed through flat's own block (`DecideOneRowPerVariant`): on its first row, or the first
  with a value of the auxiliary the clause defines (D88). It is named by its class, or, where its class holds more than
  one variant, as flat's reader names it (`DecideNameVariants`): by what its class key and its fields read at its first
  row, as written (`DecideBody::names`, programs on its root), and a collated column of its class by its least spelling
  among the clause's rows of the class; flat names a class of one variant by its key's least spelling over every read of
  the key (D69, `DecideDescent::Label`). The variants are counted for EXPLAIN ANALYZE as flat counts them. Under
  DIAGNOSE the classes each model row stands for are flat's (`DecideGroupClassSets`, `DecideNameVariantClassSets`), each
  kept row's class of K read from its root's slots; so a grouped root under `per K:` stays keyed by K, its instances a
  class of K and a group of the `by` key each, though the clause's model rows are its G's groups. A relation-qualified
  reducer whose classes its body's values split (`KeyedScopeInfo::signature`) takes the instances of its operand
  (`DecideTree::qualified_operands`) for its classes, numbered as flat numbers a signature's variants, a row it does not
  read in a class of its own. A row its `when` keeps in no class of its key stays in its group, as flat keeps it: its
  terms are computed and checked there, and the reducer reads no value of it. An auxiliary has no value where its
  definition reads a NULL unreduced or a decision with no instance (`absent_reads`, D88): the definitions are read in
  `eval_order`, which puts what defines a decision before what reads it, and a row reading an auxiliary with no value
  skips its instance, as the objective drops its term there. A `<>` clause's rows are flat's, and an AVG under `<>`
  multiplies its bound by its group's count (`HoistAvgIntoNotEqualBound`). An easy MIN or MAX, which its direction
  drives (`DecideConstraint::was_minmax_easy`), is a clause in the grouped shape: a model row per member of its G,
  grouped by its `by` key, whose terms are read as written on the member's own row, not counted once in its group. Its
  bound on one decision is admitted on each member (`DecideModelEmitter::Admit`), an entry of its term's site, the
  reducer's operand, whose filter (`admitted_member`) the descent routes, so no program tests it; a row no member stands
  on emits no model row, so what flat admits there, every value, is never read. An admitted end is never NULL, since a
  NULL coefficient or bound admits every value (`DecideReadSingletonRow`): flat's marking of a row whose end is NULL is
  reached by no query, so the tree has none, and a NULL end is an internal error. A hard MIN or MAX
  (`minmax_clause_idx`) hands stage 06 its groups' rows, over which stage 06 builds the extremum and the outer row it
  stamps USER_PARAMETER. A MIN or MAX over a relation's classes reads every row an entry of its operand stands for, as
  flat reads each a member: stage 05 gives its terms no qualifier, since a class's repeats carry its value
  (`ReadRows`). A MIN or MAX objective's members are the rows its terms' filters keep, which stage 06 reads. A composed
  clause's root holds each term's G (`DecideBody::composed`), and each term is a column over its G's members, a
  relation-qualified one's first row of each class, with flat's name and label (`DecideComposedTerm`). A qualified
  term's operand is the one its scope's keyed mapping reads, as a written reducer's and a helper's are
  (`DecideTreeBuilder::Qualified`), so a scope whose classes its values split (D70) has them. A term's factor is one
  value for the whole query, read as flat reads it, on the buffer's first chunk, fetched once,
  whatever the clause keeps (`DecideQueryWideScale`). A NULL factor or a reducer over no row skips the clause
  (`DecideComposedTermReadsNoRow`); its one instance holds where its `when` keeps a row, the rows its root holds, and
  under DIAGNOSE it stands for the classes of its `per K:` among those rows, by flat's own tail
  (`PhysicalDecide::FinishComposedClause`); the objective drops a composed term over no row. A composed
  term's G names its clause in a message, as its root does (`DecideNode::source_clause_id`). A clause's products and
  squares are read once its rows are grouped (`DecideModelEmitter::Polynomial`), each term on the rows its site's
  members stand for, a relation-qualified one's first of each class, which flat computes it on once it has left the
  repeats out; their filters decide the rows a group emits and the groups a reducer over no row leaves no instance, as
  a term's do. A product's coefficient of 1, which stage 05 leaves out, reads its sign. A NULL coefficient, or a factor
  with no instance at its row, skips the row's instance. A square's factor is read as a composed term's is, on the
  input's first row, whatever the clause keeps, and flat's block applies it (`ScaleSquaredGroup`, its NULL, NaN,
  division-by-zero and too-large messages). The objective's squared terms are read as its terms are; its products on
  the rows they are read on, with no row dropped for a decision with no instance, as flat's evaluation drops none, and
  one outside every reducer is counted once on the first row the objective keeps (`CountProductsOnce`, which flat
  shares). A product's auxiliary has no value where a factor has none (`DecideTreeColumns::AbsentAt`), which the
  definitions reading it see and stage 06 reads (`absent_reads`). A row defining an auxiliary reads its products'
  factors and its squares' terms as it reads its terms' decisions, and its squares' coefficients as its terms', but no
  product's coefficient, as flat read none; no part of a definition lies in a reducer,
  which would make the definition a value link. A MIN's or MAX's helper row per row of a product or a square reads the
  reducer's body on the reducer's members: their NULL tests are its root's member tests, and the row stands, and its
  NULLs and errors are read, only on an entry they keep (`Rows`, `NullEntries`), as flat reads them under the row's
  `when`. A helper decision of a reducer the value rewrite lifted lists its members per group of its `by` key that holds
  one, through flat's own blocks (`DecideValueLinkSpec`, `DecideListRowMembers`; `DecideModelEmitter::ValueLinks`): a
  member is an entry of its G's operand that its reducer's `when` routed and its NULL tests keep, where every decision
  it reads has an instance, a relation-qualified reducer's first of each class, and an AVG's coefficients are divided by
  its group's members (`ScaleAvgRows`). A row in no group of the key is computed on, as flat computes it, but lists no
  member. The groups that hold a member, numbered by rank among them, are the helper's classes, which every row of a
  group the clause keeps reads (`DecideModelEmitter::HelperMapping`, `DecideMapRowsInNoClass`); a row of a group with
  none reads no helper, so what reads it there skips its instance. The definitions and the helpers' classes are read in
  the tree's order, which puts what defines a decision before what reads it, a helper no clause reads but through a
  product's auxiliary included (its root of its own). A member's message names the helper's reducer as written, which
  every reader shares. Flat reads a helper's rows where it builds its classes, before any clause: the rows its path
  keeps (the reading clause's `when`), its reducer's `when` and its members' NULL tests, then its `by` key on every row
  the path keeps. The tree ranks them there, in flat's order of the keyed mappings: every scope in turn, a helper's
  after the definitions of the auxiliaries its members read, then every auxiliary's definitions
  (`DecideTreeBuilder::KeyedMappingOrder`). Flat's other recursions number nothing earlier, since the declared scopes
  come before the rewrites' helpers and the rewrites number a nested helper or auxiliary before its reader. A `when`
  the helper's G inherits from a node above it is read there at the G's rank (`DecideNode::when_rank`; a frame's O
  inherits one the same way, where flat builds the frame). A sigma field naming a reducer's `by` key reads it as a
  `by` key's read, which keeps the row, so the node grouping by the key raises what it raises: the G, after its
  members' rows (`DecideBlock::BY_KEY`). Each helper's members are read once, with its classes, and listed from there;
  their coefficients rank after the composed clauses. Stage 06 then writes a helper
  read once where it is read (D89) and accounts for it as flat does. A frame's term reads its decision at the row the
  frame selects, and has no value where the frame reads nothing or the decision has no instance there
  (`DecideTreeColumns::TermAbsentAt`, flat's `DecideFrameReadsAbsent`), wherever a term's absence is read: a clause's
  rows, the objective's entries, a definition's absence, a helper's members. A range over decisions' helper has a class
  per window (`DecideFrameWindowMapping`) and lists its members per window through flat's block
  (`DecideListFrameMembers`), its payload's and its `else`'s coefficients programs on its O, read on the rows its
  windows read and the anchors that reach the `else`, after every clause (VALUE_LINKS). A signature's frame field
  splits the variants by the frame's keys, the anchor's position as its order key compares when it reads relative to
  the anchor, else its timeline, and by what its `else` reads where the anchor reaches it (`Identity`, `Fallback`); an
  anchor where the frame reads nothing is no variant, though its class still counts, as flat counts the classes over
  every kept row (so two variants of one class beside a class whose anchors read nothing are named by their class
  alone, both alike, as flat names them: open); the variants number by first row, and each is named by the frame's
  keys, a collated position by its least spelling (`DecideBody::position_names`). A grouped clause's tightest
  frame bound (D85) bounds nothing where it is NULL or its row lies in no class of the clause's `per K:`, and each
  group takes its tightest value through flat's block (`GroupBound`, `TakeTightestFrameBound`), whose `=` message ranks
  at its clause, after its parts. A NULL coefficient of a reducer's member or a term beside the reducers, through a
  value the whole query shares, leaves its group no value (`ReadInReducer`, `ReadBesideReducers`). One of flat's rules
  has no arm, since no query reaches it: its leaving out of a hard MIN's or MAX's group a row its terms' filters all
  drop (its terms share one reducer's members, so no row the tree emits is one). A reducer's member reading an
  auxiliary with no value, which flat skipped the group of a clause in the aggregate shape for, is a helper's member in
  every probe, which the helper leaves out. Two differences are
  open, each where no query is known to reach it: an objective term
  reads an auxiliary's absence at its entry's first row rather than on each row the entry stands for (`ReadEntries`),
  which differs where the absence varies inside an entry; and a variant whose first row D88 moves to a later row of its
  entry is named by the entry's first row.

No side plan is planned: the tree folds the data reducers in the descent. Flat's side plan read a clause's `when` on
every row, with the data reducer's arguments it gates, before the query's other reads; the tree reads the `when` at the
clause's rank and the arguments only on the rows it keeps, so an argument's error ranks before an error of the `when`
that the side plan met first (open). A record no clause reads, an orphan's hidden root, is folded by no reader, so its
errors are not raised (open: no query is known to make one).

**Cost**, measured before flat was deleted, against flat on one binary (`benchmark/decide/anr_scaling.py --against` a
flat run): the tree's own time at 10^5 rows is 0.84x to 1.33x flat's on every workload (`flat-sum` the most), and its
peak memory 1.01x to 1.20x; at 10^6 `flat-sum` is 1.43x (132 against 92 ms). On TPC-H `large` (`run_benchmarks.py
--sizes large`) the wall time in total is within 1% of flat's, the model's construction 13.5% over it (2,699 against
3,063 ms; the most on the smallest queries, Q5 3.9x of 2.5 ms and Q7 2.3x of 21.5 ms), and the peak memory at most
1.30x. Before the descent was vectorized, its keys and entries one `Value` at a time, it was 2x to 32x flat's time and
1.4x to 6.3x its memory at 10^5 rows, and 66% over flat's wall time on TPC-H `large`. Growth stays linear in the rows.

**The tree against flat, query by query (G5).** Before flat was deleted, the whole suite's output with the tree alone
and no side plans (`test/decide/capture_suite_log.sh capture`), compared with two runs of flat's
(`suite_log.py OLD NEW --again OLD_AGAIN`), in the modes `auto`, `highs` and `off`, differed on 15,269 queries in four
ways, each with its cause:
- *G5-TIE*: a query with several optimal answers (a BOOL product's objective, a sum over `per` groups, a feasibility
  query) prints another one. Flat prints several of them across runs of one binary, as rows of a `UNION ALL` reach
  the sink in another order; the answers the tree prints are among them.
- *G5-PLAN*: EXPLAIN differs where side plans would be: with none, no `direct_source` or `direct_bound` CTE is
  planned, and a record's placeholder shows as the aggregate it stands for (`min(CASE WHEN ... END)`), not as its
  column (`#3`). So does the plan a stored table's copy leaves out (`test_r1_copy_...`).
- *G5-EQ*: an `=` bound that takes two values in a group names another group's two values: flat's side plan raises
  in the order its window reads its partitions, by hash, and the tree's check in the order of the rows.
- *G5-ENV*: a test that set the evaluator itself recorded another environment in each log, so its queries paired
  with none; their output was the same. One test failed under the tree (`test_null_values`, a profile span flat alone
  had; it now reads the term's member test from `DECIDB_DUMP_BODIES`).

Switches for the tree: `DECIDB_DUMP_TREE`, `DECIDB_DUMP_CONTAINERS`, `DECIDB_DUMP_BODIES` and `DECIDB_DUMP_VALUES` print
the tree, the containers, the bodies and their programs, and the columns' values, each node's programs the evaluation
computed on its entries (`DecideTreeColumns::Dump`), to stderr. `DECIDB_DESCENT_CHUNK` sets the descent's chunk size;
`DECIDB_EVALUATOR_INJECT=g1`, where the tree is built, makes stage 05's hook find no node for a reducer, which is
internal; `DECIDB_TREE_INJECT=node:data:value` replaces a G over data's fold in the columns, so a test can show a parent
reads the bottom-up values, and refuses a node that computes no data value. Profile counters (`DECIDB_PROFILE`):
`execution.input_scans` (scans of the input, 1) and `execution.descent_rows`. Tests: `test_tree_emitter.py` (flat's
model and flat's error, the objective's reads, the aggregate shape's rows, the data reducers, the reducers' filters, the
`by` key's rank, the rows in no group, the rows per class and variant, the classes a diagnosis reports, a qualified
reducer's split classes and its rows in no class, a variant's names as written, an auxiliary's absence, a definition's
error ranks, `<>`, an easy and a hard MIN or MAX, a MIN or MAX objective, the rows a reducer over a relation's classes
reads, a composed clause and objective, their qualified terms and their errors, a composed `when`'s place among its
terms' filters, a clause's products and squares and their filters, a square's factor and its messages, each square read
whole in turn, the objective's squares and products and their filters' place, a product's auxiliary with no value where
its factor has none, a definition holding a product or a square, a helper's row on the reducer's members, a data norm's
fold, a helper decision's members and classes, its rows', members' and `by` key's errors and its clause's `when`'s, a
helper read once written where it is read, a frame's reads, its decisions at the selected rows, its variants and their
names, its refusals and their ranks, the spellings a collated timeline is named by (D12), a reducer over frames' fold,
and a tightest frame bound).

## 2. The folded variable box

Whether a simple bound (`x <= 10`) becomes a column box or a model row is decided
at stage 05 — see [`../05_optimizer/done.md`](../05_optimizer/done.md), "Singleton-row
folding". This stage consumes that decision and does not re-derive it.

The sink state copies `op.singleton_row_boxes` and `op.user_singleton_row_bounds` in its
constructor. Three consumers read them:

- `Finalize` copies each box into `SolverInput` through `EffectiveLower`: a decision
  with no lower bound written takes the default 0 floor, so it is non-negative unless
  the query explicitly said otherwise. A constant lower bound under a `when` or a
  reducer filter then replaces that floor on the instances its rows reach
  (`ResolveLiftedFloors`, [`../06_model_formulation/done.md`](../06_model_formulation/done.md) §2).
- The domain-contradiction guard below rebuilds a box from the bounds as written
  (`user_singleton_row_bounds`) to reject a user bound that contradicts a variable's
  intrinsic domain, quoting the bound's clause as written. It reads the written values,
  not the admitted ones: `x = -0.5` on an INT admits nothing, and that is an ordinary
  infeasible, not a claim that `x` is non-negative. A negative floor under a `when`
  (`DecideConstraint::lifted_floor`) counts as lowering the box: the rows it keeps may
  reach below 0, so `x <= -2` beside `when p: x >= -5` is left to the solve, which
  reports the rows no such floor keeps.
- `BuildSolverInput` re-opens each `UserBoundSpec`'s direction in the rigid box, which
  `OpenElasticColumnBox` then widens for a diagnosis (below).

Flattening skips any comparison tagged `SINGLETON_ROW_BOUND_TAG`, so a
singleton-row bound does not also produce `num_rows` redundant per-row model rows. The
comparison itself stays in the tree, which is why `EXPLAIN` still renders the bound
the user wrote. A recorded one is flattened aside into
`DecidePreparedModel::singleton_row_constraints`: under DIAGNOSE the model emitter
writes it after every other prepared clause (numbered after them, `REPAIR_ROWS`) into
`evaluated_singleton_row_constraints`, and only the elastic input receives it, so the
repair model loosens it one kept row or class at a time (D28) while the solved model
keeps the box alone. Those clauses are the repair model's only rows for a folded bound.
A bound against a data column (`x <= cap`, `cap * x <= 10`, an easy `max(cap * x) <= 10`)
is not tagged: it flattens to a per-row spec that carries the classifier's admitted range
(`DecideConstraint::admitted_lower` / `admitted_upper`, stage 05). The ends read the clause
as `a * x <comparison> r` through two inputs, `admitted_a` and `admitted_r`, which both ends
read many times: the tree computes the inputs once, as programs on the clause's entries the
reducer filter `admitted_member` keeps, then both ends over them (`DecideModelEmitter::Admit`);
a row that is no member admits every value. An input that is a constant is in the
ends already, and stage 08 constant-folds each tree it evaluates once (`ConstantFoldingRule`,
in `TransformToChunkExpression`, after messages have rendered from the prepared trees). It
evaluates the ends on the kept rows in the same pass that reads a data right-hand side,
after it, admitting ±infinity as an open end, into
`EvaluatedConstraint::admitted_lower` / `admitted_upper`, and the model builder folds each
row into its column box when no diagnosis is armed (`../06_model_formulation/done.md`,
Path 3).

### The elastic box

`OpenElasticColumnBox` turns the solved model's box into the box the elastic model is
*formulated against*: every direction a repair may loosen is widened, so each is enforced
only by the loosenable row the diagnosis emits beside it. Both revertible layers move —
the user bounds stage 05 folded, and the tightenings `DecidePropagateImpliedBounds`
derived — which is why the test is simply "is this direction narrower than `rigid_*`?":
propagation only ever narrows, and stage 05's folding is already re-opened in the
rigid snapshot. A BOOLEAN's [0,1] is intrinsic and never widens past it, so a user pin on
one becomes loosenable while the 0/1 domain stays rigid.

It works on `SolverInput`, before formulation, rather than on a built `SolverModel` —
that is the whole point. The constants that go stale against a re-opened box (Big-M,
McCormick) are derived by the formulation pass, so the box has to change *before* that
pass runs, not after. `PhysicalDecide::FormulateElasticModel` widens the box on a copy of
the retained pre-formulation input and re-runs `FormulateModel` against it, so the
elastic model's constants describe the box it declares.

**The widening is bounded, and bounded per column.** An open column has no finite Big-M, so
a formulation against a fully re-opened box does not lose precision — it refuses the query.
Each loosenable direction is therefore widened by a finite amount taken from that column: ten
times its own magnitude, or whatever a row demands of it (`|rhs| / |coefficient|`), whichever
is larger. No model-wide quantity is involved, so an unrelated large value cannot inflate
this column's constants; a row that genuinely demands a much larger value still can. The
reasoning, and what the looser constants cost in numerical noise, are in
[`../../diagnostics/infeasible/done.md`](../../diagnostics/infeasible/done.md)
"The elastic model is a formulation, not a matrix patch".

### Domain contradictions are a static error, not a diagnosis

A user bound that contradicts the variable's *intrinsic* domain is deterministic —
loosening cannot help, changing the type can — so it throws here, in the main
pipeline, before the elastic engine ever sees it, quoting the bound as written
(*"per g: max(x) <= -1 cannot hold because x is non-negative (>= 0)"*):

- `x <= -1` on a non-negative type, guarded as `U < 0 AND L >= 0` so that
  `x = -1` or `x <= -1 AND x >= -5`, which explicitly lower the floor, still pass;
  a negative value in an `IN` lowers it too (`x IN (-5, 0, 5) AND x <= -3` answers -5).
  An `IN`'s bounds are removable (diagnosis may drop the whole clause), so the guard
  rebuilds the box from the retained bounds and keeps of the removable ones only a
  negative floor: that is a fact about the domain, not a bound the user wrote;
- `x >= 2` or `x = 2` on a BOOLEAN.

A purely user-vs-user inverted box such as `x >= 5 AND x <= 1` does **not** match
and proceeds to the elastic engine, which reports a least-change loosen.

### Implied bounds from constraint data

Once coefficients are numbers, a second, data-driven source of column bounds
becomes available. Stage 08 invokes it — `DecidePropagateImpliedBounds()` — but
does not own it: the pass is pure over `SolverInput` and lives at stage 06. See
[`../06_model_formulation/done.md`](../06_model_formulation/done.md) §9.

The call is ordered here, immediately after the singleton-row bounds are resolved and
before any Big-M is computed, because every downstream constant depends on the
tightened box.

The pass is a single sweep, not a fixpoint: a bound derived for one variable is
not fed back to tighten others in the same pass. It skips constraints it cannot
reason about soundly — any negative coefficient, bilinear or quadratic terms,
MIN/MAX and `<>` indicator rows, and a clause with rows outside its row map, such as one
under a `when` (whose bound would wrongly cap the excluded rows, since the derived bound
is shared across all of a variable's rows). A `per K:` clause with no `when` is not
skipped: each row it leaves out stands with its class's first row
(`rows_stand_with_their_class`).

---

## 3. The prepared linear form

Execution does **not** flatten expressions. `LogicalDecide::prepared` arrives
already decomposed into additive terms by `BuildDecidePreparedModel` (stage 05,
`src/optimizer/decide/decide_linear_form.cpp`), moved into
`PhysicalDecide::prepared` at physical planning. `DecideGlobalSinkState` aliases it:

```cpp
const vector<unique_ptr<DecideConstraint>> &constraints = op.prepared.constraints;
const unique_ptr<DecideObjective> &objective = op.prepared.objective;
```

**What this stage receives.** For every constraint: its terms, each naming a
variable index, a sign, a still-unevaluated coefficient `Expression`, an optional
filter (its reducer's own `when`), an AVG-scaling flag and the key scope of its
reducer's own `per` (`qualifier_scope_idx`); plus the RHS expression, the comparison
type, the clause's scope (`DecideClauseScope`: `when`, `per`, `class_key`), its `by` key (`group_key`), and the metadata
linking it to indicators and diagnosis provenance. The
objective arrives the same way, with its `when`, each AVG term's `avg_group_key` (an index into the key table, set
for a nested AVG read in place), and `squared_terms` and `bilinear_terms` alongside `terms`.
`ComposedMinMaxTerm::inner_terms` carries the composed MIN/MAX path's terms, so no
construct is left re-deriving its own.

**What this stage does with it.** Evaluates the coefficient expressions against the
materialized rows (PHASE 2), and nothing else. Which variable a term names and what
multiplies it were settled upstream, where a binder was in scope.

**The shape is asserted, never repaired.** A decision variable on the RHS, a
non-reducer term in an aggregate LHS, an unrewritten MIN/MAX — each is an internal
invariant failure raised by the flattening pass at plan time, not something
execution fixes. The structures themselves are documented in
`src/include/duckdb/planner/decide/decide_prepared_model.hpp`; the algebra that
produces them, and why it cannot live here, is
[`../05_optimizer/done.md`](../05_optimizer/done.md) §1a.

---

## 4. PHASE 1.5 — entity mappings

For each `KeyedScopeInfo` — a keyed decision's key (`per K: x`), a reducer's own `per`
or colon form (`sum(per K: e)`, `SUM(D: e)`), a helper decision's `by` key — before
coefficient evaluation:

1. Read the scope's key in the descent, which reads every row for a declared decision's
   scope (its classes belong to the source as well as the clauses) and a helper's where
   its rows are reached: the key's dictionary is a `DecideKeyMap` (§5). A row with
   NULL in a written key column is in no class; a relation's columns compare NULL as a
   value. A key with no columns (a reducer's `per ()`) is one class, every row in it.
2. A helper decision's scope (`KeyedScopeInfo::value_link`, the value rewrite) reads its
   link's path key on the rows its path keeps only (`ReducerValueLink::when`: the clause's
   `when` and every enclosing reducer's own), so the key is never
   evaluated where the clause never reads: one entity per group that holds a member,
   numbered as its definition numbers them. A group with no member has no helper (its
   reducer is NULL there, D68), so its rows map to the absent entity below.
3. A row in no class (a NULL written key, or a row a helper's clause drops) has no
   decision. It maps to one spare entity, `EntityMapping::absent_entity` (the Null Object
   pattern), so every row has an id and `VarIndexer::Get` stays total. The model builder
   fixes the absent column at 0, so an objective term that reads it is dropped; a reducer
   over the decision never reads the row (`DecideCanonicalizer::SkipNullData` tests the
   key's written columns for NULL), and an unreduced read skips its instance
   (§5). The output shows NULL there.
4. Entity ids are the key's group ids: numbered by key value (below), so a keyed
   decision's solver columns follow its key's values, not the rows' order.
5. Fill a `row_to_entity` vector of size `num_rows`.

The result is an `EntityMapping { num_entities, row_to_entity, absent_entity }` on the
`SolverInput`, which is what makes several rows share one solver column.

---

## 4b. PHASE 1.4 — frame selectors

Once the descent has its O's timelines (§1a), before any clause is read, `DecideTreeColumns::ReadFrames`
(`physical_decide_frames.cpp`, `decide_frame_map.hpp`) builds a
`DecideFrameMap` per frame selector (`../../language/guide.md` §5.6): its anchors are the rows its scoped expression
reads (`DecideFrameSpec::rows`), grouped into timelines by its timeline key through the one key cache, so a row whose
written `within` column is NULL is in no timeline (D75). Each anchor's order key becomes a DuckDB sort key -- the
frame's direction, NULL where `default_null_order` puts it (D74) -- and each timeline's anchors are stably sorted into
peers, rows of one order-key value. A position counts peers: `first`, `last`, `k previous`, `k next`, wrapped when
cyclic, missing past either end. Under a `per` that does not hold the timeline, `CheckDecideFramePartition` stops a
class that spans two timelines (D75).

`AppendDecideFrameColumns` then adds to the pass's columns, after the input's, each column stage 05 registered
(`DecidePreparedModel::frame_columns`): a payload is read on the first row of each selected peer only,
an `else` on the anchors that reach it only, so an unused one raises nothing (a cast that would fail there does not), and
the values are gathered onto the anchors. Every later phase reads the frames as data columns, and readback references
the input's own columns only. Once the keyed mappings exist, `CheckDecideFramePeers` holds each position a frame reads
-- a point's selected peer, every position a range's window visits, one whose NULL reads left it nothing to read
included (`DecideFrameMap::WindowVisits`) -- of several rows to one signature of its payload (D73, deck p. 110): each
column the payload reads by value (`AppendPayloadLeaves` over `DecideFramePayloads`: a point's columns, a range's
reducer argument or its helper's member terms), each decision by its instance, whatever its coefficient. A
frame's decision terms read `gstate.ReadVariable`: a frame read numbered once per decision and frame
(`SolverInput::frame_reads`, stage 06), its rows the selected ones; a query-wide decision is its own read.

Where a frame has nothing to read, every column of it holds NULL, which is no value (D84) as any NULL a clause reads:
the instance is skipped (`SkipNullInstances`), a reducer's member is out of its filter, an objective term is dropped. An
anchor in no timeline is such an anchor whatever its `else` (`DecideFrameMap::Untimed`): it misses no position, so no
`else` stands in, and it is absent (D75). A frame over data is that column alone, so a scalar function around it reads
the NULL as SQL does -- `coalesce(at(previous: t.d) over (...), 1)` is 1 there -- and only a NULL that reaches the
comparison skips it (deck p. 56). A frame over decisions is its payload's terms, each read at the selected row
(`DecideTreeColumns::AbsentAt`): where the frame has no value, or the decision has none at the selected row, the term
reads an instance with no value, as below.

**No value is one mark per decision instance** (D84, D87, D88): `DecideTreeColumns::AbsentAt` answers whether the
instance a row reads of a decision has a value. A keyed decision has none on a row in no class of its scope (its
mapping's absent class), and so has a helper's group with no member. An auxiliary -- an ABS's, a body helper, an L0
indicator, a product -- has none where the rows defining it (`DecideConstraint::defined_var`, the `DEFINES_TAG_PREFIX`
stage 05 reads; a product's factors) read an instance with none (`DecideGlobalSinkState::absent_reads`). The model
emitter (`ReadAbsence`) resolves both in the order the definitions read each other -- a helper's members may read an
auxiliary whose definition reads a helper made after it -- whatever order the rewrites numbered them in. Every reader
reads that one mark: a comparison skips the instance, a helper drops the member, the objective drops the term. A row a
rewrite added to define an auxiliary (`ConstraintKind::STRUCTURAL`) is no instance of the clause: it is skipped where
its own reads have no value, and the auxiliary with it.

A frame field of a signature (D82, deck p. 99) splits by the frame's keys (`DecideDescent::Signature`): the anchor's
peer on its timeline when it reads relative to the anchor, else its timeline, and where the anchor reaches the `else`
what the `else` reads -- its FALLBACK columns, referenced once per query (`DecideGlobalSinkState::frame_column_refs`) so
the chunk expression cache, keyed by node, never meets a freed one, and its decisions' instances at the anchor. An
anchor where the frame reads no value is no variant, so the objective's check (D72, `DecideFrameDifference`) counts only
anchors that read one, and names what differs among them: a position, a timeline or a fallback.

A frame bound of a clause over groups that differs within a group (`DecideConstraint::tightest_frame_bound`, D85) is read row by
row as any bound, its NULLs apart, and then each group's kept rows, a row a reducer's own `when` drops included, take
the group's tightest value among those where it has one (`TakeTightestFrameBound`): the least under `<=`, the greatest under `>=`, the one value under `=`, whose second is
the `=` message a side table raises for a column; a group with none is marked in `null_rows`, so its instance is
skipped. A row in no class of the clause's `per` key is not read at all, as `InClassOnly` keeps a column's from being
read (D5). The value is taken last, just before `SkipNullInstances`, and only for a group the clause still holds: a group
some rule dropped, or a NULL on its left side will, has no instance, so no bound and no `=` check (D68). Its frame
columns print as the frame is written (`FRAME_COLUMN_TAG`).

A reducer over data reading a frame (`DecideFrameColumn::Kind::AGGREGATE`, after every frame column) is computed once
the frames' columns are computed (in column order, so one may read another): its G folds its operand's entries the
filter keeps, grouped by its path key, each class of its own `per` once per signature (each frame by its keys), its
values reduced as SQL's aggregate does, a NULL skipped (`DecideAggregateStates`). Each row reads its group's value,
NULL in no group or where the group has nothing to reduce (`count`: 0).

A range's map lists its **windows** (`ListRangeWindows`): one per timeline when neither end depends on the anchor, else
one per position, parted at an anchor that reaches an `else` reading its own row. Each window reads a list -- the first
row of every `every`-th position from its first end to its last, whose other rows read the same payload (D73), a cyclic
position as often as it is visited (D76) -- and counts its missing positions, each of which reads the `else` once; a
position off a timeline that does not wrap is counted, never visited, so the work is O(values). One frame may list at
most `decide_frame_limit` values (checked before the work that lists them; an interrupt is polled per timeline and per
position). An anchor whose window reads nothing -- a reversed range, `all` meeting a missing position, no value at all
-- is absent (`DecideFrameMap::absent`). A range over data's `REDUCED` column runs the frame's own bound aggregate
through its state functions over each window's values (`ReduceRangeValues`), so SQL's types and NULL handling hold: a
NULL value is skipped, `count` counts the rest. A range over decisions is its helper decision's instances, one per
window (the emitter's `HelperMapping` reads the windows; an absent anchor is in no class), and its members are listed
per window (`DecideModelEmitter::ValueLinks`): a member per row read, times its visits for SUM and AVG, one for the
`else` times the missing positions, AVG dividing by the window's values. A value read as NULL -- a payload coefficient
at a row, an `else` coefficient at the anchor -- is no member (`SkipDecideRangeNullReads`, before the windows are
numbered), and so is a payload read of a declared keyed decision at a row in no class of its key, which has no instance
there (D84): a window left with none is absent.

## 4a. PHASE 1.6 — variants, and how a clause is read

A scoped expression with a variant signature (stage 04, D70) is numbered by its set E's
instances in the descent: the kept rows grouped by its `per` key (a NULL in a written key
column puts a row in no class, D5) and each field -- a `VALUE` by value, a `BY_KEY` by
its elements' values, NULL a value of both -- then each class of a keyed `DECISION` field,
and every row of a per-row one, splits its group again. Three readers:

- The objective's root numbers its signature over its kept rows (`DecideModelEmitter::ObjectiveDifference`): a second
  variant is the data check of [`../../language/guide.md`](../../language/guide.md) §7.3 (*"the objective would be
  produced more than once"*), naming the first field that takes two values with its least and greatest one (NULL last; a
  frame's two positions read first and last in its own order, `DecideFrameDifference`) or a decision with two instances,
  in one message (`DecideObjectiveTwiceMessage`).
- The model emitter (`Classes`) emits a per-row clause under `per K:` (`ONE_ROW_PER_CLASS`) once per variant of each
  class, at the variant's first row, which stands for the variant's rows: they agree on every field. Where a class holds
  more than one variant the clause records an EXPLAIN ANALYZE line (`RecordVariants`, `Kind::VARIANTS`, printed by
  `ParamsToString` from `variant_accounts`, which a plain EXPLAIN never fills) and names each variant for a diagnosis by
  its class's key values followed by its own field values (`DecideModelEmitter::NameVariants`: a key-values entry no key
  owns, so `ResolveGroupNames` keeps its numbering; a collated key column quotes the class's least spelling, D69). Under
  DIAGNOSE such a variant's row stands for its own class set.
- A reducer's variant scope (`KeyedScopeInfo::signature`) takes the signature's numbering
  over the rows the reducer reads, so its operand holds one entry per variant of each class
  (below).

The `VALUE_IS_VALID` checks the bound-side rewrite (`RewriteRhsAggregates`, stage 05) appends are kept on the node
(`DirectRetainRowChecks`) and raised by the tree before any clause is read (`DecideTreeColumns::CheckData`): a bound's
NaN, a NaN in a bound-side reducer's argument (the reducer skips a NULL, as SQL does), or an `=` bound that takes two
values in its group (`` `<bound>` takes more than one value here (a and b) ... infeasible.``). An empty group is no
check: a reducer over no rows is NULL, so its instance is skipped where the clause is read (D68; below).

Groups come from the descent's dictionaries (`DecideKeyMap`): a row with NULL in a written key column
(`DecideKeyNullMode::NO_GROUP`) is in no group, and a relation's columns (`VALUE`) compare NULL as a value. A key
with no columns (`per ()`, `by ()`, a reducer's `per ()`) is one group of every row the filter keeps, with no table.
A signature reads its `when` on the rows that reach it, and its key and fields only on the rows it keeps.

Every clause is then read by these rules; §5 gives the row maps they produce:

- **Kept rows first.** Its `when` is read before anything else, on every row, in one scan
  (`DecideDescent::Gate`), each window reading the parts of an AND or an OR left to right,
  each only on the rows the earlier parts leave undecided -- an AND's on the rows they keep,
  an OR's on the rows they do not (the composed rewrite folds the clause's `when` in front
  of a term's own).
  Anything else, `not` included, is computed whole. Nothing is then
  computed on a row the `when` drops: a reducer's own `when` is read on the kept rows,
  a term's coefficient on the kept rows its own `when` keeps, a bound, a `by` key and a
  class key on the kept rows: the descent passes on only the rows that reach a node, and
  each program runs on its node's entries. `ExtractDoubleColumn` writes 0 for a row it does
  not read, so a NULL or a failing expression there is no error.
- **Grouping.** The aggregate shape groups the kept rows by its `by` key
  (`keys[constraint->group_key]`), a NULL written key in no group: a group's members are the rows a
  reducer's `when` keeps (all kept rows for `<>`, whose bound values are all excluded),
  its bound the tightest over all its kept rows. A row the reducer's own `per`
  de-duplication drops leaves the group when the clause is a hard MIN/MAX
  (`minmax_agg_type`): the members are the rows the reducer reads, and a zeroed row
  would be a member pinned at 0. The rows shape under `per K:` emits one row per class,
  at its first kept row; under `when` alone, one per kept row.
- **Bound-side reducers** never reach execution: the bound-side rewrite
  (`RewriteRhsAggregates`, stage 05) made each a column of the source, a side table
  DuckDB evaluates over its own `by` groups of the kept rows, and reduced the bound of an
  aggregate clause to its group's tightest value.
- **De-duplication** for a reducer's own `per` keeps the first row of each class among
  the rows its `when` keeps (its operand's first entry of each instance, `FirstOfEachInstance`), within each group:
  a class whose first joined row the `when` drops is still counted.
- **The objective**: its `when` is folded into every
  term's filter, so reading, de-duplication and AVG denominators all follow it. A
  decision outside the reducers (`NONE`) keeps its coefficient on the first kept row only
  (`CountTupleTermsOnce`, over `CountOnce`), and so does a product of two query-wide decisions there (one inside a
  reducer is SUM and keeps every row). `CountOnce` is the one place a term is counted
  once, in the objective and in each group's row of an aggregate clause; stage 06 sums every
  row, and a debug build checks no writer left such a term on two rows (`VerifyCountedOnce`). A flat MIN/MAX
  that reads no row is dropped in formulation (D68).
- **Composed clauses** read the rows their folded filter keeps and de-duplicate among
  them. A clause in which some term reads no row is skipped, and an objective term that
  reads none is dropped (D68). Each takes its repair id here, after every prepared
  and absorbed clause (`ComposedConstraint::repair_group_id`), since formulation drops rows
  (a `<>` moved into indicator rows) and no count of the model's rows is the id space.
- **Classes of a grouped row** (D28, `DecideGroupClassSets`, under DIAGNOSE only). The rows of a clause over `by` groups
  -- an aggregate-shape group row, an easy MIN/MAX's member rows, a composed clause's one row -- stand for every class
  of its `per` key in their group: with `per K:` in front (`scope.per = NAMES_CLASSES`, `class_key`) the key's values
  among the group's kept rows (in key-value order); with no `per` (`NONE`) every kept row of the group; `per ()` the
  whole clause (one `WHOLE_CLAUSE` set its groups share). A per-row clause under `per K:` (`ONE_ROW_PER_CLASS`) calls it
  with its classes as the groups, so each class's row gets a set naming that class. One `SolverInput::ClassSet` per
  group with kept rows, its `Kind` saying which, referenced from `EvaluatedConstraint::group_class_set` (a composed
  clause's from its evaluated `class_set`, over the rows its `when` keeps); the model keeps the table and an expanded
  diagnosis reports the group once per class (`FanOutByClass`).
- **Helper decisions** (the value rewrite, `DecideModelEmitter::ValueLinks`): each link's
  reducer is read on the rows its path keeps and its own `when` keeps among them, less a
  row in no class of its own `per` (a NULL written key, D5) and a
  row at which one of its terms reads a keyed decision with no instance there -- a nested
  reducer's helper whose group holds no member, which is NULL (D68) and a reducer skips
  (D67, D87) -- grouped by its path key over those rows (`HelperMapping`, which gives the
  helper's classes too; a NULL written key in no group, only the groups that hold a
  member); its coefficients are its terms' programs on the operand's entries; its `per`
  de-duplicates within each
  group, a dropped row leaves the group, and AVG divides by the members left. The result
  is a `ReducerValueLinkSpec` carrying the members alone, with the link's label and
  provenance; stage 06 states the definition over them (a row per group for SUM/AVG on the
  sides the clauses push, an extremum link for MIN/MAX). A helper's floor is free
  (`FoldSingletonRows` marks it written) before the rigid box is captured, so a bound a
  clause folds into its box stands, and the static domain check skips it: a helper holds a
  reducer's value, negative wherever its terms are. A refusal in formulation names a helper
  by its reducer. The end of assembly runs stage 06's `InlineReducerValueLinks` on the final
  rows and objective (`assembly.inline_helpers`, D89), passing the decisions read where only a
  column stands and `SolverInput` does not show it -- a composed MIN/MAX term's, a folded
  bound's, a flat MIN/MAX objective's -- and where it inlines a helper the column space is laid out
  again and every clause's emitted rows, built against the old one for the implied bounds, are
  dropped to be built anew. The floors and implied bounds keep only what holds of a decision or
  an instance, so they stand. Everything above -- the members, the absent instances, the skipped
  rows -- is computed for an inlined helper as for any.
- **Counters** (`DECIDB_PROFILE`): `anr.frames`, `anr.frame_windows` and
  `anr.frame_positions` (one per point anchor, each value a range's windows list) once the
  frames are built; `anr.helpers`, `anr.helper_instances` (groups), `anr.members` and
  `anr.member_terms` once the helpers' members are listed; `anr.inlined_helpers` from stage 06
  -- beside the model's own `model.variables`, `model.linear_rows` and `model.linear_nonzeros`.
  `benchmark/decide/anr_scaling.py` reads them (`../../performance/benchmarking/anr_scaling.md`).

---

## 5. PHASE 2 — coefficient evaluation

### Expression transformation

`BoundColumnRefExpression` nodes reference columns by table binding;
`ExpressionExecutor` needs chunk positions. `TransformToChunkExpression` converts:

| Node | Becomes |
|---|---|
| `BoundColumnRefExpression` | `BoundReferenceExpression` on `binding.column_index` |
| `BoundFunctionExpression` | recurse into children, copy `bind_info` |
| `BoundCastExpression` | recurse into child, re-wrap with `AddCastToType` |
| `BoundAggregateExpression` | an `InternalException`: the bound-side rewrite made every reducer on the bound a column, and the binder keeps reducers out of coefficients and `when` conditions |
| anything else | copied as-is |

Results are cached per `Finalize` call, and the cache outlives every
`ExpressionExecutor` created below it, so executors may safely retain references
into cached entries.

**Precondition: resolution has already happened.** The `BoundColumnRefExpression`
branch uses `binding.column_index` as a chunk position, and the two indexings agree
only because `ColumnBindingResolver` already rewrote DECIDE's expressions. That
enumeration is maintained by hand — see
[`../03_logical_plan/done.md`](../03_logical_plan/done.md) §4.

### Per-term coefficients

Each term's coefficient expression is transformed, read window by window from the one
pass (§1a), and cast to `double` via `DefaultCastAs(LogicalType::DOUBLE)` —
**one conversion, on the result of the complete typed expression**, which is what
keeps a data cast's SQL semantics intact. Values accumulate into
`row_coefficients[term_idx]` indexed by global row.

Data-only LHS terms carry `INVALID_INDEX`. For aggregate constraints those fixed
coefficients are still evaluated per active row; the model builder subtracts their
row or group sum from the RHS.

### RHS

- **Constant** — extracted once and broadcast.
- **Expression** — evaluated per row, giving row-varying bounds.
- **Reducer** — a side-table column of the source by now (`RewriteRhsAggregates`),
  read like an expression.

The canonical RHS classification decides which: a bound tagged
`QUERY_WIDE_BOUND_TAG` is a shared scalar; anything with a row-varying component
stays data-backed.

### `when`, `by` and `per` → `row_group_ids`

One per-row vector assigns each row to a row of the clause, or excludes it. The clause's
`when` is read first (in the descent); every map is built over the kept rows only, with ids
dense in key-value order. A row with NULL in a written key column is in no group of that
key (`DecideKeyNullMode::NO_GROUP`, D5); it stays in the groups of every other key the
clause reads:

| Clause | `row_group_ids` | `num_groups` |
|---|---|---|
| no `when`, no reducer `when`, no `by`, no `per K:` over the rows shape | empty (fast path — all rows are one implicit group) | 0 |
| aggregate shape or easy MIN/MAX, `by (G)` | each row's `by` group, on the rows its reducers read (a kept row some reducer's own `when` keeps; every kept row for `<>`); `INVALID_INDEX` elsewhere | number of groups |
| aggregate shape or easy MIN/MAX, `by ()`, with a `when` or a reducer's own `when` | 0 on the rows its reducers read, `INVALID_INDEX` elsewhere | 1 |
| rows shape under `per K:` | the class id on each class's first kept row, `INVALID_INDEX` elsewhere | number of classes |
| rows shape with a `when` only | 0 on kept rows, `INVALID_INDEX` on dropped rows | 1 |

**One numbering per key** (`DecideKeyMap`, the descent's dictionary of the key, which its nodes outside an enclosing
slot share). A key is evaluated once on a row of a window, by the first node that reads it there. The descent finds
each row's group in DuckDB's aggregate hash table with no
aggregate (`GroupedAggregateHashTable::FindOrCreateGroups`, as GROUP BY and the recursive
CTE's UNION use it), which compares keys in place and names a group by the address of its
row; a group keeps its first row, not its values (late materialization). It then numbers
the groups by value through DuckDB's order-preserving sort key (`CreateSortKey`, ascending,
NULLS LAST, as `ORDER BY` and TopN compare rows), built once per new group and compared
bytewise: columns compare in key order, a relation column's NULL after every value. Each
reader takes the groups its filters keep as a subsequence, renumbered `0..K'` in the same
order, so a keyed decision's columns, a clause's group rows and a message's group labels
never depend on row order. A key is computed only on rows some reader keeps: an entry
records the rows it was computed on, and a reader needing rows outside them rebuilds it
over both sets, so the numbering stays one.

**A key groups under its collation** (D69). The planner gives each key element whose type carries a collation a
`collated` copy (`ExpressionBinder::PushCollation`, as GROUP BY binds its groups); rows group and number on it
(`DecideKeyElement::Grouping()`), and the descent keeps each group's least spelling of the element for its label (D69)
(a second scan, for a collated key only, once the groups are numbered, comparing sort keys), so a message quotes `'A'`
for a NOCASE group of 'a' and 'A' in any row order. A bound's side table groups on the same expression
(`direct_bound_tables.cpp`).

**A frame's order key compares under its collation** (D74). The binder settles it as it settles a key's and a bound
ORDER BY's, and `DecideFrameSpec::order_collation` holds it as an expression over the value the pass read, which
messages quote. A prepared frame keeps what binding settled whatever `default_collation` says at `EXECUTE`, and a
plan's round trip keeps it, which a function's result type alone would not.

The bound side of the aggregate shape gets its own map with the same numbering over every
kept row (a bound-side reducer reads its own `by` groups, §4a). A `per K:` over the
aggregate shape or an easy MIN/MAX adds no row: it only names the classes each group's
row stands for (D28, §4a); when its key leaves no class among the kept rows (every key
NULL), the clause has no instance.

**An absent decision reads like a NULL.** After the terms are read, each row where a linear,
bilinear or square term reads a decision on its absent entity (§4) is marked
(`DecideTreeColumns::AbsentAt`), and `SkipNullInstances` skips that row's instance as
it skips one with a NULL coefficient. So `sum(x) by (g) <= 0.5 * sum(x)` gives a NULL-`g`
row no limit (its `by (g)` helper is absent there) while the total still counts it.

The ids a key takes over every row are cached (`PerGroupCache`) across every constraint,
check and the objective, so one key is evaluated once however many clauses use it. A
lookup returns the groups its rows keep (`kept_groups`, each one's index in the key's own
numbering); a caller that names a group reads that group's values at its row
(`DecideDescent::Label`) and formats that one label, so no label is built for a group no
message quotes.

### A reducer's own `when`

Term-level filters are carried on individual terms, not on the whole constraint:

1. Each distinct filter expression is evaluated once into a boolean row mask, on the
   clause's kept rows only (`KeptRowsOfEach`).
2. Linear, bilinear and quadratic terms get coefficient `0` on rows their own
   filter excludes — **for that term only**.
3. A row joins a group when the clause's `when` keeps it and some reducer's own `when`
   keeps it (a reducer with no `when` keeps every kept row).
4. A reducer over no rows is NULL (D68): `SkipGroupsWithAnEmptyReducer` cuts every group
   in which some term's mask keeps no row, after the de-duplication masks are folded in.
   Only a reducer's term carries a mask (its own `when`, its NULL-data and absent-decision
   tests, its de-duplication), so a term with none reads every row of its group.
5. What the skips left is counted (D90, guide §7.4): each clause's instances once grouped
   -- in the aggregate shape every group of its kept rows by its key, from the key's
   cached grouping, whether or not a reducer reads a row of it -- and those left after
   `SkipNullInstances`, the most any of its rows holds (`ClauseInstances`). A composed
   clause is one instance where its `when` keeps a row. `ReportSkippedInstances` turns the
   counts into an EXPLAIN ANALYZE line under the clause and, for a clause left with none,
   a warning on stderr; `ReportAnEmptyObjective` reads the formulated objective (its linear,
   squared, bilinear and global coefficients) once the solved model is formulated.

So one clause can carry independent filters:

```sql
sum(when active: x * a) by (grp) + sum(when priority: x * b) by (grp) <= 10
```

`active` filters only the `a` term and `priority` only the `b` term; each `grp` group is
one row, over the rows either keeps. A group where `priority` keeps no row has no row:
its second reducer is NULL there.

### A reducer's own `per`, and `SUM(D: e)`

`sum(per K: e)` asks for one contribution per **class** of `K`, and the colon form `SUM(D: e)` is `sum(per D: e)` with
`K` every column of `D`; a join repeats each class once per matching row. De-duplication (`FirstOfEachInstance`) keeps
the first row of each class among the rows the reducer reads — the kept rows its own `when` keeps — and zeroes the rest.
A reducer whose body reads more than the key holds has a variant scope of its own (D71), so its classes are the key's
split by the body's values: every row of a kept identity carries the same value, and whichever row is kept gives the
same answer. A class whose first joined row the `when` drops is still counted once.

De-duplication runs **inside** each `by` group, after the clause's `when`, the reducer's
own `when` and the grouping. Group ids are dense in `[0, num_groups)` and class ids in
`[0, num_entities)`, so `(group, class)` flattens into one dense index with no hashing.
The mask is folded into the term's filter, so AVG's
denominator counts classes. For a hard MIN/MAX, a row the de-duplication drops leaves the
group (§4a).

### AVG scaling

`AVG` was rewritten to `SUM` by the optimizer and tagged `AVG_REWRITE_TAG`. Terms
marked `avg_scale` are divided by the number of rows the reducer reads in the row's group
(`ScaleAvgRows`):

| Clause | Denominator |
|---|---|
| `by ()` | the kept rows (every row with no `when`) |
| `by (G)` | the kept rows of the row's `by` group |
| a reducer's own `when` | further, only the rows it keeps |
| a reducer's own `per` | further, one row per class (the de-duplication above) |

In the objective, an AVG read in place from a nest (`sum(per k: avg(e) by (k))`, D81)
divides each row by its `avg_group_key` group's count among the rows it reads; any other
objective AVG by the rows it reads. A group no row reads is no group, so no denominator is
zero.

Quadratic AVG terms scale their inner linear coefficients by `1/√N`, so the
resulting outer product is scaled by `1/N`.

### Validation

Every coefficient and RHS value is checked after evaluation:

- **NULL** → the value is skipped (deck p. 50). In a constraint, `ExtractDoubleColumn`
  marks the row in `null_rows` and `SkipNullInstances` cuts the instance the row
  reaches: the row of a row constraint, the group of a grouped one (`INVALID_INDEX` in
  `row_group_ids`, which the model builder already skips). A reducer never meets a NULL
  summand: `SkipNullData` tested it in the reducer's filter. The objective never meets
  one: `DropNullObjectiveTerms` read it as 0 in stage 05, so a NULL there is an
  internal error.
- **NaN / Infinity** → `InvalidInputException` with `DecideInvalidValueMessage`: the
  value as written and the `when` that skips its rows.

**Infinity is admitted on one path only: the values that become a row's `rhs`.** An
infinite bound is not an error there — it is the absence of a constraint (`<= +inf`)
or one nothing satisfies (`>= +inf`) — and the model validator has always accepted a
non-finite `rhs` while rejecting NaN. A constant bound was never checked at all, so
the per-row expression path was the only one refusing what the rest of the pipeline
handled; canonicalization rebuilds `x + v <= K` as `x <= K - v`, which put the *same*
bound on the strict path purely because of how it was written.

The input of a **bound-side reducer** follows the same rule: `min(x) by (g) <= max(cap)
by (g)` is a window of DuckDB's over `cap`, and its value check (a VALUE_IS_VALID scope
check) refuses NaN only, so an infinite `cap` is a value; a NULL bound skips its instance. An infinite bound
behaves the same in every spelling, and its direction decides what it means. `inf - inf`
is still NaN, and still refused.

**An infinite bound keeps its SQL meaning on every path.** `x <= +inf` and `x >= -inf`
hold for every value, so they are no bound: the classifier's admitted end is open and the
column box stays as it is (a literal `x >= -inf` is the exception that frees `x` below, as
`x >= -1e30` does, D41). `x >= +inf`, `x <= -inf` and `x = ±inf` hold for no value, so
they stay a row, and `SolverModel::ProvesInfeasible` answers the model infeasible before
any backend reads it; `DIAGNOSE` names the clause. The rebuilt spelling `x + v >= +inf`
reaches the same row.

The exception is Big-M, which needs a constant that dominates the row's range, and
nothing finite dominates infinity. Every rewrite that faces this — `<>`, MIN/MAX,
ABS — classifies the bound before asking for an `M`, and all of that lives at
stage 06 along with the rows it guards. See
[`../06_model_formulation/done.md`](../06_model_formulation/done.md) §9.

The one Big-M still derived here is the auto-`M` refill for `norm(e, 0)` links,
whose `M` bounds an expression rather than answering a comparison, so there is no
bound to classify. It calls `DecideTightPerRowBigM` across the stage boundary.

A *variable* with no finite bound is the separate case, and the answer there is also
refusal rather than a constant — there is no fixed Big-M anywhere in DeciDB. The
order in which this stage calls the linearizers is load-bearing for it: ABS runs
first, because pinning an ABS auxiliary is also what determines its column box, and
every linearizer after it derives its `M` from column boxes. See
[`../06_model_formulation/done.md`](../06_model_formulation/done.md) §9.

### The construct routing — a decision this stage reads, not one it makes

Whether a construct is lowered is a **formulation** choice, and formulation belongs to
stage 05. This stage reads the answer off the operator and applies it.

```cpp
const bool native_abs = use_native_constructs.abs;          // decided by stage 05
const NativeConstructPolicy native_min_max {use_native_constructs.min_max,
                                            force_native_constructs};
const VarIndexer &var_indexer = ...;   // the flat column space, built by BuildSolverInput

DeriveAbsAuxiliaryBounds(solver_input, decide_var_names, /*refuse_when_unbounded=*/!native_abs);
if (native_abs) { EmitNativeAbs(solver_input, var_indexer); }
else            { LinearizeAbsMaximize(solver_input); }
LinearizeMinMaxConstraints(solver_input, var_indexer, decide_var_names, native_min_max);
LinearizeNotEqual(solver_input, var_indexer, decide_var_names);
LinearizeBilinear(solver_input, decide_var_names);
...
LowerDecideConstructs(solver_input, var_indexer, decide_var_names, use_native_constructs);
```

`use_native_constructs` is a `SolverConstructSupport` copied from
`LogicalDecide::use_native_constructs`, which stage 05 filled in from the chosen
backend before it rewrote anything. **This stage must not ask a backend what it
supports.** The routing has to be the same answer the rewrites upstream were selected
against, and a second, independently-derived answer is exactly how the plan and the
solve come to disagree about what was lowered. Deferring the choice also kept stage 05
from acting on it: see [`../05_optimizer/done.md`](../05_optimizer/done.md) §0.

**The `VarIndexer` is built first, before the column box is settled.** It reads
`num_rows`, the variable scopes and the entity mappings, all settled when
`BuildSolverInput` starts, and only the global block grows afterwards, with
`total_vars` refreshed before the solve. The floors a `when` lowers need its row to
instance map before the first box pass, and every construct site emits in flat
columns at the point it decides something, instead of stashing a decision for a
second pass to execute.

**MIN/MAX arrives as a policy, not an answer**, and the difference matters. It reaches
this stage as a `NativeConstructPolicy` — the capability, plus whether a declared
construct is used everywhere or only as a fallback — and the default policy is
*fallback*: the lowering is the smaller model wherever it is valid, so native is
reserved for the clause that has no valid Big-M at all. Whether a given clause has one
is a question about **evaluated coefficients**, which nothing before this stage can
answer, so this stage answers it — per clause, from the reach of that clause's own
family. That is applying a decision to data, not making one; the distinction is the
same one the `<>` range collapse already relies on. One statement can have a bounded
clause and an unbounded one, and each gets the formulation it can actually use.

`DECIDB_NATIVE_CONSTRUCTS` A/B-tests the two arms on one machine: `off` forces every
construct down its lowering path, `force` states every declared one natively, and `on`
(the default, and what an unset variable means) is the default policy. For MIN/MAX the
A/B has to be `force` against `off`: on a bounded shape the default *is* the lowering,
so comparing it against `off` would compare the lowering with itself.

The backend itself reaches this stage as a **name**. `PlannedSolverBackend()` resolves
`solver_backend_name` through the registry at the two points that are about to solve —
the primary solve and each diagnostic re-solve — so nothing here holds a solver handle
it is not immediately using, and nothing here re-selects.

**Only two constructs choose between two emitters.** `<>` does not choose: it is emitted once, as conditional rows, and
`LowerDecideConstructs` turns those into Big-M rows where the backend cannot state a
condition. Every MIN/MAX — a constraint, a flat objective, a composed term and a MIN/MAX
helper of the value rewrite, a nested reducer's among them — goes through one
`EmitExtremumLink`, which makes the choice once for all of them. What is left is ABS,
where the two formulations are genuinely different objects rather than projections of
one description: see [`../06_model_formulation/done.md`](../06_model_formulation/done.md)
§9a and [`../07_solver/done.md`](../07_solver/done.md) §2.

Only the *hard* directions need a choice at all. `MAX(e) <= K`, `MIN(e) >= K`,
`MINIMIZE MAX`, `MAXIMIZE MIN` are exact with a one-sided envelope plus outer pressure:
no Big-M, so nothing to replace.

Both ABS arms read the same stage-05 tag (`abs_aux_idx`, `abs_is_pos_bound`) — stage 05
tags rather than fully lowering, because the Big-M constants are functions of evaluated
data, and that tag *is* the native-construct record. Neither arm decides anything, which
is what keeps the two comparable.

**A member that is already a column is passed through, not copied.** A general
constraint relates columns, so a member expression normally has to be pinned to a fresh
one — but `MAX(x)`, `MIN(x)` and `ABS(x)` name a decision variable directly, and there
`t = x` was a column and an equality row per data row to restate a column that already
existed. Presolve cannot substitute a column a general constraint reads, so the copies
survived into the solve: measured on `MAXIMIZE MAX(x)` at 15K rows, 0.97s with the
copies against 0.29s without, for the same answer. Only an exact renaming qualifies —
one variable term, coefficient 1, no constant — since `MAX(2 * x)` or `ABS(x - target)`
is a genuine expression and still earns a column.

The native columns that remain are **boxed**, not free, wherever a range is derivable —
the same `AuxRange` walk that produces the Big-M constant. A free continuous column is a
measured performance cliff, and it is earned only by the one case that has no range at
all, which is precisely the query the native path exists to answer.

The box is derived **per row**, on both constructs. Each argument column stands for one
row's inner expression, so one row's reach is what bounds it; and because
`AuxRange::CoverRowSided` folds the two ends in separately, a row open on one side keeps
the side it can derive, and a row whose contributors are all bounded gets a real box
even when another row's are not.

### Output

A `SolverInput` carrying `num_rows`, `num_decide_vars`, `variable_types`
(reported as `BOOLEAN` wherever `is_boolean_var` is set), resolved
`lower_bounds` / `upper_bounds`, the `EvaluatedConstraint` vector, objective
coefficient and variable-index arrays, entity mappings, and `sense`.

When the objective is quadratic, the same evaluation runs on `squared_terms` **in
addition to** `terms`, not instead of: `EvaluateObjectiveTermList` is called once
per non-empty list, filling `objective_coefficients` / `objective_variable_indices`
for the linear half and `quadratic_inner_coefficients` /
`quadratic_variable_indices` for the quadratic half. Each term's filter — its own
`when` with the objective's folded in — applies to each list independently.

`CoefficientColumn` keeps this compact: a column that is uniform across rows — a
broadcast Big-M coefficient, a constant RHS, a McCormick constant — stores one
scalar rather than `vector<double>(num_rows, K)`. Reads via `Get(row)` are
branchless on the storage kind, and mutation lazily promotes Scalar → Dense. A
third kind, `SparseMasked`, stores a uniform value at a sorted index set with
every other row implicitly 0, built for the `<>` per-row indicator path where
every active row gets `-M` and every excluded row 0.

---

## 6. PHASE 3 — build and solve

Hands off to stage 06 ([`../06_model_formulation/done.md`](../06_model_formulation/done.md))
and stage 07 ([`../07_solver/done.md`](../07_solver/done.md)). The hand-off happens
early: `solver_input.constraints` is populated as soon as bounds are resolved, and
the linearization passes then work on it in place.

The MIN/MAX objective and the composed MIN/MAX clauses are
emitted by stage 06, not here. What stays is the evaluation they need — a composed
term's per-row coefficients, the query-wide factor on a reducer, its filter mask,
the constant RHS, and the objective's member rows. The evaluated terms travel to stage 06 as
`ComposedMinMaxTermData`; the objective's shape travels as `MinMaxObjectiveSpec`.

### Assembly and formulation are separate passes

PHASE 3 is two methods, split by *what each one reads* rather than by where it sits
in the sequence.

`BuildSolverInput` **assembles**. It moves the evaluated constraints, objective and
links off `gstate`, builds the `VarIndexer`, resolves the variable types and the
column box (the folded bounds, the `lower_bounds` sentinel, the rigid snapshot, the
floors a `when` lowers, then `DecidePropagateImpliedBounds`). The objective's AVG scaling
runs before it, at the end of the evaluation (`ScaleObjectiveAvg`). It runs
exactly once per query, because it is the pass that consumes `gstate`.

`FormulateModel` **formulates**, against a `FormulationBox` handed to it: the auto-`M`
refill for L0 links, the ABS / MIN/MAX / `<>` / bilinear linearizers, the MIN/MAX and
composed-MIN/MAX encodings, and finally `LowerDecideConstructs` for whatever the
chosen backend cannot state natively. It touches no `ClientContext`, scans no data,
and evaluates no expression — it is a pure function of the evaluated clauses plus a
box.

That purity is the point. A Big-M or a McCormick envelope is sound only for the box
it was derived from, and infeasibility diagnosis re-opens that box (see "The elastic
box" above). Because formulation is separable from evaluation, the same pass can be
run a second time, on a copy of `SolverInput` against the widened box, to derive the
constants an elastic model needs — without re-scanning the data to recompute
coefficients that a widened bound cannot change. `FormulateElasticModel` is that second
caller; see "The elastic box" above.

Two boundaries carry the weight:

- **Implied-bound propagation belongs to assembly, not formulation.** It derives
  bounds *from rows*, and in an elastic model those rows are the loosenable ones, so
  re-running it would undo the re-opening. The auto-`M` refill sits on the other side
  of the line for the mirror-image reason: it derives a *constant* from the box, so it
  has to be re-derived whenever the box changes.
- **Columns are settled before formulation.** `VarIndexer::Build` runs in assembly and
  the tail is handed the result; only the global block grows as auxiliaries are
  appended, which a debug assertion checks. A second formulation therefore names
  exactly the same columns as the first, which is what keeps `var_labels` and
  solution readback valid across both.

A slow-solve checkpoint report runs when a solve exceeds its budget; see
[`slow_solves.md`](slow_solves.md).

---

## 7. Readback (`GetData`)

`DecideGlobalSourceState` holds a scan over `gstate.data` and a
`current_row_offset`. `MaxThreads()` returns 1, so the sequential mapping between
data rows and solution values is exact.

The source state also owns a `scan_chunk`, sized to `gstate.data.Types()` via
`InitializeScanChunk` — narrower than the operator's output chunk by exactly
`total_decide_vars`, since DECIDE columns are appended after the buffered input.
`GetData` scans into `scan_chunk`, then `Vector::Reference`s each scanned column
into the output chunk's matching leading column (zero-copy). Scanning directly
into the wider output chunk would violate `ColumnDataCollectionSegment::
ReadChunk`'s `chunk.ColumnCount() == column_ids.size()` contract.

Each call scans the next chunk, fills the DECIDE columns — the last
`total_decide_vars` columns of the output — and advances the offset. The mapping
is:

```
global_row   = current_row_offset + row_in_chunk
solution_idx = gstate.var_indexer.Get(decide_var_idx, global_row)
value        = ilp_solution[solution_idx]
```

A **per-row** decision (`DecideVarScope::ROW`) has its own column on each row. A
**keyed** decision (`per K: x`, `ENTITY`) has one column per class of `K`: every row of
a class resolves to it and receives the same value, which is the point of the key. A
**query-wide** decision (`per (): x`, `SCALAR`) is one column every row resolves to.

The readback indexer is the owning `VarIndexer::Build()` form, so it outlives the
`SolverInput`.

### Type-specific projection

Solvers return doubles, and a MILP may return `0.9999999` for an integer. The
branch is on the variable's **output `LogicalType`**, not on how it was declared:

| Output type | C++ type | Projection | Reached by |
|---|---|---|---|
| `INTEGER` | `int32_t` | `round(value)`, range-checked | `x(BOOL)` and INTEGER auxiliaries |
| `BIGINT` | `int64_t` | `round(value)`, range-checked | `x(INT)` |
| `BOOLEAN` | `bool` | `value >= 0.5` | optimizer-created BOOLEAN auxiliaries |
| `DOUBLE` | `double` | direct, no rounding | `x(REAL)` |
| default | `int64_t` | as BIGINT | — |

A declared `x(BOOL)` therefore comes back as `int32` `0`/`1`, not as SQL `true`/
`false`: `BOOL` is a *domain* carried by `is_boolean_var`, and the variable's
DuckDB-facing type stays `INTEGER` so it can appear in arithmetic. The `BOOLEAN`
row is reached only by auxiliaries the optimizer declares with that type
outright.

**The integer rows are checked, not cast** (`DecideSolutionToInteger`). A solved
value is not bounded by the column's type: the solver works in doubles, and the
bound that actually pins a variable may be a column read at execution or an
aggregate row that bounds no single variable. So no earlier layer can promise the
value fits, and an unchecked cast of an out-of-range double is undefined in C++ —
it saturates on the platforms we build, which would present the type's limit as
the answer and return a row violating the query's own constraints. This is the
one place that can know, so it refuses by name instead.

`x(INT)` is `BIGINT` for the same reason: a 32-bit result column would truncate
optima that are ordinary (`SUM(x) <= 5000000000`), and `BIGINT` is what DuckDB
itself returns for generated integers. The limit that remains is the double's
rather than the column's — past `2^53` a double no longer counts consecutively,
so the solver never distinguished the value and no wider integer type recovers
it. `x(BOOL)` stays `INTEGER`: a `0`/`1` domain cannot reach either limit.

Each output vector is `FLAT_VECTOR`, written through `FlatVector::GetData<T>()`.

### Auxiliary variables

`GetData` emits a column for **all** variables, user and auxiliary. The `Projection`
operator that `plan_decide.cpp` places above `PhysicalDecide` prunes the auxiliary
columns, so only user-declared variables reach the result. Auxiliaries occupy the
last `num_auxiliary_vars` slots, and their columns are constant NULL: no one reads
them, so no value is converted or range-checked (a helper of `sum(x * 1000000000)`
past int32 would otherwise be refused under its internal name).

---

## 7b. `DIAGNOSE`: reporting on the run instead of returning it

Two things change when the statement carried the prefix (`PhysicalDecide::diagnose`,
copied from `LogicalDecide::diagnose`):

1. **The engines are armed.** `FinalizeSolveResult` reads the flag once as
   `diagnosis_armed` and turns on the shared pre-solve work the engines need —
   unbounded-ray extraction, tolerating an inverted column box through Build, and
   retaining the built model for the elastic engine. An unprefixed query pays for none of
   it, which is the whole point of the prefix being the trigger.
2. **A failure stops being an error.** Each failing terminal calls the operator-local
   `report(...)` instead of throwing: it stashes the findings and sets
   `gstate.diagnosis_only`, so `Finalize` returns `READY` with no solution and `GetData`
   emits nothing. `PhysicalDecideDiagnose`, sitting directly above in the same plan,
   swallows those (absent) rows in its `Sink` and emits the findings in its `GetData`.
   Without the prefix every failure terminal still raises, and raises the *state only* —
   naming the clause means running the elastic solve, which is exactly what the user did
   not ask for.

A successful solve clears the stash, so a `DIAGNOSE` over a query that worked finds
nothing and reports one `feasible` finding. The handoff is statement-scoped by
construction: `TakeDecideDiagnostic` consumes it, and nothing outside the statement reads
it.

**What stays raised even under the prefix.** DIAGNOSE explains the outcome of a *solve*.
A query that fails before one happens — a semantic error, a bound that contradicts a
type's own domain (`x <= -1` on a non-negative decision, checked in the sink state's
constructor), a model class the host's solver refuses — still raises, with the precise
message that path already had. There is no solve outcome to report on.

The slow-solve path is deliberately outside all of this: see `slow_solves.md`.

## 8. Source map

| Concern | Location |
|---|---|
| Everything above | `src/execution/operator/decide/physical_decide.cpp` |
| The one pass: the store and the windows its loops read | `src/execution/operator/decide/decide_row_store.cpp`, `src/include/duckdb/execution/operator/decide/decide_row_store.hpp` |
| Operator fields | `src/include/duckdb/execution/operator/decide/physical_decide.hpp` |
| `DecideTerm`, `DecideConstraint`, `DecideObjective` | `src/include/duckdb/planner/decide/decide_prepared_model.hpp` |
| The pass that fills them | `src/optimizer/decide/decide_linear_form.cpp` |
| `SolverInput`, `EvaluatedConstraint`, `CoefficientColumn` | `src/include/duckdb/decidb/formulation/solver_input.hpp` |
| Logical → physical, entity key indices, input column names | `src/execution/physical_plan/plan_decide.cpp` |
| Binding resolution shielding | `src/execution/column_binding_resolver.cpp` |
| The `DIAGNOSE` operator (sink the rows, emit the findings) | `src/execution/operator/decide/physical_decide_diagnose.cpp` |
| `per`, `when` and `by` semantics; the scope test suites | [`../../language/scopes.md`](../../language/scopes.md), [`../../testing/scope_suite.md`](../../testing/scope_suite.md) |
| Its plan-generator case | `src/execution/physical_plan/plan_decide_diagnose.cpp` |
| Slow solves: report, continuation, Ctrl-C | [`slow_solves.md`](slow_solves.md) |
