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
input row). What remains: the extraction costs the measurements found (§ 3) and the rest of the measurement work
(§ 4).

## 1. Bugs

Every filed bug is fixed.

## 2. Slowdowns since the 2026-09-09 sweep

Every slowdown found is fixed.

Q8's keyed mapping (PERF-9) is back to 257 ms, group keys are formatted only when printed (PERF-12), and Q3/Q10's memory
peak (PERF-11) is Gurobi's own (`performance/findings.md` § Regressions since 2026-09-09).

## 3. Extraction costs

- [ ] PERF-3 A passing determination check materializes every row; in progress (lane AN) —
  performance/todo.md § PERF-3
- [ ] PERF-13 Q8's key values: keep representative rows, format values late — performance/todo.md § PERF-13
- [ ] PERF-17 Q4's formulation, 310 ms (composed MIN/MAX 240), cause unknown — performance/todo.md § PERF-17
- [ ] PERF-15 RHS-bound windows hold every source column; measure, then PERF-3's shape —
  performance/todo.md § PERF-15
- [ ] PERF-16 Constant folding where a tree is built; first, messages quote the source text —
  performance/todo.md § PERF-16

## 4. Measurement (last)

The orchestrator runs the short tasks; the owner runs `benchmark/decide/direct_solve/run.py`.

- [ ] PERF-1 The sweep on a second host — performance/todo.md § PERF-1
- [ ] PERF-6 Scale ladders on both backends — performance/todo.md § PERF-6
- [ ] PERF-8 Where each workload stops fitting in memory — performance/todo.md § PERF-8
