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
2. **Objective** — `MAXIMIZE` / `MINIMIZE` and the objective expression, its `when`
   in front.
3. **Constraints** — one group per user clause, its `when c per K:` in front.

```
┌───────────────────────────┐
│           DECIDE          │
│    ────────────────────   │
│        Variables: x       │
│                           │
│         Objective:        │
│      MAXIMIZE sum(x * v)  │
│                           │
│        Constraints:       │
│      sum(x * w) <= 6      │
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
(`src/planner/decide/decide_source_provenance.cpp`) walks it, carrying the qualifier
of the wrappers above each clause:

| Node | Handling |
|---|---|
| `per K:` wrapper (`PER_CLASS_TAG`) | Add `per K` to the qualifier, the key as written (`PerClassQualifier`, from the source fragment the binder recorded); recurse into child 0 |
| `when c:` wrapper (`WHEN_CONSTRAINT_TAG`, 2 children) | Put `when c` (child 1) in front of the qualifier (`WhenQualifier`); recurse into child 0 |
| Plain `AND` | Recurse into each child |
| `IN` marker (`COMPARE_IN`) | `<target> IN (v1, v2, ...)`, each part through the renderer |
| Leaf | `RenderDecideSource`, with the qualifier and a colon in front (`FrontQualifier`, `AttachDecideQualifier`) |

Producing, for example:

```
when returnflag = 'R' per department: sum(x * weight) by (department) <= 50.0
```

**The Objective row goes through the same walker**, not a direct `GetName()`. A
conditional objective is itself a `when`-tagged `BoundConjunctionExpression`, so the
walker prints its `when` in front exactly as it does a constraint's, and the two rows
stay symmetric. `GetName()` on the wrapper would return its *alias*, the raw
`__when_constraint__` tag, because `GetName()` short-circuits to the alias whenever
one is set.

### The plan style

Every clause prints the same way, written as the user can edit it (D32):

- reducers, `norm` and the scope keywords in lower case: `sum(x * w) <= 6`,
  `MAXIMIZE when active: sum(x * w)`;
- the `when c per K:` qualifier in front of its clause, a reducer's own scope inside it
  (`sum(when priority per D: opening_cost * open)`) and its `by (G)` after it,
  the key as written (`per D.depotID`), a column by its name;
- the defaults `per row:` and `by ()` are not printed;
- the colon form keeps its spelling: `SUM(D: e)` prints as `sum(D: e)`, not `sum(per D: e)` (D38);
- a nested reducer nested, as written: `MINIMIZE when tier > 0: max(per grp: sum(x * v) by (grp))`,
  each of its reducers under its own account, *"sum(x * v) by (grp) becomes a helper
  decision per group"*, *"max(per grp: ...) becomes a helper decision"* (D75).

A diagnosis quotes clauses in the same style, and so do the labels of the rows a
rewrite adds: a `MIN`/`MAX` extremum column is `max(ship)`
(`LogicalDecide::minmax_clause_labels`, written by `RewriteMinMaxInConstraint` through
`RenderForPlan`), a `<>` binary is `sum(ship) by (D.region) <> 20`
(`ne_clause_labels`).

### Leaves render by authorship, not by type

`RenderDecideSource` is the project's single user-facing expression renderer,
documented in `decide_source_provenance.hpp` and described in
[`../../01_pipeline/03_logical_plan/done.md`](../../01_pipeline/03_logical_plan/done.md).
It replaces `ToString()` because the bound tree is not the query: it carries
casts the user cannot edit, prints functions in call form, and stores pipeline
metadata (source clause, scope role, a reducer's key and `by`) in expression aliases
that `GetName()` would print verbatim as `__source_clause_0__`.

The rule that separates a cast the user typed from one the binder added is
**authorship, not type**. `TagDecideSourceFragments` records the written spelling
of every cast and scalar subquery before binding can obscure it, so a tagged node
replays its own SQL and an untagged one is dropped. `SUM(x * CAST(q AS INTEGER))`
therefore keeps its cast — that is legal SQL, since the binder rejects a cast only
over a *decision* — while the reconciliation casts around `x` and `q` do not
appear at all.

A key prints as written rather than as binding indexes (`#[3.1]`): a `per K:` key
from the source fragment the binder recorded, a reducer's own
`per` from its key scope's `table_alias`, and a `by` key from `DecideGroupKey::text`.
A column inside an expression prints by its name, which a column reference carries as
its alias.

**Four surfaces, one implementation.** The logical node, the physical node
(`PhysicalDecide::ParamsToString`), the `when c per K:` qualifier on an infeasibility
diagnosis, and its right-hand-side label all call `RenderDecideSource`, so a
clause reads identically wherever it is quoted back. Each caller supplies the
`source_fragments` and `entity_scopes` copied down from `LogicalDecide`.

**EXPLAIN describes the model that was built, not only what the user wrote.** The
tree it walks is post-optimizer, so the rows linearization emitted (`NORM`
indicators, ABS auxiliaries, `<>` links) are part of the picture. An auxiliary column
prints as the expression it stands for (`abs(ship - 20)`, `(open * ship)`,
`(ship = 5)`, `sum(ship) by (D.region)`; the labels ride in `DecideSourceNames`), never
by its internal name, and each rewrite leaves one account line saying what it made of
the construct (D27). Section 2.1 says where they print.

