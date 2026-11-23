# Pipeline — open architectural work

Changes that cross every stage. The plan and its rulings are in `context/opa_rewrite/design.md` and
`decisions.md`. How to work and the checks: [`../direct_solve/framework/todo.md`](../direct_solve/framework/todo.md)
§ How to work any task in these todo files.

## MODEL-6 — One reading of a reducer body

- **Decision needed.** How far the planner's two readings of a clause are joined (R46).
- **Problem.** A clause is read twice at planning: the tree walk builds nodes (`decide_tree.cpp`) and the term half
  builds terms (`decide_linear_form.cpp` with `DecideTermSplitter`). The frame-column registry exists in both
  (`LowerFrames`, `RecordFallbackColumns`, `FrameAggregateRef` against `Translate`/`TranslateTree`,
  `FrameColumn`/`FrameRead`), with four agreement errors only a builder bug reaches. Term sites line up by
  construction (`decide_tree_compile.cpp:82-98`). Every shared reader (`DecideTermSplitter`, `DecideDegreeOf`,
  `ReadDecideProduct`, `ReadCanonicalAtoms`: 40 uses of `BoundExpressionReferencesDecide` in 14 files) knows a decision
  as a column of the DECIDE node; a lowered body names a child node by a `BoundReferenceExpression` past the input's
  columns, which is a helper decision, a data value or a frame column by the node's kind. A splitter over lowered
  bodies needs the tree, which its nine other users do not have.
- **Options.** (1) Full join as `context/opa_rewrite/review/TREE-25-sizing.md` sizes it: 400 to 600 lines changed,
  the shared splitter depends on the tree, like terms of two reducers stop merging (model rows change). (2) Keep two
  readings; the tree walk owns frame columns and the term half asks it for `(frame, kind)`: `LowerFrames` and its
  overloads, `RecordFallbackColumns` and the agreement errors go, the splitter is untouched. (3) Leave as is.
- **Recommendation.** 2. It removes the one duplicated registry and no shared reader changes.
- **Done when.** `./test/decide/run_tests.sh -k "one_pass_tree or frame or anr or error"`;
  `build/release/test/unittest "[decidb]"`.

## MODEL-8 — The helper of a link whose data may split its classes

- **Decision needed.** Whether a SUM read by one clause is written in where the data gives one row per group.
- **Problem.** `per g: sum(x) by (g) <= budget * open` (the facility-location link keyed by a column) makes a helper,
  since `budget` may differ within a class of `g`. The plan cannot know the data gives one row per group, so the
  helper keeps its column (`05_optimizer/done.md` § The form of a reducer helper): two columns and two rows more
  per group than the model written in, 98 ms against 37 ms to solve at 100 groups and 10.1 s against 6.7 s at
  100,000. Written in whenever the keys agree, a class the data does split holds the group's members once per row:
  7.8 million nonzeros against 15,820 on `sum(x) by (g) <= cap * y` over 8,000 rows.
- **Options.** (1) A second fact tested at the node, beside R44's: a group one row reads is written into that row,
  a group several rows read keeps its column. No model grows, the link is tight wherever the data allows; the form of
  a group depends on the data, and the helper is handed to stage 06 after its one reading clause is written.
  (2) The class key holds what a declared key determines: `per D.depotID:` holds `D.capacity` where `depotID` is a
  primary or unique key of `D`, as DuckDB lets `GROUP BY` a primary key select the table's other columns. The clause
  is then an aggregate clause with no helper, at planning; a table with no declared key keeps the helper.
  (3) Leave: the user writes `per D:` (the relation as the key), which holds every column of `D` already.
- **Recommendation.** 1, as R44 was accepted for the same reason; 2 is worth having on its own for `EXPLAIN`.
- **Done when.** `./test/decide/run_tests.sh -k "value_rewrite or anr or inlin"`;
  `build/release/test/unittest "[decidb]"`; the corpus query of `test/decide/golden/corpus.sql` line 717's
  neighbour (`per grp: sum(x) by (grp) <= budget / 10 * open`) has 6 columns and 3 rows.

## MODEL-10 — Code no reader reaches is deleted

