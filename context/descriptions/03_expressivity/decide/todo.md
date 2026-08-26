# DECIDE Clause — Planned Features

---

## Row-scoped decisions inside a relation-qualified reducer

**Deferred by the user on 2026-08-08 when the single-relation qualifier shipped.** Rejected
today:

```
Binder Error: 'y' is not a decision of D, so SUM(D: ...) cannot use it; declare it as
D.y(...) or move that term into its own reducer
```

The rejection is currently the *only* sound answer available: a row-scoped decision has one
variable per join-result row, so de-duplicating rows by `D`'s tuple identity would silently
pick one row's variable and drop the others' — an arbitrary choice, which is exactly what
§3.2.2's well-formedness rule exists to prevent.

**The open question is what it should mean instead**, if anything. Three readings, none
obviously right:

1. **Reject permanently** — the current behavior, and consistent with the rule as written.
   The workaround (declare the decision on the qualified relation) is a one-word edit and is
   what the error suggests.
2. **Sum the group's row-scoped variables before de-duplicating** — `SUM(D: c * y)` becomes
   one term per depot whose coefficient multiplies the *sum* of that depot's `y` variables.
   Well-defined, but it makes the qualifier mean two different things depending on the
   operand's scope, and nothing in the paper suggests it.
3. **Treat it as an error only when the reducer would actually be ambiguous** — allow it when
   the qualifier's key happens to determine the row (a 1:1 join). Rejected as an idea for
   `PER` already, since feasibility would then depend on data rather than on the query.

The same question **no longer applies to a query-wide (`scalar`) decision** — batch D
settled that one on 2026-08-25. A scalar is row-invariant, so it contributes the same value
to every tuple no matter which relation is said to own it; `SUM(D: opening_cost * cap)` is
legal and weights `cap` by the sum of the distinct depots' `opening_cost`. Only a *whole*
body that is row-invariant is still refused (`SUM(D: cap)`), and that is the general
row-invariance rule rather than anything the qualifier adds. This also closes what used to
be a knowing divergence from paper §3.2.2, which carves out query-wide decisions as always
allowed inside a qualified reducer. See `../sql_functions/done.md` → "A Query-Wide Decision
Multiplied by a Vector, Inside a Reducer (Batch D)".

**Test**: `test_qualified_reducer.py::test_row_scoped_decision_inside_qualified_reducer_rejected`
pins the current rejection.

**Done file**: `done.md` → "Relation-qualified reducers".

---

## EXPLAIN renders a qualified reducer identically to an unqualified one

`EXPLAIN` prints `sum((opening_cost * open))` for both `SUM(D: opening_cost * open)` and
`SUM(opening_cost * open)`, so the plan gives no way to tell two queries with different
optima apart. The tag itself does not leak into the output (it lives on the aggregate's
`alias`, which `ToString()` does not print) — the qualifier is simply not rendered.

Unlike `WHEN` and `PER`, which `CollectTaggedExpressionStrings`
(`src/planner/operator/logical_decide.cpp`) unwraps into postfix suffixes, the qualifier
sits *on* the aggregate node rather than above it, so surfacing it means intercepting the
aggregate's own rendering rather than appending a suffix. Cosmetic, but it costs a reader
the one detail that distinguishes the two plans.

*Discovered 2026-08-08 while shipping the single-relation qualifier.*
