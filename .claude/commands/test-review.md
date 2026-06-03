# /test-review — DECIDE Test Coverage Gap Finder

Systematically audits test coverage against the full DECIDE expressivity surface. Answers the question: "Are we testing everything a user might want to express?" Finds missing scenarios, untested feature interactions, and edge cases that could silently break.

## Arguments

`$ARGUMENTS` — optional scope:
- *(empty)* — full audit across all feature areas
- a feature name (e.g., `per`, `when`, `min_max`, `entity_scope`, `quadratic`) — audit only that area
- `interactions` — focus specifically on cross-feature interaction gaps (WHEN+PER, QP+WHEN, entity_scope+PER, etc.)
- `errors` — focus on error handling coverage (are all invalid inputs properly rejected?)

## Procedure

### 1. Parse arguments and determine scope

Parse `$ARGUMENTS` into a scope. If empty, audit everything.

### 2. Gather the expressivity surface

Read these docs to build the complete picture of what's expressible:

- `context/descriptions/03_expressivity/README.md` — feature index
- `context/descriptions/03_expressivity/decide/done.md` — variable types, table-scoped vars
- `context/descriptions/03_expressivity/such_that/done.md` — constraint operators, subqueries
- `context/descriptions/03_expressivity/when/done.md` — conditional constraints/objectives
- `context/descriptions/03_expressivity/per/done.md` — grouped constraints/objectives
- `context/descriptions/03_expressivity/maximize_minimize/done.md` — objective types, nesting, QP
- `context/descriptions/03_expressivity/sql_functions/done.md` — SUM, AVG, MIN, MAX, ABS
- `context/descriptions/03_expressivity/problem_types/done.md` — LP, ILP, MILP, QP, MIQP
- `.claude/CLAUDE.md` — syntax reference (condensed spec)

Also read `context/descriptions/04_optimizer/rewrite_passes/done.md` to understand all algebraic rewrites — each rewrite is a behavior that needs testing.

If scoped to a specific feature, only read the relevant subset.

### 2.5. Gather the prior coverage audit

`context/descriptions/05_testing/` is the canonical tracker of what is and isn't tested. Read it before touching the test files — it captures prior audits so you don't redo work or re-report known gaps.

Always read:
- `context/descriptions/05_testing/README.md` — area index, coverage quality levels (oracle / constraint-only / xfail / error), risk definitions, audit history

Then, for the scoped feature (or every area if scope is empty), read:
- `context/descriptions/05_testing/<area>/done.md` — what the prior audit *claims* is tested. Verify before trusting; it can go stale as tests are renamed or removed.
- `context/descriptions/05_testing/<area>/todo.md` — known gaps. Do NOT re-report these as new findings; instead, confirm they are still valid and note any that have since been addressed.

Map area names ↔ test files via the table in `05_testing/README.md`.

### 3. Verify prior coverage against actual test files

This step is a *cross-check*, not a from-scratch catalog. For each entry in the relevant `done.md`:

- Find the matching test in `test/decide/tests/`. If none exists, flag as a **stale `done.md` entry**.
- If the test exists but covers something different from what `done.md` claims, flag as **misdescribed coverage**.

Then scan `test/decide/tests/` for any test not represented in `done.md` — that is **undocumented coverage** worth folding back into the tracker.

Also read:
- `test/decide/pytest.ini` — markers (coverage categories)
- `test/decide/conftest.py` — fixtures and infrastructure

Only after this verification pass do you have a trustworthy view of current coverage to compare against the expressivity surface.

### 4. Spawn parallel audit agents

Launch up to 3 agents in parallel.

---

**Agent 1 — Single-Feature Gap Finder**

