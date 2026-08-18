# Stage 03 — Logical plan

Represents the decision query and its relational inputs as a `LogicalDecide`
operator sitting above the scan and filter operators of the child plan. It owns
where DECIDE metadata lives, how new constraints and objectives may enter after
planning, and how the whole thing serializes.

It contains no solver-specific structures and no execution mechanics.

**Key source files**

- `src/planner/binder/query_node/plan_select_node.cpp` — builds the operator
- `src/planner/operator/logical_decide.cpp` — the operator itself
- `src/include/duckdb/planner/operator/logical_decide.hpp` — the metadata contract
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

**From the optimizer** — `ne_indicator_indices`, `minmax_indicator_links`,
`bilinear_links`, `abs_maximize_links`, `aux_var_expressions`,
`composed_minmax_constraints`, `composed_minmax_objective_terms`,
`flat_objective_agg` / `flat_objective_is_easy`, and the five `per_*` objective
fields.

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

## 5. Serialization

`Serialize` / `Deserialize` are hand-maintained (the operator is marked
`"custom_implementation": true`), which means **every new field needs a matching
pair of lines**. Property ids run 200-236 and are append-only.

Structs are flattened into parallel vectors rather than serialized as structs:
`bilinear_links` becomes three `vector<idx_t>`; `abs_maximize_links` becomes two;
`entity_scopes` becomes eight (`scope_aliases`, `scope_table_indices`,
`scope_binding_counts`, `scope_binding_tables`, `scope_binding_cols`,
`scope_var_counts`, `scope_var_indices`, plus the count); `variable_scopes`
becomes two (`variable_entity_scope`, `variable_scope_kinds`); the source registry
becomes three (`constraint_source_lhs`, `..._rhs`, `..._qualifiers`).

This exists so prepared statements can be cached and replayed.

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
| Operator, entry points, serialization, EXPLAIN strings | `src/planner/operator/logical_decide.cpp` |
| Metadata contract (every field, documented) | `src/include/duckdb/planner/operator/logical_decide.hpp` |
| Binding resolution shielding | `src/execution/column_binding_resolver.cpp` |
| Logical → physical, entity key physical indices | `src/execution/physical_plan/plan_decide.cpp` |
| Source display registry | `src/planner/decide/decide_source_provenance.cpp` |
