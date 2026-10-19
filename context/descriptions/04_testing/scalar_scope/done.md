# Query-Wide Decision Test Coverage — Done

Tests live in `test/decide/tests/test_scalar_scope.py` (the file keeps its name), plus
`test_scalar_var_in_aggregate.py` for a query-wide decision as a term of an aggregate
constraint and `test_clause_order.py::test_scalar_declaration_in_split_slot` for the
pre-`FROM` declaration position.

`DECIDE per (): x(TYPE)` is one decision for the whole query, beside one per row
(`x(TYPE)`) and one per class of a key (`per K: x(TYPE)`). Semantics:
`03_expressivity/decide/done.md`; syntax: `00_project_overview/syntax_reference.md`
§2.2.

## Scenarios covered

- **Shared bound, oracle-verified** (`test_scalar_shared_bound`): the paper's §3.1
  shape — one cap constrains every row and is penalised in the objective, so the
  optimum trades a single cap value against the per-row gain it unlocks. A per-row cap
  would decouple the rows entirely, which is what makes this discriminating.
- **The objective coefficient is applied once, not per row**
  (`test_scalar_objective_coefficient_applied_once`, oracle-verified):
  `MINIMIZE 2*cap - SUM(x)` is constructed so the two readings drive `cap` to opposite
  ends of its domain. This is the case that would silently multiply a query-wide
  coefficient by input cardinality.
- **One column regardless of cardinality**
  (`test_scalar_is_one_column_regardless_of_cardinality`) and **the value is repeated
  on every output row** (`test_scalar_value_repeated_on_every_row`).
- **A row-invariant reducer body is rejected** (`TestScalarReducerRejected`, error
  tier): `SUM(cap)` and `AVG(cap)` in objective and constraint position, and
  `SUM(cap1 + cap2)`. A query-wide decision mixed with row-varying data or a per-row
  decision is legal and is weighted once per counted row
  (`test_scalar_times_data_inside_reducer_is_weighted_by_row_data`,
  `test_scalar_plus_row_scoped_term_inside_reducer_is_legal`,
  `test_scalar_var_in_aggregate.py`).
- **`scalar` is an ordinary name** (`test_scalar_still_usable_as_identifier`,
  `test_query_wide_decision_beside_a_column_named_scalar`): it is an unreserved
  keyword, so a column, alias or CTE named `scalar` still resolves, including beside a
  query-wide decision, and as the name of a decision (`test_scope_removed_syntax.py`).
- **Empty input** (`test_scalar_empty_input`): an empty selection returns an empty
  relation, exactly as for a per-row decision. There is no special case.
- **Composition with the rewrite-heavy features**, all oracle-verified except the
  `<>` case, which is pinned to an exact value. These are per-row constraints that put
  the query-wide decision under a rewrite: `ABS(x - cap) <= 2`
  (`test_scalar_with_abs_per_row_constraint`), `POWER(x - cap, 2) <= 4`
  (`test_scalar_with_quadratic_constraint`, Gurobi only), `b * cap <= 4`
  (`test_scalar_with_bilinear_per_row_constraint`, McCormick), and `cap <> 8`
  (`test_scalar_with_not_equal`, Big-M). Each rewrite appends auxiliaries to the
  global block, which sits immediately above the query-wide block in `VarIndexer`'s
  four-block layout — these are the cases where a misplaced block boundary would
  surface. Query-wide decisions beside reducers under `<>`, beside boolean products and
  squared reducers, and in products of decisions are in `test_scope_bare_clauses.py`.
- **The query-wide coefficient survives both reducer desugarings**, oracle-verified.
  `norm(e, 1)` becomes `SUM(ABS(e))` and a quadratic objective becomes
  `SUM(POWER(e, 2))`, so both put the decision inside a reducer body mixed with
  row-varying data, and both must pick up the same per-row weighting the plain
  `SUM(data * cap)` path uses.
  - `test_scalar_inside_norm_is_weighted_per_row` —
    `norm(l_linenumber * cap - 3, 1) <= 85`. The bound is chosen so the three readings
    of the coefficient give three different answers: per-row and data-weighted
    (correct, `cap = 1`), one collapsed ABS over the totals (`cap = 2`), and the data
    multiplier dropped (`cap = 6`). The test asserts the fixture still separates them,
    so it fails loudly if the data shifts.
  - `test_scalar_inside_quadratic_objective_is_weighted_per_row` —
    `MINIMIZE SUM(POWER(l_linenumber * cap - 4, 2))` with `cap` REAL, so the optimum is
    the continuous least-squares point `4*SUM(d)/SUM(d^2)` rather than an integer a
    misreading could hit by accident; dropping the data multiplier would put it at 4.
    REAL also keeps the model a plain QP, so this runs on both backends instead of
    needing Gurobi's MIQP.
- **A query-wide decision as the bound of a reduced constraint**, with and without a
  `per` key (`test_scalar_as_aggregate_rhs`, `test_scalar_as_aggregate_rhs_with_per`:
  `per c_nationkey: sum(x) by (c_nationkey) <= cap`). Canonicalization moves it to the
  decision side, and the same single column takes part in every class's row.
- **Diagnostics** (`test_unbounded_scalar_reports_a_single_instance`): an unbounded
  query-wide decision produces one runaway finding with no amount or categorical slice
  — there is no subset of rows to characterize.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| query-wide | per-row decision in the same query | ✓ |
| query-wide | row-varying comparison (`ship <= cap`) | ✓ |
| query-wide | bare in objective (`MINIMIZE cap`) | ✓ |
| query-wide | additive beside a reducer (`MINIMIZE 2*cap - SUM(x)`) | ✓ |
| query-wide | JOIN source | ✓ |
| query-wide | split clause order | ✓ (`test_clause_order.py`) |
| query-wide | colon-form reducer | ✓ when weighted by the relation's data; a bare row-invariant body is rejected |
| query-wide | empty input | ✓ |
| query-wide | ABS (per-row constraint) | ✓ |
| query-wide | quadratic constraint (QCQP, Gurobi) | ✓ |
| query-wide | bilinear `b * cap` (McCormick) | ✓ |
| query-wide | `<>` (Big-M disjunction) | ✓ |
| query-wide | unbounded diagnostics report | ✓ |
| query-wide | reduced-constraint bound | ✓ |
| query-wide | `per` | ✓ |
| query-wide | `norm(...)` (L1, desugars to `SUM(ABS(...))`) | ✓ |
| query-wide | quadratic objective (`SUM(POWER(...))`) | ✓ |
