# Code Quality Issues — Open

Code-quality issues (duplication, dead code, fragile patterns, unclear naming, missing test coverage) spotted opportunistically while working on other tasks. Not bugs — nothing here produces wrong results today; these are things that make the code harder to change safely. Actual bugs go to `../bugs/todo.md`.

Each entry: short title, location (`file:line`), what's wrong, why it matters, and when/during which task it was discovered.

Resolved entries are removed; if the fix taught a generalizable lesson, record it in `.claude/lessons.md`.

---

## The LHS data-term accumulator reduces by summing, whatever the term means

**Location**: `FixedLinearLhsOffset` in `src/decidb/utility/ilp_model_builder.cpp:624`
(callers at `:707` and `:819`).

Every LHS term with no decision variable is folded into the bound by adding it up over
the group's rows:

```cpp
for (each term with no decision variable)
    for (each row in group)
        offset += col.Get(row);      // it just sums
```

That is the correct reduction for a SUM and (after the AVG->SUM rewrite) for an AVG. It
is not a reduction at all for anything else — it is one hard-coded fold standing in for
the question "what does reducing this term mean?"

**Why it still matters, in narrower terms than the original entry.** The obvious
exposure is gone: data-only reducers now go RIGHT (B.4) and are evaluated per aggregate
kind by the RHS reducer evaluator, and the SUM/AVG-only hoist that used to feed this
accumulator was deleted at C.1. What remains is the case the canonicalizer *cannot*
move, because §1 of `canonicalize.md` defines that pass as never opening a term: a
constant or data subexpression inside a reducer **body**. `SUM(x + 3) <= 10` over two
rows still routes the `3` through here, emitting `rhs=4`, which is right because the
fold happens to be a sum. A body term whose correct reduction is not a sum would be
silently mishandled by the same code path.

So the accumulator's shape still constrains what may legally sit on the left, and that
constraint is invisible at the call site — it reads as a generic "fold the data terms"
helper. **The prediction that this would go dead was checked and is wrong**; it was
recorded here as a follow-up to B.4 and survived it.

**Fix direction**: make the fold explicit about which reduction it implements — either
by naming it for SUM specifically and rejecting anything else that reaches it, or by
routing LHS body terms through the same per-kind evaluator the RHS already has. The
first is a guard and closes the silent-mishandling hole; the second removes the
asymmetry but is only worth it if a non-sum body term is actually reachable.

**Discovered**: 2026-08-10, scoping canonicalization Phase B.4/B.5. Re-verified and
narrowed 2026-08-12.

---

## Hard-direction MIN/MAX has only a one-hot Big-M encoding, whose relaxation weakens with row count

**Location**: `src/execution/operator/decide/physical_decide.cpp:5394-5432` (flat hard MIN/MAX objective); the same encoding appears for composed terms via `EmitComposedHardMinMaxIndicators` and for nested-PER inner/outer levels.

The hard direction (`MAXIMIZE MAX`, `MINIMIZE MIN`) emits the textbook one-hot Big-M encoding: one indicator binary per active row, a linking row `z <= v_r + M*y_r` per row, and `SUM(y) >= n-1` so exactly one row binds.

The Big-M constant is not the weakness — `compute_big_m()` (line 4925) returns `global_max - global_min`, and since `z`'s own bound *is* the global extreme, a per-row constant would not be tighter. The **encoding** is the weakness: setting every `y_r = (n-1)/n` is LP-feasible and slackens the bound on `z` by `M*(n-1)/n`, which tends to `M` as `n` grows. The root relaxation therefore gets weaker the larger the instance, and branch-and-bound must close a gap that widens with row count.

**Why it matters**: this makes Q9 the least scalable query in the benchmark suite — measured 2026-07-26 at 5K 1.7s, 7.5K 5.3s, 15K 29.8s, 30K >60s, against a near-linear curve for every other MILP in the set. That ranking is an artifact of the single formulation we implement, not evidence that hard-MAX is intrinsically harder than the rest. It also caps `Q9_ROW_LIMIT` at 7.5K/15K while comparable queries run at 500K/1M.

**Fix direction**: DeciDB currently has no general-constraint, indicator-constraint, or SOS path at all — `src/decidb/` has zero hits for `genconstr`, `SOS`, or the indicator APIs, so Big-M is the only tool available anywhere in the codebase. Gurobi's `GRBaddgenconstrMax` / `GRBaddgenconstrMin` model `z = max/min(...)` natively and avoid the relaxation problem entirely. HiGHS has no equivalent, so this must be built as an **accelerator with the existing Big-M path as fallback** — the same pattern CLAUDE.md prescribes for diagnostics ("Gurobi-only APIs are accelerators, never dependencies"), not as a replacement. Introducing the general-constraint channel would also open `GRBaddgenconstrIndicator` for the `<>` disjunctions and ABS linearization, which use Big-M for the same reason.

**Discovered**: 2026-07-26, while raising benchmark scale limits and asking which limits are inherent problem complexity versus our formulation. Ruled out as *not* the cause in the sibling case: Q3's L0 Big-M looseness (`norm(adj, 0, 40)` against a tight bound of 20) measured identical to the tight and inferred variants at both 60K and 120K, so Gurobi presolve repairs a loose user-supplied constant. Big-M *tightness* is handled; Big-M *encoding* is not.

---

## Canonicalization is split across five sites, in three representations

*(all five are now deleted; Phase C is complete)*

**Authoritative task list: `../../canonicalize.md`.** That document is the plan, the
status and the decision log; this entry exists only so the issue is findable from here.
Do not track progress in both places.

