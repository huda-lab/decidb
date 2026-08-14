# EXPLAIN on a DECIDE query

`EXPLAIN`, `EXPLAIN ANALYZE` and `EXPLAIN (FORMAT JSON)` all work on a DECIDE
query. Both `LogicalDecide` and `PhysicalDecide` override `GetName()` and
`ParamsToString()` to give DuckDB's plan renderer a structured DECIDE node.

This is the *inspection* surface for a decision query, the sibling of
[`../diagnose/`](../diagnose/) — which reports on a solve that failed or was
useless, where this reports on the plan.

---

## 1. Node structure

The DECIDE node prints three sections:

1. **Variables** — user-declared decision variables. Auxiliary variables are excluded.
2. **Objective** — `MAXIMIZE` / `MINIMIZE` and the objective expression, with
   `WHEN` / `PER` suffixes.
3. **Constraints** — one line per constraint, with `WHEN` / `PER` suffixes.

```
┌───────────────────────────┐
│           DECIDE          │
│    ────────────────────   │
│        Variables: x       │
│                           │
│         Objective:        │
│   MAXIMIZE sum((x * v))   │
│                           │
│        Constraints:       │
│    __source_clause_0__    │
│                           │
│          ~3 Rows          │
└─────────────┬─────────────┘
```

The Constraints line above is **a bug, not the intended output** — see
[`todo.md`](todo.md). It should read `(sum((x * w)) <= 6)`.

---

## 2. One renderer for both rows

DuckDB binds several `SUCH THAT` constraints into a single
`BoundConjunctionExpression`. Printing that root as one string would produce an
unreadable line, so `CollectDecideExpressionStrings`
(`src/planner/operator/logical_decide.cpp:79`) walks it:

| Node | Handling |
|---|---|
| `PER` wrapper (`IsPerConstraintTag`, ≥2 children) | Build the suffix from `children[1..N]` — ` PER col`, or ` PER (col1, col2)` when there is more than one — recurse into child 0, append to every leaf |
| `WHEN` wrapper (`WHEN_CONSTRAINT_TAG`, 2 children) | Suffix ` WHEN <condition>` from child 1, recurse into child 0, append to every leaf |
| Plain `AND` | Recurse into each child |
| Leaf | `expr.GetName()` |

Producing, for example:

```
(sum(x * weight) <= 50.0) WHEN (returnflag = 'R') PER department
```

**The Objective row goes through the same walker**, not a direct `GetName()`. A
conditional objective is itself a `WHEN`/`PER`-tagged
`BoundConjunctionExpression`, so reusing the walker peels those wrappers into
postfix suffixes identically and keeps the two rows symmetric. Calling
`GetName()` on the wrapper instead returns its *alias* — the raw
`__when_constraint__` tag — because `GetName()` short-circuits to the alias
whenever one is set. That divergence was a real bug once; the shared walker is
what fixed it.

**There is no physical-layer duplicate.** `PhysicalDecide::ParamsToString`
(`physical_decide.cpp:1350, 1364`) calls the same `CollectDecideExpressionStrings`
declared in `logical_decide.hpp`. The default `physical_only` EXPLAIN therefore
renders through exactly the same code as the logical plan.

---

## 3. `EXPLAIN ANALYZE`

No manual instrumentation. DuckDB's `PipelineExecutor` measures time in each
operator's `Sink`, `Finalize` and `Source` phases automatically — which for DECIDE
usefully separates materialization, model building plus solve, and readback.

Row counts are **actual**, not estimates. DECIDE is an annotation operator — it
assigns values to every input row and filters nothing — so the count is the same
at every node in the plan.

On small datasets per-operator timing can display as `(0.00s)` from two-decimal
rounding; the `Total Time` at the top is the reliable figure.

---

## 4. `EXPLAIN (FORMAT JSON)`

Uses the same `GetName()` and `ParamsToString()`. It does **not** use
`Serialize` / `Deserialize` — those exist for prepared-statement caching, not for
rendering.

---

## 5. Cardinality

DECIDE does not override `EstimateCardinality`. The child's estimate propagates
upward, which is correct: DECIDE emits every input row. The `~N Rows` in `EXPLAIN`
and the `N Rows` in `EXPLAIN ANALYZE` match the scan's cardinality.

---

## 6. Source map

| Concern | Location |
|---|---|
| `GetName()` / `ParamsToString()`, the shared walker | `src/planner/operator/logical_decide.cpp` |
| Walker declaration | `src/include/duckdb/planner/operator/logical_decide.hpp:18` |
| Physical node rendering (calls the same walker) | `src/execution/operator/decide/physical_decide.cpp:1327-1378` |
| Tests | `test/decide/tests/test_explain.py` — 22 cases over TPC-H, including `test_explain_objective_when_postfix` |
