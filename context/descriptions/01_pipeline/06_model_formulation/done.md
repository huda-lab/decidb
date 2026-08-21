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
are converted by adjusting the RHS, which is exact for integer variables and not for
continuous ones, so `IsEvalConstraintLhsIntegerValued` gates the rewrite.

**That gate raises no user-facing error.** The whole integer-step refusal is stated on
declared types at stage 02, by `ValidateDecideNoIntegerStepComparisonOnReal` for the
decision and `ValidateDecideIntegralComparisonOperands` for every other operand. Either
failure reaching here — a REAL variable or a fractional coefficient — is an invariant
violation and throws `InternalException`.

The check stays rather than being deleted: if a future rewrite types an auxiliary REAL,
or produces a fractional coefficient, inside a strict constraint, this fails loudly
instead of silently stepping a bound that is not on the lattice.

> Moved 2026-08-17. The fractional-coefficient half used to be an `InvalidInputException`
> raised here, because a data column's values are knowable only once evaluated. It was
> moved to a *type* judgement at bind time: reading values made a query's **validity**
> depend on the table's contents, so inserting one fractional row could make a working
> query illegal. See `02_binder/done.md` §4 for the rule and what it costs.

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

Six emission sites stamp this struct — linear and quadratic, each in ungrouped
aggregate / grouped aggregate / per-row shape — and two free functions in
`ilp_model_builder.cpp` own the fields common across that shape matrix rather than
each site repeating them:

- `StampConstraintProvenance(provenance, eval_const, repair_group_id, group_key,
  group_label)` sets `source_clause_id`, `repair_group_id`, `kind`, `shape` +
  `rhs_label`, and — only when the caller passes a real one — `group_key` /
  `group_label`. All six sites call this.
- `StampAggregateProvenance(provenance, eval_const)` sets `is_aggregate` and
  `qualifier`. Only the four aggregate sites (linear and quadratic, ungrouped and
  grouped) call this; a per-row constraint has no reducer to name.

