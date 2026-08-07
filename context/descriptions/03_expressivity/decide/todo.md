# DECIDE Clause — Planned Features

---

## Entity-level aggregation over table-scoped variables

**Priority: High — paper-facing. The CIDR'27 draft proposes syntax for this (§3.2,
"relation-qualified reducer") and §4.5 describes its execution, so the language surface is
now decided in the draft even though nothing is implemented. See "Syntax proposed by the
paper" below; the older option list is kept as the alternatives that were considered.**

A table-scoped variable (`DECIDE Sensors.keepS(BOOL)`) yields one solver
variable per entity, but every aggregate over it still runs over **join-result
rows**: an entity appearing in 5 rows contributes 5 times. That is deliberate
SQL semantics (`done.md` → "Aggregate semantics with table scope";
`../../00_project_overview/syntax_reference.md` §2.1; asserted by
`test/decide/tests/test_entity_scope.py:79`). The gap is that there is **no way
to aggregate once per entity** — no `COUNT(DISTINCT keepS)`, no entity-scoped
`SUM`. Any query whose objective is "how many *tuples* did I touch" is therefore
inexpressible, and the natural-looking phrasing is silently wrong rather than
rejected.

**Motivating case** (paper §2.1, Example 2.1 — counterfactual explanation of
false alerts). The prose asks for the *minimum number of tuples* to delete from
`Sensors` or `Policy`; the query is

```sql
from Alerts A join Policy P on ... join Sensors S on ...
such that sum(falseAlarm * keepP * keepS) = 0
maximize sum(keepS) + sum(keepP)
```

which actually maximizes $\sum_s \deg(s)\cdot keepS_s + \sum_p \deg(p)\cdot keepP_p$,
where $\deg$ is the number of alerts the entity joins with — an alert-weighted
repair cost, not a tuple count. The two objectives disagree: if sensor 23 raises
three false alarms across two alert types, deleting that one sensor row costs 3
while deleting the two matching `Policy` rows costs 2, so the query returns a
2-tuple explanation when a 1-tuple one exists. The `SUCH THAT` constraint is
immune (a sum of non-negative terms equal to zero is multiplicity-invariant);
only objectives and non-zero-RHS aggregates are affected.

**Syntax proposed by the paper (§3.2).** A **relation-qualified reducer**: the aggregate
carries a bound relation name or alias, and reduces over that relation's tuple identities
instead of over join-result rows.

```sql
MAXIMIZE SUM(D: opening_cost * open) - SUM(unit_cost * ship)
```

`SUM(D: expr)` projects the currently surviving rows onto the tuple identities of `D`,
drops the repetitions the join introduced, and contributes one term per remaining `D`
tuple. Unqualified reducers keep today's semantics (one term per join-result row), so this
is opt-in and nothing existing changes meaning. If the query does not alias the relation,
the qualifier is the table name (`Depots:`).

Semantics points the draft settles, worth preserving when this is built:
- **Not `SUM(DISTINCT expr)`.** Identity is the tuple, not the value: two different depots
  with equal `opening_cost` are two terms, not one.
- **Scope is the surviving rows, not the base table.** The join and `WHERE` decide which
  `D` tuples contribute. A depot the join or a filter removed has no decision variable at
  all, so a base-table scan would emit terms for variables that do not exist and would
  silently undo the query's filters.
- **Well-formedness: everything inside the reducer comes from the qualified relation.**
  §3.2.2: "any attribute or decision column used must come from the qualified relation.
  Constants and query-wide decisions are also allowed." The draft's own counter-example is
  `sum(D: opening_cost + unit_cost * ship)`, rejected because `unit_cost` and `ship` are
  `T`'s — "DeciDB rejects this expression rather than choosing an arbitrary route for each
  depot". This **settles what was logged here as an open edge case**: the answer is reject
  at bind time, never pick a row of the group. Note the explicit carve-out for query-wide
  (`scalar`) decisions, which ties this to the `scalar` entry above.
- **Qualifiers may name an intermediate join, not just a base relation.** §3.2.2 allows
  `sum(D,T: …)`, "which maps to `Depots join Routes`", alongside the single-relation
  `D:` / `Depots:` forms. The reduction key is then the composite tuple identity of that
  join, and the well-formedness rule widens to "any attribute must come from one of the
  named relations". Grammar-wise this makes the qualifier a *list* of `ColId`, so design it
  as a list from the start rather than retrofitting one.
- **Construction order is fixed.** §3.2.2 pins it: `when` selection → `per` partitioning →
  qualifier-key grouping and de-duplication → aggregation. "First, `when` selects the
  relevant rows. Next, `per` divides them into the groups for which separate expressions
  are generated. A qualifier such as `D:` then specifies the tuple-identity key by which the
  reducer's input is grouped and de-duplicated. Finally, the reducer aggregates one
  contribution from each tuple identity." The de-duplication stage is new — there is
  nothing in the current pipeline to extend, so this is where the stage gets inserted, and
  it must sit *inside* the PER partition (de-duplicate within a group, not across the
  query).

