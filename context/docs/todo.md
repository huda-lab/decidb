# Work queue: every open task in the docs, by feature

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `language/guide.md`, `direct_solve/README.md`, `issues/bugs.md`.
Checks: `direct_solve/framework/todo.md` § How to work any task in these todo files.

Each batch finishes completely. One working tree: agents may edit disjoint batches side by side; one builds and tests
at a time. Lanes: F8. F9 runs alone.

## 9. F8 Solver runtime

- [ ] BUG-3 Q10 on HiGHS at large is killed for memory (owner runs) — issues/bugs.md § BUG-3
- [ ] BUG-16 A huge coefficient times a bound overflows a helper's box — issues/bugs.md § BUG-16

## 10. F9 Housekeeping (runs alone)

- [ ] OPS-2 Normalize the DecidB spelling — operations/todo.md § OPS-2

## 11. Measurement (last; the owner runs them)

- [ ] TST-1 A test large enough to see an optimality gap — testing/todo.md § TST-1
- [ ] PERF-1 Measurement overhead, a correctness check, a second host — performance/todo.md § PERF-1
- [ ] PERF-2 Backend discovery versus parsing and planning — performance/todo.md § PERF-2
- [ ] PERF-3 Rank extraction work; scope-check spans in `extraction` — performance/todo.md § PERF-3
- [ ] PERF-4 Rank formulation families; the shared helper box — performance/todo.md § PERF-4
- [ ] PERF-5 Gurobi and HiGHS loading overhead — performance/todo.md § PERF-5
- [ ] PERF-6 Scale ladders on both backends — performance/todo.md § PERF-6
- [ ] PERF-7 Readback, temporary results and cleanup — performance/todo.md § PERF-7
- [ ] PERF-8 Where each workload stops fitting in memory — performance/todo.md § PERF-8

## 12. Waiting

- [ ] BUG-2 An off-mode run reported one outcome fewer (waits for a recurrence) — issues/bugs.md § BUG-2
- [ ] OPS-1 Peak process memory on Windows (needs a Windows host) — operations/todo.md § OPS-1
- [ ] H-02 Front end for a second syntax (owner decides if it applies) — direct_solve/framework/todo.md § H-02
- [ ] H-06 The two-level objective (needs a rule that takes the shape) — direct_solve/framework/todo.md § H-06
- [ ] H-08 VALUE-shape clauses (owner names the rule and shape) — direct_solve/framework/todo.md § H-08
- [ ] A2, A3, A4 Start a class not yet built (owner picks) — direct_solve/todo.md