**What is still open** (everything else in the plan has landed):

- **Phase D** — `VerifyCanonical()` as a debug assert (K0–K3 only). C.3 already left a
  three-line K1 guard where the per-row re-partition used to be, which is Phase D in
  miniature. The live constraint it records: an optimizer pass that mutates a constraint
  **in place**, rather than going through `LogicalDecide::AddConstraint`, is the one way
  to break K1.
- **K3's full classification-driven rejection** is still undesigned. K3 makes the
  canonicalizer the single rejection site, so it inherits the binder's user-facing error
  messages for malformed constraints — a real responsibility under CLAUDE.md's
  user-facing voice rule, flagged rather than decided.
- **Open decisions D4** (objective canonicalization: the objective's
  `objective_constant_offset` is `lhs_offset_expr`'s twin) and **D6** (relocating the
  objective's grammar repair out of the symbolic layer). D1, D3, D5, D7, D8 are settled;
  **D2 is settled for constraints** — the simplifier was deleted whole at C.4, not
  demoted — and its remaining half (dropping SymbolicC++) is now a D4 question, since
  only `SimplifyDecideObjective` still uses it.

**Discovered**: 2026-07-30, writing paper §3.2. The paper's running example
(`demand - sum(ship) <= max_shortfall`, Example 1 line 11) turned out to be a shape the
binder's normalizer refuses — its RHS is a decision variable, so the rewrite happened in
the physical operator instead. That asymmetry is what surfaced the split. **That example
binds and solves as of C.2 (2026-08-12).** The binder check it died on also rejects
paper-sweep entries B1–B4; those are a *row-varying data bound*, need per-tuple fan-out,
and are still open, so whoever takes group B will reshape the same function.

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
before/after signal, which is the thing every entry in `canonicalize.md` verifies
against.

**Fix direction**: decide what the test is actually pinning. If the shape is expected to
be accepted, assert that and let a rejection fail. If acceptance is genuinely
solver-dependent, gate the skip on the *solver's* capability up front (as the
Gurobi-availability skip does) rather than on catching an exception from DeciDB, so the
skip is a property of the environment and not of the run.

**Discovered**: 2026-08-12, while verifying the nested-product-in-reducer fix. Noticed
only because that fix required comparing suite tallies across runs; irrelevant to the
fix itself.

---

`PER` on a per-row constraint slips through when a data-only reducer is present

**Location**: `src/planner/expression_binder/decide_constraints_binder.cpp`,
`IsAggregateConstraint` — `ContainsDecideAggregate(*comp.left)` accepts a reducer with no
decision variable in it.

`x <= 5 PER grp` is correctly refused ("PER can only be applied to aggregate (SUM)
constraints") because a per-row constraint already owns one row each. But
`SUM(price) >= x PER grp` passes the same gate: the left side contains a reducer, just
not one over decisions, so the constraint is per-row while `PER` says otherwise. The PER
columns are then carried into a constraint whose `lhs_is_aggregate` is false.

**Why it matters**: unlike the refused case this is silent — `PER` is accepted and then
has nothing to partition, so the user gets an answer computed as if they had not written
it. The refusal message exists precisely because that is the confusing outcome.

**Fix direction**: the honest predicate is "either side contains a reducer over decision
variables". C.2 applied exactly that to the *right* side (a data-only reducer there is a
bound, e.g. `x <= MIN(price)`, and must not make the constraint reduced) and deliberately
left the left side on the looser reading, because tightening it changes which error a
degenerate query like `SUM(price) <= 3 PER g` reports — today the more specific "SUM
expression must reference at least one DECIDE variable" from `BindComparison`, which runs
after the PER gate. Tightening both sides means reordering those two checks so the
better message still wins.

**Discovered**: 2026-08-12, landing C.2, while making `IsAggregateConstraint`
side-agnostic. Pre-existing on the left side; C.2 neither widened nor narrowed it.
---

## Defensive scale check in physical extraction is justified by the wrong reason

**Location**: `src/execution/operator/decide/physical_decide.cpp:1712-1722`,
inside `ExtractAggregateConstraintTerms`.

The comment justifies the decision-bearing-factor check with "constraints the OPTIMIZER
emits are canonicalized by the permissive path, which does not judge factors". That is
not true: `DecideCanonicalizer::PeelScale`'s `ReferencesDecideVar(*factor)` rejection does
not consult `judge_column_refs`, so it fires identically on the user-written and
generated paths. Only the *query-wide* judgement differs between them.

The check is nonetheless live and correct, for a reason the comment does not give:
`PeelScale` only judges factors on a **decision-bearing** reducer (`is_scalable` requires
`ContainsReducer && ReferencesDecideVar`), whereas `AsScaledAggregate` matches any
aggregate child. So `s * SUM(price)` — a decision scaling a *data-only* reducer — is
never seen by the canonicalizer's judgement and reaches here unjudged.

**Why it matters**: the stated reason implies the check is redundant for user-written
constraints, which invites deleting it during a later cleanup. The real reason shows it
covers a shape the canonicalizer deliberately does not own, so it must stay. A wrong
rationale on a safety check is how safety checks get removed.

**Fix direction**: correct the comment to name the data-only-reducer gap. Consider
whether that shape should instead be rejected at the canonicalization boundary, which
would fold it into Step 5's homogeneity validation — the same `is_scalable` asymmetry is
what item 3 of the `canonicalize.md` defect list ("the `PER` gate has a homogeneity
hole") is about.

**Discovered**: 2026-08-13, landing canonicalize.md Step 4 (reducer-scale totality),
while confirming which expression shapes the composed-scale form actually covers.
