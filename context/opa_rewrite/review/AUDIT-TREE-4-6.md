# Audit of TREE-4 … TREE-6 (independent reviewer, 2026-11-23, at d462b4ad31) — summary of its report

REMEDY, ranked by the reviewer:
1. Reducer has no KIND: ~25 sites compare `function->name` strings; AVG has two encodings (`avg`, SUM+`count_scale`);
   `agg_name` string and direct `DirectReducer` enum beside it. Norm still bound as SUM then function reset (null
   function = norm sentinel, ~18 guards; decide_binder.cpp:1187-1204). Form: `DecideReducerKind` enum set at build;
   NORM a kind bound as a scalar; delete `agg_name`, `DecideTreeReducerName`, `IsExtremumName(Lower())`.
2. Reducer `Equals`/`Hash` omit `by`, `per`, `frame`, `count_scale`, `retained` (bound_decide_reducer_expression.cpp:66,99):
   `sum(x)` equals `sum(x) by (g)`. Parallel protocol compensates (`FactsEqual`, `DecideFactsEqual`,
   `CanonicalTreesEqual`, `DecisionAliasesEqual`). Form: facts in Equals/Hash, delete the parallel one. Unknown whether a
   caller relies on loose equality → needs a full gate.
3. Frame is a reducer with a `frame` field; point frame still `first(e)` posing as aggregate; `TryGetDecideFrame` 28
   sites, `ContainsDecideFrame` 7; a frame node can carry by/per/norm/filter. Form: `BoundDecideFrameExpression
   {frame, payload}` (range's reducer already in `DecideFrameSpec::reducer`). OWNER CALL.
4. NULL test is a marker class inside the filter conjunction; 6 readers split it back out; also marks the guard around a
   recorded reducer's value (`InKey`) = BOUND_REDUCER tag under another name; 2 core executor cases + serialized class.
   Form: `member_tests` vector beside `filter` (as BoundWindowExpression keeps partitions apart); `ToAggregate` conjoins;
   `InKey` guard to the `DecideBoundAggregate` record.
5. Shape's second writer (see AUDIT-TREE-1-3 item 1). OWNER CALL on R26 wording.
6. `DecideHelperDefinition` is a union by comment; kind switches in `EnumerateExpressions`/`EnumerateBodies`. Form:
   subclasses with virtual methods. `helper_row` on every clause while `Root(clause, owner)` has the owner; L0 rows on
   the definition (`links`) but ABS/definition rows made at lowering (two paths). Form: each definition owns its rows.
7. Small: `RebuiltAlias` + alias compare in `DecideFactsEqual` vestigial; recorded reducers' query-wide derived from the
   record (removes `in_objective`); `signed_helper_vars`, `aux_var_expressions`, `KeyedScopeInfo::value_link` into the
   definition table; `retained` field + `retained_expressions` (tag as a field; not traced why needed) OWNER CALL;
   `written_name` on two core classes, hand-copied (OWNER CALL: text at location, or stop renaming).
KEEP: custom reducer serialization; `frame_aggregate`, `tightest_frame_bound`; PRODUCT/BOOLEAN_PRODUCT; `push`;
`definition` ref field on BoundFunctionExpression over a wrapper (fix its equality gap); subquery placeholder table.
Design edits recording a retreat: § 3.2.2 (kind removed; frame class → field; value node → table citing R25 against
its text), § 3.3 c4 (second writer), § 3.5 (retained home, NULL tests home, WRITTEN_NAME home).
