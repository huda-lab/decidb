# Declarations, constraints, objectives and functions

What each construct means beyond [guide.md](guide.md), why it works that way, and where the code is. The formulation
mechanics (Big-M constants, envelope rows, McCormick corners) belong to stages 05 and 06:
`../pipeline/05_optimizer/done.md` and `../pipeline/06_model_formulation/done.md`.

## Declarations

- **Two clause orders, one plan.** The grammar has two optional slots, `decide_declaration` before `from_clause` and
  `decide_body` after `where_clause`; `makeDecideClause()` (`third_party/libpg_query/grammar/grammar.cpp`) merges them
  into one `PGDecideClause`, so nothing after the parser knows which order was used. The `in_decide_clause` lexer flag
  is cleared when the declaration slot reduces and set again on `SUCH`, so `CASE WHEN` in a `JOIN … ON` or `WHERE`
  between the slots lexes as plain SQL. Tests: `test_clause_order.py`.
- **Types.** `variable_type: INT_P | REAL | BOOL_P` in `select.y`. The binder maps `REAL` to `DOUBLE`, `INT` to
  `BIGINT` and `BOOL` to `INTEGER`. `BOOL` stays `INTEGER` so it can appear in arithmetic (`x * weight`), which
  `BOOLEAN` cannot. The domain signal is separate: `is_boolean_var`, carried from the binder to `SolverInput`.
  `PhysicalDecide::Finalize` reports `BOOLEAN` to the solver for every variable with that flag, including optimizer
  auxiliaries, and the model builder gives it the `[0, 1]` box. Nothing is added to `SUCH THAT` for the domain.
- **A `BOOL` bound outside `[0, 1]`** is Boolean arithmetic: a bound that always holds (`x <= 5`) changes nothing; one
  that cannot hold (`x >= 2`) empties the box and raises before any row is read, quoting the bound
  (`DecideGlobalSinkState`). Under a `when` it is an ordinary row. Tests: `test_bool_bound_outside_domain.py`.
- **Signed variables.** Singleton-row folding (`decide_singleton_row_folding.cpp`) starts each lower bound at −1e30
  so the `std::max` combiner keeps the tightest `>=` bound for negatives too, and records whether any lower bound was
  written; `Finalize` gives 0 to a variable with none. The model builder takes the resolved lower bound directly
  instead of clamping it to the type default. A bound of magnitude 1e20 or more is no bound (D66), and a literal
  `x >= -'inf'` frees `x` below the same way (D65). Implied-bound propagation
  does not run for a variable with a negative lower bound (skipping is sound), so a signed variable in a bilinear
  product needs a written upper bound. Tests: `test_signed_variables.py`.
- **A bound on one decision admits what DuckDB admits** (D64). `DecideReadSingletonRow`
  (`decide_singleton_row_folding.cpp`) gives the interval a comparison like `0.1 * x <= c` admits as two DOUBLE
  expressions; on a whole-numbered decision each end is a whole number the written comparison, evaluated by DuckDB with
  that number cast to the decision's type, passes. The optimizer folds a literal interval, stage 08 evaluates a data
  one per row, and direct solve reads the same expressions. An infinite end keeps its SQL meaning (D65): an open end is
  no bound, and an end that admits nothing is answered infeasible before any backend (`SolverModel::ProvesInfeasible`).
  Tests: `test_singleton_row.py`, `test_infinite_bounds.py`.
- **Query-wide decisions** occupy their own block of `VarIndexer` (`[row | keyed | query-wide | auxiliary]`), below
  `global_block_start`, so "user decisions occupy `[0, global_block_start)`" holds for the model builder and the
  diagnostic labeller. `Get()` ignores the row for them. The model builder adds a query-wide coefficient **once per
  model row** when it stands beside a reducer, and accumulates it over the counted rows when it is inside one; the
  two cases are told apart by `DecideTerm::reduction` (`NONE` vs `SUM`), carried to the objective side as
  `objective_term_reductions`. A reducer whose body is row-invariant is refused in `ClassifyReducerCall` via
  `IsRowInvariantExpression` (`decide_binder.cpp`). Under DIAGNOSE a query-wide decision has one instance, so an
  unbounded report carries no escape count. Tests: `test_scalar_scope.py`, `test_scalar_var_in_aggregate.py`.
