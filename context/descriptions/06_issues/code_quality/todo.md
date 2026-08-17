# Code Quality Issues — Open

Code-quality issues (duplication, dead code, fragile patterns, unclear naming, missing test coverage) spotted opportunistically while working on other tasks. Not bugs — nothing here produces wrong results today; these are things that make the code harder to change safely. Actual bugs go to `../bugs/todo.md`.

Each entry: short title, location (`file:line`), what's wrong, why it matters, and when/during which task it was discovered.

Resolved entries are removed; if the fix taught a generalizable lesson, record it in `.claude/lessons.md`.

---


## A BOOL decision's `[0, 1]` domain never reaches `SolverInput`, so every Big-M over one falls back to `1e6`

**Location**: `src/optimizer/decide/decide_optimizer.cpp:2203` initialises `absorbed_upper_bounds` to `1e30` for every variable; only a *user-written* bound narrows it. The BOOLEAN `1.0` ceiling is applied much later, at `src/decidb/utility/ilp_model_builder.cpp:199-207`, and merged at `:221`. `SolverInput::upper_bounds` is copied from the former (`physical_decide.cpp:2625`), so it never carries the ceiling.

Every Big-M derivation reads `input.upper_bounds` through `DecideRowTermRange` (`ilp_linearization.cpp:19`), which treats `ub >= 1e20` as unbounded and makes the caller fall back to `DECIDE_BIGM_FALLBACK`. A `DECIDE x(BOOL)` with no written upper bound therefore looks unbounded to every one of them.

Measured 2026-08-17 on two spellings of the *same* feasible set, four rows each:

| Query | Big-M emitted |
|---|---|
| `DECIDE x(BOOL) ... SUM(x) <> 2` | `1000000` |
| `DECIDE x(INT) ... x <= 1 AND SUM(x) <> 2` | `7` |

**Why it matters**, in two ways:

- **Numerics.** A `1e6` coefficient beside `1.0`s widens the matrix coefficient range by six orders of magnitude, which is bad for the simplex and can weaken presolve on unrelated rows. It does *not* weaken the `<>` relaxation specifically — the convex hull of `{0,1,3,4}` is `[0,4]` whether M is 7 or 1e6, so no encoding can exclude the hole — but ABS and hard MIN/MAX share `DecideRowTermRange` and their relaxations are not hull-limited in the same way, so they may lose real strength.
- **Missed range collapses.** The `<>` collapse reads `rigid_upper_bounds`, which inherits the same gap, so the upper side of a BOOL's intrinsic box is invisible. `SUM(x) <> 9` over four BOOL decisions is unreachable and should be dropped outright; it emits the two-row disjunction instead (verified 2026-08-17). Only the `ALWAYS_TRUE` and `LOWER_ONLY` verdicts are affected — the common `<> 0` case rests on the lower bound and still collapses.

**Fix direction**: write the type's intrinsic domain into `absorbed_upper_bounds` when it is initialised, rather than defaulting every variable to `1e30` and repairing BOOLEAN at model-build time. The ceiling is a property of the declared type, known at stage 05, and it is genuinely rigid — `PhysicalDecide::Finalize` already resets BOOLEAN columns only within `[0,1]` during diagnosis (`physical_decide.cpp:4476-4484`), so nothing downstream opens it. Check the merge at `ilp_model_builder.cpp:221` stays correct once the value arrives pre-narrowed, and check no `>= 1e20` unboundedness test elsewhere was relying on the old default.

**Verification**: `baseline.dump` will change (Big-M constants shrink), so `baseline.dump.results` must be byte-identical before recapture — the models move but no optimum may.

**Discovered**: 2026-08-17, while explaining why the `<>` disjunction's Big-M was `1e6` in the range-collapse work. Unrelated to that change: the M derivation was untouched by it.

---

## Hard-direction MIN/MAX has only a one-hot Big-M encoding, whose relaxation weakens with row count

