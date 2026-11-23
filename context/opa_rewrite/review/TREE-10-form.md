# TREE-10 form note (sized at c541e8da0a, no code changed)

Status: **stopped before any code edit.** One encoding of AVG cannot be had without moving a plan text or leaving a
node whose kind and function disagree (§ 3). Two smaller points need a word too (§ 6). Everything else in the task's
stated form is the better form and is sized below, ready to build once § 3 is ruled.

## 1. The enum and its writer

`enum class DecideReducerKind : uint8_t { SUM, AVG, MIN, MAX, COUNT, NORM }`, a field `kind` of
`BoundDecideReducerExpression`, a constructor argument (no default, no setter used after construction).

What the binder can produce today, by the function it binds: `sum`, `avg`, `min`, `max` (a collated text MIN/MAX
binds to `arg_min`/`arg_max` with two children — the function's name is already not the kind), `count`, `count_star`
(both COUNT; `children.empty()` tells them apart), a norm (no function), and `first` for a point frame (§ 6a).

Writers: the nine places a node is built, each of which knows the kind without reading a name back —
`DecideBinder::BindAggregate` (from the catalog entry it resolved: the one name-to-kind switch, front-end work),
`BindFrameAggregate`, the norm branch of `BindFunction`, `DecideCanonicalizer::BindReducer`/`RebindReducer` (callers
pass the kind in place of the name string), `BindSumOver` and the two `sum` sites of `decide_rewrite_scoped.cpp`
(SUM), `decide_rewrite_rhs_bounds.cpp:314,430` (the kind of the reducer they rebuild). `Copy` and `Deserialize` carry
it. `Equals`/`Hash` compare it (it replaces the `function ? … : norm` branch).

`function` stays: it is the bound overload `ToAggregate` hands DuckDB, null for a NORM. Readers stop testing it for
null; only `ToAggregate`, `Copy`, `Serialize`, `PropagatesNullValues` touch it.

Serialization (hand-written, kept by the audit): field 202 `has_function` becomes `kind`; the function is written
when the kind is not NORM. No `.json` change for the node.

## 2. Readers after the change

| Family | Sites today | Reads after |
|---|---|---|
| Name comparisons on a reducer (`function->name` vs `"sum"`/`"avg"`/`"min"`/`"max"`/`"count"`) | 25: `decide_rewrite_scoped.cpp` 91, 200, 284, 294, 359, 368, 369, 520; `decide_linear_form.cpp` 788, 931; `decide_tree_definition.cpp` 82, 93, 150; `decide_rewrite_minmax.cpp` 69, 103; `decide_rewrite_rhs_bounds.cpp` 361; `decide_rewrite_frames.cpp` 80; `decide_rewrite_notequal_avg.cpp` 104; `decide_canonicalizer.cpp` 799, 1364; `decide_shape_check.cpp` 206, 241; `decide_tree.cpp` 31; `direct/direct_ir.cpp` 317; two tests | `reducer.kind` |
| Null-`function` guards (norm marker) | 18: six in the class (`ToString`, `Hash`, `Equals`, `Copy`, `Serialize`, `PropagatesNullValues`), `IsDecideNormMarker` ×2 with 6 callers, `decide_canonicalizer.cpp` 504, 796, `decide_source_provenance.cpp` 329, `decide_shape_check.cpp` 237, `direct_ir.cpp` 311, `decide_rewrite_frames.cpp` 80, `decide_rewrite_scoped.cpp` 359, `decide_rewrite_norm_in.cpp` 56 | `kind == NORM` |
| Helper definitions (`agg_name` string: `logical_decide.hpp:329`; written at `decide_rewrite_scoped.cpp:259`, `decide_rewrite_frames.cpp:164`; 9 readers) | | `Reducer().kind`; `agg_name` deleted (see § 6b for a frame's helper) |
| The executor tree (`DecideNode::aggregate` string; written at `decide_tree.cpp` 868, 942, 982, 1023; read at `decide_model_emitter.cpp` 1548, 1566, 1610, `physical_decide_frames.cpp` 753–754, the debug dump) | | the string becomes the enum. The node cannot read it elsewhere: it holds a `BoundAggregateExpression`, not the reducer, and a frame's node has neither. `decide_tree_columns.cpp:37` and `physical_decide_frames.cpp:317` (`function.name != "count"`) read it too |
| `ReducerValueLinkSpec::agg_name` (`solver_input.hpp:454`; 4 readers in `linearization_reducer_value.cpp`, `physical_decide.cpp:663`) | | the enum, copied from the tree node as today |
| Direct path (`DirectReducer {NONE, SUM, AVG, MIN, MAX}`, 12 uses) | | `DirectPart` holds `DecideReducerKind` and a row-level part is told apart by a flag or an optional; the five-member enum and the name ladder at `direct_ir.cpp:317-329` go. (TREE-8 deletes `DirectIR` whole; this is the smallest step that leaves one enum.) |

Deleted: `agg_name` (2 fields, 3 writers, 13 readers, 5 test writes), `DecideTreeReducerName` (1 + 2 uses),
`IsDecideNormMarker` (2 overloads, 9 callers), `IsExtremumName` (1 + 8 uses), `DirectReducer`, the name ladder in
`direct_ir.cpp`, `WrittenName()`'s fall-back to `function->name` (it prints the kind's name).

## 3. AVG — the finding, and why I stopped

**Why two encodings exist.** The binder makes `avg` (function `avg`). `DecideOptimizer::RewriteAvgInExpression`
(`decide_rewrite_notequal_avg.cpp:94-124`) replaces every AVG over decisions in a clause or objective body by a
freshly bound `SUM` of the same body with `count_scale = true`, and records the `AVG_SUM` account. It skips an AVG
over data, an AVG inside a helper DEFINITION body, and the inner AVG of a nest (which the scoped rewrite turns into a
helper with `agg_name == "avg"`).

**Who reads which after the pass.** The linear form reads `count_scale` (`decide_linear_form.cpp:600,613`) and throws
on anything not named `sum` (788, 931). The `avg` name is still read by the scoped rewrite (294, 368: the inner
reducer of a nest), `decide_tree_definition.cpp:82`, the frames walk (80), `direct_ir.cpp:321` and the shape check
(206, before the pass). Two readers accept both: `decide_canonicalizer.cpp:1364` (`avg` name **or** `count_scale`;
it runs before the pass and again when the clause is re-canonicalized after it) and `DecideTreeReducerName`
(`count_scale ? "avg" : name`), so the executor tree already holds one encoding.

**What blocks one encoding.** The swapped function is what `EXPLAIN` prints. The rewritten layer of a clause is the
post-optimizer tree rendered, and the node prints its function's name:

```
  avg(x*w) <= 2
    ≡ avg(w * x) <= 2
    ↳ avg(w * x) becomes a sum divided by its row count
    ↳ sum(w * x) <= 2          <- the node after the pass, printed
```

`context/docs/language/explain.md:17-20` documents that last line. So:

- **B — one encoding, a plan line goes.** Kind AVG from binder to lowering; the pass only records the account;
  `count_scale`, the rebinding through `BindSumOver`, and the `CopyFacts` hand-over there go; the linear form scales
  the terms of a kind-AVG reducer. The node then prints `avg(w * x) <= 2`, equal to the layer above, so the line
  `↳ sum(w * x) <= 2` is left out of every plan with an AVG over decisions. This is the form the task asks for and
  the cleanest by R27. It is what a clause written `mean(...)` already prints today: `CopyFacts` carries
  `written_name = "mean"` onto the SUM, the rewritten layer reads `mean(...)` and is left out (checked on the
  release build). So the line is not consistently owed even now. Unknown until a gate runs: the node keeps `avg`'s
  overload and argument cast in place of `sum`'s; no value should move, but only tests prove it.
- **A — no text moves, two encodings stay.** Kind AVG before the pass, kind SUM plus `count_scale` after, as today
  in enum form. The two readers that accept both keep an `||`. Everything else in TREE-10 lands. Not "one encoding".
- **C — no text moves, one kind, kind and function disagree.** Kind AVG stays, `count_scale` goes, the pass still
  swaps `function` to SUM's so the node prints `sum`. A node whose kind says AVG while `ToAggregate` yields a SUM is
  worse than either: not offered as a recommendation.

Recommendation: **B**, with the `explain.md` example and any test that pins `↳ sum(...)` after an AVG edited in the
same change. If the plan line must stay, **A**, and TREE-10's "one encoding of AVG" is dropped with the reason
recorded.

## 4. The norm binding

Today: the body is wrapped in the system `sum`, bound through `BindAggregate` (which also runs the row-invariant
check and gives the body `sum`'s argument cast), then `function`/`bind_info` are reset and, unless it is an L0 norm of
a decision, the return type is replaced by `DecideNormDefinition(...)->return_type`.

After: the norm branch binds `children[0]` with `BindChild`, refuses a body that is not a number, runs the same
row-invariant check, and builds the node with kind NORM, no function, the body as its one child, and the return type
of `DecideNormDefinition` over a reference of the body's type — for every order, the L0 norm of a decision included
(its value is a count: the type of the L0 definition, where today it is `sum(body)`'s type).

Text that may move (the one place allowed): a norm whose body `sum` has no overload for — a `VARCHAR` or other
non-numeric body — raises DuckDB's "No function matches the given name and argument types 'sum(VARCHAR)'…" binder
error today, and after the change a `BinderException` at the body saying it is not a number. Exact before/after
texts to be listed from the built binary. Unsure: whether the row-invariant message, which today quotes the rewritten
call, reads the same; it is built from the written text, so it should.

## 5. Docs in the same change

`design.md` § 3.2.2 reducer row (and the § 3.5 cell that names `agg_name`), the stage `done.md` paragraph on
`BoundDecideReducerExpression`, `explain.md` under B; § TREE-10 leaves `context/docs/pipeline/todo.md` and its line
leaves `context/docs/todo.md`.

## 6. Unsure — needs a word before or with the ruling

a. **The point frame's `first`.** `at(F: e)` binds a reducer node with `frame` set and function `first`. It needs
   some value in a non-optional `kind` field. TREE-13 says the frame stops being a reducer and the binder makes no
   `first`; until then a seventh member is unavoidable unless frames are excluded another way. Two readers reach a
   frame marker's name without a frame guard first (`decide_rewrite_frames.cpp:80`, `decide_shape_check.cpp:241`
   after its own frame test), so the value must be none of SUM/AVG/MIN/MAX. Proposal: a member that states it is the
   point frame's holder and that TREE-13 deletes; I did not want to add it unasked.
b. **A frame's reducer is held twice already** (`DecideFrameSpec::reducer`, a serialized string in `nodes.json`, and
   the marker's function), and they can differ: `PointsInQuadraticTermsAsRanges` turns a point into a range by
   setting `spec.reducer = "sum"` while the marker keeps `first`. A frame helper's `agg_name` is copied from the spec
   for that reason. With `agg_name` gone, a frame helper reads the spec, every other helper its reducer. Making
   `DecideFrameSpec::reducer` the enum (a `nodes.json` edit, `enum_util` entries by hand, a point's value unused)
   removes the last string compare (`== "count"`, `== "avg"` in the executor); leaving it a string until TREE-13
   keeps one string-to-kind conversion in the lowering walk. I would convert it here.
c. `DecideNode::aggregate` stays a field of the executor tree, as an enum: the node has nothing else to read it
   from. The task's "do not add a kind to `DecideNode`" is met in the sense that none is added; the string that is
   there changes type.
