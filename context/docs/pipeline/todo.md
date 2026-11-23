# Pipeline — open architectural work

Changes that cross every stage. The plan and its rulings are in `context/opa_rewrite/design.md` and
`decisions.md`. How to work and the checks: [`../direct_solve/framework/todo.md`](../direct_solve/framework/todo.md)
§ How to work any task in these todo files.

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
