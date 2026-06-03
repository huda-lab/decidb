# Bugs — Fixed

Log of bugs that were discovered and resolved. Kept for history; active bugs live in `todo.md`.

---

## `func_application WHEN` in global `c_expr` corrupted parsing of ordinary function calls

### Symptom

Two failure modes, one cause, both `Parser Error: syntax error at or near "then"`:

1. Catalog introspection (`duckdb_tables()`, `information_schema.*`, `pg_catalog.*`, `SHOW TABLES`, `DESCRIBE`, `sqlite_master`) failed at bind time.
2. Any call to a function name **not registered** in the catalog failed to parse (`SELECT foo()`, internal helpers like `format_pg_type`, simple `CASE func() WHEN … THEN`).

User DECIDE queries were unaffected in practice (they only call registered aggregates `SUM`/`MIN`/`MAX`/`AVG`/`POWER`), which is why CI stayed green — a coverage gap.

### Root cause

The DECIDE aggregate-local WHEN feature put `func_application WHEN decide_when_condition` in the **global** `c_expr` non-terminal. Because `c_expr` is the shared atom of every SQL expression, this created a shift/reduce conflict just after any `func_application` (`WHEN`-shift vs the nullable `within_group_clause`-reduce — Bison state 1135), silenced by `%expect 4`. The "near then" text is a Bison artifact; the catalog-dependence is real function-name-aware parse behavior in DuckDB (proven by macro-ordering: `CREATE MACRO foo …` must precede `SELECT foo(...)` for it to parse).

### Ruled out during investigation

- **Precedence tweak** — bare `foo()` fails with no `WHEN` present, so there is no resolution to re-prioritize.
- **Attach WHEN to a completed `func_expr`** — conflict moved but `foo()` still failed; any `<function> WHEN …` reachable from global expression grammar corrupts the generic path.
- **Bare removal from `c_expr`** — fixes the bug but breaks 35 aggregate-local-WHEN tests (necessary, not sufficient).
- **Full grammar mirror reusing shared leaves** — 249 reduce/reduce conflicts from unit-production aliasing; a deep self-contained mirror would be ~800–1200 lines.

### Fix (context-sensitive `WHEN_DECIDE` token)

Make WHEN inside a DECIDE clause lex as a distinct token, so the DECIDE WHEN never reaches the global grammar:

- New `%token WHEN_DECIDE` (`grammar/grammar.y`); the DECIDE productions (`c_expr` aggregate-local atom, `decide_constraint_item`/`decide_objective_item`) use `WHEN_DECIDE` instead of `WHEN` (`grammar/statements/select.y`).
- A scanner-state flag `in_decide_clause` (+ `decide_case_depth`) in `base_yy_extra_type` (`include/parser/gramparse.hpp`). `base_yylex` (`src_backend_parser_parser.cpp`) sets the flag on the `DECIDE` token, and while set rewrites `WHEN`→`WHEN_DECIDE` — **except** inside a `CASE…END` (depth > 0), so CASE-in-DECIDE still parses and is rejected with the friendly binder error rather than a parser crash. The `decide_clause` grammar action clears the flag.

The state-1135 conflict still exists but is now keyed on `WHEN_DECIDE`, which the lexer never emits outside a DECIDE clause — so ordinary function calls hit `$default reduce` and parse normally. Conflict count stays 4 (`%expect 4` unchanged).

### Why this shape

It expresses the intent directly ("WHEN is special only inside DECIDE"), needs no grammar duplication, eliminates the collision by construction, leaves the feature's AST identical (all 58 WHEN tests pass, asymmetry preserved), and reuses the codebase's existing context-token mechanism (`NOT_LA`/`WITH_LA`/`NULLS_LA` in the same `base_yylex` filter).

### Known limitation

