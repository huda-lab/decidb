# Stage 06 — Model formulation

Translates the evaluated, solver-neutral `SolverInput` into a `SolverModel`: flat
variable arrays, constraints in COO, and an optional Q matrix. It owns the
variable layout, coefficient accumulation, bounds and indexing. It does no
SQL-expression canonicalization and knows nothing about any backend.

**Key source files**

- `src/decidb/utility/ilp_model_builder.cpp` — `SolverModel::Build()`
- `src/include/duckdb/decidb/ilp_model.hpp` — `VarIndexer`, `SolverModel`, provenance
- `src/include/duckdb/decidb/solver_input.hpp` — the input contract

`Build()` takes `SolverInput` by **non-const** reference so raw global constraints
can be moved rather than copied, plus a pre-built `VarIndexer` threaded in from
`PhysicalDecide::Finalize()` so it is constructed exactly once.

---

## 1. `VarIndexer`: the four-block layout

```
Block 1: row-scoped vars       [0, num_rows * num_row_vars)
Block 2: entity-scoped vars    sum of num_entities per scope
Block 3: scalar (query-wide)   one column each
Block 4: global auxiliaries    ABS / MIN-MAX / McCormick auxiliaries
```

Scalars sit **below** `global_block_start` deliberately: the invariant "user
decide-variable columns occupy `[0, global_block_start)`" is relied on by the
model builder and by the column labeller in diagnostics.

| Block | Index formula |
|---|---|
| Row-scoped | `row * num_row_vars + row_var_offset[var_idx]` |
| Entity-scoped | `entity_var_base[var_idx] + row_to_entity[row]` |
| Scalar | `scalar_column[var_idx]` — `row` is ignored |
| Global | `global_block_start + g` |

Three accessors matter:

- **`Get(var_idx, row)`** — resolve through a *result row*. A scalar ignores `row`;
  every row resolves to the same column.
- **`InstanceColumn(var_idx, inst)`** — walk a variable's *own* instances: rows for
  row-scoped, entity ids for entity-scoped, the single column for a scalar.
- **`NumInstances(var_idx)`** — how many instances that variable has.

Two constructors:

- **`VarIndexer::Build()`** — owning; copies entity mappings so the indexer can
  outlive the `SolverInput`. This is the production form: built once early in
  `Finalize`, threaded through `SolveModel` and `Build`, then moved onto
  `gstate.var_indexer` for readback in `GetData`. `total_vars` is refreshed just
  before the solve, once every auxiliary global has been appended.
- **`VarIndexer::BuildRef()`** — non-owning; stores a `const` pointer to the input's
  entity mappings. No production callers; retained for tests.

---

## 2. Variable types and bounds

Type-driven defaults:

| `variable_types[var]` | `is_binary` | `is_integer` | Default bounds |
|---|---|---|---|
| `BOOLEAN` | true | true | `[0, 1]` |
| `INTEGER` / `BIGINT` | false | true | `[0, 1e30]` |
| `DOUBLE` / `FLOAT` | false | false | `[0, 1e30]` |

`PhysicalDecide::Finalize` reports `BOOLEAN` for any variable with
`is_boolean_var` set, whatever its DuckDB-facing type, so a declared `x(BOOL)` and
an INTEGER-typed IN-domain / L0 indicator both reach the `[0,1]` box through this
one type-driven path.

**Bound merging is asymmetric, and deliberately so** (`ilp_model_builder.cpp:210-223`):

- **Lower** — taken **directly** from `input.lower_bounds`, not `max`-ed with the
  type default. The physical operator already resolved each variable's lower bound,
  defaulting unconstrained variables to 0 and honoring explicit negative bounds, so
  it is authoritative. Taking a `max` would clamp a legitimate negative bound back
  to 0.
- **Upper** — intersected with `min`, because the type default is a true ceiling
  (`1.0` for BOOLEAN).

Afterwards, a `binary` column whose bounds ended up outside `[0,1]` — an unusual
explicit user pin such as `x >= -1` on a declared `BOOL` — is **downgraded to a
plain bounded integer**. Backends do not accept a binary paired with an
out-of-range bound; the solve result is the same, the column is just not flagged
binary.

