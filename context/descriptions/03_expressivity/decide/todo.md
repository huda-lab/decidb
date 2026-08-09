# DECIDE Clause — Planned Features

---

## Multi-relation qualifiers — `sum(D,T: expr)`

**Deferred by the user on 2026-08-08 when the single-relation qualifier shipped.** Rejected
today with a message naming the supported form:

```
Parser Error: a reducer can be qualified by one relation only; write sum(D: ...) and join
the other relation's terms in a separate reducer
```

Paper §3.2.2 allows a qualifier naming an intermediate join — `sum(D,T: …)`, "which maps to
`Depots join Routes`". The reduction key is then the composite tuple identity of that join,
and the well-formedness rule widens to "any attribute must come from one of the named
relations".

**What is already in place.** The grammar parses the qualifier as a `func_arg_list`, so
`D,T` already reaches the action as a two-element list — the rejection is a length check,
not a parser limitation. `EntityScopeInfo` holds a *vector* of key bindings, so a composite
key is concatenation of the two relations' bindings rather than a new structure. Both were
built this way deliberately so the multi-relation case is an extension, not a rewrite.

**What is missing.**
1. Drop the length check in the grammar action (`select.y`, the `func_name '(' func_arg_list
   ':' func_arg_list ')'` alternative) and carry the whole list through
   `PG_AEXPR_QUALIFIED_REDUCER` — the transform currently takes `root.rexpr` as one node and
   would take a `PGList`, mirroring how `PER_CONSTRAINT_TAG` already handles multi-column
   `PER`.
2. Generalize `FindOrCreateEntityScope` to key on a *list* of relation names and build a
   concatenated `entity_key_bindings` / `entity_key_column_types`. The `table_scope_map` key
   becomes the joined name list.
3. Widen `CheckQualifiedReducerBody` to accept a column or entity-scoped decision from any
   of the named relations.
4. Nothing changes in execution: the mask machinery keys on the resulting `EntityMapping`
   and does not care how many relations produced the key.

**Decision needed first.** Whether `sum(D,T: ...)` must name relations that are actually
joined to each other in the query, or whether any set of bound relations is allowed (a
cross product would then define the identity). The paper's example is a join.

**Test**: extend `test/decide/tests/test_qualified_reducer.py` with an oracle case over a
two-relation key whose composite degree differs from either relation's own degree.

**Done file**: `done.md` → "Relation-qualified reducers", plus
`../../00_project_overview/syntax_reference.md` §5.1.

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

Same question applies to a query-wide (`scalar`) decision inside a qualified reducer, which
is also rejected today. Note this **diverges from the paper**: §3.2.2 explicitly carves out
query-wide decisions as always allowed inside a qualified reducer "because they do not vary
across rows". The user chose rejection on 2026-08-08 so that `scalar` behaves the same way
inside a qualified reducer as it does inside an unqualified one (see `done.md` → "Reducers
are rejected"), rather than being allowed in one and refused in the other. If the carve-out
is restored later, the natural reading is coefficient = number of distinct tuple identities,
which the qualifier makes unambiguous in a way bare `SUM(cap)` is not.

**Test**: `test_qualified_reducer.py::test_row_scoped_decision_inside_qualified_reducer_rejected`
and `::test_query_wide_decision_inside_qualified_reducer_rejected` pin the current rejections.

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
