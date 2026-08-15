# Stage 06 — Model formulation

Translates the evaluated, solver-neutral `SolverInput` into a `SolverModel`: flat
variable arrays, constraints in COO, and an optional Q matrix. It owns the
variable layout, coefficient accumulation, bounds and indexing, and it owns the
**linearization** of the formulations stage 05 chose — the rows that encode a
tagged construct and the Big-M constants that scale them. It does no
SQL-expression canonicalization and knows nothing about any backend.

**Key source files**

- `src/decidb/utility/ilp_model_builder.cpp` — `SolverModel::Build()`
- `src/decidb/utility/ilp_linearization.cpp` — Big-M constants, hard MIN/MAX rows
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
*not* exact for continuous ones, so `IsEvalConstraintLhsIntegerValued` gates the
rewrite and reports the two ways an LHS can fail it differently, because only one
of them is still the user's to fix here:

- **A fractional coefficient** (`SUM(0.5 * x) < 5` on an INTEGER `x`, or any data
  column that evaluates to a non-integer) is knowable only now, once coefficients
  have been evaluated. It is an `InvalidInputException` naming the coefficient and
  suggesting `<=`.
- **A REAL decision** is knowable from the declared type, and stage 02 rejects it
  during binding (`ValidateDecideNoStrictComparisonOnReal`), where the variable and
  the clause can be named. Reaching this point with one is an invariant violation,
  so it throws `InternalException`. The check stays rather than being deleted: if a
  future rewrite types an auxiliary REAL inside a strict constraint, this fails
  loudly instead of silently stepping a continuous bound.

A strict `<` / `>` over a REAL variable is also deliberately not absorbed into
column bounds upstream (stage 05), for the same reason the step is unsafe.

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

## 9. Linearization

`src/decidb/utility/ilp_linearization.cpp` holds the half of a formulation that
only becomes writable once coefficients are numbers. Stage 05 decides *which*
encoding a construct gets and records it as a tag on the constraint; this unit
turns the tag into rows. Everything in it is a pure function of `SolverInput`
data — evaluated coefficients, variable bounds, row and group ids. No expression
tree, no executor, no data scan.

### Implied bounds

`DecidePropagateImpliedBounds()` derives a second, data-driven source of column
bounds. For a constraint `Sum_t a_t x_t (<=|=) K` where every variable is
non-negative and every coefficient is non-negative, each instance satisfies
`x <= K / a` — the other terms only help — so the shared upper bound is
`max_r K_r / a_r`. This turns declared-unbounded variables into bounded ones,
which is what makes a finite and tight Big-M possible. Only provably-implied
bounds are applied, so the feasible region and the optimum are unchanged.

`a_r` is the variable's **combined** coefficient at that row. A variable can hold
several additive terms of one constraint (`2*ship + 3*ship <= 10`, or two reducers
over the same decision), and all of them name the same solver column, so the pass
walks distinct variables and sums every term naming each one — `10/5 = 2`, not
`10/3`. The addition belongs here rather than in canonicalization because
coefficients are evaluated numbers by this point; at stage 04 they are still
unevaluated expressions over data columns, and combining them there would mean
opening terms algebraically, which that stage does not do.

The pass is a single sweep, not a fixpoint: a bound derived for one variable is
not fed back to tighten others in the same pass. That is sound and only leaves
tightness on the table for chained implications.

### Big-M constants

`DecideTightPerRowBigM()` returns the maximum over active rows of the row's
effective bound magnitude plus its worst-case term contribution, plus a 1-unit
margin covering the integer-step band of the `<>` rewrite. When every
contributing variable is bounded this is exact and typically far below the
`DECIDE_BIGM_FALLBACK` of 1e6, which is kept only for genuinely unbounded
variables. `DecideRowTermRange()` is the shared per-row worst case, also used by
the ABS-maximize and aggregate `<>` paths that still emit from stage 08.

A non-finite effective bound is refused here, in SQL terms, rather than reaching
the model validator as an internal error. Callers that can read an infinity by
direction classify it first, so what still arrives is a bound with no reading at
all — NaN — and the auto-`M` `norm(e, 0)` links, whose `M` bounds an expression
rather than answering a comparison.

### Hard MIN/MAX rows

`LinearizeMinMaxIndicators()` encodes every constraint stage 05 tagged with
`minmax_indicator_idx`, matching constraints to indicators by tag rather than
position. Untagged constraints pass through unchanged.

| Direction | Per-row rows | Selector row |
|---|---|---|
| `MAX(expr) >= K` | `expr_r - M*y_r >= K - M` | `SUM(y) >= 1` |
| `MIN(expr) <= K` | `expr_r + M*y_r <= K + M` | `SUM(y) >= 1` |

