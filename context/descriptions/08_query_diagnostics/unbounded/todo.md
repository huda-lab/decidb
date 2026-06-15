# Query Diagnostics — Unbounded (planned)

The objective improves without limit — the infeasible machine run backwards:
region too *open* in the improving direction. The *fix* is forced (the user must
add a bound or fix a sign error — you can't relax your way out), but the
*diagnosis* can be rich: name the exact variables escaping to infinity.

## Checklist

- [ ] **U1 · HiGHS INF_OR_UNBD disambiguation** — deps: F1
- [ ] **U2 · Ray extraction** — deps: F1
- [ ] **U3 · Ray→SQL mapping + reporting** — deps: U2, F2, F4, F5

> **Already done (Gurobi):** INF_OR_UNBD disambiguation via `DualReductions=0`
> re-solve (`gurobi_solver.cpp:202-219`). See `done.md`.

---

## U1 · HiGHS INF_OR_UNBD disambiguation

**Goal.** Gurobi already disambiguates (above). Provide the portable equivalent
for HiGHS: the **objective=0 probe** — re-solve with the objective replaced by 0;
feasible ⇒ unbounded, infeasible ⇒ route to `infeasible/`.

- 🔬 **Probe:** does HiGHS emit a `kUnboundedOrInfeasible`-style ambiguous status
  today (the naive path only checks kInfeasible / kUnbounded / kTimeLimit), and
  does the obj=0 probe resolve it?

**Deps:** F1.

---

## U2 · Ray extraction

**Goal.** Extract the unbounded ray `d` — the feasible improving direction
(`Ad ≤ 0`, `d ≥ 0`, `cᵀd > 0` for MAXIMIZE); usually only a small subset of
variables have `dᵢ > 0`.

- Native APIs first: Gurobi `UnbdRay` (needs `InfUnbdInfo`), HiGHS
  `getPrimalRay`.
- **Portable fallback we own:** solve `max cᵀd s.t. Ad ≤ 0, 0 ≤ d ≤ 1`; if the
  optimum > 0, `d` is a ray.
- For MILP, ray analysis on the LP relaxation suffices (integer unboundedness ⇒
  the relaxation is unbounded along the same direction).
- 🔬 **Probe:** confirm `UnbdRay` / `getPrimalRay` availability + exact signatures
  on both backends.

**Test.** Fallback ray agrees with native; correct on constructed unbounded
inputs.

**Deps:** F1.

---

## U3 · Ray→SQL mapping + reporting

**Goal.** Turn the ray into a user-facing diagnosis.

- Collect variables with `dᵢ > 0`; map to SQL names via F2 provenance. If a ray
  variable is an auxiliary (`MAX(expr)` z, ABS aux), trace back to the user's
  original expression, not the internal name.
- **DecidB narrows suspects for free:** only `IS INTEGER` / `IS REAL` can escape
  (BOOLEAN locked to [0,1]); all vars non-negative so escape is +∞; McCormick
  factors have a finite UB so can't be the source. Mechanical culprit test:
  MAXIMIZE → positive objective coefficient with no upper cap.
- **Report:** escaping variables, likely cause ("missing capacity/budget
  constraint or objective sign error"), prescription "add an upper bound."
  **Never pick the bound value** — any finite bound works; the right number is
  domain knowledge. Open: whether to *suggest* an example value (e.g. the largest
  RHS in scope) or stay silent to avoid a bad anchor.

**Test (differential).** Correct escaping variables named; `INF_OR_UNBD` routes
correctly; aux vars trace to user expressions.

**Deps:** U2, F2, F4, F5.
