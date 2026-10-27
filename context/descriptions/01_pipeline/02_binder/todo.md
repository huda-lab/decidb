# Stage 02 — Binder: open work

Language-surface work is owned by `../../03_expressivity/` (the scopes:
`../../03_expressivity/scope/`); what the language does not yet accept is listed in
`../../00_project_overview/syntax_reference.md` §10.2, and the scope suites in
`../../04_testing/scope/done.md`.

---

## 1. Room for nested aggregates

**Design.** A reducer inside a reducer is refused by one gate, in `ValidateScopePlacement`
(`decide_binder.cpp`): *"DECIDE: an aggregate inside an aggregate is not supported yet"*,
except the objective's two-level form. Nothing else in the binder assumes depth one: the
grammar is already recursive (a reducer's argument is a scoped expression that may hold
reducers), and a reducer's scope lives as tags on its own bound aggregate
(`filter`, `__qualified_by_<i>__`, `__group_key_<i>__`), so depth is encoded nowhere else.
Lifting the gate for a shape means the binder admits it and the value rewrite recurses —
an inner helper per inner class, inside the outer group (`../05_optimizer/todo.md`).
Variant signatures (deck pp. 34–40) would turn the "does not determine" error into one
constraint per distinct combination; `LogicalDecide::scope_checks` already lists the parts.

**Decision needed.** Which nested shapes enter the language, starting with deck
example 9 (a reducer nested in a constraint).

**Test.** Deck example 9 and one nested shape per reducer kind, judged against
`test/decide/tests/_brute_force.py` on both solvers; the gate's message kept for every
shape not admitted.

**Update.** `done.md` §4a (the gate), `../../00_project_overview/syntax_reference.md`
§10.2.

---

## 2. Bind-time refusals a later change may lift

Each is refused here, at bind time, with a message that says "not supported yet". None
is a wrong answer; each is a shape the later layers do not formulate.

| Refused | Message | What lifting it needs |
|---|---|---|
| A `when` inside either reducer of the two-level objective | *"DECIDE: when inside the outer reducer of sum(per g: ...) is not supported yet; write it in front of the objective"* (and the inner-reducer twin) | a per-class filter in the two-level rewrite (stage 05) and in the per-class grouping at execution |
| A `per` inside the inner reducer of the two-level objective | *"DECIDE: per inside the inner reducer of sum(per g: max(...) by (g)) is not supported yet"* | the inner reducer's own key scope inside each class, and its DETERMINES check within the class |
| A `by` key that is not the `per` key in the two-level objective | *"DECIDE: in sum(per g: max(...) by (id)) the by key must be the per key; any other is not supported yet"* | inner groups that differ from the outer classes in the two-level rewrite |
| `avg` as the two-level objective's outer reducer | *"DECIDE: avg over the classes of avg(per g: sum(...) by (g)) is not supported yet; write sum, min or max over them"* | an average over classes, whose count is data, in the two-level rewrite |
| A window function in a `when`, a key or a `by` | *"DECIDE: a window function such as 'row_number() OVER ()' is not supported in when, per or by"* | a window evaluated over the rows the scope keeps, before the scope reads it |
| `norm(D: e, p)` | *"DECIDE: norm(D: ...) is not supported yet; a relation qualifier takes SUM, AVG, MIN or MAX"* | the norm lowering (stage 05) carrying the qualifier's key scope onto the reducer it builds |
| A strict comparison over a `MIN` or `MAX` whose value can be fractional (`max(x + 0.5) > 3`) | *"Comparison '>' is not supported here: 'max(x + 0.5)' has type DECIMAL(21,1) ... Compare with '>=' instead."* | moving the constant addend onto the bound in stage 04, the shape owner; done in stage 05 it leaves the canonical contract |

**Decision needed.** Which of these enter the language, and in what order.

**Test.** For each lifted shape, an oracle-judged test on both solvers
(`_scope_oracle.py` or `_brute_force.py`). The refusals are pinned in
`test_scope_objective.py`, `test_scope_explain_diagnose.py`, `test_scope_generation.py`,
`test_scope_reducer_inner.py` and `test_scope_reducer_by.py`; a lifted shape's pin
becomes its answer test.

**Update.** `done.md` §4a, `../../00_project_overview/syntax_reference.md` §10.2.
