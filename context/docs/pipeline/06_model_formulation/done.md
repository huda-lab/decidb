# Stage 06 — Model formulation

Translates the evaluated, solver-neutral `SolverInput` into a `SolverModel`: flat
variable arrays, constraints in COO, and an optional Q matrix. It owns the
variable layout, coefficient accumulation, bounds and indexing, and it owns the
**linearization** of the formulations stage 05 chose — the rows that encode a
tagged construct and the Big-M constants that scale them. It does no
SQL-expression canonicalization and knows nothing about any backend.

**Key source files**

- `src/decidb/formulation/ilp_model_builder.cpp` — `SolverModel::Build()`
- `src/decidb/formulation/ilp_linearization.cpp` — global-auxiliary allocation and
  `LowerDecideConstructs`, the entry point; the passes themselves live alongside it in
  `linearization_bigm.cpp` (Big-M constants and the per-row range walks),
  `linearization_minmax.cpp`, `linearization_not_equal.cpp` and
  `linearization_bilinear_abs.cpp`
- `src/include/duckdb/decidb/formulation/ilp_linearization_internal.hpp` — the helpers
  those files share
- `src/include/duckdb/decidb/formulation/ilp_model.hpp` — `VarIndexer`, `SolverModel`, provenance
- `src/include/duckdb/decidb/formulation/solver_input.hpp` — the input contract

`Build()` takes `SolverInput` by **non-const** reference so raw global constraints
can be moved rather than copied, plus a pre-built `VarIndexer` threaded in from
`PhysicalDecide::Finalize()` so it is constructed exactly once.

---

## 1. `VarIndexer`: the four-block layout

```
Block 1: per-row vars (ROW)        [0, num_rows * num_row_vars)
Block 2: keyed vars (ENTITY)       sum of num_entities per scope
Block 3: query-wide vars (GLOBAL)  one column each
Block 4: global auxiliaries        ABS / MIN-MAX / McCormick auxiliaries
```

Scalars sit **below** `global_block_start` deliberately: the invariant "user
decide-variable columns occupy `[0, global_block_start)`" is relied on by the
model builder and by the column labeller in diagnostics.

| Block | Index formula |
|---|---|
| Per-row | `row * num_row_vars + row_var_offset[var_idx]` |
| Keyed | `entity_var_base[var_idx] + row_to_entity[row]` |
| Query-wide | `scalar_column[var_idx]` — `row` is ignored |
| Global | `global_block_start + g` |

Three accessors matter:

- **`Get(var_idx, row)`** — resolve through a *result row*. A scalar ignores `row`;
  every row resolves to the same column.
- **`InstanceColumn(var_idx, inst)`** — walk a variable's *own* instances: rows for a
  per-row variable, class ids for a keyed one, the single column for a query-wide one.
- **`NumInstances(var_idx)`** — how many instances that variable has.

Construction and ownership:

- **`VarIndexer::Build()`** copies entity mappings so the indexer owns every part of
  its lookup state and can outlive the `SolverInput`. It is built once early in
  `Finalize`, threaded through `SolveModel` and `Build`, then moved onto
  `gstate.var_indexer` for readback in `GetData`. `total_vars` is refreshed just
  before the solve, once every auxiliary global has been appended.

There is no non-owning constructor. Focused tests use the same owning path as
production, so entity-mapping lifetime has one contract everywhere.

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

Bounds and type flags then expand across `NumInstances(var)` for each variable, each
instance's floor read through `SolverInput::InstanceLower`.

**The instance box refines the decision box.** A constant lower bound under a `when` or
a reducer filter (`EvaluatedConstraint::lifted_floor`) replaces the default floor of 0 on
the instances its kept rows reach, and only there (`ResolveLiftedFloors`,
`instance_floors.cpp`, run after the rigid snapshot and before implied bounds). A keyed or
query-wide instance is lowered when any of its rows is kept. The box stays one per
decision (a Flyweight), and `instance_lower_bounds[v]` refines it:
- `lower_bounds[v]` is the lowest floor of any instance: a lowered instance's tightest kept
  bound, 0 for an instance no such bound keeps. It is therefore a valid floor of every
  column, and every pass that reads the decision's box stays sound.
- `instance_lower_bounds[v][i]` is 0 where the default floor holds and −∞ where the
  instance follows the decision's box. An instance's floor is
  `max(instance_lower_bounds[v][i], box[v])`.
- The exact value of a lowered instance (−5, or −3 where two bounds keep a row) comes from
  the clause's own row, which `TryFoldPerRowBound` folds into the column, and which a
  diagnosis can loosen.
- `rigid_lower_bounds[v]` is re-opened, as for a folded user bound. A default instance
  stays at 0 through its refinement, so a diagnosis widens only the lowered instances and
  never quotes the default floor.

Storing "default or follow" per instance, rather than each instance's value, keeps the
refinement valid when a later pass moves the decision's box: `DeriveReducerValueBounds`
raises a helper's floor after this runs, and `OpenElasticColumnBox` widens it for a
diagnosis. A decision with a lower bound written (an unconditional floor, a BOOL, a signed
helper) never takes the default, so a conditional floor only intersects with it and the
pass skips it. The Big-M sizings (`FlatColumnBox`, `DecideTightPerRowBigM`) and the `<>`
range collapse read each instance's floor through the same accessor, so a default
instance is sized and collapsed from 0.

Global auxiliaries take their own bounds and types from
`input.global_lower_bounds` / `global_upper_bounds` / `global_variable_types`. A helper
decision of the value rewrite (`SolverInput::reducer_value_links`) holds a reducer's
value, which may be negative: stage 05 lists it in `signed_helper_vars`, so its box has no
default floor when the rigid box is captured, and `DeriveReducerValueBounds` (§9) boxes it by its members' reach. The rigid box
therefore holds a helper open below, so a diagnosis never lifts a floor it never had.

---

## 3. Objective

A flat `obj_coeffs` of size `total_vars`, zero-initialized. For each term,
`objective_coefficients[term][row]` is placed at `Get(var_idx, row)`.

For a keyed variable, every row of a class maps to the same column, and their
coefficients **accumulate** — which is exactly right: one keyed decision participates
in the objective across all of its rows. Every merged sum grows through one helper,
`AddCoefficient(target, value, written)` (`ilp_model.hpp`): the accumulator, the per-row map,
the objective's linear, square and bilinear sums, quadratic rows, fixed terms moved to the
bound side, implied bounds, `<>`, MIN/MAX rows and value links. A sum past a DOUBLE raises
`DecideInvalidValueMessage`'s TOO_LARGE role naming the clause (`constraint_sources`) or the
objective (`objective_written`), never an InternalException or a solver API error.

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

