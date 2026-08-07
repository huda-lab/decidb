# PER Keyword — Planned Features

---

## Row-Varying RHS with PER

**Priority: High — paper-facing. This is the shape of the CIDR'27 running example
(Example 1, lines 8–9), which §3 and §5 of the draft both build on, and it does not
bind today.**

```sql
-- NOT YET SUPPORTED
SUM(x * hours) <= max_hours PER empID

-- The paper's Example 1, same shape — both lines rejected today:
SUM(ship) <= stock  PER depotID
SUM(ship) >= demand WHEN (priority = 'critical') PER regionID
```

Where `max_hours` varies per group. Requires resolving which row's value to use per group (e.g., validate all rows in a group have the same value, or take the first). Users can work around this today by using multiple WHEN constraints with explicit values.

**Where it is rejected**: `src/planner/expression_binder/decide_constraints_binder.cpp:207`
— an aggregate LHS requires the RHS to be a scalar or an aggregate, so a bare
(non-reduced) column RHS fails with *"SUM cannot be compared to an expression that is
not a scalar or aggregate without DECIDE variables"*. The check runs before PER is
considered, so it fires whether or not a PER key would make the RHS single-valued
within its partition. The draft's §3.2.1 rule — every non-reduced attribute referenced
by the constraint must have a single value within its partition — is exactly the
validation this task needs to implement.

*Priority raised 2026-08-04 during paper §5 review, after confirming against
`build/release/decidb` that neither line of Example 1 binds.*

---

## Open Design Question: PER binds the inner aggregate by convention, not by syntax

**Priority: Low — open question, no code change proposed yet. Needs a decision before it is picked up.**

In a nested-aggregate objective, PER is postfix on the whole clause:

```sql
MINIMIZE MAX(SUM(keep * l_extendedprice)) PER l_returnflag
```

Nothing in the syntax says which of the two aggregates PER groups. The rule is a convention documented in `../../00_project_overview/syntax_reference.md` §7.2: the **inner** aggregate is computed within each group, the **outer** aggregate ranges across groups. The alternative placement — PER inside the brackets, attached to the aggregate it actually modifies — would state the binding rather than imply it:

```sql
-- NOT the current syntax
MINIMIZE MAX(SUM(keep * l_extendedprice) PER l_returnflag)
```

**Why the convention holds up today**: exactly two nesting levels are supported, so "inner is grouped" is unambiguous. It also matches SQL, where GROUP BY is a trailing clause and never an aggregate argument — the nested form is the desugaring of `SELECT MAX(s) FROM (SELECT SUM(x) s FROM t GROUP BY flag)` with the subquery flattened. And it keeps one grammar shape across both sites: `a_expr PER columnref_opt_indirection` at `third_party/libpg_query/grammar/statements/select.y:259` (objective) and `:340` (constraint), mirroring postfix WHEN.

**Where it strains**:
- A third nesting level would have no way to say which aggregate PER binds. Any future extension past two levels forces this decision.
- Flat `MIN(expr) PER col` / `MAX(expr) PER col` has to be a hand-written rejection (§7.2) rather than being unparseable — with no inner aggregate, trailing PER has nothing to attach to, but the grammar accepts it anyway and the error has to be raised later.

**Cost of moving PER inside the brackets**: one keyword would have two placements (trailing in constraints, nested in objectives), and WHEN + PER composition breaks apart — `MINIMIZE MAX(SUM(x)) WHEN active PER emp` currently reads as one postfix tail, but WHEN would stay outside while PER moved inside.

**Decision needed**: keep trailing PER and document the inner-binding rule more prominently, or accept the split placement to make the binding explicit. Recommendation on current evidence is to keep trailing PER (the SQL analogy is the stronger constraint) and revisit only if nesting beyond two levels is ever pursued — see `../maximize_minimize/done.md` for the two-level inner/outer auxiliary formulation that would have to generalize.

**Raised**: 2026-07-26, from a question about why PER sits outside the brackets in benchmark Q4 (`benchmark/decide/queries/q4_minmax_nested.sql`).
