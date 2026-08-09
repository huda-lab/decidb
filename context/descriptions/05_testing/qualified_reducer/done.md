# Relation-Qualified Reducer Test Coverage — Done

Tests live in `test/decide/tests/test_qualified_reducer.py` (20 tests), plus
`test_clause_order.py::test_entity_scoped_declaration_in_split_slot` for a
qualified reducer in the split clause order.

`SUM(D: expr)` reduces over `D`'s tuple identities instead of over join-result
rows. Semantics: `03_expressivity/decide/done.md` → "Relation-qualified
reducers"; syntax: `00_project_overview/syntax_reference.md` §5.1.

The fixture is `customer JOIN nation`, where the join repeats each nation once
per customer and nations have unequal customer counts — that inequality is what
makes the row-weighted and identity-weighted optima diverge.

## Scenarios covered

- **Qualified SUM charges each entity once**
  (`test_qualified_sum_charges_each_entity_once`, oracle-verified).
- **Qualified and unqualified diverge** (`test_qualified_and_unqualified_diverge`,
  oracle-verified): the same query both ways, with two oracles. This is the test
  that would fail if the qualifier were parsed and then dropped — the failure
  mode the feature is most exposed to.
- **Qualified AVG's denominator is the distinct-identity count, not the row
  count** (`test_qualified_avg_denominator_is_distinct_entities`,
  oracle-verified). The denominator falls out of the de-duplication mask rather
  than being computed separately, so this pins the mask's placement.
- **Qualified MIN / MAX** (`test_qualified_minmax`, parametrized over the
  easy-direction pair): accepted and carried, with no effect — dropping repeats
  of a value already present cannot move an extremum.
- **Constraint position** (`test_qualified_reducer_in_constraint`,
  oracle-verified).
- **PER composition** (`test_qualified_reducer_with_per`): de-duplication runs
  inside the partition, keying on `(PER group, entity id)`.
- **Mixed qualified and unqualified reducers in one objective**
  (`test_mixed_qualified_and_unqualified_objective`, oracle-verified): pins that
  the mask is per *term*, not per clause.
- **Hard-direction MIN / MAX with a qualifier**:
  `test_qualified_hard_max_objective` (`MAXIMIZE MAX(n: ...)`, the Big-M
  direction) and `test_qualified_hard_min_constraint` (`MIN(n: ...) <= K`). The
  claim that de-duplication cannot move an extremum had only been checked for
  the easy directions, which emit no auxiliaries; these confirm it survives the
  Big-M encoding, where the mask and the indicator rows meet.
- **Aggregate-local `WHEN` composed with the qualifier, in an objective**:
  `test_qualified_reducer_with_aggregate_local_when_in_objective` uses nations 5
  and 14, whose row counts invert the ranking (identity weights 5 and 14, row
  weights 45 and 28), so a dropped qualifier changes *which nation is chosen*,
  not merely the objective value.
  `test_aggregate_local_when_filters_inside_a_qualified_reducer` then flips the
  answer using the filter alone. Together they pin that both masks apply — the
  only place two independent masks share one `TermFilterState` slot.
- **Composed MIN/MAX keeps the qualifier**
  (`test_composed_minmax_preserves_the_qualifier`,
  `test_composed_minmax_preserves_the_qualifier_in_a_constraint`): adding
  `+ MAX(...)` to a clause holding `SUM(D: ...)` must not change what the qualifier
  means. Both use the inverting 5/14 fixture, so a dropped qualifier changes the
  chosen nation (objective) or empties the selection (constraint).
- **Rejections** (error tier, all at bind time except the first): multi-relation
  qualifier `SUM(D,T: ...)` (grammar action), a column from another relation, a
  row-scoped decision, a query-wide (`scalar`) decision, and an unknown relation
  name.

## Caveats

- `test_row_scoped_decision_inside_qualified_reducer_rejected` and
  `test_query_wide_decision_inside_qualified_reducer_rejected` pin *current*
  rejections that are still open questions, not settled semantics — see
  `03_expressivity/decide/todo.md` → "Row-scoped decisions inside a
  relation-qualified reducer". The second is a deliberate divergence from paper
  §3.2.2, which carves out query-wide decisions as always allowed.
- `test_multi_relation_qualifier_rejected` pins a deferral, not a limitation of
  the design — `03_expressivity/decide/todo.md` → "Multi-relation qualifiers".
- `test_qualified_reducer_with_aggregate_local_when_in_constraint_rejected` pins
  an **inconsistency**, not intended semantics: the same expression binds in an
  objective. The message also leaks the internal `__qualified_reducer__` tag.
  Logged in `07_issues/code_quality/todo.md`.
- `test_composed_minmax_preserves_the_qualifier` and
  `…_in_a_constraint` are **regressions for a fixed wrong answer**, not aspirational
  pins: `SUM(D: ...) + MAX(...)` used to revert to row semantics because
  `ComposedMinMaxTerm` carried no `qualifier_scope_idx`. Objective and constraint are
  separate code paths, hence two tests.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| qualified reducer | SUM | ✓ |
| qualified reducer | AVG | ✓ |
| qualified reducer | MIN / MAX (easy direction) | ✓ |
| qualified reducer | constraint position | ✓ |
| qualified reducer | objective position | ✓ |
| qualified reducer | PER | ✓ |
| qualified reducer | unqualified reducer in the same objective | ✓ |
| qualified reducer | entity-scoped decision | ✓ |
| qualified reducer | row-scoped decision (rejected) | ✓ |
| qualified reducer | query-wide decision (rejected) | ✓ |
| qualified reducer | split clause order | ✓ (`test_clause_order.py`) |
| qualified reducer | hard-direction MAX objective | ✓ |
| qualified reducer | hard-direction MIN constraint | ✓ |
| qualified reducer | aggregate-local `WHEN` (objective) | ✓ |
| qualified reducer | aggregate-local `WHEN` (constraint) | rejected — see `todo.md` |
| qualified reducer | composed MIN/MAX (objective) | ✓ |
| qualified reducer | composed MIN/MAX (constraint) | ✓ |
| qualified reducer | multi-relation qualifier | rejected (deferred) |
