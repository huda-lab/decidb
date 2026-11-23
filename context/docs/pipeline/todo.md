# Pipeline — open architectural work

Changes that cross every stage. The plan and its rulings are in `context/opa_rewrite/design.md` and
`decisions.md`. How to work and the checks: [`../direct_solve/framework/todo.md`](../direct_solve/framework/todo.md)
§ How to work any task in these todo files.

## MODEL-2 — A G hands its members to its reader at the node

- **Problem.** A MIN or MAX over group values lists one member per input row (40 link rows for 8 distinct members;
  design note § 1.3), and stage 06 rewrites the helpers' members and lays the columns out a second time.
- **Change.** A G lists one member per distinct instance of its child. A G whose form is "members passed up" appends
  its groups' members to its reader's row or member list. A MIN or MAX group of one member is written as that member
  at the node (R44). `InlineReducerValueLinks`, `PlaceHelpers`, `HelperReads`, the second `VarIndexer::Build`,
  `DecideVarScope::INLINED` and the `VALUE_HELPER_INLINED` line for a sum are deleted with the tests that only
  reached them. `EXPLAIN` prints the form at planning. Each model golden that changes is inspected against
  `context/opa_rewrite/review/MODEL-1-form-differences.md`, and `DECIDB_FORM_DIFF` goes with the pass it compares.
- **Done when.** `./test/decide/run_tests.sh -k "anr or nested_decide or nested_product or explain"`;
  `build/release/test/unittest "[decidb]"`; `benchmark/decide/anr_scaling.py --only nested` states one link row per
  distinct member.

## MODEL-3 — Stage 06 reads a clause through its emitted rows

- **Problem.** The readers of an evaluated clause (`<>`, ABS, Big-M, implied bounds, absence, the builder) read
  `row_group_ids` and `row_coefficients`, arrays as long as the input, and regroup them.
- **Change.** `EvaluatedConstraint` holds one record per emitted row: its decision terms (`EmittedRows`), bound,
  name, class set and data row. Every stage 06 reader reads the record. The emitter still fills it from its row
  arrays, in one place. No model changes.
- **Needs.** MODEL-2.
- **Done when.** No file under `src/decidb/` names `row_group_ids` or `row_coefficients`; model goldens identical;
  `./test/decide/run_tests.sh -k "not_equal or abs or minmax or bigm or implied or diagnose"`;
  `build/release/test/unittest "[decidb]"`.

## MODEL-4 — An aggregate clause's root writes one record per group

- **Problem.** `DecideModelEmitter::Groups` allocates six vectors as long as the input per clause and a coefficient
  column per term (`repro/clauses_times_rows.sql`: 44 ms to 638 ms from 1 clause to 40).
- **Change.** A root E of an aggregate clause writes MODEL-3's record from its container, which holds the rows its
  `when` kept; an F's rows are written the same way. Evaluation stays vectorized (R23). `ClauseRows` and
  `ClauseEmittedRows`' regrouping are deleted.
- **Needs.** MODEL-3.
- **Done when.** Model goldens identical; `./test/decide/run_tests.sh -k "scope or anr or when or per"`;
  `context/opa_rewrite/repro/clauses_times_rows.sql` under `DECIDB_PROFILE=1`: `execution.tree_model` and peak
  memory at 40 clauses no higher than at `2b948c6b44`.

## MODEL-5 — A per-row clause's root writes one record per kept row

- **Problem.** A per-row clause emits a row per input row with the excluded ones marked.
- **Change.** A per-row root writes a record per kept row. `row_group_ids` and `row_coefficients` are deleted, with
  the row loops of `decide_value_extraction.cpp` no reader is left for.
- **Needs.** MODEL-4.
- **Done when.** No file under `src/` names `row_group_ids` or `row_coefficients`; model goldens identical;
  `./test/decide/run_tests.sh -k "per_row or bound or domain or diagnose"`; `build/release/test/unittest "[decidb]"`;
  `context/opa_rewrite/tools/ab_bench.py` against `2b948c6b44`.

## MODEL-6 — Terms are split from the finished nodes

- **Problem.** A reducer body is read twice at planning, once for its node and once for its coefficients, and four
  internal errors check that the readings agree (`context/opa_rewrite/review/TREE-25-sizing.md`).
- **Change.** First a prototype of `DecideTermSplitter`'s leaf for a child read, measured in lines and in what its
  nine other users carry. If the shared splitter is worse for it, the evidence goes to the owner (R46). Otherwise the
  term half reads lowered bodies: `LowerFrames` and its overloads, `RecordFallbackColumns`, one of
  `Translate`/`TranslateTree`, one of `FrameColumn`/`FrameRead` and the four agreement errors are deleted, and each
  order of the sizing note's § 4 that moves is inspected in its goldens.
- **Needs.** MODEL-5.
- **Done when.** `./test/decide/run_tests.sh -k "one_pass_tree or frame or anr or error"`;
  `build/release/test/unittest "[decidb]"`; lizard over the three construction files beside the figures of the
  Phase 5 hand-over.

## MODEL-7 — Phase 6 gate and docs

- **Change.** The side-by-side comparison of MODEL-1 is deleted. design.md § 4, stages 05, 06 and 08, guide § 6.4,
  `language/explain.md` and language D89 state the plan rule and the records.
- **Needs.** MODEL-6.
- **Done when.** `make decide-test`; `DECIDB_FORCE_SOLVER=highs make decide-test`;
  `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`; the debug build's suite; `unittest "[decidb]"` in both builds;
  `context/opa_rewrite/tools/ab_bench.py` and `repro/clauses_times_rows.sql` no slower in evaluation time or peak
  memory than `2b948c6b44`; `benchmark/decide/anr_scaling.py`; `make format-check`.
