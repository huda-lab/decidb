 

# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

Resolved entries are removed; if the fix taught a generalizable lesson, record it
in `.claude/lessons.md`.

---

## `EXPLAIN` renders binder-inserted casts over the user's own terms

**Symptom**: the Constraints and Objective rows of the DECIDE node show the
implicit casts DuckDB's binder inserted while reconciling types, so a constraint
the user wrote as `SUM(x * l_quantity) <= 100` renders as:

```
(sum((CAST(x AS DECIMAL(18,0)) * l_quantity)) <= CAST(100 AS DECIMAL(38,2)))
```

Expected: `(sum((x * l_quantity)) <= 100)`. The casts are an artifact of binding,
not something the user wrote or can edit.

**Why it is open now**: this was invisible until the `__source_clause_N__` leak
below it was fixed — the whole row used to print as one internal tag, so nothing
about the expression underneath was observable.

**Where to look**: `RenderDecideExpressionName` /
`CollectDecideExpressionStrings` in `src/planner/operator/logical_decide.cpp`
render the leaf with `ToString()`, which prints the tree as bound. Layer 8 already
solves exactly this for diagnosis labels — `RenderWhenPredicate` and
`RenderDiagnosticRhsLabel` in `src/execution/operator/decide/physical_decide.cpp`
recurse through the tree unwrapping casts via `StripCastsForIdentity`, which
`decide_cast_policy.hpp` documents as correct precisely for label rendering. The
infeasible-diagnosis path is clean for this reason and prints
``SUM(x * w) >= 100 WHEN grp = 'a'``.

The open question is ownership, not mechanism: EXPLAIN renders the bound tree at
layer 3, while the diagnosis label is rebuilt at layer 8 from evaluated ILP
provenance (`weight_labels` / `rhs_label` / `qualifier`), so the two cannot share
the layer-8 renderer as it stands. Either a cast-unwrapping recursive renderer
belongs beside the walker in layer 3, or the two renderers get consolidated into
one user-facing expression renderer both layers call.

**Discovered**: 2026-08-14, immediately after fixing the source-clause tag leak.

---

## An infeasible constraint with no nonzero coefficients crashes infeasible diagnosis (live, crashes the connection)

**Symptom**: a constraint row in which every decision-variable coefficient is zero, on a model that is **infeasible**, aborts with an internal assertion instead of reporting infeasibility — and the failure is fatal, so the connection is invalidated and every later statement in the session fails with *"database has been invalidated because of a previous fatal error"*.

```
INTERNAL Error: Attempted to access index 0 within vector of size 0
  ... duckdb::PhysicalDecide::Finalize ...
```

**Reproduction** (any of these; two-row table, `x(INT)`, `x <= 5`, `MAXIMIZE SUM(x)`):

```sql
SUCH THAT 0 * x <= -1                -- per-row, literal zero factor
SUCH THAT SUM(0 * x) <= -1           -- aggregate, zero inside the reducer
SUCH THAT SUM(x * 0) <= -1           -- same, other operand order
SUCH THAT x - x <= -1                -- zero by CANCELLATION, no literal 0 present
SUCH THAT SUM(x) - SUM(x) <= -1      -- cancellation between reducers
SUCH THAT 0 * SUM(x) <= -1           -- factor outside the reducer
```

**What is established**:

- It is the **infeasibility** that triggers it, not the zero coefficients. `0 * SUM(x) <= 1` (feasible, `0 <= 1`) is fine. `<= -1`, `>= 1` and `= 1` all crash.
- It is the **diagnosis path**, confirmed by bisecting with the session setting: `SET diagnose_decide='off'` makes every reproduction above return the correct, clean error — *"DECIDE optimization is infeasible: the SUCH THAT constraints cannot all be satisfied at once…"*. So model building and the solve are healthy; only the engine that runs *after* an infeasible result is broken.
- Both slack scopes crash (`SET diagnose_decide_infeasible_slack_scope` = `query` and `expanded`), so the fault is in shared elastic-engine setup rather than in either reporting mode.
- **Not aggregate-specific and not scale-specific.** The cancellation cases (`x - x <= -1`) contain no literal zero anywhere and still crash, so any fix aimed narrowly at "a zero literal factor" would miss it. The real precondition is an emitted row whose coefficient list is empty after zero elimination.

**Ruled out**: `rms_norm` in `decide_diagnostic_engines.cpp:562` — the obvious suspect, since it consumes `row.coefficients` — already guards `coeffs.empty()` and returns 0.0. The empty-vector access is somewhere else on that path.

