# Bugs — Fixed (Lessons)

Resolved bugs condensed to their generalizable lessons: what broke, why, and what to watch for when touching the same area. Full postmortems live in git history (`git log --follow -- context/descriptions/07_issues/bugs/done.md`; the file previously lived at `context/descriptions/07_bugs/done.md`). Active bugs live in `todo.md`.

---

## `func_application WHEN` in global `c_expr` corrupted ordinary function calls

**Broke**: Putting the aggregate-local-WHEN rule in the global `c_expr` non-terminal created a shift/reduce conflict (silenced by `%expect 4`) that made *any unregistered* function call and all catalog introspection (`SHOW TABLES`, `duckdb_tables()`, …) fail to parse. CI stayed green because DECIDE tests only call registered aggregates.

**Fix/lesson**: Context-sensitive token — `base_yylex` rewrites `WHEN`→`WHEN_DECIDE` while an `in_decide_clause` scanner flag is set (suspended inside `CASE…END` via `decide_case_depth`), so DECIDE WHEN never reaches the global grammar. This mirrors DuckDB's existing `NOT_LA`/`WITH_LA`/`NULLS_LA` mechanism. **Watch for**: (a) new DECIDE keywords leaking into shared non-terminals — prefer the context-token approach; (b) `in_decide_clause` is a bool, not a depth counter — promote it if nested DECIDE is ever supported; (c) grammar bugs can hide behind `%expect` — a green DECIDE suite doesn't prove the *rest* of SQL still parses (regression guard: `test_parser_catalog_introspection.py`).

Pointers: `grammar/grammar.y`, `grammar/statements/select.y`, `include/parser/gramparse.hpp`, `src_backend_parser_parser.cpp` (all under `third_party/libpg_query/`).

## Table-scoped `Table.var` didn't bind in SELECT list / per-row constraints

**Broke**: `SELECT supplier.pick … DECIDE supplier.pick IS BOOLEAN` failed with "Table has no column named pick". The DECIDE-internal binders consult `decide_variable_names` (which registers qualified + unqualified forms), but the SELECT list and the per-row constraint fall-through use the *regular* DuckDB binder, which only sees the unqualified generic binding — qualified refs routed to the real table binding. The aggregate path only worked because SymEngine *accidentally* strips qualifiers.

**Fix/lesson**: A parsed-AST pre-pass (`RewriteScopedVarRefs` in `bind_select_node.cpp`) rewrites qualified refs to scoped DECIDE vars into bare refs before any binding. **Watch for**: DECIDE name resolution is split across two binder worlds (DECIDE binders vs. regular DuckDB binder); any new place an expression escapes to the regular binder needs scoped-var refs normalized first. Don't rely on SymEngine's qualifier-stripping side effect.

## Gurobi status constants were wrong → time-limit hit threw "no solution"

**Broke**: DeciDB defined `GRB_TIME_LIMIT = 7`; Gurobi's real value is 9. On timeout, the rescue branch never matched and DeciDB threw "no solution found with status 9" even with a feasible incumbent in the pool. Also, only TIME_LIMIT was treated as "may carry an incumbent" — NODE/SOLUTION_LIMIT, INTERRUPTED, SUBOPTIMAL went straight to throw.

**Fix/lesson**: Constants in `gurobi_loader.hpp` must match `gurobi_c.h` exactly — verify against the header, never from memory. Any termination status with `SolCount > 0` should surface the incumbent (status rewritten to OPTIMAL so the extraction path runs). A raw numeric status in a user-visible error is a smell that a status enum is out of sync.

## DECIDE errors poisoned the whole session ("database has been invalidated")

**Broke**: Submit-time Gurobi failures and execution-time shape rejections threw `InternalException`, which DuckDB's connection layer treats as fatal — one bad DECIDE query killed every subsequent query in the session. Separately, `POWER(AVG(x), 2)` passed the bind check (it "contains an aggregate") and only blew up at execution.

**Fix/lesson**: User-input problems must throw `InvalidInputException`; `InternalException` is reserved for genuine invariant violations because it invalidates the connection. When validating objective shapes, distinguish additive composition (`+`/`-`/`*`, which legitimately mixes scalars and aggregates) from non-additive wrappers around aggregates (`POWER(AGG(…))` etc.) — "contains an aggregate somewhere" is not a sufficient bind-time check. Catch unsupported shapes at bind time; an execution-time throw is already a failure of the validation layer.

Pointers: `decide_objective_binder.cpp` (`GetExpressionType`), `gurobi_solver.cpp` throw sites, `physical_decide.cpp` extract rejections.

