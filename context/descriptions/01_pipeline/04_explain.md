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
