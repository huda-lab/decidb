# Known Bugs — Open

## `DECIDB_TIME_LIMIT` is not enforced on HiGHS for large models

- **Location:** `src/decidb/naive/deterministic_naive.cpp` (`HighsSession`);
  contrast `src/decidb/gurobi/gurobi_solver.cpp:125`.
- **Observed:** on Q2 at the large TPC-H tier (~1.5M variables, mixed-integer),
  `highs.optimize` began and never returned — not at the requested 300s
  `DECIDB_TIME_LIMIT`, not by 420s wall clock, when an external harness killed
  the process. Every DeciDB-owned span (parse, rewrite, extraction, model
  build, `highs.load`) completed in under 350ms total; the entire remainder
  was spent inside HiGHS's own `run()`. Reproduced during the
  `pipeline_profile.py` large-tier sweep
  (`benchmark/decide/results/pipeline_profile_raw_large/Q2_highs_0/`,
  status `process_timeout`); see `05_performance/06_solver_execution/`.
- **Cause:** `HighsSession` inherits `SolverSession::SetInterruptPoll`'s
  no-op base instead of overriding it, so a HiGHS solve has no external
  cancellation path — the only thing bounding it is HiGHS's own internal
  check of the `time_limit` option, and that check's granularity was too
  coarse to return within any reasonable margin of the budget on this model.
  Gurobi does override `SetInterruptPoll` and is not affected.
- **Expected:** `DECIDB_TIME_LIMIT` bounds a HiGHS solve the same way it
  bounds Gurobi's — the query returns with a `TIME_LIMIT` status near the
  requested budget, not an unbounded hang.
- **Impact:** a user who sets a time limit expecting a bounded wait can have
  the query hang indefinitely on HiGHS once the model is large enough; there
  is no DeciDB-level safety net.

## A query-wide decision in a quadratic constraint is counted once per row

- **Location:** `BuildQuadraticConstraint` in
  `src/decidb/formulation/ilp_model_builder.cpp`, which has no once-only arm for a
  query-wide (`scalar` / `per ()`) term, unlike the objective and the linear aggregate
  paths.
- **Observed:** `SELECT i, x, s FROM range(3) t(i) DECIDE x(REAL), scalar s(REAL)
  SUCH THAT SUM(POWER(x, 2)) + s <= 30 AND s >= 3 MAXIMIZE SUM(x)` dumps
  `qrow 0: sense=< rhs=30 | 3:3 | ...` -- `s` weighted 3, once per row -- and returns
  x = 2.6458 on each row.
- **Expected:** `s` counted once, so Σx² <= 27 and x = 3 on each row.
- **Impact:** a wrong optimum, silently.

## DIAGNOSE re-emits absorbed bounds on the wrong columns for a keyed decision

- **Location:** `EmitAbsorbedUserBoundRows` in
  `src/execution/operator/decide/physical_decide.cpp`: it loops over a decision's
  instances but looks each one up with `VarIndexer::Get(var, inst)`, which takes a row,
  instead of `InstanceColumn(var, inst)`.
- **Observed:** for a `T.x` or `per K:` decision, instance `i` is mapped through row
  `i`'s class, so some classes get the bound twice and others never (code reading; the
  map of 2026-10-09 also saw DIAGNOSE answers change with row order).
- **Expected:** every instance of the decision gets the absorbed bound once.
- **Impact:** DIAGNOSE may suggest edits for a relaxation that is not the one the query
  states.

## A decision named like a USING column is silently ignored

- **Location:** the name-conflict check in `DecideDeclarationsBinder::BindDeclarations`
  uses `BindContext::GetMatchingBinding`, which skips USING columns.
- **Observed:** `SELECT * FROM (VALUES (1, 10), (2, 20)) A(k, v) JOIN (VALUES (1, 5),
  (2, 6)) B(k, w) USING (k) DECIDE k(INT) SUCH THAT k <= 3 MAXIMIZE SUM(k)` runs; the
  clause binds `k` to the join column, so the model has no row and every decision is 0.
- **Expected:** the conflict error a decision named like any other column gets.
- **Impact:** a silently wrong answer.

