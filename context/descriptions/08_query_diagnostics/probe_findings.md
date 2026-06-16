# Query Diagnostics — Probe Findings (Phase 0)

Empirical groundwork for the **unbounded** plan (and the F1 foundation it sits
on). This is *evidence*, not implemented status: nothing here has landed in code.
The chain is **findings (this doc) → `todo.md` (plan adjusted by evidence) →
`done.md` (when implemented)**. Solver-behavior facts recorded here are durable
cross-state reference — infeasible and slow reuse the status/ray/version facts.

Each probe carries **Question / Method / Finding / Implication**. The Implication
line is the payload: what it changes in the relevant `todo.md`. Findings are
authored here from the dispatched agents' returned reports (agents report back;
they do not write this file).

**Status legend:** ⏳ pending · 🟡 partial · ✅ resolved

Scope note — deliberately excluded: HiGHS time-limit / incumbent-at-timeout /
interrupt probes. Those are F1's slow-case 🔬 items, not needed to complete
unbounded; they belong to the slow branch.

---

## P1 · Current solve-result call path & throw sites — ✅

Feeds: **F1** (`foundations/todo.md`).

- **Question.** Where does each backend signal a non-optimal status today, and
  what info is in hand at that point before it's discarded? Where must F1
  intercept to return a status instead of throwing?
- **Method.** Trace `SolveModel` → status handling → throw across
  `ilp_solver.cpp/.hpp`, `gurobi_solver.cpp`, `deterministic_naive.cpp`, and the
  caller in `physical_decide.cpp`.
- **Finding.**
  - **Call path.** `PhysicalDecide::Finalize()` (`physical_decide.cpp:5027`) →
    `SolveModel` facade (`ilp_solver.cpp:12-37`; builds `SolverModel`, picks
    Gurobi vs HiGHS by license, returns `vector<double>`) → backend `Solve()`.
    Both backends read the solver status, then **throw a static message on any
    non-optimal status**.
  - **Verified throw sites.** Gurobi (`gurobi_solver.cpp`): INF_OR_UNBD
    disambiguation `207-219`, infeasible `229-236`, unbounded `238-245`, residual
    INF_OR_UNBD `248-253`, TIME_LIMIT `255-258`, ITERATION_LIMIT `260-262`,
    catch-all `264-268`. HiGHS (`deterministic_naive.cpp`): status read at
    `205-206`, infeasible `208-216`, unbounded `218-225`, TIME_LIMIT `227-230`,
    ITERATION_LIMIT `232-234`, catch-all `236-240`.
  - **What's discarded.** At every non-optimal site the raw status is in hand and
    simply thrown away. At TIME_LIMIT both discard a usable incumbent — Gurobi
    never queries `SolCount`/`ObjVal`/`ObjBound`/`MIPGap` (all readable through
    the already-loaded API), HiGHS already holds `solution.col_value` and could
    call `getInfo()` but extracts neither. For infeasible/unbounded there is no
    solution to keep — only the status itself is lost.
  - **Interception point.** Per-backend, right after the status read (Gurobi
    ~`206`, before the `if (status != GRB_OPTIMAL)` block; HiGHS ~`205-206`, after
    `getModelStatus()`): build a result struct instead of throwing. Cleanest
    overall — change both `Solve()` signatures to return the struct and unpack in
    the facade.
- **Implication.**
  - F1 is a **clean refactor, not new solver calls** — the status is already in
    hand before the throw. Make both backends return a `SolverResult { status,
    solution, objective_value, best_bound, solution_count, mip_gap, backend,
    error_message }`; the facade branches on `status` and **keeps today's
    throw-on-infeasible/unbounded as the default** (manual-first).
  - For the unbounded path the **minimal F1 slice is just `status` + a ray slot**
    (ray filled by U2/P5). `incumbent`/`best_bound`/`gap`/`solcount` are slow-case
    fields — deferrable. So F1-for-unbounded is small.

---

## P2 · Linked backend versions & ray/status API surface — ✅

Feeds: **U2** (ray APIs), **U1** (HiGHS status enum).

- **Question.** Which Gurobi and HiGHS versions are linked? Does the linked
  Gurobi expose `UnbdRay` + `InfUnbdInfo` (exact signature)? Does bundled HiGHS
  expose `getPrimalRay` and an ambiguous-status enum
  (`kUnboundedOrInfeasible`-style)? Exact signatures + which header.
