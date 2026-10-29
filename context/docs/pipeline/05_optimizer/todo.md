# Stage 05 — Optimizer: open work

Each item makes the model smaller or the language wider without changing any answer that runs.

## OPT-1 — A box per helper instance

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

## OPT-2 — `IN` under `per K:` once per class

**Why.** `RewriteInDomain` keeps the clause's `when` on the rows it emits but not its
`per K:`, so `per g: x IN (1, 2)` over a keyed `x` emits one indicator set per kept row
rather than per class: three rows in two classes give six indicators where four would
do. It is exact, because the key determines the decision and the domain.

**Decision needed.** None in the language; one in the design, below. The indicators are row-scoped auxiliaries;
emitting them per class needs them keyed by `K` (a scope like a helper's) and the `per K:`
wrapper kept on the generated rows, as the MIN/MAX equality's easy half keeps it.

**Design choice.** Which scope keys the indicators. Per class, the indicators need a decision scope keyed by
the wrapper's `K`. A scope is made only by the binder (declarations, reducer keys) and by the value rewrite (helpers);
every key, `K` included, is an entry of the one key table (`LogicalDecide::keys`), from which a scope can be made.
(a) Key the indicators by `K`'s entry. (b) For the case where `K` is the decision's own declared key only: the
indicators take the decision's scope and the rows keep the wrapper (as the MIN/MAX easy half keeps it); any other `K`
keeps one set per row. Recommended: (a); (b) is a special case beside the one key table, which (a) reads directly.

**Test.** The dump of `per g: x IN (1, 2)` over three rows in two classes shows four
indicators and four rows (a pick-one row and a linking row per class), not six and six;
DIAGNOSE still names the clause.

**done.md.** "NORM and IN — `RewriteNorm` / `RewriteInDomain`".

## OPT-3 — A quadratic SUM behind a helper decision

**Why.** Lifting the quadratic-row refusal (`sum(power(x, 2)) + gcap * s <= 30`, `../../issues/bugs.md`
§ Limitations (not bugs), ruled: lift it) means a helper `v` for a quadratic `SUM`. Every `SUM` helper is defined by an
equality, `v = Σ e`, which for a square is a nonconvex quadratic equality, so a convex clause would become a nonconvex
model (Gurobi spatial branch and bound; HiGHS refuses either way).

**Decision needed.**
1. How is the helper defined? (a) Equality, emitted through the quadratic-row path: simplest, but the model class
   changes. (b) The epigraph form, the side the rows push against, as `MIN`/`MAX` helpers already do
   (`need_envelope`/`need_closing`): `v >= Σ e` when the rows push the term down, `v <= Σ e` when up, the equality
   only for `=`, `<>` or an unknown sign. The clause keeps its convexity, and nothing reads a helper's value back, so a
   slack `v` is harmless. Recommended: (b), applied to every `SUM`/`AVG` helper, linear ones included, so there is
   one rule; linear models change from `=` to `>=`/`<=` rows on one side (same answers, golden dumps that print them
   change).
2. Apply (b) to linear helpers too, or only to quadratic ones? Recommended: all, one rule.

**Ruling (owner, 2026-10-29).** (1) (b), the epigraph form: `v >= Σ e` when the rows push the term down, `v <= Σ e`
when up, the equality only for `=`, `<>` or an unknown sign. (2) Every `SUM`/`AVG` helper, linear ones included: one
rule.

**Test.** The refused shape answers, oracle-judged on Gurobi; HiGHS refuses it. The owner reads the golden diffs
before any regeneration.

**done.md.** `../06_model_formulation/done.md`, "Helper decisions of the value rewrite".
