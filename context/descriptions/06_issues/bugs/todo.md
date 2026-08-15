 

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

## A data reducer on the bound side still refuses an infinite value wholesale

**Symptom**: a per-group bound built from data is rejected outright if any row of the reducer's input is infinite, even where the infinity is a legitimate "no bound" for that group:

```sql
-- cap = +inf for g=0, 3.0 for g=1
SUCH THAT x >= 0 AND x <= 6 AND MIN(x) <= MAX(cap) PER g
```

```
Invalid Input Error: DECIDE constraint right-hand side aggregate contains invalid value
(NaN or Infinity) at row 0. Common causes: ...
```

The same query with a literal bound (`MIN(x) <= 1e1000::DOUBLE PER g`) now works: the infinity is classified per group and the vacuous group is dropped. Only the data-reducer spelling still refuses.

**Where to look**: `EvaluateRhsReducerPerGroup` in `src/execution/operator/decide/physical_decide.cpp` evaluates the reducer with `allow_infinite = false` (`:3537`), while the per-row RHS path directly below it passes `true` (`:4005`). Layer 8's `done.md` documents why the row's `rhs` may be infinite; this is the last RHS path that has not been moved to that rule.

**Decision to make first**: an infinity inside `MAX(cap)` is likelier to be an overflow in the user's data than a deliberate "no bound", which is not true of a literal the user typed. So this may want a different answer from the literal case — keep refusing but name the column and the row, rather than silently reading it as no bound. Decide that before changing the flag.

**Consequence today**: `ClassifyMinMaxBound`'s per-group handling is only reachable with a uniform (literal) bound; the mixed finite/infinite group case is written and tested only at the unit of a single group.

**Discovered**: 2026-08-15, while fixing the vacuous MIN/MAX infinite bound (now in `../../01_pipeline/08_execution/done.md`).

---