**Not caused by the B.3 scaled-reducer work.** Verified by writing the shapes that avoid it entirely: hand-written `SUM(0 * x) <= -1` and `x - x <= -1` crash identically and involve no factor on a reducer. B.3 only added `0 * SUM(x) <= -1` as one more spelling that reaches it.

**Where to look next**: the infeasible-diagnosis entry point in `PhysicalDecide::Finalize` and the elastic-model construction it calls in `src/decidb/utility/decide_diagnostic_engines.cpp`. Look for a place that assumes a relaxable row has at least one coefficient — picking a representative column, deriving a per-row scale, or building the slack column for a row — rather than for a second norm helper. A debug build will name the line immediately; the release trace inlines it away.

**Discovered**: 2026-08-10, auditing Phase B.3 (scaled reducers) by sweeping degenerate factor values.

---

## Bound absorption uses a single term's coefficient when a variable appears more than once in a constraint

**Symptom**: a per-row constraint whose LHS mentions the same decision variable in several additive terms gets a column bound derived from one term's coefficient rather than from their sum. The bound is loose, never wrong — the emitted row still carries the correct combined coefficient — so results are correct but the LP relaxation is weaker than it should be.

**Reproduction** (`DECIDB_DUMP_MODEL=dump.txt`, single-row table):

```sql
SUCH THAT 3*ship <= 10          -- col 0: ub=3.333  row: 0:3   ✅
SUCH THAT 2*ship <= 10          -- col 0: ub=5      row: 0:2   ✅
SUCH THAT 2*ship + 3*ship <= 10 -- col 0: ub=3.333  row: 0:5   ❌ ub should be 10/5 = 2
SUCH THAT 3*ship + 2*ship <= 10 -- col 0: ub=3.333  row: 0:5   ❌ same, so it is not term order
```

Both orderings give `10/3`, so the pass appears to select the largest single coefficient rather than accumulating. The row itself is correct (`0:5`), which is why no test caught this: the optimum is unchanged.

**Why it matters**: `ub` is 1.67x looser than necessary in this example. Column bounds feed the root relaxation and the Big-M constants computed from `global_max - global_min`, so a loose bound propagates into every Big-M encoding in the same query.

**Where to look**: the constructor's bound-absorption pass in `src/execution/operator/decide/physical_decide.cpp` (`absorbed_bound_exprs`, `TraverseBoundsConstraints`, around `:2247` where `COMPARE_LESSTHAN`/`COMPARE_GREATERTHAN` are folded into integer bounds). It reads a coefficient off the comparison without combining repeated references to the same variable.

**Note**: the symbolic simplifier does not repair this — it only fires when the LHS contains a `SUM`, so per-row constraints reach the absorption pass unnormalized. Like-term collection would mask the symptom, but the pass should accumulate regardless of the shape it is handed.

**Discovered**: 2026-08-10, first use of the new `DECIDB_DUMP_MODEL` oracle while capturing baseline dumps for the canonicalization refactor.

---



## An infinite bound in a rebuilt additive RHS is rejected, while the same bound alone is accepted

**Symptom**: `x <= 1e1000::DOUBLE` (an infinite bound) solves fine — it is absorbed as a column bound and correctly acts as "unconstrained". But move any term across the comparison so the RHS becomes an additive expression and the same bound is refused:

```sql
-- accepted, x = 6
SUCH THAT x >= 0 AND x <= 6 AND x <= 1e1000::DOUBLE
-- refused
SUCH THAT x >= 0 AND x <= 6 AND x + v <= 1e1000::DOUBLE
```

```
Invalid Input Error: DECIDE constraint right-hand side contains invalid value (NaN or Infinity) at row 0.
```

**Cause**: canonicalization rebuilds the bound as `1e1000 - v`, which evaluates to `Infinity`, and `ExtractDoubleColumn` rejects non-finite values wholesale. An infinite upper bound is not an error — it is the absence of a constraint — so the row should either be dropped or passed to the solver as its infinity sentinel.

**Not cast-related.** The reproduction above uses only INTEGER columns and contains no lossy cast. It was previously visible only through `test_canonicalize_cast.py::test_lossy_cast_infinite_bounds`, because canonicalization Step 2's preimage path happened to bypass `ExtractDoubleColumn` for cast-bearing constraints and so masked the general gap for that one shape. Removing the preimage machinery unmasked it; behaviour is now uniform across cast and non-cast constraints, which is why that test was retired rather than repaired.

**Where to look**: `ExtractDoubleColumn` and the RHS evaluation loop in `PhysicalDecide::Finalize` (`src/execution/operator/decide/physical_decide.cpp`); the accepted path is bound absorption in `TraverseBoundsConstraints`.

**Discovered**: 2026-08-13, replacing the cast-preimage machinery with the one-site cast policy.

---
