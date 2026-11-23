# TREE-13 — form (sizing in progress)

## Read so far

- A frame binds to a `BoundDecideReducerExpression` whose `frame` indexes `BoundDecide::frames`
  (`decide_binder.cpp:952-972`, `:1117`). A range binds the overload of its reducer over the payload
  (`BindFrameAggregate(name, …)`); a point binds `first(payload)`.
- The bound overload of a range is NOT a copy of `DecideFrameSpec::reducer`. Execution runs it: a range over data
  is a REDUCED frame column whose `value` is the marker's `ToAggregate()` (`decide_linear_form.cpp:52-56,200-213`),
  folded by its state functions (`physical_decide_frames.cpp:305-360,431-438`). The tree's O also takes
  `function = ToAggregate()` and the arguments (`decide_tree.cpp:1028-1038`). The audit's sentence "the node's
  function is a second copy" holds for the reducer's NAME only.
- Two authorities today. `spec.reducer` (a string) is read for a range over decisions: `decide_rewrite_frames.cpp:147`
  (count), `:164` (the helper's `agg_name`), `decide_tree.cpp:1023` (`node.aggregate`).
  `PointsInQuadraticTermsAsRanges` (`decide_rewrite_frames.cpp:105`) sets `spec.reducer = "sum"` on a point and
  leaves the marker's function `first`. The node's function is read for a range over data.
- A REDUCER_VALUE helper holds its body as a reducer node (`DecideHelperDefinition::Reducer()`), frame helpers
  included (`decide_optimizer.cpp:436`, `decide_rewrite_bilinear.cpp:52`, `decide_tree.cpp:575,926`,
  `decide_linear_form.cpp:129`). With a frame class a frame helper's body is a frame node.
- Readers of a reducer's `frame` field: 13 files, 35 guard calls (`TryGetDecideFrame`, `ContainsDecideFrame`).

## The form

Pattern: a frame is a record in a table and a node names it by typed index (R25); the node is a reference, the
record holds the frame's facts (R28).

- `BoundDecideFrameExpression`: `frame` (index into `BoundDecide::frames`) and `payload` (its one child, so every
  visitor and rewrite of a clause body reaches it). Its return type is the payload's for a point and the fold's for
  a range. It holds no function, bind data, filter, `by`, `per`, norm or rewrite fact. The four duties of a class.
- `DecideFrameSpec::reduce`: a range's bound aggregate over reference 0 of the payload's type, an expression of the
  spec as `order_collation` is (generated serialization). Null for a point. Execution folds a window with it.
  `DecideFrameSpec::reducer` stays the reducer's name until TREE-10 gives it the kind's type.
- The binder binds no `first`. `PointsInQuadraticTermsAsRanges` edits the spec alone, as it does: a range over
  decisions is linked by its name and folded by no function.
- A reader that means "a reducer" tests the reducer class. `TryGetDecideFrame` and `ContainsDecideFrame` are class
  tests or go. A frame helper's definition holds a frame node as its body.
- Copies of one frame node cannot disagree: they hold an index and a payload.

Rejected: the node holds the range's bound aggregate (each copy of the node holds a function that must agree with
the others and with the spec's reducer).

## Steps

1. The class: header, `.cpp`, `ExpressionClass`/`ExpressionType` ids, `enum_util` by hand, `expression.json` +
   generated serialization, `ExpressionIterator::EnumerateChildren`, `LogicalOperatorVisitor::VisitExpression`.
   Template: commit `b46aec0d0d` (the NULL-test class).
2. `DecideFrameSpec::reduce` in `nodes.json`; the binder builds the node and the spec.
3. Readers, file by file (the 13 files of the guard list), then the tree's `Frame`, the linear form's frame columns
   (`FrameColumnValue`, REDUCED reads `spec.reduce`), the helper definition's body, execution's `ReduceRangeValues`.
4. Delete `frame` from the reducer class, the guards, the `first` binding; docs; design cells; todo entries.

## State of the work (uncommitted, in the tree)

Done: the class (`children`, a vector: a collated MIN/MAX range reads a value and its key), its ids, `enum_util`,
iterator, visitor, `expression.json`, `DecideFrameSpec::reduce` (`nodes.json` id 144), the binder (no `first`; a
range's reducer is bound, its children move to the node, its function to `spec.reduce` over references).
Next: `TryGetDecideFrame` tests the frame class; `frame` leaves the reducer class; fix what the compiler reports;
then every `Cast<BoundDecideReducerExpression>` a frame can reach (a release build does not check a cast: build
debug and run the frame tests there). Known: `RangeLifter::Lift` (`decide_rewrite_frames.cpp`, the marker and the
helper's body), `DecideHelperDefinition::Reducer()` for a frame helper (`decide_optimizer.cpp:436`,
`decide_rewrite_bilinear.cpp:52`, `decide_tree.cpp:575,926`, `decide_linear_form.cpp:129`), `decide_term_split.cpp:296`,
`decide_shape_check.cpp:136`, the tree's `Frame` (function and arguments from the marker), the linear form's
`FrameColumnValue` and REDUCED column (reads `spec.reduce`), `physical_decide_frames.cpp` `ReduceRangeValues`.
Sites that test "a reducer and not a frame" lose the frame half: `decide_linear_form.cpp:61`,
`decide_canonicalizer.cpp:78,313`, `decide_rewrite_scoped.cpp:120,359,370`, `direct_ir.cpp:306`.