**Earlier options considered** (superseded by the paper's syntax above, kept for rationale):
1. `COUNT(DISTINCT keepS)` / `SUM(DISTINCT ...)` — familiar SQL surface, but
   `DISTINCT` on a decision variable means "distinct *entity*", not "distinct
   *value*", which reads wrong.
2. An explicit entity qualifier on the aggregate, e.g. `SUM(keepS) PER ENTITY`
   or `SUM(S.keepS)` where the table prefix at *use* site selects entity
   scope — states the binding rather than implying it.
3. No new syntax: document the multiplicity trap and the reciprocal-weight
   workaround below. Zero implementation cost.

**Reciprocal-weight workaround (verified working).** Entity-level counting *is*
expressible today without new syntax: precompute each entity's join-result
degree in a CTE and weight its variable by `1/degree`, so the `n` copies of a
shared variable sum back to exactly 1.

```sql
WITH J AS (SELECT A.sensorID, A.alertType FROM Alerts A JOIN ... ),
     sw AS (SELECT sensorID,  1.0/COUNT(*) AS ws FROM J GROUP BY sensorID),
     pw AS (SELECT alertType, 1.0/COUNT(*) AS wp FROM J GROUP BY alertType)
SELECT ... FROM Alerts A JOIN Policy P ON ... JOIN Sensors S ON ...
     JOIN sw ON sw.sensorID = S.sensorID JOIN pw ON pw.alertType = P.alertType
DECIDE S.keepS(BOOL), P.keepP(BOOL)
SUCH THAT SUM(A.falseAlarm * keepP * keepS) = 0
MAXIMIZE SUM(keepS * sw.ws) + SUM(keepP * pw.wp);
```

Three caveats keep this a workaround rather than a fix: the degree CTE must
replicate the DECIDE block's joins *and* filters exactly or the weights silently
desynchronize; `1.0/3` summed three times gives `1.9999999999999998`, harmless
against integer-1 differences but real; and the whole construction needs a
paragraph of explanation at every use site.

**Implementation sketch.** The qualifier is new surface syntax, so this now starts at the
grammar: `SUM(D: expr)` — and `SUM(D,T: expr)` — inside an aggregate argument
(`third_party/libpg_query/grammar/statements/select.y`, `make grammar-build`), then the
binder resolves each name in the qualifier list against the query's bound relations,
rejects an unknown or unbound alias, and enforces the well-formedness rule above.
Downstream, the entity→variable-index map already exists — `VarIndexer`
(`src/include/duckdb/decidb/ilp_model.hpp`) and the Phase 1.5 entity mapping in
`src/execution/operator/decide/physical_decide.cpp`. Entity-level aggregation is
coefficient accumulation that visits each entity key once instead of once per
row, so it is a coefficient-building change, not a new solver construct.

**Sequencing**: larger than the rest of the stage-1 grammar batch and not a prerequisite
for any of it. The grammar edit is small; the de-duplication stage and the multi-relation
key are the work. Land it after the clause-order / declaration-form / `scalar` batch.

**Test**: extend `test/decide/tests/test_entity_scope.py` with an oracle case
where entity degrees differ, so the row-weighted and entity-weighted optima
diverge (the assertion at line 79 pins the current row-weighted behavior and
must keep passing for the non-entity form).

**Done file**: `done.md` → "Aggregate semantics with table scope" (extend, don't
replace — both semantics will coexist), plus
`../../00_project_overview/syntax_reference.md` §2.1.

*Discovered 2026-07-26, reviewing Example 2.1 of the CIDR'27 paper draft.*

## Query-wide `scalar` decision scope

**Priority: High — the paper's Example 1 depends on it; code follows the paper.**

The draft declares `scalar max_shortfall(INT)` and describes it as one variable
for the whole query, distinct from row-scoped and table-scoped decisions
(paper §2.1, §3.4). DeciDB has **two** scopes today, not three: unqualified
names are row-scoped (one variable per result row) and qualified names are
table-scoped (one per entity). There is no `SCALAR` token in `select.y`,
`syntax_reference.md:7` documents only `DECIDE [Table.]variable_name [IS type]`,
and `done.md` describes only the two scopes.

This is a semantic addition, not a spelling:
a third scope needs a variable-indexing case (`VarIndexer`,
`src/decidb/utility/ilp_model_builder.cpp:19-51`, which currently branches on
row-scoped vs. entity-scoped), an output-column rule (the value repeats across
every result row), and aggregate semantics (a query-wide decision inside `SUM`
must not multiply by row count).

**Decision settled by the submitted draft**: `scalar` is a **keyword**, not an inference.
§3.1 writes `decide scalar max_shortfall(INT)` and describes the keyword as what "declares
one query-wide decision shared by all rows". The alternative once considered here —
inferring query-wide scope from an unqualified declaration with no row dependence — is
ruled out: it would silently reclassify existing row-scoped declarations whose constraints
happen not to reference row data.

**Unblocked**: the parenthesized type form has shipped, so the draft's spelling
`scalar name(TYPE)` is already expressible. The semantics — a third
`VarIndexer` case, the output-column repeat rule, and "must not multiply by row count"
inside a reducer — are the work, and are independent of the two orders in "Clause order".

**Test**: a query whose objective mixes a query-wide decision with a row-scoped
one; assert one solver column for the former regardless of input cardinality.

**Docs on completion**: `../../00_project_overview/syntax_reference.md` §1-§2,
`done.md` (variable scope).

**Raised**: 2026-07-30, writing paper §3.2 (parser subsection). Re-verified 2026-08-07:
`decide T.ship(INT), scalar mx(INT)` → `syntax error at or near "mx"`.
