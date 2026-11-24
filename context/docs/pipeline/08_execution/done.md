# Stage 08 — Physical execution and readback

Runs the relational input, evaluates every prepared coefficient and grouping
against the materialized data, invokes the solver contract, and projects the
solution back onto rows. It implements no rewrites or backend-specific
formulation. The tree records each node's operator structure, while the model
emitter evaluates its compiled terms into solver-neutral row records.

`PhysicalDecide` is a **blocking** operator: the optimal value of any one decision
depends on the whole dataset, so it must consume its entire input before it can
produce a row.

**Key source files**: `src/execution/operator/decide/physical_decide.cpp` (`Finalize`, the assembly, the switch over the
solve's outcome and the readback), `decide_descent.cpp` (top-down), `decide_tree_columns.cpp` (bottom-up),
`decide_model_emitter.cpp` (what stage 06 reads) and `decide_value_extraction.cpp` (the value checks and the blocks the
emitter writes through); at plan time `src/planner/decide/decide_tree.cpp` and `decide_tree_compile.cpp` (the tree and
its programs). Model building, the solve and the diagnoses live with their stages:
`src/decidb/formulation/formulate_model.cpp` (06), `src/decidb/solver/decide_solve.cpp` (07) and
`src/decidb/diagnostics/decide_diagnostic_terminals.cpp`.
**Header**: `src/include/duckdb/execution/operator/decide/physical_decide.hpp`

`Finalize` itself is a short orchestrator; it calls, in order:

| Function | Phase |
|---|---|
| `EvaluateTree` | 1–2 — the evaluation (§1a): the tree's descent over the buffered rows (`DecideDescent`), each node's programs over its entries (`DecideTreeColumns`), and the model emitter (`DecideModelEmitter`), which writes the key scopes' classes, the clauses, the objective and the helper decisions' members as stage 06 reads them |
| `BuildSolverInput` | 3a — adds the plan's facts and the declared column box to `SolverInput`, builds the `VarIndexer`, then calls stage 06's once-per-query step (`PrepareModel`), which settles the box |
| `FormulateModel` (`formulate_model.cpp`) | 3b — derives the constants and emits the rows, against a given box |
| `FinalizeSolveResult` | 3 — calls `SolveDecide` (`decide_solve.cpp`: the solve, and at a time limit the prompt to continue or the incumbent), switches on the terminal (`SOLVED`/`UNBOUNDED`/`INFEASIBLE`/`UNDIAGNOSED`), and hands the diagnosis off or finishes `gstate` |
| `UnboundedTerminal`, `InfeasibleTerminal` (`decide_diagnostic_terminals.cpp`) | under `DIAGNOSE` only — the engines a failure runs; the infeasible one formulates the repair model, 3b again against the widened box (`FormulateElasticModel`) |

`EvaluateTree` writes what the model reads of the data into the `SolverInput` itself: the model emitter holds it and
fills its clauses, links, mappings and objective, so the sink state mirrors none of them and `BuildSolverInput` adds
the plan's facts and the declared box, which `PrepareModel` settles. `FormulateModel` reads `SolverInput`, the box and the column space and nothing else: the
declared names and the native-construct policy are on `SolverInput` (`variable_names`, `use_native_constructs`), and
a row whose `M` the box gives names its indicator (`EvaluatedConstraint::sized_indicator`). Nothing in it depends on a variable bound, so the
formulation pass runs twice against two boxes from one input.

The operator holds the child plan, the tree, the run's settings (`debug`, `solver_backend_name`, the native-construct
policy) and `display`, a `DecideDisplay`: what `EXPLAIN` and the messages print (the clauses and the objective as
bound, the source text, `constraint_sources`, the auxiliary decisions' expressions, the plan-time rewrite accounts)
and, under `DIAGNOSE`, the input column names. `run_time_accounts` adds what a run counts for `EXPLAIN ANALYZE`.
Every fact execution reads (the decisions, keys, frames, signatures, singleton-row boxes, domains, the `diagnose`
flag) is the tree's: `plan_decide.cpp` moves it there from `LogicalDecide`, so the operator holds no second copy.

The blocks the emitter writes through are shared functions (`decide_value_extraction.hpp`), one copy of each rule and
its message: grouping a clause's kept rows by its shape (`GroupRowsByShape`), skipping a group whose reducer reads no
row, counting a term or a product outside the reducers once (`CountTupleTermsOnce`, `CountProductsOnce`), the
AVG-denominator division (`ScaleAvgRows`, `ScaleAvgTerms`) and the tightest frame bound. `EvaluatedProductGroup`
(`solver_input.hpp`) is the one struct for a term of degree 2; the constraint and the objective share it.

---

## 1. Order of operations

| # | Where | What |
|---|---|---|
| 1 | `GetGlobalSinkState` → `DecideGlobalSinkState` ctor | Read the folded variable box and the tree's lowered clauses |
| 2 | `Sink` | Materialize every surviving tuple into the thread's `ColumnDataCollection` |
| 2a | `Combine` | Place the thread's rows after those placed before |
| 3 | `Finalize` → `EvaluateTree` → `DecideDescent` | Read the buffer top-down, a chunk at a time: each node's `when` parts, keys and σ fields on the rows that reach it, into the containers (§1a); then settle them (`DecideDescent::Finish`: ranks, timelines) |
| 4 | `EvaluateTree` → `DecideTreeColumns` | Bottom-up: the frames' timelines and columns (§4b), the data checks, each node's programs over its entries, the data reducers' folds |
| 5 | `EvaluateTree` → `DecideModelEmitter` | The key scopes' classes (§4), the objective produced once (§4a), then the clauses, the objective and the helper decisions' members (§5) |
| 6 | `Finalize` → `BuildSolverInput` → `FormulateModel` | Assemble `SolverInput` and settle the box, then linearize against it (stage 06) |
| 7 | `Finalize` → `FinalizeSolveResult` → `SolveDecide` | `SolverModel::Build` → `SolveModel` (stage 07) |
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
| Top-down construction: one pass over the rows (pp. 83-91, 100-105) | `DecideDescent`, in `Finalize`, the buffered rows a chunk and a window at a time, by one descent |
| A node's container (p. 110) | `DecideNodeContainer`: its entries as columns, its slots found in a `DecideKeyMap` |
| Bottom-up evaluation (pp. 93-97, 106-108) | `DecideTreeColumns`: each node's programs over its entries, children before parents; then `DecideModelEmitter` writes what stage 06 reads |

**An error is raised where a read reaches it**, as in SQL: the first error the evaluation reaches stops the query, and
no order among errors is owed (R10, R11, `context/opa_rewrite/decisions.md`). A `when` decides which rows a node reads,
so a read of a row the `when` drops raises nothing (`when t.w > 0: x <= ln(t.w)` where `w` is 0, `language/guide.md`
§6.3), and neither does a value no parent reads. Nothing else is deferred: the evaluation keeps no ledger and ranks no
error. A vector that fails is read again entry by entry only to find the failing entry's own inputs (`Numbers`), so the
message names the value where it stands, and then raises at once. The reads of the descent and the programs of the tree
raise in the order the evaluator reaches them, and a parallel `Sink` can reach a different failing row on another run.
Every message states its kind and the place in the query it is about: a raise call carries its own position
(`Exception::InitializeExtraInfo`), so DuckDB prints the caret ([`language/errors.md`](../../language/errors.md)). An
interrupt, an out-of-memory error and a fatal error stop the evaluation where they arise.

A raise call carries its own `query_location`, as DuckDB's casts do. A direct rule raises where the value is found: the
too-large message is a raise call at the clause, a check is a raising filter, and no message is built as a column or
taken as a minimum.

A row in no class of its `per` key raises nothing anywhere below the `per`, a reducer, a frame or a helper included
(R11, D81). The objective's G has no exception of its own. A node that holds rows in no class
(`DecideNode::holds_rows_in_no_class`, §5.3, D75) keeps holding them.

Each row is read once. `Sink` buffers each thread's rows, `Combine` places each thread's buffer after those placed
before (`ColumnDataCollection::Combine`), and `EvaluateTree` routes the whole buffer through one descent, a chunk at a
time, each chunk's rows numbered by the rows before it, as a hash join builds its table once its build side is complete
(`execution.descent_rows`, `DECIDB_PROFILE`, counts the rows read). The evaluation fetches its first chunk alone, for a
value the whole query shares, and never scans the buffer again. A descent per thread, merged in `Finalize`, would route
on several threads at the cost of a merge of several hundred lines, so the rows are routed once. Against that merge,
over a stored table so the scan runs on several threads (`context/opa_rewrite/tools/ab_bench.py`), the two lightest
workloads spend more outside the solver: `flat-sum` 23 ms at 1 million rows (12%) and 53 ms at 2 million (14%, 3% of the
whole query), `variants-10` 35 ms at 1 million (14%); the others move by at most 6%, and peak memory falls by 3 to 8%.
The unbounded diagnosis's column groupings (§7b) explain the solved model after the evaluation, and read the named input
columns in a scan of their own.

---

### The tree evaluator

**The evaluator.** Every query is evaluated by the tree and formulated through stage 06:

- *The tree* (`decide_tree.hpp`, `DecideTreeConstruction`): built at plan time, with stage 05's lowering between its
  nodes and its programs. Its nodes come from the clause list, the link and record tables, frames, and function
  metadata. It keeps the deck's E (`SCOPE`), G (`REDUCER`) and O (`FRAME`) nodes and adds F (`FUNCTION`) for values
  whose structural rows need their own execution scope. An F represents ABS, a Boolean product, an L0 indicator, or a
  nonlinear reducer-body definition. The emitter reads its instances from the top-down tree and attaches its
  structural rows beneath it; the stage-06 linearizers consume the evaluated rows and links. An E's σ
  is the binder's signature of its clause; a generated NULL test (a `BoundDecideNullTestExpression`, which the tree
  splits from the written conditions into `DecideNode::member_tests`) is
  a member test that carries its columns and adds no σ field. A root in the grouped shape (the aggregate shape, an
  easy extremum) with no `per` is a class of its `by` key (`language/guide.md` §6.4). A root's text is
  its clause as written. The same walk hands each clause, as its root is made, to stage 05's term half, which lowers it
  into that root, the objective into its root and each helper's terms into its G or O (`DecideTermLowering`, one IR:
  the records stage 08 reads are the ones it writes), naming on each reducer's terms the G they range over
  (`DecideTerm::child`, read from the walk's map of the clause in hand);
  the solver gate reads the model class off them; then `decide_tree_compile.cpp` numbers the clause roots, finds the
  node each term is computed on, and compiles each coefficient, bound, admitted end and member test into a program over
  that node's layout, by the rules of `ToChunkReferences`: its carried columns, then the program columns its programs
  add (which the descent carries on each entry), then an admitted end's inputs a and r. A root is
  numbered in the order of the stage-05 list its body comes from (a folded bound's singleton row follows every
  constraint, and is read under DIAGNOSE alone). A read of a data reducer
  is a hole of its reader's layout (`DecideHole`), and so is a read of a frame, its O's column at the reader's row
  (`DecideHole::column`): a point's value or a range's over data where Lower read the O itself, the column stage 05
  read otherwise, and a reducer's over frames its G's fold. What a frame's columns read, its payload, its `else` and a
  range's argument, are programs on its O (`DecideFrameColumnPrograms`), in stage 05's terms. A child a data
  expression reads that has no data value is an internal error.
- *Top-down* (`decide_descent.hpp`): one descent reads the buffered rows in `EvaluateTree`, a chunk at a time, and
  numbers each row by its place in the buffer; the buffer is never scanned again to evaluate a row
  (`execution.descent_rows` the rows read). A set E is an E per K or per () alone (`IsSetNode`): every other node, an F
  among them, keeps an entry per row it holds, each an instance of its own. An F reached along several paths within
  another F's slots (an L0 indicator over an ABS, each per class) numbers its slots path by path within each chunk, so
  its numbering follows how the rows are cut, at chunk size 1 or 2,048 alike; the model does not.
  `test_descent_merge.py` runs eight shapes on real threads (a Parquet file of five row groups of 2,048 rows, the
  writer's smallest, read on four threads), whose rows reach the buffer in an order that changes from run to run, and
  asserts that every run's answer reaches its solver model's optimum. The descent is a Composite traversal vectorized by
  selection, as
  DuckDB's filters hand a selection vector on: a window of the input at a time, a node's `when` parts, keys and σ fields
  execute only on the rows that reach it (`Reach`: the rows, and the enclosing slot each arrives in), and each key's
  rows find their slots in a `DecideKeyMap` (`decide_key_map.hpp`). That is DuckDB's GROUP BY hash table
  (`GroupedAggregateHashTable`) with no aggregate and one register per group, which holds the group's slot, probed a
  vector at a time: NULL is equal to NULL, and a collated column compares under its collation. The nodes of one key
  outside an enclosing slot share its dictionary (a Flyweight, `SharedDictionary`), each numbering its own slots in the
  order its rows reach them. A key is evaluated once on a row of a window, by the first node that reads it there, and
  every later read slices those values (`KeyReads`); a row a read raised on is not held, so each node reading it raises
  at its own rank. A decision's `per` key and a reducer's `by` key alike (`per g: x` with `sum(x) by (g)`) are one
  key (`FindOrCreateDecideKey`), read once per row and hashed twice: the decision's classes in their layout's map
  (`FindDecisionGroups`), the reducer's slots in the key's dictionary (`FindGroups`). No test counts either.
  `DecideTree::Verify` checks at plan time, in a debug build, that the descent reads of a row only its nodes' own `when`
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
  (`DecideNodeGroups::spellings`), which names a variant's class. A set E keeps one entry per distinct slot, σ,
  carried columns and reads, found by one probe of its entries' `DecideKeyMap`, at least one per variant; its instances
  are its distinct (slot, σ). A clause root whose classes agree on everything it reads (`DecideNode::merges_classes`,
  from `DecideCanonicalizer::MergesClasses`) leaves the slot out of the instance key and keeps it in the entry
  (`InsertDistinct`): its instances are its distinct σ over every class, and a merged instance is named by its
  first class. It records each row it keeps with the entry standing for it
  (`DecideNodeContainer::routed`): a pair per kept row, which the objective's root, a grouped or class root and a
  relation-qualified reducer's operand read. A grouped root under `per K:` holds a kept row in no class of K too, in a
  slot of its own (`DecideNodeGroups::in_a_class`), since the row still lies in a group whose row reads it. So does an O
  an anchor whose `within` column is NULL: it lies in no timeline (D75), and its slot is no timeline's. So does a
  relation-qualified reducer's operand hold a row its `when` keeps in no class of its `per`: the reducer's terms are
  computed there and its qualifier keeps no value of the row, which still lies in its group (`language/guide.md` §5.3).
  A row a node's key leaves in no class (a NULL in a written column, `language/guide.md` §5.2) is read by nothing
  below the key: a G, a frame and a helper of it hold no slot for it, so an error in its values raises nothing (D81). A group's rank is its group number: NULL sorts after every value, as `ORDER BY` places it.
- *Arms*: the model emitter has an arm for every shape stage 06 accepts, keyed on what it reads of the clauses and the
  query: a clause with a model row per kept row, or per variant of a class of its `per K:`, the folded bounds' rows a
  repair model emits, the declared decisions' classes and a relation-qualified reducer's, the objective's linear terms,
  a clause in the aggregate shape, a `<>` clause, a row defining an auxiliary, the classes a diagnosis reports a row
  stands for, an easy MIN or MAX, a clause's products and squares, the objective's squares and products, a product the
  optimizer makes an auxiliary of (a McCormick link, an AND of two BOOLEANs), a helper decision of a reducer the value
  rewrite lifted, a frame's reads and a range over decisions' helper, and a tightest frame bound. Every query runs
  `EvaluateTree` and then the assembly, formulation and solve (`BuildSolverInput`, `FormulateModel`,
  `FinalizeSolveResult`).
- *Bottom-up* (`decide_tree_columns.hpp`): partial evaluation, as DuckDB's executor runs a projection, with no
  expression interpreted per entry. Each node's programs run over its entries, one executor per window of 2,048 entries,
  with a per-row error capture (`DecideEvaluateWindow`), and are read through `ExtractDoubleColumn`, which raises at the first value that is not finite; the failing entry is found by reading its window entry by entry
  (`Numbers`), so a coefficient computed from inputs names the expression where it stands, as the query writes it. A node's member tests decide which of
  its entries are read: an entry whose test is false is no member (a test is `X IS NOT NULL`, NULL only where it
  raised, and such an entry is no member either), its other programs are read on the members alone, so a value or an
  error elsewhere is never read. An objective term is read only on the entries that read an instance of its decision as
  well (`ReadEntries`, D84). A G over data, a data reducer, folds its operand's members, each variant of a class once
  at its first row and nothing of a row in no class (`FirstOfEachInstance`), with DuckDB's aggregate functions over a
  dense array of states, one per slot, as `PerfectAggregateHashTable` lays out its groups (`DecideAggregateStates`); a
  norm folds its desugared reducer's (a sum of squares, of absolute values); a NaN among its values, or in a norm's
  body, is the data check's to refuse, which runs before any clause is read. A reducer over frames, like a range frame's
  window, that reads a NaN has the value NaN (`DecideNanValue`), as SQL's SUM does, whatever its aggregate, but a
  count counts it; the value's reader refuses it with the ordinary message, and a norm, whose value can hide a NaN,
  through the check on its body; it folds once every frame's columns are, before any keyed mapping, in its column's order. A node reads a data reducer's value as a hole of its layout, after its carried and program columns:
  the fold in the slot its
  row lies in, NULL where the row reaches none. A data check's message is a program on its hidden root, raised before any clause is read (`CheckData`); a window of it that raises raises the error its whole evaluation
  meets first, as DuckDB's vectorized evaluation of a chunk does.
  A frame is read once the descent is finished, before any program that reads it (`ReadFrames`): its map from its O's
  timelines and positions (`FrameMap`), each anchor's position or window selected by `SelectDecideFramePeers`, then
  what its windows skip as NULL and its columns, by `SkipDecideFrameNullReads` and `ComputeDecideFrameColumns`, each
  expression they read a program on the O over its anchors (`DecideFrameRead`). A column holds a value per row with the
  error its row carries, which a program reading it there raises (`CarryFrameErrors`), so an error no reader reaches is
  never raised. An O's programs that read its own frame's columns, a range over decisions' `else`
  terms, are computed after them (`FramePrograms`). A frame refuses in this order: its range's limit after
  the reads of its rows, then its partition (D75), frame after frame; then a position of two payloads it reads (D73,
  the descent's `DecideDescentPosition::conflict`), named by its timeline's and its order key's least spellings (D69).
  Each message points at the frame (`DecideFrameSpec::location`). Its maps (`DecideGlobalSinkState::frame_maps`) give the row a term read through the frame names its
  instance at. A node is computed when it is first read, a hole's G before the node that reads it, rather than in an
  `eval_order` walk. The descent's reads that route the rows (a `when`, a key that is no grouped clause's `by` key)
  raise as they read. A program raises where its entry is read, never for an entry no member reads. A definition's
  programs are read where the auxiliary's absence is resolved (`DecideModelEmitter::ReadAbsence`), auxiliary by
  auxiliary in index order, the bound and then the coefficients together, a window at a time (`NullEntries`), and
  again where its clause is evaluated among the clauses, which checks their numbers. A reducer's filter is read before
  its terms: the reducer's own `when`, which the descent reads, then its member tests. A value error is found by
  reading its window entry by entry.
- *Formulation* (`decide_model_emitter.hpp`): the model emitter, an Adapter onto stage 06, writes the model's evaluated
  clauses and the keyed mappings into the `SolverInput` through the shared blocks of
  `decide_value_extraction.hpp` (`InitializeEvaluatedConstraint`, `DescribeBound`, `DecideAbsence::SkipNullInstances`,
  `CountInstances`), one code path for each rule and its message. A clause in the aggregate shape keeps the rows its
  root holds, groups them by the slots of its G of the `by` key, numbered by value, and emits, skips and counts them
  through `GroupRowsByShape`, `DecideAbsence::SkipEmptyReducers`, `CountTupleTermsOncePerGroup` and `ScaleAvgTerms`: a
  reducer's term is read on the members of its G, a kept row in no group among them, where a NULL or a decision with no
  instance leaves its group no value, and a term beside the reducers on every kept row. Its groups are named by their
  least spelling (`DecideGroupKeyValues`). A keyed decision with no class at a row, or a NULL coefficient or bound,
  skips its instance, and the skipped instances are reported unless a
  clause or the objective raised. An objective term in a reducer is read on the members of the G it ranges over, its
  operand's entries its member tests keep, so a reducer that keeps none drops its terms (D84); a term outside every
  reducer is read on each row the objective keeps and counted once (`CountTupleTermsOnce`). Either is dropped where its
  decision has no instance (D84). A relation-qualified reducer reads the first entry of each instance of its operand,
  its first row, however many spellings split the class into entries, so its columns hold each instance once, and AVG
  then divides them (`DecideModelEmitter::Objective`; by the G's slots for an AVG read in place, D81). An objective with a second
  variant over its kept rows raises the message `DecideObjectiveTwiceMessage` builds (D72), reading the signature's
  fields from its root's entries (`ObjectiveDifference`), and a frame field's reason with `DecideFrameDifference`: two
  positions of one timeline, two timelines (which D75 refuses first), or two values its `else` reads, a NULL one value
  (the tree compares the reads as `Identity` does). A clause in the per-row shape under `per K:` emits
  one model row per variant of each class of K among its kept rows, its root's instances, numbered by value, its class's
  and then its fields', or by first row where a field reads a decision's instance or a frame's keys, and placed by
  `DecideOneRowPerVariant`: on its first row, or the first
  with a value of the auxiliary the clause defines (D88). It is named by its class, or, where its class holds more than
  one variant, by `DecideNameVariants`: by what its class key and its fields read at its first
  row, as written (`DecideBody::names`, programs on its root), and a collated column of its class by its least spelling
  among the clause's rows of the class; a class of one variant is named by its key's least spelling over every read of
  the key (D69, `DecideDescent::Label`). The variants are counted for EXPLAIN ANALYZE. Under
  DIAGNOSE the classes each model row stands for come from `DecideGroupClassSets` and `DecideNameVariantClassSets`, each
  kept row's class of K read from its root's slots; so a grouped root under `per K:` stays keyed by K, its instances a
  class of K and a group of the `by` key each, though the clause's model rows are its G's groups. A relation-qualified
  reducer whose classes its body's values split (`KeyedScopeInfo::signature`) takes the instances of its operand
  (`DecideTree::qualified_operands`) for its classes, numbered as a signature's variants are, a row it does not
  read in a class of its own. A row its `when` keeps in no class of its key stays in its group: nothing below the
  key reads it, so an error in its values raises nothing, and the reducer reads no value of it. An auxiliary has no value where its
  definition reads a NULL unreduced or a decision with no instance (`absent_reads`, D88): the definitions are read in
  `eval_order`, which puts what defines a decision before what reads it, and a row reading an auxiliary with no value
  skips its instance, as the objective drops its term there. An AVG under `<>`
  the plan keeps as sums (`DecideConstraint::avg_scales_bound`) multiplies its bound by its group's count. An easy MIN or MAX, which its direction
  drives (`DecideConstraint::was_minmax_easy`), is a clause in the grouped shape: a model row per member of its G,
  grouped by its `by` key, whose terms are read as written on the member's own row, not counted once in its group. Its
  bound on one decision is admitted on each member (`DecideModelEmitter::Admit`), an entry of its term's site, the
  reducer's operand, whose filter the descent routes, so no program tests it; a row no member stands
  on emits no model row and admits every value. An admitted end is never NULL, since a
  NULL coefficient or bound admits every value (`DecideReadSingletonRow`): no row's end is marked NULL, since no query
  reaches one, and a NULL end is an internal error. A MIN or MAX over a relation's classes reads every row an entry of
  its operand stands for as a member: stage 05 gives its terms no qualifier, since a class's repeats carry its value
  (`ReadRows`). A qualified term's operand is the one its scope's keyed mapping reads, as a written reducer's and a
  helper's are (`DecideTreeBuilder::Qualified`), so a scope whose classes its values split (D70) has them. A clause's
  products and squares are read once its rows are grouped (`DecideModelEmitter::Polynomial`), each term on the rows its
  site's members stand for, a relation-qualified one's first of each class, the repeats left out; their filters decide
  the rows a group emits and the groups a reducer over no row leaves no instance, as a term's do. A product's
  coefficient of 1, which stage 05 leaves out, reads its sign. A NULL coefficient, or a factor
  with no instance at its row, skips the row's instance. A square's weight is a program on its site, read on each row
  its reducer reads (`DecideModelEmitter::Polynomial`), as a product's weight is. The objective's squared
  terms are read as its terms are; its products on the rows they are read on, with no row dropped for a decision with
  no instance, and one outside every reducer is counted once on the first row the objective keeps
  (`CountProductsOnce`). A product's auxiliary has no value where a factor has none (`DecideAbsence::At`), which the
  definitions reading it see and stage 06 reads (`absent_reads`). A row defining an auxiliary reads its products'
  factors and its squares' terms as it reads its terms' decisions, and its squares' coefficients as its terms', but no
  product's coefficient; no part of a definition lies in a reducer,
  which would make the definition a value link. A MIN's or MAX's helper row per row of a product or a square reads the
  reducer's body on the reducer's members: their NULL tests are its root's member tests, and the row stands, and its
  NULLs and errors are read, only on an entry they keep (`Rows`, `NullEntries`), under the row's `when`.
  A helper decision of a reducer the value rewrite lifted lists its members per group of its `by` key that holds
  one (`DecideModelEmitter::ValueLinks`): a
  member is an entry of its G's operand that its reducer's `when` routed and its NULL tests keep, where every decision
  it reads has an instance, a relation-qualified reducer's first of each class, and an AVG's coefficients are divided by
  its group's members (`ScaleAvgRows`). A row in no group of the key is computed on but lists no
  member. The groups that hold a member, numbered by rank among them, are the helper's classes, which every row of a
  group the clause keeps reads (`DecideModelEmitter::HelperRows`); a row of a group with
  none reads no helper, so what reads it there skips its instance. **A helper's class at a row** is held for the rows its
  node holds an entry for (`DecideScopeRows`): listed by row while fewer than one row in eight reads it, a class per
  row from there on, as a sparse vector turns dense. So a helper costs what reads it, and forty helpers that each
  read two thousand of two million rows cost what forty plain clauses do. The list is sorted once, since a node read
  from several places holds an entry per reader for a row. The definitions and the helpers' classes are read in
  the tree's order, which puts what defines a decision before what reads it, a helper no clause reads but through a
  product's auxiliary included (its root of its own). A member's message names the clause that first read the helper, as
  written (its definition's `origin`). A helper's rows are read where its classes are built, before any clause: the rows its
  path keeps (the reading clause's `when`), its reducer's `when` and its members' NULL tests, then its `by` key on every
  row the path keeps. They are read there, in the order of the keyed mappings: every scope in turn, a helper's
  after the definitions of the auxiliaries its members read, then every auxiliary's definitions. The declared scopes
  come before the rewrites' helpers and the rewrites number a nested helper or auxiliary before its reader. A `when`
  the helper's G inherits from a node above it is read there (a frame's O
  inherits one the same way, where the frame is built). A sigma field naming a reducer's `by` key reads it as a
  `by` key's read, which keeps the row, so the node grouping by the key raises what it raises: the G, after its
  members' rows. Each helper's members are read once, with its classes, and listed from there;
  their coefficients are read after the objective. A MIN or MAX whose members read no per-row decision
  lists equal members once (`ValueLinks`): eight distinct group values are eight link rows, whatever the input's
  length. Stage 06 then writes a helper where the plan's form says (D89). A frame's term reads its decision at the row the
  frame selects, and has no value where the frame reads nothing or the decision has no instance there
  (`DecideAbsence::TermAt`, `DecideFrameReadsAbsent`), wherever a term's absence is read: a clause's
  rows, the objective's entries, a definition's absence, a helper's members. A range over decisions' helper has a class
  per window (`DecideFrameWindowRows`) and lists its members per window through `DecideListFrameMembers`, its
  payload's and its `else`'s coefficients programs on its O, read on the rows its
  windows read and the anchors that reach the `else`, after every clause. A signature's frame field
  splits the variants by the frame's keys, the anchor's position as its order key compares when it reads relative to
  the anchor, else its timeline, and by what its `else` reads where the anchor reaches it (`Identity`, `Fallback`); an
  anchor where the frame reads nothing is no variant, though its class still counts, as the classes are counted over
  every kept row (so two variants of one class beside a class whose anchors read nothing are named by their class
  alone, both alike); the variants number by first row, and each is named by the frame's
  keys, a collated position by its least spelling (`DecideBody::position_names`). A grouped clause's tightest
  frame bound (D85) bounds nothing where it is NULL or its row lies in no class of the clause's `per K:`, and each
  group takes its tightest value through `GroupBound` and `TakeTightestFrameBound`, whose `=` message names the `by` group and points at the bound
  (R15). A NULL coefficient of a reducer's member or a term beside the reducers, through a
  value the whole query shares, leaves its group no value (`ReadInReducer`, `ReadBesideReducers`). No rule leaves a row
  out of a MIN or MAX helper's group because all its terms' filters drop it: its terms share one reducer's members, so
  no row the tree emits is one. A reducer's member reading an
  auxiliary with no value is a helper's member in every probe, which the helper leaves out.
  Objective terms check an auxiliary's value on each row an E entry stands
  for, so absence on the entry's first row does not mask a value on another row (`ReadEntries`). Their coefficient
  programs are read on the term site's rows before a relation-qualified reducer selects its classes; an invalid
  coefficient on a row in no class raises nothing (`ObjectivePart`, `ReadInReducer`).

  **A part is read by its place.** A term's coefficient, a product group's weight and each of its forms'
  coefficients are parts (`DecideModelEmitter::Part`), and one reader serves all of them in each place a part can
  stand, each place with its rule for a row where the part has no value (D84):

  | Place | Read on | A row where the part has no value | Reader |
  |---|---|---|---|
  | beside a clause's reducers | each kept row | leaves the row's instance no value | `ReadBesideReducers` |
  | in a clause's reducer | each member | is no member | `ReadInReducer` |
  | in the objective | each row it keeps, or each member | drops the part there | `ObjectivePart` |

  A product group is read where its weight and every form term are, so the objective's is dropped on a row where
  one form's decision has no value. In a reducer the rule seldom has a row to act on: the reducer's member tests
  already leave out a row where a decision of its body has no instance or a frame selects nothing, and a product
  with a nested reducer is refused. A per-row clause's terms (`Rows`), a helper's members (`ValueLinks`) and a
  frame's (`FrameMembers`) stand on their node's own entries and read their programs there
  (`DecideTreeColumns::Numbers`), with no row of another node to join. A variant whose first row D88
  moves to a later row of its entry is named by the entry's first row; no query is known to reach it.

The tree folds the data reducers in the bottom-up pass and plans nothing for them. A data reducer's arguments are
read only on the rows its clause keeps, and the clause's `when` at the clause's rank. A record no clause reads, an
orphan's hidden root, is folded by no reader, so its errors are not raised (open: no query is known to make one).

**Departure from the deck: F nodes.** The deck draws E, G and O; this implementation adds F so structural function
values and their rows follow the same instance scopes as the rest of the tree. Helper columns are allocated at plan time
because stage 06 addresses decisions by their plan-time indices. ABS envelopes, Boolean-product links and L0 indicator
links lie under F nodes. The value rewrite (`decide_rewrite_scoped.cpp`) retains nested G readers for a direct,
unfiltered outer SUM whose clause has the same key, and whose bound is one value for the whole query, linear bodies
included: kept, the sum makes the clause one row per group, and a bound that reads the rows needs a row for each value
it takes, which the value-link path writes; compatible outer SUM clauses also retain nested G readers when their own
rows need quadratic terms. Filtered or deeper linear nests keep the value-link path so their bounds and scope semantics
remain available. A nonlinear reducer body stays at its written site as a kept call that names its definition
(`BoundFunctionExpression::definition`); tree lowering wires that read to the definition's F value, and term splitting uses the reserved helper column where
optimizer-owned MIN/MAX forms need it. Tree construction owns the definition E row. Its early preparation reserves
plan-time indices and registers F-node specs before later optimizer consumers need them. The tree gives the helper
definitions their F nodes in one loop over `LogicalDecide::helper_definitions`, in the order they were made and before
any body is lowered (`RegisterHelperDefinitions`; declare, then define): an ABS, a Boolean product, an L0 indicator
and a definition each get one, found by the entry's index (`definition_nodes`). A `PRODUCT` is read where it is kept,
as its expansion, and a `REDUCER_VALUE` gets its G or O where it is first read (`Link`). The rows the definitions own
(an L0 indicator's links, an ABS's envelope rows) are rooted after the written clauses in the same order. An F node
holds those roots in one list (`DecideNode::rows`), and such a root's `parent` is its F node, set before the row is
lowered so that the row reads its helper as a decision. The finished tree holds the definitions
(`DecideTree::definitions`, moved from the plan as the frames are), and an F node or a linked G or O names its own by
index (`DecideNode::definition`): the helper's kind, decision, label, origin, a product's factors and the way an
extremum's readers push it are read there (`DecideTree::Definition`, `IsFunction`, `OwnedBy`, `ValueVar`), with no copy
on the node.
**What the emitter reads.** A node's body is its compiled term records (`DecideBody`, `DecideTerm`,
`DecideTermSite`); the tree holds no second form of a clause. The E emitter takes a clause's comparison from the
clause, its right-hand side from the body's RHS program, and each term's variable and coefficient program from the
term and its site. Evaluation gets term order, scales, scope, source identity and labels from the E/G nodes, and stage
06 gets source and removal groups from the evaluated E clause. The G/O emitter walks nodes in children-first order and
builds stage 06's member formulas from their term records, node-owned helper direction, qualification scope and labels.
Multi-member MIN/MAX results keep value columns, while SUM/AVG formulas can expand into their parent expressions; a
singleton MIN/MAX group remains a direct member read, as on deck p. 112. F output links and Boolean-product factors are
the F node's own fields (`value_var`, `product_bool_var`, `product_other_var`). The numeric model path lowers formulas
into solver rows.

**Departure from the deck: formulation is stage 06's.** The deck formulates node by
node (pp. 111-119); here the emitter hands stage 06 one record per row and per value link, and stage 06
(`LinearizeReducerValueLinks`, `EmitExtremumLink`) writes the definition rows. The form of each helper is the plan's
(`DecideHelperForm`, stage 05), so no node decides one; a helper the plan writes where it is read is written there by
the emitter. The models are the deck's: SUM inside SUM is the three rows of p. 119 over the six declared columns,
MAX inside SUM the three rows and four envelope rows of p. 115 (`test_anr_nested.py` pins both), the frame the four
rows of p. 108.

A term that reads a generated decision value names that value's plan-time helper column, and the tree's `reads`
edges name the G, O or F node that produces it. The nested MAX-inside-SUM example therefore records the E → SUM G →
MAX G path; an F node's own definition rows are read by the F and do not read it back. Each value link has its own
helper column, so the stage 06 inliner resolves a nested G reference by that column. Frame helper expressions and
NULL-read skipping select linked O nodes from tree metadata rather than optimizer reducer-link records, so the frame
reader gets its producer and compiled programs from the same node graph. The runtime EXPLAIN report for inlined
helpers reads each G/O node's helper variable and label; the physical operator carries no copy of optimizer link
records. ABS F nodes likewise provide the auxiliary and indicator for the stage-06 exactness link; the physical
operator does not retain the plan-time ABS link list.

**Cost.** The descent reads its keys and entries a vector at a time, not one `Value` at a time, and the evaluation's
time and memory grow linearly in the rows (`benchmark/decide/anr_scaling.py`).

Debugging the tree: the `decide_debug` setting, a comma list (`DecideDebug`, `decide_debug.cpp`). It is read once, at
physical planning, so a prepared statement keeps the value set when it is planned; an item it does not know is refused when
the setting is set. `tree`, `containers`, `bodies` and `values` print the tree, the containers, the bodies and their
programs, and the columns' values, each node's programs the evaluation computed on its entries
(`DecideTreeColumns::Dump`), to stderr. The containers dump gives each node its rows in and out and a `reach` line:
whether it is a root, its children, and the rows each `when` part is read on (the first part every row reaching the
node, each later one the rows the parts before it kept). Before the first row, `DecideDescent::RequireRowReads` checks
L2: every expression the descent reads (`when` parts, carried and program columns, signature values, a frame's order,
each key's elements) reads only the row, no aggregate, window or subquery and no column past the input's. `steps` prints
the containers after each piece the descent reads (`DecideDescent::Step`: per node its slots under their enclosing
slot, with the rows each holds), so with `chunk=1` a step per row is the deck's animation. `chunk=N` sets the descent's
chunk size, 1 to 2048. `inject=N:data:V` replaces node `N`'s data value, a G over data's fold, with `V` in the columns,
so a test can show a parent reads the bottom-up values; it refuses a node that computes no data value.
`test_one_pass_tree.py` pins the deck's p. 81 tree (F1_MAX's) and checks its p. 91 containers;
`test_every_dump_on_leaves_the_answer_as_it_was` (`test_one_pass_execution.py`) runs a shape per printer branch with
every dump on and compares the answer with the answer with none. No test reads a dump for a node's kind (a Boolean
product, an L0 indicator or a definition as an F node, ABS reading a reducer's function): their answers have oracle
tests. Profile counter (`DECIDB_PROFILE`): `execution.descent_rows`, the rows the descent read.

Finalize releases the descent once `EvaluateTree` returns: what the solve and the output read of the evaluation is
written into the `SolverInput` (a key's group values into `KeyValues::values_of`) and the global state's frame maps,
so the node containers do not live through the solve.

## 2. The folded variable box

Whether a simple bound (`x <= 10`) becomes a column box or a model row is decided
at stage 05 — see [`../05_optimizer/done.md`](../05_optimizer/done.md), "Singleton-row
folding". This stage consumes that decision and does not re-derive it.

The tree carries `singleton_row_boxes` and `user_singleton_row_bounds`, copied from `LogicalDecide` when the tree is
built (`decide_tree.cpp`). Three consumers read them:

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

Flattening skips any clause marked `folded_bound` (`BoundDecideClause`), so a
singleton-row bound does not also produce `num_rows` redundant per-row model rows. The
clause itself stays in the list, which is why `EXPLAIN` still renders the bound
the user wrote. A recorded one is lowered into its
root too, a folded one (`DecideNode::folded`): under DIAGNOSE the model emitter
writes it after every other lowered clause (its repair group numbered after them) into
`evaluated_singleton_row_constraints`, and only the elastic input receives it, so the
repair model loosens it one kept row or class at a time (D28) while the solved model
keeps the box alone. Those clauses are the repair model's only rows for a folded bound.
A bound against a data column (`x <= cap`, `cap * x <= 10`, an easy `max(cap * x) <= 10`)
is not `folded_bound`: it flattens to a per-row spec that carries the classifier's admitted range
(`DecideConstraint::admitted_lower` / `admitted_upper`, stage 05). The ends read the clause
as `a * x <comparison> r` through two inputs, `admitted_a` and `admitted_r`, which both ends
read many times: the tree computes the inputs once, as programs on the clause's entries the
reducer's filter keeps, then both ends over them (`DecideModelEmitter::Admit`);
a row that is no member admits every value. An input that is a constant is in the
ends already, and stage 08 constant-folds each tree it evaluates once (`ConstantFoldingRule`,
in `DecideFoldConstants`, after messages have rendered from the prepared trees). It
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
pass runs, not after. `FormulateElasticModel` (`decide_diagnostic_terminals.cpp`) widens the box on a copy of
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
pipeline, before the elastic engine ever sees it, pointing at the clause that wrote it
(*"the constraint cannot hold: 'x' is never negative"*):

- `x <= -1` on a non-negative type, guarded as `U < 0 AND L >= 0` so that
  `x = -1` or `x <= -1 AND x >= -5`, which explicitly lower the floor, still pass;
  a negative value in an `IN` lowers it too (`x IN (-5, 0, 5) AND x <= -3` answers -5).
  An `IN`'s bounds are removable (diagnosis may drop the whole clause), so the guard
  rebuilds the box from the retained bounds and keeps of the removable ones only a
  negative floor: that is a fact about the domain, not a bound the user wrote;
- `x >= 2` or `x = 2` on a BOOLEAN.

A purely user-vs-user inverted box such as `x >= 5 AND x <= 1` does **not** match
and proceeds to the elastic engine, which reports a least-change loosen.

One function states the test and the message (`DecideBoxContradictsDomain` and `DecideDomainRefusal`,
`common/decide_value_messages.hpp`), and two steps call it. Physical planning checks the box the constant bounds
fold into (`PhysicalDecide::CheckDecisionDomains`, which fills the tree's `domains`): the check reads only the
plan, so `EXPLAIN`, `PREPARE` and a query over an empty input raise it as a run does. A constant bound under a `when`, or under a `per` over a written
column, folds into no box (`../05_optimizer/done.md`, singleton-row folding), so the model emitter checks each row
such a clause writes (`DecideModelEmitter::CheckDomain`, from `Rows` and, for an easy MIN's or MAX's members, from
`Groups`). It runs after `SkipNullInstances`: a row in no instance (a NULL key, a `when` that does not hold) is never
checked, and a query whose rows all have a NULL key solves. A row's admitted ends fold into the decision's box, whose
floor is the lowest any `when` or `per` lowers the decision to; instance floors are built in stage 06, after the
emitter, and the lowest floor never refuses a query that solves. Only a bound whose admitted ends fold (`IsFoldable`),
one written as a constant, is checked; a bound that reads the data stays the solver's and DIAGNOSE's, and an IN's
bounds stay diagnosis's under a `when` as they do without one (its removal group). The refusal
points at the clause (*"the constraint cannot hold: 'x' is a BOOL decision"*), under DIAGNOSE too. Which rows
an instance keeps is known only from the data, so direct solve declines every such bound the domain cannot hold
(`DirectContradictsDomain`), and both paths name the clause.

### Implied bounds from constraint data

Once coefficients are numbers, a second, data-driven source of column bounds
becomes available: `DecidePropagateImpliedBounds()`, a pass of stage 06's `PrepareModel`, pure over `SolverInput`. See
[`../06_model_formulation/done.md`](../06_model_formulation/done.md) §9.

The call is ordered here, immediately after the singleton-row bounds are resolved and
before any Big-M is computed, because every downstream constant depends on the
tightened box.

The pass skips constraints it cannot reason about soundly — any negative coefficient,
product groups (squares and products), MIN/MAX and `<>` indicator rows, and a clause with
rows outside its row map, such as one under a `when` (whose bound would wrongly cap the
excluded rows, since the derived bound is shared across all of a variable's rows); stage
06 §9 gives the exception for a `per K:` clause.

---

## 3. The lowered terms

Execution does **not** flatten expressions. Each clause, the objective and each helper's reducer
arrives in the tree node that holds it already decomposed into additive terms by `DecideTermLowering` (stage 05,
`src/optimizer/decide/decide_linear_form.cpp`), which writes them into the tree under construction at physical
planning: a clause's `DecideConstraint` into its root's `DecideBody::clause`, the objective's into the objective
root's, a helper's members into its G or O (`DecideBody::members`, `fallback`), and the frame
columns the terms read into `DecideTree::frame_reads`. The tree reads them where they lie: `DecideTree::Constraints()`
by `clause_index`, `DecideTree::Objective()`.

**What this stage receives.** For every constraint: its terms, each naming a
variable index, a sign, a still-unevaluated coefficient `Expression`, an optional
filter (its reducer's own `when`), an AVG-scaling flag and the key scope of its
reducer's own `per` (`qualifier_scope_idx`); plus the RHS expression, the comparison
type, the clause's scope (`DecideClauseScope`: `when`, `per`, `class_key`), its `by` key (`group_key`), and the metadata
linking it to indicators and diagnosis provenance. The
objective arrives the same way, with its `when`, each AVG term's `avg_group_key` (an index into the key table, set
for a nested AVG read in place), and `product_groups` alongside `terms`.
A helper's G holds its member terms, so no construct is left re-deriving its own.

**What this stage does with it.** Evaluates the coefficient expressions against the
materialized rows (PHASE 2), and nothing else. Which variable a term names and what
multiplies it were settled upstream, where a binder was in scope.

**The shape is asserted, never repaired.** A decision variable on the RHS, a
non-reducer term in an aggregate LHS, an unrewritten MIN/MAX — each is an internal
invariant failure raised by the flattening pass at plan time, not something
execution fixes. The structures themselves are documented in
`src/include/duckdb/planner/decide/decide_tree_terms.hpp`; the algebra that
produces them, and why it cannot live here, is
[`../05_optimizer/done.md`](../05_optimizer/done.md) §1a.

---

## 4. PHASE 1.5 — keyed mappings

For each `KeyedScopeInfo` — a keyed decision's key (`per K: x`), a reducer's own `per`
(`sum(per K: e)`), a helper decision's `by` key — before
coefficient evaluation:

1. Read the scope's key in the descent, which reads every row for a declared decision's
   scope (its classes belong to the source as well as the clauses) and a helper's where
   its rows are reached: the key's dictionary is a `DecideKeyMap` (§5). A row with
   NULL in a written key column is in no class; a relation's columns compare NULL as a
   value. A key with no columns (a reducer's `per ()`) is one class, every row in it.
2. A helper decision's scope (`DecideValueHelperOfScope`, the value rewrite) reads its
   link's path key on the rows its path keeps only (`DecideHelperDefinition::when`: the clause's
   `when` and every enclosing reducer's own), so the key is never
   evaluated where the clause never reads: one class per group that holds a member,
   numbered as its definition numbers them. A group with no member has no helper (its
   reducer is NULL there, D68), so its rows map to the absent class below.
3. A row in no class (a NULL written key, or a row a helper's clause drops) has no
   decision. It maps to one spare class, `KeyedMapping::absent_class` (the Null Object
   pattern), so every row has an id and a read by row stays total. The model builder
   fixes the absent column at 0, so an objective term that reads it is dropped; a reducer
   over the decision never reads the row (`DecideCanonicalizer::SkipNullData` tests the
   key's written columns for NULL), and an unreduced read skips its instance
   (§5). The output shows NULL there.
4. Class ids are the key's group ids: numbered by key value (below), so a keyed
   decision's solver columns follow its key's values, not the rows' order.
5. A declared decision fills a `row_to_class` vector of size `num_rows`, which the output reads back. A helper's
   rows stay with the emitter (`DecideScopeRows`): what it writes names each helper's instance, so stage 06 reads no
   row of one.

The result is a `KeyedMapping { num_classes, row_to_class, absent_class }` on the
`SolverInput`, which is what makes several rows share one solver column.

---

## 4b. PHASE 1.4 — frame selectors

Once the descent has its O's timelines (§1a), before any clause is read, `DecideTreeColumns::ReadFrames`
(`physical_decide_frames.cpp`, `decide_frame_map.hpp`) builds a `DecideFrameMap` per frame selector
(`../../language/guide.md` §5.6): its anchors are the rows its scoped expression reads (`DecideFrameSpec::rows`),
grouped into timelines by its timeline key through the one key cache, so a row whose written `within` column is NULL is
in no timeline (D75). Each anchor's order key becomes a DuckDB sort key -- the frame's direction, NULL where
`default_null_order` puts it (D74) -- and each timeline's anchors are stably sorted into peers, rows of one order-key
value. A position counts peers: `first`, `last`, `k previous`, `k next`, wrapped when cyclic, missing past either end.
Under a `per` that does not hold the timeline, `DecideTreeColumns::CheckPartition` stops a class that spans two
timelines (D75).

`ComputeDecideFrameColumns` then computes each column stage 05 registered (`DecideFrameReads::columns`): a payload is
read on the first row of each selected peer only, an `else` on the anchors that reach it only, so an unused one raises
nothing (a cast that would fail there does not), and the values are gathered onto the anchors. Every later phase reads
the frames as data columns, and readback references the input's own columns only. `ReadFrames` then calls
`DecideTreeColumns::CheckPositions`, which needs the finished descent and the skipped NULL reads and holds each position
a frame reads -- a point's selected peer, every position a range's window visits, one whose NULL reads left it nothing
to read included (`DecideFrameMap::WindowVisits`) -- of several rows to one signature of its payload (D73, deck p. 110):
each column the payload reads by value (a point's columns, a range's reducer argument or its helper's member terms),
each decision by its instance, whatever its coefficient. A frame's decision term names its decision, at the instance
the selected row reads (`DecideModelEmitter::InstanceOfRow`, `DecideInstanceOfRow::frame`); a query-wide decision has
one instance, whichever row is selected.

Where a frame has nothing to read, every column of it holds NULL, which is no value (D84) as any NULL a clause reads:
the instance is skipped (`DecideAbsence::SkipNullInstances`), a reducer's member is out of its filter, an objective term is dropped. An
anchor in no timeline is such an anchor whatever its `else` (`DecideFrameMap::Untimed`): it misses no position, so no
`else` stands in, and it is absent (D75). A frame over data is that column alone, so a scalar function around it reads
the NULL as SQL does -- `coalesce(at(previous: t.d) over (...), 1)` is 1 there -- and only a NULL that reaches the
comparison skips it (deck p. 56). A frame over decisions is its payload's terms, each read at the selected row
(`DecideAbsence::At`): where the frame has no value, or the decision has none at the selected row, the term
reads an instance with no value, as below.
**No value is one rule** (literal plan §6.3; D67, D68, D84, D87, D88, D90): `DecideAbsence`
(`decide_absence.hpp`) holds its table, from a read up and never down -- a keyed decision's row in no class, a helper's
group with no member, an auxiliary whose definition reads a decision with none, a product either of whose factors has
none, a frame's term where its frame selects nothing -- and the two steps that skip a clause's instance, a group one
of whose reducers reads no row (p. 56, before the group's bound is read) and an instance a NULL reaches (p. 50).
Everything that reads absence asks it. `DecideAbsence::At` answers whether the
instance a row reads of a decision has a value. A keyed decision has none on a row in no class of its scope (its
mapping's absent class), and so has a helper's group with no member. An auxiliary -- an ABS's, a body helper, an L0
indicator, a product -- has none where the rows defining it (`DecideConstraint::defined_var`, the helper of the definition that states the row;
a product's factors) read an instance with none (`DecideModelEmitter::absent_reads`). The model
emitter (`ReadAbsence`) resolves both in the order the definitions read each other -- a helper's members may read an
auxiliary whose definition reads a helper made after it -- whatever order the rewrites numbered them in. Every reader
reads that one mark: a comparison skips the instance, a helper drops the member, the objective drops the term. A term's
own read is one check, `DecideAbsentReads` (`decide_absence.hpp`), built over the terms a clause reads that may read
none and asked per row: whether the term stands in a row (`Rows`), beside a reducer (`ReadBesideReducers`) or in a
reducer (`ReadInReducer`), a product's part as a term. Every folded bound asks it too, so a folded bound
counts its skipped instances and, with none kept, warns that it constrains nothing, as the clause unfolded does: a lone
`max(when val > 100: x) >= 5` over no matching row, or `per k: c BETWEEN 0 AND 10` over rows in no class. `MayBe`
answers exactly once the classes are read, so a folded bound over a keyed decision with a class at every row reads
nothing. A row a rewrite added to define an auxiliary (`ConstraintKind::STRUCTURAL`) is no instance of the clause: it
is skipped where its own reads have no value, and the auxiliary with it.

A frame field of a signature (D82, deck p. 99) splits by the frame's keys (`DecideDescent::Signature`): the anchor's
peer on its timeline when it reads relative to the anchor, else its timeline, and where the anchor reaches the `else`
what the `else` reads -- its FALLBACK columns and its decisions' instances at the anchor. An
anchor where the frame reads no value is no variant, so the objective's check (D72, `DecideFrameDifference`) counts only
anchors that read one, and names what differs among them: a position, a timeline or a fallback.

A frame bound of a clause over groups that differs within a group (`DecideConstraint::tightest_frame_bound`, D85) is read row by
row as any bound, its NULLs apart, and then each group's kept rows, a row a reducer's own `when` drops included, take
the group's tightest value among those where it has one (`TakeTightestFrameBound`): the least under `<=`, the greatest under `>=`, the one value under `=`, whose second is
the `=` message a side table raises for a column; a group with none is marked in `null_rows`, so its instance is
skipped. A row in no class of the clause's `per` key is not read at all, as `InClassOnly` keeps a column's from being
read (D5). The value is taken last, just before `DecideAbsence::SkipNullInstances`, and only for a group the clause still holds: a group
some rule dropped, or a NULL on its left side will, has no instance, so no bound and no `=` check (D68). Its frame
columns print as the frame is written: a column past the source's columns is named by its text
(`DecideSourceNames::source_columns`).

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
window (the emitter's `HelperRows` reads the windows; an absent anchor is in no class), and its members are listed
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
  class, at the variant's first row, which stands for the variant's rows: they agree on every field. A clause with no
  `per` (or `per row:`) that reads no per-row decision is read as `per ()` is, once per distinct signature over the kept
  rows: a query-wide or keyed decision is one value on the rows of its instance, and one that reads a per-row decision
  keeps a row per kept row. Classes of a `per K:` clause that agree on everything it reads are one variant and one model
  row; a clause that reads a field its key holds (a column of the key, or a function of one) or a frame keeps a row per
  class, which costs rows and no answer. Where a class holds more than one variant the clause records an EXPLAIN
  ANALYZE line (`RecordVariants`, printed by `ParamsToString` from `run_time_accounts`, which a plain EXPLAIN never
  fills); a clause with no `per` has no binder signature and records none. The emitter names each variant for a
  diagnosis by its class's key values followed by its own field values (`DecideModelEmitter::NameVariants`: a
  key-values entry of its own, numbered by variant; a collated key column quotes the class's least spelling, D69).
  Under DIAGNOSE such a variant's row stands for its own class set.
- A reducer's variant scope (`KeyedScopeInfo::signature`) takes the signature's numbering
  over the rows the reducer reads, so its operand holds one entry per variant of each class
  (below).

The value checks the bound-side rewrite (`RewriteRhsAggregates`, stage 05) appends stay on the node
(`LogicalDecide::value_checks`) and are raised by the tree before any clause is read (`DecideTreeColumns::CheckData`): a bound's
NaN, a NaN in a bound-side reducer's argument (the reducer skips a NULL, as SQL does), or an `=` bound that takes two
values in its group (`` `<bound>` takes more than one value here (a and b), in the group t.g = 'a' ... infeasible.``).
An `=` bound under `per K:` is checked per group and class first, then per group (`AddDisagreementCheck`): a class
whose rows hold two values is named with its group (`in the group g = 'a'`), and two classes holding one value
each name none, since a group merges its classes' instances into one constraint. The checks stand among the scope
checks, after the bound's NaN check, whether or not a direct-solve rule takes the clause, so every path raises the
same one. The group is named
as the query writes its key (`DecideBoundDisagreementMessage`, called through the internal scalar function
`__internal_decide_bound_disagreement`, and `DescribeKeyValues`, the spelling DIAGNOSE uses); a bound over the
whole query names none. A collated key prints the spelling on the row that raises the check. A frame bound's check
is the tree's, made as it reads the frame (`TakeTightestFrameBound`), and prints its group's least spelling; no
direct-solve rule takes a frame.
An empty group is no check: a reducer over no rows is NULL, so its instance is skipped where the clause is
read (D68; below).

Groups come from the descent's dictionaries (`DecideKeyMap`): a row with NULL in a written key column
(`DecideKeyNullMode::NO_GROUP`) is in no group, and a relation's columns (`VALUE`) compare NULL as a value. A key
with no columns (`per ()`, `by ()`, a reducer's `per ()`) is one group of every row the filter keeps, with no table.
A signature reads its `when` on the rows that reach it, and its key and fields only on the rows it keeps.

Every clause is then read by these rules; §5 gives the row maps they produce:

- **Kept rows first.** Its `when` is read before anything else, on every row, in one scan
  (`DecideDescent::Gate`), each window reading the parts of an AND or an OR left to right,
  each only on the rows the earlier parts leave undecided -- an AND's on the rows they keep,
  an OR's on the rows they do not. Anything else, `not` included, is computed whole. Nothing is then
  computed on a row the `when` drops: a reducer's own `when` is read on the kept rows,
  a term's coefficient on the kept rows its own `when` keeps, a bound, a `by` key and a
  class key on the kept rows: the descent passes on only the rows that reach a node, and
  each program runs on its node's entries. `ExtractDoubleColumn` writes 0 for a row it does
  not read, so a NULL or a failing expression there is no error.
- **Grouping.** The aggregate shape groups the kept rows by its `by` key
  (`keys[constraint->group_key]`), a NULL written key in no group: a group's members are the rows a
  reducer's `when` keeps (all kept rows for `<>`, whose bound values are all excluded),
  its bound the tightest over all its kept rows. A row the reducer's own `per`
  de-duplication drops leaves the group when the reducer is a MIN or MAX (a helper's group): the members are the rows
  the reducer reads, and a zeroed row would be a member pinned at 0. The rows shape under `per K:` emits one row per
  class, at its first kept row; under `when` alone, one per kept row.
- **Bound-side reducers** never reach execution: the bound-side rewrite
  (`RewriteRhsAggregates`, stage 05) made each a column of the source, a side table
  DuckDB evaluates over its own `by` groups of the kept rows, and reduced the bound of an
  aggregate clause to its group's tightest value.
- **De-duplication** for a reducer's own `per` keeps the first row of each class among
  the rows its `when` keeps (its operand's first entry of each instance, `FirstOfEachInstance`), within each group:
  a class whose first joined row the `when` drops is still counted.
- **The objective**: its `when` is folded into every term's filter, so reading, de-duplication and AVG denominators all
  follow it. A decision outside the reducers (`NONE`) keeps its coefficient on the first kept row only
  (`CountTupleTermsOnce`, over `CountOnce`), and so does a product or a square of query-wide decisions there (one inside
  a reducer is SUM and keeps every row). `CountOnce` is the one place a term is counted once, in the objective and in
  each group's row of an aggregate clause; stage 06 sums every row.
- **Classes of a grouped row** (D28, `DecideGroupClassSets`, under DIAGNOSE only). The rows of a clause over `by` groups
  -- an aggregate-shape group row (one that reads a MIN/MAX helper among them) and an easy MIN/MAX's member rows --
  stand for every class of its `per` key in their group: with `per K:` in front (`scope.per = NAMES_CLASSES`,
  `class_key`) the key's values among the group's kept rows (in key-value order); with no `per` (`NONE`) every kept row
  of the group; `per ()` the whole clause (one `WHOLE_CLAUSE` set its groups share). A per-row clause under `per K:`
  (`ONE_ROW_PER_CLASS`) calls it with its classes as the groups, so each class's row gets a set naming that class. One
  `SolverInput::ClassSet` per group with kept rows, its `Kind` saying which, referenced from
  `EvaluatedConstraint::group_class_set`; the model keeps the table and an expanded diagnosis reports the group once per
  class (`FanOutByClass`).
- **Helper decisions** (the value rewrite, `DecideModelEmitter::ValueLinks`): each link's
  reducer is read on the rows its path keeps and its own `when` keeps among them, less a
  row in no class of its own `per` (a NULL written key, D5) and a
  row at which one of its terms reads a keyed decision with no instance there -- a nested
  reducer's helper whose group holds no member, which is NULL (D68) and a reducer skips
  (D67, D87) -- grouped by its path key over those rows (`HelperRows`, which gives the
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
  by its reducer.
- **A helper the plan writes where it is read** (`DecideHelperForm`, R43, R44, R47). `ValueLinks` runs before any
  clause or the objective is read, children first. `HandLink` keeps such a helper's members in the emitter
  (`written_helpers`), and `Settle` ends its listing: the groups that pass the form's test are written where they are
  read, and the others go to stage 06 as a link. A `WRITTEN_IN` SUM or AVG is settled whole and a
  `ONE_MEMBER_WRITTEN_IN` MIN or MAX by its groups' member counts, both as they are listed. A `ONE_ROW_WRITTEN_IN`
  SUM or AVG waits for the one clause that reads it: as that clause's records are finished, `SettleRowHelpers` counts
  the rows that name each instance and writes in the groups one row reads; a helper no row names keeps every column
  (`SettleUnreadHelpers`, once every clause is read). `WriteMembersIn` then rewrites each list of
  terms that names a written-in instance, replacing the term by the instance's members times its coefficient and
  adding their constants to the row's: a clause's records as `Rows` and `Groups` finish them (an ABS envelope's rows
  among them), a later helper's members, and the objective's terms (`ObjectiveCoefficients`, which then sums
  every term of the objective per instance, `SolverInput::objective_coefficients`). One function serves the three, since a clause's records and a helper's members
  delimit their terms the same way. Stage 06 receives `written_in`, the instances with no column, and lays its columns
  out once. A clause row or a member that reads a written-in helper where it has no member is an internal error; the
  objective's read of one is no read (D84).
- **Counters** (`DECIDB_PROFILE`): `anr.frames`, `anr.frame_windows` and
  `anr.frame_positions` (one per point anchor, each value a range's windows list) once the
  frames are built; `anr.helpers`, `anr.helper_instances` (groups), `anr.members` and
  `anr.member_terms` and `anr.inlined_helpers` (the helpers handed no link) once the helpers' members are listed
  -- beside the model's own `model.variables`, `model.linear_rows` and `model.linear_nonzeros`.
  `benchmark/decide/anr_scaling.py` reads them (`../../performance/benchmarking/anr_scaling.md`).

---

## 5. PHASE 2 — coefficient evaluation

### Expression transformation

The tree's compiler copies an expression over its site's layout (`ToChunkReferences`), and `DecideFoldConstants`
constant-folds a program once with DuckDB's own `ConstantFoldingRule` where it is executed, so no constant subtree
is evaluated per row. Folding rewrites no node: `ColumnBindingResolver` has
already turned every column reference into a chunk position, and the bound-side rewrite has made every reducer on a
bound a column.

Results are cached per `Finalize` call, and the cache outlives every
`ExpressionExecutor` created below it, so executors may safely retain references
into cached entries.

**Precondition: resolution has already happened.** Stage 08 reads a column reference as a chunk position, which
holds only because `ColumnBindingResolver` already rewrote DECIDE's expressions. That
enumeration is maintained by hand — see
[`../03_logical_plan/done.md`](../03_logical_plan/done.md) §4.

### Per-term coefficients

Each term's coefficient expression is transformed, read window by window from the one
pass (§1a), and cast to `double` via `DefaultCastAs(LogicalType::DOUBLE)` —
**one conversion, on the result of the complete typed expression**, which is what
keeps a data cast's SQL semantics intact.

Data-only LHS terms carry `INVALID_INDEX`. For aggregate constraints those fixed
coefficients are still evaluated per active row; their row or group sum is the record's constant, which the model
builder subtracts from the RHS.

### A clause is read over the rows it keeps

`DecideModelEmitter::Keep` lists the rows a clause's root reached, each once and in row order, with the entry of the
root's container that holds it (a row routed to a root twice is read at the entry that reached it last). A row's
**place** is its position in that list. Everything the emitter evaluates for the clause is as long as the list, never
as long as the input: a reducer's filter masks, the group of each row, the coefficient columns of an aggregate
clause. One scratch vector as long as the input (`place_of_row`) maps a data row to its place while the clause is
read and is cleared after it (`Unplace`), so no clause allocates by input length.

- **A per-row clause** (`Rows`) writes its records straight from the root's entries: one record per kept row, its
  terms, bound and admitted ends read from the entry's values, with no column between.
- **An aggregate clause** (`Groups`) reads each term into a column over the kept rows (`EvaluatedClauseColumns`:
  `term_variables`, `row_coefficients`, `rhs_values`, the admitted ends, `row_group_ids`), then `DecideClauseRowSets` lists the places
  of each group by a counting sort and `DecideWriteClauseRecords` writes one record per group, turning places back
  into data rows. The same sets give an aggregate `<>` its excluded values and a quadratic clause the data rows of
  its products.
- **A clause that keeps no row emits no row** (D68), aggregate or per-row.

**Why an aggregate clause is evaluated, grouped, then gathered.** The columns over the kept rows are the vectorized
evaluation (R23), and the grouping is read from the tree's container slots, not computed again: the sets are one
linear bucket pass. A record written straight from a reducer's container fits two of the five shapes below, since a
term beside the reducers, a `per K:` class and a `when` with no key give a kept row a group no reducer's container
holds. What the path decides is the language's rules (which rows emit a group, D68, `<>` reading every kept row), which
a walk per group states again. An aggregate clause always holds a group for each kept row, so a clause that keeps
nothing and a clause with one group are never the same state. A quadratic clause's product groups are columns over the
same places (`QuadraticClause::rows`, `data_rows`): as lists of terms they take about three times the memory, and
the objective's, dense over every row, would keep the columns, leaving the builder two readers.

The records are `ClauseRecords` (`../06_model_formulation/done.md` § The rows a clause emits). Measured on
`context/opa_rewrite/repro/clauses_times_rows.sql` (40 clauses over 2,000,000 rows, each keeping 2,000) against
columns as long as the input: `execution.tree_model` 32 ms against 534 ms, implied bounds 0 ms against 181 ms, peak
memory 1.03 GB against 2.78 GB. A clause that keeps every row gains nothing from places and pays for its records:
`y <= v` alone over 1,000,000 rows spends 103 ms in `execution.tree_model` against 53 ms, and 66 ms against 82 ms in
implied bounds and the builder's clause rows.

### RHS

- **Constant** — extracted once and broadcast.
- **Expression** — evaluated per row, giving row-varying bounds.
- **Reducer** — a side-table column of the source by now (`RewriteRhsAggregates`),
  read like an expression.

The canonical RHS classification decides which: a clause whose `query_wide_bound` is set
(`BoundDecideClause::query_wide_bound`, carried by the lowering to `DecideConstraint::query_wide_bound`) has a shared
scalar; anything with a row-varying component stays data-backed. Stage 08 reads the clause's fields and inspects no
expression for it.

### `when`, `by` and `per` → `row_group_ids`

One vector over the kept rows assigns each to a row of the clause, or excludes it. The clause's
`when` is read first (in the descent); every map is built over the kept rows only, with ids
dense in key-value order. A row with NULL in a written key column is in no group of that
key (`DecideKeyNullMode::NO_GROUP`, D5); it stays in the groups of every other key the
clause reads:

| Clause | `row_group_ids` | `num_groups` |
|---|---|---|
| aggregate shape, no `by`, no `when`, no reducer `when` | 0 on every kept row | 1 |
| rows shape, no `when`, no `per K:` | empty: every kept row emits its own row and no class numbers them | 0 |
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
plan's round trip keeps it, which a function's result type alone would not. The descent evaluates the collation at
the rows' own positions (`DecideDescent::FrameOrder`), so a collation that raises names its row. The default build's
collations (`nfc`, `noaccent`, `nocase`) raise on no valid UTF-8 (NUL, 200,000-character strings, U+10FFFF, U+FFFD
and combining marks tried), and ICU is not built in, so no test reaches that error.

The bound side of the aggregate shape gets its own map with the same numbering over every
kept row (a bound-side reducer reads its own `by` groups, §4a). A `per K:` over the
aggregate shape or an easy MIN/MAX adds no row: it only names the classes each group's
row stands for (D28, §4a); when its key leaves no class among the kept rows (every key
NULL), the clause has no instance.

**An absent decision reads like a NULL.** After the terms are read, each row where a linear
term or a product group's form reads a decision on its absent class (§4) is marked
(`DecideAbsence::At`), and `DecideAbsence::SkipNullInstances` skips that row's instance as
it skips one with a NULL coefficient. So `sum(x) by (g) <= 0.5 * sum(x)` gives a NULL-`g`
row no limit (its `by (g)` helper is absent there) while the total still counts it.

A key's groups are the tree node's dictionary, filled once by the pass, so one key is
read once however many clauses use it. A clause takes the groups its rows keep
(`kept_groups`, each one's rank in the node's dictionary); a caller that names a group
reads that group's values (`DecideDescent::Label`) and formats that one label, so no label
is built for a group no message quotes.

### A reducer's own `when`

Term-level filters are carried on individual terms, not on the whole constraint:

1. Each distinct filter expression is evaluated once into a boolean row mask, on the
   clause's kept rows only (`TermFilterState::mask`).
2. Linear terms and product groups get coefficient `0` (a group, weight `0`) on rows their own
   filter excludes — **for that term only**.
3. A row joins a group when the clause's `when` keeps it and some reducer's own `when`
   keeps it (a reducer with no `when` keeps every kept row).
4. A reducer over no rows is NULL (D68): `DecideAbsence::SkipEmptyReducers` cuts every group
   in which some term's mask keeps no row, after the de-duplication masks are folded in.
   Only a reducer's term carries a mask (its own `when`, its NULL-data and absent-decision
   tests, its de-duplication), so a term with none reads every row of its group.
5. What the skips left is counted (D90, guide §7.4): each clause's instances once grouped -- in the aggregate shape
   every group of its kept rows by its key, from the key's cached grouping, whether or not a reducer reads a row of it;
   under a `per K:` that names classes, the groups a class's kept rows reach, plus one for each class whose rows reach
   none (a NULL `by` key), which has no value -- and those left after `DecideAbsence::SkipNullInstances`, the most any
   of its rows holds (`ClauseInstances`). `ReportSkippedInstances` turns the counts into an EXPLAIN ANALYZE line under
   the clause and, for a clause left with none, a warning on stderr, once per clause the user wrote: the two comparisons
   a `BETWEEN` becomes are counted apart and reported as one, with the most either holds (`WrittenClauseId`, which the
   removal groups of stage 05 read too); `ReportAnEmptyObjective` reads the formulated objective (its linear, squared,
   bilinear and global coefficients) once the solved model is formulated.

So one clause can carry independent filters:

```sql
sum(when active: x * a) by (grp) + sum(when priority: x * b) by (grp) <= 10
```

`active` filters only the `a` term and `priority` only the `b` term; each `grp` group is
one row, over the rows either keeps. A group where `priority` keeps no row has no row:
its second reducer is NULL there.

### A reducer's own `per`

`sum(per K: e)` asks for one contribution per **class** of `K`, and in `sum(per D: e)`
`K` is every stored column of `D`; a join repeats each class once per matching row. De-duplication (`FirstOfEachInstance`) keeps
the first row of each class among the rows the reducer reads — the kept rows its own `when` keeps — and zeroes the rest.
A reducer whose body reads more than the key holds has a variant scope of its own (D71), so its classes are the key's
split by the body's values: every row of a kept identity carries the same value, and whichever row is kept gives the
same answer. A class whose first joined row the `when` drops is still counted once.

De-duplication runs **inside** each `by` group, after the clause's `when`, the reducer's
own `when` and the grouping. Group ids are dense in `[0, num_groups)` and class ids in
`[0, num_classes)`, so `(group, class)` flattens into one dense index with no hashing.
The mask is folded into the term's filter, so AVG's
denominator counts classes. For a MIN or MAX helper, a row the de-duplication drops leaves the
group (§4a).

### AVG scaling

The linear form marks the terms of an AVG reducer (`DecideReducerKind::AVG`) `avg_scale`. Terms
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

A product group from an AVG scales its weight by `1/N`.

### Validation

Every coefficient and RHS value is checked after evaluation:

- **NULL** → the value is skipped (deck p. 50). In a constraint, `ExtractDoubleColumn`
  marks the row in `null_rows` and `DecideAbsence::SkipNullInstances` cuts the instance the row
  reaches: the row of a row constraint, the group of a grouped one (`INVALID_INDEX` in
  `row_group_ids`, so the clause emits no row for it). A reducer never meets a NULL
  summand: `SkipNullData` tested it in the reducer's filter. The objective never meets
  one: `DropNullObjectiveTerms` read it as 0 in stage 05, so a NULL there is an
  internal error.
- **NaN / Infinity** → `InvalidInputException` with `DecideInvalidValueMessage`: the
  value as written and the in-place `CASE` that skips its rows.

**Infinity is admitted on one path only: the values that become a row's `rhs`.** An
infinite bound is not an error there — it is the absence of a constraint (`<= +inf`)
or one nothing satisfies (`>= +inf`) — and the model validator accepts a non-finite
`rhs` while rejecting NaN. Refusing it on the per-row path alone would treat one bound
two ways by spelling: canonicalization rebuilds `x + v <= K` as `x <= K - v`.

The input of a **bound-side reducer** follows the same rule: `min(x) by (g) <= max(cap)
by (g)` is a window of DuckDB's over `cap`, and its value check refuses NaN
only, so an infinite `cap` is a value; a NULL bound skips its instance. An infinite bound
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
bound to classify. It calls `DecideTightPerRowBigM` across the stage boundary. The
rows it refills are the first two of an `L0_INDICATOR` F node whose definition is
`auto_m`, which the emitter stamps as it routes them; no row is recognised by its shape
or by a decision's name.

A *variable* with no finite bound is the separate case, and the answer there is also
refusal rather than a constant — there is no fixed Big-M anywhere in DeciDB. The order
in which this stage calls the linearizers (ABS first) is load-bearing for it: see
[`../06_model_formulation/done.md`](../06_model_formulation/done.md) §9, "Big-M constants".

### The construct routing — a decision this stage reads, not one it makes

Whether a construct is lowered is a **formulation** choice, and formulation belongs to
stage 05. This stage reads the answer off the operator and applies it. `FormulateModel`
builds a `NativeConstructPolicy` for ABS and one for MIN/MAX from `use_native_constructs` and
`force_native_constructs`; hands over the ABS records with their envelope rows (`EmitAbsEnvelopes`); derives every ABS
auxiliary's box with the product and helper boxes until none moves, then decides the refusals (an exact ABS refuses an
unbounded contributor only where the backend declares no native ABS); states each exact ABS
natively or lowered (`EmitAbsConstraints`); then runs the value-link, MIN/MAX, `<>` and bilinear linearizers, and last
`LowerDecideConstructs`.

`use_native_constructs` is a `SolverConstructSupport` copied from
`LogicalDecide::use_native_constructs`, which stage 05 filled in from the chosen
backend before it rewrote anything. **This stage must not ask a backend what it
supports.** The routing has to be the same answer the rewrites upstream were selected
against, and a second, independently-derived answer is exactly how the plan and the
solve come to disagree about what was lowered. Choosing here would also keep stage 05
from acting on the choice: see [`../05_optimizer/done.md`](../05_optimizer/done.md) §0.

**The `VarIndexer` is built first, before the column box is settled.** It reads
`num_rows`, the variable scopes and the keyed mappings, all settled when
`BuildSolverInput` starts, and only the global block grows afterwards, with
`total_vars` refreshed before the solve. The floors a `when` lowers need its row to
instance map before the first box pass, and every construct site emits in flat
columns at the point it decides something, instead of stashing a decision for a
second pass to execute.

**A construct arrives as a policy, not an answer** (`NativeConstructPolicy`: the capability, whether its lowering is
measured faster, and the test-only force; the one rule is in [`../05_optimizer/done.md`](../05_optimizer/done.md) §0).
Whether a given
clause has a valid Big-M is a question about evaluated coefficients, so this stage answers
it per clause, from the reach of that clause's own family. One statement can have a
bounded clause and an unbounded one, and each gets the formulation it can use.

`DECIDB_NATIVE_CONSTRUCTS` A/B-tests the two arms on one machine: `off` forces every
construct down its lowering path, `force` states every declared one natively, and `on`
(the default, and what an unset variable means) is the default policy. For MIN/MAX the
A/B has to be `force` against `off`: on a bounded shape the default *is* the lowering,
so comparing it against `off` would compare the lowering with itself. For ABS the default is native, so the default
against `off` compares the two forms.

The backend itself reaches this stage as a **name**, resolved by `PlannedSolverBackend()`
only where a solve is about to run ([`../07_solver/done.md`](../07_solver/done.md) §2).

**Each construct chooses in one place.** `<>` does not choose: it is emitted once, as conditional rows, and
`LowerDecideConstructs` turns those into Big-M rows where the backend cannot state a
condition. Every MIN/MAX — a constraint's, the objective's and a nested reducer's — is a helper of the value rewrite and
goes through one `EmitExtremumLink`, which makes the choice once for all of them. An ABS makes it in
`EmitAbsConstraints`, where the two formulations are genuinely different objects rather than projections of
one description: see [`../06_model_formulation/done.md`](../06_model_formulation/done.md)
§9a and [`../07_solver/done.md`](../07_solver/done.md) §2.

Only the *hard* directions need a choice at all. `MAX(e) <= K`, `MIN(e) >= K`,
`MINIMIZE MAX`, `MAXIMIZE MIN` are exact with a one-sided envelope plus outer pressure:
no Big-M, so nothing to replace.

Both ABS forms read the same record (`AbsLinkSpec`), which stage 05 keeps rather than lowering, because the Big-M
constants are functions of evaluated data. Neither form decides anything, which is what keeps the two comparable;
`EmitAbsConstraints` does.

A member that is already a column is passed through, not copied, and the native
columns that remain are boxed per row wherever a range is derivable:
[`../06_model_formulation/done.md`](../06_model_formulation/done.md) §9a and "ABS".

### Output

A `SolverInput` carrying `num_rows`, `num_decide_vars`, `variable_types`
(reported as `BOOLEAN` wherever `is_boolean_var` is set), resolved
`lower_bounds` / `upper_bounds`, the clause rows (`constraints`, `not_equals` and `quadratics`), objective
coefficient and variable-index arrays, keyed mappings, and the objective's constant. The objective is a
minimization and carries no sense.

**One list per kind of row.** The emitter reads each clause into an `EmittedClause`: the row
(`EvaluatedConstraint`) and what only some kinds of clause carry, which are a `<>`'s NULL bounds and AVG scaling and a
quadratic clause's product groups. `Groups` (the aggregate shape) and `Rows` (the per-row shape) return it, and one
`DecideModelEmitter::Route` puts it in the list of its kind, so a row never says what kind it is. A folded bound's
clause goes among the repair model's rows (`evaluated_singleton_row_constraints`, under `DIAGNOSE` only). A
`COMPARE_NOTEQUAL` goes to `SolverInput::not_equals`, a clause with product groups to `quadratics`, and any
other to `constraints` (`../06_model_formulation/done.md` § 4). An ABS's envelope rows go to the ABS's record instead
(`AbsEnvelope`, `SolverInput::abs_links`).

The row starts from its clause: `InitializeEvaluatedConstraint` sets its `provenance` from the tree's constraint (the
`origin` with the repair group execution numbers, and the `kind`), `DescribeBound` sets the `shape` and the `rhs_label`
of its bound. The model builder copies that record into each row it states.

When the objective is quadratic, the same evaluation runs on `product_groups` **in
addition to** `terms`, not instead of: the linear terms are read as columns and summed per instance
(`ObjectiveCoefficients`), and `DecideModelEmitter::Objective` fills `objective_product_groups`
for the quadratic half. Each term's filter — its own
`when` with the objective's folded in — applies to each list independently.

`CoefficientColumn` keeps this compact: a column that is uniform across rows — a
broadcast Big-M coefficient, a constant RHS, a McCormick constant — stores one
scalar rather than `vector<double>(num_rows, K)`. Reads via `Get(row)` are
branchless on the storage kind, and mutation lazily promotes Scalar → Dense.

---

## 6. PHASE 3 — build and solve

Hands off to stage 06 ([`../06_model_formulation/done.md`](../06_model_formulation/done.md))
and stage 07 ([`../07_solver/done.md`](../07_solver/done.md)). The hand-off happens
early: `solver_input.constraints` is populated as soon as bounds are resolved, and
the linearization passes then work on it in place.

A MIN or MAX is a helper decision, and stage 06 emits its extremum link, not this stage. What stays here is the
evaluation the link needs: each helper's members per group, which travel to stage 06 as `ReducerValueLinkSpec`, and the
clause or objective that reads the helper's column.

### Assembly and formulation are separate passes

PHASE 3 is two passes, split by *what each one reads* rather than by where it sits
in the sequence.

`BuildSolverInput` **assembles**. It builds the `VarIndexer` and resolves the variable types and the
declared column box (the folded bounds, the `lower_bounds` sentinel, the rigid snapshot), then calls `PrepareModel`
(stage 06: the ABS envelopes, the floors a `when` lowers, then `DecidePropagateImpliedBounds`). The objective's AVG scaling
runs before it, where the objective is read (`DecideModelEmitter::Objective`). It runs
exactly once per query.

`FormulateModel` **formulates**, against a `FormulationBox` handed to it: the auto-`M`
refill for L0 links, the ABS / `<>` / bilinear linearizers, the helpers' definitions (every MIN/MAX's extremum link
among them), and finally `LowerDecideConstructs` for whatever the chosen backend cannot state natively. It touches no
`ClientContext`, scans no data, and evaluates no expression — it is a free function of `SolverInput`, a box and the
column space (`formulate_model.cpp`), and reads no operator field.

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
solution_idx = gstate.var_indexer.ColumnOrNone(decide_var_idx, global_row)
value        = ilp_solution[solution_idx]
```

A **per-row** decision (`DecideVarScope::ROW`) has its own column on each row. A
**keyed** decision (`per K: x`, `KEYED`) has one column per class of `K`: every row of
a class resolves to it and receives the same value, which is the point of the key. A
**query-wide** decision (`per (): x`, `GLOBAL`) is one column every row resolves to.

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

## 7b. `DIAGNOSE`: reporting on the run instead of returning it

Two things change when the statement carried the prefix (the tree's `diagnose`,
copied from `LogicalDecide::diagnose`):

1. **The engines are armed.** `FinalizeSolveResult` reads the flag once as
   `diagnosis_armed` and `SolveDecide` turns on the shared pre-solve work the engines need —
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
constructor, or under a `per` or `when` as the emitter writes the clause's rows), a model
class the host's solver refuses — still raises, with the precise
message that path already had. There is no solve outcome to report on.

The slow-solve path is deliberately outside all of this: see `slow_solves.md`.

## 7c. Arms no test reaches, kept (BLOAT-25)

Each is a guard the type system or a stage boundary needs, or a path only a nondeterministic run takes:

- **Typed reads and stage handoffs.** A NULL `Value` before `GetValue<double>` (`IsFiniteOnRow`); `ExtractDoubleColumn`
  without `null_rows` (the canonicalizer promises no NULL there); the backend the plan
  names by string (`PlannedSolverBackend`); a bound side that must be foldable (the optimizer asserts it); the solution
  index read from a backend (`GetData`); the sink state's `written_bound` fallback and its
  `bound.origin.source_clause_id < constraint_sources.size()` test (a recorded singleton-row bound indexes
  `constraint_sources` through the source clause in its clause's origin, which the binder writes on every written row and rewrites
  copy; a bound with no source names the variable and its value).
- **Loops.** The settling loop's pass bound, and the fixed-point comparison that must read every value it writes.
- **The OS.** `getrusage` failing, a process past 1 GiB, and Windows' zero peak memory.
- **Nondeterministic runs.** A Ctrl-C after the solver's last check; the "Keep searching?" prompt and its
  Enter-to-continue path (a pty, and a race the suite does not run); a `SUBOPTIMAL` stop (`DECIDB_STATUS_MARKERS`); a
  time-limit stop with an incumbent and no bound; a residual `INF_OR_UNBD`; an unbounded ray that names nothing; a
  repair solve that times out.

---

## 8. Source map

| Concern | Location |
|---|---|
| Everything above | `src/execution/operator/decide/physical_decide.cpp` |
| The one pass: the descent | `src/execution/operator/decide/decide_descent.cpp`, `src/include/duckdb/execution/operator/decide/decide_descent.hpp` |
| Operator fields | `src/include/duckdb/execution/operator/decide/physical_decide.hpp` |
| `DecideTerm`, `DecideConstraint`, `DecideObjective` | `src/include/duckdb/planner/decide/decide_tree_terms.hpp` |
| The pass that fills them | `src/optimizer/decide/decide_linear_form.cpp` |
| `SolverInput`, `EvaluatedConstraint`, `CoefficientColumn` | `src/include/duckdb/decidb/formulation/solver_input.hpp` |
| Logical → physical, key indices, input column names | `src/execution/physical_plan/plan_decide.cpp` |
| Binding resolution shielding | `src/execution/column_binding_resolver.cpp` |
| The `DIAGNOSE` operator (sink the rows, emit the findings) | `src/execution/operator/decide/physical_decide_diagnose.cpp` |
| `per`, `when` and `by` semantics; the scope test suites | [`../../language/scopes.md`](../../language/scopes.md), [`../../testing/scope_suite.md`](../../testing/scope_suite.md) |
| Its plan-generator case | `src/execution/physical_plan/plan_decide_diagnose.cpp` |
| Slow solves: report, continuation, Ctrl-C | [`slow_solves.md`](slow_solves.md) |
