# Query Diagnostics — Unbounded (how it works)

The objective improves without limit — the feasible region is too *open* in the
improving direction. The fix is forced (the user must add a bound or correct a
sign; you cannot relax your way out), but the diagnosis is rich: it **names the
exact variables escaping to infinity**. Opt-in via `PRAGMA diagnose_decide` =
`unbounded` or `auto`; with no pragma the solve throws the static error unchanged.

This doc describes the shipped behavior, topic by topic. Remaining enrichments are
in `todo.md`. Shared plumbing it builds on (the pragma gate, provenance, the
reporting relation) is in `foundations/done.md`.

## Status disambiguation (`INFEASIBLE` vs `UNBOUNDED`)

A presolve can report the ambiguous "infeasible *or* unbounded" without deciding
which. Both backends are normalized to a definitive status before diagnosis runs:

- **Gurobi** — on `GRB_INF_OR_UNBD`, re-solve once with `DualReductions=0`, which
  forces a definitive `INFEASIBLE` / `UNBOUNDED` (`gurobi_solver.cpp`).
- **HiGHS** — has no MIP disambiguation equivalent. The portable classifier
  re-solves the *same* `SolverModel` on the same backend with the objective
  zeroed out (`diagnostic_solves.cpp`): a feasible probe ⇒ the original was
  `UNBOUNDED`; an infeasible probe ⇒ `INFEASIBLE`; anything else preserves the
  ambiguous status. The zero-objective probe is sense-agnostic (works for
  MAXIMIZE and MINIMIZE).

The solve facade (`ilp_solver.cpp`) builds the model and selects the backend once,
and performs this classification before the operator sees the result.

## Ray extraction (portable box-LP)

To name escapers we need a recession ray (a direction the solver can travel
forever improving the objective). Rather than depend on solver-specific ray APIs,
DeciDB extracts one with a portable LP built over the prepared `SolverModel`
(`BuildUnboundedRayFallbackModel`, `diagnostic_solves.cpp`):

- maximize `signed(c)ᵀ d` (the objective direction),
- preserve each linear row's sense with RHS `0` (homogenized),
- relax all variables to continuous,
- box each direction component to `0 ≤ dᵢ ≤ 1` **only** where the original upper
  bound is effectively infinite (`≥ 1e20`); finite-upper-bound columns are fixed
  to `dᵢ = 0`.

The ray is attached (`SolverResult::ray`) only when this LP is `OPTIMAL` with
signed objective improvement > `1e-8`. It is opt-in (`SolveModelOptions::
extract_unbounded_ray`), pre-armed by the pragma gate only for unbounded/auto, so
the default failure path pays nothing. Quadratic objectives/constraints are out of
scope (no ray extracted; the diagnosis falls back to a detail-less row).

**Suspect filtering is free:** because the box-LP fixes finite-UB columns to 0, a
non-zero ray entry *is already* the type/sign/bound-filtered set of suspects — no
extra filter needed.

## Naming the escaping variables

The ray gives non-zero entries per solver *column*. Each column is mapped back to
the user-facing variable through the `ColumnProvenance` map (variable provenance,
`foundations/done.md`): user columns resolve to the declared variable name, aux
columns to the source expression they were generated from.
`BuildUnboundedDiagnostic` collects columns with `|ray[i]| > 1e-8`, resolves each
through the map, dedups by name, and emits one row per escaping variable.

**Only user variables escape in practice (verified, both backends).** Auxiliary
variables are structurally bounded — ABS Big-M and bilinear McCormick require
finite bounds (they error before the solver), and MIN/MAX/`<>` indicators are
BOOLEAN `[0,1]`. The aux→expression naming is therefore *defensive* (correct if an
aux ever escapes); the practical escapers are user INTEGER/REAL variables.

## The output relation

The diagnosis surfaces through `decide_diagnostics()` (schema in
`foundations/done.md`) as a **variable-centric** relation — one row per escaping
variable:

    query_id | state | variable | direction | group_label | suggested_bound

- `variable` — the escaping variable's name.
- `direction` — the sign of its ray entry. Always `+∞` today: user variables are
  non-negative (`[0, 1e30]`), so escape is always upward. The sign is computed
  from the ray, so a future signed/free variable would report `-∞` — but that
  path is unreachable and untested until signed variables exist (see `todo.md`
  and `03_expressivity/decide/todo.md`).
- `query_id` — ties together the rows of one failed solve.
- `group_label`, `suggested_bound` — reserved, currently NULL (see `todo.md`).

The error thrown points the user at the relation: `SELECT * FROM
decide_diagnostics()`.

## Load-bearing limit — names a variable, not a guilty clause

The ray identifies a *missing* bound. It can name the runaway variable but
**cannot** finger a single guilty clause: a flipped-sign constraint is
mathematically indistinguishable from an absent one. So any clause-level output
can only ever be *context* ("`x` appears in clauses 2, 5; none cap it"), never
blame. This shapes every clause-aware enrichment in `todo.md`.

## Tests

Differential vs `oracle_solver` / pinned scenarios:
`test/decide/tests/test_query_diagnostics_f6.py` (REAL var, INTEGER/MILP via the
HiGHS `INF_OR_UNBD` path, multi-var dedup, `auto` routing, no-pragma silence) ·
`test/common/test_decidb_diagnostic_solves.cpp` (ray fallback: full-support ray,
signed objective, finite-UB zeroing, row-sense preservation, integrality
relaxation, opt-in attachment) · `test/common/test_decidb_variable_provenance.cpp`
(USER/AUX/GLOBAL_AUX resolution). Demoed end-to-end on the TPC-H DB via `run.sh`
(a 3-variable `part` model where only `promo` escapes).