## A generated column splits the entities of `T.x`

- **Location:** `FindOrCreateEntityScope` in
  `src/planner/expression_binder/decide/decide_binder.cpp` keys `T.x` on every name of
  the table binding, generated columns included. The storage-level cause of the split
  was not traced. `per T:` skips generated columns and is not affected.
- **Observed:** `CREATE TABLE G(a INTEGER, b INTEGER GENERATED ALWAYS AS (a * 2));
  INSERT INTO G (a) VALUES (1), (1), (2), (3);` then `DECIDE G.x(INT)` builds 4
  decisions for 3 distinct tuples.
- **Expected:** 3, one per distinct tuple.
- **Impact:** more decisions than entities, so rows of one entity can disagree.

## `SUM(T: ...)` drops a tuple once its data column leaves the key

- **Location:** the key trimming of `T.x` and `SUM(T: ...)` in
  `DecideDeclarationsBinder::BindDeclarations` (columns the clause reads as data leave
  the key), combined with the qualifier's keep-first-row de-duplication.
- **Observed:** over `(1, 10, 'a'), (1, 20, 'a'), (2, 30, 'b')` as `T(id, cost, tag)`,
  `DECIDE T.x(INT) SUCH THAT x <= 1 MAXIMIZE SUM(T: cost * x)` keys on `(id, tag)` and
  builds objective coefficients 10 and 30: the tuple with cost 20 is never counted.
- **Expected:** every distinct tuple of `T` counted once.
- **Impact:** a wrong objective, silently. `per` keys are not trimmed (item 2 of
  `09_anr_language/todo.md`); the trimming goes with `T.x` in item 7.

## A negative `IN` value under `WHEN` is never allowed

- **Location:** WHEN-guarded comparisons are not folded into a decision's bounds
  (`decide_bound_absorption.cpp`), the `IN` rewrite emits its negative floor under the
  same `WHEN` (`decide_rewrite_norm_in.cpp`), and `PhysicalDecide::Finalize` leaves the
  untouched floor at 0.
- **Observed:** `SELECT i, r FROM (VALUES (1, true), (2, false)) t(i, p) DECIDE r(INT)
  SUCH THAT r IN (-5, 7, 12) WHEN p AND r <= 20 MINIMIZE SUM(r)` returns r = 7 on the
  row where `p` holds; -5 is optimal. Every scope is affected, keyed decisions included.
- **Expected:** -5. Open: does a negative floor under `WHEN` make the decision signed on
  every row (for a keyed decision, its whole class), or only on the rows it selects?
- **Impact:** a wrong optimum, silently.

## Any DECIDE query fails with the DECIDE optimizer switched off

- **Location:** `physical_decide.cpp` (an index into an empty vector, around line 1275);
  `05_optimizer/done.md` and `plan_decide.cpp` say the path without stage 5 works.
- **Observed:** `SET disabled_optimizers TO 'decide_optimizer'` (or `PRAGMA
  disable_optimizer`, or the unoptimized run of `PRAGMA enable_verification`), then any
  DECIDE query: *"INTERNAL Error: Attempted to access index 0 within vector of size
  0"*, which also invalidates the database.
- **Expected:** the query runs, or a message in SQL terms says the optimizer is needed.
- **Impact:** an internal error on a documented path.

## A DECIDE subquery that reads the outer row fails internally

- **Location:** decorrelation of a dependent join holding a DECIDE
  (`flatten_dependent_join.cpp`), and `ColumnBindingResolver` when the outer column is
  used only in `SUCH THAT`.
- **Observed:** `SELECT i, (SELECT MAX(x) FROM range(2) s(j) DECIDE x(INT) SUCH THAT
  x <= t.i MAXIMIZE SUM(x)) AS m FROM range(3) t(i)` gives *"INTERNAL Error: Failed to
  bind column reference "i""*; a LATERAL or WHERE reference fails in the decorrelator.
- **Expected:** a bind-time "not supported yet" naming the outer reference, until a
  correlated DECIDE subquery is supported.
- **Impact:** an internal error instead of a message.

## Key grouping ignores collation