Bounds and type flags then expand across `NumInstances(var)` for each variable.
Global auxiliaries take their own bounds and types from
`input.global_lower_bounds` / `global_upper_bounds` / `global_variable_types`.

---

## 3. Objective

A flat `obj_coeffs` of size `total_vars`, zero-initialized. For each term,
`objective_coefficients[term][row]` is placed at `Get(var_idx, row)`.

For an entity-scoped variable, several rows map to the same column, and their
coefficients **accumulate** — which is exactly right: one entity variable
participates in the objective across all of its rows.

`maximize` comes from `input.sense`.

---

## 4. Constraints

Each `EvaluatedConstraint` becomes one or more `ModelConstraint`s.

### The fast-path selector

`CanUseRowScopedFastPath()` inspects `variable_indices` once and returns true only
when (a) every term references a **row-scoped** decide variable and (b) no decide
variable appears in more than one term. Under those conditions each `(term, row)`
pair yields a unique flat index, so coefficients push straight into
`constr.indices` / `constr.coefficients`.

Anything else — entity-scoped variables, or a repeated decide variable — falls
back to `SparseCoeffAccumulator`, which handles `(entity, row)` collisions. The
accumulator picks one strategy per constraint and reuses scratch across groups:
a dense `vector<double>` indexed by flat var index plus a `touched` list when the
decide-variable index span fits the cap (~1M slots), or a sorted/merged
`(idx, coeff)` pair list otherwise.

Both aggregate branches use the same selector.

### Path 1 — aggregate, ungrouped

One `ModelConstraint` summing over all rows. RHS is `rhs_values[0]`, then fixed
LHS aggregate terms (`variable_index == INVALID_INDEX`) are summed over all rows
and subtracted.

### Path 2 — aggregate, grouped (`WHEN` and/or `PER`)

A `group_to_rows` index is built, skipping `INVALID_INDEX` rows, and one
`ModelConstraint` is emitted per non-empty group. Only that group's rows
contribute; fixed LHS terms are summed over that group's rows and subtracted.

### Path 3 — per-row

One `ModelConstraint` per row, skipping rows the `WHEN` excluded. RHS is
`rhs_values[row]`; fixed LHS terms are subtracted one row at a time.

