# Stage 02 — Binder: open work

Language-surface work is owned by `../../03_expressivity/` (the scopes:
`../../03_expressivity/scope/`); what the language does not yet accept is listed in
`../../00_project_overview/syntax_reference.md` §10.2, and the scope suites in
`../../04_testing/scope/done.md`.

---

## 1. Bind-time refusals a later change may lift

Each is refused here, at bind time, with a message that says "not supported yet". None
is a wrong answer; each is a shape the later layers do not formulate.

| Refused | Message | What lifting it needs |
|---|---|---|
| `norm` inside a reducer (D74) | *"DECIDE: norm inside a reducer is not supported yet"* | a norm's lowering (stage 05) whose sum or maximum is itself a member, a helper over its auxiliaries |
| A reducer over data alone added inside one over decisions (`sum(x + max(t.v) by (t.g))`) | *"DECIDE: max(...) over data alone inside a reducer over decisions is not supported yet; compute it in a subquery, or reduce it outside"* | a data reducer's value per path group read into a helper's member constants at execution (`EvaluateReducerValueLinks`), as the bound side reads one |
| A window function in a `when`, a key or a `by` | *"DECIDE: a window function such as 'row_number() OVER ()' is not supported in when, per or by"* | a window evaluated over the rows the scope keeps, before the scope reads it |
| `norm(D: e, p)` | *"DECIDE: norm(D: ...) is not supported yet; a relation qualifier takes SUM, AVG, MIN or MAX"* | the norm lowering (stage 05) carrying the qualifier's key scope onto the reducer it builds |
| A frame's payload or fallback that is not linear in the decisions (`at(previous: abs(x - 3)) over (...)`) | *"DECIDE: 'abs((x - 3))' inside at(...) is not linear in the decisions, which is not supported yet; write it outside the frame"* | the rows a rewrite lowers such a payload into (an ABS envelope) stated at the row the frame selects, not the anchor; for `abs` without an `else` the user's own `abs(at(...))` is already equivalent |
| A fallback that reads a decision beside a payload that reads none (`at(previous else s: t.demand) over (...)`) | *"DECIDE: in at(previous else s: t.demand) over (t.period), else s reads a decision while the payload reads none, which is not supported yet"* | the fallback visible to the layers below as the payload is: a marker whose children are both, so a frame over data with a decision `else` is a frame over decisions (a point read in place, a range a helper per window) |
| An aggregate among an `IN` list's values (`x IN (1, sum(cap))`) (A05) | *"DECIDE: an IN value cannot be an aggregate such as 'sum(cap)'; compute it in a subquery or a CTE and list that value"* | each value read as a bound reads one (`x <= sum(cap)`): one value per group of the clause's kept rows, evaluated at execution into the coefficient of its indicator (`RewriteInDomain`'s linking row), as `EvaluateRhsReducerPerGroup` evaluates a bound side |
| A strict comparison over a `MIN` or `MAX` whose value can be fractional (`max(x + 0.5) > 3`) | *"Comparison '>' is not supported here: 'max(x + 0.5)' has type DECIMAL(21,1) ... Compare with '>=' instead."* | moving the constant addend onto the bound in stage 04, the shape owner; done in stage 05 it leaves the canonical contract |

**Decision needed.** Which of these enter the language, and in what order.

**Test.** For each lifted shape, an oracle-judged test on both solvers
(`_scope_oracle.py` or `_brute_force.py`). The refusals are pinned in
`test_scope_generation.py`, `test_scope_reducer_inner.py`, `test_scope_reducer_by.py`
and `test_anr_nested.py`; a lifted shape's pin becomes its answer test.

**Update.** `done.md` §4a, `../../00_project_overview/syntax_reference.md` §10.2.

---

## 2. A reducer in a `when`, a key or a `by` is refused at execution

**Today.** `ValidateScopeData` refuses a decision and a window function in a `when`
condition, a `per` key or a `by` key, but not a reducer. `when sum(cap) > 1: x <= 3` and
`per g: sum(x) by (sum(cap)) <= 3` bind, and execution refuses them from
`TransformToChunkExpression` with *"DECIDE: an aggregate cannot be used here. Reducers
are allowed on either side of a constraint and in the objective, not inside a WHEN
condition or a coefficient."*: no caret, an upper-case `WHEN`, and a layer-8 refusal of a
question the query text answers.

**Work.** Refuse a reducer in `ValidateScopeData`, at bind time, with a caret on the
reducer and the scope words in lower case.

**Test.** Both queries above refused at bind time, and a data-only reducer on a bound
side still accepted (`test_scope_bare_clauses.py`).

**Update.** `done.md` §4a (the `when`, `per` and `by` data rule).

## 3. Messages the review of 2026-10-29 found off

- `MAXIMIZE t.w * max(per t.g: sum(x) by (t.g))` renders the nest `MAX(SUM(x))`, its
  `per` and `by` dropped, and suggests `SUM(x * w)`, which does not apply to a nest.
- `MINIMIZE max(x) + max(per t.g: sum(x) by (t.g))` is refused as *"... beside MIN or MAX
  is not supported yet"*, against §4's "a nest ... beside other terms" (§10.2 lists only a
  decision beside a MIN/MAX).
- `MAXIMIZE -max(power(x - t.v, 2))` says "beside another term" where there is none.
- A quoted `"previous"`, `"every"` or `"cyclic"` is read as the frame word: the lexer
  does not tell a quoted identifier from a bare one.
- `MAXIMIZE when t.id = 1: w * sum(x)` and `... : x + w` are refused for `w` varying per
  row although the `when` keeps one row, which §4 and §5 accept ("one value over the kept
  rows").

**Update.** `done.md`, `../../00_project_overview/syntax_reference.md` §4, §10.2.