- **Method.** Inspect in-tree headers/libs and version macros; grep the vendored
  HiGHS status enum.
- **Finding.**
  - **Versions.** Gurobi is **runtime-loaded** (dlopen), supporting **9.5–13.0**;
    the version is parsed from the lib filename and stored in
    `api.version_{major,minor,tech}` (`gurobi_loader.cpp`, `gurobi_loader.hpp:91-93`)
    — so there is no fixed version, it depends what's installed. HiGHS is
    **vendored 1.11.0** (`third_party/highs/HConfig.h:21-23`).
  - **Gurobi ray.** `UnbdRay` attribute, requires `InfUnbdInfo=1` set first;
    readable through the **existing** `getdblattrarray` fn-ptr
    (`gurobi_loader.hpp:85`), but the `"UnbdRay"` constant isn't loaded yet (must
    add it to the loader). Available **Gurobi 11.0+** → caveat: absent on 9.5/10.0.
    `DualReductions` disambiguation already wired (`gurobi_solver.cpp:202-219`).
  - **HiGHS ray.** `getPrimalRay(bool& has_primal_ray, double* = nullptr)`
    (`Highs.h:539-540`, returns `HighsStatus`); also `getDualRay`,
    `getDualRaySparse`, `getDualUnboundednessDirection`. **No param needed**, but
    the header notes it may cost an extra LP solve. Currently unused.
  - **HiGHS status enum** (`HConst.h:188-216`, 19 values) **includes
    `kUnboundedOrInfeasible`** — the direct equivalent of Gurobi's
    `GRB_INF_OR_UNBD`. It is **currently unhandled**: it falls through to the
    generic-error catch-all (`deterministic_naive.cpp:236-240`). HiGHS exposes
    **no** `DualReductions`-style knob to disambiguate in a re-solve.
- **Implication.**
  - **U1 is genuinely needed, not redundant.** HiGHS has the ambiguous status and
    no built-in disambiguation, so the **obj=0 probe is the path**. Two concrete
    edits: add an explicit `kUnboundedOrInfeasible` branch in
    `deterministic_naive.cpp` (today it hits the catch-all), then re-solve obj=0.
    P3+P4 will confirm HiGHS actually *emits* this status on our cases.
  - **U2 native-first is supported on both.** Gurobi via `InfUnbdInfo`+`UnbdRay`
    (add the constant to the loader; **gate on version ≥ 11**, fall back to the
    portable LP for older Gurobi); HiGHS via `getPrimalRay` (unconditional, may
    trigger an extra LP solve). The **portable fallback (P6) is the floor** for
    old Gurobi and the solver-agnostic default.

---

## P3 · Status behavior on constructed cases — ✅

Feeds: **F1**, **U1**. Fixtures reused by P4.

- **Question.** What status does each backend report on (a) a minimal unbounded
  LP, (b) a minimal infeasible LP, (c) a minimal MILP-unbounded case? Does
  Gurobi's `DualReductions=0` disambiguation (`gurobi_solver.cpp:202-219`)
  actually fire? Does HiGHS ever return an ambiguous status, or always definitive?
- **Method.** Write the three tiny DECIDE queries; run on both backends; capture
  raw status.
