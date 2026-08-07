# Code Quality Issues — Open

Code-quality issues (duplication, dead code, fragile patterns, unclear naming, missing test coverage) spotted opportunistically while working on other tasks. Not bugs — nothing here produces wrong results today; these are things that make the code harder to change safely. Actual bugs go to `../bugs/todo.md`.

Each entry: short title, location (`file:line`), what's wrong, why it matters, and when/during which task it was discovered.

Resolved entries are removed; if the fix taught a generalizable lesson, record it in `.claude/lessons.md`.

---

## Hard-direction MIN/MAX has only a one-hot Big-M encoding, whose relaxation weakens with row count

**Location**: `src/execution/operator/decide/physical_decide.cpp:5394-5432` (flat hard MIN/MAX objective); the same encoding appears for composed terms via `EmitComposedHardMinMaxIndicators` and for nested-PER inner/outer levels.

The hard direction (`MAXIMIZE MAX`, `MINIMIZE MIN`) emits the textbook one-hot Big-M encoding: one indicator binary per active row, a linking row `z <= v_r + M*y_r` per row, and `SUM(y) >= n-1` so exactly one row binds.

The Big-M constant is not the weakness — `compute_big_m()` (line 4925) returns `global_max - global_min`, and since `z`'s own bound *is* the global extreme, a per-row constant would not be tighter. The **encoding** is the weakness: setting every `y_r = (n-1)/n` is LP-feasible and slackens the bound on `z` by `M*(n-1)/n`, which tends to `M` as `n` grows. The root relaxation therefore gets weaker the larger the instance, and branch-and-bound must close a gap that widens with row count.

**Why it matters**: this makes Q9 the least scalable query in the benchmark suite — measured 2026-07-26 at 5K 1.7s, 7.5K 5.3s, 15K 29.8s, 30K >60s, against a near-linear curve for every other MILP in the set. That ranking is an artifact of the single formulation we implement, not evidence that hard-MAX is intrinsically harder than the rest. It also caps `Q9_ROW_LIMIT` at 7.5K/15K while comparable queries run at 500K/1M.

**Fix direction**: DeciDB currently has no general-constraint, indicator-constraint, or SOS path at all — `src/decidb/` has zero hits for `genconstr`, `SOS`, or the indicator APIs, so Big-M is the only tool available anywhere in the codebase. Gurobi's `GRBaddgenconstrMax` / `GRBaddgenconstrMin` model `z = max/min(...)` natively and avoid the relaxation problem entirely. HiGHS has no equivalent, so this must be built as an **accelerator with the existing Big-M path as fallback** — the same pattern CLAUDE.md prescribes for diagnostics ("Gurobi-only APIs are accelerators, never dependencies"), not as a replacement. Introducing the general-constraint channel would also open `GRBaddgenconstrIndicator` for the `<>` disjunctions and ABS linearization, which use Big-M for the same reason.

**Discovered**: 2026-07-26, while raising benchmark scale limits and asking which limits are inherent problem complexity versus our formulation. Ruled out as *not* the cause in the sibling case: Q3's L0 Big-M looseness (`norm(adj, 0, 40)` against a tight bound of 20) measured identical to the tight and inferred variants at both 60K and 120K, so Gurobi presolve repairs a loose user-supplied constant. Big-M *tightness* is handled; Big-M *encoding* is not.

---

## BOOLEAN domains round-trip through the constraint tree instead of being variable bounds

**Location**: `src/planner/binder/query_node/bind_select_node.cpp:1196-1220` (injection); `src/execution/operator/decide/physical_decide.cpp:2092-2110` (`ExtractVariableBounds` / `TraverseBoundsConstraints`, recovery).

A `IS BOOLEAN` declaration does not set a variable's bounds. The binder synthesizes `x >= 0 AND x <= 1` as ordinary `ComparisonExpression`s and prepends them to the `SUCH THAT` tree, so the `0/1` box travels through every downstream stage as constraints. At model build, `TraverseBoundsConstraints` walks the tree to recognize those same comparisons and fold them back into `col_lower`/`col_upper`. The domain therefore makes a round trip: declaration to constraint rows to bounds.

**Why it matters**: bounds and constraints are different things to a solver, and conflating them costs on three fronts. The rewrite passes carry constraint nodes that carry no information beyond the declared type. The recovery is pattern-matching on expression shape, so any rewrite that reassociates or wraps those comparisons silently drops the variable back to its default box. And the diagnostics engines treat constraint rows as loosenable, so a synthetic `x <= 1` is structurally indistinguishable from a limit the user wrote — the elastic program should never offer to loosen a variable's declared type. A `DomainSpec` carried on the variable and applied directly to `col_lower`/`col_upper` would remove all three.

