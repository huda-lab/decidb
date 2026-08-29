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
3. **Constraints** — one group per user clause, with `WHEN` / `PER` suffixes.

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
`"-"("*"(a, b), c)`. Section 2 covers the shared renderer; section 2.1 covers how
a clause the pipeline reshaped is grouped with what it became.

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
| `IN` marker (`COMPARE_IN`) | `<target> IN (v1, v2, ...)`, each part through the renderer |
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
indicators, ABS auxiliaries, `<>` links) are part of the picture, carrying their
internal variable names. Those names are real model columns; the renderer makes
them legible without pretending they are SQL. Section 2.1 says where they print.

---

## 2.1 Layered rendering: as written → canonical → rewritten

A DECIDE clause is read three times on its way to a solver, and the Constraints
section shows each reading that says something different about it:

1. **As written** — `ABS(ship - 20) <= 5`
2. **Canonical** — decisions left, bound right, after stage 04
3. **Rewritten** — the rows the solver receives, after stage 05

The written clause heads its group. An indented `≡` line follows when
canonicalization moved something, and indented `↳` lines follow for the rows the
solver actually receives. **A clause nothing touched stays a single line** — both
lower layers are noise when they read the same as the one above.

```
Constraints:
  MAX(ship) + SUM(ship) <= 100
    ↳ MAX(ship) becomes an auxiliary variable
  AVG(ship) >= 10
    ↳ SUM(ship) >= 10
  ABS(ship - 20) <= 5
    ↳ __abs_aux_0__ <= 5
    ↳ __abs_aux_0__ - ship >= -20
    ↳ __abs_aux_0__ + ship >= 0 + 20
  ship <= capacity * open
    ≡ ship - capacity * open <= 0.0
  SUM(ship) >= 300
```

Before this, EXPLAIN printed whichever tree happened to sit on `LogicalDecide` at
render time, and the same three clauses came out as a bare `true`, a
`SUM(ship) >= 10` that is not what `AVG(ship) >= 10` means, and three loose rows
naming an `__abs_aux_0__` with no account of where it came from. Two queries with
different optima could produce identical plans.

**Where each layer comes from.** Nothing re-derives a form at render time:

| Layer | Source | Written by |
|---|---|---|
| As written | `ConstraintSourceInfo::written_lhs` / `written_cmp` / `written_rhs` | `InitializeConstraintSourceInfo`, on the bound-but-not-yet-canonical tree |
| Canonical | `ConstraintSourceInfo::canonical_lhs` / `canonical_cmp` / `canonical_rhs` | `FinalizeConstraintSourceInfo`, right after canonicalization |
| Rewritten | The post-optimizer tree itself | walked at render time |

`written_*` is recorded for **every** clause. Its narrower sibling
`source_lhs` / `source_rhs` answers a different question — *should a repair quote
the written form instead of the canonical one?* — so it is set only for the one
rewrite a user cannot recognize and cleared when the two forms agree. Both are
rendered once, in one place, and assigned to both readers.

The objective has the same two snapshots as `LogicalDecide::written_objective`
and `canonical_objective`, captured around the single `CanonicalizeObjective`
call in `Binder::CreatePlan`. It is one objective rather than a registry, so it
needs no ids.

**Association is `source_clause_id`, not position.** The constraint set is not
stable across stage 05: `AddConstraint` appends rows and `RewriteComposedMinMax`
removes a clause from the tree entirely, so layer 3 is not a line-for-line image
of layer 2 and positional matching would drift. The binder stamps a
`source_clause_id` on every written comparison, and the optimizer copies it onto
the rows it emits (`MarkFormulationConstraint`, `CopySourceClauseTag`), including
through the ABS and McCormick rewrites, which carry the enclosing clause's alias
down their recursion. The mapping is not one-to-one in either direction and the
renderer tolerates both:

- **One clause, many rows** — a linearized clause owns every row it generated.
- **One clause, no rows** — a composed MIN/MAX clause was lifted out of the tree
  into `composed_minmax_constraints`, leaving a `TRUE` placeholder. Its group
  reports which reducers became auxiliary variables, since that is the one thing
  no other line shows.
- **A row with no clause** — printed under `added by formulation:` rather than
  dropped or misattributed. Nothing currently reaches this path; it exists so a
  future rewrite that forgets to carry an id degrades visibly instead of lying.

**Display follows the tree, not the registry.** Source ids are handed out in two
passes — every comparison first, then the DECIDE-variable `IN`s — so registry
order would print an `IN` written first as though it were written last. Clause
order is taken from first appearance in the post-optimizer tree instead.

That makes the tree the only record of written order, which is what the `TRUE`
placeholder is for. Both lowerings that lift a clause out of the tree — `IN` and
composed MIN/MAX — leave one behind, and `MakeTrueExpression` stamps it with the
clause id it replaces. The renderer emits a placeholder as an empty string, so it
holds the clause's position without printing a bare `true`. An untagged
placeholder is indistinguishable from a clause that never existed and sorts to
the end of the plan.

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

DuckDB splits a multi-line section value into a JSON array, so the layered
Constraints section arrives as one array entry per line, `≡` and `↳` markers
included, rather than a single pre-joined string.

---

## 5. Cardinality

DECIDE does not override `EstimateCardinality`. The child's estimate propagates
upward, which is correct: DECIDE emits every input row. The `~N Rows` in `EXPLAIN`
and the `N Rows` in `EXPLAIN ANALYZE` match the scan's cardinality.

---

## 6. Source map

| Concern | Location |
|---|---|
| `GetName()` / `ParamsToString()` on the logical node | `src/planner/operator/decide/logical_decide.cpp` |
| The shared walker and `RenderDecideSource` | `src/planner/decide/decide_source_provenance.cpp` |
| Their declarations | `src/include/duckdb/planner/decide/decide_source_provenance.hpp` |
| Physical node rendering (calls the same walker) | `src/execution/operator/decide/physical_decide.cpp:728-772` |
| Source fragments carried to both nodes | `LogicalDecide::source_fragments`, `PhysicalDecide::source_fragments` |
| The three-layer record | `ConstraintSourceInfo` in `src/include/duckdb/common/decide_source_info.hpp` |
| Layering and formatting | `CollectDecideClauseLayers` / `RenderDecideClauseLayers` / `RenderDecideObjectiveLayers` |
| Clause id carried onto emitted rows | `MarkFormulationConstraint`, `CopySourceClauseTag`, `DescendSourceAlias` in `src/optimizer/decide/decide_optimizer.cpp` (shared via `decide_optimizer_internal.hpp`) |
| Objective snapshots captured | `src/planner/binder/query_node/plan_select_node.cpp`, around `CanonicalizeObjective` |
| Tests | `test/decide/tests/test_explain.py` — 35 cases over TPC-H, including `test_explain_renders_user_casts_only`, `test_explain_objective_when_postfix`, and the layered-rendering group; plus `test_diagnosis_written_clause.py::test_explain_leads_with_the_written_clause` |
