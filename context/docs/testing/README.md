# Testing

What the DECIDE test suite covers and how it judges an answer. Tests live in `test/decide/` (Python, differential
against independent solvers) and `test/common/` (C++).

| File | Holds |
|---|---|
| [oracle.md](oracle.md) | How the suite works: the gurobipy oracle, comparison, fixtures, cache, markers, adding a test, and the golden model dumps |
| [scope_suite.md](scope_suite.md) | The scoping language's suite (`per`, `when`, `by`): the scope oracle, the 784-cell grid, deck examples 1–8, the removed-spelling scanner |
| [coverage.md](coverage.md) | What each feature area's tests cover, one section per area |
| [todo.md](todo.md) | Open coverage gaps |

Coverage of direct solve is in each rule's `done.md` under [../direct_solve/rules/](../direct_solve/rules/).

## Feature areas

| Section of coverage.md | Feature | Key test files |
|---|---|---|
| Clause order | The two `DECIDE` clause positions (split / single-block) | `test_clause_order.py` |
| Variable types | BOOL / INT / REAL, multi-variable | `test_var_*.py` |
| Keyed decisions | Keyed decisions (`DECIDE per K: x(TYPE)`) over joins | `test_entity_scope.py` |
| Query-wide decisions | Query-wide decisions (`DECIDE per (): x(TYPE)`) | `test_scalar_scope.py`, `test_scalar_var_in_aggregate.py` |
| Relation-qualified reducers | The colon form `SUM(D: expr)`, which means `sum(per D: expr)` | `test_qualified_reducer.py` |
| Constraint operators | `=`, `<`, `<=`, `>`, `>=`, `<>`, BETWEEN, IN; per-row vs aggregate | `test_cons_*.py` |
| `when` | `when` in front of a constraint or the objective, and a reducer's own `when` | `test_when_*.py`, `test_aggregate_local_when.py` |
| `per` constraints and two-level objectives | `per K:` constraints and `outer(per k: inner(e) by (k))` | `test_per_*.py` |
| MIN and MAX | MIN/MAX linearization (easy/hard cases, composed, two-level) | `test_min_max.py`, `test_per_objective.py` |
| AVG | AVG (execution-time coefficient scaling) | `test_avg.py` |
| ABS | ABS linearization | `test_abs_linearization.py` |
| Bilinear terms | `x * y` (McCormick for Bool × anything, Q-matrix otherwise) | `test_bilinear.py` |
| Quadratic programs | QP objectives, QCQP constraints | `test_quadratic.py`, `test_quadratic_constraints.py` |
| Subqueries | Scalar and correlated subqueries as a constraint's bound | `test_cons_subquery.py`, `test_cons_correlated_subquery.py` |
| Errors | Parser errors, binder errors, infeasibility, solver-specific errors | `test_error_*.py` |
| Edge cases and data shapes | Boundary conditions, data shapes, scale, JOIN sources, EXPLAIN plan, objective shapes | `test_edge_cases.py`, `test_large_scale.py`, `test_sql_joins.py`, `test_explain.py`, `test_obj_minimize.py`, `test_obj_complex_coeffs.py` |

## Coverage quality levels

- **oracle** (✓): DeciDB's output is compared with an independently formulated gurobipy model via `compare_solutions`,
  on both the objective and the decision vector (`identical`, or `optimal` for an alternate optimum at the same
  objective). It catches silent optimality bugs: wrong coefficients, wrong constraint direction, rewrite errors, Big-M
  encoding bugs.
- **scope oracle** (✓): the scope suites state rows, decisions and clauses to `_scope_oracle.ScopedModel` in plain
  Python; `judge_scoped` checks that the answer gives every class one value, satisfies every generated clause and
  matches the oracle's optimum. For shapes the scope oracle cannot state, `_brute_force` enumerates every assignment.
  See [scope_suite.md](scope_suite.md).
- **constraint only**: checks that constraints hold and the result is non-empty, not optimality. Acceptable only for a
  pure feasibility query (no objective); a new correctness test uses an oracle.
- **xfail**: a known defect or an unbuilt feature, kept so the fix is detected.
- **error test**: an invalid query is rejected with the right message.

A hand-computed expected answer (`expected = {1: 10.0, 2: 20.0}`) is not an oracle: it checks only the instance its
author thought through, and an encoding bug that matches it passes. Every correctness test states the same problem
independently, in gurobipy via `oracle_solver` and `compare_solutions`, or to `_scope_oracle` / `_brute_force`. For a
non-linear objective, pass `decidb_objective_fn` to `compare_solutions`. The pattern is in [oracle.md](oracle.md) §7;
shared primitives are in `test/decide/tests/_oracle_helpers.py`.

## Risk priorities for gaps

- **High:** an optimizer rewrite interacting with another feature (`per` with bilinear, Big-M with `per`, a Q matrix
  with `per`); auxiliary-variable indexing across linearizations; a boundary condition that could crash or hang.
- **Medium:** a documented feature with no standalone test; an untested but plausible pair of features; an error path
  with no explicit test.
- **Low:** unlikely edge cases; stress tests of structural limits; error-message wording.

## Keeping this folder current

When a change touches a tested area, read its section of `coverage.md` and the gaps in `todo.md` first. After adding
tests, update the section; a closed gap leaves `todo.md` in the same change.
