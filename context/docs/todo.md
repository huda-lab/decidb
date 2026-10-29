# Work queue: every open task in the docs, by feature

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `language/guide.md`, `direct_solve/README.md`, `issues/bugs.md`.
Checks: `direct_solve/framework/todo.md` § How to work any task in these todo files.

Each batch finishes completely. One working tree: agents may edit disjoint batches side by side; one builds and tests
at a time. Lanes: F11.

## 10. F11 Derived-number overflow (one lane; BUG-16 states the rule)

- [ ] BUG-16 An overflowed range reads as an open variable — issues/bugs.md § BUG-16
- [ ] BUG-18 A literal infinite coefficient on one decision drops the clause — issues/bugs.md § BUG-18
- [ ] BUG-19 R1's SQL overflows to an infinite room — issues/bugs.md § BUG-19
- [ ] BUG-21 One overflow, different messages — issues/bugs.md § BUG-21

## 11. Measurement (last; the owner runs them)

- [ ] TST-1 A test large enough to see an optimality gap — testing/todo.md § TST-1
- [ ] BUG-23 Q3 on HiGHS at large overran its limit in the September sweep — issues/bugs.md § BUG-23
- [ ] PERF-1 Measurement overhead, a correctness check, a second host — performance/todo.md § PERF-1
- [ ] PERF-2 Backend discovery versus parsing and planning — performance/todo.md § PERF-2
- [ ] PERF-3 Rank extraction work; scope-check spans in `extraction` — performance/todo.md § PERF-3
- [ ] PERF-4 Rank formulation families; the shared helper box — performance/todo.md § PERF-4
- [ ] PERF-5 Gurobi and HiGHS loading overhead — performance/todo.md § PERF-5
- [ ] PERF-6 Scale ladders on both backends — performance/todo.md § PERF-6
- [ ] PERF-7 Readback, temporary results and cleanup — performance/todo.md § PERF-7
- [ ] PERF-8 Where each workload stops fitting in memory — performance/todo.md § PERF-8

## 12. Waiting

- [ ] OPS-1 Peak process memory on Windows (needs a Windows host) — operations/todo.md § OPS-1
- [ ] H-06 The two-level objective (needs a rule that takes the shape) — direct_solve/framework/todo.md § H-06
- [ ] H-08 VALUE-shape clauses (owner names the rule and shape) — direct_solve/framework/todo.md § H-08
- [ ] A2–S4 Start one of the 16 defined classes not yet built (owner picks) — direct_solve/todo.md
