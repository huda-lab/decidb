# Work queue: every open task in the docs, by feature

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `language/guide.md`, `direct_solve/README.md`, `issues/bugs.md`.
Checks: `direct_solve/framework/todo.md` § How to work any task in these todo files.

Each batch finishes completely. One working tree: agents may edit disjoint batches side by side; one builds and tests
at a time.

## Where things stand (2026-10-30)

Every filed bug is fixed and merged, HiGHS is 1.15.1, and DIAGNOSE's example corpus prints the same answer on both
backends but one (a model HiGHS refuses before solving). The large benchmark sweep (`performance/todo.md` § PERF-1)
found two regressions, both fixed (BUG-25 and BUG-28: HiGHS past its time limit; BUG-26: scope checks that sorted every
input row). What remains: a slowdown left from September (§ 2) and the original measurement work (§ 3).

## 1. Bugs

Every filed bug is fixed.

## 2. Slowdowns since the 2026-09-09 sweep

- [ ] PERF-10 Extraction: constraint setup and implied bounds still slower on Q1 — performance/todo.md § PERF-10

Q8's keyed mapping (PERF-9) is back to 257 ms, group keys are formatted only when printed (PERF-12), and Q3/Q10's memory
peak (PERF-11) is Gurobi's own (`performance/findings.md` § Regressions since 2026-09-09).

## 3. Measurement (last; the owner runs them)

- [ ] PERF-1 Measurement overhead, a correctness check, a second host — performance/todo.md § PERF-1
- [ ] PERF-2 Backend discovery versus parsing and planning — performance/todo.md § PERF-2
- [ ] PERF-3 Rank extraction work; scope-check spans in `extraction` — performance/todo.md § PERF-3
- [ ] PERF-4 Rank formulation families; the shared helper box — performance/todo.md § PERF-4
- [ ] PERF-5 Gurobi and HiGHS loading overhead — performance/todo.md § PERF-5
- [ ] PERF-6 Scale ladders on both backends — performance/todo.md § PERF-6
- [ ] PERF-7 Readback, temporary results and cleanup — performance/todo.md § PERF-7
- [ ] PERF-8 Where each workload stops fitting in memory — performance/todo.md § PERF-8
