# SQL Functions & Expressions — Implemented Features

This file documents which SQL functions and expressions work inside DECIQL clauses today.

---

## Aggregate Functions

### SUM()

The primary aggregate over expressions involving decision variables. Valid in both constraints and objectives.

```sql
MAXIMIZE SUM(x * value)
SUCH THAT SUM(x * weight) <= 50
SUCH THAT SUM(x + cost) <= budget
SUCH THAT SUM(q * (price + x)) <= budget
```

In constraints, data-only additive terms inside the aggregate body are supported. They are evaluated over the same rows as the decision terms — the clause's kept rows, the reducer's `by` group, its own `when` and its own `per` — with `AVG` scaling, then subtracted from the bound before the solver row is built.

**Code**: Validated in `decide_objective_binder.cpp` and `decide_constraints_binder.cpp` — aggregates other than SUM, AVG, MIN, and MAX are rejected with an error.

#### Data-Only Aggregate RHS in Aggregate Constraints

An aggregate constraint may bound the decision aggregate by a **data-only** aggregate on the other side — a `SUM`/`AVG` whose argument references no DECIDE variable:

```sql
SUCH THAT sum(x * val) <= sum(val)                    -- pick a subset whose value ≤ the total
SUCH THAT avg(x + cost) <= avg(cost) + 1              -- average stays within 1 of the data mean
SUCH THAT sum(x * val) by (grp) <= sum(val) by (grp)  -- per-group bound (each group its own)
SUCH THAT sum(x * cost) <= sum(cost) + 10             -- aggregate plus a scalar offset
SUCH THAT sum(x * cost) <= sum(when w: cost)          -- the bound's own when: over the w-rows
```

These are evaluated **in place, on the right**, by the bound-side reducer evaluator described under "Reducers as a Bound" below: each data-only reducer collapses to one value per group, honouring the clause's `when`, its own `by` group, its own `when`, and the de-duplication of its own key. A bound-side reducer with a `by` therefore carries a **per-group bound** (each group's `sum(val)` is its own bound), not one uniform scalar. A reducer's own `when` scopes only that reducer: in `sum(x * cost) <= sum(when w: cost)` the bound sums the w-rows while the left side still sums every row.

A reducer on **both** sides is accepted (`sum(x) <= sum(x * val)`): the canonicalizer moves every decision-bearing term to the left. `count(*)` on the bound side counts the rows its group reads (`sum(x) by (grp) <= count(*) by (grp)`).

**Tests**: `test/decide/tests/test_cons_rhs_aggregate.py` — parity with the explicitly-subtracted form and the scalar-bound equivalent, scalar-plus-aggregate bound, per-group bounds, AVG mixed with other terms, a bound's own `when`, MIN/MAX bounds, and group-aware `count(*)`.

#### Reducers as a Bound

Any reducer may stand on the bound side of a reduced constraint, evaluated to **one value per group**:

```sql
SUCH THAT sum(x * price) <= min(price) * 4          -- MIN/MAX, not just SUM/AVG
SUCH THAT sum(x) by (grp) <= count(*) by (grp)      -- counts the group's rows
SUCH THAT sum(x * price) <= min(price) + 90         -- mixed with an ordinary term
SUCH THAT sum(x) <= (SELECT b FROM g WHERE ...)     -- row-varying: tightest bound
SUCH THAT sum(ship) by (depotID) <= stock           -- a plain column, the same way (C1)
```

The bound side is reduced by its own evaluator rather than folded into the decision side, where a data term could only be summed, so `MIN` and `MAX` work there as well as `SUM` and `AVG`.

**Semantics.** The evaluator follows the construction order every reducer takes (paper §3.2.2): the rows its `by` selects among the clause's kept rows → its own `when` → one value per class of its own key, de-duplicated → aggregation. A reducer's own `when` scopes only that reducer, so the bound is *not* narrowed by a filter on the left side: in `sum(when a: x) <= min(b)`, `min(b)` still ranges over every kept row.