Anything else — keyed or query-wide variables, or a repeated decide variable — falls
back to `SparseCoeffAccumulator`, which handles `(entity, row)` collisions. The
accumulator picks one strategy per constraint and reuses scratch across groups:
a dense `vector<double>` indexed by flat var index plus a `touched` list when the
decide-variable index span fits the cap (~1M slots), or a sorted/merged
`(idx, coeff)` pair list otherwise.

Both aggregate branches use the same selector.

### A term beside the reducers: counted once by execution

The builder sums every row's coefficient of every term. A term outside the reducers
(`SUM(x) - max_shortfall <= cap`, `MAXIMIZE sum(x) + w * s`) counts once: execution kept
its coefficient on one row, the first kept row or its group's first row, and zeroed the
others (`CountOnce`, `../08_execution/done.md`). A query-wide decision inside a reducer
(`SUM(opening_cost * cap)`) keeps every row, so its coefficient is the sum of the counted
rows' data. The rule has one owner, so a writer cannot leave the builder a term whose
count it reads from elsewhere; a debug build checks that no term outside the reducers
reaches the builder on two rows of a group (`VerifyCountedOnce`). `CanUseRowScopedFastPath()`
still sends a query-wide term to the accumulator, since its one solver index would collide
with itself under the fast path. See `../../language/functions.md` ("Query-wide decisions").

### Path 1 — aggregate, ungrouped

One `ModelConstraint` summing over all rows. RHS is `rhs_values[0]`, then fixed
LHS aggregate terms (`variable_index == INVALID_INDEX`) are summed over all rows
and subtracted.

### Path 2 — aggregate, grouped (a `when`, a reducer's own `when`, or a `by` key)

A `group_to_rows` index is built, skipping `INVALID_INDEX` rows, and one
`ModelConstraint` is emitted per non-empty group. Only that group's rows
contribute; fixed LHS terms are summed over that group's rows and subtracted.

### Path 3 — per-row

One `ModelConstraint` per row of the clause's row map (`row_group_ids`): a kept row, a
member of an easy MIN/MAX, or under `per K:` a class's first kept row; every other row is
skipped. RHS is `rhs_values[row]`; fixed LHS terms are subtracted one row at a time.

**A row that bounds one decision goes into its column box instead** (`TryFoldPerRowBound`).
When the clause is a bound on one decision (`x <= cap`, `2 * x >= lo`, `cap * x <= 10`,
`0.1 * x = c`), stage 08 has already evaluated, on each row, the values of `x` the comparison
admits (`EvaluatedConstraint::admitted_lower` / `admitted_upper`, from the classifier
`DecideReadSingletonRow`, `../05_optimizer/done.md`). The builder intersects that column's
`col_lower` / `col_upper` with them and emits no row. This is the per-row counterpart of
singleton-row folding, which folds a literal bound once per decision in the optimizer; a
data-valued bound differs per row and is known only here. The fold:

- **narrows, never replaces.** The admitted range is intersected with the box it finds, so it
  keeps the meaning of the row: a negative data value does not lower a decision's floor of 0,
  unlike a literal `x >= -3` (D41). A constant floor under a `when` has already replaced
  the default on the instances it keeps (§2), so its row sets their exact value. Several
  rows on one column (an entity-scoped decision, a keyed decision) all hold.
- **admits what DuckDB admits.** A whole-numbered decision's range ends on whole numbers that
  pass the written comparison as DuckDB evaluates it, so `0.1 * x <= c` with `c = 0.3` admits
  3, as SQL does; the builder does no division or rounding of its own.
- **leaves an open end open.** `x <= +inf` admits every value, so the box stays as it is. An
  end that admits nothing (`x >= +inf`) keeps the row, which `SolverModel::ProvesInfeasible`
  answers before any backend.
- **does not run under diagnosis** (`keep_singleton_rows`, set by the `DIAGNOSE`
  prefix). The elastic engine relaxes rows and quotes the clause from their provenance
  (`x <= cap_col + delta`), so a diagnosed statement keeps the rows.

A row with two decisions stays a row. A box the fold inverts is caught by the sanity check
(§8), which answers INFEASIBLE as the row would have. Constants derived from the formulation
box (Big-M, McCormick) are computed before the fold, against a box at least as wide, so they
stay valid. Gurobi's presolve turns such rows into bounds on its own; HiGHS's QP solver does
not, and a 9,600-row box-bounded QP that ran past 60 s as rows solves in 0.4 s as bounds. The
golden corpus (cases 102 to 110) shows each branch: lower, equality, coefficient -1,
coefficient 2, two decisions, an infinite bound, `DIAGNOSE`, a data coefficient that changes
sign, and a DECIMAL coefficient.

AVG terms are already coefficient-scaled by stage 08, by the rows the reducer reads in
its group: the kept rows of its `by` group, less those its own `when` drops, one per class
under its own `per` (`../08_execution/done.md` §5). The builder consumes the evaluated
coefficients directly.

### Quadratic and bilinear constraints

Constraints with bilinear terms or `POWER(..., 2)` go through the quadratic
constraint builder into `model.quadratic_constraints`. Linear fixed LHS terms use
the same `INVALID_INDEX` convention and are subtracted from the RHS over the
selected row set before the quadratic constraint is finalized. A decision beside the
reducers enters once per row set, because execution left it on one row (`CountOnce`):
`SUM(POWER(x, 2)) + s <= 30` is not `... + n*s`.

### Normalization on emission

Every path pushes through `PushNormalizedConstraint()` rather than
`model.constraints.push_back()`. It drops empty-LHS tautologies (`0 <= k` for
`k>=0`, `0 >= k` for `k<=0`, `0 = 0`).

A *violated* empty-LHS row — `0 <= -1` after bounds folding and term
cancellation, as written by `SUM(0 * x) <= -1` or `x - x <= -1` — is **kept**
(`ConstantRowFails`, which allows 1e-9 for the cancellation). A singleton row whose
coefficient is 0 on a data row (`cap * x <= -1e-10` where `cap` is 0) is judged by
DuckDB's own comparison instead, through the values the classifier admits (all or
none), so `-1e-10` fails as SQL says. The verdict is stored on the row
(`ModelConstraint::constant_fails`) and read by `ProvesInfeasible`. The row stays
coefficient-free and keeps its own
`source_clause_id`, which is what lets the infeasible-diagnosis engine name the
clause and report a least-change repair (`SUM(0 * x) <= 0`) instead of a bare
status. The infeasibility is decided by DeciDB, not by a solver:
`SolverModel::ProvesInfeasible` reads the model's rows, and `SolveModel` answers
INFEASIBLE on it, so no backend is handed a row with no coefficients.

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

