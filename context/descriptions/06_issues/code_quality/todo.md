# Code Quality Issues — Open

Code-quality issues (duplication, dead code, fragile patterns, unclear naming, missing test coverage) spotted opportunistically while working on other tasks. Not bugs — nothing here produces wrong results today; these are things that make the code harder to change safely. Actual bugs go to `../bugs/todo.md`.

Each entry: short title, location (`file:line`), what's wrong, why it matters, and when/during which task it was discovered.

Resolved entries are removed; if the fix taught a generalizable lesson, record it in `.claude/lessons.md`.

---


## Hard-direction MIN/MAX has only a one-hot Big-M encoding, whose relaxation weakens with row count

**Location**: `src/execution/operator/decide/physical_decide.cpp:5394-5432` (flat hard MIN/MAX objective); the same encoding appears for composed terms via `EmitComposedHardMinMaxIndicators` and for nested-PER inner/outer levels.

The hard direction (`MAXIMIZE MAX`, `MINIMIZE MIN`) emits the textbook one-hot Big-M encoding: one indicator binary per active row, a linking row `z <= v_r + M*y_r` per row, and `SUM(y) >= n-1` so exactly one row binds.

The Big-M constant is not the weakness — `compute_big_m()` (line 4925) returns `global_max - global_min`, and since `z`'s own bound *is* the global extreme, a per-row constant would not be tighter. The **encoding** is the weakness: setting every `y_r = (n-1)/n` is LP-feasible and slackens the bound on `z` by `M*(n-1)/n`, which tends to `M` as `n` grows. The root relaxation therefore gets weaker the larger the instance, and branch-and-bound must close a gap that widens with row count.

**Why it matters**: this makes Q9 the least scalable query in the benchmark suite — measured 2026-07-26 at 5K 1.7s, 7.5K 5.3s, 15K 29.8s, 30K >60s, against a near-linear curve for every other MILP in the set. That ranking is an artifact of the single formulation we implement, not evidence that hard-MAX is intrinsically harder than the rest. It also caps `Q9_ROW_LIMIT` at 7.5K/15K while comparable queries run at 500K/1M.

**Fix direction**: DeciDB currently has no general-constraint, indicator-constraint, or SOS path at all — `src/decidb/` has zero hits for `genconstr`, `SOS`, or the indicator APIs, so Big-M is the only tool available anywhere in the codebase. Gurobi's `GRBaddgenconstrMax` / `GRBaddgenconstrMin` model `z = max/min(...)` natively and avoid the relaxation problem entirely. HiGHS has no equivalent, so this must be built as an **accelerator with the existing Big-M path as fallback** — the same pattern CLAUDE.md prescribes for diagnostics ("Gurobi-only APIs are accelerators, never dependencies"), not as a replacement. Introducing the general-constraint channel would also open `GRBaddgenconstrIndicator` for the `<>` disjunctions and ABS linearization, which use Big-M for the same reason.

**Discovered**: 2026-07-26, while raising benchmark scale limits and asking which limits are inherent problem complexity versus our formulation. Ruled out as *not* the cause in the sibling case: Q3's L0 Big-M looseness (`norm(adj, 0, 40)` against a tight bound of 20) measured identical to the tight and inferred variants at both 60K and 120K, so Gurobi presolve repairs a loose user-supplied constant. Big-M *tightness* is handled; Big-M *encoding* is not.

---

## Theme: work that sits in the physical layer without needing a row

The entry below came from one audit (2026-08-15) that sorted every operation in `physical_decide.cpp` by one test — *does it need a row?* Six operations are genuine execution and are staying: the scan and materialization, chunk rebinding, PHASE 2 coefficient evaluation, `WHEN`/`PER` group ids, PHASE 1.5 entity mappings, and readback. The rest were filed; all but this one have shipped.

**Why they ended up there.** Coefficients are expressions over user data, so they can only become numbers once the relational input has run. That put coefficient evaluation at layer 08, correctly. Everything else touching those same expression trees then followed it down, whether or not it needed the data.

One entry remains: the nested-`PER` emitter, whose destination is layer 06. It names the questions it has to answer first.

**Verifying a chunk.** A structural refactor that changes no semantics must leave the golden dump byte-identical, so `./test/decide/golden/capture.sh` and a clean `diff` against `test/decide/golden/baseline.dump` is the primary signal, alongside `make decide-test`. A chunk that legitimately changes the model (a tightened bound, a different encoding) must show `baseline.dump.results` unchanged before the baseline is recaptured.

---

## The nested-`PER` emitter is fused with a late evaluation pass

Every other linearization emitter now lives at layer 6 in `src/decidb/utility/ilp_linearization.cpp`, documented in [`../../01_pipeline/06_model_formulation/done.md`](../../01_pipeline/06_model_formulation/done.md) §9. **One emitter stayed behind, and it is not a move**: the nested-`PER` two-level formulation, plus the composed MIN/MAX row emission that shares its scaffolding.

**Location**: `src/execution/operator/decide/physical_decide.cpp`, Finalize PHASE 3, from the two-level `PER` auxiliary construction onward.

**Why it is different from the ones that shipped.** The others were pure functions of `SolverInput` wearing a physical-layer costume. This one re-scans `gstate.data` with an `ExpressionExecutor` at six sites *inside* the emitter, evaluating inner expressions that PHASE 2 never evaluated. So it is not "an emitter that ended up downstream" — it is an emitter fused with a late evaluation pass.

**Fix direction**: hoist the evaluation into PHASE 2 first, so the nested-`PER` path receives evaluated coefficients like every other construct, and only then move the emission to layer 6. Open question: whether PHASE 2 can evaluate those inner expressions without knowing the group structure the emitter derives, or whether the two are genuinely interleaved — if they are, this becomes a question about the `PER` contract rather than about layer placement. Worth answering before committing to the move.

**Verification note**: the emitters that already moved left `baseline.dump` byte-identical, and this one should too. If it cannot, that is evidence the evaluation and the emission are not separable, which is the finding rather than a failure.

**Discovered**: 2026-08-15, physical-layer audit.

---
