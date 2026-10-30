# Work queue: every open task in the docs, by feature

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `language/guide.md`, `direct_solve/README.md`, `issues/bugs.md`.
Checks: `direct_solve/framework/todo.md` § How to work any task in these todo files.

Each batch finishes completely. One working tree: agents may edit disjoint batches side by side; one builds and tests
at a time.

## Where things stand (2026-10-30)

Every bug filed up to BUG-28 is fixed and merged. The large benchmark sweep (90 runs, `performance/todo.md` § PERF-1)
found two regressions, both fixed: HiGHS ran past its time limit on Q1 (BUG-25, then BUG-28 in presolve), and Q8 ran
five times slower (BUG-26: scope checks that can never fail sorted every input row). Overall, total solve time on
Gurobi is 3 % lower than in September. What remains: work in progress (§ 1), three slowdowns since September (§ 2),
posting the HiGHS report (§ 3), and the original measurement work (§ 4).

## 1. In progress (2026-10-30)

- [ ] Upgrade HiGHS to its latest release through `make upgrade-highs` (a patch series over pristine upstream). HiGHS
  1.11 takes 55 s on R1 at 1.6M decisions, 10 s on 2026-10-26: since bounds reach it as column bounds, its postsolve
  returns a basis primal simplex repairs for 79 s; 1.13.1 solves the same LP in 1.4 s. Record in
  `third_party/highs/README.md`, `pipeline/07_solver/done.md`, `issues/highs_upstream.md`,
  `performance/benchmarking/direct_solve.md`.
- [ ] Diagnostics ready to capture: no raw solver status in a message, a small gap printed truthfully, and
  `benchmark/decide/diagnose_examples/run.py` recording DIAGNOSE for a corpus reaching every `edit_source`. Record in
  `pipeline/07_solver/done.md`, `diagnostics/README.md`.

## 2. Slowdowns since the 2026-09-09 sweep

- [ ] PERF-9 Q8's keyed-decision mapping: 229 → 1,169 ms — performance/todo.md § PERF-9
- [ ] PERF-10 Extraction 40-240 ms slower on most queries — performance/todo.md § PERF-10
- [ ] PERF-11 Peak memory up 0.7-0.8 GiB on Q3 and Q10 — performance/todo.md § PERF-11

In progress as one batch: the profiling pass found the causes of PERF-9 and PERF-10 (per-row `Value` work in group ids
and labels, one scan per filter part); fixes are landing. PERF-11's peak is inside Gurobi's solve.

## 3. The owner's actions

- [ ] Post the HiGHS upstream issue (six loops) — issues/highs_upstream.md
  - Before posting: attach an MPS file for Q1, Q2, Q3 and Q10 at `large`; list only the loops the upgraded release
    still lacks (§ 1).

## 4. Measurement (last; the owner runs them)

- [ ] PERF-1 Measurement overhead, a correctness check, a second host — performance/todo.md § PERF-1
- [ ] PERF-2 Backend discovery versus parsing and planning — performance/todo.md § PERF-2
- [ ] PERF-3 Rank extraction work; scope-check spans in `extraction` — performance/todo.md § PERF-3
- [ ] PERF-4 Rank formulation families; the shared helper box — performance/todo.md § PERF-4
- [ ] PERF-5 Gurobi and HiGHS loading overhead — performance/todo.md § PERF-5
- [ ] PERF-6 Scale ladders on both backends — performance/todo.md § PERF-6
- [ ] PERF-7 Readback, temporary results and cleanup — performance/todo.md § PERF-7
- [ ] PERF-8 Where each workload stops fitting in memory — performance/todo.md § PERF-8
