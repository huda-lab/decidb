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
│      MAXIMIZE SUM(x * v)  │
│                           │
│        Constraints:       │
│      SUM(x * w) <= 6      │
│                           │
│          ~3 Rows          │
└─────────────┬─────────────┘
```

Each user-clause line is rendered in the spelling the user can edit. That is not
what the bound tree says — `Expression::ToString()` would print
`(sum((CAST(x AS DECIMAL(13,0)) * w)) <= CAST(6 AS HUGEINT))`, spelling out casts
the binder inserted while reconciling types and arithmetic as
`"-"("*"(a, b), c)`. Optimizer-emitted mechanism rows may also appear in the same
section with their internal variable names; section 2 covers the shared renderer.

---

## 2. One renderer, four surfaces

DuckDB binds several `SUCH THAT` constraints into a single
`BoundConjunctionExpression`. Printing that root as one string would produce an
unreadable line, so `CollectDecideExpressionStrings`
(`src/planner/decide/decide_source_provenance.cpp`) walks it:

| Node | Handling |
|---|---|
| `PER` wrapper (`IsPerConstraintTag`, ≥2 children) | Build the suffix from `children[1..N]` — ` PER col`, or ` PER (col1, col2)` when there is more than one — recurse into child 0, append to every leaf |
| `WHEN` wrapper (`WHEN_CONSTRAINT_TAG`, 2 children) | Suffix ` WHEN <condition>` from child 1, recurse into child 0, append to every leaf |
| Plain `AND` | Recurse into each child |
| Leaf | `RenderDecideSource` |

Producing, for example:

```
SUM(x * weight) <= 50.0 WHEN returnflag = 'R' PER department
```

**The Objective row goes through the same walker**, not a direct `GetName()`. A
conditional objective is itself a `WHEN`/`PER`-tagged
`BoundConjunctionExpression`, so reusing the walker peels those wrappers into
postfix suffixes identically and keeps the two rows symmetric. Calling
`GetName()` on the wrapper instead returns its *alias* — the raw
`__when_constraint__` tag — because `GetName()` short-circuits to the alias
whenever one is set. That divergence was a real bug once; the shared walker is
what fixed it.

### Leaves render by authorship, not by type

`RenderDecideSource` is the project's single user-facing expression renderer,
documented in `decide_source_provenance.hpp` and described in
[`../../01_pipeline/03_logical_plan/done.md`](../../01_pipeline/03_logical_plan/done.md).
It replaces `ToString()` because the bound tree is not the query: it carries
casts the user cannot edit, prints functions in call form, and stores pipeline
metadata (source clause, `WHEN`/`PER` role, reducer scope) in expression aliases
that `GetName()` would print verbatim as `__source_clause_0__`.

The rule that separates a cast the user typed from one the binder added is
**authorship, not type**. `TagDecideSourceFragments` records the written spelling
of every cast and scalar subquery before binding can obscure it, so a tagged node
replays its own SQL and an untagged one is dropped. `SUM(x * CAST(q AS INTEGER))`
therefore keeps its cast — that is legal SQL, since the binder rejects a cast only
over a *decision* — while the reconciliation casts around `x` and `q` do not
appear at all.

A `PER` key still prints its column name rather than a binding index (`#[3.1]`):
it is a column reference whose alias *is* its name, and `RenderDecideSource`
falls through to `ToString()` for a plain column ref, which prefers that alias.

**Four surfaces, one implementation.** The logical node, the physical node
(`PhysicalDecide::ParamsToString`), the `WHEN`/`PER` qualifier on an infeasibility
diagnosis, and its right-hand-side label all call `RenderDecideSource`, so a
clause reads identically wherever it is quoted back. Each caller supplies the
`source_fragments` and `entity_scopes` copied down from `LogicalDecide`.

**EXPLAIN describes the model that was built, not only what the user wrote.** The
tree it walks is post-optimizer, so the rows linearization emitted (`NORM`
indicators, ABS auxiliaries, `<>` links) appear alongside the user's clauses,
carrying their internal variable names. Those names are real model columns; the
renderer makes them legible without pretending they are SQL.

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
| `GetName()` / `ParamsToString()` on the logical node | `src/planner/operator/logical_decide.cpp` |
| The shared walker and `RenderDecideSource` | `src/planner/decide/decide_source_provenance.cpp` |
| Their declarations | `src/include/duckdb/planner/decide/decide_source_provenance.hpp` |
| Physical node rendering (calls the same walker) | `src/execution/operator/decide/physical_decide.cpp:728-772` |
| Source fragments carried to both nodes | `LogicalDecide::source_fragments`, `PhysicalDecide::source_fragments` |
| Tests | `test/decide/tests/test_explain.py` — 27 cases over TPC-H, including `test_explain_renders_user_casts_only` and `test_explain_objective_when_postfix` |
