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
| Canonical | `≡` | canonicalization changed it | `ConstraintSourceInfo::spelled_*`, set by `FinalizeConstraintSourceInfo` |
| Rewritten | `↳` | the optimizer changed it | the post-optimizer tree, walked at render time |

A clause nothing touched stays one line, and a row that reads the same as the layer above is left out. The canonical
layer prints the spelling canonicalization wrote, with every product coefficient first, so a clause written
`SUM(x * w) <= 6` prints `≡ sum(w * x) <= 6`; the clause already written in that order stays one line. Under a
rewritten clause, the account lines (what the rewrite made of the construct) come first, then the rows. An account is
a fact the rewrite recorded (`LogicalDecide::rewrite_accounts`), worded in one place by `RenderDecideRewriteAccount`
(D27). The objective has the same layers (`written_objective`, cut from the `OBJECTIVE` span; `canonical_objective` and
`objective_canonical_differs`, captured around `CanonicalizeObjective` in `Binder::CreatePlan`); rows a rewrite added
for it carry `OBJECTIVE_ROW_TAG` and print under `Objective:`. The canonical objective is a minimization
(`../pipeline/04_canonicalizer/done.md` §3.9), and its layer renders in the sense written (`RenderWrittenSense`
reverses each term's sign and keeps its factor), so the flip of a `MAXIMIZE` objective adds no `≡` line.

EXPLAIN shows the model that was built, not only the query: the rows linearization emitted are part of the picture.
An auxiliary column prints as the expression it stands for (`abs(ship - 20)`, `(open * ship)`, `max(ship)`,
`sum(ship) by (D.region) <> 20`), never by its internal name. The labels live in `aux_var_expressions`,
`minmax_clause_labels` and `ne_clause_labels`.

## Rendering

**Written text is cut, never re-rendered** (a source map, as Clang's SourceManager and DuckDB's `CreateInfo::sql`).
Inside a DECIDE clause the parser records one span per expression (`EXPR`, with how tightly its own text binds:
`DecideBinding`), plus a span for a reducer's call without its `by` (`CALL`), a `per` key, a `by` key, the two halves
of a `BETWEEN` and the objective (anchored at the start of the tail). A span is `{anchor, role, start, end, binding}`;
the parentheses around an expression are not part of its span. `SelectNode::decide_text` (`DecideSourceText`) keeps
the tail as typed and its spans keyed by `(query_location, role)`; it is serialized, `Equals` ignores it, and
`SelectNode::ToString` prints the tail verbatim, so a print-and-parse round trip keeps the spans. The binder keeps the
parsed node's query location on the bound node, and the operator carries the text (`LogicalDecide::source_text`,
copied to `PhysicalDecide`). A table macro whose argument lands in a DECIDE tail clears `decide_text`, since the
argument is not in the body's text; that clause renders by tree. A scalar macro's expansion stands where its call
stood: the call is quoted as written and the macro definition's text is not kept. A parameter is quoted as written
(`$1`).

**The location rule** (Clang's TreeTransform; `../pipeline/architecture.md` §3). A replacement takes the location of
exactly the node it equals in value, so a cast the binder added, a respelled product and a helper column for a data
reducer quote as the text they stand for. A node a rewrite built, whose value differs from every node it replaced, has
no location: it prints its operator and quotes its children, never an ancestor's span.

**One tree renderer.** `RenderDecideSource` (`decide_source_provenance.cpp`) cuts the span of a located node (whitespace
and comments verbatim) before it reprints anything, and parenthesizes the cut by its own binding, so `(a + b) * c`
keeps its parentheses and `a + b * c` gains none. A message and a `DIAGNOSE` repair quote every located node. The
canonical and rewritten layers quote every located subtree that reads no decision and reprint the decision structure,
which canonicalization and the rewrites reshape: the colon form prints as `sum(per D: e)` there (D38). A reprinted node
prints its operator and quotes its children; a cast with no location is display noise and prints as its child. The
canonical layer prints the respelled product, and a message never searches for the other layer's text. A reprinted
call prints the name the user wrote, which the binder keeps in a tag when it renames an alias to its primary
(`DecideWrittenName`, `../pipeline/02_binder/done.md` § One spelling per function): `pow(x - v, 2)` reprints as `pow`
and `@(x - v)` as `@`, also in the ABS rewrite's account label.
`Expression::ToString()` is not used because the bound tree is not the query: it carries casts the binder inserted,
prints functions in call form, and keeps pipeline tags in aliases that `GetName()` prints verbatim
(`__source_clause_0__`).

**The style** (D32): quoted text as typed, in the user's case; a reprinted node (a rewritten row, the decision structure
of a layer) with reducers, `norm` and keywords in lower case; the `when c per K:` qualifier in front; a
reducer's own scope inside it and its `by (G)` after it; keys as written (`per D.depotID`), never as binding indexes;
the defaults `per row:` and `by ()` not printed; the two-level objective printed whole.

**Casts by location.** A cast the user wrote has its own span, so it is quoted with the expression around it:
`SUM(x * CAST(q AS INTEGER))` keeps its cast. A cast the binder added takes its child's location and quotes as the
child, so reconciliation casts never appear.

**Walking the conjunction.** The binder puts all of `SUCH THAT` into one `BoundConjunctionExpression`.
`CollectDecideExpressionStrings` walks it, building the qualifier of the wrappers above each clause:

| Node | Handling |
|---|---|
| `PER_CLASS_TAG` (`per K:`) | add `per K`, key as written (`PerClassQualifier`); recurse |
| `WHEN_FILTER_TAG` (`when c:`) | add `when c` in front (`WhenQualifier`); recurse |
| `TWO_LEVEL_OBJECTIVE_TAG` (two-level objective) | print it whole where its outer reducer stands in the term, so a sign or factor prints around it: `-max(per k: ...)`, `2 * max(per k: ...)` (`RenderTwoLevelObjective`) |
| `AND` | recurse into each child |
| `COMPARE_IN` marker | `<target> IN (v1, …)` |
| leaf | `RenderDecideSource`: the cut text of a located node, else the reprint; quoted with its qualifier by `QuoteWrittenClause` |

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