`avg_scaled`, `weight_labels` and `folded_terms` stay hand-stamped at exactly the
two **linear**-aggregate sites and are never set on the quadratic ones. That is
not leftover drift: `FormatQuadraticLhs` (stage 07's diagnosis renderer) never
reads those three fields — it reconstructs a quadratic row's LHS from
`linear_coefficients`/`q_coefficients` directly, the same way `FormatLhs` (the
linear renderer) reads `avg_scaled`/`folded_terms`/`weight_labels`. Stamping them
on a `QuadraticConstraint` would be dead weight with no reader, not a fix. The
comment marking these six sites is numbered 1-6 in emission order (was
non-contiguous 1/2/3/5/6/7 before this consolidation, from a since-removed site
4).

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
margin covering the integer-step band of the `<>` rewrite. `DecideRowTermRange()`
is the shared per-row worst case, also used by the ABS-maximize and aggregate `<>`
paths that still emit from stage 08. `AuxRange::BigM()` is the auxiliary-family
twin, for the MIN/MAX auxiliaries that are pinned against a whole expression rather
than compared to a fixed bound.

**There is no fixed Big-M.** When a contributing variable has no finite bound, no
constant dominates its range, and the query is **refused** — naming a column to
bound and the edit that bounds it. It is not given a large constant instead. That is
the one failure mode worth designing against: a guessed `M` the true range exceeds
does not error, it silently cuts the feasible region and returns a confidently wrong
optimum. `<>` and MIN/MAX used to take a `max(M, 1e6)` floor here while the ABS path
already refused; they now agree, and the 1e6 constant is gone from the tree.

The refusal applies on **both** backends. Leaving one on the floor would mean the
same query answers correctly on Gurobi and wrongly on HiGHS, with no story for the
divergence — and a query's legality is not a property of the machine it runs on.

It fires only when the bound is genuinely unknowable. A box from implied-bound
propagation counts, and so does an ABS auxiliary's box: `LinearizeAbsMaximize`
narrows the auxiliary's column to the range it pins it to, so an outer MIN/MAX over
`ABS(...)` sees a finite column. **That is why ABS is linearized first** — every
linearizer after it derives its `M` from column boxes, so an auxiliary they read must
already be boxed. Run them in the other order and `MAX(ABS(x - 5)) >= 3` over a
bounded `x` is refused for a bound that was there to be computed.

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
infinite bounds. A bound-side reducer is what reaches the mixed case from SQL —
`MIN(x) <= MAX(cap) PER g` where `cap` holds an infinity in one group and finite
values in another — so one group drops while the group beside it still
linearizes. A literal bound gives every group the same verdict.

Emitted rows carry `ConstraintKind::USER_MECHANISM`: they are rigid mechanism
rows, not user parameters, so diagnosis does not offer to loosen them.

### MIN/MAX objectives

`LinearizeMinMaxObjective()` is the objective-side counterpart. A MIN/MAX
objective cannot be a coefficient vector, so it becomes a global auxiliary the
rows pin: `z` for the flat `MAXIMIZE MAX(expr)` spelling, and for the nested
`OUTER(INNER(expr)) PER key` spelling a `z_g` per group plus an outer `w` over
them. The objective is then just that auxiliary.

Each level is *easy* or *hard*. Easy (`MINIMIZE`+`MAX`, `MAXIMIZE`+`MIN`) means
the optimization direction already drives the auxiliary onto the extremum, so one
envelope row per active row is enough — no binaries. Hard (`MAXIMIZE`+`MAX`,
`MINIMIZE`+`MIN`) pushes the auxiliary the other way, so the envelope alone would
let it float; those levels add one indicator binary per active row, a Big-M link
on the opposite side, and `SUM(y) >= 1` to make one row bind. `M` is the global
spread of the objective expression over the rows (`max_r exprmax - min_r exprmin`),
which dominates `|z - expr_r|` at every row.

Rows are skipped where every coefficient is zero, which keeps a binary and a row
off each vacuous row; a `PER` group left with no active row has its `z_g` pinned to
0 by its bounds instead, since the vacuous rows it used to emit were what held it
there. The shape flags (`flat_agg`, `per_inner_agg`, easy/hard per level, the
inner-AVG rewrite) arrive from stage 05 as `MinMaxObjectiveSpec`; the `WHEN` mask
and the coefficients arrive evaluated.

### Composed MIN/MAX

A composed clause mixes reducers additively — `SUM(a) + 2*MAX(b) <= K`, or the same
in an objective. `LinearizeComposedMinMaxConstraint()` and
`LinearizeComposedMinMaxObjective()` give every MIN/MAX term its own global `z_k`
with the same envelope + indicator layer as above, then compose: the constraint
sums the `z_k`s and the SUM/AVG terms into one outer row against the constant RHS,
the objective writes the same composition into `global_obj_coeffs` and
`objective_coefficients`. Each `z_k` is labelled with the user's source text
(`MAX(x)`) through the global-label channel, so diagnosis names the clause rather
than an internal column.

Terms arrive as `ComposedMinMaxTermData`: everything data-dependent — per-row
coefficients, the query-wide factor the canonicalizer peeled off the reducer, the
row mask the reducer runs over — is already evaluated by stage 08, and an empty row
set is refused there before these functions see it.

### `<>` disjunctions

`LinearizeNotEqual()` encodes `x <> K` as the disjunctive pair
`x - M*z <= K-1` / `x - M*z >= K+1-M`, where `z` is the binary stage 05 created.
Both rows carry the same `ne_indicator_idx`, which is what lets the elastic engine
group them and offer removal rather than loosening half a disjunction.

Two guards run before any `M` is asked for, and the order matters:

- **`NELhsIsIntegerValued`** is an invariant check, not a user-facing refusal. The ±1 band
  is only exact on the integer lattice; on a continuous quantity it would silently cut the
  feasible points in `(K-1, K+1)`. Both ways an LHS can fail — a REAL decision, a
  fractional coefficient — are refused on declared types at stage 02, so either arriving
  here raises an `InternalException`. The check is kept rather than deleted so a future
  rewrite introducing a REAL auxiliary or a fractional coefficient inside a `<>` fails
  loudly instead of silently cutting the band.
- **`NEIsIntegerValuedRhs`** *drops* a comparison whose bound no integer can equal,
  because every assignment already satisfies it. Emitting the pair anyway would
  wrongly exclude `floor(K)` and `ceil(K)`. An infinite `K` is the same case for the
  same reason, and it must drop here rather than reach the Big-M — the predicate
  requires finiteness outright, since `inf - round(inf)` is NaN and every comparison
  against NaN is false.

A per-row spelling with a row-varying bound masks only its non-integer rows, via
`row_group_ids`, instead of dropping the whole constraint.

#### The range collapse

`LHS <> K` is a disjunction only when `K` sits strictly **inside** the range the LHS can
actually reach. When the reachable range lies wholly on one side of `K`, one branch is
dead and the survivor is a plain inequality — no indicator term, no Big-M, and an LP
relaxation that is tight instead of empty. `ClassifyNEConstraint` reads the four cases off
the signed interval:

| Range vs `K` | Emitted |
|---|---|
| `K` interior | the two-row Big-M disjunction |
| range never falls below `K` | `LHS >= K+1`, one row |
| range never exceeds `K` | `LHS <= K-1`, one row |
| `K` unreachable | nothing — the row excludes nothing |

This matters because the Big-M pair's relaxation carries no information at all: setting
`z = 0.5` slackens both rows by `M/2`, so the LP admits `LHS = K` — the very value being
forbidden — and the disjunction only starts to bind under branching. The common
`SUM(x) <> 0` over decisions that cannot go negative is exactly the collapsible case, and
it becomes `SUM(x) >= 1`.

`DecideRowSignedRange` computes the interval, respecting coefficient signs where
`DecideRowTermRange` takes magnitudes — a Big-M only has to dominate a range, but a
collapse has to know which side of `K` it lies on. An unbounded side yields an infinite
endpoint rather than declining outright, because both collapses are one-sided: a decision
that is merely non-negative has a finite floor and no ceiling, and that floor alone
licenses `>= K+1`.

**The collapse may only read the rigid box** (`SolverInput::rigid_lower_bounds`), meaning a
variable's intrinsic domain — BOOLEAN 0/1, default non-negativity — and nothing else. The
column box also accumulates user bounds absorbed by stage 05 and implied tightenings from
`DecidePropagateImpliedBounds`, and both are backed by rows the elastic engine *reverts*
during infeasibility diagnosis so the loosenable row is the sole enforcer. A Big-M constant
may read the full box, since a loosened bound only makes it conservative; a rewrite that
changes what a constraint *means* may not, because it cannot be reverted along with the
bound it consumed. So `x <= 5 AND SUM(x) <> 0` collapses on the intrinsic floor, while
`x >= 0 AND x <= 5 AND SUM(x) <> 0` — the same feasible set — keeps the disjunction.

Per-row and aggregate spellings differ in granularity. Groups are already emitted
independently, so each gets the encoding its own range earns. A per-row constraint shares
one `EvaluatedConstraint` across every row, so a mixed verdict would mean splitting it into
up to three constraints with complementary row masks; instead the verdict must be unanimous
across active rows, and a mixed one keeps the Big-M pair unsplit.

A collapsed row keeps its `ne_indicator_idx` even though the indicator no longer appears in
it, and a collapsed aggregate group still allocates its `z`. That is what carries the
clause's label and groups its rows for the remove-only `<>` repair, so diagnosis reads the
same whichever encoding a clause received. The removal engine falls back to a range-derived
`M₂` when it finds no indicator coefficient to read one from — without that fallback the
group would get a coefficient of 0 and the removal would be offered but inert.

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

This site was **strict about bounds** before any other was: a contributing variable
with no finite bound is named and refused. The indicator paths used to take a fixed
constant here instead; they now make the same refusal, and this is the wording they
follow. An infinity in the bound carries the constant part of the expression the user
wrote inside `ABS()`, so that refusal names the expression and the row, not the
linearization.

**Two arms, one tag.** `DeriveAbsAuxiliaryBounds` is phase 1 and runs on both paths;
phase 2 is either `LinearizeAbsMaximize` (the Big-M rows) or `EmitNativeAbs` (a free
column `t`, an equality row `t = inner`, and a `GeneralConstraint` saying `aux = |t|`).
Stage 08 picks the arm from `SolverCapabilities::abs`; both arms read the same stage-05
tag, which is what makes them comparable. The native arm runs later than the lowering
one — after the `VarIndexer` exists — because a general constraint names flat columns.

`refuse_when_unbounded` is the one place the arms differ in *outcome*: the lowering
path has no finite `M` over an unbounded contributor and refuses, while the native path
leaves the auxiliary unboxed and answers. That divergence is the capability's payoff.

The lowering arm also **narrows the auxiliary's column box** to the `M` it derives. The rows it
emits pin `aux = |inner|`, so `M` is a valid upper bound — and the only one anything
derives, since `aux >= inner` / `aux >= -inner` bound the auxiliary from below only.
That box is why this runs before every other linearizer: they all derive their own
`M` from column boxes, and without it an outer MIN/MAX over `ABS(...)` sees an
unbounded column and refuses a query whose bound was computable.

---

## 9a. General constraints — what is NOT a row

`SolverModel::general_constraints` carries the constructs the chosen backend takes
whole. Each record is a kind, a result column, argument columns, and the same
`ConstraintProvenance` every row carries.

It names **columns, not expressions**, and that is the design decision. Every backend's
general-constraint API relates variables to variables, so a record carrying an
expression would force each adapter to synthesize the same auxiliary column and
equality row — that is routing, and routing belongs in the gate. An adapter only
translates, and it never receives a kind its backend did not declare, so it may treat
an unknown one as an internal error. `SolveModel` asserts that contract before handing
the model over.

The linear half of a native construct is therefore an ordinary row emitted alongside
it: `t = inner` for ABS, and one such row per member for a MIN/MAX family, plus the
extremum column the general constraint pins.

Those columns are **boxed by the same `AuxRange` walk the Big-M constant comes from**,
never left free as a matter of course. A free continuous column is a measured
performance cliff (see stage 05's auxiliary-box section), and it is not a consequence
of going native. A column is free only where no range is derivable at all — which is
exactly the query the native path exists to answer, since a general constraint needs no
bound where a Big-M would.

The lowered rows the construct would have produced are **not** also emitted; a model
holds one or the other, never both. The two exact rows stage 05 already wrote
(`aux >= inner`, `aux >= -inner`) do stay: they are implied by `aux = |t|`, they need
no Big-M, and they carry the clause provenance the elastic engine reads.

Two consequences follow from a construct having no rows. `DumpSolverModel` renders
general constraints explicitly, or the corpus diff would read a native construct as
rows quietly disappearing. And `BuildUnboundedRayFallbackModel` **declines** a model
that has any, exactly as it declines a quadratic one: the ray model rebuilds the
matrix row by row, so a non-row cannot come along, and dropping it would relax the
model past what a ray argument permits.

---

## 10. Source map

| Concern | Location |
|---|---|
| `SolverModel::Build`, all constraint paths, Q construction | `src/decidb/utility/ilp_model_builder.cpp` |
| Implied bounds, Big-M constants, MIN/MAX (constraint, objective, composed), `<>`, McCormick, ABS rows | `src/decidb/utility/ilp_linearization.cpp` |
| `MinMaxObjectiveSpec`, `ComposedMinMaxTermData` — what stage 08 hands over | `src/include/duckdb/decidb/ilp_linearization.hpp` |
| `BilinearLinkSpec`, `AbsMaximizeLinkSpec` — the formulation tags | `src/include/duckdb/decidb/solver_input.hpp` |
| `VarIndexer`, `SolverModel`, `ModelConstraint`, provenance | `src/include/duckdb/decidb/ilp_model.hpp` |
| `SolverInput`, `EvaluatedConstraint`, `CoefficientColumn` | `src/include/duckdb/decidb/solver_input.hpp` |
| Golden model corpus (the characterization oracle) | `test/decide/golden/` |
