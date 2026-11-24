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
