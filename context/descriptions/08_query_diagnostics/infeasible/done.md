# Query Diagnostics — Infeasible (how it works)

> Router terminal: **failed → infeasible** (`elastic` → report). See `router/README.md`.

The feasible region is empty — no assignment satisfies every `SUCH THAT` constraint at
once. The planned diagnosis (I1+) builds a *second* optimization, the **elastic program**,
whose optimum is the least-change fix (which constraints to loosen, and by how much). Only
the **engine seam** has shipped so far (I0); the elastic logic is in `todo.md` (I1 onward).
Shared plumbing it builds on (the pragma gate, provenance, the reporting relation) is in
`foundations/done.md`.

## Engine seam: infeasible

The seam is wired; the diagnosis logic is not. On an infeasible solve under `auto`,
`PhysicalDecide::Finalize`'s `DiagnosisTerminal::INFEASIBLE` arm calls `DiagnoseInfeasible`
(`decide_diagnostic_engines.cpp`), mirroring the unbounded arm: a valid diagnosis is
stashed (`StashDecideDiagnostic`) and surfaced (`ThrowDecideDiagnosisReady`); otherwise
control falls through to the static error (`ThrowDecideSolveError`). Today the engine is an
**empty pipe** — it returns `valid=false` — so **behavior is unchanged**: an infeasible
DECIDE still throws the static *"DECIDE optimization is infeasible: the SUCH THAT
constraints cannot all be satisfied at once…"* (`ilp_solver.cpp`). Under `off` the router
returns `UNDIAGNOSED` and the arm is never reached. A residual `INF_OR_UNBD` (empty ray) is
normalized to `INFEASIBLE` in the arm before the message is built.

**Engine boundary (mirrors `DiagnoseUnbounded`).** `InfeasibleDiagnosisInput`
(`decide_diagnostic_engines.hpp`) is the unbounded input with one structural swap: it
carries the built **`SolverModel`** (the elastic transform will reshape its rows) in place
of a solved ray, alongside the `VarIndexer`, per-variable labels / is-aux flags,
`DecideDiagParams`, and an **injected solve callback**
`std::function<SolverResult(const SolverModel &)>`. The callback lets the engine run the
elastic re-solve without depending on the operator or the solver facade — `Finalize` binds
it to `SolvePreparedModel` on the primary solve's backend (`SelectSolverBackend` is
deterministic, so the re-solve uses the same backend). This keeps the engine
solver-agnostic and unit-testable, the same boundary as the unbounded engine's
`get_candidates` injection. The engine will build its own `ClauseRowIndex` from the model
(`BuildClauseRowIndex` is pure), so that index is not carried in the input.

**Model retention.** `SolveModel` builds the `SolverModel` as a local and discards it —
only the `SolverResult` returns — and `SolverModel::Build` *moves* the global constraints
out of the `SolverInput`, so the base model is gone by the time the arm runs and cannot be
faithfully rebuilt from the gutted input. To give the engine a model to transform,
`SolveModel` takes an optional out-param **`SolverModel *retained_model`**
(`ilp_solver.hpp`): `Finalize` passes it only when diagnosis is armed, and the built model
is moved into it after the solve completes (the post-solve disambiguation / ray helpers
stay file-private inside `ilp_solver.cpp`, so all solve orchestration keeps its single
home). The retained model is freed when `Finalize` returns.

**Label dedup.** Per-decide-variable labels + is-aux flags (for column provenance) are now
built by a shared `build_var_labels` lambda in `Finalize`, used by both the UNBOUNDED and
INFEASIBLE arms (previously inline in the unbounded arm only).

## Elastic engine: stage-1 core (simple shapes) + elastic-infeasible signal

I1 fills the seam: on an infeasible solve under `auto`, `DiagnoseInfeasible` builds and
solves a **second** optimization — the *elastic program* — whose optimum is the
least-change fix. Each relaxable user constraint gets a non-negative slack that lets its
RHS stretch; minimizing the total loosening, the positive-slack support names the
constraints to edit and the slack values are the amounts.

```
stage 1:   min  Σ wᵢ sᵢ              (uniform wᵢ = 1)
           s.t. Aᵢ x ≤ bᵢ + sᵢ ,  sᵢ ≥ 0    (relaxable rows + relaxable single-instance bounds)
                structural / mechanism rows rigid
           →  support {i : s*ᵢ > 0} names the edits; s*ᵢ is the amount
```

**Operator / engine split.** The engine is solver-agnostic and free of DuckDB
`Expression` types, so the work splits across the I0 boundary:

