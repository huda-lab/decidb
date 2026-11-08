# Testing — open work

Open coverage gaps, one section each. Coverage that exists is in [coverage.md](coverage.md) and
[scope_suite.md](scope_suite.md).

## BLOAT-22 — Test helpers copied with the same body

- **Problem.** Helpers with one body are defined in several test files: `_judged` and the `SHIP*` constants in the
  scope files (`test_scope_*`, `test_collated_keys`, `test_null_keys`, `test_per_class_null_key_bound`), and
  `_const(value)` in `test_empty_groups`, `test_reducer_filter`, `test_scope_bare_clauses` and
  `test_scope_value_rewrite`. `_values` (9 files), `_by_group` (3), `_solve_oracle` (2), `_expect_gurobi` (2) and
  `_solve_and_compare` (2) share a name but not a body, so they are not copies.
- **Change.** Move each helper whose bodies are byte-identical into `_scope_cases.py` (scope helpers) or the shared
  module its callers already import, and import it at each caller. Leave the helpers whose bodies differ.
- **Done when.** Each moved helper has one definition, the focused runs over its callers pass, and no test id changes.

## BLOAT-23 — Scope combination cells that share one branch

- **Problem.** `test_scope_combinations` runs 784 cells, a cross product of scope, clause and shape. Reading the code
  does not show which cells reach a branch no other cell reaches.
- **Change.** From a coverage run, delete each cell that reaches no arm another cell reaches (`.claude/CLAUDE.md`
  § Testing).
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