> You are auditing DeciDB's DECIDE test suite for **gaps in single-feature coverage**. For each DECIDE feature, check whether all documented behaviors are tested.
>
> **Start with the prior audit**: read `context/descriptions/05_testing/README.md` and the relevant area's `done.md` + `todo.md`. Treat `done.md` as the prior claim of coverage (verify against test files before trusting), and `todo.md` as known gaps — don't re-report these as new findings, only confirm they still apply.
>
> **Then read the expressivity docs** (listed above) to understand every feature's documented behavior, and **read the relevant test files** in `test/decide/tests/`. Report only *new* gaps not already in `todo.md`, plus any `done.md` entries that appear stale. For each feature, check:
>
> 1. **Variable types**:
>    - IS BOOLEAN: tested with all constraint types? With objectives? With WHEN? With PER?
>    - IS INTEGER: same checks
>    - IS REAL: same checks. Are fractional solutions tested (not just integer-valued)?
>    - Multiple variables: tested with different types in the same query? (e.g., `DECIDE x IS BOOLEAN, y IS REAL`)
>    - Table-scoped variables (`DECIDE Table.var`): tested with all constraint types? With WHEN? With PER? Mixed with row-scoped vars?
>
> 2. **Constraint operators** (each of `=`, `<`, `<=`, `>`, `>=`, `<>`, `BETWEEN`, `IN`):
>    - Tested on per-row constraints?
>    - Tested on aggregate (SUM) constraints?
>    - Tested with negative coefficients?
>    - Tested with zero RHS?
>    - `<>`: tested with the Big-M indicator rewrite?
>    - `IN`: tested on decision variables (not just columns)?
>
> 3. **Aggregates** (SUM, AVG, MIN, MAX):
>    - Each aggregate tested in constraints AND objectives?
>    - AVG: tested with uneven group sizes (where AVG ≠ SUM behavior)?
>    - MIN/MAX: all easy cases tested? All hard cases tested? Both constraint and objective usage?
>
> 4. **WHEN clause**:
>    - Tested on per-row constraints, aggregate constraints, and objectives separately?
>    - Compound conditions (AND, OR)?
>    - WHEN that filters ALL rows (empty result)?
>    - WHEN with string equality, numeric comparison, NULL handling?
>
> 5. **PER clause**:
>    - Single column, multi-column?
>    - PER on constraints with all aggregate types?
>    - PER on objectives with nested aggregates (SUM(SUM(...)), MIN(MAX(...)), etc.)?
>    - Groups of different sizes? Single-row groups? Empty groups after WHEN filtering?
>
> 6. **Quadratic Programming**:
>    - All three syntax forms: `POWER(expr, 2)`, `expr ** 2`, `(expr) * (expr)`?
>    - With WHEN? With PER? With multiple variables?
>    - MAXIMIZE rejection tested?
>    - Mixed linear + quadratic objectives?
>
> 7. **Error handling**:
>    - Every documented restriction/limitation has a test that verifies the error is raised?
>    - Nonlinear constraint (`x * y`) rejected?
>    - Invalid aggregate rejected?
>    - PER on flat MIN/MAX objective rejected?
>    - MAXIMIZE with quadratic rejected?
>
> **Scope**: {scope_description}
>
> **Output format**:
> ```
> ## Single-Feature Coverage Gaps
>
> ### [Feature Name]
> **Tested**: [summary of what IS tested]
> **New gaps** (not in `todo.md`):
> - [scenario not tested] — why it matters: [what could break undetected]
> **Stale `done.md` entries** (claim coverage but no test found):
> - [entry] — claimed in `05_testing/[area]/done.md`, no matching test in `[file]`
> **`todo.md` entries to retire** (now actually covered):
> - [entry] — covered by `[test_file::test_name]`
> ```

---

**Agent 2 — Interaction Gap Finder**

> You are auditing DeciDB's DECIDE test suite for **cross-feature interaction gaps**. Many bugs live at the intersection of two features. Your job is to find feature combinations that are NOT tested together.
>
> **Start with the prior audit**: read `context/descriptions/05_testing/README.md` and the `done.md` + `todo.md` for each area touched by the interaction matrix below. `done.md` lists prior-claimed coverage (verify before trusting); `todo.md` lists known gaps — don't re-report those, only confirm they still apply.
>
> **Then read the relevant test files** in `test/decide/tests/` and **the expressivity docs** to understand all features. Report only *new* interaction gaps not already in any `todo.md`, plus any `done.md` interaction claims that are stale. Then systematically check the following interaction matrix:
>
> For each pair of features, check if there is at least one test that combines them:
>
> | Feature A | Feature B | What to look for |
> |-----------|-----------|------------------|
> | WHEN | PER | Conditional grouped constraint — does WHEN filter interact correctly with PER grouping? |
> | WHEN | MIN/MAX | Conditional MIN/MAX — does WHEN work on linearized MIN/MAX constraints? |
> | WHEN | AVG | Conditional AVG — does WHEN interact with AVG→SUM scaling? |
> | WHEN | ABS | Conditional ABS — does WHEN work on linearized ABS constraints? |
> | WHEN | QP | Conditional quadratic objective |
> | WHEN | `<>` | Conditional not-equal — WHEN + Big-M disjunction |
> | WHEN | entity_scope | Conditional with table-scoped variables |
> | PER | MIN/MAX | Grouped MIN/MAX — nested aggregate linearization |
> | PER | AVG | Grouped AVG — coefficient scaling per group size |
> | PER | ABS | Grouped ABS constraints |
> | PER | `<>` | Grouped not-equal constraints |
> | PER | entity_scope | Grouped constraints with table-scoped variables |
> | PER | QP | Grouped quadratic objective |
> | entity_scope | MIN/MAX | Table-scoped variables with MIN/MAX linearization |
> | entity_scope | AVG | Table-scoped variables with AVG |
> | entity_scope | multiple vars | Mixed table-scoped + row-scoped variables |
> | entity_scope | QP | Table-scoped variables with quadratic objectives |
> | QP | multiple vars | Quadratic with multiple decision variables |
> | multiple vars | PER | Multiple variables with grouped constraints |
> | multiple vars | WHEN | Multiple variables with conditional constraints |
> | BOOLEAN + INTEGER | same query | Mixed variable types in constraints/objectives |
> | BOOLEAN + REAL | same query | Mixed variable types |
> | subquery | WHEN | Subquery RHS with conditional constraint |
> | subquery | PER | Subquery RHS with grouped constraint |
> | JOIN | PER | JOIN source with grouped constraints |
> | JOIN | entity_scope | JOIN source with table-scoped variables |
> | JOIN | WHEN | JOIN source with conditional constraints |
>
> Also check for **triple interactions** (three features combined):
> - WHEN + PER + MIN/MAX
> - WHEN + PER + entity_scope
> - WHEN + PER + multiple variables
> - entity_scope + PER + WHEN
> - QP + WHEN + multiple variables
>
> **Scope**: {scope_description}
>
> **Output format**:
> ```
> ## Interaction Coverage Matrix
>
> ### TESTED (confirmed by existing test)
> | Feature A | Feature B | Test File | Test Name |
> |-----------|-----------|-----------|-----------|
>
> ### NOT TESTED (gap)
> | Feature A | Feature B | Risk | Example Query That Would Test It |
> |-----------|-----------|------|----------------------------------|
>
> ### TRIPLE INTERACTIONS
> | Features | Tested? | Risk |
> |----------|---------|------|
>
> ### Stale `done.md` entries (claim coverage but no test found)
> | Area | Claim | File checked |
> |------|-------|--------------|
> ```