**Location**: `src/execution/operator/decide/physical_decide.cpp:5394-5432` (flat hard MIN/MAX objective); the same encoding appears for composed terms via `EmitComposedHardMinMaxIndicators` and for nested-PER inner/outer levels.

The hard direction (`MAXIMIZE MAX`, `MINIMIZE MIN`) emits the textbook one-hot Big-M encoding: one indicator binary per active row, a linking row `z <= v_r + M*y_r` per row, and `SUM(y) >= n-1` so exactly one row binds.

The Big-M constant is not the weakness — `compute_big_m()` (line 4925) returns `global_max - global_min`, and since `z`'s own bound *is* the global extreme, a per-row constant would not be tighter. The **encoding** is the weakness: setting every `y_r = (n-1)/n` is LP-feasible and slackens the bound on `z` by `M*(n-1)/n`, which tends to `M` as `n` grows. The root relaxation therefore gets weaker the larger the instance, and branch-and-bound must close a gap that widens with row count.

**Why it matters**: this makes Q9 the least scalable query in the benchmark suite — measured 2026-07-26 at 5K 1.7s, 7.5K 5.3s, 15K 29.8s, 30K >60s, against a near-linear curve for every other MILP in the set. That ranking is an artifact of the single formulation we implement, not evidence that hard-MAX is intrinsically harder than the rest. It also caps `Q9_ROW_LIMIT` at 7.5K/15K while comparable queries run at 500K/1M.

**Fix direction**: DeciDB currently has no general-constraint, indicator-constraint, or SOS path at all — `src/decidb/` has zero hits for `genconstr`, `SOS`, or the indicator APIs, so Big-M is the only tool available anywhere in the codebase. Gurobi's `GRBaddgenconstrMax` / `GRBaddgenconstrMin` model `z = max/min(...)` natively and avoid the relaxation problem entirely. HiGHS has no equivalent, so this must be built as an **accelerator with the existing Big-M path as fallback** — the same pattern CLAUDE.md prescribes for diagnostics ("Gurobi-only APIs are accelerators, never dependencies"), not as a replacement. Introducing the general-constraint channel would also open `GRBaddgenconstrIndicator` for the `<>` disjunctions and ABS linearization, which use Big-M for the same reason.

**Discovered**: 2026-07-26, while raising benchmark scale limits and asking which limits are inherent problem complexity versus our formulation. Ruled out as *not* the cause in the sibling case: Q3's L0 Big-M looseness (`norm(adj, 0, 40)` against a tight bound of 20) measured identical to the tight and inferred variants at both 60K and 120K, so Gurobi presolve repairs a loose user-supplied constant. Big-M *tightness* is handled; Big-M *encoding* is not.

---

## A test in `test_per_objective.py` flips between passing and skipping across runs

**Location**: `test/decide/tests/test_per_objective.py:1200` and `:1286` — both
`pytest.skip(f"DecidB rejected non-convex shape: {e}")`.

Two consecutive full-suite runs of the identical command (`test/decide/run_tests.sh`,
which is all `make decide-test` does) reported different tallies: `1040 passed, 2 skipped` then `1041 passed, 1 skipped`. Zero failures both times. The only skip whose
reason was surfaced is the Gurobi-availability one in `test_quadratic_constraints.py`,
which is stable; the two call sites above are the only other skips in the suite that
depend on solver behavior rather than on the environment, so one of them is the
candidate.

**Why it matters**: a `skip` on a caught rejection cannot distinguish "the solver
declined this non-convex shape today" from "a regression made DeciDB reject a shape it
used to accept." The test reports green either way, so a real expressivity regression
in these two shapes would be invisible. It also makes the suite tally an unreliable
before/after signal, which is what every structural refactor verifies against.