- **Problem.** Nine items have no caller or reader (each confirmed by a search of `src` and `test/common`):
  `AuxRange::Span` and `MarkUnbounded` with the doc block around them (`ilp_linearization.hpp:339-364`);
  `CoefficientColumn::MakeDense`, `Empty`, `IsUniform`, `UniformValue`, `Negated` (`solver_input.hpp:66-101`);
  `SolverBackend::DisplayName` and `display_name` (`solver_registry.hpp`, `.cpp`); `TransformToChunkExpression`;
  `DecideTreeBuilder::Scope(idx_t, const DecideClauseScope &)` (`decide_tree.cpp:532`);
  `DecideConstraint::admitted_member` (written, never read); `DecideDescent::decision_class_layouts` (pushed to,
  never read); `FormulationBox::global_lower` and `global_upper`; `MinMaxLinkRow::AddConstant`. Seven parameters are
  passed and unused: `BuildSolverInput`'s `evaluated`; `var_names` through `LinearizeNotEqual` and
  `ExpandAggregateNotEqual`; `ReadBesideReducers`' `site`; `DecideProgramCompiler::Translate`'s `body` with the
  `owner_body` computed for it; `DecideDescent::ReadKey`'s `rows`; `RewriteForSolver`'s `root`; `DecideKeptGroups`'
  `loose_row_filter`, whose one caller passes a filter that keeps every row.
- **Change.** Delete each with the comments that name it. `SolverInput::global_bounds_unbounded` is read only by an
  assertion: keep it or delete the assertion with it, whichever the assertion's value decides.
- **Done when.** The release build; `build/release/test/unittest "[decidb]"`;
  `./test/decide/run_tests.sh -k "scope or frame or not_equal or abs"`; model goldens identical.

## MODEL-11 — The tree is verified in debug builds, once

- **Problem.** `DecideTree::Validate` (`decide_tree.cpp:1176-1318`, complexity 75, 18 internal errors) checks in
  every release query what the builder above it built; no test reaches a message of it. The emitter repeats three of
  its checks (`decide_model_emitter.cpp:118`, `589-595`, `676-682`). The canonical form's verifier runs under
  `#ifdef DEBUG` (`VerifyCanonical`).
- **Change.** `Validate` runs in debug builds, as `VerifyCanonical` does; a check a type or the builder's own
  control flow makes impossible is deleted, in `Validate` and in the emitter.
- **Done when.** `./test/decide/run_tests.sh -k "one_pass_tree or scope or abs"`;
  `build/release/test/unittest "[decidb]"`; lizard over `decide_tree.cpp` beside 75.

## MODEL-12 — The model's constraints have one struct each, and one mapping from a comparison to a sense

- **Problem.** `SolverModel::GeneralConstraint` holds the four fields of `GeneralConstraintSpec`,
  `SolverModel::IndicatorConstraint` is `IndicatorConstraintSpec` with its `RawConstraint` flattened, and the builder
  copies field by field between them (`ilp_model_builder.cpp:644-655`, `879-901`). The mapping from a comparison to a
  row's sense, with the strict step and the typed bound a diagnosis quotes, is written in `ApplyComparisonSense`
  (`:676-699`) and again in `BuildQuadraticConstraint` (`:833-855`).
- **Change.** One type per constraint kind, moved from the input to the model; one function maps a comparison.
- **Done when.** `build/release/test/unittest "[decidb]"`; `./test/decide/run_tests.sh -k "quadratic or native or
  indicator or diagnose"`; model goldens identical.

## MODEL-13 — A term names the instance it reads

- **Problem.** A term between execution and stage 06 is `(decision, data row, coefficient)` (`ClauseRecords`,
  `ReducerValueMembers`, `ObjectiveMembers`), and stage 06 finds its column by looking the row up
  (`VarIndexer::Get(var, row)`: 16 uses, `InstanceOf`: 12, `ColumnOrNone`: 3, in four files). Every decision that is
  not per row therefore carries an array as long as the input (`KeyedMapping::row_to_class`), one per helper
  (`HelperMapping`), copied by `VarIndexer::Build`: 40 clauses that each read a helper over 2,000,000 rows peak at
  2.41 GB against 1.09 GB for one, with 185 ms against 26 ms in `execution.tree_model` and 52 ms in the layout. The
  node that writes a term holds the instance: its container's slot.
- **Change.** A term is `(decision, instance, coefficient)`: the row for a per-row decision, the class for a keyed
  one, 0 for a query-wide one. The emitter writes it; stage 06 reads the column by `InstanceColumn`. A record keeps
  its first data row for a diagnosis (`ClauseRecords::data_rows`). `WriteMembersIn` reads the instance off the term.
- **Done when.** No reader of a term in `src/decidb` calls `Get(var, row)`; model goldens identical;
  `./test/decide/run_tests.sh -k "scope or anr or per or frame or abs or not_equal or diagnose"`;
  `build/release/test/unittest "[decidb]"`.

## MODEL-14 — A helper has no array as long as the input

- **Problem.** See MODEL-13: with terms naming instances, a helper's `row_to_class` has no reader in stage 06, and a
  helper is never an output column.
