# Bugs — Fixed (Lessons)

Resolved bugs condensed to their generalizable lessons: what broke, why, and what to watch for when touching the same area. Full postmortems live in git history (`git log --follow -- context/descriptions/07_issues/bugs/done.md`; the file previously lived at `context/descriptions/07_bugs/done.md`). Active bugs live in `todo.md`.

---

## Infeasible diagnosis mislabeled a data-weighted `AVG` clause as `SUM`

**Broke**: `FormatLhs` (`decide_diagnostic_engines.cpp`) renders an AVG-rewritten row (`provenance.avg_scaled`) via `FormatAvgLhs`, which collapses the 1/N-scaled fan-out back to `AVG(inner)` — but only when the per-variable coefficient is *uniform*. A **data-varying** coefficient (`AVG(buy * p_retailprice)`, price differs per row) made `FormatAvgLhs` bail, and the row fell through to `FormatSumLhs`, which hardcoded a `"SUM("` wrapper. So an infeasible `AVG(buy * p_retailprice) >= 5000` was diagnosed and *suggested* as `SUM(buy * p_retailprice) >= 904.5`. The inner (`buy * p_retailprice`) and the number (904.5, the achievable AVG) were correct — only the aggregate label was wrong — but since 904.5 is in AVG units, the displayed `SUM(...) >= 904.5` is a *different, wrong* constraint. The suggested edit is the area's core promise (smallest edit in the user's own units/syntax), so a mislabel that a user applies literally corrupts their query.

**Fix/lesson**: Thread an `agg_name` through `FormatSumLhs` (default `"SUM"`), and in `FormatLhs` pass `"AVG"` whenever `provenance.avg_scaled` — so a data-varying AVG that can't take the clean `FormatAvgLhs` path still renders as `AVG(...)` (the same Facet-B fallback wrapper too). The inner terms and RHS were already correct; only the wrapper label changed. **Watch for**: the aggregate *label* and the numeric *value* live in different code paths — a rendering fallback that hardcodes an aggregate name will silently disagree with a value computed in another aggregate's units. Any clause reconstruction must carry the aggregate identity, not re-guess it. Tests: `test_query_diagnostics_relation.py::TestInfeasibleReporting::test_infeasible_data_varying_avg_keeps_avg_label` (both backends). Found in the query-diagnostics README example catalog.

## Infeasible diagnosis rendered equality as DuckDB alias `==`

**Broke**: Infeasible diagnostic clause rendering used the internal equality spelling
`==` (`x == 5` -> `x == 8`) even when the SQL user wrote `=`. DuckDB accepts `==`, but the
diagnosis relation and suggested edit are user-facing SQL, so the rendered subject did not
match the query text and the apply-the-fix harness needed a special subject override.

**Fix/lesson**: Diagnostic renderers should prefer standard SQL spelling over internal or
accepted-alias spellings. `SenseStr` now renders equality as `=`, and the relation/apply-fix
tests assert the suggested edit is directly pasteable. **Watch for**: any diagnostic label
that comes from solver/model notation should be translated back to the user's SQL surface
before reaching `decide_diagnostics()`.

Pointers: `decide_diagnostic_engines.cpp` (`SenseStr`), `test_decidb_diagnostic_engines.cpp`,
`test_query_diagnostics_relation.py` (`TestEqualityBoundConflict`).

## Quadratic infeasible diagnosis leaked no-op edits and implicit RHS casts

**Broke**: A QCQP infeasible query with a data-backed linear floor
(`POWER(x,2) <= 4 AND x >= lo`) reported the real quadratic loosening plus a second
zero-amount virtual offset on the data floor. That no-op edit survived the pre-snap epsilon
filter, then rounded to `0` for display. The same row rendered the data RHS from
`rhs_expr->GetName()`, leaking the binder's implicit `CAST(lo AS DOUBLE)` into a suggested
SQL edit that `SUCH THAT` cannot accept.