- **Keyed decisions** resolve as in [scopes.md](scopes.md#binder); `EntityScopeInfo` (`logical_decide.hpp`) holds the
  key as written, its bindings and types, the relations it reads (`source_table_indices`) and the decisions declared
  on it. Physical index resolution is in `plan_decide.cpp`.

## Constraints

- **Side does not matter.** `BindComparison` (`decide_constraints_binder.cpp`) classifies both sides and accepts when
  either holds a decision; the canonicalizer then moves decision terms left and data right. This happens before
  bounds fold into column boxes, so diagnostics still see the canonical bound.
- **A bound column is like a subquery.** `IsAllowedDecisionFreeBoundExpression` accepts a plain column exactly as it
  accepts a scalar subquery; `RewriteRhsAggregates` reduces either per group and does not care where the value came
  from.
- **Strict bounds and `<>`.** A `REAL` decision is refused by `ValidateDecideNoIntegerStepComparisonOnReal` on the
  parsed tree; any other fractional operand by `ValidateDecideIntegralComparisonOperands` on the bound tree. A
  McCormick auxiliary for `BOOL × INT` is declared `INTEGER` (`decide_rewrite_bilinear.cpp`) so the product still
  counts as whole-numbered. The model builder keeps a value check for a coefficient that evaluates fractional
  (`IsEvalConstraintLhsIntegerValued`).
- **`<>` details.** Lowered, every contributing variable must be bounded, because no finite Big-M dominates an
  unbounded range and a too-small one would silently cut the feasible region. A fractional `K` is a tautology on a
  whole-numbered side and is dropped: per row for each row whose `K` is fractional, per group for `AVG(x) <> K` when
  `K·n_g` is fractional. When `K` varies inside a group, `ExpandAggregateNotEqual` (`linearization_not_equal.cpp`)
  gives each distinct value its own binary and Big-M pair against the same left side. For `AVG(x) <> K` the left side
  stays an integer `SUM` and the right is scaled (`ne_avg_rhs_scale`). A `<>` that reads only some rows stores its
  indicator column sparse (`CoefficientColumn::SparseMasked`). The integer tolerance is `1e-9`.
- **Composed `MIN`/`MAX`.** An additive side may mix `MIN`/`MAX` terms with `SUM`/`AVG`
  (`sum(x * v) + max(x * v) <= 20`). Each `MIN`/`MAX` becomes an auxiliary `z_k` pinned per row by its envelope; when
  the outer comparison pushes `z_k` the wrong way (or for `=`), an indicator layer pins it to the true extreme. Signs
  thread through subtraction (`WalkComposedLhs`), so a subtracted `MAX` under `<=` is the hard direction. `=` is one
  native row; `BETWEEN` works but costs two auxiliaries. A `<>` is refused. With a `by`, a non-constant bound or a
  query-wide decision beside, the clause takes the VALUE shape instead. The bound test is foldability, not a literal
  node, so `(0 - 3) + 0` still counts as constant. All links go through `EmitExtremumLink` (`linearization_minmax.cpp`).
- **Subqueries.** Correlated subqueries are decorrelated by DuckDB (`PlanSubqueries`) into per-row values.
  `DecideBinder::BindExpression` refuses a subquery that references a decision (`ExpressionContainsDecideVariable`).
- **NULL.** A reducer over data reads its argument as the same SQL aggregate does: it skips a NULL, so `AVG(cap)`
  divides by the caps that are not NULL. A group whose every value is NULL has a NULL reducer, and a NULL bound is an
  error (`DECIDE: max(cap) by (g) is NULL. Impute it with COALESCE(), ...`). Every other NULL the model reads is an
  error, a NULL that multiplies a decision (`sum(price * x)`) included: skipping it would change the model silently.
  `ExtractDoubleColumn` is the one gate every evaluated coefficient and bound passes; on failure
  `CollectNullColumnsAtRow` / `DescribeNullSource` name the column by walking the expression's leaf references, so a
  NULL inside `x * (weight / capacity)` names `weight`. A bound's reducer is a column by then; it carries
  `BOUND_REDUCER_TAG`, so a message quotes it as written instead of naming it as a column. No row number is given:
  the fix is to the column, and a scan ordinal is not something a user can look up. `COALESCE`/`IFNULL` work inline
  when decision-free. Tests: `test_null_coalesce_reducer.py`, `test_rhs_aggregate_window.py`.

## Objectives

- **Feasibility.** With no objective, `DecideSense::FEASIBILITY` sets every objective coefficient to zero.
- **Composed `MIN`/`MAX` objectives** (`MAXIMIZE min(when tier: x * profit) + sum(x * revenue)`) use the same
  auxiliary and walker as composed constraints; the sense plays the comparison's role. A keyed reducer inside keeps
  its de-duplication (`ComposedMinMaxTerm::qualifier_scope_idx`); for `MIN`/`MAX` that is a no-op, applied anyway so
  there is one code path.
- **Quadratic inner in the two-level form**: `sum(per g: sum(power(x - t, 2)) by (g))` reaches the QP extractor
  intact, since nothing expands it before binding. A `MIN`/`MAX` over a squared body is modelled as a sum of squares —
  a known wrong answer (`../issues/bugs.md`).
- **Linear objectives** become the `c` vector; a `when` in front zeroes other rows' coefficients, a reducer's own
  `when` zeroes only its term's. Additive constants accumulate on `LogicalDecide::objective_constant_offset`.
- Code: `decide_objective_binder.cpp` (`BindScopedObjective`, `BindTwoLevelObjective`); `AnalyzeObjective` and
  `ExtractAggregateObjectiveTerms` (`decide_linear_form.cpp`).

## Reducers

- **Accepted reducers.** The binders accept `SUM`, `AVG`, `MIN`, `MAX` (and `count(*)`, `norm` with a `by`); others
  (`STDDEV`, `BIT_AND`) are refused in `BindAggregate`.
- **Data terms inside `SUM`/`AVG`** are evaluated over the same rows as the decision terms (kept rows, `by` group, own
  `when`, own `per`, `AVG` scaling) and subtracted from the bound.
- **Reducers as a bound.** Any reducer may be a bound, evaluated to one value per group by its own evaluator, so
  `MIN` and `MAX` work there as well as `SUM`/`AVG`: `sum(x * price) <= min(price) * 4`,
  `sum(x) by (g) <= count(*) by (g)`. A bound reducer's own `when` narrows only itself. The single implementation is
  `RewriteRhsAggregates`: each bound reducer becomes a DuckDB window column partitioned by its `by` key over the kept
  rows (a keyed or colon reducer counts the first row of each class via `ROW_NUMBER`). So the solver and direct solve
  read one number computed in DuckDB's own types, and nothing is computed on a dropped row. `AVG` on a bound keeps its
  fractional value: the AVG→SUM rewrite skips an `AVG` with no decision. Tests: `test_cons_rhs_aggregate.py`.
- **`AVG`.** Binding keeps `AVG` (and its `DOUBLE` type, so a fractional bound survives coercion). The optimizer
  rewrites it to `SUM` tagged `AVG_REWRITE_TAG`; execution scales linear and bilinear coefficients by `1/n` and
  quadratic inner terms by `1/√n`. In the two-level objective, `avg` inside scales each row by `1/n_g`. Flat
  `MAXIMIZE AVG(e)` has the same argmax as `SUM(e)`, but mixed terms keep their own scaling, so `AVG(a) + SUM(b)` is
  not `SUM(a) + SUM(b)`. Tests: `test_avg.py`.
- **`MIN`/`MAX`.** The inner expression is any linear combination plus a constant (`max((cost + 1) * x)`,
  `max(cost * x + 5)`). Each link row is accumulated per solver column before emission (`MinMaxLinkRow`), because a
  column can reach one row more than once (a distributed term, a keyed or query-wide variable) and both solvers reject
  a repeated column index. Constants fold into the row's bound. On Gurobi the hard cases are general constraints with
  no Big-M; the choice is made once at execution from the backend's capability table, and both paths reach the same
  optimum. Classification and scaffolding: `RewriteMinMaxConstraints` (`decide_rewrite_minmax.cpp`).
- **The colon form.** `SUM(D: e)` binds onto the exact key scope `sum(per D: e)` uses, so the two share one
  `EntityMapping`, one de-duplication mask and one `DETERMINES` check. A composite qualifier's key is every named
  relation's stored columns, sorted by binding, so order does not matter. The grammar reads the qualifier as an
  argument list and checks its shape in the C action, because `sum(D, T: e)` and `sum(a, b)` share a prefix LALR(1)
  cannot split at the comma. Parse node `PG_AEXPR_QUALIFIED_REDUCER` → `QUALIFIED_REDUCER_TAG`
  (`transform_operator.cpp`). A query-wide decision inside is allowed unless it is the whole body. Tests:
  `test_qualified_reducer.py`.

## Scalar functions and operators

- **`ABS`** over a decision becomes an auxiliary `d` with the lower envelope `d >= e`, `d >= -e`. Each occurrence is
  classified by the sign it carries in `LHS - RHS` (through `+`, `-`, casts, reducer bodies and literal factors):
  - *Path A* — the comparison pushes `d` down (`ABS(e) <= K`, `SUM(ABS(e)) <= K`, `MINIMIZE SUM(ABS(e))`): the
    envelope alone is exact.
  - *Path B* — anything else (`ABS(e) >= K`, `=`, `<>`, `BETWEEN`, `IN`, a subtracted ABS, `MAXIMIZE SUM(ABS(e))`, a
    data weight of unknown sign, an ABS under a hard `MIN`/`MAX`, an ABS inside an ABS): a sign binary and two Big-M
    rows pin `d = |e|`, so every decision in `e` needs a finite bound, written or inferred. On Gurobi Path B is a
    native absolute-value constraint with no Big-M.
  - A factor of exactly zero needs neither. `ABS` over data is plain SQL.
  - Code: `TagAbsForBigM` / `CollectAbsWithSign` and `RewriteAbs` (`decide_rewrite_abs.cpp`); links in
    `LogicalDecide::abs_maximize_links` (both Path B users, objective and constraint). Tests:
    `test_abs_linearization.py`.
- **Non-linear functions over a decision** are refused by `ValidateDecideNoNonLinearScalar` (`decide_binder.cpp`) on
  the parsed tree, before binding, in every position (per row, inside a reducer, inside `ABS`). Without it, such a
  function would be dropped or reach execution unmodelled. The same pass checks `POWER`'s exponent. A catalog lookup
  keeps scalar and aggregate refusals apart. Tests: `test_error_binder.py`.
- **Functions and operators over data only** (`%`, bitwise, `mod()`, `floor()`, `POWER(qty, 3)`, `COALESCE`) are
  per-row coefficients: the term is an atom nothing opens, and DuckDB's `ExpressionExecutor` evaluates it. A data-only
  *aggregate* inside a reducer (`SUM(avg(col) * x)`) is refused, since it needs the whole row set; compute it as a
  scalar subquery. Tests: `test_error_unsupported_operator.py`, `test_power_over_data.py`.
- **Division** by a constant or data column folds into the coefficient (`x / col` → `1/col[row]`), in per-row
  constraints, reducers and quadratic bases; division by a decision is refused. Code: `SplitLinear`
  (`decide_term_split.cpp`), `IsLinearInDecideVars`.
- **Per-row linear left sides** (`x + 3 <= 10`, `x - col <= 1`, `-x <= -2`, `x / 2 + 1 <= 3`): `SplitLinear` separates
  decision terms from constants and data, and the model builder moves the latter to the per-row bound. A decision
  under anything else is an unknown term, never read as a plain variable. Tests: `test_cons_perrow.py`.
- **`IN` on a decision** binds as a marker; `RewriteInDomain` (`decide_rewrite_norm_in.cpp`) lowers it to K binaries
  with `Σ z = 1` and `x = Σ v·z`. `x IN (0, 1)` on a `BOOL` is dropped; `x IN (v)` becomes `x = v`. Large K grows the
  model; a range is cheaper when it fits.
- **`norm`.** The binder keeps it as a marker; canonicalization replaces orders 1, 2 and `'inf'` with their
  definitions (keeping a display tag so text reads `norm(...)`), and `RewriteNorm` lowers order 0. Desugaring after
  binding means types, scopes and casts are already resolved, and every later stage sees ordinary terms. A norm may
  sit under `+`, `*` or a cast at any depth; `VerifyNormMarkersLowered` fails the query rather than let a marker
  through, where it would read as the plain `SUM(e)` it is built on. A norm inside another aggregate is refused.
  - **L0** uses three links per row: forward `|e| <= M·z` and reverse `|e| >= tol·z`. The reverse link is what makes
    the count exact; without it `SUM(z)` would only bound the count from above, which is wrong for `>=`, `=` and
    `MAXIMIZE`. The tolerance must exceed the solver's feasibility tolerance (~1e-6); default `1e-4`, floor `1e-5`
    (`SET decide_l0_tolerance`; `L0ToleranceSetCallback`). Automatic `M` is filled after implied-bound propagation
    (`DecideTightPerRowBigM`).
  - The user supplies the weight; automatic weight selection is deliberately not built.
  - Tests: `test_norm.py`.
- **Logical operators.** `AND` separates constraints at the top level and is a plain `and` inside a `when`. `OR` is
  data inside a `when` or subquery and refused between constraints. `IS NULL` works in `when` and `WHERE`, not over
  decisions.

## Summary

| Construct | In constraints | In the objective | In `when` / `WHERE` |
|---|---|---|---|
| `SUM`, `AVG` over decisions | yes | yes | — |
| `MIN`, `MAX` over decisions | yes | yes | — |
| `ABS` over decisions | yes | yes | — |
| `norm(e, p)` | yes | yes | — |
| `x * data`, `x / data` | yes | yes | — |
| `x * y` (bilinear) | yes | yes | — |
| `POWER(e, 2)` | yes (Gurobi) | yes | — |
| `=`, `<`, `<=`, `>`, `>=`, `BETWEEN`, `IN` | yes | — | yes |
| `<>` | yes | — | yes |
| `AND` / `OR` | `AND` separates; `OR` refused | — | yes |
| other scalar functions over a decision | refused | refused | yes, over data |
