# Testing — open work

Open coverage gaps, one section each. Coverage that exists is in [coverage.md](coverage.md) and
[scope_suite.md](scope_suite.md).

## No differential test is large enough to see a solver's optimality gap

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

## `group_indices` drops NULL keys

`_oracle_helpers.group_indices` drops every row whose key function returns `None`, and
its docstring says that matches DECIDE's grouping. It does not: a NULL key is its own
class in `per` and its own group in `by` (D5). Nine feature files build their oracles
with it (`test_aggregate_local_when.py`, `test_avg.py`, `test_cons_correlated_subquery.py`,
`test_edge_cases.py`, `test_min_max_multiterm.py`, `test_per_clause.py`,
`test_per_multi_column.py`, `test_per_objective.py`, `test_per_qualified.py`). They pass
because their grouped keys are never NULL, so the helper is a trap for the next test
that groups on a nullable column.

- Work: the NULL-key ruling (`../pipeline/08_execution/todo.md` §2) makes dropping a NULL key DECIDE's rule, so
  this helper becomes right and `_scope_oracle.class_ids`, which keeps NULL as a class, becomes the one that disagrees.
  Do this item in that change: make both helpers drop a NULL key and fix the docstrings.
- Test: a feature test that groups on a NULL key, judged by both helpers.
- Update: [scope_suite.md](scope_suite.md) → "The reference pieces".

## Names that still say the removed spellings

These test files and markers carry the removed vocabulary: `test_entity_scope.py` and its
`test_entity_scoped_*` tests, `test_scalar_scope.py`, `test_aggregate_local_when.py`, and
the `per_clause` and `when*` markers with their descriptions in `pytest.ini`. The code
identifiers they echo are `DecideVarScope::ENTITY`, `EntityScopeInfo` and the DIAGNOSE
scope label `entity`. **Ruling (owner, 2026-10-27).** Rename the tests in the same change
as those identifiers; renaming them together keeps one
vocabulary.

- Update: the area table in [README.md](README.md) and every section of [coverage.md](coverage.md) that names these files.

## `MAX(x) <= MAX(v)` has no test

A hard `MIN`/`MAX` with a reducer bound is untested.

- Test: `MAX(x) <= MAX(v)` on a dataset where the hard encoding binds, judged by the oracle.
- Update: [coverage.md](coverage.md) → MIN and MAX.

## DIAGNOSE of a hard `MIN`/`MAX` over a keyed decision has no test

- Test: an infeasible query with a hard `MIN`/`MAX` over a keyed decision (`DECIDE per K: x(TYPE)`), run under
  `DIAGNOSE`; each suggested edit pasted back runs.
- Update: [coverage.md](coverage.md) → MIN and MAX.