**A row-varying bound** — a plain data column (C1) or a correlated subquery — with no `per` collapses to the single bound the per-tuple conjunction implies for `<=`/`<`/`>=`/`>`: MIN for `<=`/`<`, MAX for `>=`/`>`, per paper §3.2.1. Under an explicit `per K:` each value of the bound in a class is a variant of its own (`../scope/done.md` §5, D64), which bounds the group alike. `=` has no such collapse and is refused as a contradiction when the values genuinely differ (two different single numbers can't both be *the* sum). `<>` also has no collapse, but for the opposite reason: nothing contradicts, so every excluded value is kept rather than reduced to one (C3) — `SUM(x) <> 500 AND SUM(x) <> 350` within one group, not just the tighter of the two. Both are Standing Decision 3.

**AVG on the right** keeps its fractional value, including when mixed with other terms (`<= 2 * avg(col)`). The optimizer's AVG→SUM rewrite would redeclare the node with SUM's integral type while its value stays fractional, so `RewriteAvgInExpression` **skips any AVG with no decision variable** — there is nothing to linearize in one, and DuckDB's `AVG` already returns `DOUBLE`.

**Keyed and filtered reducers as bounds** (`<= sum(per D: cost)`, `<= sum(when w: b)`) run the same de-duplication and filtering the left side does.

**Code**: `EvaluateRhsReducerPerGroup` and `ReduceAggregateRhsPerGroup` in `src/execution/operator/decide/physical_decide.cpp`; reducer substitution via `TransformToChunkExpression`'s `agg_substitutions`.

#### A Query-Wide Decision Multiplied by a Vector, Inside a Reducer

A reducer is rejected iff its **body is row-invariant** — holds the same value on every
row, with no data column and no per-row or keyed decision anywhere inside it — not
merely because it *contains* a query-wide (`per ()`) decision:

```sql
SUM(cap)                    -- rejected: row-invariant, nothing to reduce over
SUM(cap1 + cap2)            -- rejected: still row-invariant (two scalars, no data)
SUM(opening_cost * cap)     -- valid: opening_cost varies per row, means (Σcost)·cap
SUM(D: opening_cost * cap)  -- valid: means (Σ over D's distinct tuples of cost)·cap
SUM(x + cap)                -- valid: x is per row, means SUM(x) + n·cap
```

`SUM(opening_cost * cap)` varies per row and its reduction is unambiguous — a standard
scalar-times-vector product. The colon form follows the same rule: a query-wide decision
is row-invariant, so it splits no tuple and it contributes uniformly to every tuple,
but a keyed reducer whose entire body is query-wide decisions and constants is rejected
for the same "nothing to reduce over" reason.

**Formulation (stage 05/06).** Term extraction (`decide_linear_form.cpp`) is
scope-agnostic — `FindDecideVariable`/`ExtractTerms` don't special-case a variable's
scope, so `opening_cost * cap` extracts to `DecideTerm{var=cap,
coefficient=opening_cost}` whatever `cap`'s scope. The **accumulator** that folds
per-row-evaluated coefficients into the flat solver column a variable resolves to
(`ilp_model_builder.cpp`) tells the two shapes of a `SCALAR`-scoped term apart by
`DecideTerm::reduction`. `NONE` marks a query-wide decision standing **beside** a reducer
as a bare additive term (`sum(x) - max_shortfall <= cap`), which is row-invariant: its
coefficient is added **once**, from the group's first row. `LinearTermReduction::SUM`
marks a term from **inside** a reducer (stamped by `ApplyAggregateMetadata` on the
constraint side, and in `ExtractAggregateObjectiveTerms`/`AnalyzeObjective`'s aggregate
branch on the objective side): it takes the per-row accumulation loop keyed variables
use, so its coefficient is the sum of every counted row's data. `SolverInput` carries
`objective_term_reductions` (the constraint side has
`EvaluatedConstraint::linear_term_reductions`) so the objective-side accumulator
(`BuildIlpModel`'s linear-objective loop) makes the same distinction.

**De-duplication.** `SUM(D: cost * cap)` charges each of `D`'s distinct tuples once,
even when two tuples share the same `cost` — identity is the tuple, not the value, as for
every keyed reducer (`syntax_reference.md` §5.1). The key's row mask
(`ApplyQualifierToFilter`/`BuildQualifierKeepMask`) runs before the accumulator sees the
row data, so the accumulator needs no key-specific case.

**Tests**: `test/decide/tests/test_scalar_scope.py` —
`test_scalar_times_data_inside_reducer_is_weighted_by_row_data` (unqualified, oracle
discriminates coefficient `Σcost` from both the bare-scalar and row-count misreadings),
`test_scalar_plus_row_scoped_term_inside_reducer_is_legal` (additive mix),
`test_sum_over_two_scalars_rejected` (still-rejected row-invariant case).
`test/decide/tests/test_qualified_reducer.py` —
`test_qualified_reducer_scalar_times_entity_data_is_weighted_and_deduplicated` (oracle,
weighted by distinct-entity data, equal-cost entities still counted twice),
`test_query_wide_decision_alone_inside_qualified_reducer_rejected` (still-rejected).

**Code**: `DecideBinder::IsRowInvariantExpression` and `ClassifyReducerCall` (the
parsed-tree gate, every reducer) in `decide_binder.cpp`;
`ExtractAggregateConstraintTerms`/`ExtractAggregateObjectiveTerms` in
`decide_linear_form.cpp` (the `BOUND_AGGREGATE` exclusion on the bare-scalar shortcut);
the three `var_scope == SCALAR` sites in `ilp_model_builder.cpp` (two constraint paths,
one objective path).

### Several Relations at Once — A Composite Qualifier

`SUM(D, T: expr)` (`syntax_reference.md` §5.1) names several relations; it means
`sum(per D, T: expr)` (paper §3.2.2).

**Semantics.** A composite qualifier's tuple identity is the concatenation of every
named relation's own key — the same "all columns of the table" key a single-relation
scope already used, just for each relation in the list. A row is a duplicate only when
it repeats on *every* named relation's key at once, so naming a relation removes the
fan-out it would otherwise contribute, and a relation left unnamed still contributes
its fan-out uncollapsed. `expr` may draw columns and decisions from any of the named
relations — one value per variant of each tuple of the composite key (D65), as for
`sum(per D, T: e)` —
and the relation list is order-independent — `D, T` and `T, D` resolve to the same scope.

One consequence of that identity rule is worth stating: with exactly two relations in
the whole query, `SUM(D, T: expr)` and the unqualified `SUM(expr)` are the same
reducer — the composite key already *is* the join-result row, since there is no third,
unqualified relation left to contribute fan-out for the qualifier to collapse. The two
forms only diverge once a relation the query joins is left out of the qualifier list —
which needs three relations to demonstrate, since the two-relation case can't produce
an unqualified relation to fan out from.

**Implementation.** `EntityScopeInfo` (`logical_decide.hpp`) carries
`source_table_indices`, every relation the key reads; the key is every named
relation's stored columns sorted by binding (`FindOrCreateExactScope`), so the
composite scope is built once regardless of the order the query names its relations in.
Everything below the binder — the optimizer's `qualifier_scope_idx` stamping, the
prepared model's `DecideTerm`/`BilinearConstraintTerm`/objective terms, and physical
execution's `BuildQualifierKeepMask`/`BuildEntityMappings` — treats a scope as an
opaque index into `entity_scopes`, so the composite-ness lives entirely inside the
`EntityScopeInfo` the binder builds. The grammar passes the relation list
through as one `PGList`.

**Tests**: `test/decide/tests/test_qualified_reducer.py` —
`test_three_relation_composite_qualifier_differs_from_single_and_unqualified` (oracle,
a customer/orders/lineitem chain where the composite, single-relation and unqualified
forms compute three genuinely different weightings — order count, one, and row count —
and are checked against three independent oracle models),
`test_two_relation_composite_qualifier_equals_unqualified_when_query_has_only_those_two_relations`
(confirms the no-op case above, row-for-row), `test_unknown_relation_in_multi_relation_qualifier_rejected`.

**Code**: grammar actions for `PG_AEXPR_QUALIFIED_REDUCER` in
`third_party/libpg_query/grammar/statements/select.y`; the matching transform case in
`transform_operator.cpp`; `FindOrCreateExactScope`, `BindQualifiedReducer` in
`decide_binder.cpp`; the `EntityScopeInfo` struct in `logical_decide.hpp`, serialized from
`src/include/duckdb/storage/serialization/nodes.json` (`source_table_indices` is
field 101).

### AVG() — Coefficient Scaling at Execution Time

`AVG(expr)` over decision variables is treated as an aggregate constraint like SUM, but terms are scaled by the row count N at execution time so the model represents the average, not the raw sum.

**Semantics**: Standard SQL AVG — divide by count of all rows (decision variables are never NULL). This is always linear since N is a data-determined constant.

**Constraints**: Semantically, `AVG(expr) op K` is equivalent to `SUM(expr) op K*N` where N is the number of values the reducer receives: the rows of its `by` group among the clause's kept rows that pass its own `when`, or the number of classes of its own `per`. With no scope at all, N is the total row count.

**Objectives (flat)**: `MAXIMIZE/MINIMIZE AVG(expr)` uses the same optimal assignment as `SUM(expr)` when there is one global denominator. In mixed additive aggregate expressions, DeciDB preserves AVG scaling per term so `AVG(a) + SUM(b)` is not treated as `SUM(a) + SUM(b)`.

**Objectives (two-level form)**: in `outer(per k: avg(e) by (k))` the inner AVG scales each row's coefficient by `1/n_g` (group size), producing true per-group averages. An outer `avg` over the classes is refused; `sum` over them ranks the solutions the same way. See [`../scope/done.md`](../scope/done.md) §6.

```sql
SUCH THAT avg(x * weight) <= 10                       -- sum(x * weight) <= 10*N
SUCH THAT avg(x + cost) <= 5                          -- fixed avg(cost) is moved to the bound
SUCH THAT avg(x) <= 0.5                               -- at most half the rows selected (BOOL)
SUCH THAT when active: avg(x * cost) <= 5             -- only among active rows
SUCH THAT avg(when active: x * cost) + sum(when priority: x * fee) <= 100
SUCH THAT per emp: avg(x * hours) by (emp) <= 8       -- per-group average
MAXIMIZE avg(x * profit)                              -- same as MAXIMIZE sum(x * profit)
```

**Code**: AVG flows through binding natively (no parse-time rewrite), preserving its DOUBLE return type so fractional RHS values survive type coercion. The binders (`decide_constraints_binder.cpp`, `decide_objective_binder.cpp`) accept `"avg"` alongside `"sum"`. The `DecideOptimizer` rewrites AVG to SUM while tagging the aggregate with `AVG_REWRITE_TAG`. At execution time (`physical_decide.cpp`), expression analysis marks extracted terms with `avg_scale`; coefficient evaluation scales linear and bilinear terms by `1/N`, and quadratic inner terms by `1/sqrt(N)`. Exception: for `AVG(expr) <> K` the LHS scaling would produce fractional coefficients and trip the NE integer-step guard, so DeciDB sets `EvaluatedConstraint::ne_avg_rhs_scale` and leaves the LHS as SUM; the deferred NE expansion multiplies the RHS by the per-group size instead.

**Tests**: `test/decide/tests/test_avg.py` covers objectives, constraints, a `when` in
front and inside, `per` with `by`, BOOL, INT, non-linear rejection, `<>` with and without
a `when`, and no-decision-variable passthrough.

---

### MIN() / MAX() — Per-Row and Big-M Indicator Rewrites

`MIN(expr)` and `MAX(expr)` over decision variables are supported in both SUCH THAT constraints and MAXIMIZE/MINIMIZE objectives. The implementation strategy depends on whether the case is "easy" (naturally per-row) or "hard" (requires Big-M indicator variables and a global auxiliary variable).

#### Easy Constraint Cases (No Big-M)

When the comparison direction already bounds each row individually, no auxiliary variables are needed:

- `MAX(expr) <= K` → per-row constraint: `expr <= K` for every row
- `MIN(expr) >= K` → per-row constraint: `expr >= K` for every row

These are trivially correct: bounding every row satisfies the aggregate bound. With a `by`, each member is bounded by its group's bound. With no `when` and no filter on the reducer, a bound on a lone decision (`max(x) <= K`, `min(x) >= K`) is absorbed into the decision's box, so a negative `K` makes `x` signed (`../scope/done.md` §3). The reducer stays in the tree (`MINMAX_EASY_REWRITE_TAG`), so EXPLAIN and DIAGNOSE quote the clause as written.

#### Hard Constraint Cases (Big-M Indicators)

When the aggregate must be tight (equality or the "wrong" direction), a global auxiliary variable `z` and per-row binary indicators are introduced:

- `MAX(expr) >= K` → global variable `z >= K`, per-row: `z >= expr`, plus Big-M indicators ensuring `z` equals some row's value
- `MIN(expr) <= K` → global variable `z <= K`, per-row: `z <= expr`, plus Big-M indicators
- Equality cases (`MAX(expr) = K`, `MIN(expr) = K`) → both directions constrained

**On a backend that expresses MIN/MAX natively, the hard cases are not encoded at
all.** Gurobi states `z = MAX(t..)` as a general constraint, so there is no Big-M, no
indicator binaries, and no requirement that any contributing variable be bounded —
`SUCH THAT MAX(x) >= 5` answers there with an unbounded `x`. All four hard forms
(constraint, flat objective, two-level objective, composed term) take that path. The
choice is made once, at execution, from the backend's capability table, and both paths
reach the same optimum wherever both can run.

**Lowered, a hard case needs every contributing variable bounded.** The Big-M constant has to
dominate the expression's reachable range, and nothing finite dominates an unbounded
one — so `MAX(x) >= 5` with an unbounded `x` is **refused**, naming `x` and the bounds
to add, rather than encoded against a large constant. A constant the true range
exceeds would not fail; it would silently cut the feasible region and return a wrong
optimum. The same rule and the same message on both backends.

A bound DeciDB can derive counts as a bound: `SUCH THAT SUM(x) <= 10 AND MAX(x) >= 5`
answers, because implied-bound propagation reads `x <= 10` off the first clause. So
does `MAX(ABS(x - 5)) >= 3` over a bounded `x` — the ABS auxiliary's own box is
derived from `x`'s. Easy cases never ask for a Big-M and are unaffected.

#### Objective Cases

- **Easy objectives**: `MINIMIZE MAX(expr)` and `MAXIMIZE MIN(expr)` — a single global auxiliary variable `z` with per-row linking constraints (`z >= expr_i` for MAX, `z <= expr_i` for MIN). The objective optimizes `z` directly.
- **Hard objectives**: `MAXIMIZE MAX(expr)` and `MINIMIZE MIN(expr)` — requires `z` plus per-row binary indicator variables to ensure `z` equals some row's actual value (Big-M formulation).

#### Row Expression Shape

The inner expression is any linear combination of decision variables plus a constant — it is not restricted to a single product term. `MIN((cost + 1) * x)`, `MIN(cost * x + x)`, and `MIN(cost * x + 5)` are all accepted, in constraints and objectives, flat, two-level and composed alike — including `sum(per k: min(e) by (k))`.

Each linking row is accumulated **per solver column** before emission (`MinMaxLinkRow` in `physical_decide.cpp`). This matters because the term arrays are indexed by term, not by variable, so one column can reach a row more than once: `(cost + 1) * x` distributes into `cost*x + 1*x`, and a keyed or query-wide variable resolves to a single column across every row of a group. A repeated column index is rejected outright by both Gurobi and HiGHS, so coefficients are summed and columns whose terms cancel are dropped. Constant terms carry no column and fold into the row's bound: `z <= expr + k` is emitted as `z - expr <= k`.

#### Composition

- **`when`**: a `when` in front of the clause or the reducer's own `when` selects which rows participate in the MIN/MAX aggregate, and constraint/indicator generation skips the others.
- **`by`**: a hard MIN/MAX gets one extremum column per `by` group; an easy one bounds each member by its group's bound.

```sql
-- Easy constraint cases (no Big-M)
SUCH THAT max(x * cost) <= 100         -- per-row: x*cost <= 100
SUCH THAT min(x * hours) >= 2          -- per-row: x*hours >= 2

-- Hard constraint cases (Big-M indicators)
SUCH THAT max(x * cost) >= 50          -- global z, binary indicators
SUCH THAT min(x * hours) = 4           -- equality: both directions

-- Objectives
MINIMIZE max(x * cost)                 -- easy: global z, minimize
MAXIMIZE min(x * profit)               -- easy: global z, maximize
MAXIMIZE max(x * profit)               -- hard: z + binary indicators
MINIMIZE min(x * cost)                 -- hard: z + binary indicators

-- Multi-term and constant row expressions
MINIMIZE max((cost + 1) * x)           -- distributes to cost*x + 1*x, one column
MINIMIZE max(cost * x + x)             -- same shape written out
MINIMIZE max(cost * x + 5)             -- constant folds into the row's bound

-- With a when
SUCH THAT when category = 'electronics': max(x * cost) <= 50
MINIMIZE when active = 1: max(x * deviation)
```

**Code**: nothing rewrites a MIN/MAX before binding — a reducer is one atomic term to the canonicalizer, which never opens it, so a multi-term `inner` is safe by construction. Post-bind, `DecideOptimizer::RewriteMinMaxConstraints`/`RewriteMinMaxInConstraint` (`decide_rewrite_minmax.cpp`) walk the bound expression tree to classify each MIN/MAX as easy/hard and emit the Big-M indicator scaffolding. The binders (`decide_constraints_binder.cpp`, `decide_objective_binder.cpp`) whitelist MIN/MAX alongside SUM and AVG. At execution time, `physical_decide.cpp` generates the appropriate per-row constraints, Big-M indicator constraints, and global auxiliary variables. Global variable and constraint support is provided by `solver_input.hpp` and `ilp_model_builder.cpp`.

---

## Rejected: Non-Linear Scalar Functions over a DECIDE Variable

Any scalar function other than `ABS()` and `POWER(..., 2)` that wraps a decision variable is rejected at bind time with:

```
Binder Error: Scalar function 'sqrt' over a DECIDE variable is not supported:
it would make the model non-linear. Only ABS() and POWER(..., 2) can wrap a
decision variable.
```

This covers (non-exhaustive) `SQRT`, `EXP`, `LN`, `LOG`, `FLOOR`, `CEIL`, `ROUND`, `SIN`, `COS`, `TAN`, and any user-defined or built-in scalar function that doesn't have a dedicated linearization path. The rejection fires in every position: per-row constraint LHS (`SUCH THAT sqrt(x) <= 2`), inside an aggregate (`SUM(exp(x))`, `MAXIMIZE SUM(log(x))`), and nested inside `ABS()` or `POWER()` (e.g., `ABS(sqrt(x) - 1)`).

**Scalar functions wrapping only table columns are not affected** — e.g., `SUM(x * sqrt(l_quantity))` would fold `sqrt(l_quantity)` into a per-row coefficient. The walker only flags a function when one of its arguments transitively contains a DECIDE variable.

**Aggregate mis-uses** (e.g., `BIT_AND(x)`, `STDDEV(x)`) are routed to the existing aggregate-specific rejection in `BindAggregate` with the more informative "only SUM, AVG, MIN, MAX, or COUNT is allowed" message. A catalog lookup distinguishes scalar from aggregate so the two rejection paths don't collide.

**Why this guard exists**: no later stage models a non-linear scalar over a decision, so without it such a function would be dropped or reach execution unmodelled. The validator catches every position — per row, inside a reducer, and nested in `ABS()` — at bind time.

**Code**: `ValidateDecideNoNonLinearScalar` in `src/planner/expression_binder/decide/decide_binder.cpp`, called from `BindDeclarations` (`src/planner/expression_binder/decide/decide_declarations_binder.cpp`) on the parsed tree, before binding.

**Tests**: `test/decide/tests/test_error_binder.py` — `test_nonlinear_scalar_per_row_lhs`, `test_nonlinear_scalar_inside_sum`, `test_nonlinear_scalar_inside_abs` (parametrized over SQRT, EXP, LN, LOG, FLOOR, CEIL, ROUND, SIN, COS).

**POWER exponent check**: The same pre-pass rejects `POWER(base, exp)` / `POW(base, exp)` / `base ** exp` when `base` contains a DECIDE variable and `exp` is not a constant numeric equal to `2`. That covers fractional exponents (`POWER(x, 0.5)`), negative exponents (`POWER(x, -1)`), higher-integer exponents (`POWER(x, 3)`), degenerate exponents (`POWER(x, 0)`, `POWER(x, 1)`), and non-constant exponents (`POWER(x, x)`, `POWER(x, col)`). The pre-pass catches all non-2 cases with the same error messages the `ValidateQuadraticPower` whitelist uses inside SUM, so the wording is the same in SUM and non-SUM contexts.

**Data-only named scalar functions fold too**: `SUM(f(col) * x)` where `f` is an arbitrary named scalar *function* on data columns (`mod()`, `floor()`, …) folds to a per-row coefficient, exactly like data-only operators — see "Data-only operators the algebra doesn't model" under Arithmetic Operators. Only the variable-bearing case (`f(x)`) is rejected. Ordinary arithmetic data-only subterms such as `SUM(x + cost)` and `SUM(q * (price + x))` are supported in aggregate constraints.

**`POWER` over data is one of them, and the exponent is unrestricted there**: `SUM(x * POWER(qty, 2))` is linear in `x` — the base references no decision variable, so the whole call is a per-row coefficient — and `POWER(qty, 3)` is equally fine. The quadratic rules above (`exp == 2`, base must be linear, constraints vs objectives) govern a POWER whose base *is* decision-bearing, and only that.

Tests: `test/decide/tests/test_power_over_data.py` (oracle-verified, and one case pins that `POWER(x-2, 2)` takes the quadratic path).

---

## ABS() — Linearized Automatically

`ABS(expr)` over decision variables is automatically linearized using standard ILP techniques. The formulation depends on whether ABS appears in a constraint or in the objective, and on the optimization sense.

### ABS in constraints (two paths: lower-envelope or Big-M)

For each `ABS(expr)` that references a DECIDE variable, the system:

1. Introduces an auxiliary REAL variable `d` (hidden from query output)
2. Adds two lower-bound constraints: `d >= expr` and `d >= -expr`
3. Replaces `ABS(expr)` with `d`

The lower-envelope alone forces `d >= |expr|`. To pin `d` to exactly `|expr|`, one of two mechanisms is used per ABS occurrence:

Classification is **per ABS occurrence and by sign**, not by which side of the comparison the ABS was written on. The constraint is read as `E <op> 0` with `E = LHS - RHS`; each ABS then carries an effective sign, and the question "does this comparison push `d` down?" has a single answer per occurrence. Sign is tracked through `+`, binary and unary `-`, casts, aggregate bodies, and numeric literal factors. A factor whose sign is known only at execution — a data column, as in `SUM(w * ABS(x - t))` — is classified as unknown and conservatively takes the Big-M path.

**An ABS inside an ABS** — `ABS(ABS(e) - k)`, or `ABS(ABS(SUM(x) BY (g)) - k)` over a reducer — is supported: each ABS gets its own auxiliary, innermost first, and an ABS nested under another takes Path B, whatever the outer clause's direction or the objective's sense; under a factor of exactly zero neither the outer nor the inner needs anything, since the term is dead. The outer ABS is not monotone in its argument, so nothing else pins the inner auxiliary: held by its lower envelope alone it could float to wherever the outer clause is easiest to satisfy (over `x` in `[-1, 1]`, `abs(abs(x) - 2) <= 0` would hold at an inner value of 2 with no `x` reaching it). Over `x` in `[-2, 3]`, `abs(abs(sum(x) by (g)) - 2) <= 0` keeps every group's total at magnitude 2, so `MINIMIZE sum(x)` reaches -4. Tests: `test_nested_abs_*` in `test/decide/tests/test_abs_linearization.py`.

**Path A (lower-envelope only) — when the constraint pushes `d` down.** The effective sign is positive under `<` / `<=`, or negative under `>` / `>=`. Covers `ABS(...) <= K` (LHS), `K >= ABS(...)` (RHS), the aggregate forms `SUM(ABS) <= K`, `MAX(ABS) <= K`, `MIN(ABS) <= K`, `AVG(ABS) <= K`, and the *positively-signed* ABS of a shape like `ABS(a - k) - ABS(b - k) <= 0`. The constraint itself caps `d` from above; the solver picks `d_i = |e_i|` to minimize slack. No extra variables.

**On a backend that expresses ABS natively, Path B is not taken.** Gurobi states
`aux = |inner|` as a general constraint, which needs no Big-M and therefore no bound
on the contributing variables — so `SUCH THAT ABS(x - target) >= 2` answers there with
an unbounded `x`, where the Big-M path has to refuse. The choice is made once, at
execution, from the backend's capability table; the lowering below is what every other
backend still gets, and the two reach the same optimum wherever both can run. See
`01_pipeline/08_execution/done.md`.

**Path B (Big-M sign-indicator) — for hard-direction shapes.** The effective sign runs against the relation, or the relation pins nothing (`=`, `<>`, BETWEEN, IN). Covers `ABS(...) >= K`, `ABS(...) > K`, `ABS(...) = K`, `ABS(...) <> K`, `ABS(...) BETWEEN a AND b`, ABS in equality / not-equal between aggregates, the analogous aggregate forms (`SUM(ABS) >= K`, `MIN(ABS) >= K`, `MAX(ABS) >= K`), and any **subtracted** ABS — `ABS(a-k) - ABS(b-k) <= 0` upper-bounds the difference, so the second auxiliary is free to float upward and must be pinned. These shapes do not naturally upper-bound `d`, so a binary sign indicator `y ∈ {0,1}` is allocated per ABS term and two Big-M upper-bound constraints are added (same formulation as the MAXIMIZE-objective path below):

- `d <= expr  + 2M·(1−y)`  (active when `y=1`, selecting the positive branch)
- `d <= −expr + 2M·y`      (active when `y=0`, selecting the negative branch)

Combined with the lower envelope, these force `d = |expr|` exactly regardless of constraint direction. `M` is computed at execution time from the bounds of variables in `expr` — when Path B is used, **every DECIDE variable referenced inside `ABS(expr)` must have a finite bound, either declared or inferred** by implied-bound propagation (e.g. `SUM(x) = K` implies `x <= K`; see `../../01_pipeline/05_optimizer/done.md`). Only when no bound can be derived is the query rejected. (Path A queries don't need bounds because the constraint itself bounds `d`.)

The classifier (`TagAbsForBigM` in `decide_rewrite_abs.cpp`, via `CollectAbsWithSign`) walks the constraint tree and the objective once and tags each ABS occurrence with `ABS_NEEDS_BIGM_TAG` if it falls into Path B. A scope doesn't change classification — the per-row Big-M envelope is unconditional, and the scope only affects which rows participate in the outer aggregate or constraint.

### ABS in objectives (by sign under the sense)

The objective reads like a constraint, with the sense standing in for the relation: `MINIMIZE E` pins what `E <= K` pins and `MAXIMIZE E` what `E >= K` pins. A positive ABS under MINIMIZE (`MINIMIZE SUM(ABS(expr))`) and a negative one under MAXIMIZE (`MAXIMIZE -ABS(expr)`) are pushed down by the solver, so the lower envelope suffices. The other sign — `MAXIMIZE SUM(ABS(expr))`, `MINIMIZE 0 * SUM(x) - ABS(SUM(x) - 9)` — leaves `d` with no ceiling and the solver pushes it to +∞, a spurious "unbounded"; so does a data weight whose sign is unknown before the query runs (`MINIMIZE SUM(w * ABS(x - t))` with a negative `w` on some row). These take Path B, and so does an ABS under a MIN or MAX pushed its hard way (`MINIMIZE MIN(ABS(expr))`, `MAXIMIZE -MIN(ABS(expr))`, `MAXIMIZE MAX(ABS(expr))`): the extremum's own Big-M needs every member's box, which only the exact envelope gives an auxiliary. A factor that is exactly zero (`0 * SUM(ABS(expr))`) makes the term vanish and needs neither. Path B is a binary sign indicator `y ∈ {0,1}` and two Big-M upper-bound constraints that pin `d = |expr|` exactly:

- `d <= expr  + 2M·(1−y)`  (upper-bound when `y=1` selects the positive branch)
- `d <= −expr + 2M·y`      (upper-bound when `y=0` selects the negative branch)

`M` is computed at execution time from the variable bounds involved in `expr`: `M = max_r(|rhs[r]| + Σ |coeff[t][r]| · max(|lb_t|, |ub_t|))` over all non-aux terms (this shares `DecideRowTermRange` with the other Big-M sites). **All DECIDE variables referenced inside `ABS(expr)` must have finite bounds** — declared or inferred by implied-bound propagation. Only if a variable still lacks a bound (lb ≤ −1e20 or ub ≥ 1e20) is an `InvalidInputException` raised naming the unbounded variable.

```sql
-- Pushed down by the sense: lower-envelope, no extra variables needed
MINIMIZE SUM(ABS(new_hours - hours))
MAXIMIZE SUM(x) - ABS(SUM(x) - 9)

-- Pushed up by the sense: Big-M upper-bound, binary indicator y allocated per ABS term
MAXIMIZE SUM(ABS(x - target))
MINIMIZE 0 * SUM(x) - ABS(SUM(x) - 9)
-- requires: x <= <upper_bound> (and optionally x >= <lower_bound>)

-- A data weight: its sign is unknown before the query runs, so Big-M as well
MINIMIZE SUM(w * ABS(x - target))

-- In per-row constraints: bound deviation per row (lower-envelope, always correct)
SUCH THAT ABS(new_qty - l_quantity) <= 5

-- In aggregate constraints: bound total deviation (lower-envelope, always correct)
SUCH THAT SUM(ABS(new_qty - l_quantity)) <= 50
```

`ABS()` without decision variables (e.g., `ABS(col1 - col2)`) is left as regular SQL — no rewrite occurs.

**Code**:
- Classifier: `TagAbsForBigM` in `decide_rewrite_abs.cpp` runs before `RewriteAbs`. Walks the constraint tree and the objective; for each comparison, and for the objective body under its sense, `CollectAbsWithSign` gathers every ABS occurrence with the sign it carries in `LHS - RHS`, classifies each as Path A (sign pushes `d` down) or Path B (it does not), and tags Path-B ABS function expressions with `ABS_NEEDS_BIGM_TAG`. BETWEEN/IN/equality/<> subtrees are conservatively tagged.
- Optimizer rewrite: `DecideOptimizer::RewriteAbs` in `decide_rewrite_abs.cpp`. Phase 1 (`FindAndReplaceAbs`) reads the tag and propagates `needs_bigm` to `AbsPairInfo` (an ABS nested inside another ABS is pinned regardless). Phase 2 emits the lower envelope unconditionally. For each pair where `needs_bigm`, tags the lower-bound constraints with `ABS_UB_POS_TAG_PREFIX` / `ABS_UB_NEG_TAG_PREFIX` (payload: the **auxiliary's** index) and pushes an `AbsMaximizeLink{aux_idx, y_idx}` to `LogicalDecide::abs_maximize_links`. The `y` binary is allocated **only when the query is being lowered** (`!decide.use_native_constructs.abs`); a backend that states `aux = |t|` natively emits no Big-M, so there is nothing for a sign indicator to switch and `y_idx` stays `INVALID_INDEX`. `y` is one per row, so allocating it on the native arm would cost one dead binary per data row. The link vector is named `abs_maximize_links` for historical reasons but covers both Big-M users (objective MAXIMIZE and constraint hard-direction).
- Tag constants: `ABS_UB_POS_TAG_PREFIX`, `ABS_UB_NEG_TAG_PREFIX`, `ABS_NEEDS_BIGM_TAG` in `decide.hpp`.
- Tag parsing in `AnalyzeConstraint` (`decide_linear_form.cpp`) sets `DecideConstraint::abs_aux_idx`/`abs_is_pos_bound` (the field keys on the auxiliary because only the lowering arm has a `y`); execution copies them onward: these are copied to `EvaluatedConstraint`; the Big-M finalization block (after the bilinear block) iterates `abs_maximize_links`, computes M from variable bounds, and emits two derived `EvaluatedConstraint`s (C_ub1 and C_ub2) per ABS term. The error message at finite-bound check is generic (covers both objective-MAXIMIZE and constraint hard-direction triggers).
- Transfer: `plan_decide.cpp` moves `abs_maximize_links` from logical to physical operator.
- Serialization: `logical_decide.cpp` (`LogicalDecide::Serialize`/`Deserialize`, hand-maintained) fields 230/231 (`abs_maximize_link_aux`, `abs_maximize_link_y`).

**Tests**: `test/decide/tests/test_abs_linearization.py` covers MINIMIZE objectives,
MAXIMIZE objectives (including the missing-bound error), constraints, scopes,
multiple ABS terms, no-decision-variable passthrough, and mixed variable types.

---

## Arithmetic Operators

### Multiplication (`*`) — variable x constant or variable x column

```sql
x * 5              -- OK: variable * literal
x * weight         -- OK: variable * column (constant per row)
SUM(x * weight)    -- OK: aggregate of linear product
```

`x * y` (variable times variable) **is supported** as a bilinear term via `DecideOptimizer::RewriteBilinear` (McCormick envelopes when one factor is Boolean; non-convex Gurobi QCQP otherwise). `x * x` / `POWER(x, 2)` is supported as a quadratic (QP) term. See `03_expressivity/bilinear/done.md` and `syntax_reference.md` §4 for solver eligibility.

### Addition / Subtraction (`+`, `-`)

```sql
x + y              -- OK: sum of variables
SUM(x * a + y * b) -- OK: linear combination in aggregate
SUM(x + cost)      -- OK in constraints: data-only offset is subtracted from RHS
```

### Division (`/`) by a constant or data column

`x / divisor` is supported in per-row constraints, aggregate constraints, and quadratic objectives (inside `POWER(..., 2)`) as long as the divisor doesn't contain a DECIDE variable. `x / y` between two decision variables is non-linear and rejected at bind time with a clear `Division by a DECIDE variable is not supported` error. The divisor folds into the extracted coefficient — for `x / 2`, the solver sees `0.5 * x`; for `x / col`, the per-row coefficient is `1/col[row]`. A data-only aggregate as the divisor of a decision term (`sum(x) / sum(cap)`, `x / avg(cap)`), or divided together with one (`(sum(x) + sum(cap)) / 2`), is refused like the multiplier below.

```sql
SUCH THAT x / 2 <= 1                       -- OK: equivalent to x <= 2
SUCH THAT SUM(x / weight) <= budget        -- OK: per-row scaled sum
MINIMIZE SUM(POWER(x / 2 - 1, 2))          -- OK: QP with division inside base
MINIMIZE SUM(POWER(x / weight - 1, 2))     -- OK: data-column divisor in QP
```

**Code**:
- Bind-time validation: `IsAllowedNameOverDecideVar` and the dedicated `/`-arm of `ValidateDecideNoNonLinearScalar` (per-row pre-pass) and `ValidateSumArgumentInternal` (SUM/POWER inner) in `src/planner/expression_binder/decide/decide_binder.cpp` reject any `/` whose divisor contains a decide variable.
- Per-row extraction: `ExtractTerms` at `src/optimizer/decide/decide_linear_form.cpp` walks `/` by recursing into the numerator and wrapping each emitted coefficient as `coef / divisor`.
- QP linearity check: `IsLinearInDecideVars` in the same file accepts `/` when the divisor is decide-var-free, so quadratic patterns like `POWER(x/2 - 1, 2)` reach the QP extractor.

### Data-only operators and named functions the algebra doesn't model (`%`, bitwise, `mod()`, `floor()`, …)

DECIDE models a fixed set of operators (`+ - * / ^ **`) plus a handful of named forms (`abs`, `power`, the reducers). Any *other* operator — `%`, bitwise ops — or *named scalar function* — `mod()`, `floor()`, `sqrt()`, … — is rejected inside a DECIDE clause **only when it touches a decision variable**. When the operands reference no DECIDE variable, the whole subexpression is a per-row constant: nothing needs to model it, and the physical layer evaluates it through DuckDB as an ordinary coefficient:

```sql
SUCH THAT SUM(((id * 7) % 97) * x) <= 3   -- OK: `(id*7)%97` is a per-row coefficient
MAXIMIZE SUM(((id * 7) % 97) * x)         -- OK: same, in an objective
SUCH THAT SUM(mod(id, 5) * x) <= 3        -- OK: named function, data-only coefficient
MAXIMIZE SUM(floor(price) * x)            -- OK: same, in an objective
SUCH THAT SUM(COALESCE(weight, 0) * x) <= 3 -- OK: operator node, data-only coefficient
SUCH THAT SUM((x % 97)) <= 3              -- rejected: `%` wraps a decision variable
SUCH THAT SUM(mod(x, 5)) <= 3             -- rejected: function wraps a decision variable
SUCH THAT SUM(COALESCE(x, 0)) <= 3        -- rejected: operator wraps a decision variable
```

This is the same "fold data-only subterms" idea already applied to `x + cost` and `x / col`, generalized to operators *and* named scalar functions outside the modelled set.

**Rejected — data-only aggregates as coefficients**: `SUM(avg(col) * x)` is a different beast — the inner aggregate needs the whole row set, not a per-row fold — so it is rejected (not folded here). A data-only aggregate (`avg`/`min`/`max`/`sum`/`count` over columns only) that shares a scaled term with a decision — a factor (`avg(col) * x`), a divisor (`sum(x) / sum(col)`), or inside a divided term (`(sum(x) + sum(col)) / 2`) — is caught in `ValidateDecideNoNonLinearScalar` on the parsed tree, before binding. The message says to pre-compute it as a scalar in a subquery or CTE. A data-only aggregate as a constraint *RHS* (`SUM(x) <= AVG(col)`) stays supported.

**Code** (operators and functions share the mechanism):
- Bind-time: `ValidateSumArgumentInternal` in `src/planner/expression_binder/decide/decide_binder.cpp` returns success (instead of an error) when `ExpressionContainsDecideVariable` is false — both in the unsupported-operator arm and in the unsupported-named-function arm.
- Downstream: a data-only subterm is simply an atom nothing opens. The canonicalizer treats it as one term, and physical coefficient evaluation runs it through DuckDB's own `ExpressionExecutor` — so any scalar function DuckDB can evaluate works as a coefficient without DECIDE needing to model it.

**Tests**: `test/decide/tests/test_error_unsupported_operator.py` — operator forms (`test_modulo_data_coefficient_matches_oracle`, `test_modulo_data_coefficient_in_constraint_runs`), function forms (`test_mod_function_data_coefficient_matches_oracle` oracle-verified, `test_floor_function_data_coefficient_in_constraint`), and the `TestUnsupportedOperatorOverVariableRejection` cases pinning that a variable-bearing `%` or `mod()` still errors without a stack trace. `test/decide/tests/test_null_coalesce_reducer.py` covers the `COALESCE`/`IFNULL` operator node in reducer constraints and objectives and keeps its decision-bearing form rejected.

### Per-row linear LHS (`+ const`, `- col`, `/ const`, unary `-`)

Per-row constraints accept full linear shapes on the LHS, not just `x op K` and `c*x op K`. The extractor walks the LHS tree to separate decide-variable terms from constants and row-varying data columns; the latter are moved to the RHS per row so the solver sees an algebraically equivalent `sum(decide_terms) op adjusted_rhs` constraint.

```sql
SUCH THAT x + 3 <= 10          -- x <= 7
SUCH THAT x - ps_availqty <= 1 -- per row: x <= 1 + ps_availqty[row]
SUCH THAT -x <= -2             -- x >= 2
SUCH THAT 2 * x + 3 <= 11      -- x <= 4
SUCH THAT x / 2 + 1 <= 3       -- x <= 4
```

**Code**: `ExtractTerms` in `src/optimizer/decide/decide_linear_form.cpp` handles `+`, `-` (binary and unary), `*`, `/` (divisor must be decide-var-free), and `CAST`. `ExtractConstraintTerms` delegates there. In `src/decidb/formulation/ilp_model_builder.cpp`, the per-row constraint loop subtracts LHS terms whose `variable_index == INVALID_INDEX` (constants / row-data) from the per-row RHS instead of silently dropping them.

**Tests**: `test/decide/tests/test_cons_perrow.py` — `test_perrow_linear_lhs_upper_bound` (parametrized over `x+c`, `x-c`, `x/c`, `c*x+c`, `x/c+c`, `x+c-c`), `test_perrow_unary_minus_lower_bound`, `test_perrow_data_column_in_lhs`, all oracle-verified.

---

## Comparison Operators

### =, <, <=, >, >=

Six standard comparison operators are supported in constraints (including `<>`, documented below).

```sql
SUCH THAT x <= 1
SUCH THAT SUM(x * w) >= 10
SUCH THAT SUM(x) = 5
```

`<>` (not-equal) is supported on both per-row and aggregate constraints. It uses Big-M disjunction with an auxiliary binary indicator variable:

- `SUM(x) <> K` → two constraints: `SUM(x) <= K-1 + M*z` and `SUM(x) >= K+1 - M*(1-z)`
- `x <> K` → same pattern applied per-row

**Complexity**: Adds 1 binary variable and 2 constraints per `<>`. The Big-M value M is computed from variable bounds at execution time. Loose bounds produce weaker LP relaxations.

**Code**: Auxiliary indicator variable created by `DecideOptimizer::RewriteNotEqual` in `decide_rewrite_notequal_avg.cpp`; Big-M constraints generated at execution time in `physical_decide.cpp` (`Finalize()`), where data bounds are available.

### BETWEEN ... AND ...

Desugars to `>= lower AND <= upper`. Produces two constraints.

```sql
SUCH THAT SUM(x) BETWEEN 10 AND 50
-- equivalent to: SUM(x) >= 10 AND SUM(x) <= 50
```

### IN (...)

Constrains a value to be in a literal set. Works on both table columns and decision variables.

```sql
SUCH THAT category IN ('A', 'B', 'C')   -- table column (SQL filter)
SUCH THAT x IN (0, 1, 3)                -- decision variable domain restriction
```

**Decision variable IN**: `x IN (v1, ..., vK)` binds as a marker and is lowered by `DecideOptimizer::RewriteInDomain` into K auxiliary binary indicator variables with two constraints:
- Cardinality: `z_1 + ... + z_K = 1` (exactly one value selected)
- Linking: `x = v1*z_1 + ... + vK*z_K` (x takes the selected value)

**Complexity**: Adds K binary variables and 2 constraints per IN. For small K (2–5 values) this is cheap. Large K (e.g., 100 values) adds significant model size — consider whether the domain can be expressed as a range constraint instead.

**Optimizations**:
- `x IN (0, 1)` on BOOL → trivially satisfied, no rewrite
- `x IN (v)` single value → rewritten to `x = v`

**Code**: `decide_rewrite_norm_in.cpp` (`DecideOptimizer::RewriteInDomain`), run on the bound tree. IN on aggregates (e.g., `SUM(x) IN (...)`) remains unsupported.

---

## NULL-Related Expressions

### IS NULL / IS NOT NULL

Supported in `when` conditions and the `WHERE` clause (not over decision variables).

```sql
when distance is not null: imputed_distance = distance AND
when distance is null and mode = 'walk-bike': imputed_distance <= 10
```

Each `when` must keep a row; one that keeps none is an error (`../scope/done.md` §3).

---

## Boolean / Logical Operators

### AND — Two Roles

1. **Constraint separator** at the top level of `SUCH THAT`
2. **Logical `and`** inside a `when` condition: everything before the `per` or the `:` belongs to the condition, so it needs no parentheses

### OR

Valid inside a `when` condition, a data value or a subquery. Between constraints it is refused (*"SUCH THAT: '((x <= 1) OR (x <= 2))' joins constraints with OR, which is not supported; every constraint must hold, so join them with AND. To choose between alternatives, model the choice with a BOOL decision."*).

---

## norm(expr, p) — L_p Regularization (Desugared at Bind Time)

`norm(expr, p)` exposes a lasso/ridge-style regularization term over a
decision-variable expression. The user supplies the weight as an ordinary
coefficient, e.g. `MINIMIZE SUM(cost*x) + 0.5 * norm(x - base, 1)`. It is
kept as a DECIDE marker through binding and lowered by
`DecideOptimizer::RewriteNorm` (p = 1 / 2 / 'inf' and the p = 0 indicator
links), so it inherits all downstream handling (ABS / MAX / POWER
linearization, scopes, `by`) and works in both objectives and constraints. Lowering
after binding means types, scopes and casts are already resolved.

| `p` | desugars to | meaning | class |
| --- | ----------- | ------- | ----- |
| `1` | `SUM(ABS(expr))` | L1, sparse-leaning | LP |
| `2` | `SUM(POWER(expr, 2))` | squared L2 / ridge | convex QP |
| `'inf'` | `MAX(ABS(expr))` | L-infinity (worst deviation) | LP |
| `0[, M]` | indicator linked so `z=1` iff `|expr| >= tol`; term → `SUM(z)` | L0 / count of nonzeros (exact) | MILP |

- **L0 indicator (exact in every context).** One INT 0/1 indicator `z` per row.
  Each term emits **three** linking constraints so `z` is the *exact* nonzero
  indicator, not merely an upper bound:
  - **Forward** (`z = 1` when `expr != 0`): the data-driven pair `M*z >= expr`,
    `M*z >= -expr` (i.e. `|expr| <= M*z`), written decision-variable-first so the
    inner expression's terms drive the Big-M.
  - **Reverse** (`z = 0` when `|expr| < tol`): `ABS(expr) >= tol*z`. **The
    soundness-critical link.** Without it `SUM(z)` is only an *upper* bound on the
    count — sound when the context pushes the count down (`MINIMIZE` penalty,
    `norm <= K`) but **unsound** when it pulls the count up (`norm >= K`, `= K`,
    `MAXIMIZE`): a spurious `z=1` on a zero row inflates the count and a problem
    that should be infeasible solves anyway. The reverse reuses ABS linearization
    (its lower-bounded form is tagged for the exact Big-M envelope), so `expr` must
    have **finite bounds**.
  - The term becomes `SUM(z)`.
- **Nonzero tolerance.** A row counts as nonzero when `|expr| >= tol`. `tol` must
  sit above the solver feasibility tolerance (~1e-6) or the reverse link is not
  enforced (the boundary violation is exactly `tol`); default `1e-4`, configurable
  via `SET decide_l0_tolerance` (floor `1e-5`, validated). The forward link is kept
  exact (`z = 0 ⇒ expr = 0`) so "zero" rows read back as clean `0`; the trade-off is
  a small **dead zone** — a value *forced* into `(0, tol)` has no valid `z` and is
  reported infeasible (pathological, and genuinely tolerance-ambiguous).
  Code: `decide_rewrite_norm_in.cpp` (`DecideOptimizer::RewriteNorm`); the tolerance setting lives in
  `decide_diagnostic.cpp` (`L0ToleranceSetCallback` / `GetDecideL0Tolerance`).
- **Big-M source.** `norm(expr, 0)` uses a placeholder `M` on the forward links
  (indicator `__l0auto_ind_*`) and the physical operator fills a tight per-problem
  `M` after implied-bound propagation (`DecideTightPerRowBigM`, the `<>` path); the
  refill skips the reverse link (its only non-indicator variable is the ABS aux).
  `norm(expr, 0, M)` supplies `M` explicitly (`__l0_ind_*`). The fill lives in
  `physical_decide.cpp` (`Finalize`, right after implied-bound propagation).
- Usable as an objective penalty, a sole objective, or a constraint
  (`norm(e, 1) <= K`, and the exact count cap/floor `norm(e, 0[, M]) <= K` / `>= K`).
- **Composes with arithmetic anywhere.** A norm is a scalar, so it may sit under
  `+`, `*` or a cast at any depth on either side — `MINIMIZE SUM(cost*x) + 0.5 *
  norm(x - base, 1)`, `MINIMIZE norm(e, 1) + 0.25 * norm(e, 2)` (elastic-net
  shaped), `SUCH THAT 2 * norm(e, 1) <= K`. `RewriteNorm` descends every container
  to reach it, and `VerifyNormMarkersLowered` fails the query rather than let a
  marker leave the optimizer, where it would read as the plain `SUM(e)` it is built
  on and the norm would be silently absent from the model. The one place a norm may not go is inside another aggregate
  (`SUM(norm(...))`), which the binder rejects.
- Unsupported orders (e.g. `p = 3`) and `norm(e, 0)` without `M` raise a clear
  binder error.
- The user supplies the weight λ directly; scale-free `α`/`λ_max` auto-selection
  is intentionally not built (see project memory).
- Tests: `test/decide/tests/test_norm.py` — per-order desugaring equivalence,
  composition with scopes and `by`, HiGHS backend (QP + MILP), error paths, and L0 exactness
  (`test_norm_l0_*`: lower-bound/equality/maximize infeasibility on both backends,
  feasible-is-honest, and the `decide_l0_tolerance` pragma). Composition with
  arithmetic is pinned separately (`test_norm_*_in_arithmetic_*`,
  `test_norm_scaled_*`, `test_norm_combined_l1_l2_objective`): each compares the
  *composed* objective against the same composition written out by hand, which is
  the only spelling that can tell a dropped marker from a lowered one.

## Summary Table (Implemented Only)

| Function / Operator | In Constraints | In Objective | In `when` / WHERE |
|---|---|---|---|
| `SUM()` over dec. vars | Yes | Yes | N/A |
| `AVG()` over dec. vars | Yes (RHS scaled) | Yes (→SUM) | N/A |
| `MIN()` / `MAX()` over dec. vars | Yes (per-row / Big-M) | Yes (global aux / Big-M) | N/A |
| `ABS()` over dec. vars | Yes (linearized) | Yes (linearized) | N/A |
| `norm(expr, p)` (p = 1 / 2 / 'inf' / 0,M) | Yes (incl. count cap) | Yes (penalty / sole) | N/A |
| `*` (var x const/col) | Yes | Yes | N/A |
| `*` (var x var, bilinear) | Yes (McCormick / Gurobi QCQP) | Yes (McCormick / Gurobi non-convex) | N/A |
| `POWER(expr, 2)` / `expr ** 2` (QP) | N/A | Yes (convex: both solvers; non-convex: Gurobi) | N/A |
| `+`, `-` (binary and unary) | Yes (per-row linear LHS shapes fully supported) | Yes | Yes |
| `/` (by data column or constant) | Yes | Yes | N/A |
| `=`, `<`, `<=`, `>`, `>=` | Yes | N/A | Yes |
| `<>` (not-equal) | Yes (Big-M) | N/A | Yes |
| `BETWEEN` | Yes | N/A | Yes |
| `IN (...)` | Yes (dec. vars + columns) | N/A | Yes |
| `IS NULL` / `IS NOT NULL` | N/A | N/A | Yes |
| `AND` (constraint sep.) | Yes | N/A | N/A |
| `AND` / `OR` (logical) | Over data inside a value or subquery; `OR` between constraints **rejected** | N/A | Yes |
| Any other scalar (`SQRT`, `EXP`, `LN`, `LOG`, `FLOOR`, `CEIL`, `ROUND`, trig, ...) over a DECIDE variable | **Rejected** (non-linear) | **Rejected** (non-linear) | Yes (over data columns) |