Whether a coefficient may be fractional is a *type* judgement made at bind time, not a
value check here: reading values would make a query's **validity** depend on the table's
contents, so inserting one fractional row could make a working query illegal. See
`../02_binder/done.md` §4 for the rule and what it costs.

A strict `<` / `>` over a REAL variable is also deliberately not folded into
column bounds upstream (stage 05), for the same reason the step is unsafe.

---

## 6. Quadratic objective

When `input.has_quadratic_objective`, Q is built for the form
`minimize xᵀQx + cᵀx` — **not** the `(1/2) xᵀQx` form that some solvers take. A
stored value is the plain coefficient of its monomial: `q_vals[k]` multiplies
`x[q_rows[k]] · x[q_cols[k]]` exactly once, so `3·x²` stores 3 and `4·x·y` stores 4.
`ilp_model.hpp` states this convention on the struct, and every backend adapter
converts to its own solver's spelling rather than assuming one (§7 of
`../07_solver/done.md`).

The inner linear expression of `SUM(POWER(expr, 2))` is already evaluated per row
in `quadratic_inner_coefficients`; for row *r* it is `Σₜ a_{t,r}·x_{varₜ}`. Q is
the sum of outer products across rows:

1. **Variable terms** — `Q[i,i] += aᵢ²` on the diagonal and `Q[i,j] += 2·aᵢ·aⱼ`
   off it. The factor of 2 is not a convention choice: expanding the square
   produces the pair `aᵢ·aⱼ·xᵢxⱼ` twice, once each way round, and only one
   triangle is stored.
2. **Constant terms** — a constant `c` in the inner expression expands as
   `(expr + c)² = expr² + 2c·expr + c²`, so the cross-terms `2c·aₜ` are added to
   `obj_coeffs`.
3. **Storage** — accumulated in a `std::map<pair<int,int>, double>` over the lower
   triangle, then serialized to `q_rows` / `q_cols` / `q_vals`.

**Convexity is guaranteed by construction**: `Q = Σ a·aᵀ = AᵀA` is positive
semidefinite. That is the payoff of enforcing convexity through the syntax rather
than checking it numerically.

It is also, for a `POWER` group spanning more than one decision variable, *singular*
— one row of `A` gives a Q of rank one, whose minimizers form a flat valley rather
than a point. Gurobi handles that. HiGHS is declared not to support the
`singular_quadratic` model class, so stage 05 refuses the query before rows are read;
see `../05_optimizer/done.md` → "The model-class gate".

---

## 7. Provenance carried on every row

`ConstraintProvenance` (`ilp_model.hpp`) travels with each emitted constraint so
diagnosis can report at the **user-clause** level rather than at raw matrix rows:
the source comparison for display, `repair_group_id` for elastic grouping (execution
numbers every clause it evaluates -- the prepared clauses, then under DIAGNOSE the
folded (singleton-row) bounds' clauses, then the composed clauses, whose id
`LinearizeComposedMinMaxConstraint` takes as given -- so no two clauses share one), the
group id (a `by` group or a `per` class) and its printable key, `ConstraintKind` (user
parameter vs rigid mechanism row), the elastic shape (does this row share one slack with its
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
  `group_label`. All six sites call this; the four aggregate sites then set
  `class_set` from `EvaluatedConstraint::group_class_set` (`ClassSetOf`), the index of
  the group's classes in `SolverModel::class_sets` (moved from `SolverInput::class_sets`
  by `Build`; D28), each of a `Kind`: `KEY_VALUES` (a `per K:`'s values), `KEPT_ROWS` (no
  `per` key) or `WHOLE_CLAUSE` (`per ()`, one class with no key value). The per-row
  sites set it for an easy MIN/MAX's member rows from their group and for a `per K:`
  clause's row from its class, then stamp `data_row` (the row it was emitted at,
  DIAGNOSE's `row`) and `per_kept_row` (a row with no class set: each kept row is a
  class of its own) through `StampDataRow`. A flat row copies
  `RawConstraint::class_set`, which the hard MIN/MAX bound row carries from its group and
  the composed clause's outer row from its evaluated clause
  (`LinearizeComposedMinMaxConstraint`).
- `StampAggregateProvenance(provenance, eval_const)` sets `is_aggregate`. Only the four
  aggregate sites (linear and quadratic, ungrouped and grouped) call this; a per-row
  constraint has no reducer to name.

No site stamps the clause's `when c per K:` qualifier: a diagnosis reads it from the
source registry by `source_clause_id` (`SourceAwareProvenance`,
`decide_diagnostic_render.cpp`), the one place it lives.

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

1. **Bounds** — every variable must have finite bounds. An inverted box
   (`lower > upper`) is kept, so the model survives for the elastic engine;
   stage 07 short-circuits to `INFEASIBLE` without handing it to a backend.
2. **Objective coefficients** — every entry finite.
3. **Constraints** — `indices` and `coefficients` the same size, every index in
   `[0, total_vars)`, every coefficient finite, RHS not NaN (infinity is allowed
   for range representations).

These catch evaluation and model-building bugs where the message can still name
the column, rather than at the solver where it cannot.

---

## 9. Linearization

The `src/decidb/formulation/` linearization files hold the half of a formulation that
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
`max_r K_r / a_r`. A per-row constraint over one variable (`x <= cap`) drops no
other term, so it bounds a signed variable as well, by a bound that may be
negative; an aggregate one (`lhs_is_aggregate`, `SUM(x) = -3`) sums every row's
instance and keeps the sign condition. This turns declared-unbounded variables
into bounded ones, which is what makes a finite and tight Big-M possible. Only
provably-implied bounds are applied, so the feasible region and the optimum are
unchanged.

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

A row outside `row_group_ids` normally means "not constrained", which disqualifies the
clause. `EvaluatedConstraint::rows_stand_with_their_class` says otherwise for a
`per K:` clause with no `when`: it is emitted once per class, at the class's first row,
and every other row carries the same decisions and data (a scope check), so it is
skipped instead.

### The formulation box