---

**Agent 3 — Edge Case & Data Shape Auditor**

> You are auditing DeciDB's DECIDE test suite for **edge cases and unusual data shapes** that could expose bugs. Your focus is on boundary conditions, degenerate inputs, and realistic usage patterns that are easy to overlook.
>
> **Start with the prior audit**: read `context/descriptions/05_testing/README.md`, `05_testing/edge_cases/done.md` and `05_testing/edge_cases/todo.md` (plus any other area `done.md`/`todo.md` whose edge cases overlap your scope). `done.md` claims prior coverage; `todo.md` tracks known gaps — don't re-report those.
>
> **Then read the test files** in `test/decide/tests/` (especially `test_edge_cases.py`) and the expressivity docs. Report only *new* gaps not in `todo.md`, plus any `done.md` entries that look stale. Then check if these scenarios are tested:
>
> **Boundary conditions**:
> - Zero rows matching (empty result after WHERE or WHEN filters everything)
> - Single row (trivial problem — should still work)
> - All decision variables forced to same value by constraints
> - Infeasible problem (constraints contradict each other)
> - Unbounded problem (no objective bounds)
> - All-zero coefficients in objective
> - Negative coefficients in constraints and objectives
> - Very large coefficients (numeric stability)
> - RHS = 0 for constraints
> - Variables appearing in constraints but NOT in objective
> - Variables appearing in objective but NOT in constraints (unconstrained)
>
> **Data shapes**:
> - Single group in PER (entire table is one group)
> - PER where one group has 1 row and another has 1000 rows (asymmetric)
> - WHEN that filters to exactly 1 row in a group
> - WHEN that filters out an entire PER group (group becomes empty)
> - NULL values in PER columns (group key is NULL)
> - NULL values in WHEN condition columns
> - NULL values in constraint coefficient columns
> - Duplicate values in PER columns (many rows per group)
> - JOIN that produces duplicate rows (fan-out)
>
> **Realistic stress patterns**:
> - Multiple WHEN conditions on different constraints in the same query
> - Multiple PER constraints with different grouping columns
> - Constraint where LHS and RHS both contain decision variables
> - Objective with many terms (10+ columns multiplied by decision variable)
> - Query with 5+ constraints of mixed types (per-row + aggregate + PER)
> - Subquery that returns exactly one row vs multiple rows
> - Correlated subquery referencing decision variable (should this work? is it tested either way?)
>
> **Solver-specific**:
> - The oracle is Gurobi-only by design (see `02_operations/oracle.md`); do not flag "no HiGHS oracle backend" as a gap. Do flag any test that still encodes an analytical / hand-computed closed-form expected value — those are forbidden.
> - DeciDB's CLI-side solver dispatch is Gurobi-preferred with HiGHS as a fallback. Are the HiGHS-rejection paths tested (QP with INTEGER via HiGHS, non-convex bilinear on HiGHS, quadratic constraints on HiGHS — each should emit a clear "requires Gurobi" error)?
>
> **Scope**: {scope_description}
>
> **Output format**:
> ```
> ## Edge Case & Data Shape Audit
>
> ### TESTED (found in test suite)
> - [scenario] — tested in [test_file::test_name]
>
> ### NOT TESTED (gap)
> - [scenario] — risk: [what could break] — example: [brief SQL or description]
>
> ### UNCLEAR (couldn't determine)
> - [scenario] — [why it's unclear]
>
> ### Stale `done.md` entries
> - [entry] — claimed in `05_testing/[area]/done.md`, no matching test
>
> ### `todo.md` entries to retire (now actually covered)
> - [entry] — covered by `[test_file::test_name]`
> ```