**Discovered**: 2026-07-28, while tracing the binder to answer reviewer questions on the paper's architecture section (what the binder resolves versus where variable domains are actually fixed). The paper claim "the binder turns type declarations into variable domains" is not accurate against this code path, which is what surfaced it.

---

## Symbolic layer's architecture comment names the wrong library (SymEngine vs SymbolicC++)

**Location**: `src/decidb/symbolic/decide_symbolic.cpp:20,32,39,49,61` (file header comment); also `src/planner/binder/query_node/bind_select_node.cpp:449`.

The header comment that documents the normalizer's four paths attributes the algebra to "SymEngine" five times ("The default normalizer uses SymEngine (`expand().simplify()`)", "SymEngine `.expand()` would distribute the square", etc.). The vendored library is **SymbolicC++** — `third_party/symboliccpp`, included as `symbolicc++.h:81`. There is no SymEngine in the tree; the only two files matching the string are these comments.

**Why it matters**: this comment is the primary architecture documentation for the normalizer and is what anyone reads before touching the four-path structure. Naming the wrong CAS misleads on capability (SymEngine and SymbolicC++ have different APIs and simplification strength), and it is a live citation hazard — the CIDR'27 architecture section is being written from these comments, and a wrong library name would ship in the paper.

**Discovered**: 2026-07-30, while explaining the symbolic normalization layer for the paper's §3.2 (parser/binder split).

---

## Canonicalization is split across two stages, in two representations

**Location**: `src/decidb/symbolic/decide_symbolic.cpp:1272-1400` (`NormalizeComparisonExpr`, binder-time, over `ParsedExpression`); `src/execution/operator/decide/physical_decide.cpp:1610-1642` (per-row constraint extraction, model-build time, over bound `Expression`).

Putting a constraint into `Ax <= b` shape — decision terms on the left, data on the right — happens in two different places, at two different stages, over two different expression types.

The binder's normalizer handles only a narrow slice. It returns the constraint untouched unless all three guards pass (`decide_symbolic.cpp:1274-1287`): the comparison is `<`, `<=`, `>`, or `>=` (**equality is never normalized**), the RHS is a numeric constant, and the LHS contains a `SUM`. Three further structural bypasses return `cmp.Copy()` for quadratic LHS, composed `MIN`/`MAX`, and aggregate-local `WHEN`.

Everything the binder declines is re-partitioned at model build. `physical_decide.cpp:1614` collects decision references on the RHS, pushes them into `lhs_terms` with negated sign, zeroes them out of the RHS via `StripDecideVars`, and moves the LHS's data part to the bound as `lhs_offset_expr`. That is the same left/right migration the binder's normalizer performs, reimplemented against bound expressions.

**Why it matters**: there is no single point in the pipeline that answers "what shape is a constraint in." The binder's output is canonical for aggregate constraints with a literal bound and non-canonical for everything else, so every consumer downstream must handle both. The sign-flip and offset-migration logic exists twice, in two representations, and a fix to one does not reach the other. It also makes the stage boundary undescribable: the natural sentence "the binder puts decision terms on the left and data on the right" is true of the system but false of the binder, which is what surfaced this.

**Fix direction — pick one home.** Two coherent end states:

1. *All in the binder*, over `ParsedExpression`, before binding. The physical extractor then assumes canonical input and only reads terms off. This requires the binder to handle decision-bearing RHS, equality, and bare per-row constraints, and it requires replacing the three structural bypasses with a classification-driven normalizer — which the file's own `REFACTOR TRIPWIRE` comment (`decide_symbolic.cpp:72-76`) already identifies as the right move once a fifth path is needed. This is the option the paper's §3.2 assumes.
2. *All at model build*, deleting the binder normalizer. Simpler, but gives up canonical form for the optimizer passes that run between binding and execution, and moves an algebraic concern into the execution operator.

Option 1 is preferred: canonical form is a property of the query, not of one execution strategy, and the optimizer rewrites in `src/optimizer/decide/decide_optimizer.cpp` read constraints between the two stages.

**Test**: a parity suite over constraint shapes — equality, decision-on-RHS, per-row, quadratic, composed `MIN`/`MAX`, aggregate-local `WHEN` — asserting the same solver rows regardless of which stage does the partition. This must exist before either migration starts; the current split has no test that pins the two paths to the same result.

**Discovered**: 2026-07-30, writing paper §3.2. The binder subsection needed a canonicalization example, and the paper's running example (`demand - sum(ship) <= max_shortfall`, Example 1 line 11) turned out to be one the binder's normalizer refuses — its RHS is a decision variable, so the rewrite happens in the physical operator instead.