Every constant this stage derives — a Big-M, a McCormick envelope — is valid only
against a specific column box. `FormulationBox` (`solver_input.hpp`) names that
dependency, and each constant-deriving entry point takes it explicitly:
`DecideTightPerRowBigM()`, `LowerDecideConstructs()` (through `FlatRowReach` /
`FlatColumnBox`), and `LinearizeBilinear()`. `FormulationBox::OfSolvedModel(input)` is
the box the solved model declares.

Passing the box explicitly makes "which box is this constant valid for?" a question the
call site answers, not one about call order. It matters because the box does move: an infeasibility diagnosis
re-opens every user-folded and implied bound (`OpenElasticColumnBox`, stage 08) and
then runs against a strictly larger box than the retained constants assume.

Three cases, kept distinct:

| Reads | Who | Why |
| --- | --- | --- |
| `rigid_*` | `ClassifyNEConstraint`, `DecideRowSignedRange` | decides *structure* (is a disjunct dead), so the verdict must not turn on a bound a repair may lift |
| `FormulationBox` | the three constant-deriving sites above | a constant belongs to the box it was derived from |
| `lower_bounds` / `upper_bounds` | column declarations (e.g. widening a bilinear auxiliary for a negative `x`) | a write to the box, not a reading of it |

A constant may **not** read `rigid_*`. That box is open wherever the user supplied the
only finite bound, and these sites refuse rather than guess when a box is open, so the
query would be rejected while asking for the bound the user already wrote.

Passing the box explicitly changes no constant on the solved path — that call site passes
`OfSolvedModel`, which is what the sites read before. What it buys is a second formulation
against a *different* box, which is how an infeasibility diagnosis gets constants valid for
the widened box a repair searches instead of inheriting the solved model's. See
[`../../diagnostics/infeasible/done.md`](../../diagnostics/infeasible/done.md)
"The elastic model is a formulation, not a matrix patch".

### Big-M constants

`DecideTightPerRowBigM()` returns the maximum over active rows of the row's
effective bound magnitude plus its worst-case term contribution, plus a 1-unit
margin covering the integer-step band of the `<>` rewrite; its per-row worst case,
`DecideRowTermRange()`, sums each term's largest magnitude and serves only it. Every
other Big-M is sized from the expression's **signed interval**, walked by
`DecideRowSignedRange()`: ABS, `<>` and MIN/MAX share that one walk, and terms that
cancel (`c*x - c*y`) are measured together rather than each at its worst.
`AuxRange::Span()` is the auxiliary-family twin, for the MIN/MAX auxiliaries that are
pinned against a whole expression rather than compared to a fixed bound. An
`AuxRange` covers its members and nothing else: each end starts at the first member
that reaches it, so a family over [100, 200] spans 100. A family that covers no row
stays [0, 0], which the sites that pin an empty auxiliary to 0 rely on.

A tight M does not make a large coefficient safe. Past about `c * bound / K ≈ 5e5` on
HiGHS, an integer decision inside the integrality tolerance times `c` meets the row on
its own, whatever M is; that is precision in the user's numbers, and the large-number
warning (`../07_solver/done.md` §3) names it.

**There is no fixed Big-M.** When a contributing variable has no finite bound, no
constant dominates its range, and the query is **refused** — naming a column to
bound and the edit that bounds it. It is not given a large constant instead. That is
the one failure mode worth designing against: a guessed `M` the true range exceeds
does not error, it silently cuts the feasible region and returns a confidently wrong
optimum. ABS, `<>` and MIN/MAX all refuse; none falls back to a fixed large constant.

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

### Extremum links — the one place a MIN/MAX auxiliary is pinned

Every MIN/MAX in the model is the same statement: some auxiliary column equals the
extremum of a family of linear expressions. `EmitExtremumLink()` is the only thing
that writes one, and `ExtremumLinkSpec` is the only description of one. Six sites
feed it — a hard MIN/MAX *constraint*'s `z` per group, the flat objective's `z`, the
two-level objective's `z_g` per class, the outer `w` over those or over group sums, each
composed term's `z_k`, and a MIN/MAX helper of the value rewrite per group. None writes
a formulation by hand.

A link has **two independent sides**, and which of them the surrounding model already
supplies is a property of the *site*, not of the construct:

- The **envelope** (`result >= member` for MAX) holds the result at or above every
  member. An objective that minimizes it supplies this for free.
- The **closing** side pins the result down onto some member: one Big-M row per
  member and a `SUM(y) >= 1` that makes one bind. An objective that maximizes it
  supplies this for free.

An objective-side link therefore needs exactly one of the two, and which one is the
easy/hard classification stage 05 makes. A composed clause sits in a *constraint*,
where no optimization pressure acts on the auxiliary at all, so its hard direction
needs both. A MIN/MAX constraint needs only the closing side: `MAX(e) >= K` needs
`z <= MAX(e)`, or a `z` inflated past every member would satisfy a bound nothing else
does — but nothing pushes `z` down, so the envelope would be rows for nothing.

**Native is the closing side's alternative, never the envelope's.** The envelope costs
one row per member, needs no constant to dominate anything, and is *implied* by a
general constraint rather than contradicted by it. So where a spec asks for it, it is
emitted on both arms. The choice — `result = MIN/MAX(cols)` stated for the backend, or
the Big-M family that encodes it — is made once, inside `EmitExtremumLink`, and the
rule is `NativeConstructPolicy`: native is the fallback, taken where the family's reach
is underivable and there is therefore no Big-M at all.

A member stage 05 defined for a non-linear body (`max(power(x, 2))`) is a helper column
with no box (`../05_optimizer/done.md`, "Non-linear reducer bodies"), so its family is
always underivable and every extremum over it takes the native arm, the outer extremum of a
two-level objective over defined inner members included. `ExtremumArgumentColumn` hands the
helper itself to the general constraint (a sole column), so no pinned copy is added. Stage
05 refuses such a query on a backend with no native MIN/MAX, so the lowering arm never meets
one.

**`closing_kind` is a diagnosis-visible choice, not bookkeeping.** A closing row carries the
Big-M, and marking it `USER_PARAMETER` offers it to the elastic engine as a clause the user
may be told to edit. It is not one: it encodes `z = MIN/MAX(members)`, its slack is in the
auxiliary's units rather than the user's bound's, and `ExtremumLinkSpec` carries no
`source_clause_id` for a diagnosis to name it by. Both MIN/MAX paths therefore mark closing
rows `USER_MECHANISM`; the user's clause is the outer row, which holds the bound in the
user's own units and already carries its source. See `../../diagnostics/infeasible/done.md`,
I2.e, for what went wrong when the composed path diverged from this.

