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

Seven entries came from one audit (2026-08-15) and share a root cause; six remain below. They are listed here so the shared reasoning is not re-derived each time; each entry stands alone and can be picked up independently.

`physical_decide.cpp` is 6,359 lines against 2,217 for layer 05, 1,402 + 1,012 for layer 06 and 985 for layer 04. The audit sorted every operation in it by one test — *does it need a row?* Six are genuine execution and are staying: the scan and materialization, chunk rebinding, PHASE 2 coefficient evaluation, `WHEN`/`PER` group ids, PHASE 1.5 entity mappings, and readback. The rest are filed below.

**Why they ended up there.** Coefficients are expressions over user data, so they can only become numbers once the relational input has run. That put coefficient evaluation at layer 08, correctly. Everything else that touches those same expression trees then followed it down, whether or not it needed the data. The code records this: `ApplyScaleToExtracted` rebuilds a scaled coefficient by reusing the original node's `FunctionData` because, in its own comment, that is how it gets rebuilt "without a binder here" — layer 08 reconstructing binder output because it sits downstream of the binder.

| Entry | Candidate destination |
|---|---|
| Linear-form flattening runs at execution time, without a binder | 05 |
| No pass collects like terms | 05 |
| Degree and linearity are analyzed twice, in two layers | 02 |
| Each linearized formulation is split between the layer that chooses it and the layer that encodes it | 06 |
| Three renderers answer one question about showing users their own expressions | shared |
| Structural and value validation sit in the same guards | 02, partly |

Destinations are candidates, not decisions; each entry names the questions its chunk has to answer first. The table order implies no batch order. One real dependency remains, recorded in an **Ordering** paragraph in each: flattening gates like-term collection. The others are independent of everything, including each other.

Bound absorption shipped to layer 5 on 2026-08-15 and is documented in [`../../01_pipeline/05_optimizer/done.md`](../../01_pipeline/05_optimizer/done.md). It had gated flattening; it no longer does.

**Verifying a chunk.** A structural refactor that changes no semantics must leave the golden dump byte-identical, so `./test/decide/golden/capture.sh` and a clean `diff` against `test/decide/golden/baseline.dump` is the primary signal, alongside `make decide-test`. A chunk that legitimately changes the model (a tightened bound, a different encoding) must show `baseline.dump.results` unchanged before the baseline is recaptured.

---

## Linear-form flattening runs at execution time, without a binder

**Location**: `src/execution/operator/decide/physical_decide.cpp` — `ExtractTerms` (`:1059`), `TryDistributeMultiplyOverAdd` (`:671`), `ExtractCoefficientWithoutVariable` (`:883`), `ApplyScaleToExtracted` / `ApplyScaleToObjective` (`:1566`, `:2195`).

Layer 8 turns a bound constraint tree into a list of `(variable, coefficient)` terms, and doing so performs real algebra: it distributes `K * (1 - pick)` into `K - K*pick`, pulls coefficients out of `*` chains, pushes a divisor into every produced coefficient with cast repair so `x / 2` does not truncate, strips casts, and folds unary minus. `ApplyScaleToExtracted` additionally pushes an outer factor into each coefficient, so `2 * SUM(x * price)` yields coefficients `2 * price`.

None of this reads a row. It needs types, not data.

**Why it matters**: the algebra is performed where no binder is in scope, so new expression nodes cannot be bound the ordinary way. `ApplyScaleToExtracted` works around this by reusing the original node's `FunctionData` — its own comment says this is how the coefficient gets rebuilt "without a binder here". A workaround of that shape is a reliable signal that the code sits downstream of where it belongs. The flattened form is also invisible to `EXPLAIN`, which still renders the unflattened tree, and every later pass that wants linear terms either re-walks the tree or trusts an invariant it cannot check.