## PER with every group empty was rejected instead of skipped

**Broke**: An explicit `num_groups == 0` guard in `physical_decide.cpp` rejected PER constraints when WHEN filtered out *all* groups — contradicting the documented "empty groups are skipped" semantics (single empty groups were already skipped downstream).

**Fix/lesson**: Removed the guard; zero groups → zero constraints emitted, which is the mathematically correct vacuous case. Non-PER empty WHEN remains rejected (almost always a user mistake). **Watch for**: spec/implementation drift at boundary conditions — the all-empty case took a different code path from the some-empty case.

## ABS hard-direction constraints were silently unsound → proper Big-M fix

**Broke** (critical soundness bug): `RewriteAbs` emitted only the lower envelope (`aux >= e`, `aux >= -e`), which forces `aux >= |e|` but lets `aux` float above it. Sound when the constraint *upper-bounds* `aux` (`ABS ≤ K`, `SUM(ABS) ≤ K`, MINIMIZE); silently wrong for lower-bounding shapes (`ABS >= K`, `=`, `<>`, BETWEEN, `SUM/MIN/MAX(ABS) >= K`) — the solver inflated `aux` and reported infeasible-relative-to-the-predicate solutions as feasible. Undetected for a long time because existing tests *coincidentally* produced correct-looking answers. An interim fix rejected the hard shapes at bind time.

**Fix/lesson**: Final fix tags hard-direction ABS nodes (`ABS_NEEDS_BIGM_TAG` via `TagAbsConstraintsForBigM`) so `RewriteAbs` adds a binary sign indicator plus the upper envelope `aux <= e + 2M(1-y)`, `aux <= -e + 2My`, pinning `aux = |e|` exactly (same machinery the MAXIMIZE-objective path always had; emission iterates `abs_maximize_links` in `physical_decide.cpp`). **Watch for**: any auxiliary-variable linearization is only as sound as the envelope *the constraint direction actually enforces* — check both directions explicitly, and don't trust passing tests whose optimum happens to coincide with the sound region. Bind-time rejection is the right stopgap when a sound formulation isn't ready. Doc: `03_expressivity/sql_functions/done.md` (Path A / Path B).

## Linear-after-distribution shapes misclassified as nonlinear

**Broke**: Per-row constraints like `MIN(s_acctbal * pick + 1000 * (1 - pick)) <= 500` — linear in `pick` after distribution — threw "unexpanded nonlinear product": `ClassifyNormalizedProduct` expected each `*` factor to be a bare data expression or bare decide-var, and the per-row path doesn't go through SymEngine. Separately, multiple terms over the *same* variable produced duplicate column indices that `GRBaddconstr` rejected.

**Fix/lesson**: `TryDistributeMultiplyOverAdd` (`physical_decide.cpp`) distributes `*` over `+`/`-` before classification at every extractor `*` branch; per-row emission in `ilp_model_builder.cpp` now sums coefficients per column index before pushing. **Watch for**: the per-row extraction path has no SymEngine normalization — algebraic shapes that the aggregate path handles "for free" need explicit handling there. Solver APIs typically reject duplicate column indices in one constraint; always aggregate coefficients first. Big-M bound scans must skip `INVALID_INDEX` (constant) entries.

## `CASE WHEN` inside DECIDE crashed with internal error + stack trace

**Broke**: `ToSymbolicRecursive` had no `CASE` arm, so CASE expressions hit the catch-all `default:` `InternalException` — a ~20-frame stack trace for what is simply an unsupported user input.

**Fix/lesson**: Added explicit `ExpressionClass::CASE` arms (symbolic layer + `ValidateSumArgumentInternal` in `decide_binder.cpp`) throwing `InvalidInputException` with the supported alternatives (postfix WHEN, PER, pre-computed CTE). **Watch for**: catch-all `default:` arms in expression-class dispatches turn unsupported-but-reachable user input into internal errors — every user-reachable expression class needs either support or a friendly rejection. Tests assert the *absence* of "INTERNAL Error"/"Stack Trace" in output (`test_error_case_expression.py`).

## Hard-coded 300s clobbered user's `gurobi.env` TimeLimit

**Broke**: `setdblparam(env, "TimeLimit", 300.0)` ran unconditionally *after* Gurobi auto-loaded `gurobi.env`, silently discarding any user override — and made the time-limit test hang past its subprocess timeout.

