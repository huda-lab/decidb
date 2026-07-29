# DECIDE Clause — Planned Features

---

## Entity-level aggregation over table-scoped variables

**Priority: Medium — open design question, no syntax proposed yet. Needs a decision before it is picked up.**

A table-scoped variable (`DECIDE Sensors.keepS IS BOOLEAN`) yields one solver
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

**Decision needed — how to express it.** Options, not yet chosen:
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
DECIDE S.keepS IS BOOLEAN, P.keepP IS BOOLEAN
SUCH THAT SUM(A.falseAlarm * keepP * keepS) = 0
MAXIMIZE SUM(keepS * sw.ws) + SUM(keepP * pw.wp);
```

Three caveats keep this a workaround rather than a fix: the degree CTE must
replicate the DECIDE block's joins *and* filters exactly or the weights silently
desynchronize; `1.0/3` summed three times gives `1.9999999999999998`, harmless
against integer-1 differences but real; and the whole construction needs a
paragraph of explanation at every use site.

**Implementation sketch** (once syntax is fixed): the entity→variable-index map
already exists — `VarIndexer` (`src/include/duckdb/decidb/ilp_model.hpp`) and the
Phase 1.5 entity mapping in
`src/execution/operator/decide/physical_decide.cpp`. Entity-level aggregation is
coefficient accumulation that visits each entity key once instead of once per
row, so it is a coefficient-building change, not a new solver construct.

**Test**: extend `test/decide/tests/test_entity_scope.py` with an oracle case
where entity degrees differ, so the row-weighted and entity-weighted optima
diverge (the assertion at line 79 pins the current row-weighted behavior and
must keep passing for the non-entity form).

**Done file**: `done.md` → "Aggregate semantics with table scope" (extend, don't
replace — both semantics will coexist), plus
`../../00_project_overview/syntax_reference.md` §2.1.

*Discovered 2026-07-26, reviewing Example 2.1 of the CIDR'27 paper draft.*

---

## Signed decision variables — finite negative bounds: DONE; free (-∞) domain: deferred


**Deferred — fully-free ($-\infty \ldots +\infty$) domain.** Out of scope by
design: a signed variable always has a finite lower bound. A truly
unbounded-below variable (e.g. `x <= 10` meaning `(-inf, 10]`) is the case most
likely to make objectives unbounded, so it is not expressible without a future
opt-in (`FREE`/`IS REAL UNBOUNDED` keyword). Two known smaller gaps left for
later: (1) **column-valued** `IN` domains with negative data values are not
auto-widened (only constant literals are); (2) a signed variable in a bilinear
product needs an *explicit* upper bound (implied-bound propagation skips signed
variables).

**Diagnostics interaction.** The unbounded diagnosis reports an escape
`direction` (`+∞` / `-∞`). Because signed variables still have a finite lower
bound, no variable is unbounded *below*, so downward escape remains unreachable
and `direction` is still always `+∞`. The `-∞` branch only becomes testable if
the deferred free-domain work lands. See
`08_query_diagnostics/unbounded/todo.md` (direction / downward escape).
