# Bugs — Fixed

Log of bugs that were discovered and resolved. Kept for history; active bugs live in `todo.md`.

---

## Table-Scoped DECIDE Variables Cannot Be Projected as `Table.var` In The SELECT List (and per-row LHS)

### Symptom

Queries declaring a table-scoped DECIDE variable (e.g. `DECIDE supplier.pick IS BOOLEAN`) failed at bind time when the variable was referenced through its table qualifier outside the DECIDE-internal binders:

```
Binder Error: Table "supplier" does not have a column named "pick"
```

Two distinct call sites hit this:

1. `Table.var` in the SELECT list — `SELECT supplier.pick FROM supplier DECIDE supplier.pick IS BOOLEAN ...`
2. `Table.var` as the LHS of a *per-row* SUCH THAT constraint — `SUCH THAT supplier.pick <= 1 ...`

The DECIDE / SUCH THAT (aggregate) / MAXIMIZE / MINIMIZE clauses themselves accepted the qualified form fine, so the failure shape was confusing to users who had qualified the variable consistently.

### Root cause

Two binders were seeing two different name spaces.

`bind_select_node.cpp` registers each table-scoped variable under both its unqualified name (`pick`) and its qualified name (`supplier.pick`) in a `decide_variable_names` map. That map is consulted by `DecideConstraintsBinder` and `DecideObjectiveBinder`, so SUCH THAT / objective references resolve correctly. But two other paths bypassed the map:

- The SELECT list is bound by the *regular* DuckDB binder. It only sees decision variables through the generic bind-context binding `decide_variables` (added at `bind_select_node.cpp:814`), which exposes only unqualified `var_names`. When given `supplier.pick`, it routed `supplier` to the real `TableBinding` for the supplier table, which has no `pick` column.
- The per-row branch of `DecideConstraintsBinder` (`decide_constraints_binder.cpp:182`) falls through to `ExpressionBinder::BindExpression` for column refs — same regular DuckDB binder, same failure mode. The aggregate-SUM branch only worked because `NormalizeDecideConstraints` round-trips the LHS through SymEngine (`decide_symbolic.cpp:257` / `:778`), which silently drops table qualifiers as a side effect.

### Fix

Added a single parsed-AST pre-pass `RewriteScopedVarRefs` in `bind_select_node.cpp` that walks an expression tree and rewrites any qualified `ColumnRefExpression` whose `Table.col` form matches a registered scoped DECIDE variable into a bare `ColumnRefExpression(col)`. The rewrite is applied to:

- `statement.decide_constraints`
- `statement.decide_objective`
- each element of `statement.select_list`

It runs immediately after `decide_variable_names` is fully built, before `RewriteInDomain`, normalization, and binding. After this pass, every reference to a scoped decision variable is unqualified, so the regular DuckDB binder (SELECT) and the per-row branch of `DecideConstraintsBinder` both resolve them through the existing generic `decide_variables` binding. The aggregate-SUM path is unaffected — it already arrived at the bare form via SymEngine; the rewrite just gets there earlier and uniformly.

The pass is a no-op when no scoped variables are declared, and bare `ColumnRefExpression`s are skipped, so unrelated qualified refs (e.g. `supplier.s_acctbal` for a real column) are untouched.

### Verification

- `make decide-test` — 547 passed, 0 failed.
- Stress-query repros restored to working: P13, P14 (`stress_queries/04_problem_classes.sql`); V8–V13 (`stress_queries/06_variables.sql`); per-row qualified LHS (V12), alias-qualified scope (V9), bilinear over two scoped tables (V13).

### Code pointers

- Helper: `src/planner/binder/query_node/bind_select_node.cpp` (`RewriteScopedVarRefs`)
- Wire-in site: same file, immediately after `decide_variable_names` is built and before `RewriteInDomain`
- Background — qualified-name registration: same file (`decide_variable_names.emplace(qualified_name, var_idx)`)
- Background — generic SELECT binding alias: same file (`bind_context.AddGenericBinding(result->decide_index, "decide_variables", var_names, var_types)`)
- Background — per-row fall-through: `src/planner/expression_binder/decide_constraints_binder.cpp` (`return ExpressionBinder::BindExpression(expr_ptr, depth)`)
- Background — accidental qualifier strip on aggregate path: `src/packdb/symbolic/decide_symbolic.cpp` (`ToSymbolicRecursive` reads only `colref.GetColumnName()`; `FromSymbolic` rebuilds unqualified)

