# EXPLAIN Support for DECIDE

## Overview

The DECIDE operator supports all three forms of DuckDB's `EXPLAIN` mechanism:

- **`EXPLAIN`** — displays the query plan (logical or physical) with DECIDE node details
- **`EXPLAIN ANALYZE`** — executes the query and includes timing and row count profiling
- **`EXPLAIN (FORMAT JSON)`** — outputs the plan in JSON format

Both the `LogicalDecide` and `PhysicalDecide` operators override `GetName()` and `ParamsToString()` to produce structured output that DuckDB's plan renderer consumes.

---

## EXPLAIN Output Structure

The DECIDE node displays three sections:

1. **Variables** — user-declared decision variables (auxiliary variables are excluded)
2. **Objective** — `MAXIMIZE` or `MINIMIZE` followed by the objective expression, with WHEN/PER suffixes rendered the same way as constraints
3. **Constraints** — individual constraints, each on its own line, with WHEN/PER suffixes

The Objective and Constraints rows share **one** tagged-expression renderer, so an objective WHEN/PER prints symmetrically with a constraint WHEN/PER — e.g. `MAXIMIZE sum(x * price) WHEN (returnflag = 'R')`. (Earlier these diverged: the objective called `GetName()` directly on the wrapper, which short-circuited on its alias and leaked the raw internal tag `__when_constraint__` into the output.)

Example output for a basic knapsack query:

```
┌───────────────────────────┐
│           DECIDE          │
│    ────────────────────   │
│        Variables: x       │
│                           │
│         Objective:        │
│  MAXIMIZE sum((CAST(x AS  │
│      DECIMAL(18,0)) *     │
│      l_extendedprice))    │
│                           │
│        Constraints:       │
│ (x >= CAST(0 AS INTEGER)) │
│ (x <= CAST(1 AS INTEGER)) │
│    (CAST(sum((CAST(x AS   │
│      DECIMAL(18,0)) *     │
│  l_quantity)) AS DOUBLE) <│
│       = 100.0) WHEN       │
│  (l_returnflag = CAST('R' │
│        AS VARCHAR))       │
│                           │
│        ~12035 Rows        │
└─────────────┬─────────────┘
```

---

## AND-Tree Splitting

DuckDB binds multiple `SUCH THAT` constraints into a single `BoundConjunctionExpression` (AND-tree). Printing the root expression as one string would produce an unreadable single line. Instead, a recursive `CollectTaggedExpressionStrings` function traverses the tree:

1. **AND nodes** — recurse into each child
2. **WHEN wrappers** (`alias == WHEN_CONSTRAINT_TAG`) — extract the condition from `children[1]`, recurse into `children[0]`, and append ` WHEN <condition>` to each leaf
3. **PER wrappers** (`alias == PER_CONSTRAINT_TAG`) — extract column names from `children[1..N]`, recurse into `children[0]`, and append ` PER col` for one column or ` PER (col1, col2)` for multiple columns to each leaf
4. **Leaf nodes** (comparisons, aggregates) — emit `expr.GetName()`

This produces one line per constraint with WHEN/PER suffixes, e.g.:

```
(sum(x * weight) <= 50.0) WHEN (returnflag = 'R') PER department
```

The **objective** is rendered through the same `CollectTaggedExpressionStrings` walker rather than a direct `GetName()` call. An objective is also a WHEN/PER-tagged `BoundConjunctionExpression` when conditional, so reusing the walker peels those wrappers into postfix suffixes identically — keeping the Objective and Constraints rows symmetric. Calling `GetName()` directly on the wrapper would instead return its alias (the raw `__when_constraint__` tag), because `GetName()` short-circuits to the alias when one is set.

---

## EXPLAIN ANALYZE

`EXPLAIN ANALYZE` executes the query and reports per-operator timing and row counts. No manual instrumentation is needed — DuckDB's `PipelineExecutor` automatically measures time spent in each operator's `Sink`, `Finalize`, and `Source` phases.

Row counts in `EXPLAIN ANALYZE` show **actual** counts, not estimates. Since DECIDE is an annotation operator (it assigns values to every input row without filtering), the row count is the same across all nodes in the plan.

For small datasets, per-operator timing may display as `(0.00s)` due to two-decimal-place rounding. The `Total Time` at the top of the output is the reliable measurement.

---

## EXPLAIN (FORMAT JSON)

JSON output uses the same `GetName()` and `ParamsToString()` methods. It does **not** use `Serialize`/`Deserialize` — those are for prepared statement caching and plan storage, not EXPLAIN rendering.

---

## Cardinality Estimates

DECIDE does not override `EstimateCardinality`. The default behavior propagates the child's estimate upward, which is correct: DECIDE emits every input row (it assigns decision variable values, it does not filter). The `~N Rows` estimate in `EXPLAIN` and the `N Rows` count in `EXPLAIN ANALYZE` will match the scan's cardinality.

