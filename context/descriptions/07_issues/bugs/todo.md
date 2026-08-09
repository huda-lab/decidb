# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

Resolved bugs are moved to `done.md`.

---

## `SUM(MIN(expr)) PER col` is rejected at bind time when the inner expression has more than one term

**Symptom.** A nested PER objective whose *outer* aggregate is `SUM` and whose inner
`MIN`/`MAX` argument is not a single product term fails in the binder, before any model
is built:

```
Binder Error: Nested MIN() inside DECIDE expression must reference a DECIDE variable,
found 'sum((x * (min(l.qty) + __MIN__)))'
```

**Reproduction** (`orders o JOIN lineitem l`, entity-scoped `o.x`, `build/release/decidb`):

```sql
-- fails
MAXIMIZE SUM(MIN((l.qty + 1) * x)) PER o.grp;
MAXIMIZE SUM(MIN(l.qty * x + x)) PER o.grp;
MINIMIZE SUM(MAX((l.qty + 1) * x)) PER o.grp;

-- succeeds — same shape, single-term inner
MAXIMIZE SUM(MIN(l.qty * x)) PER o.grp;

-- succeeds — same multi-term inner under a MIN/MAX outer aggregate
MAXIMIZE MAX(MIN((l.qty + 1) * x)) PER o.grp;
MAXIMIZE MIN(SUM((l.qty + 1) * x)) PER o.grp;
```

**Scope.** Only the outer-`SUM` nested form is affected; outer `MIN`/`MAX` accept the same
inner expression. Both inner aggregates (`MIN` and `MAX`) fail identically.

**Where to look.** The quoted expression is the tell: `sum((x * (min(l.qty) + __MIN__)))`.
The `__MIN__` marker tag has been placed *inside* the coefficient rather than wrapping the
inner aggregate, and the data column has been re-aggregated as a plain SQL `min(l.qty)`.
So the inner argument is being restructured during marker insertion / symbolic round trip
when it carries more than one additive term — the single-term case leaves nothing to
reassociate, which is why it survives. Start at `RewriteMinMaxObjective` in
`src/optimizer/decide/decide_optimizer.cpp` and the `__MIN__`/`__MAX__` marker handling in
`src/decidb/symbolic/decide_symbolic.cpp`; the validation that raises is the
nested-aggregate check in `decide_objective_binder.cpp`.

**Ruled out.** Not a model-build issue — the failure is a `Binder Error`, raised well
before `physical_decide.cpp` runs. Not related to entity scoping: the same rejection
occurs for row-scoped variables.

_Discovered 2026-08-09, while writing regression coverage for the MIN/MAX linking-row fix
(see `done.md` → "MIN/MAX linking rows named the same solver column twice"). Independent
of that fix — it reproduces identically before and after, and on both solver backends,
since it never reaches a solver._
