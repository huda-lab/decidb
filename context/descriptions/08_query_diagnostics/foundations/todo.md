# Query Diagnostics — Foundations (planned)

Shared infrastructure consumed by all diagnosis states. **Build first** — every
state engine depends on these. Checklist with dependencies; detail follows.

## Checklist

- [ ] **F1 · Structured solver result** (v1.3) — deps: none
- [ ] **F2 · Constraint provenance** (v1.2-A) — deps: none
- [ ] **F3 · Relaxability tagging** (v1.2-B) — deps: F2
- [ ] **F4 · Invocation pragma `PRAGMA diagnose_decide`** — deps: F1
- [ ] **F5 · Diagnostic reporting relation** — deps: F2
- [ ] **F6 · Variable provenance** (column-side; index→name + aux→expression) — deps: none; used by U3

External dependency (tracked in `03_expressivity/sql_functions/todo.md`):
**decision-variable norms (v1.1)** — abs-aux / count-binary+Big-M / max-aux
linearizations reused by the elastic engine (`infeasible/` I3).

---

## F1 · Structured solver result

**Goal.** Replace the bare `vector<double>` return with a result struct carrying
status + diagnostics, so callers branch on outcome instead of catching an
exception.

Today `SolveModel` returns `vector<double>` (`src/include/duckdb/decidb/ilp_solver.hpp:29`)
and both backends *throw* on any non-optimal status. **Confirmed (P1): the status
is always in hand before the throw — F1 is a pure refactor, no new solver calls.**
Verified throw sites: Gurobi `gurobi_solver.cpp:229/238/248/255/260/264`, HiGHS
`deterministic_naive.cpp:208/217/226/231/236`.

**Build.**
- Result struct: `status` enum {optimal, infeasible, unbounded, inf_or_unbd,
  time_limit}, `solution` (when present), `incumbent`, `best_bound`, `gap`, and a
  ray/diagnosis slot. **Must model HiGHS `kUnboundedOrInfeasible` (status 9) →
  `inf_or_unbd` (P2/P3): HiGHS returns it on MILP-unbounded and the naive backend
  currently drops it into a generic catch-all.**
- Stop throwing on non-optimal; return the status. Default user-facing behavior
  still errors (manual-first) — this only makes the info *available*.
- Keep Gurobi's incumbent at TIME_LIMIT (currently discarded — it throws without
  reading `SolCount`, `gurobi_solver.cpp:254`).
- **Separate infeasibility source (P3):** single-variable contradictions are
  caught by a pre-solve bound check in the model builder and never reach the
  solver status switch — relevant to the infeasible engine; F1's status model
  isn't the only failure surface.
- 🔬 **Probe (slow-case, still open):** confirm HiGHS honors `time_limit` (none is
  set in `deterministic_naive.cpp` today) and returns a usable status on timeout.
  (The infeasible / unbounded status questions are now answered — P3.)

**Test.** Status parity across backends on constructed optimal / infeasible /
unbounded / timeout inputs; incumbent retained at timeout; HiGHS honors
`time_limit`.

**Gates:** everything.

---

## F2 · Constraint provenance (Pillar A)

**Goal.** Map every emitted matrix row back to the user clause that produced it —
diagnosis reports at the clause level, so nothing works without this.

**Scope (P7).** F2 is *constraint/row* provenance (which clause a row came from).
It does **not** provide variable names — naming an escaping *variable* (U3) needs
*column-side* provenance, the separate **F6**. F2 = rows → clauses; F6 = columns →
user variables/expressions.

Today `ModelConstraint` carries only indices / coefficients / sense / rhs
(`src/include/duckdb/decidb/ilp_model.hpp:78-83`) — no provenance.

**Build.**
- Add `{clause_id, group_key, kind}` to `ModelConstraint`. `clause_id` = index
  into `input.constraints`; `group_key` = the PER group / row at emission;
  `kind` = enum (F3).
- Populate at the builder fan-out sites in
  `src/decidb/utility/ilp_model_builder.cpp`: the normalized-constraint push, the
  aggregate path, the PER group-loop, the per-row path, the global push. (Verify
  exact lines at build time — research notes cite ~:421 / :552 / :593 / :694 /
  :902 but line numbers drift.)
- Reverse index (clause → its rows): one forward pass grouped by `clause_id`.
- Thread `group_key` (integer) → human-readable PER value label (integer keys are
  fine for a first cut).

**Test.** Provenance correct across aggregate / PER / per-row shapes; reverse
index round-trips. Bonus: improves EXPLAIN output and error messages generally.

**Gates:** F3, F5, the elastic engine, infeasibility reporting. (Variable naming
for ray→SQL mapping is F6, not F2.)

---

## F3 · Relaxability tagging (Pillar B)

**Goal.** Distinguish *user* rows (choices — relaxable) from *structural* rows
(definitions — rigid). Slackening a McCormick / Big-M row redefines the math and
solves a different problem, so the elastic engine must slacken only user rows.

