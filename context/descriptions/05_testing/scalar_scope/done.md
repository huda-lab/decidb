# Query-Wide (`scalar`) Scope Test Coverage — Done

Tests live in `test/decide/tests/test_scalar_scope.py` (20 tests), plus
`test_clause_order.py::test_scalar_declaration_in_split_slot` for the
pre-`FROM` declaration position.

`DECIDE scalar x(TYPE)` is the third variable scope alongside row-scoped
(`x(TYPE)`) and table-scoped (`T.x(TYPE)`). Semantics:
`03_expressivity/decide/done.md` → "Query-wide (`scalar`) variables";
syntax: `00_project_overview/syntax_reference.md` §2.2.

## Scenarios covered

- **Shared bound, oracle-verified** (`test_scalar_shared_bound`): the paper's
  §3.1 shape — one cap constrains every row and is penalised in the objective,
  so the optimum trades a single cap value against the per-row gain it unlocks.
  A row-scoped cap would decouple the rows entirely, which is what makes this
  discriminating.
- **The objective coefficient is applied once, not per row**
  (`test_scalar_objective_coefficient_applied_once`, oracle-verified):
  `MINIMIZE 2*cap - SUM(x)` is constructed so the two readings drive `cap` to
  opposite ends of its domain. This is the case that would silently multiply a
  scalar's coefficient by input cardinality.
- **One column regardless of cardinality**
  (`test_scalar_is_one_column_regardless_of_cardinality`) and **the value is
  repeated on every output row** (`test_scalar_value_repeated_on_every_row`).
- **Reducers over a scalar are rejected** (`TestScalarReducerRejected`, error
  tier): `SUM(cap)` and `AVG(cap)` in both objective and constraint position,
  and `SUM(x + cap)` where the scalar is an additive term inside the reducer.
  There is one column, so nothing to reduce over, and the two plausible readings
  (coefficient 1 vs. coefficient `n`) are different problems — the rejection is
  what stops one being picked silently.
- **Grammar** (error tier): a `scalar` with no type, and `scalar T.x(TYPE)`,
  each rejected with a message naming the fix.
- **`scalar` remains an ordinary identifier**
  (`test_scalar_still_usable_as_identifier`,
  `test_scalar_identifier_and_keyword_in_one_query`) — it is an unreserved
  keyword, so a column named `scalar` still resolves, including in a query that
  also declares a `scalar` decision.
- **Empty input** (`test_scalar_empty_input`): an empty selection returns an
  empty relation, exactly as for a row-scoped decision. There is no special case.
- **Composition with the rewrite-heavy features**, all oracle-verified except the
  `<>` case, which is pinned to an exact value. A scalar cannot appear inside a
  reducer and objectives must be aggregates, so `ABS` / `POWER` / bilinear reach
  a scalar only through **per-row constraints** — that is the surface covered:
  `ABS(x - cap) <= 2` (`test_scalar_with_abs_per_row_constraint`),
  `POWER(x - cap, 2) <= 4` (`test_scalar_with_quadratic_constraint`, Gurobi
  only), `b * cap <= 4` (`test_scalar_with_bilinear_per_row_constraint`,
  McCormick), and `cap <> 8` (`test_scalar_with_not_equal`, Big-M). Each
  rewrite appends auxiliaries to the global block, which sits immediately above
  the scalar block in `VarIndexer`'s four-block layout — these are the cases
  where a misplaced block boundary would surface.
- **A scalar on the RHS of a reduced constraint is rejected**
  (`test_scalar_as_aggregate_rhs_rejected`, and the `PER` variant). This is the
  paper's own §3.1 shape and is blocked on group B; see `todo.md`.
- **Diagnostics** (`test_unbounded_scalar_reports_a_single_instance`): an
  unbounded scalar reports only `grows_toward`, with no `affected_rows` /
  `affected_entities` cell — there is no subset of rows to characterize.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| scalar | row-scoped variable in the same query | ✓ |
| scalar | row-varying comparison (`ship <= cap`) | ✓ |
| scalar | bare in objective (`MINIMIZE cap`) | ✓ |
| scalar | additive beside a reducer (`MINIMIZE 2*cap - SUM(x)`) | ✓ |
| scalar | JOIN source | ✓ |
| scalar | split clause order | ✓ (`test_clause_order.py`) |
| scalar | qualified reducer (rejected) | ✓ (`test_qualified_reducer.py`) |
| scalar | empty input | ✓ |
| scalar | ABS (per-row constraint) | ✓ |
| scalar | quadratic constraint (QCQP, Gurobi) | ✓ |
| scalar | bilinear `b * cap` (McCormick) | ✓ |
| scalar | `<>` (Big-M disjunction) | ✓ |
| scalar | unbounded diagnostics report | ✓ |
| scalar | reduced-constraint RHS | rejected (group B) |
| scalar | `PER` | not reachable — see `todo.md` |