---

## Code Pointers

### Logical Layer

- **`GetName()` / `ParamsToString()`**: `src/planner/operator/logical_decide.cpp` — `ParamsToString()` renders both the Objective and Constraints rows through `CollectTaggedExpressionStrings`
- **`CollectTaggedExpressionStrings()`**: `src/planner/operator/logical_decide.cpp` — recursive tagged-expression walker that produces one string per constraint (or the single objective string) with WHEN/PER suffixes
- **Declaration**: `src/include/duckdb/planner/operator/logical_decide.hpp`

### Physical Layer

- **`GetName()` / `ParamsToString()`**: `src/execution/operator/decide/physical_decide.cpp` — duplicates the logical-layer rendering (both Objective and Constraints route through the walker); the default `physical_only` EXPLAIN uses this path
- **`CollectTaggedExpressionStringsPhysical()`**: `src/execution/operator/decide/physical_decide.cpp` — physical-layer variant of the same walker
- **Declaration**: `src/include/duckdb/execution/operator/decide/physical_decide.hpp`

### Tests

- **Python pytest (TPC-H)**: `test/decide/tests/test_explain.py` — 22 tests covering EXPLAIN on real TPC-H tables with WHEN/PER edge cases, including `test_explain_objective_when_postfix` which pins the objective-WHEN suffix rendering

---

## TODO: Layered constraint rendering (as-written → canonical → rewritten)

**Goal.** The Constraints section prints one form per constraint — whatever expression tree happens to sit on `LogicalDecide` at render time. That conflates three different things and gives the user no way to see what became of their query. Render all three layers:

1. **As written** — the user's own syntax, before any restructuring: `demand - sum(ship) <= cap`
2. **Canonical** — decision and reducer terms on the left, data on the right: `sum(ship) - cap >= -demand`
3. **Rewritten** — after `DecideOptimizer`: AVG→SUM, ABS/MIN/MAX linearization, McCormick, `<>` indicators, plus the auxiliary constraints those passes emit

**Why.** The gap between what a user writes and what the solver receives is invisible today. Someone who writes `MAX(x) <= K` and gets a cheap per-row rewrite sees the same EXPLAIN as someone who writes `MAX(x) >= K` and gets a Big-M encoding with an indicator variable per row. Layer 3 is also where auxiliary variables (`__abs_aux_0__`, `__minmax_y_3__`) first become explicable — they surface in diagnostics today with no account of where they came from.

**What already supports this.** After the canonicalization refactor (`07_issues/code_quality/todo.md`) there is exactly one point in the pipeline where layer 1 becomes layer 2. `DecideCanonicalizer::Canonicalize` is a **pure function** — it returns a new tree and leaves its input untouched. That is deliberate and is the seam this feature rests on: at each call site the pre-image and the canonical form are live locals at the same instant, so this becomes a rendering job rather than a re-plumbing job. An in-place mutator would destroy layer 1 at the moment of canonicalization. Layer 3 needs no new hook either: optimizer-emitted constraints all arrive through `LogicalDecide::AddConstraint`, and in-place rewrites substitute leaves rather than moving terms across the relation, so a snapshot taken at the end of `DecideOptimizer::Optimize` diffs cleanly against layer 2.

**The hard part — solve it when building, not before.** Associating a rendered line with its origin across the optimizer. The constraint set is not stable: `AddConstraint` appends (ABS envelopes, Big-M rows, McCormick) and `RewriteComposedMinMax` removes constraints from the tree entirely into `composed_minmax_constraints`. Layer 3 is therefore not a line-for-line image of layer 2, and positional association will drift. Needs a stable per-constraint identity, or a rendering model that tolerates one-to-many and one-to-none. Deliberately left unsolved: no provenance field was added to `LogicalDecide` during the canonicalizer refactor, since it would need `Serialize`/`Deserialize` support and would sit unused. Given the pure-function seam, adding it later is cheap.

**Scope notes.**

- Applies to the Objective row too — it shares the renderer (`CollectTaggedExpressionStrings`).
- Layers 2 and 3 are noise when neither canonicalization nor the optimizer touched a constraint. Collapse to one line in that case.
- `EXPLAIN (FORMAT JSON)` should carry the layers as structured fields, not one pre-joined string.
- The physical layer duplicates this rendering (`CollectTaggedExpressionStringsPhysical`) and would need the same treatment, or the duplication resolved first.
- `test/decide/tests/test_explain.py` asserts on the current single-form output and will need updating.

**Prerequisite**: the canonicalization refactor. Until canonicalization happens in one place, "the canonical form" is not a well-defined thing to render.

**Filed**: 2026-08-10, while designing the canonicalization refactor — deferred rather than folded into that work.