---

## Gurobi Time-Limit Termination Threw "No Solution Found" Despite Feasible Incumbent

### Symptom

Hard MIQPs that exhausted the 300s solver time limit (e.g. `stress_queries/04_problem_classes.sql` P7-large) raised:

```
Invalid Input Error: DECIDE optimization failed with Gurobi status 9.
The optimization could not find a solution.
```

…even when Gurobi's own log reported `Solution count 4: -90 -62 -60 180` and a finite best objective. PackDB simply never surfaced the incumbent.

### Root cause

Two compounding bugs in `src/packdb/gurobi/gurobi_loader.hpp`:

1. **Wrong status constants.** PackDB defined `GRB_TIME_LIMIT = 7` and `GRB_ITERATION_LIMIT = 8`, but Gurobi's actual values are `ITERATION_LIMIT = 7`, `NODE_LIMIT = 8`, `TIME_LIMIT = 9`. So when Gurobi returned `status == 9`, the time-limit branch in `gurobi_solver.cpp` never matched, the SolCount-check was skipped, and the fallthrough else-branch threw "no solution found with status %d" — the `9` in the user-visible error was the give-away.
2. **Narrow accept-incumbent branch.** Even with the correct constants, only `GRB_TIME_LIMIT` was being treated as "may carry a feasible solution"; `NODE_LIMIT`, `SOLUTION_LIMIT`, `INTERRUPTED`, `SUBOPTIMAL` (all of which can also leave an incumbent in the pool) all went to the throw path.

### Fix

- `gurobi_loader.hpp`: corrected status codes and added the missing ones (`GRB_CUTOFF=6`, `GRB_NODE_LIMIT=8`, `GRB_TIME_LIMIT=9`, `GRB_SOLUTION_LIMIT=10`, `GRB_INTERRUPTED=11`, `GRB_NUMERIC=12`, `GRB_SUBOPTIMAL=13`, `GRB_INPROGRESS=14`, `GRB_USER_OBJ_LIMIT=15`).
- `gurobi_solver.cpp`: widened the SolCount-rescue branch to all terminations that may carry an incumbent (time/iter/node/solution limit, interrupt, suboptimal). When `SolCount > 0`, status is rewritten to `GRB_OPTIMAL` so the existing `getdblattrarray("X")` extraction path runs.

### Verification

- P7-large on `small.db` (170 vars, p_size<5, qty<=10, SUM=30, MIN sum-of-squares): now returns the best feasible solution (objective 590, sum=30) after 300s instead of throwing. Connection survives for follow-up queries.
- `make decide-test` — 547 passed, 0 failed.

### Code pointers

- Status constants: `src/packdb/gurobi/gurobi_loader.hpp` (status code block)
- Time-limit acceptance branch: `src/packdb/gurobi/gurobi_solver.cpp` (after the `getintattr(STATUS)` call)

---

## DECIDE Errors Cascaded "database has been invalidated" Across The Session

### Symptom

A single bad DECIDE query — either a malformed shape that Gurobi rejected at submission time, or a non-aggregate term that slipped past the bind check into execution — raised an `INTERNAL Error`. DuckDB's connection-invalidation policy treats `InternalException` as fatal, so every subsequent query in the same session failed with:

```
FATAL Error: Failed: database has been invalidated because of a previous fatal error.
The database must be restarted prior to being used again.
```

This produced cascading FATAL errors after `stress_queries/01_constraints.sql` C14 and `stress_queries/05_rejected.sql` R18 — those single bad queries silently truncated the rest of the file.

### Reproductions

C14 (correlated subquery scaled by decision variable on both sides):

```sql
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT s_acctbal * pick >= (
    SELECT MIN(s2.s_acctbal) FROM supplier s2 WHERE s2.s_nationkey = supplier.s_nationkey
  ) * pick
  AND SUM(pick) >= 5
MAXIMIZE SUM(pick);
```

R18 (`POWER` wrapping an aggregate, bypassing the bind-time check):