- **Location:** `BuildGroupIds` in `src/execution/operator/decide/physical_decide.cpp`,
  shared by `per K:`, `T.x` and the old `PER`.
- **Observed:** over a `VARCHAR COLLATE NOCASE` column holding 'abc', 'ABC', 'Abc' and
  'xyz', `GROUP BY c` sees 2 groups while `DECIDE per C.c: x(INT)` builds 4 decisions.
- **Expected:** open — should key equality follow the column's collation, as GROUP BY
  does? Items 3–4 (`09_anr_language`) kept plain value equality, so a `per` or `by` key
  of the scope spellings groups the same way.
- **Impact:** more classes than SQL grouping would give on collated text keys.

## An old DECIDE spelling inside a subquery names an internal function

- **Location:** the parser markers `WHEN_CONSTRAINT_TAG` and
  `QUALIFIED_REDUCER_TAG` (`src/include/duckdb/common/enums/decide.hpp`); no
  DECIDE check looks inside a subquery for them.
- **Observed:** the lexer arms the DECIDE keywords inside a subquery written in
  the clause, so `x <= (SELECT sum(v) WHEN g > 1 FROM t)` and
  `x <= (SELECT sum(t: v) FROM t)` parse, and the subquery's ordinary binder then
  answers *"Catalog Error: Scalar Function with name __when_constraint__ does not
  exist!"* (resp. `__qualified_reducer__`).
- **Expected:** a message in SQL terms naming the spelling, as the new scope
  spellings get from `ValidateDecideScopeSpellings` in the same position.
- **Impact:** the statement is refused either way; only the message is wrong.
  The postfix `WHEN` goes with item 7 of `09_anr_language/todo.md`; `SUM(D: e)`
  stays and keeps the problem until it is fixed.

## A scaled `norm(e, 0)` leaves a constraint the debug verifier calls non-canonical

- **Location:** `RewriteNorm` in `src/optimizer/decide/decide_rewrite_norm_in.cpp` lowers
  the marker in place (`2 * norm(e, 0, M)` becomes `2 * SUM(z)`), so the comparison is not
  canonicalized again, and `VerifyCanonical` at `PhysicalPlanGenerator::CreatePlan`
  finds that it would be.
- **Observed:** on a debug build, `test/decide/tests/test_norm.py::test_norm_scaled_l0_constraint`
  (`2 * norm(new_qty - l_quantity, 0, 1000) <= 6`) raises *"INTERNAL Error: DECIDE
  canonical invariant C4/C6/C7 violated: canonicalizing the tree again changes its
  structure, reducer scale, or provenance tags"*, on this branch and on its parent
  commit `1c12622bbb`. A release build runs it and answers correctly.
- **Expected:** a lowered clause re-enters through `LogicalDecide::AddConstraint` (or is
  canonical as emitted), so the debug check holds.
- **Impact:** debug builds only; release skips the check.

## `MAX(x) <= MAX(v)` raises an internal error

- **Location:** the old spelling's easy MIN/MAX rewrite (`RewriteMinMaxInConstraint` in
  `src/optimizer/decide/decide_rewrite_minmax.cpp`) turns the clause into the per-row
  `x <= MAX(v)`, which the linear form then classifies as neither per-row nor aggregate.
- **Observed:** over `(1,'b',9),(2,'b',1),(3,'a',2) t(id,g,v)`, `x <= 10 AND
  MAX(x) <= MAX(v)` raises *"INTERNAL Error: DECIDE constraint reached term extraction
  with invalid aggregate/per-row homogeneity: '(x <= CAST(max(v) AS BIGINT))'"*, on this
  branch and its parent. `per (): max(x) <= max(v)` answers `[9, 9, 9]`.
- **Expected:** every `x` bounded by 9.
- **Impact:** an internal error on a valid query.

## Old spellings: messages that name internal columns, tags or casts

