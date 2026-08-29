# Stage 03 — Logical plan

Represents the decision query and its relational inputs as a `LogicalDecide`
operator sitting above the scan and filter operators of the child plan. It owns
where DECIDE metadata lives, how new constraints and objectives may enter after
planning, and how the whole thing serializes.

It contains no solver-specific structures and no execution mechanics.

**Key source files**

- `src/planner/binder/query_node/plan_select_node.cpp` — builds the operator
- `src/planner/operator/decide/logical_decide.cpp` — the operator itself
- `src/include/duckdb/planner/operator/decide/logical_decide.hpp` — the metadata contract
- `src/execution/column_binding_resolver.cpp` — the `LOGICAL_DECIDE` case
- `src/execution/physical_plan/plan_decide.cpp` — logical → physical

---

## 1. Placement

`LogicalDecide` is inserted **above** the source scan and any `Filter`, so the
solver only ever sees rows that survived `WHERE`. A `Projection` sits above it and
prunes auxiliary variable columns from the user's result.

`GetColumnBindings()` returns the child's bindings plus one per entry of
`decide_variables` — **including auxiliaries**, because constraint and objective
expressions reference them and column-binding resolution has to find them.
`ResolveTypes()` mirrors that.

---

## 2. Subquery planning and correlation provenance

`plan_select_node.cpp:42-138`, and this ordering is load-bearing.

`PlanSubqueries` flattens scalar subqueries into joins. After flattening, a
row-varying column, an **uncorrelated** subquery and a **correlated** subquery are
all indistinguishable — each is a plain `BoundColumnRefExpression`, and the
flattened subquery is even literally named `SUBQUERY`. Correlation is visible only
*before* flattening.

So the operator is built in this order:

1. **Collect**, from constraints **and the objective**, every `BOUND_SUBQUERY` of
   `SubqueryType::SCALAR`, partitioned by `IsCorrelated()`. The owning *slot* is
   held, not the node — `PlanSubqueries` replaces the node in place.
2. **Flatten** with `PlanSubqueries`.
3. **Read back** what each slot became, peeling casts for identity
   (`StripCastsForIdentity`), and record the resulting table index in
   `query_wide_table_indexes` or `correlated_subquery_table_indexes`.
4. **Tag** each flattened value with `QUERY_WIDE_VALUE_TAG` or
   `ROW_VARYING_SUBQUERY_TAG`, so the same semantic fact survives copies and
   optimizer-generated re-canonicalization even where the table-index sets are
   unavailable.
5. **Canonicalize** constraints and objective, then verify (stage 04).

The objective is collected too, and must be: an uncorrelated `(SELECT k) * MAX(x)`
is otherwise indistinguishable from a row-varying column and hits the fail-safe
default, which rejects it for varying per row when it does not.

`correlated_subquery_table_indexes` never changes a decision — a correlated
subquery is row-varying by construction and the fail-safe default would reject it
anyway. It exists so the rejection can name what the user wrote instead of
`SUBQUERY`.

### Step 2 flattens unconditionally, including inside another flattening

`Binder::PlanSubqueries` normally *defers* a subquery it meets while an outer
subquery is still being flattened: it sets `has_unplanned_dependent_joins` and
leaves the node in place, and `RecursiveDependentJoinPlanner` finishes the job once
the outer subquery has been peeled. Ordinary clauses tolerate that because nothing
reads their expressions in between.

A DECIDE clause does not. Step 5 runs in this same `CreatePlan`, and
canonicalization copies the constraint tree — and `BoundSubqueryExpression::Copy()`
throws by design. So the DECIDE block saves `is_outside_flattened`, forces it true
across the two `PlanSubqueries` calls, and restores it. Layer 4's input contract is
a subquery-free bound tree, so the flattening is not optional at this point.

Without it, *any* subquery written inside a nested DECIDE failed with
`Serialization Error: Cannot copy BoundSubqueryExpression` — a DECIDE nested two
deep, but equally a plain scalar subquery used as a bound inside a single nested
clause, or one scaling its objective. One level of nesting worked only because the
outermost `CreatePlan` is not itself inside a flattening. Pinned by
`test/decide/tests/test_nested_decide.py`.

---

