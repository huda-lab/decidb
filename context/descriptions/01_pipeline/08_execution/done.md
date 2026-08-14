# Stage 08 — Physical execution and readback

Runs the relational input, extracts model terms from the canonical trees,
evaluates every coefficient and grouping against the materialized data, invokes
the solver contract, and projects the solution back onto rows. It implements no
rewrites and no backend-specific formulation.

`PhysicalDecide` is a **blocking** operator: the optimal value of any one decision
depends on the whole dataset, so it must consume its entire input before it can
produce a row.

**Key source file**: `src/execution/operator/decide/physical_decide.cpp` (~7,400 lines)
**Header**: `src/include/duckdb/execution/operator/decide/physical_decide.hpp`

---

## 1. Order of operations

| # | Where | What |
|---|---|---|
| 1 | `GetGlobalSinkState` → `DecideGlobalSinkState` ctor | Bound absorption, then term extraction from the canonical trees |
| 2 | `Sink` / `Combine` | Materialize every surviving tuple into a `ColumnDataCollection` |
| 3 | `Finalize` PHASE 1.5 | Build entity mappings for table-scoped variables |
| 4 | `Finalize` PHASE 2 | Evaluate coefficients, `WHEN` masks, `PER` groups, RHS values |
| 5 | `Finalize` PHASE 3 | `SolverModel::Build` (stage 06) → `SolveModel` (stage 07) |
| 6 | `GetData` | Project solution values back onto rows |

**Extraction precedes materialization.** The sink state is constructed before the
first `Sink` call, so steps 1 and 2 are in that order — extraction is purely
structural and needs no data.

Read consistency follows from step 2: the optimization runs on the snapshot the
query saw. Concurrent modifications cannot affect a running solve.

---

## 2. Bound absorption

`TraverseBoundsConstraints()` runs first, before any term extraction, and pulls
simple per-variable bounds (`x >= 5`, `x <= 10`, `x BETWEEN 0 AND 100`) out of the
constraint tree and into column-bound arrays. A bound qualifies when the LHS is a
bare DECIDE variable — not inside a reducer — and the RHS is a constant.

The traversal recurses through `AND`, and into child 0 only of `PER` and `WHEN`
wrappers. **A `WHEN`-guarded comparison is never absorbed**: it is conditional per
row, not a domain.

- `<=` tightens the upper bound (`min`), `>=` the lower (`max`), `=` sets both.
- Strict `<` / `>` on an integer variable tighten by ±1.
- Strict `<` / `>` on a REAL variable are deliberately **not** absorbed, so
  `ApplyComparisonSense` in the model builder reaches them and rejects them with a
  clear message rather than silently approximating.

Lower bounds start at an `ABSORBED_LOWER_UNSET` sentinel rather than 0, so an
explicit negative bound (`x >= -5`, `BETWEEN -10 AND 10`) is honored instead of
clamped. `max(-1e30, k) == k` still picks the tightest of several `>=`
constraints, and `Finalize` resolves anything still at the sentinel to 0 —
non-negative unless the query explicitly says otherwise.

Absorbed comparisons are recorded in `gstate.absorbed_bound_exprs` and skipped by
`AnalyzeConstraint`, so they do not also produce `num_rows` redundant per-row
model rows.

Each absorbed bound is additionally recorded as a `UserBoundSpec {var, sense, k}`
in `gstate.user_absorbed_bounds` so infeasible diagnosis can re-emit it as a
loosenable row — it carries no provenance otherwise. **A variable's intrinsic
domain is excluded**: a BOOLEAN's `[0,1]` box is never synthesized as a constraint
at all, so one only appears here when the user redundantly restated it, and the
recording consults `op.is_boolean_var` to skip that restatement (and the default
non-negativity for any type).

### Domain contradictions are a static error, not a diagnosis

A user bound that contradicts the variable's *intrinsic* domain is deterministic —
loosening cannot help, changing the type can — so it throws here, in the main
pipeline, before the elastic engine ever sees it:

- `x <= -1` on a non-negative type, guarded as `U < 0 AND L >= 0` so that
  `x = -1` or `x <= -1 AND x >= -5`, which explicitly lower the floor, still pass;
- `x >= 2` or `x = 2` on a BOOLEAN.