**Fix direction**: decide what the test is actually pinning. If the shape is expected to
be accepted, assert that and let a rejection fail. If acceptance is genuinely
solver-dependent, gate the skip on the *solver's* capability up front (as the
Gurobi-availability skip does) rather than on catching an exception from DeciDB, so the
skip is a property of the environment and not of the run.

**Discovered**: 2026-08-12, while verifying the nested-product-in-reducer fix. Noticed
only because that fix required comparing suite tallies across runs; irrelevant to the
fix itself.

---

## Theme: work that sits in the physical layer without needing a row

Seven entries came from one audit (2026-08-15) and share a root cause; three remain below. They are listed here so the shared reasoning is not re-derived each time; each entry stands alone and can be picked up independently.

`physical_decide.cpp` is 4,865 lines against 2,218 + 1,713 for layer 05, 1,402 + 1,254 for layer 06 and 985 for layer 04. The audit sorted every operation in it by one test — *does it need a row?* Six are genuine execution and are staying: the scan and materialization, chunk rebinding, PHASE 2 coefficient evaluation, `WHEN`/`PER` group ids, PHASE 1.5 entity mappings, and readback. The rest are filed below.

**Why they ended up there.** Coefficients are expressions over user data, so they can only become numbers once the relational input has run. That put coefficient evaluation at layer 08, correctly. Everything else that touches those same expression trees then followed it down, whether or not it needed the data. The code recorded this: `ApplyScaleToExtracted` rebuilt a scaled coefficient by reusing the original node's `FunctionData` because, in its own comment, that was how it got rebuilt "without a binder here" — layer 08 reconstructing binder output because it sat downstream of the binder. The flattening entry that fixed this shipped 2026-08-15.

| Entry | Candidate destination |
|---|---|
| ~~Linear-form flattening runs at execution time, without a binder~~ — shipped 2026-08-15 | 05 |
| ~~No pass collects like terms~~ — shipped 2026-08-15 | 05 |
| Degree and linearity are analyzed twice, in two layers | 02 |
| Each linearized formulation is split between the layer that chooses it and the layer that encodes it | 06 |
| Three renderers answer one question about showing users their own expressions | shared |
| ~~Structural and value validation sit in the same guards~~ — fully shipped; strict `<`/`>` 2026-08-15, `<>` 2026-08-17 | 02, partly |

Destinations are candidates, not decisions; each entry names the questions its chunk has to answer first. The table order implies no batch order. The one dependency that spanned entries — flattening gating like-term collection — is discharged: both shipped to layer 05 on 2026-08-15 and are documented in [`../../01_pipeline/05_optimizer/done.md`](../../01_pipeline/05_optimizer/done.md) §1a. The remaining entries are independent of everything, including each other.

Bound absorption shipped to layer 5 on 2026-08-15 and is documented in [`../../01_pipeline/05_optimizer/done.md`](../../01_pipeline/05_optimizer/done.md). It had gated flattening; it no longer does.

**Verifying a chunk.** A structural refactor that changes no semantics must leave the golden dump byte-identical, so `./test/decide/golden/capture.sh` and a clean `diff` against `test/decide/golden/baseline.dump` is the primary signal, alongside `make decide-test`. A chunk that legitimately changes the model (a tightened bound, a different encoding) must show `baseline.dump.results` unchanged before the baseline is recaptured.

---

## Degree and linearity are analyzed twice, in two layers

**Location**: `src/optimizer/decide/decide_linear_form.cpp` (`IsLinearInDecideVars`, `DetectQuadraticPattern`), against the binder's own degree analysis at `src/planner/expression_binder/decide_binder.cpp:91`.

The binder computes polynomial degree to decide whether a DECIDE expression is valid at all. The flattening pass re-derives the same property to route an expression to the linear, quadratic or bilinear extraction path.

