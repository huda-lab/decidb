# Testing — open work

Open coverage gaps, one section each. Coverage that exists is in [coverage.md](coverage.md) and
[scope_suite.md](scope_suite.md).

## Arms no test reaches in the DECIDE pipeline (BLOAT-25 to BLOAT-30)

A coverage run over the whole DECIDE pipeline (`COVERAGE_SOURCES="planner/decide/* planner/expression_binder/decide/*
planner/operator/decide/* optimizer/decide/* optimizer/decide/direct/* decidb/formulation/* execution/operator/decide/*"
make decide-coverage`) at `73b52ab1d1` finds 1,093 of 10,624 arms that no test reaches; 53 are direct solve
(`../direct_solve/framework/todo.md` § TEST-2). Each task below is one area. A task lists its arms with
`COVERAGE_SOURCES=<its files> make decide-coverage` on the current tree, as the counts move when code changes.

- **Change, for each arm.** Delete it as code no query reaches (owner ruling 2026-10-21), keep it as a stage-handoff
  or typed-read guard, or add a test that reaches it (oracle-compared where it answers).
- **Done when, for each task.** Every unreached arm in its files is deleted, named as kept in the area's `done.md`, or
  reached, and `make decide-test` passes.

## BLOAT-27 — Canonicalizer and provenance: arms no test reaches

- **Problem.** 298 of 1,928 arms in `planner/decide/` are unreached; `decide_canonicalizer.cpp` has 149 and
  `decide_source_provenance.cpp` 85.
