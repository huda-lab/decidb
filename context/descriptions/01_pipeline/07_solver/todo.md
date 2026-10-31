# Stage 07 — Solver: open work

## 1. HiGHS 1.11.0's MIP presolve answers correct models wrongly

**Why.** The vendored HiGHS (`third_party/highs`, 1.11.0) is wrong on models the ANR
phase-6 fuzzer built (scratchpad `fuzz/fuzz_compose.py`, seeds 540, 790 and 7129), each formulated
correctly — Gurobi solves each to the oracle's optimum under both the native and the lowered
formulation, and HiGHS with `presolve` off does too:

- **A suboptimal point called optimal.** Seed 540 (`MINIMIZE sum(x) + avg(x - max(from
  previous to first every 3: x + 0.5) over (t.period desc within t.g))`, beside two clauses):
  presolve reduces the model to nothing and reports 0.5, where the optimum is −0.5.
- **A feasible model called infeasible.** Seed 790 (an AVG over `abs` of a range, beside a
  `min` range, maximizing an AVG of a range): presolve reports *infeasible*; the optimum is
  2.8333. Seed 7129 (`x >= min(from next to previous else 0.5 * t.w: 2) over (...)`,
  maximizing a MAX of a range) likewise, where the optimum is 5.5.

The models, dumped (`DECIDB_DUMP_MODEL`) and fed to `highspy`, reproduce the answers on
1.11.0, and are solved right on 1.12.0, 1.13.0, 1.14.0 and 1.15.1. None is caught by the
retry without presolve (`deterministic_naive.cpp`), which follows a solve *error* only: a
wrong optimum cannot be told from a right one, and re-solving every infeasible MIP without
presolve could cost minutes on a large model.

**What it needs.** Upgrading the vendored HiGHS to 1.12.0 or later (1.15.1 is current),
then the dual-backend verification list; the three models make regression tests once a
fixed HiGHS is in.

**Decision (2026-10-30).** The user keeps 1.11.0 for now; the upgrade waits until they
choose it. Meanwhile a HiGHS answer on a model shaped like these three may be wrong, and
Gurobi is unaffected.