**Why it matters**: CLAUDE.md gives degree to layer 2. Two independent implementations can disagree, and when they do the failure lands at plan time in extractor vocabulary rather than at bind time as a sentence about the query. (Flattening moved to layer 05 on 2026-08-15, which relocated this duplication but did not remove it — the point of the entry.) The binder's own comment records exactly that having happened: before the `POWER` fix, `SUM(POWER(x, 2) * y)` "passes the gate and is rejected much later by physical extraction" (`decide_binder.cpp:115`). That fix closed the gap for one operator; the duplication that allowed it is still here.

**Fix direction**: compute once at layer 2 and carry the answer on the bound node so the flattening pass reads a field. Open question: how much the binder's classification would have to grow, since the flattener's version distinguishes shapes the binder currently has no reason to name (self-product versus `POWER(expr, 2)`, bilinear versus quadratic). Worth measuring that gap before committing.

**Discovered**: 2026-08-15, physical-layer audit.

---

## Each linearized formulation is split between the layer that chooses it and the layer that encodes it

**Mostly shipped 2026-08-15**, across two slices. Every emitter with no physical dependency now lives at layer 6 in `src/decidb/utility/ilp_linearization.cpp` — Big-M constants and implied bounds, hard MIN/MAX, `<>` (both the per-row expansion and the deferred aggregate one), McCormick, and ABS-under-MAXIMIZE. Documented in [`../../01_pipeline/06_model_formulation/done.md`](../../01_pipeline/06_model_formulation/done.md) §9. `physical_decide.cpp` went 7,344 → 6,359.

**What remains is one emitter, and it is not a move**: the nested-`PER` two-level formulation, plus the composed MIN/MAX row emission that shares its scaffolding.

**Location**: `src/execution/operator/decide/physical_decide.cpp`, Finalize PHASE 3, from the two-level `PER` auxiliary construction onward.

**Why it is different from the ones that shipped.** The others were pure functions of `SolverInput` wearing a physical-layer costume. This one re-scans `gstate.data` with an `ExpressionExecutor` at six sites *inside* the emitter, evaluating inner expressions that PHASE 2 never evaluated. So it is not "an emitter that ended up downstream" — it is an emitter fused with a late evaluation pass.

**Fix direction**: hoist the evaluation into PHASE 2 first, so the nested-`PER` path receives evaluated coefficients like every other construct, and only then move the emission to layer 6. Open question: whether PHASE 2 can evaluate those inner expressions without knowing the group structure the emitter derives, or whether the two are genuinely interleaved — if they are, this becomes a question about the `PER` contract rather than about layer placement. Worth answering before committing to the move.

**Verification note**: both shipped slices left `baseline.dump` byte-identical, and this one should too. If it cannot, that is evidence the evaluation and the emission are not separable, which is the finding rather than a failure.

**Discovered**: 2026-08-15, physical-layer audit.

---

## Three renderers answer one question about showing users their own expressions

**Location**: `src/execution/operator/decide/physical_decide.cpp:540` (`RenderWhenPredicate`), `:564` (`RenderDiagnosticRhsLabel`), `:1340` (`ParamsToString`), and `src/planner/operator/logical_decide.cpp:87` (`RenderDecideExpressionName`).

Layer 8 strips the binder's implicit casts before rendering a diagnosis label; layer 3 renders the bound tree raw for `EXPLAIN`. The user-visible symptom is filed as a bug — see [`../bugs/todo.md`](../bugs/todo.md), "`EXPLAIN` renders binder-inserted casts over the user's own terms" — and is not restated here.

**Why it matters as code quality**: the bug is one symptom of the duplication, not the whole of it. Any future surface that needs to echo a constraint back to the user is a fourth implementation, and they will keep diverging in exactly the ways the cast handling already has.

**Fix direction**: the obstacle is stated in the bug entry — layer 3 renders a bound tree while layer 8 rebuilds from evaluated ILP provenance, so consolidation means agreeing on one input first. That decision belongs to whoever picks up the bug; this entry exists so the consolidation is not lost once the `EXPLAIN` symptom is fixed.

**Discovered**: 2026-08-15, physical-layer audit.

---