`ClassifyMinMaxBound()` reads an infinite bound by the direction it points before
any `M` is asked for. A hard MAX is "some active row has LHS >= K", so `K = -inf`
holds for every assignment and the group is dropped, while `K = +inf` is out of
every row's reach and the group is re-emitted as a plain per-row constraint
carrying that bound — an ordinary infeasibility naming the user's clause, not a
refusal. MIN is the mirror image. The verdict is per group because
`ReduceAggregateRhsPerGroup` has already collapsed a row-varying bound to the
tightest one, which is also what settles a group whose rows mix finite and
infinite bounds.

Emitted rows carry `ConstraintKind::USER_MECHANISM`: they are rigid mechanism
rows, not user parameters, so diagnosis does not offer to loosen them.

### `<>` disjunctions

`LinearizeNotEqual()` encodes `x <> K` as the disjunctive pair
`x - M*z <= K-1` / `x - M*z >= K+1-M`, where `z` is the binary stage 05 created.
Both rows carry the same `ne_indicator_idx`, which is what lets the elastic engine
group them and offer removal rather than loosening half a disjunction.

Two guards run before any `M` is asked for, and the order matters:

- **`NELhsIsIntegerValued`** refuses a REAL variable or a non-integer coefficient
  outright. The ±1 band is only exact on the integer lattice; on a continuous
  variable it would silently cut the feasible points in `(K-1, K+1)`.
- **`NEIsIntegerValuedRhs`** *drops* a comparison whose bound no integer can equal,
  because every assignment already satisfies it. Emitting the pair anyway would
  wrongly exclude `floor(K)` and `ceil(K)`. An infinite `K` is the same case for the
  same reason, and it must drop here rather than reach the Big-M — the predicate
  requires finiteness outright, since `inf - round(inf)` is NaN and every comparison
  against NaN is false.

A per-row spelling with a row-varying bound masks only its non-integer rows, via
`row_group_ids`, instead of dropping the whole constraint.

**Aggregate spellings cannot expand in place.** They need one *global* binary per
group, and the group's `M` must cover the summed range over its rows — a single
per-row bound is far too small at scale and would silently cap the aggregate. So
`LinearizeNotEqual` moves them to a deferred list and
`ExpandDeferredAggregateNotEqual()` finishes them once `VarIndexer` exists,
emitting `SolverInput::RawConstraint`s in flat column space. Each group allocates
its own `z` and carries the clause text stage 05 recorded, so a dropped aggregate
`<>` can be named in a repair. Groups skipped by the integer-RHS guard allocate no
`z`, so the model stays clean.

### Bilinear products

`LinearizeBilinear()` emits the McCormick envelope for each `w = b * x` link, with
`b` Boolean and `x` in `[L, U]`:

| Row | Condition |
|---|---|
| `w <= U*b` | always |
| `w >= x - U*(1-b)` | always |
| `w <= x - L*(1-b)` | `L < 0`; collapses to `w <= x` when `L >= 0` |
| `w >= L*b` | `L < 0` only |

For `L >= 0` the lower corner is implied by `w`'s own non-negative bound, so three
rows suffice. For `L < 0` all four are emitted and `w`'s own lower bound is widened
so the product can reach the negative value of `x` at `b = 1`. A missing finite
upper bound on `x` is refused by name — there is no envelope without it.

### ABS under MAXIMIZE

Stage 05 emits `aux >= inner` and `aux >= -inner` and tags them via `abs_y_idx` /
`abs_is_pos_bound`. Under MINIMIZE those two alone pin `aux = |inner|`, because the
objective pushes `aux` down. Under MAXIMIZE they do not — `aux` is free to run
above `|inner|` — so `LinearizeAbsMaximize()` pairs the tagged rows and derives the
matching Big-M upper bounds that close it.

This site is **strict about bounds**: unlike the indicator paths there is no
fallback constant, so a contributing variable with no finite bound is named and
refused rather than given `DECIDE_BIGM_FALLBACK`. An infinity in the bound carries
the constant part of the expression the user wrote inside `ABS()`, so the refusal
names that and the row, not the linearization.

---

## 10. Source map

| Concern | Location |
|---|---|
| `SolverModel::Build`, all constraint paths, Q construction | `src/decidb/utility/ilp_model_builder.cpp` |
| Implied bounds, Big-M constants, MIN/MAX, `<>`, McCormick, ABS rows | `src/decidb/utility/ilp_linearization.cpp` |
| `BilinearLinkSpec`, `AbsMaximizeLinkSpec` — the formulation tags | `src/include/duckdb/decidb/solver_input.hpp` |
| `VarIndexer`, `SolverModel`, `ModelConstraint`, provenance | `src/include/duckdb/decidb/ilp_model.hpp` |
| `SolverInput`, `EvaluatedConstraint`, `CoefficientColumn` | `src/include/duckdb/decidb/solver_input.hpp` |
| Golden model corpus (the characterization oracle) | `test/decide/golden/` |