---

## 2.1 Layered rendering: as written → canonical → rewritten

A DECIDE clause is read three times on its way to a solver, and the Constraints
section shows each reading that says something different about it:

1. **As written** — `abs(ship - 20) <= 5`
2. **Canonical** — decisions left, bound right, after stage 04
3. **Rewritten** — the rows the solver receives, after stage 05

The written clause heads its group. An indented `≡` line follows when
canonicalization moved something, and indented `↳` lines follow for the rows the
solver actually receives. **A clause nothing touched stays a single line** — both
lower layers are noise when they read the same as the one above.

```
Constraints:
  max(ship) + sum(ship) <= 100
    ↳ max(ship) becomes an extremum column
  avg(ship) >= 10
    ↳ avg(ship) becomes a sum divided by its row count
    ↳ sum(ship) >= 10
  abs(ship - 20) <= 5
    ↳ abs(ship - 20) becomes an auxiliary variable
    ↳ abs(ship - 20) - ship >= -20
    ↳ abs(ship - 20) + ship >= 0 + 20
  ship <= capacity * open
    ≡ ship - capacity * open <= 0.0
  sum(ship) >= 300
```

The account lines come first under their clause, then the rows; a row that reads as
the layer above it (the ABS clause's own row, `abs(ship - 20) <= 5`) is left out. Each
account is a fact a rewrite recorded (`LogicalDecide::rewrite_accounts`: the construct
and a `RewriteAccount::Kind`), worded in one place by `RenderDecideRewriteAccount`. The
objective prints its own accounts, and the rows a rewrite added for it (an ABS
envelope, an L0 link, tagged `OBJECTIVE_ROW_TAG`), under `Objective:`.

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
the rows it emits (`MarkFormulationConstraint`, `CopyClauseProvenanceTags`). Every
rewrite that emits rows carries the enclosing clause's alias down its recursion
through the one shared helper, `DescendSourceAlias`: a node adopts an alias only
when it parses as a source-clause or removal-group tag, and otherwise inherits its
parent's, so no rewrite can attach a row to an alias that is not a clause id.

The mapping is not one-to-one in either direction and the renderer tolerates
both:

- **One clause, many rows** — a linearized clause owns every row it generated.
- **One clause, no rows** — a composed MIN/MAX clause was lifted out of the tree
  into `composed_minmax_constraints`, leaving a `TRUE` placeholder. Its account lines
  report which reducers became extremum columns, since that is the one thing no other
  line shows.
- **A row for the objective** — a row a rewrite added for the objective carries
  `OBJECTIVE_ROW_TAG` instead of a clause id and prints under the objective.
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

**What the data made of the classes.** Under a clause whose `per` classes held more than
one variant (D64), or a reducer whose own `per` read more values than classes (D65), one
more account line says so: `↳ per t.g: 2 constraints from 1 class, one per distinct (a)
within a class`, `↳ sum(per D: ...): 3 values from 2 classes, one per distinct (T.unit,
ship) within a class`. Only the data can tell, so execution records it
(`PhysicalDecide::variant_accounts`, `RewriteAccount::Kind::VARIANTS`, written by
`RecordVariants` during `Finalize`); the profiler reads `ParamsToString` again when the
operator's result is read, after `Finalize`, which is what puts the line in `EXPLAIN
ANALYZE` and keeps it out of a plain `EXPLAIN`, which runs nothing. A clause with one
variant per class prints nothing new. Test: `test_anr_variants.py`.

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
| Physical node rendering (calls the same walker) | `PhysicalDecide::ParamsToString` in `src/execution/operator/decide/physical_decide.cpp` |
| Source fragments carried to both nodes | `LogicalDecide::source_fragments`, `PhysicalDecide::source_fragments` |
| The three-layer record | `ConstraintSourceInfo` in `src/include/duckdb/common/decide_source_info.hpp` |
| Layering and formatting | `CollectDecideClauseLayers` / `RenderDecideClauseLayers` / `RenderDecideObjectiveLayers` |
| Clause id carried onto emitted rows | `MarkFormulationConstraint`, `CopyClauseProvenanceTags`, `DescendSourceAlias` in `src/optimizer/decide/decide_optimizer.cpp` (shared via `decide_optimizer_internal.hpp`) |
| Objective snapshots captured | `src/planner/binder/query_node/plan_select_node.cpp`, around `CanonicalizeObjective` |
| Account lines and auxiliary labels | `RewriteAccount` in `logical_decide.hpp`; `AddRewriteAccount`, `RenderForPlan` in `decide_optimizer.cpp`; `RenderDecideRewriteAccount`, `DecidePlanNames` in `decide_source_provenance.cpp` |
| Tests | `test/decide/tests/test_explain.py` — 35 cases over TPC-H, including `test_explain_renders_user_casts_only`, `test_explain_objective_when_postfix` (the objective's `when` in front), and the layered-rendering group; `test_scope_explain_diagnose.py` (accounts, labels and the scope echo); plus `test_diagnosis_written_clause.py::test_explain_leads_with_the_written_clause` |