- **Finding.**
  - **Backend selection** (incidental): env var `DECIDB_FORCE_SOLVER=gurobi|highs`
    (`ilp_solver.cpp:19`), **not a PRAGMA**; default auto-picks Gurobi if
    available. Gurobi IS available on this host. All non-OPTIMAL terminations
    currently throw → query error.
  - **Two infeasibility paths.** A single-variable contradiction (`x>=5 AND x<=1`)
    is caught by a **pre-solve bound check** in the model builder ("contradictory
    bounds on variable 0") and **never reaches the solver**. Only an aggregate
    conflict (`x<=1 AND SUM(x)>=10 AND SUM(x)<=2`) reaches solver-level INFEASIBLE.
    Also: `MAXIMIZE`/`MINIMIZE` **without `SUCH THAT` is a parser error** — an
    unbounded test case needs a non-bounding row like `SUCH THAT x>=0`.
  - **Status table:**

    | Case | Gurobi | HiGHS |
    | --- | --- | --- |
    | UNBOUNDED (REAL/LP) | definitive "unbounded" (`gurobi_solver.cpp:237`) | definitive "unbounded" (`kUnbounded`=10, `deterministic_naive.cpp:217`) |
    | INFEASIBLE (aggregate) | "infeasible" (`GRB_INFEASIBLE`) | "infeasible" (`kInfeasible`=8) |
    | UNBOUNDED (INTEGER/MILP) | definitive "unbounded" (`GRB_UNBOUNDED`) | **ambiguous `kUnboundedOrInfeasible`=9, UNHANDLED** → generic "solver status 9" (catch-all `deterministic_naive.cpp:235-241`) |

  - Gurobi gives clean unbounded-vs-infeasible on LP **and** MILP; `DualReductions=0`
    did not need to fire on these tiny models. HiGHS is definitive only for LP; for
    **MILP-unbounded it returns the ambiguous status 9**.
- **Implication.**
  - **F1 must model HiGHS `kUnboundedOrInfeasible`(9)** — today it falls into the
    generic catch-all. Separately, the **pre-solve bound-check infeasibility is a
    distinct source** that never hits the solver status switch — relevant to the
    *infeasible* engine (single-var contradictions short-circuit before any
    elastic re-solve would see them).

---

## P4 · obj=0 disambiguation probe (U1) — ✅

Feeds: **U1** (`unbounded/todo.md`). Shares P3 fixtures.

- **Question.** Does re-solving with the objective replaced by `0` reliably
  distinguish unbounded (feasible) from infeasible (infeasible) — specifically on
  HiGHS, the portable path?
- **Method.** On P3's unbounded + infeasible cases, swap objective → 0, re-solve,
  check feasibility.
- **Finding.** Dropping the objective (pure feasibility): UNBOUNDED-REAL →
  **feasible** (both backends); MILP-UNBOUNDED-INTEGER → **feasible** (both);
  INFEASIBLE-aggregate → **infeasible** (both). The rule **"feasible ⇒ was
  unbounded; infeasible ⇒ was infeasible" holds on both backends** — including the
  MILP case where HiGHS otherwise returns the ambiguous status 9.
- **Implication.** **U1's obj=0 probe is load-bearing for the HiGHS MILP-unbounded
  case** (status 9 — exactly the case HiGHS can't disambiguate natively). For LP
  it's redundant but harmless. It's the portable analogue of Gurobi's
  `DualReductions=0`, and verified to behave identically on Gurobi too — so it
  works as a fully solver-agnostic disambiguator.

---

## P5 · Native ray extraction (U2) — ✅

Feeds: **U2** (`unbounded/todo.md`).

- **Question.** On a constructed unbounded model, do `UnbdRay` (Gurobi) and
  `getPrimalRay` (HiGHS) return a usable ray? What flag/param must be set first?
  MIP-vs-LP behavior (ray for a MIP, or only the LP relaxation)?
- **Method.** Rays aren't wired into the SQL path (it throws), so likely a small
  standalone C++ probe per library, or temporary instrumentation at the throw
  site.
- **Finding (empirical — re-ran in the main tree with temporary `InfUnbdInfo=1` +
  `UnbdRay` / `getPrimalRay` instrumentation; build OK, instrumentation reverted).**

  | Backend | Case | Native ray result |
  | --- | --- | --- |
  | Gurobi | LP-unbounded | `UnbdRay` **rc=0, ray=[1,1]** ✅ |
  | Gurobi | MILP-unbounded | status=UNBOUNDED but `UnbdRay` **rc=10005 "Unable to retrieve attribute 'UnbdRay'"** ❌ (LP-only, confirmed) |
  | HiGHS | LP-unbounded (status 10) | `getPrimalRay` **rc=0, has_ray=1, ray=[1,0]** ✅ |
  | HiGHS | MILP-unbounded (status 9) | `getPrimalRay` **rc=0, has_ray=1, ray=[1,0]** ✅ — returns a ray **even on the ambiguous `kUnboundedOrInfeasible(9)`** (clears integrality → LP-relaxation ray) |

  `InfUnbdInfo=1` on the env makes Gurobi `UnbdRay` available — but **only for
  continuous models**; on a MIP Gurobi refuses the attribute (error 10005,
  data-not-available). HiGHS `getPrimalRay` returns a usable ray on LP **and**
  MILP-unbounded, including under the ambiguous status 9.
- **Implication.**
  - **Asymmetry, confirmed:** Gurobi native ray = continuous/LP only (errors on
    MIP → must drop integrality or use the fallback); HiGHS native ray works for
    both LP and MIP. Neither is uniformly better — both are accelerators.
  - A ray returned on HiGHS status 9 is corroborating evidence of unboundedness but
    does **not** replace U1's obj=0 confirmation (the status-9 contract is murky).
  - U2 plan holds: native rays as accelerators; **gate Gurobi `UnbdRay` to
    continuous models**, fall back otherwise.

---

## P6 · Portable fallback ray LP (U2) — ✅

Feeds: **U2** (`unbounded/todo.md`). Compares against P5.

- **Question.** Does the fallback we own — `max cᵀd s.t. Ad ≤ 0, 0 ≤ d ≤ 1`,
  optimum > 0 ⇒ `d` is a ray — produce a ray, and does it agree with P5's native
  ray on the same model?
- **Method.** Prototype the auxiliary LP (via our model builder if feasible, else
  standalone); compare `d` to P5's ray.
- **Finding (empirical).** The fallback bounded LP — expressed as a plain DECIDE
  query `… SUCH THAT x <= 1 AND SUM(x) >= 0 MAXIMIZE …` — solved on **both**
  backends (rc=0), returning **d=[1,1]** (optimum 2 > 0). No instrumentation
  needed; the solution *is* `d`.
  - **Support / agreement.** The test model is symmetric — both vars have a
    positive objective coeff and no upper cap, so the true escaping set is
    {x₁, x₂}. The fallback's `d=[1,1]` names **both**. Gurobi native `[1,1]` also
    names both; HiGHS native `[1,0]` is a valid extreme ray but names only x₁
    (under-reports). So the fallback support ⊇ the native supports, and it hit the
    *complete* escaping set.
- **Implication.** Fallback is viable and solver-agnostic as predicted — **and it
  yields a fuller-support ray** (every objective-improving unbounded direction
  pushed to its box max), which is exactly what "name all escaping variables"
  wants, vs. a native single extreme ray that can be sparse. Confirms **U2 default
  = portable fallback; native rays = accelerators.** Nuance: extreme-ray degeneracy
  means support differs by method; the fallback's box-max behavior is the more
  complete (and more useful) choice for diagnosis.

---

## P7 · Variable structure, bounds & provenance — ✅

Feeds: **U3** (ray→SQL mapping), **F2** scoping.

- **Question.** What does `VarIndexer` expose for index → user name? How do
  auxiliary vars (`MAX` z, ABS aux, McCormick factors) appear — any metadata to
  trace them to the user's expression? Do U3's "narrows suspects for free"
  assumptions hold: BOOLEAN locked to [0,1], all vars non-negative, McCormick
  factors get a finite UB, MAXIMIZE ⇒ positive obj coeff with no cap?
- **Method.** Read the builder / `VarIndexer` in `ilp_model_builder.cpp` and the
  variable-bound emission; confirm the bound claims against a built model.
- **Finding.**
  - **Index→name: not implemented.** `VarIndexer` (`ilp_model.hpp:23-75`) maps
    `(decide_var_idx, row)` → flat solver index, but **no variable names** are
    stored in `VarIndexer` / `SolverInput` (`solver_input.hpp:274-344`) /
    `SolverModel`. User names live only in `LogicalDecide.decide_variables[*].alias`
    (`logical_decide.hpp:51`) and don't survive into the solver layer — so
    reverse-mapping a flat index back to a user name is **impossible post-build**
    without new threading.
  - **Aux vars: named but not traceable to source.** ABS (`__abs_aux_N__`),
    bilinear (`__bilinear_aux_N__`), MIN/MAX (`__minmax_ind_N__`), `<>`
    (`__ne_ind_N__`) get generated names + partial link metadata in `LogicalDecide`
    (`abs_maximize_links` / `bilinear_links` / `minmax_indicator_links` /
    `ne_indicator_indices`), but the **original user expression is not stored** —
    tracing an aux back to `ABS(x+y)` / `MAX(...)` needs new provenance captured
    at optimizer time.
  - **Bound claims — all TRUE.** BOOLEAN→[0,1] (`ilp_model_builder.cpp:135-138`);
    user vars non-negative, explicit bounds only tighten (`128-149`); McCormick
    non-Boolean factor forced to a finite UB, throws if `≥1e20`
    (`physical_decide.cpp:3582-3616`); MAXIMIZE + positive coeff + no upper cap ⇒
    unbounded (`ilp_model_builder.cpp:202-226`). **Nuance:** internal *global*
    auxiliaries (McCormick products, `<>` disjunctions) can have LB `−1e30`
    (`physical_decide.cpp:4169/4311`) — pinned by constraints, not ≥0; so "+∞
    only" holds for *user* vars, not every column.
- **Implication.**
  - **U3's "narrow suspects for free" is sound** and runs on data already in the
    model: filter to `(DECIDE var, INTEGER/REAL)`, then the mechanical sign test
    (MAXIMIZE & coeff>0 & ub≥1e20, or MINIMIZE & coeff<0 & ub≥1e20); the ray then
    picks which candidates actually escaped. **No new data needed for the
    candidate filter.**
  - **Variable-side provenance is a SEPARATE slice from F2.** F2 (constraint
    provenance, `{clause_id,…}` on `ModelConstraint`) does **not** give U3 names.
    U3 reporting needs: (a) thread the user `.alias` from `LogicalDecide` → solver
    result to print a name (modest); (b) *optionally* capture aux→source-expression
    provenance at optimizer time (heavier — defer; until then label an escaping
    aux as `<unnamed aux>` and note that an escaping aux likely signals a
    model-generation bug, not a user error).
  - **Two-tier U3:** *thin* = mechanical filter on user vars + alias threading
    (works with today's data); *thick* = full aux-expression tracing (~300–400
    lines, additive, no redesign — deferrable).

---

## Net changes to todos (rollup)

Filled once probes resolve — the consolidated list of edits to make to
`foundations/todo.md` and `unbounded/todo.md` based on the evidence above.

- **F1** (P1, P3): clean refactor (status already in hand before the throw)
  returning a `SolverResult` struct; record verified throw-site lines and the
  minimal-for-unbounded slice (`status` + ray slot, defer incumbent/gap). **Add to
  the status model: HiGHS `kUnboundedOrInfeasible`(9)** (today unhandled) and note
  the **pre-solve bound-check** infeasibility as a separate source that never
  reaches the solver switch.
- **U1** (P2, P3, P4): confirmed **needed, not redundant**. HiGHS returns the
  ambiguous status 9 on MILP-unbounded and has no native disambiguation knob. Edit
  `deterministic_naive.cpp` to add an explicit `kUnboundedOrInfeasible` branch,
  then re-solve with the objective dropped (feasible ⇒ unbounded, infeasible ⇒
  infeasible). Note it's redundant-but-harmless for LP. Drop the todo's "🔬 does
  HiGHS emit an ambiguous status" question — answered yes.
- **U2** (P2, P5, P6): **portable fallback LP is the default** — confirmed to give
  a full-support ray on both backends. Native rays are *accelerators*: Gurobi
  `UnbdRay`+`InfUnbdInfo` works for **continuous only** (errors 10005 on MIP; gate
  on version ≥ 11; set `InfUnbdInfo` before the `DualReductions=0` re-solve too);
  HiGHS `getPrimalRay` works for **LP and MIP** (even status 9), at the cost of an
  extra LP solve and with QP / row-less gaps. Gate Gurobi `UnbdRay` to continuous
  models.
- **U3** (P7): "narrow suspects for free" filter is **sound on existing data**
  (type + sign + bound); naming escaping vars needs `.alias` threading from
  `LogicalDecide`; aux→expression tracing is the deferrable thick part. **F2
  (constraint provenance) ≠ variable provenance** — U3 needs its own variable-side
  slice; label untraceable aux as `<unnamed aux>` (and an escaping aux likely
  signals a model-gen bug).
- **General:** backend selection is the env var `DECIDB_FORCE_SOLVER`, not a
  PRAGMA (relevant when wording F4's separate `diagnose_decide` pragma).

## Decisions taken (2026-06-16)

- **U2 = fallback-only for v1** — build only the portable box-LP ray; native
  vendor rays deferred to a later optional accelerator.
- **U3 = full output** — name escaping user variables *and* trace auxiliary
  columns back to their source expression.
- **New foundation F6 · variable provenance** holds the naming plumbing
  (`.alias` threading + aux→expression). Column-side complement of F2
  (rows/clauses); the aux-tracing is the bulk of the "full output" cost.