**Fix direction**: layer 5 is the one layer permitted to do mathematics on the bound tree, and already rewrites trees for ABS, MIN/MAX and bilinear with types, scopes and casts known. Producing the linear form there would let layer 8 read a prepared list instead of deriving one. Open questions for whoever picks this up: what the prepared form is (a field on the bound node, or a side table on `LogicalDecide`); whether coefficients stay expressions until layer 8 evaluates them, which they must, since they reference data columns; and whether the scale folding moves in the same chunk as the extraction or a later one. Larger than it looks — extraction is entangled with quadratic and bilinear classification.

**Ordering**: no longer gated — bound absorption moved to layer 5 on 2026-08-15, which deleted the `absorbed_bound_exprs` pointer handshake that used to couple absorption and extraction to the same layer. Extraction now has nothing to skip beyond an alias tag it can read anywhere. Still do this before "No pass collects like terms", which needs flat terms to group and would otherwise require a second flattener that this entry then replaces.

**Discovered**: 2026-08-15, during a full audit of what the physical layer owns, prompted by the repeated-coefficient bound bug. Shares a root cause with the six entries below; see the theme heading above.

---

## No pass collects like terms

**Location**: nothing implements it. Layer 4 is forbidden to (`04_canonicalizer/done.md` §5), and no later layer claimed it.

`2*ship + 3*ship <= 10` reaches the solver as two terms naming one column. Every downstream consumer then has to remember they are the same variable.

**Why it matters**: one consumer did not, and that was the implied-bound bug fixed 2026-08-15 — it derived a column bound from one term's coefficient instead of their sum. The model builder folds duplicate column entries when writing the matrix row, so the emitted row was always correct and no result-level test could fail; only the model dump showed it. The same trap is waiting for any future pass that iterates `variable_indices` without asking whether an index can repeat.

