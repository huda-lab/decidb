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

## MODEL-24 — The owner of a clause's emitted rows

- **Decision needed.** Where the rows a clause emits are kept between the passes that read them.
- **Problem.** `ClauseEmittedRows` sums a clause's terms per solver column once and keeps the result on the record
  (`EvaluatedConstraint::emitted_rows`, `mutable`, dropped by a copy). Three passes read it: implied-bound
  propagation (`PrepareModel`), the aggregate `<>` expansion (`FormulateModel`) and `SolverModel::Build`, which
  takes it. A pass that edits a record after the first read leaves the kept rows stale; the one that does
  (`FormulateModel` sizing a `norm(e, 0)` row) edits rows the first reader skips.
- **Options.** (1) A stage 06 object holds the rows by clause, passed to `PrepareModel`, `FormulateModel`,
  `SolveDecide` and `Build`: four signatures and the diagnosis copy carry it, and a key that survives
  `input.constraints` growing. (2) Each reader sums afresh: no kept state, the per-column sum runs twice for every
  `<=` and `=` clause. (3) Leave the rows on the record.
- **Recommendation.** 3. The rows are a function of the record and the column space, both fixed before the first
  read; 1 moves the same state behind more plumbing and 2 pays time for no reader's gain.
- **Done when.** Model goldens identical; `./test/decide/run_tests.sh -k "bound or not_equal or diagnose"`.

## MODEL-7 — The model-writing time of the clauses benchmark

- **Decision needed.** Whether the objective's coefficients keep their two passes over a per-row decision.
- **Problem.** `context/opa_rewrite/repro/clauses_times_rows.sql` spends 30 ms in `execution.tree_model` for 40
  clauses over 2,000,000 rows and 27 ms for one clause, against 25 ms and 23 ms with one pass; `model.linear_objective`
  takes 6 ms against 4 ms. The query takes about 850 ms and peak memory is equal (1.03 GB). The cause is the
  objective's hand-over (`DecideModelEmitter::ObjectiveCoefficients`): the emitter sums each term's column into a
  coefficient per instance (`SolverInput::objective_coefficients`), and stage 06 copies that into the model's
  columns. A per-row decision's 2,000,000 coefficients are read twice and held in one more 16 MB array.
- **Options.** (1) Leave it: every decision's objective coefficients reach stage 06 in one form, and stage 06 reads
  no data row. (2) Hand a per-row decision's lone dense column over as it is: one pass, and a second form of the
  hand-over that stage 06 and `ReportAnEmptyObjective` both read.
- **Recommendation.** 1: the cost is 5 ms in 850 ms, and option 2 adds a second route for one shape.
- **Done when.** The owner rules; under option 2, `DECIDB_PROFILE=1` on the benchmark reads 25 ms.
