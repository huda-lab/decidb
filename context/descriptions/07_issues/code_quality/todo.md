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

## BOOL domains round-trip through the constraint tree instead of being variable bounds

**Location**: `src/planner/binder/query_node/bind_select_node.cpp:1196-1220` (injection); `src/execution/operator/decide/physical_decide.cpp:2092-2110` (`ExtractVariableBounds` / `TraverseBoundsConstraints`, recovery).

A `BOOL` declaration does not set a variable's bounds. The binder synthesizes `x >= 0 AND x <= 1` as ordinary `ComparisonExpression`s and prepends them to the `SUCH THAT` tree, so the `0/1` box travels through every downstream stage as constraints. At model build, `TraverseBoundsConstraints` walks the tree to recognize those same comparisons and fold them back into `col_lower`/`col_upper`. The domain therefore makes a round trip: declaration to constraint rows to bounds.

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

---

## LogicalDecide's serialization has silently diverged from its generator

**Location**: `src/storage/serialization/serialize_logical_operator.cpp:404-540` (`LogicalDecide::Serialize` / `Deserialize`) vs. `src/include/duckdb/storage/serialization/logical_operator.json` (the `LogicalDecide` entry).

The `.cpp` carries the banner *"This file is automatically generated by scripts/generate_serialization.py — Do not edit this file manually, your changes will be overwritten"*, and it is genuinely generated from the JSON spec. But the JSON's `LogicalDecide` member list **stops at field 215** (`per_inner_was_avg`), while the `.cpp` hand-writes fields **216 through 233**: `variable_scopes`, the flattened `entity_scopes` (aliases, table indices, binding counts, binding tables/cols, var counts), the MIN/MAX link vectors, `aux_var_expressions`, and `variable_scope_kinds`.

Those hand-written fields include loops and comments no generator would emit — e.g. the "Flatten entity_scopes into parallel vectors for serialization" block.

**Why it matters**: running `python3 scripts/generate_serialization.py` regenerates the file from the JSON and **drops every DECIDE field from 216 up**. The failure is silent at build time — the file still compiles, because it only stops writing/reading properties — and shows up as a plan that round-trips with no entity scopes, no variable scopes, and no auxiliary-variable metadata. Anyone touching an unrelated serialized operator and regenerating would trigger it without any signal that DECIDE was involved.

**Fix direction**: move fields 216-233 into `logical_operator.json` so the generator owns them. Types that the generator cannot express directly (`vector<EntityScopeInfo>`, `vector<DecideVarScopeInfo>`) should be added as the same flattened parallel primitive vectors the hand-written code already uses, so the emitted code is equivalent to what is there today. If the generator genuinely cannot express them, the fallback is a loud one: a comment block at the top of the `LogicalDecide` JSON entry *and* at the hand-written section naming the hazard, so regeneration is a deliberate act.

**Test**: a plan round-trip assertion for a DECIDE query with a table-scoped and a query-wide variable, asserting `entity_scopes` and `variable_scopes` survive serialize→deserialize. There is no such test today, which is why the drift went unnoticed.

**Discovered**: 2026-08-08, implementing A3 (`scalar` query-wide decisions). Adding a scope field meant editing this file, and the JSON turned out not to mention the field being edited. Not triggered by that work — the new field 233 was added in the same hand-written style as its neighbours — but the next regeneration would take the whole DECIDE block with it.

---

## Declaring DECIDE in both clause slots reports the wrong fix

**Location**: `third_party/libpg_query/grammar/statements/select.y` (the `decide_declaration` / `decide_body` slots and `makeDecideClause()` in `grammar/grammar.cpp`).

A query that declares in both slots —

```sql
SELECT c_custkey, x DECIDE x(BOOL) FROM customer DECIDE x(BOOL)
SUCH THAT SUM(x) <= 3 MAXIMIZE SUM(x * c_acctbal)
```

— is correctly rejected, but the message is `DECIDE requires a SUCH THAT clause; add SUCH THAT with at least one constraint`, pointing at the first `DECIDE`. The declaration slot closes when the second `DECIDE` appears where `SUCH THAT` was expected, so the missing-`SUCH THAT` production fires.

**Why it matters**: the message names a fix the user already applied — the query *does* have a `SUCH THAT`. Per the project's user-facing output principle, the message should name the offending object (the duplicate declaration) and the smallest edit (drop one of the two `DECIDE` lists). As written it sends the reader looking for a clause that is already there.

**Fix direction**: detect the two-slot case where it is actually knowable — either a grammar production that accepts a `DECIDE` token in the body position purely to raise a dedicated message, or a check in `makeDecideClause()` if both slots can reach it. The four existing reject-only productions for the removed declaration spellings are the pattern to follow.

**Test**: `test/decide/tests/test_clause_order.py::TestClauseOrderErrors::test_declaration_in_both_slots_rejected` pins the rejection today but deliberately asserts only `parser error`, not the wording. Tighten that regex when the message is fixed.

**Discovered**: 2026-08-08, writing the clause-order regression tests while confirming group A of `context/descriptions/todo.md` was complete.

---

## Aggregate-local WHEN on a qualified reducer: rejected in constraints, works in objectives, and the message leaks an internal tag

**Location**: the `SUCH THAT` binder path (`src/planner/expression_binder/decide_constraints_binder.cpp`) versus `decide_objective_binder.cpp`; tag constants in `src/include/duckdb/common/enums/decide.hpp`.

`SUM(D: expr) WHEN (cond)` binds in an **objective** and behaves correctly — both masks apply, and the qualifier survives (pinned by `test_qualified_reducer.py::test_qualified_reducer_with_aggregate_local_when_in_objective`). The same expression in a **constraint** is rejected:

```
Binder Error: SUCH THAT clause does not support '(sum(keep) __qualified_reducer__ n)'(ExpressionClass::FUNCTION)
```

Two problems in one.

1. **The asymmetry is undocumented and probably unintended.** `done.md` describes the qualifier mask as ANDed into the same `TermFilterState` slot aggregate-local `WHEN` already uses, which is a description of a composition that works. It works on one side only.
2. **The message leaks `__qualified_reducer__`**, an internal alias tag, plus a C++ enumerator name (`ExpressionClass::FUNCTION`). Per the project's user-facing output principle this should name the SQL construct and the smallest edit — something like "a relation-qualified reducer cannot carry a WHEN filter in SUCH THAT; move the condition into the WHERE clause". The tag is an implementation detail of how the binder marks the aggregate; it has no meaning to a SQL user.

**Why it matters**: the leak is the more visible of the two — any user who writes this shape sees an internal identifier. The asymmetry means the same expression is legal in one clause and not the other, which is the kind of rule users cannot infer.

**Test**: `test/decide/tests/test_qualified_reducer.py::test_qualified_reducer_with_aggregate_local_when_in_constraint_rejected` pins the current rejection and matches only on `SUCH THAT clause does not support`, so tightening the message will not break it. Widen the assertion when the message is fixed; delete the test and add a positive one if the composition is made to work.

**Discovered**: 2026-08-08, writing the deferred test coverage for group A.