A purely user-vs-user inverted box such as `x >= 5 AND x <= 1` does **not** match
and proceeds to the elastic engine, which reports a least-change loosen.

---

## 3. Term extraction

`AnalyzeConstraint()` and `AnalyzeObjective()` turn bound expression trees into
`DecideConstraint` / `Objective` structs. The goal is purely structural: which
variable each term references, and what coefficient expression multiplies it.
Numeric evaluation is PHASE 2's job.

This stage **consumes** the canonical shape and never repairs it. Invalid decision
placement or aggregate shape here is an internal invariant failure, not a
user-facing repair point.

### Wrappers

Constraints may be wrapped, as tagged `BoundConjunctionExpression`s:

| Alias | Children | Handling |
|---|---|---|
| `PER_CONSTRAINT_TAG` | `[constraint, col...]` | extract `per_columns`, recurse into child 0 |
| `WHEN_CONSTRAINT_TAG` | `[constraint, condition]` | extract `when_condition`, recurse into child 0 |
| *(none)* | N children | plain `AND` — each child is an independent constraint |

### Constraint LHS

The LHS is unwrapped with `UnwrapDecideCasts(expr, decide_index)`. The parsed
boundary already rejected every user-authored cast over decision algebra, so this
looks only through DuckDB's bound type-reconciliation wrappers and **stops at a
data-only cast**.

- **Aggregate LHS** — `ExtractAggregateConstraintTerms()` walks additive aggregate
  expressions, calls `ExtractConstraintTerms()` on each reducer's child, and copies
  reducer metadata onto the extracted terms. `lhs_is_aggregate` is set.
  Aggregate-local `WHEN` filters land on `Term::filter`, on bilinear term filters,
  or on quadratic group filters. A reducer aliased `AVG_REWRITE_TAG` marks its
  terms for AVG scaling.
- **Per-row LHS** — first the **C2 guard**: `CollectDecideVarRefs()` on the RHS must
  find nothing. The canonicalizer has already moved every decision-bearing term
  left, so a hit means a rewrite broke canonical form upstream, and the constraint
  is rejected with an internal error rather than mis-solved. Then either a
  single-variable bound (`FindDecideVariable`, coefficient 1) or a complex additive
  LHS (`ExtractConstraintTerms`) such as `z_0 + z_1 = 1` or `d - x >= -c`.

### Objective

`AnalyzeObjective()` handles linear, bilinear and quadratic objectives, including
**mixed** shapes where a quadratic group and linear siblings appear inside one
reducer (`SUM(POWER(x - t, 2) + c * x)`).

The reducer argument is walked by `ExtractLinearAndBilinearTerms`, which probes
`DetectQuadraticPattern` at **every** additive node. That detector recognizes:

- `POWER(linear, 2)` / `POW(linear, 2)` / `expr ** 2` — the exponent unwrapped from
  casts, since DuckDB wraps the literal `2` in a `BoundCastExpression`;
- `(expr) * (expr)` self-products with identical `ToString()`;
- `-(pattern)` and `K * pattern` for constant `K` on either side, nesting into
  composed signs.

A match routes the inner linear expression into `squared_terms` with a scalar
`quadratic_sign`; linear and bilinear siblings in the same `+`/`-` tree go into
`terms` / `bilinear_terms` from the same walk. Pure-linear, pure-quadratic and
mixed objectives therefore share one traversal.

**At most one quadratic group per objective.** A second match raises
`InvalidInputException` — `SUM(POWER(x,2)) + SUM(POWER(y,2))` is rejected —
because downstream Q construction assumes a single scalar sign.

**Degree guard.** `IsLinearInDecideVars` is applied to the inner of every
POWER/self-product and to each side of a bilinear `*`. Degree > 2 shapes
(`POWER(x,2)*POWER(x,2)`, `POWER(x,2)*POWER(y,2)`, `a*POWER(x,2)`,
`POWER(POWER(x,2),2)`) are rejected rather than silently misclassified as a
lower-degree Q or a bilinear with a garbage coefficient.

### Rebuilding is never hand-assembled

`RebindOperator(context, name, children)` (and its `RebindMultiply` wrapper)
re-resolve an operator against the children it is actually given, via
`FunctionBinder::BindScalarFunction`. **Every rebuild of a reshaped subtree must go
through it.**