---

### 5. Synthesize findings

Once all three agents return, merge their results:

1. **Collect** all gaps from all agents
2. **Deduplicate**: if two agents flag the same missing scenario, merge
3. **Prioritize** by risk using the tier definitions in `context/descriptions/05_testing/README.md` (§ "Risk priorities for gaps"). Use the same HIGH/MEDIUM/LOW vocabulary the tracker uses so the report and the `todo.md` files stay in sync.
4. **Group** by feature area for easy navigation

### 6. Present the report

```
# Test Coverage Gap Report
**Scope**: [full / feature name] | **Date**: [date]
**Tests Audited**: [count] across [file count] files
**Coverage Surface**: [brief description of what was checked against]

## Summary
[2-3 sentences: overall coverage health, biggest gap areas, estimated number of new tests needed]

## High-Risk Gaps ([count])
These gaps could hide silent correctness bugs — prioritize first.

| # | Gap | Feature(s) | Risk | Example Query |
|---|-----|------------|------|---------------|
| 1 | ... | ...        | ...  | ```sql ...``` |

## Medium-Risk Gaps ([count])
Documented features or error cases without coverage.

| # | Gap | Feature(s) | Risk |
|---|-----|------------|------|

## Low-Risk Gaps ([count])
Edge cases and unlikely scenarios.

| # | Gap | Feature(s) |
|---|-----|------------|

## Interaction Matrix Summary
[Which feature pairs are well-tested, which have zero coverage]

## Cross-Cutting Observations
[Patterns — e.g., "entity_scope has almost no interaction tests", "QP is only tested standalone"]

## Recommended New Tests (prioritized)
1. **[test name]** — Tests [what]. Add to `test/decide/tests/[file]`.
   ```sql
   -- Example query this test should run
   SELECT ... FROM ... DECIDE ... SUCH THAT ... MINIMIZE ...
   ```
2. **[test name]** — Tests [what].
   ```sql
   ...
   ```
3. ...
```

### 6.5. Offer to update the tracker

Before writing any tests, offer to fold this audit's findings back into `context/descriptions/05_testing/`:

- Append newly found gaps to the relevant `<area>/todo.md` files, grouped by the same HIGH/MEDIUM/LOW tiers used in the report.
- Remove or correct any stale entries flagged in `<area>/done.md`.
- Move any `todo.md` items confirmed as now-covered into `done.md`.
- Add a new line to the audit history at the bottom of `05_testing/README.md` with today's date and a one-line summary of scope + headline finding.

Ask the user before writing — if they decline, the report still stands as an ephemeral artifact and the tracker is left untouched.

### 7. Offer to write tests

After (optionally) updating the tracker, ask: "Want me to write these tests? I'll start with the high-risk gaps and work down."

If the user says yes, write the tests following the existing pattern:
- Oracle-verified only. Every correctness test must formulate the same problem in gurobipy via `oracle_solver` and compare via `comparison.compare.compare_solutions`. Analytical / hand-computed closed-form assertions are forbidden (see `context/descriptions/05_testing/README.md`).
- The flow: `decidb_cli.execute(sql)` → `(rows, cols)` from DeciDB; `duckdb_conn.execute(...)` → source data (vanilla duckdb against `_tpch_oracle.duckdb`); build the ILP via `oracle_solver` using helpers in `tests/_oracle_helpers.py`; call `compare_solutions(..., coeff_fn=...)` — or `decidb_objective_fn=...` for non-linear (QP/QCQP) objectives.
- For discrete constructs (`<>`, IN), use Gurobi native indicator constraints (`add_indicator_constraint` or the matching helpers). Do not mirror DeciDB's Big-M rewrite — independent semantics is the point.
- Follow naming conventions: `test_<feature>_<scenario>`
- Add appropriate pytest markers
- Place in the correct test file (or create a new one if a new category)
- After each test passes, move the corresponding entry from `<area>/todo.md` → `<area>/done.md` in the same session so the tracker stays in sync.