## 3. The two post-planning entry points

After planning, `decide_constraints` and `decide_objective` may be modified only
through these two methods. Assigning either field directly bypasses
canonicalization and verification.

```cpp
void LogicalDecide::AddConstraint(ClientContext &, unique_ptr<Expression>);
void LogicalDecide::SetObjective(ClientContext &, unique_ptr<Expression>);
```

`AddConstraint` canonicalizes and verifies the incoming subtree, then ANDs it onto
the existing tree. It verifies only the fresh subtree, not the accumulated tree,
so repeated appends stay linear rather than quadratic.

`SetObjective` canonicalizes and verifies the replacement. Its peeled constant is
**added** to `objective_constant_offset` rather than assigned, so the constant the
user wrote survives every later optimizer rewrite. A null objective clears the
field.

Together with the call in `plan_select_node.cpp`, these are the only three places
`DecideCanonicalizer` runs.

---

## 4. What `LogicalDecide` carries

Grouped by who writes it:

**From the binder** — `decide_index`, `decide_variables`, `decide_constraints`,
`decide_sense`, `decide_objective`, `num_auxiliary_vars`, `is_boolean_var`,
`variable_scopes`, `entity_scopes`, `entity_key_expressions`,
`constraint_sources`.

**From the canonicalizer** — `objective_constant_offset`.

**From the optimizer** — `solver_backend_name`, `use_native_constructs`,
`force_native_constructs`, `ne_clause_labels`, `minmax_clause_labels`, `bilinear_links`,
`abs_maximize_links`, `aux_var_expressions`, `composed_minmax_constraints`,
`composed_minmax_objective_terms`, `flat_objective_agg` /
`flat_objective_is_easy`, the five `per_*` objective fields, the absorbed variable
box, and the prepared linear form.

`optimized` marks the boundary between the first two groups and the third. It is
set before the optimizer's first rewrite and is what takes this plan off the wire —
see §5.

### The solver is named, not held

Stage 05 has to settle three solver questions before it rewrites anything (see
[`../05_optimizer/done.md`](../05_optimizer/done.md) §0), and every answer rides the
plan from here. What the plan carries is the *answers*, never the solver:

- `solver_backend_name` — the registered identifier (`"gurobi"`, `"highs"`).
- `use_native_constructs` — a `SolverConstructSupport`, which constructs this backend
  can state itself.
- `force_native_constructs` — the policy over them: whether a declared construct is used
  wherever it is declared, or only where the lowering has no valid Big-M. False ships.

The third is a policy rather than a fact, and that is deliberate. Applying it needs a
per-clause test on evaluated coefficients, which no stage before execution can run — so
the plan carries the *rule* and stage 08 carries it out. That is not the plan deferring
a decision; it is the decision arriving where the data it ranges over exists.

A live `SolverBackend` handle here would let a logical plan open a solver session,
and session creation is stage 07's. A name cannot: turning it back into a backend is
a registry lookup (`SolverRegistry::Find`), and that happens where a solve is about
to run — `PhysicalDecide::PlannedSolverBackend()` — not on the plan.

None of the three is serialized. Which solver a host has is a fact about the host, not
about the query, so a plan replayed elsewhere re-resolves them.

### `entity_key_expressions`

A `BoundColumnRefExpression` per entity-key column, held here **only so DuckDB's
column pruning keeps those columns alive**. Without them, an entity key not
referenced in `SELECT` / `WHERE` / the clause would be pruned from the scan,
silently collapsing distinct entities into whatever grouping happened to survive.
`plan_decide.cpp` reads refreshed bindings back off these expressions — the
pruner's rebinding pass keeps them in sync — and copies them into
`entity_scopes.entity_key_bindings`.

They are deliberately **not** column-binding-resolved: `plan_decide.cpp` matches
them against the child's `GetColumnBindings()` by `(table_index, column_index)`,
which needs the logical binding intact.

### Column binding resolution

`ColumnBindingResolver`'s `LOGICAL_DECIDE` case enumerates DECIDE's
expression-holding fields **explicitly** rather than calling
`VisitOperatorExpressions`, because DECIDE variables must be shielded from
resolution via `ignored_bindings`. Currently enumerated: `decide_variables`,
`decide_constraints`, `decide_objective`, `composed_minmax_objective_terms`,
`composed_minmax_constraints`.

