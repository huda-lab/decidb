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

## Two "no internal error leaked" tests still forbid a symbol that no longer exists

**Location**: `test/decide/tests/test_error_unsupported_operator.py:204-208` and
`test/decide/tests/test_error_case_expression.py:125-129` — both assert that a
`forbidden` token list does not appear in the CLI's combined stdout+stderr:

```python
forbidden = [
    "INTERNAL Error",
    "Stack Trace",
    "ToSymbolicRecursive",   # <- symbol deleted
    "assertion failure",
]
```

`ToSymbolicRecursive` was deleted with the SymbolicC++ layer, so that one entry can
never match. The other three still do real work; only this element is vacuous. The
docstring at `test_error_unsupported_operator.py:108` also still names the symbol as
the thing the test is a regression pin *against* — it was rewritten to describe the
behavior instead, but the assertion list was deliberately left untouched.

**Why it matters**: the tests are not broken and are not reporting a false green —
their load-bearing assertions (`INTERNAL Error`, `Stack Trace`, `assertion failure`)
are intact, so a genuine regression to the internal-error path still fails them.
The cost is one dead element that reads as protection and is not, and the risk is
that a later reader treats the list as a maintained inventory of leak markers rather
than an ad-hoc set. If a *new* internal path can leak a symbol name, nothing here
would catch it — the list has no mechanism for staying current.

**Fix direction**: two options, and the second is the more useful one.
(a) Delete the dead element, leaving the three general markers.
(b) Replace the symbol-specific entry with a pattern that keeps working as internals
change — e.g. reject any `__[A-Za-z_]+__` internal tag or any `duckdb::` frame in
user-facing output. That would also have caught the live `__source_clause_N__` leak
in `EXPLAIN` filed in `../bugs/todo.md`, which is exactly this class of defect and
was found by eye rather than by a test.

**Not done here deliberately**: changing a test's assertions is a behavioral decision
about what the test pins, not documentation cleanup.

**Discovered**: 2026-08-14, correcting test docstrings during the pipeline
documentation restructure.

---

## `DecideDegreeInternal` under-estimates degree through `POWER`, so degree-3 products are caught late

**Location**: `src/planner/expression_binder/decide_binder.cpp:100-137` (`DecideDegreeInternal`); the late rejection comes from `src/execution/operator/decide/physical_decide.cpp:742`.

The degree function handles the `+`/`-`/`*`/`/` spine exactly and falls back to occurrence counting for everything else, `POWER` included. So `POWER(x,2)` reports degree 1 rather than 2, and `SUM(POWER(x,2) * y)` — genuinely degree 3 — passes the binder gate at line 620.

Nothing computes a wrong answer: physical extraction rejects all three shapes (`POWER(x,2) * y` in a constraint, the same in an objective, and `POWER(x,2) * POWER(y,2)`), verified 2026-08-14. But the rejection arrives as an `Invalid Input Error` from the extractor instead of a `Binder Error`, and its wording is both stale and jargon-laden: *"still references decision variables after normalization (total degree > 2 or unexpanded nonlinear product)"* names a normalization pass that no longer exists and offers the user no edit.

**Why it matters**: the fix is ~6 lines — a `POWER`/`**` case returning `degree(base) * n` for constant `n` — and it would move the message to the boundary that already words these well. Left out of the Step 8 change deliberately, to keep the degree fix provably scoped to the measured failure: tightening `POWER` could reject shapes that pass today, which needs its own before/after run rather than riding along.

**Discovered**: 2026-08-14, auditing the degree fix during the canonicalization refactor.

---

## The committed golden baseline is older than the corpus it baselines

**Location**: `test/decide/golden/baseline.dump` (+ `.results`) vs `test/decide/golden/corpus.sql`.

`baseline.dump` was captured 2026-08-12; `corpus.sql` was last edited 2026-08-13 and currently has uncommitted changes. Diffing a fresh capture against the committed baseline therefore reports differences that are corpus edits, not regressions.

**Why it matters**: `capture.sh`'s whole purpose is to be a characterization oracle — a change claiming not to alter the built model is verified by diffing dumps. A baseline that no longer corresponds to the corpus silently removes that guarantee, and the natural reaction to a noisy diff is to stop trusting it. Step 8's verification worked around this by capturing its own before/after pair, which is correct method but does not repair the committed artifact.

**Fix**: regenerate `baseline.dump` and `baseline.dump.results` in the same commit that settles `corpus.sql`.

**Discovered**: 2026-08-14, during the canonicalization refactor measurement.