```sql
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty <= 10
MAXIMIZE POWER(AVG(qty), 2);
```

### Root cause

Two independent root causes, both surfacing through the same connection-poisoning policy:

1. **R18 specifically**: `DecideObjectiveBinder::GetExpressionType` accepted any function as long as `ContainsDecideAggregate` was true anywhere in its subtree. So `POWER(AVG(...), 2)` passed bind because it contained `AVG`, was rewritten by the optimizer to `POWER(SUM(...), 2)`, and only blew up at execution in `physical_decide.cpp` as `InternalException("DECIDE objective contains a non-aggregate term: ...")`. The supported quadratic shape is `SUM(POWER(_, 2))` (aggregate outermost), not `POWER(AGG(_), _)`.
2. **Both bugs**: The throw sites in `gurobi_solver.cpp` (`addconstr`/`addqconstr`/`addqpterms`/`optimize` failures) and in `physical_decide.cpp` (the non-aggregate-term rejections) used `InternalException`. DuckDB's connection layer marks the database as invalidated on any `InternalException` raised from execution, killing the session.

### Fix

1. **Tighten the bind-time check.** `decide_objective_binder.cpp:GetExpressionType` now distinguishes the additive composition path (`+`/`-`/`*`) — which legitimately mixes scalar arithmetic with aggregates — from non-additive wrappers (`POWER`, `SQRT`, `LOG`, etc.) that wrap an aggregate. The latter is rejected at bind time with a message pointing the user at `SUM(POWER(expr, 2))`.
2. **Convert the relevant throw sites to `InvalidInputException`.** All of `gurobi_solver.cpp`'s submit-time failures (`addconstr`, `addqconstr`, `addqpterms`, `optimize`) and `physical_decide.cpp`'s non-aggregate-term / non-SUM rejections now raise `InvalidInputException`, so a malformed query rejects only itself instead of poisoning the connection. `InternalException` is preserved for genuine PackDB invariant violations (Gurobi env/license setup, NaN/Inf in extracted solution).

### Verification

- R18 repro now fails with: `Binder Error: [MAXIMIZE|MINIMIZE] does not support wrapping an aggregate in 'power'. ...`. The next query in the session runs normally.
- C14 repro now fails with: `Invalid Input Error: Failed to add constraint to Gurobi: Problem adding constraints`. The next query in the session runs normally.
- `make decide-test` — 547 passed, 0 failed.

### Code pointers

- Bind-time tightening: `src/planner/expression_binder/decide_objective_binder.cpp` (`GetExpressionType` else-branch)
- Gurobi submit-time throw types: `src/packdb/gurobi/gurobi_solver.cpp` (`addconstr`/`addqconstr`/`addqpterms`/`optimize` error branches)
- Physical-execution non-aggregate-term rejections: `src/execution/operator/decide/physical_decide.cpp` (`ExtractAggregateConstraintTerms` and `ExtractAggregateObjectiveTerms` non-aggregate / non-SUM branches)

### Notes

- C14 still doesn't *succeed*; the optimizer/binder doesn't yet support a per-row constraint that puts the same decision variable on both sides of a comparison alongside a correlated scalar subquery. That's a separate expressivity gap. The fix here only ensures it fails gracefully instead of taking out the session. Earlier rejection at bind/optimizer time (so Gurobi never sees the bad shape) is still a worthwhile follow-up.

---

## PER-Grouped Aggregate With Every Group Empty Was Rejected Instead Of Skipped

### Symptom

A PER-grouped aggregate constraint where the WHEN clause filtered out every row of every group raised:

```
Invalid Input Error: DECIDE empty row set for aggregate in constraint.
An empty aggregate has no well-defined value; check your WHEN clause.
```

…even though the documented spec (`CLAUDE.md`: "Empty groups (WHEN filters out all rows in a group) are skipped") and the stress-test comments (M9 in `02_modifiers.sql`, N5 in `09_null_edge.sql`) say empty PER groups are skipped silently.

### Reproduction (M9 / N5)

```sql
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 2 WHEN s_acctbal > 9999999 PER s_nationkey
MAXIMIZE SUM(pick);
```

(No supplier has acctbal > 9.99M, so every PER group ends up empty.)

### Root cause

