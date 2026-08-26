# Relation-Qualified Reducer Test Coverage — Done

Tests live in `test/decide/tests/test_qualified_reducer.py` (23 tests), plus
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
- **Aggregate-local `WHEN` composed with the qualifier, in a constraint, WHEN
  before the comparison**: `test_qualified_reducer_with_aggregate_local_when_in_constraint`
  (`SUM(D: expr) WHEN cond <= bound`) — the shape that used to be rejected (grammar
  fix, `03_expressivity/decide/done.md` → "Relation-qualified reducers"). Nation 5
  sits outside the filter so it never counts against the cap and is always kept;
  nations 14/15/16 do count and only the value-maximizing one (16) fits, so nation
  5's keep value is what distinguishes a correctly scoped WHEN from one silently
  dropped back to the old bug.
- **Composed MIN/MAX keeps the qualifier**
  (`test_composed_minmax_preserves_the_qualifier`,
  `test_composed_minmax_preserves_the_qualifier_in_a_constraint`): adding
  `+ MAX(...)` to a clause holding `SUM(D: ...)` must not change what the qualifier
  means. Both use the inverting 5/14 fixture, so a dropped qualifier changes the
  chosen nation (objective) or empties the selection (constraint).
- **A composite qualifier weights differently from both alternatives**
  (`test_three_relation_composite_qualifier_differs_from_single_and_unqualified`,
  oracle-verified): a customer/orders/lineitem chain where `sum(c, o: ...)`,
  `sum(c: ...)` and the unqualified `sum(...)` weight the same decision by distinct
  order count, by 1, and by lineitem row count respectively — each checked against
  its own independent oracle model. A two-relation fixture cannot tell these apart, which is
  why this one needs three.
- **Naming every relation the query joins is a no-op**
  (`test_two_relation_composite_qualifier_equals_unqualified_when_query_has_only_those_two_relations`):
  with no third, unnamed relation left to fan out, `SUM(D, T: expr)` and the
  unqualified `SUM(expr)` agree row for row.
- **A query-wide decision inside a qualified reducer is weighted and de-duplicated**
  (`test_qualified_reducer_scalar_times_entity_data_is_weighted_and_deduplicated`,
  oracle-verified): `SUM(n: (n_nationkey + 1) * cap)` over `customer JOIN nation`
  weights `cap` by the sum over *distinct* nations, not over join-result rows.
- **Rejections** (error tier, all at bind time): a column from a relation not named
  in the qualifier, a row-scoped decision, an unknown relation name — including one
  inside a multi-relation list
  (`test_unknown_relation_in_multi_relation_qualifier_rejected`) — and a
  row-invariant body, i.e. a query-wide decision standing alone inside the reducer
  (`test_query_wide_decision_alone_inside_qualified_reducer_rejected`).

## Caveats

- `test_row_scoped_decision_inside_qualified_reducer_rejected` pins a *current*
  rejection that is still an open question, not settled semantics — see
  `03_expressivity/decide/todo.md` → "Row-scoped decisions inside a
  relation-qualified reducer".
- `test_query_wide_decision_alone_inside_qualified_reducer_rejected` is **not** in
  that category: it pins the general row-invariance rule (a body with nothing to
  aggregate over), and the paper's §3.2.2 carve-out for query-wide decisions is
  honoured by the neighbouring positive test.
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
| qualified reducer | query-wide decision times entity data | ✓ (oracle) |
| qualified reducer | query-wide decision as the whole body (rejected) | ✓ |
| qualified reducer | split clause order | ✓ (`test_clause_order.py`) |
| qualified reducer | hard-direction MAX objective | ✓ |
| qualified reducer | hard-direction MIN constraint | ✓ |
| qualified reducer | aggregate-local `WHEN` (objective) | ✓ |
| qualified reducer | aggregate-local `WHEN` (constraint) | ✓ |
| qualified reducer | composed MIN/MAX (objective) | ✓ |
| qualified reducer | composed MIN/MAX (constraint) | ✓ |
| qualified reducer | multi-relation qualifier, 3 relations | ✓ (oracle) |
| qualified reducer | multi-relation qualifier, no-op case | ✓ |
| qualified reducer | unknown relation in a multi-relation list (rejected) | ✓ |