Hand-assembling a `BoundFunctionExpression` from another node's `function` /
`return_type` / `bind_info` does not fail when the children's types disagree — it
reinterprets their *physical* representation, returning a plausible wrong number
and potentially reading past the end of a narrower vector. DECIDE has hit that
failure mode three times. Two things guarantee a mismatch after a rewrite:
distribution replaces a factor with one of its addends (narrower than the sum it
came from), and dropping a factor shifts the survivors out of alignment with
`function.arguments`. This is why `ExtractTerms` and
`ExtractCoefficientWithoutVariable` take a `ClientContext`.

### Helpers

| Function | Purpose |
|---|---|
| `FindDecideVariable(expr)` | First `BoundColumnRefExpression` matching a DECIDE variable, or `INVALID_INDEX` |
| `ContainsVariable(expr, var_idx)` | Whether a specific variable appears |
| `IsLinearInDecideVars(expr)` | Degree ≤ 1 in the decision variables |
| `ExtractCoefficientWithoutVariable(ctx, expr, var)` | `x * 5 * l_tax` → `5 * l_tax`; constant 1 when `expr` *is* the variable |
| `ExtractTerms(ctx, expr, out)` | Decompose a reducer argument: `+` recurses, binary `-` flips sign, `*` splits variable from coefficient, a decision-free cast stays whole as a typed fixed term (peeling it would change the value — `CAST(1.6 AS INTEGER)` is 2), a decision-bearing cast is peeled |
| `TryDistributeMultiplyOverAdd` | Distributes a product over a sum, rebinding each node |
| `BuildCoefficientFromFactors(ctx, factors)` | Folds a flattened factor list back into a product |
| `CombineBilinearCoefficients(a, b, mul)` | Merges coefficients from both sides of a bilinear product; no coefficient for `1 * 1` |
| `CollectDecideVarRefs(expr, sign, refs, op)` | Sign-tracking reference collection; its only remaining caller is the C2 guard, which needs emptiness rather than signs |

---

## 4. PHASE 1.5 — entity mappings

For each `EntityScopeInfo`, before coefficient evaluation:

1. Evaluate the entity-key columns per row from the materialized data, using
   `BoundReferenceExpression`s built from `entity_key_physical_indices` so one
   `ExpressionExecutor` covers them all.
2. Concatenate each row's key values into a composite string key with NULL-safe
   tagging, so a real NULL is distinguishable from the string `"NULL"` — the same
   pattern `PER` grouping uses.
3. Assign entity ids in first-seen order via an `unordered_map<string, idx_t>`.
4. Fill a `row_to_entity` vector of size `num_rows`.

The result is an `EntityMapping { num_entities, row_to_entity }` on the
`SolverInput`, which is what makes several rows share one solver column.

---

## 5. PHASE 2 — coefficient evaluation

### Expression transformation

`BoundColumnRefExpression` nodes reference columns by table binding;
`ExpressionExecutor` needs chunk positions. `TransformToChunkExpression` converts:

| Node | Becomes |
|---|---|
| `BoundColumnRefExpression` | `BoundReferenceExpression` on `binding.column_index` |
| `BoundFunctionExpression` | recurse into children, copy `bind_info` |
| `BoundCastExpression` | recurse into child, re-wrap with `AddCastToType` |
| `BoundAggregateExpression` | a reference to an extra chunk column holding that reducer's per-group value, cast back to the reducer's own type (see `EvaluateRhsReducerPerGroup`) |
| anything else | copied as-is |

A reducer reached with no substitution prepared — inside a `WHEN` condition or a
coefficient, where it has no meaning — is rejected with `InvalidInputException`.

Results are cached per `Finalize` call, and the cache outlives every
`ExpressionExecutor` created below it, so executors may safely retain references
into cached entries.

**Precondition: resolution has already happened.** The `BoundColumnRefExpression`
branch uses `binding.column_index` as a chunk position, and the two indexings agree
only because `ColumnBindingResolver` already rewrote DECIDE's expressions. That
enumeration is maintained by hand — see
[`../03_logical_plan/done.md`](../03_logical_plan/done.md) §4.

### Per-term coefficients

Each term's coefficient expression is transformed, executed chunk by chunk against
`gstate.data`, and cast to `double` via `DefaultCastAs(LogicalType::DOUBLE)` —
**one conversion, on the result of the complete typed expression**, which is what
keeps a data cast's SQL semantics intact. Values accumulate into
`row_coefficients[term_idx]` indexed by global row.