`physical_decide.cpp` had an explicit guard: if a PER aggregate ended up with `num_groups == 0` (i.e. every group was empty after WHEN filtering), it called `RejectEmptyAggregate`. That guard contradicted the documented "skip silently" semantics — a single empty group was already skipped downstream, but *all* groups empty hit the global reject path.

The non-PER empty case (N6: `SUM(pick) <= 5 WHEN false_for_all_rows`) is different and remains rejected — without PER, "empty WHEN" almost always means a user mistake, not a vacuous constraint.

### Fix

Removed the all-groups-empty rejection from the PER constraint branch in `physical_decide.cpp` (around line 2382). When `num_groups == 0`, the downstream emission loop simply emits no constraints, which is mathematically equivalent to writing no constraint at all — the documented spec.

### Verification

- M9 now returns a feasible solution (constraint vacuously satisfied) instead of throwing.
- N5 now returns a feasible solution.
- N6 (non-PER empty WHEN) still rejects with the same error, as documented.
- `make decide-test` — 547 passed, 0 failed.

### Code pointer

- `src/execution/operator/decide/physical_decide.cpp` (PER constraint emission, removed `RejectEmptyAggregate(eval_const.num_groups, ...)` call)

---

## ABS Hard-Direction Constraints Were Silently Unsound (Soundness Bug)

> **Superseded — see "ABS Hard-Direction Constraints — Proper Big-M Fix" further down.** This entry documents the original soundness bug and the conservative bind-time-rejection stopgap that was applied first. The proper Big-M sign-indicator implementation has since landed; the rejection-flavored statements below ("rejected at bind time", "C22/C23 rewritten", "R26/R27/R28 added to lock in the rejection") describe the stopgap state, not current behavior. Hard-direction ABS shapes now solve correctly.

**Severity: critical** — solver returned solutions that violated the constraint, with no error.

### Symptom

Any constraint that lower-bounded an `ABS(...)` expression over a decision variable accepted solutions where `|inner|` did not actually satisfy the bound. Examples (all were silently broken):

```sql
-- 1. Per-row ABS >= K
SUCH THAT qty <= 6 AND ABS(qty - 5) >= 4
-- Forces qty <= 1 (only qty=0 or 1 give |qty-5| >= 4); solver returned qty=6.

-- 2. Per-row ABS = K
SUCH THAT qty <= 6 AND qty >= 4 AND ABS(qty - 5) = 3
-- Infeasible (qty in [4,6] gives |qty-5| in [0,1]); solver returned qty=6 as feasible.

-- 3. MIN(ABS(...)) >= K (rewrites to per-row ABS >= K)
SUCH THAT qty <= 4 AND MIN(ABS(qty - 4)) >= 1
-- Requires every row qty in 0..3; solver returned qty=4 with |qty-4|=0.

-- 4. SUM(ABS(...)) >= K, MAX(ABS(...)) >= K, AVG(ABS(...)) <> K, etc.
```

### Root cause

`RewriteAbs` in `src/optimizer/decide/decide_optimizer.cpp` replaces `ABS(e)` with an auxiliary `aux` and emits only the lower envelope — `aux >= e` and `aux >= -e` — which forces `aux >= |e|` but leaves `aux` free to grow above `|e|`. Soundness depends entirely on the *constraint* upper-bounding `aux`:

- **Sound** when the constraint context upper-bounds `aux`: `ABS(...) <= K`, `ABS(...) < K`, `SUM(ABS) <= K`, `MAX(ABS) <= K`, `MIN(ABS) <= K`, etc. The solver naturally picks `aux = |e|` to satisfy the upper bound.
- **Unsound** when the constraint lower-bounds `aux`: `>=`, `>`, `=`, `<>`. The solver can pick any `aux >= max(|e|, K)`, satisfying the constraint without forcing `|e|` to actually meet the bound.

The MAXIMIZE-objective ABS path *does* allocate the missing upper-envelope (`aux <= e + 2M(1-y)`, `aux <= -e + 2My`) with a sign-indicator binary `y`, so `MAXIMIZE SUM(ABS(...))` was correctly bounded. The constraint path didn't allocate this Big-M machinery, leaving the unsoundness.