- **Observed** (on this branch and its parent):
  - `AVG(t: y) < 3` with `t.y(INT)`: *"Comparison '<' is not supported here:
    '__qualified_by_0__' has type DOUBLE ..."* (`FindFractionalOperand` names the
    aggregate by its alias, which holds the qualifier tag).
  - `MAX(x) - MIN(x) <= MAX(D: capacity)`: *"Composed MIN/MAX in DECIDE v1 requires a
    constant RHS; got 'CAST(__qualified_by_0__ AS BIGINT)'"* (`EvaluateComposedClauses`
    prints the bound with `ToString`).
  - A NULL correlated scalar subquery, `x <= (SELECT max(u.cap) FROM U u WHERE u.g = t.g)`:
    *"DECIDE: column "__source_fragment_0____row_varying_subquery__" is NULL. Impute it
    with COALESCE(__source_fragment_0____row_varying_subquery__, 0) ..."*
    (`DescribeNullSource`).
  - `MAXIMIZE SUM(x) - MAX(x) + s` with `scalar s(INT)`: *"... Got non-aggregate term:
    CAST(s AS HUGEINT)"* (`WalkComposedLhs`).
  - `x <= 1 WHEN (row_number() over () > 1)`: *"Not implemented Error: Unimplemented
    expression class"*; the scope spellings refuse a window function in a `when`, a key
    or a `by` at bind time.
- **Expected:** the query's own SQL, as the scope spellings quote it (D17 keeps the old
  text).
- **Impact:** messages only.

## Messages print a subquery, a condition or a `by` key as DuckDB renders it

- **Location:** a subquery's source fragment, and a `by` key's text, is its parsed
  expression's `ToString` (`TagDecideSourceFragments` in `decide_source_provenance.cpp`,
  `BindReducerBy`), and a condition is re-rendered from its bound form
  (`RenderDecideSource`).
- **Observed:** `when v in (SELECT 1000): x <= 1` gives *"DECIDE: when (v = ANY(SELECT
  1000)) matches no rows."*; `when exists (SELECT 1 WHERE false): ...` gives
  *"... when EXISTS(SELECT 1 WHERE CAST('f' AS BOOLEAN)) ..."*; `when id > 100 and g is
  not null:` gives *"when id > 100 AND (g IS NOT NULL)"*. A `VALUES` list in a subquery
  becomes `(SELECT * FROM (VALUES ('East', 30)) AS valueslist) AS c(region, cap)`, in a
  `when` message and in the NULL-subquery message alike; `by (id % 2)` is quoted as
  `by ((id % 2))`.
- **Expected:** the text as written. That needs each node's source span, which the
  parser does not keep (only its start).
- **Impact:** messages only; they stay recognizable.

## Messages raised while choosing a formulation name columns without their relation

- **Location:** stage 05 has no bind context, so `RenderDecideSource` there (the
  composed walker's *"'s' beside MIN or MAX"*, the scoped objective's product refusal)
  cannot qualify a column by its relation over a join; stage 04's refusals can
  (`QuoteMessagesWith`).