`in_decide_clause` is a single `bool`, not a depth counter, so **nested DECIDE clauses** (a DECIDE inside a subquery that is itself inside another DECIDE) are not handled — the inner `decide_clause` action would clear the flag prematurely. This shape is unsupported semantically today, so the single flag suffices; if nested DECIDE is ever added, promote the flag to a depth counter. See `context/descriptions/03_expressivity/when/done.md` ("How DECIDE `WHEN` is tokenized").

### Regression guard

`test/decide/tests/test_parser_catalog_introspection.py` — catalog views and unregistered/user-defined function calls must parse (the coverage gap that let this slip past CI).

### Code pointers

- `third_party/libpg_query/grammar/grammar.y` — `%token WHEN_DECIDE`, `%expect` comment.
- `third_party/libpg_query/grammar/statements/select.y` — `WHEN_DECIDE` in DECIDE productions; flag-clear in `decide_clause`.
- `third_party/libpg_query/include/parser/gramparse.hpp` — `in_decide_clause`, `decide_case_depth`.
- `third_party/libpg_query/src_backend_parser_parser.cpp` — `base_yylex` rewrite + `raw_parser`/`tokenize` init.

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
- Background — accidental qualifier strip on aggregate path: `src/decidb/symbolic/decide_symbolic.cpp` (`ToSymbolicRecursive` reads only `colref.GetColumnName()`; `FromSymbolic` rebuilds unqualified)

---

## Gurobi Time-Limit Termination Threw "No Solution Found" Despite Feasible Incumbent

### Symptom

Hard MIQPs that exhausted the 300s solver time limit (e.g. `stress_queries/04_problem_classes.sql` P7-large) raised:

```
Invalid Input Error: DECIDE optimization failed with Gurobi status 9.
The optimization could not find a solution.
```

…even when Gurobi's own log reported `Solution count 4: -90 -62 -60 180` and a finite best objective. DeciDB simply never surfaced the incumbent.

### Root cause

Two compounding bugs in `src/decidb/gurobi/gurobi_loader.hpp`:

1. **Wrong status constants.** DeciDB defined `GRB_TIME_LIMIT = 7` and `GRB_ITERATION_LIMIT = 8`, but Gurobi's actual values are `ITERATION_LIMIT = 7`, `NODE_LIMIT = 8`, `TIME_LIMIT = 9`. So when Gurobi returned `status == 9`, the time-limit branch in `gurobi_solver.cpp` never matched, the SolCount-check was skipped, and the fallthrough else-branch threw "no solution found with status %d" — the `9` in the user-visible error was the give-away.
2. **Narrow accept-incumbent branch.** Even with the correct constants, only `GRB_TIME_LIMIT` was being treated as "may carry a feasible solution"; `NODE_LIMIT`, `SOLUTION_LIMIT`, `INTERRUPTED`, `SUBOPTIMAL` (all of which can also leave an incumbent in the pool) all went to the throw path.

### Fix

- `gurobi_loader.hpp`: corrected status codes and added the missing ones (`GRB_CUTOFF=6`, `GRB_NODE_LIMIT=8`, `GRB_TIME_LIMIT=9`, `GRB_SOLUTION_LIMIT=10`, `GRB_INTERRUPTED=11`, `GRB_NUMERIC=12`, `GRB_SUBOPTIMAL=13`, `GRB_INPROGRESS=14`, `GRB_USER_OBJ_LIMIT=15`).
- `gurobi_solver.cpp`: widened the SolCount-rescue branch to all terminations that may carry an incumbent (time/iter/node/solution limit, interrupt, suboptimal). When `SolCount > 0`, status is rewritten to `GRB_OPTIMAL` so the existing `getdblattrarray("X")` extraction path runs.

### Verification

- P7-large on `small.db` (170 vars, p_size<5, qty<=10, SUM=30, MIN sum-of-squares): now returns the best feasible solution (objective 590, sum=30) after 300s instead of throwing. Connection survives for follow-up queries.
- `make decide-test` — 547 passed, 0 failed.

### Code pointers