- **Change.** A helper's scope holds its number of instances and no row map; only a declared keyed decision keeps
  `row_to_class`, for the output. `VarIndexer` reads the mappings it is given and copies none. A frame read's base
  instance is resolved where its term is written, if every reader of `SolverInput::frame_reads` can then go
  (`DecideVarScope::FRAME_READ`, `ResolveRead`: 22 uses in stage 06); say in the task's commit which.
- **Needs.** MODEL-13.
- **Done when.** `context/opa_rewrite/repro/clauses_times_rows.sql` with each clause written
  `when g = k: 2 * x <= sum(w * x) + 1`, 40 clauses over 2,000,000 rows: peak memory and `execution.tree_model`
  within a tenth of the 40 plain clauses'; `./test/decide/run_tests.sh -k "anr or frame or scope or per"`; model
  goldens identical.

## MODEL-15 — The objective is a coefficient per instance

- **Problem.** The objective reaches stage 06 in three forms: a column over every input row per term with
  `objective_variable_indices`, `ObjectiveMembers` triples for its terms on a written-in helper, and product groups
  as columns. The builder reads both linear forms (`ilp_model_builder.cpp:549-579`), and
  `ReportAnEmptyObjective` both. The objective is one row, so its linear part is one coefficient per column.
- **Change.** The emitter sums the objective's linear terms into one coefficient per instance of each decision it
  reads, and the constant; stage 06 copies them to their columns. `objective_coefficients`,
  `objective_variable_indices` and `ObjectiveMembers` go.
- **Needs.** MODEL-13.
- **Done when.** `./test/decide/run_tests.sh -k "objective or anr or avg or minmax"`;
  `build/release/test/unittest "[decidb]"`; model goldens identical; a per-row objective over 2,000,000 rows peaks no
  higher than before.

## MODEL-16 — Stage 06's bound passes walk instances, and absence is looked up once per decision

- **Problem.** `DecidePropagateImpliedBounds` loops over every input row for each decision of each `<=` or `=`
  clause (`linearization_bigm.cpp:253`): clauses times rows in stage 06. `DecideAbsence::At` walks every helper
  definition on each call (`decide_absence.cpp:41-47`) and is called per row per term
  (`decide_model_emitter.cpp:766`, `1107`, `1143`, `1440`, `1622`).
- **Change.** The bound pass walks the instances a clause's rows name. What makes a decision absent at a row is
  resolved once per decision before its terms are read.
- **Needs.** MODEL-13.
- **Done when.** `./test/decide/run_tests.sh -k "bound or absent or null or bigm"`; model goldens identical;
  `repro/clauses_times_rows.sql` under `DECIDB_PROFILE=1`: `assembly.implied_bounds` and `execution.tree_model` no
  higher than `08_execution/done.md` § A clause is read over the rows it keeps.

## MODEL-17 — The emitter reads a term in one place

