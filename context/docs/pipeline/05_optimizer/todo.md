# Stage 05 — Optimizer: open work

Both items make the model smaller or the language wider without changing an answer that runs now.

## 1. A box per helper instance

**Why.** A helper keyed by a `by` key has one instance per group, but one column box: the
cover of every group's reach (`DeriveReducerValueBounds`, stage 06), because the column box
is per decision, not per instance. A small group's instance is boxed as wide as the
largest group's, which loosens the LP and the closing Big-M of a MIN/MAX helper, read off
that box.

**Ruling (owner, 2026-10-28).** Do it now, without waiting for the measurement. It builds on
the per-instance refinement the floors under a `when` use (`SolverInput::instance_lower_bounds`,
`../06_model_formulation/done.md` §2): the decision's box stays the cover of every instance's
box, so passes that read it stay sound.

**Design (architect review and owner, 2026-10-28).**
- A helper's per-instance reach goes in fields of its own (`instance_reach_lower` /
  `instance_reach_upper`), not in the floors' refinement. The floors' values hold against
  any box, the rigid one included; a reach is derived from the members' current boxes and
  holds only for the formulation it came from.
- Readers take a formulation view or a rigid view. The `<>` range collapse keeps the rigid
  view and never reads a derived reach.
- `DeriveReducerValueBounds` writes the reach inside `FormulateModel`, mapping each group to
  its instance through `VarIndexer::InstanceOf`, so a diagnosis re-derives it from widened
  members.
- The builder, the closing Big-M and `FlatColumnBox` read the instance box. The closing
  Big-M's `value_var` blame arm becomes unreachable and goes, with any test that reached
  only it.
- ABS and `power` over a helper read the instance box too. `__body_` helpers
  (`decide_rewrite_define.cpp`) are checked and boxed per instance if they share one box.
- Golden diffs are expected (only an open group's closing goes native); the owner reads them
  before any regeneration.

**Test.** A dump of a helper over groups of different sizes shows each instance boxed by
its own group's reach and a smaller closing Big-M; the answer is unchanged on both
solvers.

**done.md.** `../06_model_formulation/done.md`, "Helper decisions of the value rewrite".

## 2. `IN` under `per K:` once per class

**Why.** `RewriteInDomain` keeps the clause's `when` on the rows it emits but not its
`per K:`, so `per g: x IN (1, 2)` over a keyed `x` emits one indicator set per kept row
rather than per class: three rows in two classes give six indicators where four would
do. It is exact, because the key determines the decision and the domain.

**Decision needed.** None in the language. The indicators are row-scoped auxiliaries;
emitting them per class needs them keyed by `K` (a scope like a helper's) and the `per K:`
wrapper kept on the generated rows, as the MIN/MAX equality's easy half keeps it.

**Test.** The dump of `per g: x IN (1, 2)` over three rows in two classes shows four
indicators and four rows (a pick-one row and a linking row per class), not six and six;
DIAGNOSE still names the clause.

**done.md.** "NORM and IN — `RewriteNorm` / `RewriteInDomain`".