**A member becomes a column only when it is not one already.** `ExtremumArgumentColumn`
hands the general constraint the member's own column where the member is an exact
renaming — one surviving term, unit coefficient, no constant. `MAX(x)` is that shape,
and pinning `t = x` there costs one column and one equality row per member to say
nothing; a general constraint *reads* its arguments, so presolve cannot substitute the
copies away. Measured on `MAXIMIZE MAX(x)` at 15K rows: 0.97s with the copies, 0.29s
without, same answer. `MAX(2 * x)` and `MAX(x + 1)` are genuine expressions and still
earn a column, boxed by that member's own reach.

**The closing Big-M is resolved by the caller**, because the family a link reduces
over is not always the per-row family: an outer MIN/MAX over group *sums* reduces over
the group-sum family, which leaves any single row's range as soon as a group holds more
than one row. Every caller passes the family its own members live in, and the Big-M and
the blame column both come from it, so a link cannot be scaled off one family while its
members sit in another.

**The value is always `AuxRange::Span()` — the family's full reach, constants
INCLUDED.** A deactivated member row reads `z - expr_var <= M + const`, whose worst case
is `family.hi - member.lo`; `family.hi` belongs to whichever row reaches highest, so one
row's constant is measured against a *different* row's. Only within a single row does a
constant cancel in the `(aux - expr)` difference. A reach that ignores the constants is
sound only while every row carries the same one: for `MAX(x + c)` with `c` a data column
it gives an `M` too small to be valid, and a too-small `M` cuts off legal answers rather
than merely loosening the relaxation. Corpus queries 89–92 cover it.

So `AuxRange` carries one bracket and `Span()` is its only Big-M; there is no
constant-free reading a caller could pick by mistake.

### MIN/MAX constraints

`LinearizeMinMaxConstraints()` turns every clause stage 05 marked as a hard MIN/MAX
into what it says: an extremum column per group, and the user's own bound as a single
row over it.

`ClassifyMinMaxBound()` runs first and reads an infinite bound by the direction it
points, before any `M` is asked for. A hard MAX is "some active row has LHS >= K", so
`K = -inf` holds for every assignment and the group is dropped, while `K = +inf` is out
of every row's reach and the group is re-emitted as a plain per-row constraint carrying
that bound — an ordinary infeasibility naming the user's clause, not a refusal. MIN is
the mirror image. The verdict is per group because the bound-side window rewrite
(`RewriteRhsAggregates`) has already reduced a row-varying bound to the tightest one, which is also what settles a
group whose rows mix finite and infinite bounds. A bound-side reducer is what reaches
the mixed case from SQL — `min(x) by (g) <= max(cap) by (g)` where `cap` holds an
infinity in one group and finite values in another — so one group drops while the group
beside it still linearizes. A literal bound gives every group the same verdict.

**The clause reads the same on both arms, and that is the point of stating it this
way.** Both arms produce `z <op> K`, the line of SQL the user wrote, so an infeasible
MIN/MAX is diagnosed identically whatever backend the host has. Spreading the bound
across a per-row Big-M family (`e_r - M*y_r >= K - M`) would hide `K` inside every row.
The cost is one column and one row per group; the per-member rows carry `z` in place
of the bound.

The outer row carries `ElasticShape::SHARED_SCALAR` for a literal bound, without which
it does not fold: a clause with a `by` key emits one of these per group and they are all
the same literal, so the user edits it once and every group moves. A genuinely per-group
bound stays `PER_ROW_DATA` and reports a virtual offset. The extremum column is
labelled with the text stage 05 recorded (`minmax_clause_labels`), the reducer as the
plan prints it, so a repair names `max(x * c)` rather than an internal column.

A strict bound steps on that row, as `ApplyComparisonSense` does for any other: `z > K`
is `z >= floor(K) + 1`, `z < K` is `z <= ceil(K) - 1`. That is exact because the binder
refused any member that can be fractional, a constant inside the extremum included
(`ValidateDecideIntegralComparisonOperands`, stage 02 §4). The row keeps the written bound
(`RawConstraint::strict`, `typed_k`), so a diagnosis quotes `> K`. A negative factor
arrives inside the extremum (stage 05), so the row's relation is the clause's own.

**No indicator is allocated before the rows that read it.** Stage 05 records only the
marking — `minmax_agg_type`, and a `minmax_clause_idx` naming the clause. The binaries
a Big-M pinning needs are global-block columns `EmitExtremumLink` creates for the
members it actually writes, so the native arm allocates none at all.

### Helper decisions of the value rewrite

Stage 05 replaced each decision reducer of a VALUE-shape clause by a helper
decision (`../05_optimizer/done.md`); stage 08 evaluated that reducer's members per `by`
group into a `ReducerValueLinkSpec` (`solver_input.hpp`): an `EvaluatedConstraint` whose
terms are the body's, nonzero only on the rows the clause and the reducer keep, grouped
by the `by` key over the kept rows, with the link's label and provenance. The helper's
own column enters only here (`linearization_reducer_value.cpp`).

`DeriveReducerValueBounds` brackets, per group, the reducer's value — the sum of the
members' signed brackets for SUM/AVG (an AVG's coefficients already carry the 1/n), their
cover for MIN/MAX — and boxes each helper instance by its own group's reach
(`instance_reach_lower` / `instance_reach_upper`, mapped through `VarIndexer::InstanceOf`),
each end on its own; an end no member bounds stays open. The decision's box is the cover of
every instance's reach, so every pass that reads it stays sound, and only ever tightens.
The reach is not the floors' refinement: a floor holds against any box, the rigid one
included, while a reach is derived from the members' current boxes and holds only for the
formulation it came from. It is written afresh by every `FormulateModel`, so a diagnosis
derives it from its widened members. The model builder, the closing Big-M, `FlatColumnBox`
(through `FormulationBox::InstanceUpper` / `InstanceLower`) and the ABS argument boxes read
the instance box (`InstanceBox` with `reach` set); the `<>` range collapse keeps the rigid
view (`InstanceBox` default) and never reads a reach. A member may be an ABS
auxiliary, a product with a helper (`b * sum(x)`) or hold one, so when a model has helpers
`FormulateModel` alternates `DeriveBilinearAuxiliaryBounds` and `DeriveAbsAuxiliaryBounds`
(neither refusing) with this derivation until no box moves — a reducer holds no reducer, so
the chain is finite — and only then runs the two that refuse (a bilinear factor with no
ceiling names the unbounded member, not the helper). Each ABS derivation starts afresh
(`range_unbounded` is reset), so a range open on one pass may close on the next.