Data-only LHS terms carry `INVALID_INDEX`. For aggregate constraints those fixed
coefficients are still evaluated per active row; the model builder subtracts their
row or group sum from the RHS.

### RHS

- **Constant** — extracted once and broadcast.
- **Expression** — evaluated per row, giving row-varying bounds.
- **Reducer** — evaluated per group and by reducer kind
  (`EvaluateRhsReducerPerGroup`).

The canonical RHS classification decides which: a bound tagged
`QUERY_WIDE_BOUND_TAG` is a shared scalar; anything with a row-varying component
stays data-backed.

### `WHEN` and `PER` → `row_group_ids`

One per-row vector assigns each row to a constraint group or excludes it:

| Case | `row_group_ids` | `num_groups` |
|---|---|---|
| Neither | empty (fast path — all rows are one implicit group) | 0 |
| `WHEN` only | 0 where true, `INVALID_INDEX` where false or NULL | 1 |
| `PER` only | first-seen id per distinct key; NULL key → `INVALID_INDEX` (matching SQL `GROUP BY` NULL semantics) | K |
| Both | `WHEN` filters first, then `PER` groups the survivors | K |

`PER` keys use a `string`-keyed hash map with the same NULL-safe tagging as entity
mapping. Unfiltered `PER` assignments are cached across every constraint and the
objective, so one `PER` spec is evaluated once however many clauses use it.

### Aggregate-local `WHEN` masks

Term-level filters are carried on individual terms, not on the whole constraint:

1. Each distinct filter expression is evaluated once into a boolean row mask.
2. Linear, bilinear and quadratic terms get coefficient `0` on rows their own
   filter excludes — **for that term only**.
3. With `PER` present, a row joins a group only if it passes the expression-level
   `WHEN` (if any), has a non-NULL `PER` key, and participates in at least one
   aggregate-local term.

So one clause can carry independent filters:

```sql
SUM(x * a) WHEN active + SUM(x * b) WHEN priority <= 10 PER grp
```

`active` filters only the `a` term, `priority` only the `b` term, and `PER grp`
groups rows participating in at least one.

### Relation-qualified reducers

`SUM(D: expr)` asks for one contribution per **tuple identity** of `D`, but the
join repeats each `D` tuple once per matching row. De-duplication keeps the first
row of each identity and zeroes the rest — sound because the binder already
guarantees every row sharing an identity carries the same value, so whichever row
is kept gives the same answer.

De-duplication runs **inside** the `PER` partition, after `WHEN` selection and
`PER` partitioning, which is the construction order the paper pins. Group ids are
dense in `[0, num_groups)` and entity ids in `[0, num_entities)`, so
`(group, entity)` flattens into one dense index with no hashing.

### AVG scaling

`AVG` was rewritten to `SUM` by the optimizer and tagged `AVG_REWRITE_TAG`. Terms
marked `avg_scale` are divided by the active row count of the applicable scope:

| Scope | Denominator |
|---|---|
| Ungrouped | total active rows |
| Expression-level `WHEN` | rows passing the mask |
| `PER` | group size |
| Aggregate-local `WHEN` | rows passing that filter, within the group if `PER` is present |

Quadratic AVG terms scale their inner linear coefficients by `1/√N`, so the
resulting outer product is scaled by `1/N`.

### Validation

Every coefficient and RHS value is checked after evaluation:

- **NULL** → `InvalidInputException` suggesting `COALESCE()` or a `WHERE` filter.
- **NaN / Infinity** → `InvalidInputException` naming the common causes (division
  by zero, arithmetic overflow, NULL propagation through math).

Both apply to constraint and objective coefficients alike.

### Output

A `SolverInput` carrying `num_rows`, `num_decide_vars`, `variable_types`
(reported as `BOOLEAN` wherever `is_boolean_var` is set), resolved
`lower_bounds` / `upper_bounds`, the `EvaluatedConstraint` vector, objective
coefficient and variable-index arrays, entity mappings, and `sense`.