**Fix direction**: the natural home is beside the linear-form flattening above, since collection is a grouping step over already-flat terms. Two constraints hold wherever it lands: it must emit through `AddConstraint` rather than editing a tree in place (layer 4's C2 rule), and it must not merge across reducer boundaries — `SUM(x) + SUM(x)` may merge, `SUM(x) + MIN(x)` may not. Pass ordering is an open question: ABS, IN and bilinear rewrites emit fresh terms, so collection either runs after them or runs twice. The defensive accumulate now in `DecidePropagateImpliedBounds` stays either way.

**Ordering**: take this *after* "Linear-form flattening runs at execution time", not before. It looks like the smallest entry here and is a tempting opening move, but collection needs flat terms to group; doing it first means writing a second flattener at layer 5 that the flattening entry then replaces. Nothing is waiting on this one — the bug that produced it is already fixed by the defensive accumulate — so there is no cost to taking it late.

**Discovered**: 2026-08-15, while fixing the repeated-coefficient bound bug.

---

## Degree and linearity are analyzed twice, in two layers

**Location**: `src/execution/operator/decide/physical_decide.cpp:821` (`IsLinearInDecideVars`) and `:968` (`QuadraticPattern`), against the binder's own degree analysis at `src/planner/expression_binder/decide_binder.cpp:91`.

The binder computes polynomial degree to decide whether a DECIDE expression is valid at all. Layer 8 re-derives the same property to route an expression to the linear, quadratic or bilinear extraction path.

**Why it matters**: CLAUDE.md gives degree to layer 2. Two independent implementations can disagree, and when they do the failure lands at execution time in extractor vocabulary rather than at bind time as a sentence about the query. The binder's own comment records exactly that having happened: before the `POWER` fix, `SUM(POWER(x, 2) * y)` "passes the gate and is rejected much later by physical extraction" (`decide_binder.cpp:115`). That fix closed the gap for one operator; the duplication that allowed it is still here.

**Fix direction**: compute once at layer 2 and carry the answer on the bound node so layer 8 reads a field. Open question: how much the binder's classification would have to grow, since layer 8's version distinguishes shapes the binder currently has no reason to name (self-product versus `POWER(expr, 2)`, bilinear versus quadratic). Worth measuring that gap before committing.

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

## Structural and value validation sit in the same guards

**Location**: two layers, since absorption moved. Layer 8: `RejectEmptyAggregate` (`src/execution/operator/decide/physical_decide.cpp:1190`, called at `:3325`, `:3578`, `:3595`) and the NaN/Infinity checks in `ExtractDoubleColumn` (`:174`, the test itself at `:201`). Layer 5: the deliberate decline on strict `<` / `>` over a REAL variable (`src/optimizer/decide/decide_optimizer.cpp:2133`, `:2139` — `absorbed = target.is_integer`), which defers the rejection to `ApplyComparisonSense` (`src/decidb/utility/ilp_model_builder.cpp:474`).

Two kinds of check are interleaved. Structural ones — a strict inequality on a REAL decision has no valid encoding — are knowable from types alone. Value ones — a NaN coefficient, an aggregate over zero rows — can only be caught once data is in hand.

**Why it matters**: a structural refusal reported at execution time arrives after a full scan, and is phrased in the vocabulary of whichever pass happened to catch it. The same refusal at bind time is immediate and can name the clause the user wrote.

**Fix direction**: the sorting test is "could this have been said without reading a row?" Structural checks move to layer 2; value checks stay.

By that test the three guards sort cleanly and only one is contentious:

- **`ExtractDoubleColumn` NaN/Infinity** — pure value check, needs the column. Stays at layer 8. Not part of this chunk.
- **`RejectEmptyAggregate`** — value check (how many rows survived `WHERE`/`WHEN`), needs the scan. Stays at layer 8. Not part of this chunk.
- **strict `<` / `>` on a REAL decision** — the only structural one. `x < 10.5` on a REAL has no valid encoding, and that is knowable from the declared type alone, with no data.

**The refusal is a conjunction, and that is the actual defect.** Every one of these guards rejects on "a REAL variable **or** a non-integer coefficient" — a structural condition OR'd with a value condition, tested at the only point where both are known. That is why they are interleaved: not carelessness, but a single predicate doing two jobs. Splitting it is the work:

- the **structural half** (the LHS references a REAL decision variable) is knowable at bind time and should reject there, naming the clause and the declared type;
- the **value half** (a data column produced a non-integer coefficient) genuinely needs the scan and stays at layer 8.

Four sites share this shape, so the chunk is a little larger than one guard but well bounded: `ilp_model_builder.cpp:481`, `:493` (per-row strict), `:1070`, `:1081` (aggregate strict), plus the sibling `<>` refusal in `NELhsIsIntegerValued` (now `src/decidb/utility/ilp_linearization.cpp`, moved there 2026-08-15 with the `<>` emitter), which is the same conjunction in a different construct. Take the `<>` one in the same chunk or explicitly leave it — do not split the pair silently.

**Test surface**: 8 assertions in `test/decide/tests/test_cons_comparison.py`, all routed through two module-level regexes (`_STRICT_LT_REAL_MSG`, `_STRICT_GT_REAL_MSG`) plus `_NE_REAL_MSG`, so a message change is a three-line edit there rather than eight.

Open question for whoever takes it: whether the model-builder guard survives as a backstop once layer 2 rejects the structural half early, or is narrowed to the value half only. A backstop that can no longer fire is dead code; one that can still fire on the coefficient path is not.

**Ordering**: independent. The strict-`<`-on-REAL case *is* absorption's mechanism — the value is left unabsorbed precisely so the model builder rejects it — and absorption moved to layer 5 on 2026-08-15 with that routing deliberately unchanged. The decline now happens in `DecideOptimizer::AbsorbVariableBounds`, so this entry's question is whether the *rejection* should follow it up to layer 2 and take its message along. The remaining guards in this entry are unaffected and can be left alone.

**Discovered**: 2026-08-15, physical-layer audit.

---
