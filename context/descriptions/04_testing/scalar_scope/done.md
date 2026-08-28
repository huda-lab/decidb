# Query-Wide (`scalar`) Scope Test Coverage — Done

Tests live in `test/decide/tests/test_scalar_scope.py`, plus
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
- **A row-invariant reducer body is rejected** (`TestScalarReducerRejected`, error
  tier): `SUM(cap)` and `AVG(cap)` in both objective and constraint position. A
  scalar mixed with row-varying data or a row-scoped decision is legal and is
  weighted once per counted row; `test_scalar_var_in_aggregate.py` and the
  scalar-in-reducer tests in `test_scalar_scope.py` cover that distinction.
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
- **The scalar's coefficient survives both reducer desugarings**, oracle-verified.
  `norm(e, 1)` becomes `SUM(ABS(e))` and a quadratic objective becomes
  `SUM(POWER(e, 2))`, so both put the scalar inside a reducer body mixed with
  row-varying data and both must pick up the same per-row weighting the plain
  `SUM(data * cap)` path uses.
  - `test_scalar_inside_norm_is_weighted_per_row` —
    `norm(l_linenumber * cap - 3, 1) <= 85`. The bound is chosen so the three
    readings of the coefficient give three different answers: per-row and
    data-weighted (correct, `cap = 1`), one collapsed ABS over the totals
    (`cap = 2`), and the data multiplier dropped (`cap = 6`). The test asserts
    the fixture still separates them, so it fails loudly if the data shifts.
  - `test_scalar_inside_quadratic_objective_is_weighted_per_row` —
    `MINIMIZE SUM(POWER(l_linenumber * cap - 4, 2))` with `cap` REAL, so the
    optimum is the continuous least-squares point `4*SUM(d)/SUM(d^2)` rather
    than an integer a misreading could hit by accident; dropping the data
    multiplier would put it at 4. REAL also keeps the model a plain QP, so this
    runs on both backends instead of needing Gurobi's MIQP.
- **A scalar on the RHS of a reduced constraint is supported**, with and without
  `PER` (`test_scalar_as_aggregate_rhs`,
  `test_scalar_as_aggregate_rhs_with_per`). Canonicalization moves the scalar to
  the model side, and the same single query-wide column participates in every
  group row.
- **Diagnostics** (`test_unbounded_scalar_reports_a_single_instance`): an
  unbounded scalar produces one runaway finding with no amount or categorical
  slice — there is no subset of rows to characterize.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| scalar | row-scoped variable in the same query | ✓ |
| scalar | row-varying comparison (`ship <= cap`) | ✓ |
| scalar | bare in objective (`MINIMIZE cap`) | ✓ |
| scalar | additive beside a reducer (`MINIMIZE 2*cap - SUM(x)`) | ✓ |
| scalar | JOIN source | ✓ |
| scalar | split clause order | ✓ (`test_clause_order.py`) |
| scalar | qualified reducer | ✓ when weighted by qualified-row data; bare row-invariant body rejected |
| scalar | empty input | ✓ |
| scalar | ABS (per-row constraint) | ✓ |
| scalar | quadratic constraint (QCQP, Gurobi) | ✓ |
| scalar | bilinear `b * cap` (McCormick) | ✓ |
| scalar | `<>` (Big-M disjunction) | ✓ |
| scalar | unbounded diagnostics report | ✓ |
| scalar | reduced-constraint RHS | ✓ |
| scalar | `PER` | ✓ |
| scalar | `norm(...)` (L1, desugars to `SUM(ABS(...))`) | ✓ |
| scalar | quadratic objective (`SUM(POWER(...))`) | ✓ |
