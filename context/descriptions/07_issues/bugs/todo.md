 

# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

Resolved bugs are moved to `done.md`.

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

## ABS Big-M analysis assumes a data coefficient is non-negative (live, data-dependent)

**Status**: reachable today, no canonicalization required. Deliberately left open by Phase A of the canonicalization refactor, which fixes only the *syntactic* half of the same root cause. **Scheduled** (decided 2026-08-10): fix after the canonicalization plan completes, not inside it — it is independent of the five-site refactor, and Phase C.3 churns the same ABS emission path. Do not pull it forward without re-deciding.

**Symptom**: `SUCH THAT SUM(w * ABS(x - t)) <= K` is classified "sound, no Big-M" regardless of `w`. For any row where `w < 0`, that row's ABS auxiliary is free to float upward — making it larger makes the row easier — so the auxiliary no longer equals `|x - t|` and the constraint is weaker than written. Wrong results, not just a weak relaxation.

**Cause**: same root as the entry above. `ClassifyAbsConstraints` (`decide_optimizer.cpp:290`) asks only "does this side contain an ABS over a decide var, and does the relation upper-bound this side?" via `ContainsAbsOverDecideVar`, which walks the whole side without tracking the sign of the path it took. Phase A makes the walk sign-aware through `+`, binary `-`, unary `-` and negative numeric literals — all statically known. A data-column factor's sign is not known until execution, so Phase A leaves it optimistic.

**Why it was scoped out of Phase A**: the only sound *static* answer for an unknown-sign factor is "assume it may be negative, add Big-M", which would put a binary indicator on every coefficient-scaled ABS constraint including the common `w >= 0` case. Phase A's mandate was to not regress on canonicalization, not to close every sign hole.

**Two candidate fixes, in the order they should be tried:**

1. **Conservative, then measure.** Tag Big-M whenever a factor's sign is not statically known — roughly a one-line change to `CollectAbsWithSign`'s `*`/`/` branch, sound immediately. Then run `/bench` and find out what it actually costs. The claim that this is "a large performance regression" is an **assumption, never measured**; if the benchmark says otherwise, this is the whole fix and option 2 is unnecessary work. Try this first.
2. **Decide per row at execution time.** Only if the measurement shows a real regression. The physical layer already evaluates `w` per row (`EvaluateTermCoefs`), so the Big-M rows could be emitted only for rows whose coefficient is actually negative. The obstacle is that the `y` indicator is allocated at **plan** time (`decide_optimizer.cpp:1153`, inside `if (needs_bigm)`), so it cannot be decided lazily as the code stands. The shape of the fix: make the plan-time tag tri-state (pinned / needs-Big-M / sign-unknown), allocate `y` speculatively for sign-unknown, emit the Big-M rows at runtime only where needed, and fix the unused `y` to a constant so presolve drops the column.

**Discovered**: 2026-08-10, reading `ClassifyAbsConstraints` while scoping Phase A of the canonicalization refactor.

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