**Build.**
- Stamp `kind`: `USER` on the plain emission path, `STRUCTURAL` in each
  linearization expansion path. Stamped, not inferred — structural rows already
  emit in separate builder paths keyed on explicit tags.
- **Row-role** the elastic engine needs (research note 8): within a clause each
  row is PARAMETER (carries the user's editable `K`; slack lands here) or
  MECHANISM (linking `Σy≥1`, McCormick/ABS definitions; rigid). Open design call:
  second field, or a refinement of `kind`?
- Variable bounds: the only structural case is the McCormick-required finite UB —
  **widenable but not removable** (widening keeps it finite/safe; removal breaks
  the envelope). Gate the removal dial, not the widen dial.
- **Enumerate every structural kind** and confirm each maps to a distinct
  emission path: McCormick, Big-M MIN/MAX, `<>`, ABS, AVG scaling, entity-scoping
  links, composed-MIN/MAX pins. Structural rewrites live in
  `src/optimizer/decide/decide_optimizer.cpp`.

**Test.** Every structural rewrite stamps STRUCTURAL; the elastic program never
slackens a structural row; an all-structural conflict makes the elastic program
itself infeasible (the scope diagnostic, not a fake fix).

**Deps:** F2.

---

## F4 · Invocation pragma `PRAGMA diagnose_decide`

**Goal.** The manual-first consent gate. A sticky DuckDB session pragma — no
grammar change, reversible.

**Build.**
- Modes: `none` (default; fail fast as today), `infeasible` / `unbounded` /
  `slow` (scoped to that state), `auto` (whichever of the three the solve lands
  in).
- **Filter semantics, not force:** a mode acts only when the solve *actually*
  lands in that state; otherwise the query behaves normally. A left-on pragma is
  harmless and `auto` doesn't violate manual-first (setting the pragma *is* the
  opt-in). Force semantics would be redundant (status is free), unsafe (no ray
  when not unbounded), or incoherent (slow isn't assertable).
- 🔬 **Open / probe:** exact pragma registration, and how the diagnostic relation
  surfaces when the result schema switches with the runtime outcome (fine at a
  REPL, fragile for embedding — a table-function / EXPLAIN surface is the path if
  programmatic embedding becomes a goal).

**Test.** `none` reproduces today's behavior exactly; a scoped mode fires only on
its state; `auto` routes by actual status.

**Deps:** F1 (needs status to filter on).

---

## F5 · Diagnostic reporting relation

**Goal.** A shared, structured output the state engines populate, rendered at the
user-clause level.

**Build.**
- The relation `(clause, group_key, edit_kind, suggested_change)` with
  `edit_kind ∈ {loosen RHS, widen bound, remove}`; PER clauses reported **per
  group**.
- Clause-level rendering: convert raw row-unit slack `s*` → reported `Δ` via the
  per-clause `(scale λ, shift δ)` (research note 8) — e.g. AVG reports `s*/N_g`,
  strict `</>` re-quotes against the typed `K`.
- `clause` / `group_key` labels come from F2 provenance.

**Test.** Renders aggregate / PER / per-row correctly; AVG and strict-inequality
unit conversions match the typed user value; per-group rows don't collapse to a
single clause when groups diverge.

**Deps:** F2. **Used by:** infeasibility reporting (I4).

---

## F6 · Variable provenance (column-side)

**Goal.** Map every solver *column* back to the user-facing thing it represents,
so the unbounded diagnosis can name the escaping variable (U3). The column-side
complement of F2 (which does rows/clauses).

Today names die at the solver boundary (P7): `VarIndexer` (`ilp_model.hpp:23-75`)
maps `(decide_var_idx, row)` → flat column index, but no names reach `SolverInput`
/ `SolverModel`. User names live only in `LogicalDecide.decide_variables[*].alias`
(`logical_decide.hpp:51`). Auxiliary columns from linearization (`__abs_aux_N__`,
`__bilinear_aux_N__`, `__minmax_ind_N__`, `__ne_ind_N__`) carry generated names +
partial link metadata, but **no link to the source expression**.

**Build (full — per the U3 "full output" decision).**
- **User variables:** thread the `.alias` from `LogicalDecide` through to the
  solver result so a column index resolves to the user's variable name. Modest.
- **Auxiliary variables:** capture an aux→source-expression link at optimizer
  time (where the expressions still exist — the ABS / MIN-MAX / McCormick / `<>`
  passes in `src/optimizer/decide/decide_optimizer.cpp`), so an escaping aux
  resolves to the user's original `ABS(...)` / `MAX(...)` / product expression,
  not the internal name. ~300–400 lines across LogicalDecide + execution layer
  (P7). **This is the bulk of the "full output" cost.**
- Reverse map: flat column index → `(decide var | aux)` → name / expression.

**Test.** A column index resolves to the correct user variable name; an auxiliary
column resolves to its originating user expression across ABS / MIN-MAX /
McCormick / `<>`.

**Deps:** none. **Used by:** U3 (ray→SQL naming). Column-side complement of F2.
