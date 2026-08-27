# Code Quality Issues — Open

Code-quality issues include duplication, dead code, fragile patterns, unclear
naming, missing coverage, and diagnostic-quality defects that do not change the
ordinary optimization result. Solve-correctness bugs belong in `../bugs/todo.md`.

Each entry: short title, location (`file:line`), what's wrong, why it matters, and when/during which task it was discovered.

## Big-M and envelope constants ignore the rigid box they are contracted to use

**Location**: the contract is defined at
`src/execution/operator/decide/physical_decide.cpp:2598-2617`
(`SolverInput::rigid_lower_bounds` / `rigid_upper_bounds`). Three sites derive a constant
without it:

| Construct         | Site                                                                                         | Reads                                |
| ----------------- | -------------------------------------------------------------------------------------------- | ------------------------------------ |
| L0 `norm` auto-M | `physical_decide.cpp:2672` → `DecideTightPerRowBigM`                              | `lower_bounds` / `upper_bounds`     |
| `<>` lowering    | `ilp_linearization.cpp` `LowerDecideConstructs` → `FlatRowReach` → `FlatColumnBox` | `lower_bounds` / `upper_bounds`     |
| McCormick        | `ilp_linearization.cpp` `LinearizeBilinear`                                       | `lower_bounds` / `upper_bounds`     |

**What's wrong.** `SolverInput` carries two column boxes. `lower_bounds`/`upper_bounds` is
the box as the query states it, with user bounds like `x <= 5` absorbed into the column by
stage 05. `rigid_*` is the same box with every user-absorbed direction re-opened, and
`physical_decide.cpp:2598` defines it as "the only part of the column box that survives
infeasibility diagnosis unchanged, and therefore **the only part a structural rewrite may
rely on**."

A Big-M and a McCormick envelope constant are exactly such structural-rewrite constants, so
by that contract they must read `rigid_*`. None of them does — `rigid_*` has only two
consumers in the whole tree, both in the `<>` *classifier* (`ilp_linearization.cpp:1122`,
`:1238`), never in a derived constant. So each site bakes the user's own repairable bound
into a constant.

**Why it matters.** The elastic engine repairs an infeasible query by widening a bound. It
may add all the slack it likes to `x <= 5`; the baked constant still says `x` cannot pass
~5, so the widening repair is unrepresentable and the search settles for gutting a different
clause — usually the user's actual target.

Two signatures, both measured:

- **Big-M paths (L0, lowered `<>`)** — the repair caps at exactly `box + 1`, the trailing
  `+ 1.0` in `DecideTightPerRowBigM` and `LowerDecideConstructs`. `x <= 5` → repair
  `x <= 6`; `x <= 7` → `x <= 8`.
- **McCormick** — the box is never widened at all; the envelope caps the product at the
  stale `x_U`, so the engine only cuts the user's target down to what the box allows. The
  reported achievable objective tracks `rows × box` exactly (box 5 → 15, 7 → 21, 9 → 27).

Three rows, `x >= 0 AND x <= 5`; "obj" is the reported achievable objective, and the
control's is the correct one:

| Case                             | Gurobi                    | HiGHS                      | Correct |
| -------------------------------- | ------------------------- | -------------------------- | ------- |
| control `SUM(x) >= 60`         | 60 (`x <= 20`)          | 60 (`x <= 20`)           | 60      |
| control `SUM(x) >= 30`         | 30 (`x <= 10`)          | 30 (`x <= 10`)           | 30      |
| `+ x <> 3`                     | 60 (`x <= 20`)          | **18** (`x <= 6`)  | 60      |
| `+ norm(x, 0) <= 2`            | **18** (`x <= 6`) | **15** (no box edit) | 30      |
| `SUM(b * x) >= 30`, `b` BOOL | **15**              | **15**               | 30      |

`<>` is clean on Gurobi only because Gurobi states it natively and `LowerDecideConstructs`
returns early, deriving no Big-M at all. On HiGHS it is lowered and shows the `box + 1` cap.
**So the reported repair depends on the backend** — the same query is told to widen to
`x <= 20` on one solver and `x <= 6` on the other. That is precisely the per-backend
divergence `DemandedAuxReach`'s own comment warns about; B3 fixed it for ABS and MIN/MAX and
it survived everywhere else.

**Severity: quality, not correctness of the solve.** Every repair probed is valid — applying
it verbatim makes the query solve. What is wrong is that a smaller, less invasive repair
exists, is not offered, and is not backend-stable. Note that `achievable_objective` is
reported low in these cases (18 or 15 where 30 is reachable), so a user-facing number *is*
wrong even though no solve is.

**Isolating proof.** The L0 Big-M can be supplied by hand. `norm(x, 0, 100)` instead of
`norm(x, 0)` — same query, same repair weighting — moves the answer from "three edits,
objective 18" to "one edit, `SUM(x) >= 30` untouched, objective 30". That rules out the
repair-tie weighting and isolates the constant as the cause.

**Fix shape.** Point the three sites at `rigid_*`. ABS
and MIN/MAX read the non-rigid box too, but B3 gave them a targeted mitigation
(`DemandedAuxReach`, family widening) that patches the symptom for the *auxiliary* case;
`rigid_*` is the general contract, introduced by the `<>` classifier work and never adopted
by the constant-deriving paths. The change **widens every affected Big-M**, so it moves the
golden dumps and loosens root relaxations on queries that solve fine today. It needs its own
batch with a golden re-capture and a benchmark pass.

**Test**: the five rows of the table above, asserted with `assert_backends_agree` — the
per-backend divergence is the sharpest available oracle and needs no hand-computed number.
`test_query_diagnostics_relation.py::TestRepairsTheModelCanReach` is where B3's analogous
cases live.

**Done file**: `07_query_diagnostics/infeasible/done.md` — the B3 "Residual class, accepted
not fixed" paragraph is what this closes.

**Discovered**: 2026-08-27 during release review.

---

## A cancelled-by-you message on a composed query that no one cancelled

**Location**: the slow-solve checkpoint report in
`src/execution/operator/decide/physical_decide.cpp` (S2 block, "Slow-solve checkpoint
report"), reached from `Finalize`.

**What's wrong.** When a SELECT composes two DECIDE subqueries side by side and the
*second* one is infeasible, its throw aborts the pipeline while the first is still
running. The first operator then prints its interruption report, which is worded for
Ctrl-C:

```
DECIDE stopped at your request before finding a solution yet.
  elapsed 0.00065s · peak memory 29.3 MB
Invalid Input Error: DECIDE optimization is infeasible. …
```

Nobody asked it to stop. The two lines contradict each other, and the elapsed/memory
figures describe a solve that was never the problem.

**Why it matters.** It is the first thing printed, so it is the first thing read, and
it sends the user looking for a cancellation instead of the infeasible clause named
underneath. Purely presentational — the error and its exit status are correct.

**Reproduces**: two DECIDE subqueries joined with `USING`, the second infeasible, no
`DIAGNOSE` prefix. A single DECIDE clause does not show it, and neither does a *nested*
DECIDE whose inner clause is infeasible — only the sibling shape, where one operator is
live when another throws.

**Fix shape**: the interruption report should distinguish "the user interrupted" from
"another operator in this query failed" and stay silent in the second case.

**Test**: the composed-infeasible query above, asserting the cancellation wording is
absent while the infeasible error is present. `test_diagnose_trigger.py` already holds
the sibling fixtures.

**Discovered**: 2026-08-27, while verifying the multi-DECIDE `DIAGNOSE` refusal.
