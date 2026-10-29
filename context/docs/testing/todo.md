# Testing — open work

Open coverage gaps, one section each. Coverage that exists is in [coverage.md](coverage.md) and
[scope_suite.md](scope_suite.md).

## TST-1 — No differential test is large enough to see a solver's optimality gap

- **Gap:** a MIP solver with a nonzero relative gap may stop at an incumbent short of the optimum. The differential
  suite cannot see this. Its datasets are small enough that Gurobi and HiGHS always close the gap, and
  `compare_objectives` (`test/decide/comparison/compare.py`) accepts an absolute difference of 1e-4, which a relative
  gap of 1e-4 on a large objective exceeds by orders of magnitude while staying inside it on a small one.
- **Found by:** the direct-solve benchmark on S1 at 1.6M rows, where Gurobi at its default `MIPGap` of 1e-4 returned
  objectives 5.5e-6 to 4.6e-5 relative short of direct solve's (`../performance/benchmarking/direct_solve.md`). Both
  backends now solve to a gap of 0 (`../pipeline/07_solver/done.md`, Optimality gap), but no test fails if that is
  undone.
- **Test to add:** one MIP whose objective is large and whose default-gap incumbent is short of the optimum, compared
  with a relative tolerance, so that removing the gap-0 setting from either backend fails it.

## TST-2 — Names that still say the removed spellings

These test files and markers carry the removed vocabulary: `test_entity_scope.py` and its
`test_entity_scoped_*` tests, `test_scalar_scope.py`, `test_aggregate_local_when.py`, and
the `per_clause` and `when*` markers with their descriptions in `pytest.ini`. The code
identifiers they echo are `DecideVarScope::ENTITY`, `EntityScopeInfo` and the DIAGNOSE
scope label `entity`. **Ruling (owner, 2026-10-27).** Rename the tests in the same change
as those identifiers; renaming them together keeps one
vocabulary.
The identifiers and the names still open are `../issues/code_quality.md` § CQ-3; this task lands in the same change.

- Update: the area table in [README.md](README.md) and every section of [coverage.md](coverage.md) that names these files.

**Ruling (owner's delegate, 2026-10-29).** `test_keyed_scope.py` (`test_keyed_*`), `test_global_scope.py`,
`test_reducer_filter.py`; markers keep `when*` and `per_clause`, `when_perrow` becomes `when_row`, and descriptions use
the guide's terms.