**Any new field holding expressions that execution will evaluate must be added to
that enumeration.** An optimizer rewrite that moves subexpressions out of
`decide_objective` into a side vector — as the composed MIN/MAX rewrite does —
otherwise silently opts them out, and the operator then reads a logical index as a
chunk position. A single-table source hides the bug entirely, because the two
indexings coincide there; it surfaces only once a join reorders columns.

---

## 4b. The `DIAGNOSE` flag, and the operator that reads it

`LogicalDecide::diagnose` is a plain `bool`, set by `Binder::BindDiagnose`
(`bind_showref.cpp`) when the statement carried the `DIAGNOSE` prefix, and copied to
`PhysicalDecide::diagnose` at physical planning (`plan_decide.cpp`). It is the ONLY thing
that arms the diagnostics engines.

Two properties matter, and both are the reason it lives here rather than in a session
setting:

- **It is a property of the statement.** Two queries on one connection can differ, and a
  plan carries its own answer. Nothing downstream reads it back out of a setting; layer 8
  reads the field it was handed, the same way it reads `solver_backend_name`.
- **It travels down, never sideways.** parser → binder → this operator → stage 08. The
  binder does not consult the executor, and the executor does not consult the session.

`BindDiagnose` binds the inner query exactly as it would without the prefix — DIAGNOSE
changes what is reported, never what is asked — then finds the first `LogicalDecide`
under projections, ORDER BY, or LIMIT, sets the flag, and wraps the whole plan in
`LogicalDecideDiagnose`. A plan composed from multiple DECIDE subqueries can contain
several operators; only the first is armed today, so a later failing subquery can bypass
the diagnosis relation. The unresolved policy is tracked in
[`../../07_query_diagnostics/foundations/todo.md`](../../07_query_diagnostics/foundations/todo.md).

**`LogicalDecideDiagnose`** (`planner/operator/decide/logical_decide_diagnose.hpp`) is the
prefix's own plan node: one child (the whole decision query), and an output schema that
is *not* the child's. It resolves its types from `GetDecideDiagnoseSchema` — the single
definition of the diagnosis relation's columns, shared with the physical operator that
fills them. The child's rows never surface: DIAGNOSE reports on the run, it does not
return the run's output.

A query with no `DECIDE` clause is rejected here, at bind time, rather than returning an
empty relation. That is the right layer for it: whether a plan contains a decision is a
binding fact, and the parser cannot see it (the clause may sit inside a subquery).

## 5. Serialization

`LogicalDecide` and `LogicalDecideDiagnose` are serialized by DuckDB's generator.
The field list lives in `storage/serialization/logical_operator.json`; the structs
it names — `EntityScopeInfo`, `DecideVarScopeInfo`, `ConstraintSourceInfo`,
`DecideSourceColumnName` — have their own entries in `nodes.json`, and both
directions are generated into `src/storage/serialization/serialize_*.cpp`. There is
no hand-written copy. Adding a field means adding one JSON entry, not two mirrored
lines in two functions that can drift apart.

Nothing is flattened into parallel vectors. `entity_key_bindings` is serialized as
`vector<ColumnBinding>`, using the `ColumnBinding` entry `nodes.json` already had.
Source-column display metadata is likewise a `vector<DecideSourceColumnName>`:
each record keeps a `ColumnBinding` and its user-written name together from binding,
through serialization, to physical planning.

### The wire carries bound plans only

The format carries what the binder and the canonicalizer produced. Everything
`DecideOptimizer` writes stays off it: `LogicalDecide::optimized` is set before the
first rewrite runs, and `SupportSerialization()` reports false from that point on.

That is a claim about portability, not about cost. Stage 05 chooses a formulation
from the constructs **this host's** solver declares — the same reason
`solver_backend_name` and `use_native_constructs` are not serialized (§4). An
optimized DECIDE plan is an answer computed for one machine, so it declines to be
copied rather than arriving somewhere else as a formulation that machine cannot
honor. The composed MIN/MAX terms, the absorbed variable box and the prepared
linear form are all rebuilt from the canonical tree by the optimizer, in
milliseconds, wherever the plan lands.

