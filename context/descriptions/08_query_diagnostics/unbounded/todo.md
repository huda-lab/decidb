# Query Diagnostics — Unbounded (planned)

The objective improves without limit — the infeasible machine run backwards:
region too *open* in the improving direction. The *fix* is forced (the user must
add a bound or fix a sign error — you can't relax your way out), but the
*diagnosis* can be rich: name the exact variables escaping to infinity.

## Checklist

- [ ] **U1 · HiGHS INF_OR_UNBD disambiguation** (obj=0 probe) — deps: F1
- [ ] **U2 · Ray extraction** (portable fallback only) — deps: F1
- [ ] **U3 · Ray→SQL mapping + reporting** (full output) — deps: U2, F6, F4, F5

> **Already done (Gurobi):** INF_OR_UNBD disambiguation via `DualReductions=0`
> re-solve (`gurobi_solver.cpp:202-219`). See `done.md`.

---

## U1 · HiGHS INF_OR_UNBD disambiguation (obj=0 probe)

**Goal.** Gurobi already disambiguates (above). Provide the portable equivalent
for HiGHS: the **objective=0 probe** — re-solve with the objective replaced by 0;
feasible ⇒ unbounded, infeasible ⇒ route to `infeasible/`.

**Confirmed by probe (P2/P3/P4):** HiGHS *does* return the ambiguous
`kUnboundedOrInfeasible` (status 9) on MILP-unbounded models, and the naive
backend mishandles it today — it falls into the generic "solver status 9"
catch-all (`deterministic_naive.cpp:235-241`). The obj=0 re-solve disambiguates
cleanly on both backends (feasible ⇒ unbounded), MILP included.
Redundant-but-harmless for LP (HiGHS already returns a definitive `kUnbounded`).

**Build.** Add an explicit `kUnboundedOrInfeasible` branch in
`deterministic_naive.cpp`; on hitting it, re-solve with the objective zeroed and
classify by feasibility.

**Deps:** F1.

---

## U2 · Ray extraction (portable fallback)

**Goal.** Extract the unbounded ray `d` — the feasible improving direction
(`Ad ≤ 0`, `d ≥ 0`, `cᵀd > 0` for MAXIMIZE); the nonzero entries `{i : dᵢ > 0}`
are the escaping variables.

**Decision (probe P5/P6): fallback-only for v1.** Build the ray in our own model
builder; do **not** wire the native vendor ray APIs yet.

- **The fallback we own:** solve the bounded LP `max cᵀd s.t. Ad ≤ 0, 0 ≤ d ≤ 1`
  (homogenize each original row by sense: `≤` → `a·d ≤ 0`, `≥` → `a·d ≥ 0`,
  `=` → `a·d = 0`); if the optimum > 0, `d` is a ray. One extra solve, only on the
  opt-in unbounded path.
- **Why fallback-only:** identical on both backends (no vendor quirks), and P6
  showed it returns a *fuller-support* ray than native extreme rays — it names
  *every* escaping variable, where a native extreme ray can name only one
  (degeneracy). It also sidesteps Gurobi `UnbdRay`'s MIP gap (errors 10005 on
  integer models) and HiGHS's row-less / QP caveats. See `probe_findings.md`
  P5/P6.
- For MILP the box LP is naturally the LP-relaxation ray (integer unboundedness ⇒
  the relaxation is unbounded along the same direction).

**Later (not v1):** native rays as an *accelerator* only if profiling shows the
extra solve matters — Gurobi `UnbdRay` + `InfUnbdInfo` gated to continuous models,
HiGHS `getPrimalRay`. Logged so we don't forget; not built now.

**Test (differential vs `oracle_solver`).** On constructed unbounded inputs the
fallback yields optimum > 0 and `d`'s support equals the true escaping set;
bounded/feasible inputs yield no ray.

**Deps:** F1.

---

## U3 · Ray→SQL mapping + reporting (full output)

**Goal.** Turn the ray into a user-facing diagnosis.

- Collect variables with `dᵢ > 0`; map each to its SQL name via **F6 variable
  provenance**. **Decision: full output** — if a ray variable is an auxiliary
  (`MAX(expr)` z, ABS aux, McCormick / `<>`), trace it back to the user's original
  expression (F6's aux→expression link), not the internal `__abs_aux_N__` name.
- **DecidB narrows suspects for free (verified, P7):** only `IS INTEGER` /
  `IS REAL` can escape (BOOLEAN locked to [0,1]); all *user* vars are non-negative
  so escape is +∞; McCormick factors have a finite UB so can't be the source.
  Mechanical culprit test: MAXIMIZE → positive objective coefficient with no upper
  cap. The candidate filter needs **no new data** — it runs on the model's
  existing bounds / coeffs / types.
- **Report:** escaping variables (fully named), likely cause ("missing
  capacity/budget constraint or objective sign error"), prescription "add an
  upper bound." **Never pick the bound value** — any finite bound works; the right
  number is domain knowledge. Open: whether to *suggest* an example value (e.g.
  the largest RHS in scope) or stay silent to avoid a bad anchor.

**Test (differential).** Correct escaping variables named — user vars AND aux vars
traced to their source expression; `INF_OR_UNBD` routes correctly.

**Deps:** U2, **F6** (variable provenance — full, incl. aux→expression), F4, F5.
