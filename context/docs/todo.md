# Work queue: every open task in the docs, by feature

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `language/guide.md`, `direct_solve/README.md`, `issues/bugs.md`.
Checks: `direct_solve/framework/todo.md` § How to work any task in these todo files.

Each batch finishes completely. One working tree: agents may edit disjoint batches side by side; one builds and tests
at a time.

## Where things stand (2026-10-30)

Every bug filed up to BUG-26 is fixed and merged. The large benchmark sweep (90 runs, `performance/todo.md` § PERF-1)
found two regressions, both fixed: HiGHS ran past its time limit on Q1 (BUG-25: two HiGHS loops never read the clock),
and Q8 ran five times slower (BUG-26: scope checks that can never fail sorted every input row). Overall, total solve
time on Gurobi is 3 % lower than in September. What remains: two new bugs that need a ruling (§ 1), three slowdowns
since September with no known cause yet (§ 2), posting the HiGHS report (§ 3), and the original measurement work
(§ 4).

## 1. Bugs (the owner rules each fix before work starts)

- [ ] BUG-27 Two collations of one column share a key, so a scope check that should stop the query passes — issues/bugs.md § BUG-27
  - Suggested ruling: key elements compare as rows group on them, collation included, in `FindOrCreateDecideKey` and
    `KeyHolds`. Batch alone (planner). Record in `pipeline/04_canonicalizer/done.md` § Scope checks.
- [ ] BUG-28 HiGHS's parallel rows and columns presolve ignores the time limit — issues/bugs.md § BUG-28
  - Suggested ruling: the BUG-23 pattern, a clock check once per amount of work. Batch alone (`third_party/highs`).
    Record in `pipeline/07_solver/done.md`, `third_party/highs/README.md`, `issues/highs_upstream.md` § 6.

## 2. Slowdowns since the 2026-09-09 sweep (cause unknown; measure first)

- [ ] PERF-9 Q8's keyed-decision mapping: 229 → 1,169 ms — performance/todo.md § PERF-9
- [ ] PERF-10 Extraction 40-240 ms slower on most queries — performance/todo.md § PERF-10
- [ ] PERF-11 Peak memory up 0.7-0.8 GiB on Q3 and Q10 — performance/todo.md § PERF-11

One batch: one profiling pass at `large` on the current build serves all three; fixes follow per cause.

## 3. The owner's actions

- [ ] Post the HiGHS upstream issue (five loops; BUG-28 makes six once fixed) — issues/highs_upstream.md
  - Before posting: attach an MPS file for Q1, Q2, Q3 and Q10 at `large`; check the current HiGHS release.

## 4. Measurement (last; the owner runs them)

- [ ] PERF-1 Measurement overhead, a correctness check, a second host — performance/todo.md § PERF-1
- [ ] PERF-2 Backend discovery versus parsing and planning — performance/todo.md § PERF-2
- [ ] PERF-3 Rank extraction work; scope-check spans in `extraction` — performance/todo.md § PERF-3
- [ ] PERF-4 Rank formulation families; the shared helper box — performance/todo.md § PERF-4
- [ ] PERF-5 Gurobi and HiGHS loading overhead — performance/todo.md § PERF-5
- [ ] PERF-6 Scale ladders on both backends — performance/todo.md § PERF-6
- [ ] PERF-7 Readback, temporary results and cleanup — performance/todo.md § PERF-7
- [ ] PERF-8 Where each workload stops fitting in memory — performance/todo.md § PERF-8
