# EXPLAIN

`EXPLAIN`, `EXPLAIN ANALYZE` and `EXPLAIN (FORMAT JSON)` work on a DECIDE query. `LogicalDecide` and `PhysicalDecide`
override `GetName()` and `ParamsToString()` to give DuckDB's plan renderer a DECIDE node. EXPLAIN reports on the plan;
DIAGNOSE ([diagnose.md](diagnose.md)) reports on a solve that failed. The user-facing summary is
[guide.md §9](guide.md#9-explain).

## The node

The DECIDE node prints three sections: **Variables** (declared decisions only, no auxiliaries), **Objective** (the
sense and the expression, its `when` in front) and **Constraints** (one group per clause, its `when c per K:` in
front).

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

Each clause appears in up to three layers:

| Layer | Marker | Shown when | Source |
|---|---|---|---|
| As written | none | always | `ConstraintSourceInfo::written_*`, set by `InitializeConstraintSourceInfo` on the bound tree before canonicalization |
| Canonical | `≡` | canonicalization changed it | `ConstraintSourceInfo::canonical_*`, set by `FinalizeConstraintSourceInfo` |
| Rewritten | `↳` | the optimizer changed it | the post-optimizer tree, walked at render time |

A clause nothing touched stays one line, and a row that reads the same as the layer above is left out. Under a
rewritten clause, the account lines (what the rewrite made of the construct) come first, then the rows. An account is
a fact the rewrite recorded (`LogicalDecide::rewrite_accounts`), worded in one place by `RenderDecideRewriteAccount`
(D27). The objective has the same layers (`written_objective`, `canonical_objective`, captured around
`CanonicalizeObjective` in `Binder::CreatePlan`); rows a rewrite added for it carry `OBJECTIVE_ROW_TAG` and print under
`Objective:`.

EXPLAIN shows the model that was built, not only the query: the rows linearization emitted are part of the picture.
An auxiliary column prints as the expression it stands for (`abs(ship - 20)`, `(open * ship)`, `max(ship)`,
`sum(ship) by (D.region) <> 20`), never by its internal name. The labels live in `aux_var_expressions`,
`minmax_clause_labels` and `ne_clause_labels`.

## Rendering

**Written text is cut, never re-rendered** (a source map, as Clang's SourceManager and DuckDB's `CreateInfo::sql`).
Inside a DECIDE clause the parser records spans (`{anchor, role, start, end}`) for a comparison's two sides, `when`,
`per`, `by` and every column. `SelectNode::decide_text` (`DecideSourceText`) keeps the tail as typed and its spans
keyed by `(query_location, role)`; it is serialized, `Equals` ignores it, and `SelectNode::ToString` prints the tail
verbatim, so a print-and-parse round trip keeps the spans. Binders cut text into `source_fragments` and tag the bound
node; a bound data column carries its spelling (`TagWrittenColumn`). A table macro whose argument lands in a DECIDE
tail clears `decide_text`, since the argument is not in the body's text; that clause renders by tree. DuckDB's plan
nodes strip only the source-fragment tag (`StripSourceFragmentTags`); rewrites read the other tags.

**One tree renderer.** `RenderDecideSource` (`decide_source_provenance.cpp`) prints text DeciDB writes itself (the
canonical layer, moved bounds, edited clauses) and any node without a span. `Expression::ToString()` is not used because the bound tree is not the query: it carries
casts the binder inserted, prints functions in call form, and keeps pipeline tags in aliases that `GetName()` prints
verbatim (`__source_clause_0__`).

**The style** (D32): quoted text as typed; written text with reducers, `norm` and keywords in lower case; the
`when c per K:` qualifier in front; a
reducer's own scope inside it and its `by (G)` after it; keys as written (`per D.depotID`), never as binding indexes;
the defaults `per row:` and `by ()` not printed; the colon form kept as `sum(D: e)` (D38); the two-level objective
printed whole.

**Casts by authorship.** `TagDecideSourceFragments` records the written spelling of every cast and scalar subquery
before binding. A tagged node replays its own SQL; an untagged one, which the binder added, is dropped. So
`SUM(x * CAST(q AS INTEGER))` keeps its cast and reconciliation casts never appear.

**Walking the conjunction.** The binder puts all of `SUCH THAT` into one `BoundConjunctionExpression`.
`CollectDecideExpressionStrings` walks it, building the qualifier of the wrappers above each clause:

| Node | Handling |
|---|---|
| `PER_CLASS_TAG` (`per K:`) | add `per K`, key as written (`PerClassQualifier`); recurse |
| `WHEN_FILTER_TAG` (`when c:`) | add `when c` in front (`WhenQualifier`); recurse |
| `TWO_LEVEL_OBJECTIVE_TAG` (two-level objective) | print it whole (`RenderTwoLevelObjective`) |
| `AND` | recurse into each child |
| `COMPARE_IN` marker | `<target> IN (v1, …)` |
| leaf | the cut text (else `RenderDecideSource`), quoted with its qualifier by `QuoteWrittenClause` |

Messages quote a clause only through `QuoteWrittenClause` or `QuoteEditedClause` (`decide_source_info.cpp`), which
put the qualifier in front. The objective goes through the same walker. `GetName()` on a `when` wrapper would return its alias, the raw tag.

**Clause identity is an id, not a position.** Stage 05 appends rows and removes clauses, so positional matching
would drift. The binder stamps a `source_clause_id` on every written comparison, and every rewrite that emits rows
carries it down through one helper, `DescendSourceAlias` (with `MarkFormulationConstraint`,
`CopyClauseProvenanceTags`). A node adopts an alias only if it parses as a clause or removal-group tag, so no row can
attach to a non-clause alias. The mapping can be:

- one clause → many rows (a linearized clause);
- one clause → no rows (`IN` and composed `MIN`/`MAX` are lifted out of the tree and leave a `TRUE` placeholder
  stamped with the clause id by `MakeTrueExpression`, printed as nothing; its account line says what happened);
- a row for the objective (`OBJECTIVE_ROW_TAG`);
- a row with no clause, printed under `added by formulation:`. Nothing reaches this today; it makes a rewrite that
  forgets the id visible instead of misattributed.

**Order follows the tree.** Clause ids are handed out in two passes (comparisons, then `IN`s), so display order is
first appearance in the post-optimizer tree. That is why the placeholders carry ids: an untagged placeholder would
sort to the end.

## EXPLAIN ANALYZE, JSON and cardinality

- **ANALYZE** needs no instrumentation: DuckDB times each operator's `Sink` (materialization), `Finalize` (model
  build and solve) and `Source` (readback). Row counts are actual. On small inputs per-operator times can round to
  `(0.00s)`; use `Total Time`.
- **JSON** uses the same `GetName()`/`ParamsToString()`, not `Serialize` (which exists for prepared statements).
  DuckDB splits a multi-line value into an array, one entry per line, markers included.
- **Cardinality.** DECIDE does not override `EstimateCardinality`; it emits every input row, so the child's estimate
  is correct.

## Code

| What | Where |
|---|---|
| Node rendering | `logical_decide.cpp`; `PhysicalDecide::ParamsToString` (`physical_decide.cpp`) |
| Walker and renderer | `decide_source_provenance.cpp` / `.hpp` (`CollectDecideClauseLayers`, `RenderDecideClauseLayers`, `RenderDecideObjectiveLayers`, `DecidePlanNames`) |
| Three-layer record | `ConstraintSourceInfo` (`decide_source_info.hpp`) |
| Clause ids on emitted rows | `decide_optimizer.cpp` (shared through `decide_optimizer_internal.hpp`) |
| Accounts and auxiliary labels | `RewriteAccount` (`logical_decide.hpp`); `AddRewriteAccount`, `RenderForPlan` (`decide_optimizer.cpp`) |
| Tests | `test_explain.py`, `test_scope_explain_diagnose.py`, `test_diagnosis_written_clause.py::test_explain_leads_with_the_written_clause` |
