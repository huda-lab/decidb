# Testing — open work

Open coverage gaps, one section each. Coverage that exists is in [coverage.md](coverage.md) and
[scope_suite.md](scope_suite.md).

## BLOAT-23 — Scope combination cells that share one branch

- **Problem.** `test_scope_combinations` runs 784 cells, a cross product of scope, clause and shape. Reading the code
  does not show which cells reach a branch no other cell reaches.
- **Coverage (`73b52ab1d1`, § BLOAT-25's run).** Removing one cell at a time while every arm stays reached by some
  test removes 783 of the 784 cells; but no two cells reach the same set of arms, so every cell takes a path of its
  own through the binder, canonicalizer, optimizer and formulation.
- **Decision needed.** Whether a cross-product grid of interactions is trimmed by the arm rule (783 go) or kept
  because each cell is a distinct path (none go). The grid exists to find bugs that only a combination shows, which no
  single arm records. Recommendation: keep it, and close this task.
- **Change.** As decided.
- **Done when.** `coverage_report.py compare` shows no arm lost, and `make decide-test` passes.

## TEST-1 — Expectations computed by hand

- **Problem.** Some tests compare DeciDB's answer with a number worked out by hand, not with the independent oracle:
  `test_canonicalize_scale`, `test_cons_perrow` and `test_abs_linearization`'s
  `test_abs_maximize_bound_inferred_from_sum`. A hand value shares the author's reading of the model, so a wrong
  formulation and a wrong expectation can agree. `test_explain`'s `test_explain_json_structure` opens with an assert
  that always passes (`... or "DECIDE" in out`).
- **Change.** Compare each with `oracle_solver` on the same data (`oracle.md`); make the `EXPLAIN` assert check the
  JSON operator name only.
- **Done when.** No test in the list asserts a hand-computed optimum, and the focused runs pass on Gurobi and HiGHS.

## BLOAT-25 — Arms no test reaches in the rest of the DECIDE pipeline

- **Problem.** A coverage run over the whole DECIDE pipeline (`COVERAGE_SOURCES="planner/decide/*
  planner/expression_binder/decide/* planner/operator/decide/* optimizer/decide/* optimizer/decide/direct/*
  decidb/formulation/* execution/operator/decide/*" make decide-coverage`) at `73b52ab1d1` finds 1,093 of 10,624 arms
  that no test reaches; 53 are direct solve (`../direct_solve/framework/todo.md` § TEST-2). The rest, by area: binder
  202 of 1,324, canonicalizer and provenance (`planner/decide`) 298 of 1,928, the logical operator 15 of 176, the
  optimizer passes 238 of the 1,968 outside `direct/` (three more are in `decide_rewrite_rhs_bounds.cpp`, under TEST-2),
  formulation 141 of 1,436, execution 146 of 1,490. The files with the most are `decide_canonicalizer.cpp` (149),
  `physical_decide.cpp` (146), `decide_binder.cpp` (120) and `decide_source_provenance.cpp` (85).
- **Change.** Per file, for each arm: delete it as code no query reaches (owner ruling 2026-10-21), keep it as a
  stage-handoff or typed-read guard, or add a test that reaches it.
- **Done when.** Each file's unreached arms are deleted, named as kept, or reached, and `make decide-test` passes.