AVG terms are already coefficient-scaled by stage 08 (by total active rows, by
the `WHEN` mask, by group size, or by an aggregate-local filter's surviving rows).
The builder consumes the evaluated coefficients directly.

### Quadratic and bilinear constraints

Constraints with bilinear terms or `POWER(..., 2)` go through the quadratic
constraint builder into `model.quadratic_constraints`. Linear fixed LHS terms use
the same `INVALID_INDEX` convention and are subtracted from the RHS over the
selected row set before the quadratic constraint is finalized.

### Normalization on emission

Every path pushes through `PushNormalizedConstraint()` rather than
`model.constraints.push_back()`. It drops empty-LHS tautologies (`0 <= k` for
`k>=0`, `0 >= k` for `k<=0`, `0 = 0`).

A *violated* empty-LHS row — `0 <= -1` after bounds absorption and term
cancellation, as written by `SUM(0 * x) <= -1` or `x - x <= -1` — is **kept**, and
sets `model.build_proven_infeasible`. The row stays coefficient-free and keeps its
own `source_clause_id`, which is what lets the infeasible-diagnosis engine name the
clause and report a least-change repair (`SUM(0 * x) <= 0`) instead of a bare
status. The infeasibility is still decided here, not by a solver: `SolveModel`
short-circuits on the flag, so no backend is handed a row with no coefficients.

Emitting nothing and throwing instead would take the whole model with it — every
later consumer, diagnosis included, then has no model to read.

### Reservation

Before the loop the builder sums an upper bound on `model.constraints.size()`
(aggregate → 1 or `num_groups`, per-row → `num_rows`) and reserves once. The HiGHS
backend does the same for its COO arrays.

---

## 5. `ApplyComparisonSense()`

| `ExpressionType` | Sense | RHS |
|---|---|---|
| `COMPARE_GREATERTHANOREQUALTO` | `'>'` | direct |
| `COMPARE_GREATERTHAN` | `'>'` | `floor(rhs) + 1.0` |
| `COMPARE_LESSTHANOREQUALTO` | `'<'` | direct |
| `COMPARE_LESSTHAN` | `'<'` | `ceil(rhs) - 1.0` |
| `COMPARE_EQUAL` | `'='` | direct |

`'>'` and `'<'` mean `>=` and `<=` — standard ILP convention. Strict inequalities
are converted by adjusting the RHS, which is exact for integer variables. It is
*not* exact for continuous ones, which is why a strict `<` / `>` on a REAL variable
is deliberately not absorbed into bounds upstream: it reaches here and is rejected
with a clear message instead of being silently approximated.

---

## 6. Quadratic objective

When `input.has_quadratic_objective`, Q is built for the standard form
`minimize (1/2) xᵀQx + cᵀx`.

The inner linear expression of `SUM(POWER(expr, 2))` is already evaluated per row
in `quadratic_inner_coefficients`; for row *r* it is `Σₜ a_{t,r}·x_{varₜ}`. Q is
the sum of outer products across rows:

1. **Variable terms** — `Q[i,j] += 2·aᵢ·aⱼ`, the factor of 2 on both diagonal and
   off-diagonal entries following the `(1/2)` convention.
2. **Constant terms** — a constant `c` in the inner expression expands as
   `(expr + c)² = expr² + 2c·expr + c²`, so the cross-terms `2c·aₜ` are added to
   `obj_coeffs`.
3. **Storage** — accumulated in a `std::map<pair<int,int>, double>` over the lower
   triangle, then serialized to `q_rows` / `q_cols` / `q_vals`.

**Convexity is guaranteed by construction**: `Q = Σ a·aᵀ = AᵀA` is positive
semidefinite. That is the payoff of enforcing convexity through the syntax rather
than checking it numerically.

---

## 7. Provenance carried on every row

`ConstraintProvenance` (`ilp_model.hpp`) travels with each emitted constraint so
diagnosis can report at the **user-clause** level rather than at raw matrix rows:
the source comparison for display, `repair_group_id` for elastic grouping, the
`PER`/`WHEN` group id and its printable key, `ConstraintKind` (user parameter vs
rigid mechanism row), the elastic shape (does this row share one slack with its
siblings or get its own), whether coefficients were pre-scaled by `1/N_g` for an
AVG rewrite, whether the user wrote a strict comparison (with the original literal
kept so a suggestion can be re-quoted against it), and the flat column of the `<>`
disjunction binary the row belongs to.

`FoldedAggTerm` pairs each summed decide variable with the coefficient the user
wrote for it. `has_unit` is false when that coefficient is data-varying — there is
no literal to quote, so rendering falls back to the symbolic name in
`weight_labels`.

---

## 8. Sanity checks

After building:

1. **Bounds** — every variable must have finite bounds with `lower <= upper`.
   Under `tolerate_infeasible_bounds` (diagnosis mode) an inverted box is kept
   rather than thrown, so the model survives for the elastic engine; stage 07
   short-circuits to `INFEASIBLE` without handing it to a backend.
2. **Objective coefficients** — every entry finite.
3. **Constraints** — `indices` and `coefficients` the same size, every index in
   `[0, total_vars)`, every coefficient finite, RHS not NaN (infinity is allowed
   for range representations).

These catch evaluation and model-building bugs where the message can still name
the column, rather than at the solver where it cannot.

---

## 9. Source map

| Concern | Location |
|---|---|
| `SolverModel::Build`, all constraint paths, Q construction | `src/decidb/utility/ilp_model_builder.cpp` |
| `VarIndexer`, `SolverModel`, `ModelConstraint`, provenance | `src/include/duckdb/decidb/ilp_model.hpp` |
| `SolverInput`, `EvaluatedConstraint`, `CoefficientColumn` | `src/include/duckdb/decidb/solver_input.hpp` |
| Golden model corpus (the characterization oracle) | `test/decide/golden/` |