**Fix/lesson**: Time limit now comes from `DECIDB_TIME_LIMIT` (seconds; default 300; garbage/non-positive values fall back silently), alongside `DECIDB_FORCE_SOLVER`. **Watch for**: ordering of solver-environment configuration — anything set after env auto-load overrides the user's config file; defaults should be applied only when the user hasn't specified a value, and overrides need an explicit, documented knob.

## `PER table.column` failed to parse

**Broke**: The PER grammar arms used the `columnref` non-terminal (bare `ColId` only), so qualified references (`PER r.resource_id`) died with a parser error — inconsistent with the rest of SQL. The transformer was already general; only the grammar was restrictive.

**Fix/lesson**: Swapped to `columnref_opt_indirection` (the production DuckDB uses elsewhere for qualified refs) in the PER arms and `columnrefList`. `a_expr` was deliberately rejected: PER is followed by `AND`/`,`/`MAXIMIZE`, and `a_expr` would swallow `r.resource_id AND …` as one boolean expression — pick the narrowest non-terminal that covers the need. **Watch for**: DECIDE-only grammar non-terminals silently diverging from standard SQL surface; when a qualified form works in SELECT/GROUP BY but not in a DECIDE clause, suspect the grammar rule, not the transformer. Requires `make grammar-build` to regenerate.

## Per-row constraint with decide vars on both sides + data-led LHS silently not enforced

**Broke**: In the multi-variable per-row path (`physical_decide.cpp` `AnalyzeConstraint`, BOUND_COMPARISON), decide-var terms from both sides are moved into `lhs_terms` and the bound is set to `StripDecideVars(rhs)` — but the LHS's *data/constant* part was never captured. So `(10 - x) <= y` became `-x - y <= 0` (`x+y >= 0`) instead of `x+y >= 10`; the `10` was dropped. The constraint was silently mis-solved (no error). Only triggered when decide vars appear on both sides (RHS-var path) *and* the LHS carries a constant/data term; everything-on-LHS forms (`10 - x - y <= 0`, constant RHS) took the `else`/`ExtractConstraintTerms` path which folds LHS constants correctly.

**Fix/lesson**: Carry the stripped LHS data on the constraint (`DecideConstraint::lhs_offset_expr = StripDecideVars(lhs)`) and **subtract it from the bound at evaluation** (foldable → one scalar subtract; data column → per-row). No-op for bare-var LHS (folds to 0), so ABS/MIN-MAX/L0-link constraints are unaffected. **Watch for**: when moving terms across a comparison, *both* sides contribute data to the bound — `bound = rhs_data - lhs_data`; the manual multi-var path handled only RHS data. Regression: `test_cons_lhs_offset.py`. Discovered while building L0 `norm(e,0)` auto-M.

## Explicit negative lower bounds were silently clamped to 0 (`x >= -5`, `x IN (-5, ...)`)

**Broke**: Two independent clamps pinned every decision variable to `[0, +inf)` with no error. (1) Bound absorption (`physical_decide.cpp` `DecideGlobalSinkState`) initialized `absorbed_lower_bounds` to `0` and combined `>=` constraints with `std::max`, so `x >= -5` became `max(0, -5) = 0`. (2) The model builder (`ilp_model_builder.cpp`) independently re-clamped via `std::max(per_var_lower /*=0*/, input.lower_bounds)`. So a user writing `x >= -5` / `x BETWEEN -10 AND 10` got `x >= 0` silently; an `x IN (-5, 0, 5)` domain silently dropped the `-5` selection (the linking equality made `z=1` for `-5` infeasible against `x >= 0`), and a sole negative IN value (`x IN (-5)`, K=1 → `x = -5`) made the query infeasible. Inconsistent, too: the `=` absorption path had no `std::max`, so `x = -5` *was* honored while `x >= -5` was not.

**Fix/lesson**: Lower-bound absorption now initializes to an "unset" sentinel (`ABSORBED_LOWER_UNSET`); the `std::max` combiner keeps the tightest of multiple `>=` constraints for negatives too; `Finalize` resolves any still-unset variable to the default `0`. The model builder takes the resolved lower bound directly instead of re-clamping. The `IN` rewrite (`bind_select_node.cpp`) widens the variable's lower bound to the domain minimum when a negative constant literal is present. **Watch for**: a "default" implemented as `std::max(value, 0)` is also a silent *floor* — initialize to a sentinel and apply the default only to the unset case so legitimate negatives survive. Two sites clamped independently; grep for every place a bound is defaulted. Regression: `test/decide/tests/test_signed_variables.py`. Discovered + fixed while implementing signed (negative-domain) decision variables.