`LinearizeReducerValueLinks` then states each definition over the groups
`BuildMinMaxGroups` splits the members into, consuming the evaluated rows. A SUM/AVG
helper gets one flat row per group in `global_constraints`, on the side stage 05 recorded
(the epigraph form): `value - members' expressions >= members' constants` when the clauses
push the value down (`need_envelope`), `<=` when up (`need_closing`), `=` when both. The
members' columns are accumulated first (a keyed decision may stand in several rows of a
group), STRUCTURAL, with the clause's provenance. A quadratic body is not in the row: stage 05
defined it per row, so the members are columns. A MIN/MAX helper gets one extremum link per
group through `EmitExtremumLink`, its result column the helper's instance for that group, on
the same sides; the closing Big-M is the instance's box end against the family's far end
(the instance's ceiling `- family.lo` for MAX, `family.hi -` its floor for MIN), so a small
group's Big-M is its own reach, not the widest group's. Where that constant does not exist the
lowering arm refuses naming a column to bound and the native arm states the extremum, as
for every other MIN/MAX.

### MIN/MAX objectives

`LinearizeMinMaxObjective()` is the objective-side counterpart. A MIN/MAX
objective cannot be a coefficient vector, so it becomes a global auxiliary the
rows pin: `z` for the flat `MAXIMIZE max(e)`, and for the two-level
`outer(per k: inner(e) by (k))` a `z_g` per class of `k` plus an outer `w` over
them. The objective is then just that auxiliary.

Each level is *easy* or *hard*, and that classification is exactly which of the two
sides of an extremum link the objective supplies for free. Easy (`MINIMIZE`+`MAX`,
`MAXIMIZE`+`MIN`) means the direction already drives the auxiliary onto the extremum,
so the closing side is free and only the envelope is emitted — no binaries. Hard
(`MAXIMIZE`+`MAX`, `MINIMIZE`+`MIN`) pushes the auxiliary the other way, so the
envelope is free and the closing side is emitted instead. `M` is the global span of
the objective expression over the rows (`max_r exprmax - min_r exprmin`, constants
included), which dominates `|z - expr_r|` at every row.

This function builds members and specs; it emits nothing itself. All four of its links
go through `EmitExtremumLink`, so which formulation each takes is decided once, in one
place, by the rule described above — and a query cannot take the native arm for its
inner aggregate and the lowered arm for its outer one, because all four read the same
row family's derivability.

The members are the rows the objective's reducer reads, `MinMaxObjectiveSpec::member_rows`
(stage 08 folds the objective's `when` into the terms' filters), whatever their
coefficients: a row whose data is 0 is the member 0, not an absent one, and every class of
the two-level form, an all-zero one included, is a member of the outer link. A flat
objective with no member row reads no row, so it is NULL and dropped: the model keeps no
objective term (D68). The shape flags (`flat_agg`, `per_inner_agg`,
easy/hard per level, the inner-AVG rewrite) arrive from stage 05 as `MinMaxObjectiveSpec`;
the coefficients arrive evaluated.

### Composed MIN/MAX

A composed clause mixes reducers additively — `SUM(a) + 2*MAX(b) <= K`, or the same
in an objective. `LinearizeComposedMinMaxConstraint()` and
`LinearizeComposedMinMaxObjective()` give every MIN/MAX term its own global `z_k`,
pinned by the same `EmitExtremumLink` every other MIN/MAX uses, then compose: the constraint
sums the `z_k`s and the SUM/AVG terms into one outer row against the constant RHS,
the objective writes the same composition into `global_obj_coeffs` and
`objective_coefficients`. Each `z_k` is labelled with its term as the plan prints it
(`max(x)`) through the global-label channel, so diagnosis names the clause rather
than an internal column.

Terms arrive as `ComposedMinMaxTermData`: everything data-dependent — per-row
coefficients, the query-wide factor the canonicalizer peeled off the reducer, the
row mask the reducer runs over, the clause's `when` folded in — is already evaluated by
stage 08; the scope checks have refused an empty one. The outer row steps a strict bound
onto the lattice (`> 30` is `>= 31`), keeping the written bound for a diagnosis.

A composed term needs **both** sides of its link in the hard direction, and that is the
one place the two-flag spec earns its keep. An outer `=` needs both sides on *every*
term — the equality drives `z_k` in neither direction, so stage 05 marks each term
hard (`exact_pin`) and the outer row is emitted with sense `'='`, one native equality
row rather than a pair of opposing inequalities the presolver would have to re-pair. The clause sits in a constraint, so nothing
drives `z_k` at all: the envelope holds it against the members and the closing side
pins it onto one. In the easy direction the outer comparison supplies the closing side
and the envelope alone is exact. Every term's reach is walked once by
`ComposedTermRange` and reused three times — as the auxiliary's box, as the closing
Big-M, and as the box of any column the native arm pins — so the arms cannot disagree
about what the term can reach.

### `<>` disjunctions

`LinearizeNotEqual()` encodes `x <> K` as the disjunction it is: two **conditional
rows**, `z == 0 => LHS <= K-1` and `z == 1 => LHS >= K+1`, on a binary of that
instance's own. That is the only spelling it emits. Nothing here asks which backend is
in play and nothing here computes a bound — a conditional row needs no constant to
dominate it, so whether a contributing variable is bounded is a question for
`LowerDecideConstructs` and only for it.

Both halves carry the clause's `indicator_col` **on the row**, which is what lets the
elastic engine group them and offer removal rather than loosening half a disjunction.
That is also why `<>` is stated as a conditional row rather than as a general
constraint: a general constraint carries no row for diagnosis to reach, and dropping
the clause is the only repair a `<>` has.

Two guards run before anything is emitted, and the order matters:

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
  same reason.

A per-row spelling with a row-varying bound masks only its non-integer rows, via
`row_group_ids`, instead of dropping the whole constraint.

**The binary is allocated where the disjunction is written**, one global per emitted
instance, and labelled with the clause text stage 05 recorded (`ne_clause_labels`).
Stage 05 records only the marking, `ne_clause_idx`, and allocates nothing: a row-scoped
Boolean per data row would be wrong for three of the four things a `<>` can become — an aggregate spelling needs one binary per *group*, a collapsed one needs a
column but no disjunction, and a dropped one needs nothing. None of those is knowable
before the data is seen.

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
column box also accumulates user bounds folded by stage 05 and implied tightenings from
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
across active rows, and a mixed one keeps the disjunction unsplit.

A collapsed row **still allocates its binary**, appearing in no row, because the ordinary
formulation keeps the same column layout across equivalent `<>` shapes. Atomic DROP no
longer depends on that column: the source comparison's `removal_group_id` is propagated to
every emitted descendant and the diagnostic candidate physically omits the whole group.
A row dropped as a tautology allocates nothing at all.

**Aggregate spellings expand per group.** They need one binary per group rather than per
row, and their LHS is the group's rows summed onto one row, so `ExpandAggregateNotEqual`
accumulates each group's coefficients and emits the same two conditional rows over the
sum. A decision beside the reducers enters once per group, since execution left it on the
group's first row (`CountOnce`; `SUM(x) + s <> 5` is not `... + n*s`); a clause with one takes no range collapse, since
the per-row
ranges would count it once per row. Groups skipped by the integer-RHS guard allocate no binary, so the model stays
clean.

### Bilinear products

`LinearizeBilinear()` emits the rows of each `w = b * x` link once per instance of `w`, in flat
columns (as a helper's definition is), from the box of the instance of `x` that instance
reads (`VarIndexer::InstanceUnder`). A query-wide product is one block, a keyed one a block per
key, a row-scoped one a block per row (the scope of `w` is stage 05's `JoinScopes`); the
instance fixed at 0 for rows that read no key links nothing. For two Booleans the block is the
AND, `w <= b`, `w <= x`, `w >= b + x - 1`. With `b` Boolean and `x` in `[L, U]` the
McCormick envelope is:

| Row | Condition |
|---|---|
| `w <= U*b` | always |
| `w >= x - U*(1-b)` | always |
| `w <= x - L*(1-b)` | `L < 0`; collapses to `w <= x` when `L >= 0` |
| `w >= L*b` | `L < 0` only |

For `L >= 0` the lower corner is implied by `w`'s own non-negative bound, so three
rows suffice. For `L < 0` all four are emitted and `w`'s own lower bound is widened
so the product can reach the negative value of `x` at `b = 1`. A missing finite
upper bound on `x` is refused by name — there is no envelope without it. A factor that is a
helper has no box until its members' are derived, so the refusal waits for them and names the
member.

### ABS under MAXIMIZE

Stage 05 emits `aux >= inner` and `aux >= -inner` and tags them via `abs_aux_idx` /
`abs_is_pos_bound`. Under MINIMIZE those two alone pin `aux = |inner|`, because the
objective pushes `aux` down. Under MAXIMIZE they do not — `aux` is free to run
above `|inner|` — so `LinearizeAbsMaximize()` pairs the tagged rows and derives the
matching Big-M upper bounds that close it.

**Each sign row has its own M.** `DeriveAbsAuxiliaryBounds` takes the signed interval
`[lo, hi]` of `inner` over every row (`AbsMaximizeLinkSpec::inner_lo` / `inner_hi`). The
row `aux + inner <= A2·y` holds `2·inner` when `y = 1` pins `aux = inner`, so
`A2 = 2·max(0, hi)`; the row `aux - inner <= A1·(1 - y)` holds `-2·inner` when `y = 0`
pins `aux = -inner`, so `A1 = 2·max(0, -lo)`. One shared constant, `hi - lo`, is too
small for an interval that is not symmetric (`[-1, 100]` needs 200 on one side), and a
side the interval never reaches needs no relaxation at all.

The tag keys on the **auxiliary**, not on a sign indicator, because
only the lowering arm has a sign indicator: see "one binary, on one arm only" below.

This site is **strict about bounds**: a contributing variable with no finite bound is
named and refused, and the indicator paths make the same refusal in the same words. An infinity in the bound carries the constant part of the expression the user
wrote inside `ABS()`, so that refusal names the expression and the row, not the
linearization.

**Two arms, one tag.** `DeriveAbsAuxiliaryBounds` is phase 1 and runs on both paths;
phase 2 is either `LinearizeAbsMaximize` (the Big-M rows) or `EmitNativeAbs` (a column
`t`, an equality row `t = inner`, and a `GeneralConstraint` saying `aux = |t|`).
Stage 08 routes on the arm **stage 05 chose** (`use_native_constructs.abs`); both arms
read the same stage-05 tag, which is what makes them comparable. The native arm runs
later than the lowering one — after the `VarIndexer` exists — because a general
constraint names flat columns.

`refuse_when_unbounded` is the one place the arms differ in *outcome*: the lowering
path has no finite `M` over an unbounded contributor and refuses, while the native path
leaves the auxiliary unboxed and answers. That divergence is the capability's payoff.

**One binary, on one arm only.** The sign indicator `__abs_y_N__` exists to switch the
Big-M envelope, and the native arm emits no envelope — it states `aux = |t|`. Stage 05
therefore allocates it only on the lowering arm. It is a **row-scoped** variable, so
leaving it in on the native arm cost one free binary *per data row*, referenced by no
row and no general constraint: presolved away by the solver, so never a wrong answer,
but built, stored and marshalled across the solver API for every row of the input, and
growing with the relation. Suppressing it is only expressible because stage 05 owns the
formulation choice — the arm is known at the moment the variable would be allocated.
`AbsMaximizeLinkSpec::y_idx` is `INVALID_INDEX` on the native arm.

**Each `t` is boxed by its own row.** `EmitNativeAbs` brackets the argument column from
the row's own C1: `inner_r = k_r - sum_{v != aux} c_v x_v`, walked with
`DecideRowSignedRange` (skipping the auxiliary) and folded in with
`AuxRange::CoverRowSided` — the same per-row machinery
`LinearizeMinMaxConstraints` already uses, keeping each end it can derive
independently. One interval per link, the **cover over rows**, would hand every row
but the extreme one more room than it can reach, and a single unbounded row would leave
every row's column free. A slack continuous column is a measured cost at the root LP relaxation, which is
why this is worth doing rather than a tidiness. A column is left free only where
nothing at all is derivable — which is exactly the query the native arm exists to
answer.

Phase 1 also **narrows the auxiliary's column box** to `max(|lo|, |hi|)`, on both arms. The
formulation pins `aux = |inner|`, so that is a valid upper bound — and the only one anything
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

**Unless the member is already a column.** `ABS(x)`, `MAX(x)` and `MIN(x)` name a
decision variable directly, and there `t = x` restates a column that already exists —
one column and one equality row per data row, saying nothing. They are not free: a
general constraint reads its arguments, so presolve cannot substitute them away, and
the copies survive into the solve. Measured on `MAXIMIZE MAX(x)` at 15K rows, 0.97s
with the copies against 0.29s without, same answer. So an exact renaming — one variable
term, coefficient 1, no constant — passes the variable's own column to the general
constraint. `MAX(2 * x)` and `ABS(x - target)` are genuine expressions and still earn a
column.

Those columns are **boxed by the same `AuxRange` walk the Big-M constant comes from**,
never left free as a matter of course. A free continuous column is a measured
performance cliff (see stage 05's auxiliary-box section), and it is not a consequence
of going native. A column is free only where no range is derivable at all — which is
exactly the query the native path exists to answer, since a general constraint needs no
bound where a Big-M would.

**Each end is decided separately.** `AuxRange` carries `lo_unbounded` and
`hi_unbounded`, not one flag, because the two ends fail independently: `x >= 0` with no
ceiling has a derived floor and no derivable roof, and the auxiliary over it is emitted
`[0, 1e30]`. Only a Big-M needs both ends, since a constant has to dominate the whole
span — `Span()` asserts `!Unbounded()` and every refusal site tests the same. Boxing
is the one caller entitled to read the ends apart, because half a box is still a box,
and it is strictly better than none for the root simplex.

The lowered rows the construct would have produced are **not** also emitted; a model
holds one or the other, never both. The two exact rows stage 05 already wrote
(`aux >= inner`, `aux >= -inner`) do stay: they are implied by `aux = |t|`, they need
no Big-M, and they carry the clause provenance the elastic engine reads.

### Indicator constraints — a row, conditioned

`SolverModel::indicator_constraints` is the second native list, and it exists because
`<>` is naturally a conditional **row**, not a result-column relationship.

A `<>` clause has no scalar row of its own. Its two disjunction rows *are* the clause, both
`USER_MECHANISM`; a conditional row — `binary == value` implies this row — states that
semantic form directly. Exact grouped removal filters the indicator representation with
the same `removal_group_id` used for matrix, quadratic, and native-general constraints.
That is why the two native lists remain separate: one relates columns, the other conditions
a row.

`SolverInput::IndicatorConstraintSpec` therefore **holds a `RawConstraint`** rather than
restating its fields. A conditional row and the matrix row it lowers to are the same row
with the same provenance, and composing them is what makes that literally true instead
of a convention two field lists have to keep.

`z == 0 => LHS <= K-1` and `z == 1 => LHS >= K+1` say what a Big-M pair says, with no
constant to dominate the row — so no contributing variable needs a finite bound.

Two consequences follow from a construct having no rows. `DumpSolverModel` renders
general constraints explicitly, or the corpus diff would read a native construct as
rows quietly disappearing. And `BuildUnboundedRayFallbackModel` **declines** a model
that has any — or any indicator constraint — exactly as it declines a quadratic one: the
ray model rebuilds the matrix row by row, so a conditional row cannot come along either,
and dropping one would relax the model past what a ray argument permits.

---

## 9b. `LowerDecideConstructs` — the one place a construct is lowered

The last pass of this stage. Every construct site above emits the **semantic** form and
nothing else: a `<>` becomes a pair of conditional rows whether or not any backend can
state one. This pass reads what the chosen backend declared — the answer stage 05
recorded on the plan, carried here as a value, not a fresh question to a backend — and
rewrites whatever it cannot state into ordinary rows.

Lowering a conditional row is one rewrite: give it a Big-M term on its own binary, sized
so the row is exactly slack when the condition is off.

| Condition | Sense | Emitted |
|---|---|---|
| `z == 0` | `<=` | `a·x - M z <= b` |
| `z == 0` | `>=` | `a·x + M z >= b` |
| `z == 1` | `<=` | `a·x + M z <= b + M` |
| `z == 1` | `>=` | `a·x - M z >= b - M` |

`M` is the distance from the row's bound to the far end of its own reach —
`max(a·x) - b` for a `<=` row, `b - min(a·x)` for a `>=` one — read off the boxes of the
columns actually in that row, with each coefficient's sign respected before an end is
blamed. Relaxed by exactly that much, the row admits everything the columns can produce
and cuts nothing. Plus a one-unit margin: the `<>` bound is `K±1` on an integer lattice,
so a unit of slack costs nothing there and keeps the relaxed branch clear of
floating-point wobble in the solver's own row activity.

Derived **per half**, from that half's own row, not one constant maximised over every
row of the clause: on the corpus an aggregate `<>` over four binary columns gets
`M = 22` and `M = 2` where a shared constant would be 21 on both.

It runs last because a Big-M reads column boxes and the sites above are what narrow
them. It refuses, naming a column to bound, where no finite `M` exists — and that
refusal belongs here and only here, because a construct the backend states needs no
constant to dominate it. `VarIndexer::OwnerOf` is what lets a pass working in flat
columns still name a decide variable in that message.

---

## 10. Source map

| Concern | Location |
|---|---|
| `SolverModel::Build`, all constraint paths, Q construction | `src/decidb/formulation/ilp_model_builder.cpp` |
| `LowerDecideConstructs`, global-auxiliary allocation, flat column boxes | `src/decidb/formulation/ilp_linearization.cpp` |
| Implied bounds, Big-M constants, per-row range walks | `src/decidb/formulation/linearization_bigm.cpp` |
| MIN/MAX — constraint, objective, extremum links, composed | `src/decidb/formulation/linearization_minmax.cpp` |
| `<>` collapse classification and Big-M disjunction | `src/decidb/formulation/linearization_not_equal.cpp` |
| McCormick and ABS rows | `src/decidb/formulation/linearization_bilinear_abs.cpp` |
| The value rewrite's helper decisions: boxes and definitions | `src/decidb/formulation/linearization_reducer_value.cpp` |
| Helpers shared across those files | `src/include/duckdb/decidb/formulation/ilp_linearization_internal.hpp` |
| `MinMaxObjectiveSpec`, `ComposedMinMaxTermData` — what stage 08 hands over | `src/include/duckdb/decidb/formulation/ilp_linearization.hpp` |
| `BilinearLinkSpec`, `AbsMaximizeLinkSpec`, `ReducerValueLinkSpec` — the formulation tags | `src/include/duckdb/decidb/formulation/solver_input.hpp` |
| `VarIndexer`, `SolverModel`, `ModelConstraint`, provenance | `src/include/duckdb/decidb/formulation/ilp_model.hpp` |
| `SolverInput`, `EvaluatedConstraint`, `CoefficientColumn` | `src/include/duckdb/decidb/formulation/solver_input.hpp` |
| Golden model corpus (the characterization oracle) | `test/decide/golden/` |