**Fix/lesson**: Filter display-snapped slack amounts too, not only raw solver amounts, so
sub-tolerance stage-2 noise cannot become an actionable-looking `amount = 0` edit. Also,
provenance labels intended for user SQL must use a diagnostic renderer, not raw bound
expression names: data RHS labels now unwrap top-level binder casts before storage.
**Watch for**: stage-2 repair re-solves can introduce tiny nonzero slacks while optimizing
the user's objective; every readback path that snaps values for display needs a post-snap
reportability check.

Pointers: `decide_diagnostic_engines.cpp` (`ReadElasticEdits`), `physical_decide.cpp`
(`RenderDiagnosticRhsLabel`), `test_query_diagnostics_relation.py`
(`test_infeasible_quadratic_loosens_linear_rhs`).

## Infeasible least-change reported a degenerate "require nothing" edit

**Broke**: With two editable constraints in *incomparable units* — a count floor
`SUM(buy) >= 30` and a dollar budget `SUM(buy*l_extendedprice) <= 100` — the infeasible
diagnosis reported `loosen SUM(buy) >= 30 to >= 0` (`achievable_objective = 0`, "select
nothing") instead of loosening the genuinely-tight budget. **Two independent causes**,
both pointing at the same degenerate edit, which is why it looked like a pure units
problem: (1) the elastic engine's stage-1 objective `min Σ wᵢ sᵢ` (uniform `wᵢ = 1`)
summed slacks as one currency, so 30 count-units looked cheaper than the thousands of
dollar-units to relax the budget; and (2) `DecidePropagateImpliedBounds` had absorbed the
budget into the column box as `buyᵢ ≤ 100/priceᵢ ≈ 0.1`, and the diagnosis bound-reset
loop skipped BOOLEAN columns — so every integer `buy` was pinned to 0 and the budget could
never be exercised even after being made relaxable.

**Fix/lesson**: (1) Weight each editable slack `ref / rms(Aᵢ)`, where `rms(Aᵢ) =
√(Σcⱼ²/nnz)` is the row's root-mean-square coefficient (T1). This makes the objective track
the loosening *per unit of decision*, so the large-coefficient budget is the cheaper edit.
RMS, not the plain L2 norm `‖Aᵢ‖₂`: L2 grows with the term count, so it would make a
many-variable aggregate floor cheaper to loosen than a single-variable cap purely for
having more terms — reintroducing the degeneracy on a different query. (2) The diagnosis
bound-reset now reverts an absorbed tightening on a BOOLEAN back to its `[0,1]` box
(upper → 1, lower → 0) instead of skipping it, so the slackable row is the sole enforcer.
**Watch for**: a "least total slack" race across rows in different units is
scale-dependent (normalize by RMS coefficient — RHS normalization `1/|b|` does *not* fix a
row needing a large absolute loosening); and a presolve/bound-absorption tightening baked
into a variable's box will silently veto a constraint the diagnosis is trying to relax —
revert absorbed box tightenings (booleans included) in the diagnosis model.

Pointers: `decide_diagnostic_engines.cpp` (`BuildElasticModel`, editable slack weighting +
`FormatSumLhs`), `physical_decide.cpp` (diagnosis bound-reset boolean box revert),
`diagnostic_constants.hpp`, `test_decidb_diagnostic_engines.cpp` (weight structural case),
`test_query_diagnostics_tpch.py` (`test_E_loosen_should_not_be_degenerate`),
`test_query_diagnostics_relation.py` (`test_data_weighted_sum_renders_symbolic_column`,
`test_ungrouped_weighted_sum_folds_with_uniform_coeff`).

---

## Entity-scoped unbounded diagnostics ignored joined dimension labels

**Broke**: For an entity-scoped variable such as `DECIDE s.keep`, unbounded
diagnostics named the escaping variable but fell back to a bare count
(`2 of 50 entities`) when the escaping slice was described by a joined dimension
column (`n_name = 'GERMANY'`). Row-scoped diagnostics already scanned all named
input columns; entity-scoped diagnostics scanned only the entity key columns.

**Fix/lesson**: Entity-scoped characterization now scans every named DECIDE-clause
input column, then lifts the row grouping to entity granularity only if every row
for an entity has the same candidate value. This keeps useful many-to-one
dimension labels while rejecting genuinely one-to-many labels that would invent a
single value for an entity. **Watch for**: widening a diagnostic candidate scan
without a functional-dependency/constancy guard can produce plausible but false
explanations.

Pointers: `physical_decide.cpp` (`UnboundedCandidateProvider::LiftRowGroupingToEntities`),
`test_query_diagnostics_escaping_instances.py` (`test_entity_scoped_join_column_escape_rule`),
`test_query_diagnostics_tpch.py` (`test_B_entity_escape_names_join_column`).

## "SUCH THAT rejects composite PER" was a mis-diagnosis — bare `PER a, b` is a grammar ambiguity, not a binder gap

**Broke (allegedly)**: `SUCH THAT SUM(x) >= 1 PER region, yr` failed with `Binder Error: ... does not support 'yr'(COLUMN_REF)`, logged as "the constraint binder only consumes the first PER column." **Reality**: composite PER already works end-to-end via the parenthesized form `PER (region, yr)` — the grammar has a `PER '(' columnrefList ')'` production and `BindPerConstraint` already loops over all columns `children[1..N]`. The bare comma-separated form has no grammar production. As of 2026-07-04, top-level commas in `SUCH THAT` are rejected by the parser with a "use AND" hint, so this shape no longer reaches the binder as a trailing bare-column "constraint."

**Fix/lesson**: The bare form is a genuine LALR(1) shift/reduce ambiguity — after `PER region` with lookahead `,`, the parser cannot tell "another PER column" from "next constraint" (a constraint item can be a bare `a_expr`). This is *why* the parenthesized form exists; parentheses are the disambiguator (same reason SQL parenthesizes row-value tuples). Decided (2026-07-02) not to add the bare form. The top-level `COLUMN_REF` case in `decide_constraints_binder.cpp` still emits an actionable hint for lone-column constraints, but comma-separated constraints are no longer accepted as an alternate spelling for `AND`. Documented the parenthesization requirement in `syntax_reference.md` §7.3. **Watch for**: before "fixing a binder gap," check the grammar — a symptom that looks like an incomplete binder can be a deliberately unambiguous syntax boundary.

## HiGHS timeouts raised INTERNAL instead of TIME_LIMIT (kWarning swallowed)

**Broke**: `DeterministicNaive::Solve` guarded `highs.run()` with `if (status != HighsStatus::kOk) throw`. But HiGHS returns `kWarning` (== 1), not `kOk`, when it stops at the time limit — so a hard MILP that hit `DECIDB_TIME_LIMIT` on HiGHS crashed with `INTERNAL Error: HiGHS solver failed: status 1, model_status 13` before control ever reached the `kTimeLimit → TIME_LIMIT` branch of the model-status switch. Gurobi timed out correctly; only HiGHS was affected, contradicting the doc/comment that claimed HiGHS "returns kTimeLimit". This blocked all slow-diagnostics work on HiGHS.

**Fix/lesson**: Throw only on `HighsStatus::kError`; let `kWarning` fall through to the model-status switch, which maps `kTimeLimit → TIME_LIMIT` (and the incumbent / best-bound / gap become readable). **Watch for**: a solver "success" guard written as `!= kOk` conflates a warning-with-usable-result with a hard failure — gate the throw on the *error* sentinel, not on "not-perfect." Verified on both backends via the market-split probe. Discovered 2026-07-02 probing time-limit behavior for slow diagnostics; fixed as S0 of the slow branch.

## Data-only terms inside DECIDE SUM constraints escaped as RHS aggregates

**Broke**: `SUM(q * (price + x)) <= K` and similar aggregate constraints could bind, but symbolic normalization split the data-only aggregate contribution onto the RHS as `SUM(data_expr)`. Physical RHS evaluation only knows how to fold generated `count_star()` aggregates, so execution threw an `InternalException` for an unsupported RHS `sum` instead of either accepting the linear form or rejecting it as user input.

**Fix/lesson**: Keep every `SUM(...)` additive contribution on the aggregate LHS, including data-only pieces. Coefficient evaluation already represents fixed LHS terms as `variable_index == INVALID_INDEX`; the linear, grouped, and quadratic model-builder paths now subtract those fixed row/group contributions from the scalar RHS. The remaining direct RHS aggregate path throws `InvalidInputException`, not `InternalException`. **Watch for**: normalizers should not manufacture execution-time expression shapes that the physical evaluator cannot handle; if a data-only term is algebraically part of the active aggregate group, preserve that grouping until rows/groups are known.

Pointers: `decide_symbolic.cpp` (`NormalizeComparisonExpr`), `ilp_model_builder.cpp` (`FixedLinearLhsOffset`, quadratic RHS adjustment), `physical_decide.cpp` (`TransformToChunkExpression`, aggregate `<>` fixed offset), `test_cons_aggregate.py` (`test_sum_body_data_only_offset_*`, `test_avg_body_data_only_offset`), `test_quadratic_constraints.py` (`test_aggregate_quadratic_constraint_data_only_offset`), `test_error_binder.py` (`test_data_only_rhs_aggregate_errors_without_internal`). Fixed 2026-06-29.

## Strict quadratic infeasible diagnosis exposed the normalized non-strict row

**Broke**: Strict quadratic / bilinear constraints use the quadratic model-builder path.
That path correctly enforced the integer-step rewrite (`POWER(x,2) < 10` → `<= 9`) but did
not stamp `ConstraintProvenance::strict` / `typed_k`, so infeasible diagnosis reported
`POWER(x, 2) <= 9` → `POWER(x, 2) <= 16` instead of re-quoting the user's strict literal as
`POWER(x, 2) < 10` → `POWER(x, 2) < 17`.

**Fix/lesson**: Every path that applies strict integer-step normalization must also carry
the user's adjusted typed literal into provenance. The linear path already did this in
`ApplyComparisonSense`; the quadratic path now mirrors it in `BuildQuadraticConstraint`.
Pinned by a Gurobi-gated diagnostics relation regression.

Pointers: `ilp_model_builder.cpp` (`BuildQuadraticConstraint` strict `<` / `>` cases),
`test_query_diagnostics_relation.py` (`test_infeasible_strict_quadratic_requotes_typed_literal`).

## Column-bound conflicts were invisible to the infeasible elastic engine

**Broke**: An infeasible DECIDE whose conflict lived in a variable's **column bounds** (not a matrix row) was never diagnosed — it fell through to the plain static error instead of a least-change fix. Two flavors, one root cause: (1) a foldable single-variable cap like `x <= 2+3` is emitted as a slackable row *and* copied into a rigid implied `col_upper` by `DecidePropagateImpliedBounds` (a presolve tightening with no provenance), so the rigid box shadowed the row and the solver loosened the wrong (data) constraint; (2) two opposite absorbed bounds (`x <= 4 AND x >= 10`) inverted the box (`col_lower > col_upper`), and `SolverModel::Build` threw `DecideInfeasibleModelException` **before `retained_model` was populated** (`ilp_solver.cpp`), handing the engine an empty model. The built `SolverModel`'s `col_lower`/`col_upper` is a *fused* product of intrinsic domain + absorbed user bounds + implied tightenings, with no way to tell them apart after the fact.

**Fix/lesson**: Three parts, all keyed on "every user limit must be carried by a slackable row, the column box only the intrinsic domain." (A) In the `DiagnosisTerminal::INFEASIBLE` arm, after re-emitting absorbed bounds as rows, reset each non-BOOLEAN decide column's implied tightenings back to intrinsic (`col_lower>0 → 0`, `col_upper → +inf`) — safe because every implied tightening has a backing row that still enforces it. (B) Add `SolverInput::tolerate_infeasible_bounds` (set when diagnosis is armed): `Build` keeps the inverted box instead of throwing, and `SolveModel` short-circuits to INFEASIBLE **without handing the inverted box to the backend** (HiGHS hard-rejects `lb>ub` at load and poisons the session; Gurobi tolerates it — solver-agnostic care). (C) A user bound that contradicts the *intrinsic* domain (`x <= -1` on non-negative REAL, `x >= 2`/`x = 2` on BOOLEAN) is a deterministic type error, **not** an elastic edit: a post-absorption check in the sink constructor throws a precise static message (`"x >= 2 cannot hold because x is BOOLEAN (0 or 1)"`). **Watch for**: BOOLEAN is lowered to an INTEGER carrying a `[0,1]` box, so `is_binary[col]` is *false* for it — `op.is_boolean_var[var]` is the only reliable boolean signal (using `is_binary` to gate the column reset silently widened the 0/1 box to `+inf` and turned the variable unbounded, regressing every boolean infeasible case). The intrinsic-conflict guard must use `U < 0 AND L >= 0` (not just `U < 0`) so an explicitly-lowered floor (`x <= -1 AND x >= -5`) stays feasible.

Pointers: `physical_decide.cpp` (`DiagnosisTerminal::INFEASIBLE` arm — re-emission + intrinsic reset; the post-absorption Part C check after `TraverseBoundsConstraints`; `tolerate_infeasible_bounds` set in `Finalize`), `ilp_model_builder.cpp` (gated inverted-box throw), `ilp_solver.cpp` (`SolveModel` short-circuit), `solver_input.hpp` (`tolerate_infeasible_bounds`). The foldable-RHS shared-literal classification (`rhs_is_shared_literal = IsFoldable()`) is a prerequisite. Deferred sibling (uncorrelated scalar-subquery RHS) is in `todo.md`.

## Absorbed strict variable bounds lost their strict re-quote in infeasible diagnosis

**Broke**: A strict simple bound absorbed into the column box (e.g. `x < 10` on an INTEGER, normalized to `x <= 9`) was re-emitted for the elastic engine as the normalized non-strict row, so the diagnosis reported `x <= 9` instead of the user's `x < 10` → `x < 16`. The absorbed-bound path recorded only `{var_idx, sense, k}` and bypassed the strict-provenance stamping that the non-absorbed model-builder path does in `ApplyComparisonSense`.

**Fix/lesson**: Carry `strict` + `typed_k` (the user's original literal) on `UserBoundSpec`, set them in the strict `<`/`>` branches of `TraverseBoundsConstraints` (and the non-inclusive BETWEEN sides), and copy them onto the re-emitted row's `ConstraintProvenance` so `MakeLoosenEdit` re-quotes `<`/`>` against `typed_k`. **Watch for**: any second path that synthesizes a `ModelConstraint` for a user clause must mirror the provenance fields the main builder stamps (`strict`, `typed_k`, `shape`, `avg_scaled`) — diagnosis rendering reads provenance, not the raw row.

Pointers: `physical_decide.cpp` (`UserBoundSpec`, `TraverseBoundsConstraints` strict + BETWEEN branches, INFEASIBLE-arm re-emission), `ilp_model_builder.cpp` (`ApplyComparisonSense`), `decide_diagnostic_engines.cpp` (`MakeLoosenEdit`).

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

## L0 `norm(e, 0)` over-counted zeros in lower-bound / equality / maximize contexts

**Broke**: `RewriteNormL0` (`bind_select_node.cpp`) emitted only a one-way Big-M link forcing `z = 1` when `e != 0`, never forcing `z = 0` when `e = 0`. So `SUM(z)` was an *upper* bound on the nonzero count, not exact. Sound where the context pushes the count down (`MINIMIZE` penalty, `norm(e,0) <= K`), but **unsound** where it pulls the count up: `norm(e,0) >= K`, `= K`, and `MAXIMIZE norm(e,0)` could satisfy the target with spurious `z = 1` on zero rows. E.g. `x = 0 AND norm(x,0,100) >= 2` (true count 0) returned a solution instead of infeasible.

**Fix/lesson**: Added the **reverse** link `ABS(e) >= tol*z` (forces `z = 0` when `|e| < tol`), making `SUM(z)` the exact count in every context. Reuses ABS linearization (lower-bounded ABS → exact Big-M envelope; requires finite bounds on `e`). **Watch for**: (1) the tolerance must exceed the solver feasibility tolerance (~1e-6) or the boundary violation (`= tol`) is swallowed and the link silently doesn't bite — set default `1e-4`, configurable via `decide_l0_tolerance` (floor `1e-5`); (2) the auto-M Big-M refill keys on "indicator + inner var" and would clobber the reverse link's fixed `tol` coefficient — it now skips links whose only non-indicator var is an ABS aux. Keeping the forward link exact leaves a pathological dead zone `(0, tol)` (a value forced there is reported infeasible). Tests: `test/decide/tests/test_norm.py::test_norm_l0_*` (lower-bound / equality / maximize infeasibility, both backends; the tolerance pragma). Reported by a teammate during norm review.

## Contradictory per-row equality bounds returned a wrong answer instead of infeasible

**Broke**: Bound absorption (`physical_decide.cpp`, `COMPARE_EQUAL` arm) set `lower_bounds[v] = upper_bounds[v] = bound_value` by **raw assignment** — last-writer-wins — while the `<=`/`>=` arms correctly intersect with `std::min`/`std::max`. So `x = 5 AND x = 10` silently returned `x = 10` (and `x = 10 AND x = 5` returned `x = 5`): a wrong optimum with no error, no diagnosis. Inequality conflicts (`x <= 5 AND x >= 10`) and aggregate equalities (`SUM(x) = 5 AND SUM(x) = 10`, which take the constraint-row path, not absorption) were already caught. The same missing-`std::max` on the `=` path was *noted* in the negative-lower-bounds entry above but only for the negative-survival angle — the conflict consequence was missed.

**Fix/lesson**: Intersect on the `=` arm too — `lower = std::max(lower, k)`, `upper = std::min(upper, k)` — so two equalities invert the box (`lower 10 > upper 5`), which the existing user-vs-user inverted-box path routes to the elastic engine for a least-change loosen. A single `x = -3` stays correct (the non-negativity floor guard only fires on `U < 0 AND L >= 0`, and `[-3,-3]` has `L < 0`). **Watch for**: any bound combiner that *assigns* instead of intersecting is a silent last-writer-wins conflict swallower — every `OP`-arm of an absorption must `min`/`max` against the running bound, never `=`. Tests: `test_query_diagnostics_relation.py::TestEqualityBoundConflict` (contradictory → infeasible; consistent/negative equality still solves, both backends). Found probing infeasible-diagnosis edge cases.

## Unsupported operator (`%`) inside a DECIDE clause crashed with INTERNAL

**Broke**: `ToSymbolicRecursive` (`decide_symbolic.cpp`) handles a fixed set of operators rendered as functions (`+ - * / ^ **`) and threw `InternalException("ToSymbolic: Unsupported operator function: %s")` on any other. A modulo in a coefficient position (`SUM(((id*7)%97)*x) <= 3`) aborted with `INTERNAL Error: … Unsupported operator function: %` and a C++ stack trace — making an unsupported feature look like a decidb bug. Same UX symptom that motivated the CASE-expression fix. (The `%` was *data-only* here — no decide var — so the coefficient is foldable, but there is no data-folding pass yet.)

**Fix/lesson**: Swapped the throw to a friendly `InvalidInputException` that names the offending operator and points at the pre-compute alternative ("The '%s' operator is not supported inside a DECIDE clause. Pre-compute it as a column in a subquery or CTE … and reference that column instead."). This is the floor fix; a future general fix could fold data-only subexpressions before symbolizing so any unsupported *data* operator evaluates as data instead of erroring — tracked in `03_expressivity/sql_functions/todo.md` ("Fold Data-Only Subexpressions Before Symbolizing"). **Watch for**: the same default-arm crash lurks in every operator/function/expression-class switch in the symbolic translator — an unsupported *user* expression must exit through `InvalidInputException`, never `InternalException`. Tests: `test/decide/tests/test_error_unsupported_operator.py` (rejection wording + no stack trace, in SUCH THAT and objective). Discovered building slow-diagnostics test instances.

**Superseded 2026-07-02 (data-only case)**: the data-folding pass now lands for operators — a data-only `%`/bitwise subexpression folds to a per-row coefficient and *succeeds* instead of erroring (binder allows it, `ToSymbolicRecursive` stores it in `data_map`, `FromSymbolic` restores it). The friendly `InvalidInputException` remains only for the variable-bearing case (`x % 97`). See `03_expressivity/sql_functions/done.md` ("Data-only operators the algebra doesn't model"). `test_error_unsupported_operator.py` was rewritten accordingly (data-only cases assert success, oracle-verified; variable-bearing case asserts the friendly error).
