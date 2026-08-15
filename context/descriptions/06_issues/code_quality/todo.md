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

Seven entries came from one audit (2026-08-15) and share a root cause; four remain below, plus the `<>` remainder of a fifth. They are listed here so the shared reasoning is not re-derived each time; each entry stands alone and can be picked up independently.

`physical_decide.cpp` is 4,843 lines against 2,218 + 1,614 for layer 05, 1,402 + 1,012 for layer 06 and 985 for layer 04. The audit sorted every operation in it by one test — *does it need a row?* Six are genuine execution and are staying: the scan and materialization, chunk rebinding, PHASE 2 coefficient evaluation, `WHEN`/`PER` group ids, PHASE 1.5 entity mappings, and readback. The rest are filed below.

**Why they ended up there.** Coefficients are expressions over user data, so they can only become numbers once the relational input has run. That put coefficient evaluation at layer 08, correctly. Everything else that touches those same expression trees then followed it down, whether or not it needed the data. The code recorded this: `ApplyScaleToExtracted` rebuilt a scaled coefficient by reusing the original node's `FunctionData` because, in its own comment, that was how it got rebuilt "without a binder here" — layer 08 reconstructing binder output because it sat downstream of the binder. The flattening entry that fixed this shipped 2026-08-15.

| Entry | Candidate destination |
|---|---|
| ~~Linear-form flattening runs at execution time, without a binder~~ — shipped 2026-08-15 | 05 |
| No pass collects like terms | 05 |
| Degree and linearity are analyzed twice, in two layers | 02 |
| Each linearized formulation is split between the layer that chooses it and the layer that encodes it | 06 |
| Three renderers answer one question about showing users their own expressions | shared |
| ~~Structural and value validation sit in the same guards~~ — shipped 2026-08-15; only the `<>` refusal remains | 02, partly |

Destinations are candidates, not decisions; each entry names the questions its chunk has to answer first. The table order implies no batch order. The one dependency that spanned entries — flattening gating like-term collection — is discharged: flattening shipped to layer 05 on 2026-08-15 and is documented in [`../../01_pipeline/05_optimizer/done.md`](../../01_pipeline/05_optimizer/done.md) §1a. The remaining entries are independent of everything, including each other.

Bound absorption shipped to layer 5 on 2026-08-15 and is documented in [`../../01_pipeline/05_optimizer/done.md`](../../01_pipeline/05_optimizer/done.md). It had gated flattening; it no longer does.

**Verifying a chunk.** A structural refactor that changes no semantics must leave the golden dump byte-identical, so `./test/decide/golden/capture.sh` and a clean `diff` against `test/decide/golden/baseline.dump` is the primary signal, alongside `make decide-test`. A chunk that legitimately changes the model (a tightened bound, a different encoding) must show `baseline.dump.results` unchanged before the baseline is recaptured.

---

## No pass collects like terms

**Location**: nothing implements it. Layer 4 is forbidden to (`04_canonicalizer/done.md` §5), and no later layer claimed it. The terms it would group are `DecideTerm` lists produced by `BuildDecidePreparedModel` (`src/optimizer/decide/decide_linear_form.cpp`).

`2*ship + 3*ship <= 10` reaches the solver as two terms naming one column. Every downstream consumer then has to remember they are the same variable.

**Why it matters**: one consumer did not, and that was the implied-bound bug fixed 2026-08-15 — it derived a column bound from one term's coefficient instead of their sum. The model builder folds duplicate column entries when writing the matrix row, so the emitted row was always correct and no result-level test could fail; only the model dump showed it. The same trap is waiting for any future pass that iterates `variable_indices` without asking whether an index can repeat.

**Fix direction**: the natural home is beside the linear-form flattening in `src/optimizer/decide/decide_linear_form.cpp`, since collection is a grouping step over already-flat terms. Two constraints hold wherever it lands: it must emit through `AddConstraint` rather than editing a tree in place (layer 4's C2 rule), and it must not merge across reducer boundaries — `SUM(x) + SUM(x)` may merge, `SUM(x) + MIN(x)` may not. Pass ordering is an open question: ABS, IN and bilinear rewrites emit fresh terms, so collection either runs after them or runs twice. The defensive accumulate now in `DecidePropagateImpliedBounds` stays either way.

**Ordering**: unblocked. Flattening shipped to layer 05 on 2026-08-15, so the flat terms this needs to group now exist as `DecideTerm` lists on `LogicalDecide::prepared`.

**Discovered**: 2026-08-15, while fixing the repeated-coefficient bound bug.

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

## The `<>` refusal still tests structure and value in one predicate

**Location**: `NELhsIsIntegerValued` (`src/decidb/utility/ilp_linearization.cpp:439`, thrown at `:477`).

The strict-`<` / `>` version of this conjunction was split on 2026-08-15: the REAL-variable half now rejects at bind time (`ValidateDecideNoStrictComparisonOnReal`, stage 02) and the fractional-coefficient half stays in the model builder. See [`../../01_pipeline/02_binder/done.md`](../../01_pipeline/02_binder/done.md) §4 and [`../../01_pipeline/06_model_formulation/done.md`](../../01_pipeline/06_model_formulation/done.md) §5.

`<>` was deliberately left out of that chunk, so it still rejects on "a REAL variable **or** a non-integer coefficient" at model-build time — the same single predicate doing two jobs, in a different construct. A `<>` over a REAL decision is refused only after a full scan, in the vocabulary of the linearization pass rather than the user's clause.

**Fix direction**: the same split, and it should be smaller than the strict one was, because the bind-time machinery now exists. Extend `ValidateDecideNoStrictComparisonOnReal` to `COMPARE_NOTEQUAL` (or generalize its name), leave the coefficient half where it is, and make the REAL branch of `NELhsIsIntegerValued` an `InternalException` for the same reason the strict one is.

Two things to settle first, neither of which the strict chunk answered:

- `<>` has a **silent-drop** case the strict path does not: `NEIsIntegerValuedRhs` treats an integer-valued LHS with a fractional or infinite `K` as a tautology and drops the row. A bind-time refusal on the LHS must not change which queries reach that drop.
- The strict chunk chose to refuse on the **declared type** rather than on what the term becomes, so `norm(e, 0, M) < K` on a REAL decision is refused even though its L0 count is integral. Whether `<>` follows the same rule or is more permissive should be a deliberate choice, not an accident of implementation.

**Test surface**: `_NE_REAL_MSG` in `test/decide/tests/test_cons_comparison.py` (one assertion, `test_real_sum_not_equal_rejected`).

**Discovered**: 2026-08-15, physical-layer audit; scoped out of the strict-inequality chunk the same day.

---
