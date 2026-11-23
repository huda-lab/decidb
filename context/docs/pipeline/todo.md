# Pipeline — open architectural work

Changes that cross every stage. The plan and its rulings are in `context/opa_rewrite/design.md` and
`decisions.md`. How to work and the checks: [`../direct_solve/framework/todo.md`](../direct_solve/framework/todo.md)
§ How to work any task in these todo files.

## MODEL-4 — An aggregate root writes its records with no column between, and a helper read is written as its members

- **Problem.** An aggregate clause is read into columns over its kept rows (`EvaluatedClauseColumns`), sorted into
  groups (`DecideClauseRowSets`) and copied into records (`DecideWriteClauseRecords`); a per-row clause writes its
  records directly. A helper the plan writes in is declared, given a column, and removed again in stage 06
  (`WriteHelpersIn`), which lays the columns out a second time.
- **Change.** A root E of an aggregate clause writes one record per group from its container, as `Rows` does per
  kept row; `EvaluatedClauseColumns`, `DecideClauseRowSets` and `DecideWriteClauseRecords` are deleted with the row
  loops of `decide_value_extraction.cpp` no reader is left for. A quadratic clause's product groups become terms of
  its records, so `QuadraticClause::rows` and `data_rows` go. A read of a helper whose form is `WRITTEN_IN`, or of a
  MIN or MAX group of one member, is written as the members (R43, R44), so `WriteHelpersIn`,
  `SolverInput::inlined_helpers` and `written_in`, `DecideVarScope::INLINED` and the second `VarIndexer::Build` are
  deleted with the tests that only reached them. The floors and implied bounds that read a written-in helper's
  column before it is removed (`PhysicalDecide`, `assembly.inline_helpers`) need a reading over its members first.
  Evaluation stays vectorized (R23).
- **Done when.** No file under `src/` names `row_group_ids` or `row_coefficients`; model goldens identical;
  `./test/decide/run_tests.sh -k "scope or anr or when or per or bound or domain or diagnose"`;
  `build/release/test/unittest "[decidb]"`; `context/opa_rewrite/repro/clauses_times_rows.sql` under
  `DECIDB_PROFILE=1` no slower and no larger than the figures of `08_execution/done.md` § A clause is read over the
  rows it keeps; `y <= v` alone over 1,000,000 rows no slower than 85 ms across `execution.tree_model`, implied
  bounds and the builder's clause rows.

## MODEL-6 — Terms are split from the finished nodes

- **Problem.** A reducer body is read twice at planning, once for its node and once for its coefficients, and four
  internal errors check that the readings agree (`context/opa_rewrite/review/TREE-25-sizing.md`).
- **Change.** First a prototype of `DecideTermSplitter`'s leaf for a child read, measured in lines and in what its
  nine other users carry. If the shared splitter is worse for it, the evidence goes to the owner (R46). Otherwise the
  term half reads lowered bodies: `LowerFrames` and its overloads, `RecordFallbackColumns`, one of
  `Translate`/`TranslateTree`, one of `FrameColumn`/`FrameRead` and the four agreement errors are deleted, and each
  order of the sizing note's § 4 that moves is inspected in its goldens.
- **Done when.** `./test/decide/run_tests.sh -k "one_pass_tree or frame or anr or error"`;
  `build/release/test/unittest "[decidb]"`; lizard over the three construction files beside the figures of the
  Phase 5 hand-over.

## MODEL-7 — Phase 6 gate and docs

- **Change.** design.md § 4 and stages 06 and 08 state the records.
- **Needs.** MODEL-6.
- **Done when.** `make decide-test`; `DECIDB_FORCE_SOLVER=highs make decide-test`;
  `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`; the debug build's suite; `unittest "[decidb]"` in both builds;
  `context/opa_rewrite/tools/ab_bench.py` and `repro/clauses_times_rows.sql` no slower in evaluation time or peak
  memory than `2b948c6b44`; `benchmark/decide/anr_scaling.py`; `make format-check`.
