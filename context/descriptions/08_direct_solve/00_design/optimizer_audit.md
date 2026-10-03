# S1 result-boundary optimizer audit

The direct rewrite runs inside `DECIDE_OPTIMIZER`, after the first filter
pushdown and before the passes below (`src/optimizer/optimizer.cpp`). The
logical result extension advertises the original DECIDE bindings and types.
Its dependency expressions initially reference every mapped output slot and
the hidden rank. Unused-output removal can replace an output dependency
only when source evaluation is proved safe to skip; the rank dependency stays.
The shared map checks output slots and types before the
original DECIDE node is replaced; physical lowering resolves the child once and
projects the mapped slots in original order.

| Later pass | Boundary behavior checked in source | Behavioral evidence |
|---|---|---|
| CTE filter pusher and filter pushdown | A filter may move into a materialized CTE definition, but `FilterPushdown::Rewrite` handles the extension through `FinishPushdown`, which leaves pending filters above it and optimizes its child independently. | `test_materialized_result_filter_keeps_global_input_and_late_guard` checks a selected row and a late invalid score. |
| Join order | Relation extraction passes unary operators until the generated result projection. It treats that projection as a relation and optimizes its child separately, so outer joins do not become joins inside the rank input. | `test_parent_join_filter_does_not_shrink_decide_input` checks both the global winner and a late invalid row under an outer join. |
| Unused columns and both column-lifetime runs | The extension's hook replaces a certified unused output dependency with a typed NULL before generic visitation. Unknown or potentially observable outputs remain live. The mandatory rank dependency survives even when the parent reads only `COUNT(*)`; the lifetime pass has no extension-specific projection-map rewrite. | Late-score and computed-error tests cover unused `x`, `COUNT(*)`, stored scans, and an inner-join passthrough; normal and serializer suites pass. |
| Limit pushdown and TopN | Limit pushdown swaps only an immediate projection child; TopN recognizes a limit over projection(s) and an order, and does not descend through this extension. They may change a parent plan but do not cap the rank input. | Late-score tests cover `LIMIT 1`, `ORDER BY ... LIMIT 1`, and `LIMIT 0`. |
| Late materialization | Its eligible descent chain is limited to projection, filter, and table scan; the extension ends that chain. | Parent limit/order cases pass with the pass enabled. |
| Empty-result pullup | It recurses into children but does not replace an extension because its child is empty. | `test_parent_context_and_duplicate_rows` checks the empty input result. |
| Statistics propagation and compressed materialization | The extension takes the generic child-propagation branch. Compression only rewrites aggregate, join, distinct, and order nodes; it has no extension rewrite. Estimates are not used to establish direct-rule eligibility. | Full DECIDE suite and the parent aggregate/join cases run with both passes enabled. |
| Join-filter and TopN dynamic-filter pushdown | Target discovery has no extension case, so a parent join or TopN cannot install a scan filter under the direct boundary. | The parent-join regression has a higher-scoring row outside the join and an invalid row outside it. |

The relevant code is `src/optimizer/cte_filter_pusher.cpp`,
`src/optimizer/filter_pushdown.cpp`, `src/optimizer/join_order/relation_manager.cpp`,
`src/optimizer/remove_unused_columns.cpp`, `src/optimizer/column_lifetime_analyzer.cpp`,
`src/optimizer/limit_pushdown.cpp`, `src/optimizer/topn_optimizer.cpp`,
`src/optimizer/late_materialization.cpp`, `src/optimizer/empty_result_pullup.cpp`,
`src/optimizer/statistics_propagator.cpp`,
`src/optimizer/join_filter_pushdown_optimizer.cpp`, and
`src/optimizer/decide/direct/direct_result_boundary.cpp` and
`src/optimizer/decide/direct/direct_builder.cpp`.

This audit covers the current built-in optimizer pipeline. Any future pass that
crosses logical extensions must preserve the boundary's full-input validation
and slot map. `RemoveUnusedColumns` now calls the extension's opt-in pruning
hook before visiting child dependencies. The direct result boundary replaces
only unreferenced, provably skippable outputs; its mandatory rank dependency
remains. The [wide-output regressions](../03_correctness/done.md) check binding,
serializer, and late-error behavior through stored scans and an inner
comparison join. A computed payload's error must still occur on both direct
and solver paths. Join predicate dependencies remain owned by the join and
are visited independently of the result boundary's output map.