- **Observed:** over the deck's join, `MAXIMIZE when S.priority: sum(ship) - max(ship) +
  D.capacity * s` gives *"DECIDE: 'capacity * s' beside MIN or MAX is not supported
  yet"*. A reducer's `by` key keeps its written text either way.
- **Expected:** `'D.capacity * s'`: stage 05 carries the relation names (a `LogicalDecide`
  field, serialized), or the refusal moves to stage 04.
- **Impact:** messages only.

## Old spellings: wrong answers found while building items 3–4

Each case below runs through code the scope spellings share, where a `scoped` flag
keeps the old order (decision D17 of `09_anr_language/todo.md`): the new spelling of
the same query is correct, or refused in SQL terms, and covered by
`test/decide/tests/test_scope_*.py`. Every
query was run on this branch and on its parent commit with the same answer.

### The objective's WHEN zeroes a query-wide decision, or reads row 0's data for it

- **Location:** `EvaluateObjective` in `src/execution/operator/decide/physical_decide.cpp`
  (the WHEN loop zeroes every row outside the condition) and the SCALAR arm of the
  objective in `src/decidb/formulation/ilp_model_builder.cpp` (reads `col.Get(0)`).
- **Observed:** over `(1,'a'),(2,'b'),(3,'b')`, `MAXIMIZE SUM(x) + s WHEN g = 'b'` with
  `scalar s(INT)`, `s <= 7` returns `s = 0`: row 0 fails the WHEN, so `s` gets
  coefficient 0. `MAXIMIZE SUM(x) - w * s` takes row 0's `w` for `s` whatever the
  other rows hold.
- **Expected:** `s` counted once (`s = 7`); a data factor that varies over the rows
  refused, as `when c: ... + w * s` refuses it (`per () does not determine w`).
- **Impact:** a wrong optimum, silently.

### A relation-qualified reducer de-duplicates before its own WHEN

- **Location:** `BuildQualifierKeepMask` callers in `physical_decide.cpp` (constraint
  terms, bound-side reducers, the objective, composed terms) keep each tuple's first
  row among all rows, then apply the filter.
- **Observed:** `S(1,'D1',flag false),(2,'D1',true),(3,'D2',true)` joined to
  `D(D1, cost 7),(D2, 5)`: `SUM(x) <= SUM(D: cost) WHEN (S.flag)` bounds the total by 5
  instead of 12, since D1's first row fails the flag. In the composed
  `SUM(D: open) WHEN (S.flag) + MAX(y) <= 1`, D1 drops out and both depots open.
- **Expected:** each tuple counted once among the rows the WHEN keeps.
- **Impact:** a constraint looser or tighter than written, silently.

### Easy MIN/MAX drops a WHEN or a PER

- **Location:** `RewriteMinMaxInConstraint` in `src/optimizer/decide/decide_rewrite_minmax.cpp`.
- **Observed:** `MAX(x) = 2 WHEN c` bounds every row by 2, `c` false included (the easy
  half loses the WHEN). `MAX(x) <= cap PER g` bounds every row by the tightest `cap` of
  all rows, ignoring the groups, and bounds the NULL-key rows PER excludes.
- **Expected:** the members of the WHEN rows, per group, under the group's bound.
- **Impact:** a wrong optimum, silently.

### `BETWEEN ... WHEN` bounds every row

- **Location:** `AnalyzeConstraint` in `src/optimizer/decide/decide_linear_form.cpp`: a
  BETWEEN binds to a plain conjunction of its two halves, and a plain conjunction under a
  WHEN wrapper analyzes each child without the WHEN.
- **Observed:** `x BETWEEN 1 AND 2 WHEN (w > 2.5)` bounds the `w <= 2.5` rows too.
- **Expected:** only the rows the WHEN keeps.
- **Impact:** a wrong optimum, silently.

### A reducer inside a reducer is silently modelled as one reducer

- **Location:** the old spellings are not checked for nesting (the scope spellings refuse
  it with *"an aggregate inside an aggregate is not supported yet"*).
- **Observed:** `SUM(MAX(x)) <= 5` over four rows is modelled as `SUM(x) <= 5`.
- **Expected:** refused, or `4 · MAX(x) <= 5`.
- **Impact:** a wrong optimum, silently.

### `AVG` under `<>`: joined rows, mixed terms

- **Location:** the `ne_avg_hoist` in `EvaluateConstraints` (`physical_decide.cpp`) and
  `linearization_not_equal.cpp`, which scales the bound by the group's row count.
- **Observed:** `AVG(D: keep) <> 2` over a join where D1 has two rows accepts
  `keep = 2, 2` (the bound is scaled by the joined rows, not the tuples).
  `AVG(x) + SUM(y) <> 3` raises an INTERNAL Error (a fractional coefficient reaches the
  integrality guard).
- **Expected:** the average over tuples excluded; a refusal in SQL terms for the mixed
  shape (the scope spellings refuse both: *"AVG in a <> constraint is not supported yet
  ..."*).
- **Impact:** a wrong answer, or an internal error.

### `norm(D: e, p)` becomes `SUM(D: e)`, for every order

- **Location:** `BindQualifiedReducer` assigns the qualifier tag over the alias that
  carries the norm marker (`decide_binder.cpp`).
- **Observed:** `norm(D: open, 'inf') <= 1` with `D.open(INT)` is modelled as
  `open_D1 + open_D2 <= 1`; orders 1 (the ABS dropped), 2 and 0 become the same SUM row.
  The scope spellings refuse it (*"DECIDE: norm(D: ...) is not supported yet; ..."*);
  `max(per D: abs(open)) <= 1` is the working spelling of the `'inf'` case.
- **Expected:** `MAX(D: ABS(open)) <= 1`, or a refusal.
- **Impact:** a wrong optimum, silently.

### The old WHEN reads the rows it drops

- **Location:** coefficient extraction in `physical_decide.cpp` computes every
  expression on every row before the WHEN mask zeroes the dropped ones.
- **Observed:** `SUM(x * t) <= 9 WHEN (t IS NOT NULL)` and
  `MAXIMIZE SUM(x * t) WHEN (t IS NOT NULL)` raise *"column "t" is NULL"*;
  `x <= CAST(s AS INTEGER) WHEN (s <> 'abc')` raises *"Conversion Error: Could not
  convert string 'abc' to INT32"*.
- **Expected:** nothing computed on the dropped rows, as under `when c:`
  (`ExecuteOnReadRows`).
- **Impact:** the documented way to skip NULLs, or rows an expression cannot take,
  fails with the old spelling.

### A negative factor on a hard MIN/MAX reverses the clause

- **Location:** the hard branch of `RewriteMinMaxInConstraint`
  (`decide_rewrite_minmax.cpp`), and the hard half of its equality branch, keep the
  factor outside the extremum, and `LinearizeMinMaxConstraints`
  (`linearization_minmax.cpp`) takes the members' coefficients, factor included, under
  the written MIN or MAX.
- **Observed:** over `range(3)`, `x <= 10`, `MINIMIZE SUM(x)`: `-2 * MAX(x) <= -10`
  returns `[0, 0, 0]` (the clause became "some `-2x` reaches -10 from above", which zeros
  satisfy), and so does `-2 * MAX(x) = -10`; the mirror `-2 * MIN(x) >= -10` returns
  `[0, 0, 5]` where `[0, 0, 0]` holds. The scope spellings take `-2 * max(x)` as
  `min(-2 * x)` (`[0, 0, 5]`, and `[0, 0, 0]` for the mirror).
- **Expected:** some `x` at least 5.
- **Impact:** a wrong optimum, silently.

### A strict bound on a hard MIN/MAX or a composed clause is solved as non-strict

- **Location:** the outer row of `LinearizeMinMaxConstraints` and the composed outer row
  of `LinearizeComposedMinMaxConstraint` (`linearization_minmax.cpp`) keep the written
  bound with `>`/`<`, where `ApplyComparisonSense` would step it.
- **Observed:** over `range(3)` with `x <= 10`, `MINIMIZE SUM(x)`: `MAX(x) > 3` returns
  `[0, 0, 3]`; `SUM(x) + MAX(x) > 30` returns `[10, 10, 0]` (30, not above 30). The scope
  spellings step both (`[0, 0, 4]`, 21).
- **Expected:** the clause holds strictly.
- **Impact:** an answer that violates the clause, silently.

### A query-wide decision squared in the objective is counted once per row

- **Location:** the objective's squared terms in `ilp_model_builder.cpp`, the objective
  sibling of the quadratic-constraint entry above.
- **Observed:** over three rows, `MINIMIZE SUM(x) + s * s - 10 * s` with `scalar s(INT)`,
  `s <= 5` dumps `q 3,3:3` and returns `s = 2`.
- **Expected:** `s²` counted once and `s = 5`. The scope spellings refuse a product of
  decisions outside the objective's reducers (*"... multiplies decisions outside a
  reducer, which is not supported yet"*).
- **Impact:** a wrong optimum, silently.

### A MIN/MAX objective leaves out a member whose value is 0

- **Location:** the active-row filter of the MIN/MAX objective in
  `linearization_minmax.cpp` (a row whose coefficients are all zero is skipped).
- **Observed:** over `(1,'a',0),(2,'a',3),(3,'b',1) t(id,g,c)`, `x <= 3 AND SUM(x) <= 3
  MAXIMIZE SUM(MIN(c * x)) PER g` returns `[0, 3, 0]`, whose objective is
  `min(0, 9) + 0 = 0`: row 1 is no member of group a. The scope spelling
  `sum(per g: min(c * x) by (g))` counts it (`MinMaxObjectiveSpec::member_rows`) and
  reaches 3 with `[0, 0, 3]`.
- **Expected:** every row of the group a member.
- **Impact:** a wrong optimum, silently.

### A product of query-wide decisions beside a reducer raises an internal error

- **Location:** term extraction in `decide_linear_form.cpp`, after `RewriteBilinear`
  has replaced the product with an auxiliary.
- **Observed:** over the deck's join, `scalar b(BOOL), scalar s(INT)` (or `per ()`) and
  `SUM(ship) + b * s <= 50` raise *"INTERNAL Error: DECIDE constraint reached term
  extraction with invalid aggregate/per-row homogeneity: '((sum(ship) +
  CAST(__bilinear_aux_3__ AS HUGEINT)) <= ...'"*. The scope spellings refuse it
  (*"DECIDE: 'b * s' beside a reducer, in a constraint that multiplies decisions, is not
  supported yet"*).
- **Expected:** the refusal in SQL terms.
- **Impact:** an internal error, naming an internal column.

## Defects shared by both spellings, found while building items 3–4

Neither is specific to the scoping language; both spellings reach them. Each was run on
this branch and its parent with the same result.

- **A lambda in a coefficient:** `SUM(x * list_reduce([cap, b], (a, z) -> a + z)) <= 30`
  raises *"INTERNAL Error: lambda reference expressions are transient, ToString should
  never be called"* (also `when c: sum(...)`, a per-row bound, the objective);
  `SUM(x * len(list_filter([cap, b], q -> q > 1))) <= 12` answers *"Column 'q' must be one
  of the DECIDE variables"*.
- **DIAGNOSE names a NULL slice `g = 'NULL'`:** for `x <= cap WHEN (g IS NOT NULL)` (or
  `when g is not null: x <= cap`) over data where a NULL-`g` row leaves `x` unbounded,
  the suggested change reads `x <= <cap> WHEN g = 'NULL'`, which leaves the query
  unbounded; `g IS NULL` repairs it.
- **Internal names in messages:** `x NOT BETWEEN 1 AND 3` answers *"...
  (ExpressionType::OPERATOR_NOT)"*, `MAXIMIZE x` *"... (ExpressionClass::COLUMN_REF)"*,
  `MAXIMIZE 3` *"... (ExpressionClass::CONSTANT)"*; `MINIMIZE SUM(MAX(D: y)) PER region`
  answers *"Unsupported operator '__qualified_reducer__' inside DECIDE SUM expression"*;
  a NULL correlated subquery in an old-spelling objective,
  `MAXIMIZE SUM(x * (SELECT ...))`, names *"column "SUBQUERY__row_varying_subquery__""*.
- **A PER key NULL on every row:** `SUM(x) <= 7 PER nn` answers *"DECIDE empty row set
  ... check your WHEN clause"* though there is no WHEN; `MINIMIZE SUM(MAX(x)) PER nn`
  drops the PER and the MAX and minimizes `SUM(x)` over every row.
- **DIAGNOSE of a hard MIN or MAX over a keyed decision:** `DECIDE per t.g: x(INT) SUCH
  THAT x >= 15 AND x <= 20 AND MIN(x) <= 12` (or `per (): min(x) <= 12`,
  `MAX(x) >= 25`) is infeasible, and DIAGNOSE answers *"no loosening of the clauses you
  wrote restores feasibility"* (`undiagnosed`); the same query over a per-row `x(INT)`
  suggests `min(x) <= 15`. Found 2026-10-17 while building item 5; the same on
  `1c12622bbb`, so not of items 3–5. Expected: the loosening the per-row form suggests.
- **Expected:** a refusal or an answer in SQL terms; `g IS NULL` for a NULL slice.
- **Impact:** internal errors, misleading messages, and one wrong optimum (`PER nn`).

Resolved behavior is documented by its owning `done.md`.