The bug had been in the codebase since ABS support was first added; existing stress queries C22/C23 happened to *coincidentally* return correct-looking answers (the maximizer wanted high `qty`, which happened to satisfy the ABS bound), so no test caught it. Audit-time, tighter tests showed the bug clearly.

### Fix

Conservative bind-time rejection. A new `ValidateAbsConstraintDirection` pass runs before `RewriteAbs` and throws `InvalidInputException` when ABS over a decision variable appears in a constraint context that doesn't upper-bound the auxiliary. The check covers:

- Per-row comparisons: ABS on LHS of `<=`/`<` or RHS of `>=`/`>` is sound; everything else (including `=`, `<>`, BETWEEN, IN) is rejected.
- Aggregates of ABS (`SUM`, `AVG`, `MIN`, `MAX`): same rule applied to the comparison wrapping the aggregate.
- WHEN/PER wrappers and AND-conjunctions: traversed transparently.

ABS in the **objective** is unchanged — the existing MINIMIZE (lower envelope only, sound by descent) and MAXIMIZE (full Big-M with sign indicator, sound) paths handle it correctly.

A correct hard-direction *constraint* implementation would mirror the existing MAXIMIZE-objective Big-M (binary indicator + bound-aware constraints emitted at execution time once variable bounds are known). That is a substantive feature and was not done in this fix — bind-time rejection is preferable to a partial implementation that risks new soundness or numerical issues.

### Verification

- Per-row `ABS >= K`, `ABS = K`, `ABS <> K`, `ABS > K`: rejected with a clear message.
- `MIN(ABS) >= K`, `MAX(ABS) >= K`, `SUM(ABS) >= K`, `AVG(ABS) >= K` and the corresponding `=`/`<>`/`>` forms: all rejected.
- `ABS <= K`, `ABS < K`, `SUM(ABS) <= K`, `MAX(ABS) <= K`, `MINIMIZE SUM(ABS)`, `MAXIMIZE SUM(ABS)`, `MAX(ABS) <= K WHEN ...`, etc.: all still work.
- `make decide-test`: 547 passed, 0 failed.
- `stress_queries/01_constraints.sql` C22/C23 rewritten to use the sound easy direction (`MAX(ABS) <= K` / `SUM(ABS) <= K`); R26/R27/R28 added to `05_rejected.sql` to lock in the rejection.

### Code pointers

- New validator: `src/optimizer/decide/decide_optimizer.cpp` (`ValidateAbsConstraintDirection`, `RejectAbsHardDirection`, `ContainsAbsOverDecideVar`).
- Wire-in site: `OptimizeDecide`, immediately before `RewriteAbs`.
- Existing sound MAXIMIZE-objective Big-M (kept as the reference for any future hard-direction constraint implementation): same file, `RewriteAbs` MAXIMIZE branch.

### Notes — coverage gaps surfaced by this audit

The audit that found this bug also identified several feature-composition gaps in the stress queries that have now been filled (C30/C31/C32 in `01_constraints.sql`, M19 in `02_modifiers.sql`, O37–O41 in `03_objectives.sql`, P15 in `04_problem_classes.sql`, OP17 in `07_operators.sql`). These are not bugs — just coverage additions for combinations the docs claim are supported but no test exercised.

---

## C6/C9/C14: Linear-After-Distribution Shapes Were Misclassified As Nonlinear

### Symptom

Three stress queries in `01_constraints.sql` failed even though the underlying expressions are linear in decision variables after algebraic distribution:

- **C6**: `MIN(s_acctbal * pick + 1000 * (1 - pick)) <= 500` — linear in `pick`: `(s_acctbal - 1000) * pick + 1000`.
- **C9**: `MIN(s_acctbal * pick + 100000 * (1 - pick)) >= 1000` — same shape, easy direction.
- **C14**: `s_acctbal * pick >= (correlated_subq) * pick` — rearranges to `(s_acctbal - subq) * pick >= 0`.

Errors observed:
- C6/C9: `Invalid Input Error: DECIDE expression contains an unsupported product factor that still references decision variables after normalization (total degree > 2 or unexpanded nonlinear product).`
- C14: `Invalid Input Error: Failed to add constraint to Gurobi: Problem adding constraints.`

### Root cause