- **Operator (`physical_decide.cpp`).** Knows the `Expression`s and the sink state, so it
  handles **absorbed bounds (decision 1a)**. A user `x <= 10` / `x BETWEEN a AND b` is
  pre-absorbed into the column-bound arrays (never a row), so it carries no provenance and
  the row-based engine can't see it. `TraverseBoundsConstraints` now also records each
  absorbed user bound as a `UserBoundSpec {decide_var_idx, sense, k}` (COMPARISON → one
  spec; **BETWEEN → two**, the previously-untracked side now captured). In the
  `DiagnosisTerminal::INFEASIBLE` arm, for each spec on a **single-instance** variable the
  operator appends a `USER_PARAMETER` row (`coeff 1·x sense k`) to the retained model and
  **relaxes the rigid column bound** for that direction (to ±1e30) so the bound is enforced
  only by the loosenable row. Default non-negativity (`lower = 0`) and BOOLEAN `0/1` bounds
  stay rigid (never recorded / skipped). A bound on a **multi-instance** variable fans into
  one shared knob across N rows — the shared-slack mechanism of I2 — so it is *not*
  re-emitted; instead `has_unhandled_user_bounds` is set on the engine input.
- **Engine (`decide_diagnostic_engines.cpp`, `DiagnoseInfeasible`).** Pure model math.
  Copies the model, rebuilds the objective as `min Σ sᵢ` (zeroes the user objective, drops
  the quadratic objective, `maximize=false`), and adds a slack to every relaxable **linear**
  row (`IsRelaxableForElastic` ⇒ `USER_PARAMETER`). Quadratic rows and
  `STRUCTURAL`/`USER_MECHANISM` rows stay rigid — quadratic-RHS slack is I2; ABS pin rows
  are already `USER_PARAMETER` and are picked up automatically while their envelopes
  (`STRUCTURAL`) stay rigid. **Slack direction (decision 3):** `≤` → `−s` (`Ax − s ≤ b`),
  `≥` → `+s` (`Ax + s ≥ b`), `=` → **two** non-negative slacks `−s⁺ + s⁻` (stays linear,
  uniform `sᵢ ≥ 0`). **Slack type (decision 4):** REAL, `≥ 0`, uncapped, even for an
  integer RHS. It re-solves through the injected `solve_model` callback (same backend as the
  primary solve).

**Reading the result.** On `OPTIMAL`, every slack above `DIAGNOSTIC_RAY_EPSILON` is an
edit; the amount is the slack value (`=` reports the net `s⁺ − s⁻`). Each clause is labelled
by reconstructing its algebra from the **original** row's coefficients over user-facing
column names (`BuildColumnProvenance`) — `x <= 5` → suggestion `x <= 10` — so no source
expression text needs threading. `BuildInfeasibleDiagnostic` emits, per edit, EAV rows
`subject_kind='clause'`, `subject=<clause as written>`, `attribute='suggested_change' |
'amount'`, plus an actionable one-line summary ("the constraints cannot all be satisfied at
once. Loosen `x <= 5` to `x <= 10`.").

**Elastic-infeasible signal.** If the elastic program is *itself* infeasible, the conflict
reaches rigid rows → `BuildElasticInfeasibleDiagnostic` renders a distinct outcome
(`subject_kind='model'`, `attribute='elastic_infeasible'`, summary: loosening your SUCH THAT
limits cannot fix it). This is claimed **only** when every user constraint was actually made
relaxable: if the operator punted a multi-instance bound (`has_unhandled_user_bounds`), the
engine returns an invalid diagnosis and falls through to the static error rather than
wrongly declaring the query unfixable. Likewise, when there are no relaxable rows at all,
the engine returns invalid (static error).

**Scope (deferred to later phases).** Shared-slack / multi-row shapes — MIN/MAX blocks, PER
groups, AVG, strict `<>`, quadratic-RHS, and multi-instance bounds — are **I2**. The
achievable-objective re-solve is **I3**. The L0/removal dial is **I4**. Full
`decide_diagnostics()` rendering (runnable rewritten query, PER-per-group) is **I5**; I1
ships the minimal edit list.

## Tests

`test/common/test_decidb_diagnostic_engines.cpp` — SECTIONs drive `DiagnoseInfeasible`
against the bundled HiGHS backend on one-variable models: a relaxable cap conflicting with
a rigid floor reports the unique minimal loosening (`x <= 5` → `x <= 10`, amount 5); an
equality row loosens via its two-sided slack (`x == 5` → `x == 8`); a rigid-only conflict
with a non-helping relaxable row renders the elastic-infeasible row; the
`has_unhandled_user_bounds` flag suppresses that claim (invalid → static error); and an
all-rigid model returns invalid. End-to-end differential coverage (both backends, vs
`oracle_solver`, including absorbed-bound and BETWEEN cases) is in
`test/decide/tests/test_query_diagnostics_relation.py`.