- **Problem.** "Read a term's program at its site, project it onto the reader's rows, mark NULLs and absence" is
  written seven times, each with its own index space and NULL rule: `Rows` (`decide_model_emitter.cpp:734`),
  `ReadBesideReducers`, `ReadInReducer`, `ReadPart`, `ObjectiveTerms`, `FrameMembers`, `ValueLinks`. Product groups
  are read once for a clause (`Polynomial`, `ReadPart`) and again for the objective (inline in `Objective`). Three
  index spaces (a container's entries, places among kept rows, data rows) are joined ad hoc.
- **Change.** One type for the rows a reader holds (place, data row, entry) and one reader that returns a term's
  values and NULL mask in it; the seven sites call it. One reader of product groups serves a clause and the
  objective.
- **Needs.** MODEL-15.
- **Done when.** Model goldens identical; `./test/decide/run_tests.sh -k "scope or anr or when or per or quadratic or
  avg or null or absent"`; lizard over `decide_model_emitter.cpp`: `Rows`, `Groups` and `ValueLinks` beside 32, 22
  and 30.

## MODEL-18 — A clause's working columns are apart from its result, and the emitter fills the solver input

- **Problem.** `EvaluatedClauseColumns` inherits the stage 06 `EvaluatedConstraint` and is sliced in `Route`, which
  still reads the working bound column for a `<>` (`decide_model_emitter.cpp:638-668`). An aggregate clause's rows
  are dropped by about eight steps that each overwrite `row_group_ids` in an order stated only in comments
  (`:1279-1330`). Fifteen fields of `DecideGlobalSinkState` mirror `SolverInput` and are moved one by one in
  `BuildSolverInput` (`physical_decide.cpp:509-511`, `597-614`, `658-663`).
  `EvaluatedConstraint::variable_indices` means each term's decision before a helper's members are written in and
  the distinct decisions after, with `members_written_in` to tell them apart; `emitted_rows` is a cache stage 06
  keeps inside the record it is handed.
- **Change.** A builder owns a clause's working columns and returns a plain `EvaluatedConstraint` (GoF Builder). Each
  rule that drops an instance contributes a named mask, combined once. The emitter writes into the `SolverInput` the
  sink state owns. A record states the decisions its rows name, once; stage 06 holds its own cache of emitted rows.
- **Needs.** MODEL-17.
- **Done when.** Model goldens identical; `./test/decide/run_tests.sh -k "scope or when or per or not_equal or absent
  or null or diagnose"`; `build/release/test/unittest "[decidb]"`.

## MODEL-19 — Stage 06 has one driver, and the AVG under `<>` is the optimizer's choice

- **Problem.** `PhysicalDecide::BuildSolverInput` runs stage 06 passes in an order that matters
  (`physical_decide.cpp:613-655`: the ABS envelopes, the rigid box, `ResolveLiftedFloors`,
  `FloorBilinearAuxiliaries`, `DeriveReducerValueBounds`, `DecidePropagateImpliedBounds`), and `FormulateModel` runs
  `DeriveReducerValueBounds` again: a construct is wired into two files of two layers.
  `HoistAvgIntoNotEqualBound` (`decide_value_extraction.cpp`) chooses how an AVG under `<>` is encoded at run time
  from plan facts alone (R42).
- **Change.** One entry point in `src/decidb/formulation` for what needs no box, called before `FormulateModel`;
  `PhysicalDecide` calls the pair. The optimizer stamps the AVG encoding on the clause.
- **Done when.** `./test/decide/run_tests.sh -k "bound or floor or abs or not_equal or avg or diagnose"`;
  `build/release/test/unittest "[decidb]"`; model goldens identical.

## MODEL-20 — The helper-form rule names the readers that take members

- **Problem.** `ChooseHelperForms` writes a helper in unless a listed reader holds its column
  (`decide_helper_form.cpp:43-46`, `154`, `161`, `199-220`; complexity 71 in one function): a reader the list misses
  leaves a term naming an instance with no column, an internal error for a keyed helper and column -1 for a
  query-wide one (`ilp_model.hpp:85`). A rewrite account is matched to its helper by label text (`:262-265`,
  `physical_decide.cpp:448-451`), so two helpers that print alike share accounts.
- **Change.** The rule lists the readers that take a linear expression (a linear row of a clause, the linear
  objective, one helper's member); any other holds the column. The pass is split into the reading of readers and the
  choice. An account names its helper's definition. A debug check in stage 06 that no term names an instance with no
  column.
- **Done when.** `build/release/test/unittest "[decidb]"` (`test_decidb_helper_form.cpp`);
  `./test/decide/run_tests.sh -k "anr or nested or inlin or explain"`; lizard over `decide_helper_form.cpp` beside
  71.

## MODEL-21 — A clause's and the objective's terms are extracted by one function

- **Problem.** `decide_linear_form.cpp` reads a clause and the objective through parallel functions that each fill a
  list of terms and a list of product groups: `ExtractAggregateConstraintTerms` and
  `ExtractAggregateObjectiveTerms` (`:749-781`, `:884-924`), `ExtractReducerConstraintTerms` and
  `ExtractReducerObjectiveTerms`, `ExtractConstraintTerms` and `ExtractObjectiveBodyTerms`, with the same internal
  error twice (`:785`, `:932`) and `ThrowOpaqueTerm` at four split sites.
- **Change.** One extraction over a pair of terms and product groups, which a clause and the objective both hold.
- **Done when.** `./test/decide/run_tests.sh -k "objective or scope or quadratic or avg"`;
  `build/release/test/unittest "[decidb]"`; model goldens identical.

## MODEL-7 — Phase 6 gate and docs

- **Change.** design.md § 4 and stages 05, 06 and 08 state the system as it is. `language/guide.md` states that an
  exact ABS or a MIN or MAX whose Big-M the data do not bound is solved natively on Gurobi and refused on HiGHS
  (`NativeConstructPolicy::Use`, `formulate_model.cpp:157`).
- **Needs.** Every task above that is not a decision.
- **Done when.** `make decide-test`; `DECIDB_FORCE_SOLVER=highs make decide-test`;
  `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`; the debug build's suite; `unittest "[decidb]"` in both builds;
  `repro/clauses_times_rows.sql` and its helper form (MODEL-14) no slower in evaluation time or peak memory than
  `08_execution/done.md` states; `make format-check`.