Two interacting gaps in the constraint extraction pipeline:

1. **No multiply-over-add distribution at the per-row constraint extractor.** The per-row constraint path (and the inner-of-aggregate path that feeds it) calls `ClassifyNormalizedProduct` on each `*` chain. The classifier expected each factor to be either a bare data expression or a bare decide-var reference; an additive sub-expression like `(1 - pick)` made it throw "unexpanded nonlinear product." The symbolic normalizer that handles SymEngine-based aggregate normalization didn't run on per-row constraints, so the additive factor reached the classifier intact.
2. **No coefficient deduplication when the same decision variable appeared in multiple LHS terms of a single per-row constraint.** This affected C14 directly (`s_acctbal * pick` and `subq * pick` after move-to-LHS) and any post-distribution shape where the additive expansion produced multiple `*pick` terms (`s_acctbal*pick + (-1000*pick) + 1000`). The per-row emission in `ilp_model_builder.cpp` pushed each term as its own `(column_index, coefficient)` pair into the Gurobi constraint, producing duplicate column indices that `GRBaddconstr` rejected.

### Fix

Three coordinated changes:

1. **`TryDistributeMultiplyOverAdd`** (`src/execution/operator/decide/physical_decide.cpp`): a new helper that, given a `*` chain with at least one `+`/`-`/unary-`-` factor, returns a vector of `(sign, product)` pairs where each product replaces the additive factor with one of its addends. Algebraically equivalent: `K * (a - b * x)` becomes `[(+1, K * a), (-1, K * b * x)]`.
2. **Apply distribution before the classifier** at every `*` branch that previously fell into `ClassifyNormalizedProduct`: `ExtractConstraintTerms`, `ExtractLinearAndBilinearTerms` (objective), and the linear `ExtractTerms`. Distribution runs *before* classification (the classifier throws rather than returning false on additive factors). When distribution applies, the caller recurses into each distributed product with its sign; otherwise the existing logic runs unchanged.
3. **Per-row coefficient aggregation** (`src/packdb/utility/ilp_model_builder.cpp`): the per-row constraint emission now sums coefficients into an `unordered_map<int, double>` keyed on Gurobi column index before pushing entries into `ModelConstraint`. Constants (LHS terms with `var_idx == INVALID_INDEX`) continue to be folded into the RHS adjustment as before.
4. **Big-M constant skip** (same file's hard MIN/MAX finalize, around line 3128): the Big-M auto-tuner now skips `INVALID_INDEX` entries when scanning `variable_indices` for upper-bound lookup. Without this, a constant LHS term in a hard MIN/MAX inner expression caused an out-of-bounds vector access during finalization.

### Verification

- C6, C9, C14 all return solutions on `small.db`. Sums and constraints check out by hand on a few rows.
- Existing easy-direction MIN/MAX (C4, C5, C22, C23, M10), per-row ABS (C21, R26–R28), bilinear (C11, C24, C32), nested-aggregate (M11, M12, M13, O37–O41), table-scoped variables (V8–V13, P13–P15), and feasibility (P11, P12) all continue to work.
- `make decide-test`: 547 passed, 0 failed.
- All 9 stress files: only the previously-documented expected rejections remain (R-series, N6, N7-N12, R17 silent-accept).

### Code pointers

- Distribution helper: `src/execution/operator/decide/physical_decide.cpp` (`TryDistributeMultiplyOverAdd`).
- Wire-in sites (all in same file): `ExtractTerms` `*` branch; `ExtractLinearAndBilinearTerms` `*` branch; `ExtractConstraintTerms` `*` branch.
- Per-row coefficient aggregation: `src/packdb/utility/ilp_model_builder.cpp` (per-row constraint loop, ~line 642).
- Big-M constant skip: `src/execution/operator/decide/physical_decide.cpp` (hard MIN/MAX finalize Big-M scan, ~line 3128).


---

## ABS Hard-Direction Constraints — Proper Big-M Fix (Supersedes Earlier Stopgap)

### Symptom

Earlier: PackDB's ABS rewrite was a pure lower-envelope (`aux >= e`, `aux >= -e`), forcing only `aux >= |e|`. For constraint shapes that did not upper-bound `aux` — `ABS(...) >= K`, `ABS(...) > K`, `ABS(...) = K`, `ABS(...) <> K`, `ABS(...) BETWEEN`, ABS on both sides of a comparison, and the analogous aggregate forms (`SUM(ABS) >= K`, `MIN(ABS) >= K`, `MAX(ABS) >= K`) — the solver could satisfy the constraint by inflating `aux` above `|e|`, silently producing infeasible-relative-to-the-original-predicate solutions reported as feasible.

The earlier fix (a bind-time soundness gate, `ValidateAbsConstraintDirection`) rejected these shapes at bind time. Correct, but conservative: it reduced the supported surface and forced users to manually reformulate.

### Proper fix

Replace the rejecting validator with a tagging classifier (`TagAbsConstraintsForBigM`) that runs in the same place but, instead of throwing, marks each ABS occurrence in a hard-direction position with `ABS_NEEDS_BIGM_TAG` (set on the `BoundFunctionExpression.alias` of the ABS node).

`RewriteAbs` Phase 1 (`FindAndReplaceAbs`) reads the tag and propagates `needs_bigm` to the per-ABS `AbsPairInfo`. Phase 2 always emits the lower envelope; for any pair where `needs_bigm || (in_objective && MAXIMIZE)`, it additionally allocates a binary sign indicator `y` and tags the lower-bound constraints `ABS_UB_POS_TAG_PREFIX{y_idx}` / `ABS_UB_NEG_TAG_PREFIX{y_idx}` so the existing physical-execution Big-M emitter (originally written for `MAXIMIZE SUM(ABS)`) emits the upper-envelope pair `aux <= e + 2M(1-y)` and `aux <= -e + 2M*y`. Combined with the lower envelope these force `aux = |e|` exactly.

The Big-M emission lives in `physical_decide.cpp` and iterates `LogicalDecide::abs_maximize_links` (vector name preserved for historical reasons; entries are now produced for both objective MAXIMIZE and constraint hard-direction users). M is computed at execution time from the bounds of variables in `expr` — finite bounds are required, with a generic error message naming the unbounded variable.

WHEN/PER on the original constraint are unaffected — the per-row Big-M envelope is unconditional, and the WHEN/PER filter operates on the outer aggregate or constraint that consumes the now-pinned `aux`. ABS on both sides of a comparison is also handled correctly: both auxes are tagged, both pinned, and the comparison reduces to `|e1| op |e2|`.

### Verification

- C33–C37 in `01_constraints.sql` cover per-row hard-direction (`ABS >= K`), equality (`ABS = K`), aggregate hard via easy-MIN strip (`MIN(ABS) >= K`), aggregate hard direction (`SUM(ABS) >= K`), and BETWEEN. All produce oracle-verified correct sums on `small.db`.
- R26/R27/R28 (the previous rejection-pinning stress queries) are removed from `05_rejected.sql`; that file is now smaller by 3 entries (24 errors vs. 27 before).
- Existing `MAXIMIZE SUM(ABS(...))` objective path (test `test_abs_linearization.py`) continues to work — the same code path handles both Big-M users.
- Sound shapes (`ABS <= K`, `SUM(ABS) <= K`, etc.) skip the upper envelope as before — no extra variables, no regressions.
- `make decide-test`: 547 passed, 0 failed.

### Code pointers

- Classifier: `src/optimizer/decide/decide_optimizer.cpp` — `TagAbsConstraintsForBigM`, `ClassifyAbsConstraints`, `TagAbsForBigM`.
- Wire-in: `OptimizeDecide`, immediately before `RewriteAbs`.
- Phase 1 tag-read: `FindAndReplaceAbs` reads `func.alias == ABS_NEEDS_BIGM_TAG`.
- Phase 2 y-allocation: `RewriteAbs` — `needs_bigm = pair.needs_bigm || (in_objective && MAXIMIZE)`.
- Big-M emission (unchanged code path, broadened context): `src/execution/operator/decide/physical_decide.cpp` (search for `abs_maximize_links`).
- Tag constant: `src/include/duckdb/common/enums/decide.hpp` (`ABS_NEEDS_BIGM_TAG`).
- Doc: `03_expressivity/sql_functions/done.md` (Path A / Path B classification).