- Status constants: `src/decidb/gurobi/gurobi_loader.hpp` (status code block)
- Time-limit acceptance branch: `src/decidb/gurobi/gurobi_solver.cpp` (after the `getintattr(STATUS)` call)

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
2. **Convert the relevant throw sites to `InvalidInputException`.** All of `gurobi_solver.cpp`'s submit-time failures (`addconstr`, `addqconstr`, `addqpterms`, `optimize`) and `physical_decide.cpp`'s non-aggregate-term / non-SUM rejections now raise `InvalidInputException`, so a malformed query rejects only itself instead of poisoning the connection. `InternalException` is preserved for genuine DeciDB invariant violations (Gurobi env/license setup, NaN/Inf in extracted solution).

### Verification

- R18 repro now fails with: `Binder Error: [MAXIMIZE|MINIMIZE] does not support wrapping an aggregate in 'power'. ...`. The next query in the session runs normally.
- C14 repro now fails with: `Invalid Input Error: Failed to add constraint to Gurobi: Problem adding constraints`. The next query in the session runs normally.
- `make decide-test` — 547 passed, 0 failed.

### Code pointers

- Bind-time tightening: `src/planner/expression_binder/decide_objective_binder.cpp` (`GetExpressionType` else-branch)
- Gurobi submit-time throw types: `src/decidb/gurobi/gurobi_solver.cpp` (`addconstr`/`addqconstr`/`addqpterms`/`optimize` error branches)
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
3. **Per-row coefficient aggregation** (`src/decidb/utility/ilp_model_builder.cpp`): the per-row constraint emission now sums coefficients into an `unordered_map<int, double>` keyed on Gurobi column index before pushing entries into `ModelConstraint`. Constants (LHS terms with `var_idx == INVALID_INDEX`) continue to be folded into the RHS adjustment as before.
4. **Big-M constant skip** (same file's hard MIN/MAX finalize, around line 3128): the Big-M auto-tuner now skips `INVALID_INDEX` entries when scanning `variable_indices` for upper-bound lookup. Without this, a constant LHS term in a hard MIN/MAX inner expression caused an out-of-bounds vector access during finalization.

### Verification

- C6, C9, C14 all return solutions on `small.db`. Sums and constraints check out by hand on a few rows.
- Existing easy-direction MIN/MAX (C4, C5, C22, C23, M10), per-row ABS (C21, R26–R28), bilinear (C11, C24, C32), nested-aggregate (M11, M12, M13, O37–O41), table-scoped variables (V8–V13, P13–P15), and feasibility (P11, P12) all continue to work.
- `make decide-test`: 547 passed, 0 failed.
- All 9 stress files: only the previously-documented expected rejections remain (R-series, N6, N7-N12, R17 silent-accept).

### Code pointers

- Distribution helper: `src/execution/operator/decide/physical_decide.cpp` (`TryDistributeMultiplyOverAdd`).
- Wire-in sites (all in same file): `ExtractTerms` `*` branch; `ExtractLinearAndBilinearTerms` `*` branch; `ExtractConstraintTerms` `*` branch.
- Per-row coefficient aggregation: `src/decidb/utility/ilp_model_builder.cpp` (per-row constraint loop, ~line 642).
- Big-M constant skip: `src/execution/operator/decide/physical_decide.cpp` (hard MIN/MAX finalize Big-M scan, ~line 3128).


---

## ABS Hard-Direction Constraints — Proper Big-M Fix (Supersedes Earlier Stopgap)

### Symptom

Earlier: DeciDB's ABS rewrite was a pure lower-envelope (`aux >= e`, `aux >= -e`), forcing only `aux >= |e|`. For constraint shapes that did not upper-bound `aux` — `ABS(...) >= K`, `ABS(...) > K`, `ABS(...) = K`, `ABS(...) <> K`, `ABS(...) BETWEEN`, ABS on both sides of a comparison, and the analogous aggregate forms (`SUM(ABS) >= K`, `MIN(ABS) >= K`, `MAX(ABS) >= K`) — the solver could satisfy the constraint by inflating `aux` above `|e|`, silently producing infeasible-relative-to-the-original-predicate solutions reported as feasible.

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

---

## `CASE WHEN` Inside DECIDE Crashed With Internal Error + C++ Stack Trace

### Symptom

Any DECIDE query whose `SUCH THAT` or objective contained a SQL `CASE` expression aborted with an `INTERNAL Error` and a ~20-frame C++ stack trace, instead of a clean user-facing rejection:

```
INTERNAL Error: ToSymbolic: Unsupported expression class: CASE

Stack Trace:
0  duckdb::Exception::Exception(...)
1  duckdb::InternalException::InternalException(...)
...
3  duckdb::ToSymbolicRecursive(...)
```

Two distinct code paths were involved:
- CASE wrapped by `SUM` (or any aggregate) inside an objective/constraint hit `ValidateSumArgumentInternal` in `decide_binder.cpp`, which produced a `Binder Error: Unsupported expression of type ExpressionClass::CASE inside DECIDE SUM expression` — clean rejection, but uninformative.
- CASE elsewhere (e.g. inside a comparison or arithmetic outside an aggregate) fell through to `ToSymbolicRecursive`'s default arm, which threw `InternalException` with the C++ stack trace shown above.

### Root cause

`ToSymbolicRecursive` in `src/decidb/symbolic/decide_symbolic.cpp` dispatched on `ExpressionClass` with arms for `CONSTANT`, `COLUMN_REF`, `OPERATOR`, `CAST`, `COMPARISON`, `BETWEEN`, `CONJUNCTION`, `FUNCTION`, and `SUBQUERY`. There was no arm for `CASE`, so it hit the catch-all `default:` that throws `InternalException` (intended for genuine internal class drift, not user-facing rejection).

Even where the binder did catch CASE first, the message (`"Unsupported expression of type ExpressionClass::CASE inside DECIDE SUM expression"`) did not point users to the supported alternatives.

### Fix

Two co-ordinated changes so both rejection paths surface the same friendly, actionable message:

1. **`src/decidb/symbolic/decide_symbolic.cpp`** — added an explicit `case ExpressionClass::CASE` arm in `ToSymbolicRecursive` before the catch-all `default:`. It throws `InvalidInputException` (not `InternalException`) with a message naming the supported alternatives:
   > CASE expressions are not supported inside DECIDE constraints or objectives. Use postfix WHEN to gate on a row predicate (e.g. `SUM(x) >= 1 WHEN category = 'A'`), PER to partition by a column, or a CTE/subquery to pre-compute conditional values before the DECIDE clause.
2. **`src/planner/expression_binder/decide_binder.cpp`** (`ValidateSumArgumentInternal`) — added a `case ExpressionClass::CASE` ahead of the generic default arm with the same friendly message, so CASE inside `SUM`/`MIN`/`MAX`/`AVG` surfaces the same wording.

The `default:` arms remain in place (now reserved for genuine internal class drift).

### Verification

- `make decide-test` — 10 new cases in `test/decide/tests/test_error_case_expression.py` exercise every surface area: SUCH THAT, MAXIMIZE, MINIMIZE, searched-vs-simple CASE, bare-SUM-of-CASE, AVG/MIN/MAX wrappers (parameterized), PER-wrapped CASE, plus an explicit no-stack-trace assertion (`INTERNAL Error`, `Stack Trace`, `ToSymbolicRecursive`, `assertion failure` must all be absent from the output).
- Manual repro from the bug doc (`SUM(x * CASE WHEN category = 'A' THEN 1 ELSE 0 END) >= 1`) now produces a single-line friendly error with no stack trace.

### Code pointers

- Symbolic-layer arm: `src/decidb/symbolic/decide_symbolic.cpp` (`ToSymbolicRecursive`, the `case ExpressionClass::CASE` insertion before `default:`).
- Binder-layer arm: `src/planner/expression_binder/decide_binder.cpp` (`ValidateSumArgumentInternal`, `case ExpressionClass::CASE` before the catch-all default).
- Tests: `test/decide/tests/test_error_case_expression.py` (`TestCaseExpressionRejection`).
- Doc: `03_expressivity/when/done.md` ("Rejection of inline CASE inside DECIDE" subsection under "WHEN vs SQL CASE WHEN").

---

## `gurobi.env` `TimeLimit` Was Silently Clobbered By A Hard-Coded 300s

### Symptom

`make decide-test` failed `tests/test_error_time_limit.py::test_time_limit_surfaces_friendly_error`:

```
FAILED tests/test_error_time_limit.py::test_time_limit_surfaces_friendly_error
subprocess.TimeoutExpired: Command '[...decidb ... MINIMIZE SUM(POWER(qty - 2, 2))]'
  timed out after 30 seconds
```

The test dropped a `gurobi.env` containing `TimeLimit 1` in the working directory and expected a symmetric MIQP to terminate within ~1s with the friendly "exceeded time limit" message. Instead the decidb subprocess ran past the 30s `subprocess.run(timeout=30)` cap, was SIGKILL'd, and produced no output for the assertion to match.

Beyond the test failure, any user dropping a tighter `TimeLimit` (or other limit-style parameters that DeciDB also re-set after Gurobi auto-loaded `gurobi.env`) had their override silently discarded — a footgun for power users.

### Root cause

`src/decidb/gurobi/gurobi_solver.cpp` ran:

```cpp
api.setdblparam(guard.env, "TimeLimit", 300.0);
```

unconditionally *after* `emptyenv_internal` — the call where Gurobi auto-reads `gurobi.env`. So any `TimeLimit` in the env file was overwritten by the hard-coded 300s before `startenv`, and the 1s budget the test relied on never fired.

The friendly-error branch (`status == GRB_TIME_LIMIT` → "DECIDE optimization exceeded time limit.") was wired correctly; it just was never reachable in a 30s window with the limit pinned at 300s.

### Fix

Replaced the hard-coded `300.0` with a value read from a new `DECIDB_TIME_LIMIT` env var (seconds, double), defaulting to 300.0 when the env var is absent, empty, unparseable, or non-positive. The env var sits alongside the existing `DECIDB_FORCE_SOLVER` knob and is the supported override mechanism going forward.

```cpp
double time_limit = 300.0;
if (const char *env_limit = std::getenv("DECIDB_TIME_LIMIT")) {
    try {
        double parsed = std::stod(env_limit);
        if (parsed > 0.0) {
            time_limit = parsed;
        }
    } catch (...) {
        // Ignore unparseable values; keep the default.
    }
}
api.setdblparam(guard.env, "TimeLimit", time_limit);
```

`std::stod` is wrapped in a try/catch so non-numeric values don't bubble up; non-positive values are rejected because they're nonsensical as a time budget.

The test (`test/decide/tests/test_error_time_limit.py`) was rewritten as a `TestTimeLimit` class with 7 parameterized cases covering: env-var-surfaces-friendly-error, default-doesn't-break-normal-queries, four garbage-value cases that must fall back silently, and a 0.5s value on the pathological MIQP that catches off-by-orders-of-magnitude parsing bugs.

### Verification

- `make decide-test` — `test_error_time_limit.py` passes all 7 cases (Gurobi-only; skips when Gurobi is unavailable). No regressions in the rest of the suite.
- Manual repro: `DECIDB_FORCE_SOLVER=gurobi DECIDB_TIME_LIMIT=1 build/release/decidb decidb.db -readonly -c "<symmetric MIQP>"` terminates in ~1s with "DECIDE optimization exceeded time limit." Without the env var, normal queries still complete under the 300s default.

### Code pointers

- Env-var read + override: `src/decidb/gurobi/gurobi_solver.cpp` (in `GurobiSolver::Solve`, just after `emptyenv_internal`).
- Friendly-error branch (unchanged): same file (`status == GRB_TIME_LIMIT`).
- Constant: `src/decidb/gurobi/gurobi_loader.hpp` (`GRB_TIME_LIMIT = 9`, must match Gurobi's `gurobi_c.h`).
- Tests: `test/decide/tests/test_error_time_limit.py` (`TestTimeLimit`).

---

## `PER table.column` (Qualified Column Reference) Failed To Parse

### Symptom

The `PER` clause rejected table-qualified column references with a parser error pointing at the dot:

```
Parser Error: syntax error at or near "."

LINE 9:     SUM(x) <= 3 PER r.resource_id AND
                             ^
```

Unqualified column names parsed and executed correctly, including in JOIN queries where the column lived on a specific side. The inconsistency with the rest of SQL (`SELECT`, `GROUP BY`, `ORDER BY`, `WHERE` all accept the qualified form) made the failure surprising — the workaround (drop the qualifier) was unobvious from the error message.

### Root cause

In `third_party/libpg_query/grammar/statements/select.y`, the four single-column PER rule arms (lines 238, 259, 316, 337) used the `columnref` non-terminal, which accepts only `ColId` — a bare unqualified identifier. The multi-column form `PER '(' columnrefList ')'` had the same restriction because `columnrefList` was itself built over `columnref`.

`columnref` was used nowhere else in the grammar — only by the DeciDB PER/WHEN rules — so the restriction was effectively a DeciDB-specific limitation that diverged from the rest of DuckDB's SQL surface.

The C++ transformer (`src/parser/transform/expression/transform_operator.cpp`, the `PG_AEXPR_PER_CONSTRAINT` arm) was already general — it called `TransformExpression(...)` on the right side, which handles both bare and qualified `ColumnRef` AST nodes identically. Only the grammar needed to change.

### Fix

Swapped `columnref` for `columnref_opt_indirection` in the PER rule arms and inside `columnrefList`:

- `columnref_opt_indirection` (already defined at `select.y:4002`) accepts `ColId` and `ColId indirection`, producing a properly-qualified `ColumnRef` node. This is the same production used elsewhere in DuckDB SQL (e.g. `d_expr` at line 2933 of `select.y`).
- The original `columnref` rule is left in place but becomes unused — keeping it is harmless and deleting it is a separate cleanup if it ever matters.

`a_expr` (the GROUP BY production) was explicitly rejected: PER appears mid-statement followed by `AND`, `,`, `MAXIMIZE`, etc., and `a_expr` would have created shift/reduce ambiguity — `PER r.resource_id AND ...` could parse `r.resource_id AND ...` as one boolean. `columnref_opt_indirection` is restrictive enough to avoid that ambiguity while covering the use case the bug actually asks for.

Regenerated parser via `make grammar-build`.

### Verification

- `make decide-test` — 8 new cases in `test/decide/tests/test_per_qualified.py`: single-column qualified in constraint (with oracle comparison), multi-column all-qualified, multi-column mixed qualified+bare, qualified-equivalent-to-unqualified semantic check, qualifier with table alias, WHEN+PER+qualifier, objective-side qualified, and an unknown-qualifier clean-error case. All existing PER tests (`test_per_clause.py`, `test_per_multi_column.py`, `test_per_objective.py`, `test_per_interactions.py`) still pass.
- Manual repro from the bug doc (`SUM(x) <= 3 PER r.resource_id AND SUM(x) = 1 PER s.slot_id` on a JOIN-based DECIDE) now parses and produces a valid solution.

### Code pointers

- Grammar: `third_party/libpg_query/grammar/statements/select.y` (PER arms in `decide_objective_item` and `decide_constraint_item`; `columnrefList` rule).
- Generated parser (regenerated, do not hand-edit): `third_party/libpg_query/src_backend_parser_gram.cpp`.
- Transformer (unchanged, already general): `src/parser/transform/expression/transform_operator.cpp` (`PG_AEXPR_PER_CONSTRAINT` arm).
- Tests: `test/decide/tests/test_per_qualified.py`.
- Doc: `03_expressivity/per/done.md` ("Examples" subsection); `00_project_overview/syntax_reference.md` §7.
