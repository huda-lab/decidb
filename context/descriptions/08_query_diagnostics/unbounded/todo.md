# Query Diagnostics — Unbounded (planned)

The objective improves without limit — the infeasible machine run backwards:
region too *open* in the improving direction. The *fix* is forced (the user must
add a bound or fix a sign error — you can't relax your way out), but the
*diagnosis* can be rich: name the exact variables escaping to infinity.

## Checklist

- [x] **U1 · HiGHS INF_OR_UNBD disambiguation** (obj=0 probe) — deps: F1 — ✅ **done**, see `done.md`
- [ ] **U2 · Ray extraction** (portable fallback only) — deps: F1
- [ ] **U3 · Ray→SQL mapping + reporting** (full output) — deps: U2, F6, F2, F4, F5

> INF_OR_UNBD disambiguation is complete for both backends: Gurobi via
> `DualReductions=0`, HiGHS via U1's zero-objective probe. See `done.md`.

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
- **Shared reporting surface (why F2 is a dep):** U3 emits through the unified
  **F5** relation, not a one-off variable-only format, so every diagnosis state
  renders consistently — and **F5 needs F2** for clause-level labels. Unbounded's
  *own* content is variable-centric (F6 names the escaping vars), so F2's primary
  payoff here is **building + battle-testing the shared F2+F5 reporting stack on
  the simpler unbounded case** before the infeasible engine relies on it.
  *Optional enrichment F2 enables (not v1):* a constraint-aware line — "clause N
  references escaping var x but never bounds it."

**Test (differential).** Correct escaping variables named — user vars AND aux vars
traced to their source expression; `INF_OR_UNBD` routes correctly.

**Deps:** U2, **F6** (variable provenance — full, incl. aux→expression),
**F2** (constraint provenance — supplies F5's clause labels; see note above),
F4, F5.