`Planner::VerifyPlan` runs twice — on the bound plan (`planner.cpp:84`) and again
after every optimizer (`optimizer.cpp:305`). The first round-trips DECIDE; the
second asks `OperatorSupportsSerialization` and is told no, so it returns without
touching the plan. `LogicalOperator::Copy` asks the same question and raises rather
than handing back a truncated plan.

### The property ids are a sequence, not a lookup

`BinaryDeserializer::OnPropertyBegin` requires the *next* field in the stream to
equal the id being read. The ids are a running checkpoint that the two sides are
still in step, not keys that can be sought. Write order and read order must match
exactly — which is why they are generated from one list, and why an id may be
renumbered freely: a serialized logical plan lives for microseconds inside one
process and is never persisted, so there is no older layout to stay readable for.

### What exercises it

`PRAGMA verify_serializer` round-trips the bound plan and **replaces the live plan
with the copy**, so a field that never reaches the wire becomes a wrong answer
rather than a silent omission. Run over the existing suite, that is a guard nobody
has to maintain per field:

```bash
DECIDB_VERIFY_SERIALIZER=1 test/decide/.venv/bin/python3 -m pytest test/decide/tests
```

`DECIDB_VERIFY_SERIALIZER=1` makes `test/decide/decidb_cli.py` attach the pragma to
every non-interactive query it runs through shell startup commands, with the pragma's
own result redirected away. Parsed DECIDE statements now reparse from `ToString()`;
see [`../01_parser/done.md`](../01_parser/done.md) §5. The parsed verifier does not run
a second solver execution, because equally optimal assignments need not have identical
rows. The two PTY-driven continuation cases are likewise outside DuckDB's materializing
verifier. Model-dump assertions select one complete build when verifier paths append
equivalent copies.

The guarded suite passes 1,602 tests with no skips.

---

## 6. EXPLAIN

`GetName()` returns `"DECIDE"`. `ParamsToString()` builds the structured plan
output, using `CollectDecideExpressionStrings` to render `WHEN` and `PER` wrappers
back into DECIDE postfix syntax — it recurses into child 0 and appends
` WHEN <cond>` / ` PER <cols>` to each leaf, parenthesizing a multi-column `PER`.

Both the walker and the leaf renderer `RenderDecideSource` live in
`src/planner/decide/decide_source_provenance.cpp`, beside the source-fragment
tagging they depend on, because every surface that echoes a DECIDE clause back to
a user needs them: the logical node, the physical node, and the labels an
infeasibility diagnosis prints. `Expression::ToString()` cannot serve any of them —
it prints the tree as bound, so it shows the casts the binder inserted while
reconciling types and spells arithmetic in call form. `RenderDecideSource` decides
what to show by **authorship**: `TagDecideSourceFragments` recorded the written
spelling of every cast and scalar subquery before binding could obscure it, so a
tagged node replays its own SQL out of `source_fragments` and an untagged cast is
dropped as reconciliation noise.

`source_fragments` therefore rides on `LogicalDecide` (serialized as field 238)
and is copied to `PhysicalDecide`, alongside the `constraint_sources` registry
that shares the same renderer.

See [`../../03_expressivity/explain/done.md`](../../03_expressivity/explain/done.md)
for the output itself.

---

## 7. Source map

| Concern | Location |
|---|---|
| Operator construction, subquery provenance | `src/planner/binder/query_node/plan_select_node.cpp` |
| Operator, entry points, serialization, EXPLAIN strings | `src/planner/operator/decide/logical_decide.cpp` |
| Metadata contract (every field, documented) | `src/include/duckdb/planner/operator/decide/logical_decide.hpp` |
| Binding resolution shielding | `src/execution/column_binding_resolver.cpp` |
| Logical → physical, entity key physical indices | `src/execution/physical_plan/plan_decide.cpp` |
| `DIAGNOSE` binding, and the flag's origin | `src/planner/binder/tableref/bind_showref.cpp` (`BindDiagnose`) |
| The `DIAGNOSE` plan node and its schema | `src/planner/operator/decide/logical_decide_diagnose.cpp` |
| Source display registry | `src/planner/decide/decide_source_provenance.cpp` |