When the objective is quadratic, the same evaluation runs on `squared_terms` **in
addition to** `terms`, not instead of: `EvaluateObjectiveTermList` is called once
per non-empty list, filling `objective_coefficients` / `objective_variable_indices`
for the linear half and `quadratic_inner_coefficients` /
`quadratic_variable_indices` for the quadratic half. The expression-level `WHEN`
mask and each term's local filter are applied independently to each list.

`CoefficientColumn` keeps this compact: a column that is uniform across rows — a
broadcast Big-M coefficient, a constant RHS, a McCormick constant — stores one
scalar rather than `vector<double>(num_rows, K)`. Reads via `Get(row)` are
branchless on the storage kind, and mutation lazily promotes Scalar → Dense. A
third kind, `SparseMasked`, stores a uniform value at a sorted index set with
every other row implicitly 0, built for the `<>` per-row indicator path where
every active row gets `-M` and every excluded row 0.

---

## 6. PHASE 3 — build and solve

Hands off to stage 06 ([`../06_model_formulation/done.md`](../06_model_formulation/done.md))
and stage 07 ([`../07_solver/done.md`](../07_solver/done.md)). Also emitted here:
data-driven Big-M refill for auto-`M` links, composed MIN/MAX row emission, the
deferred `<>` expansion, and the ABS `MAXIMIZE` upper-bound derivation.

A slow-solve checkpoint report runs when a solve exceeds its budget; see
[`../../07_query_diagnostics/slow/done.md`](../../07_query_diagnostics/slow/done.md).

---

## 7. Readback (`GetData`)

`DecideGlobalSourceState` holds a scan over `gstate.data` and a
`current_row_offset`. `MaxThreads()` returns 1, so the sequential mapping between
data rows and solution values is exact.

Each call scans the next chunk, fills the DECIDE columns — the last
`total_decide_vars` columns of the output — and advances the offset. The mapping
is:

```
global_row   = current_row_offset + row_in_chunk
solution_idx = gstate.var_indexer.Get(decide_var_idx, global_row)
value        = ilp_solution[solution_idx]
```

For **row-scoped** variables each row has its own column. For **entity-scoped**
variables every row of an entity resolves to the same column and therefore
receives the same value — which is the whole semantic point of a table-scoped
variable. For **scalar** variables every row resolves to the one column.

The readback indexer is the owning `VarIndexer::Build()` form, so it outlives the
`SolverInput`.

### Type-specific projection

Solvers return doubles, and a MILP may return `0.9999999` for an integer. The
branch is on the variable's **output `LogicalType`**, not on how it was declared:

| Output type | C++ type | Projection | Reached by |
|---|---|---|---|
| `INTEGER` | `int32_t` | `round(value)` | `x(INT)` **and `x(BOOL)`** |
| `BIGINT` | `int64_t` | `round(value)` | — |
| `BOOLEAN` | `bool` | `value >= 0.5` | optimizer-created BOOLEAN auxiliaries |
| `DOUBLE` | `double` | direct, no rounding | `x(REAL)` |
| default | `int64_t` | as BIGINT | — |

A declared `x(BOOL)` therefore comes back as `int32` `0`/`1`, not as SQL `true`/
`false`: `BOOL` is a *domain* carried by `is_boolean_var`, and the variable's
DuckDB-facing type stays `INTEGER` so it can appear in arithmetic. The `BOOLEAN`
row is reached only by auxiliaries the optimizer declares with that type
outright.

Each output vector is `FLAT_VECTOR`, written through `FlatVector::GetData<T>()`.

### Auxiliary variables

`GetData` projects **all** variables, user and auxiliary. The `Projection`
operator that `plan_decide.cpp` places above `PhysicalDecide` prunes the auxiliary
columns, so only user-declared variables reach the result. Auxiliaries occupy the
last `num_auxiliary_vars` slots.

---

## 8. Source map

| Concern | Location |
|---|---|
| Everything above | `src/execution/operator/decide/physical_decide.cpp` |
| `Term`, `DecideConstraint`, `Objective`, operator fields | `src/include/duckdb/execution/operator/decide/physical_decide.hpp` |
| `SolverInput`, `EvaluatedConstraint`, `CoefficientColumn` | `src/include/duckdb/decidb/solver_input.hpp` |
| Logical → physical, entity key indices, input column names | `src/execution/physical_plan/plan_decide.cpp` |
| Binding resolution shielding | `src/execution/column_binding_resolver.cpp` |
