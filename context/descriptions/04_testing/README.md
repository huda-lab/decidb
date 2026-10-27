# Test Coverage Documentation

Tracks the state of the DECIDE test suite — what is covered, what gaps remain,
and which tests verify correctness (oracle comparison) vs. feasibility only.

For **how** the testing framework works (oracle solver, fixtures, comparison
logic), see [../02_operations/oracle.md](../02_operations/oracle.md). This
folder answers **what** is being tested and what isn't.

## Structure

Each subdirectory corresponds to a feature area and contains:
- `done.md` — covered scenarios, with oracle vs. constraint-only status
- `todo.md` — remaining gaps and upgrade candidates

## Areas

| Directory | Feature | Key test files |
|-----------|---------|----------------|
| `clause_order/` | The two `DECIDE` clause positions (split / single-block) | `test_clause_order.py` |
| `variables/` | BOOL / INT / REAL, multi-variable | `test_var_*.py` |
| `scope/` | The scoping language (`per`, `when`, `by`): the scope oracle, declarations, generation, `by`, a reducer's own scope, the objective, determination and empty sets, the value rewrite, bare clauses, the 784-cell grid, deck examples 1–8, the EXPLAIN/DIAGNOSE echo, the removed spellings and the scanner that keeps them out | `test_scope_*.py`, `_scope_oracle.py`, `test_no_old_spellings_remain.py` |
| `entity_scope/` | Keyed decisions (`DECIDE per K: x(TYPE)`) over joins | `test_entity_scope.py` |
| `scalar_scope/` | Query-wide decisions (`DECIDE per (): x(TYPE)`) | `test_scalar_scope.py`, `test_scalar_var_in_aggregate.py` |
| `qualified_reducer/` | The colon form `SUM(D: expr)`, which means `sum(per D: expr)` | `test_qualified_reducer.py` |
| `constraints/` | Comparison operators (`=`, `<`, `<=`, `>`, `>=`, `<>`, BETWEEN, IN), per-row vs aggregate | `test_cons_*.py` |
| `when/` | `when` in front of a constraint or the objective, and a reducer's own `when` | `test_when_*.py`, `test_aggregate_local_when.py` |
| `per/` | `per K:` constraints and the two-level objective `outer(per k: inner(e) by (k))` | `test_per_*.py` |
| `min_max/` | MIN/MAX linearization (easy/hard cases, composed, two-level) | `test_min_max.py`, `test_per_objective.py` |
| `avg/` | AVG aggregate (execution-time coefficient scaling) | `test_avg.py` |
| `abs/` | ABS linearization | `test_abs_linearization.py` |
| `bilinear/` | Bilinear terms `x * y` (McCormick for Bool × anything, Q-matrix otherwise) | `test_bilinear.py` |
| `quadratic/` | QP objectives, QCQP constraints | `test_quadratic.py`, `test_quadratic_constraints.py` |
| `subquery/` | Scalar and correlated subqueries as constraint RHS | `test_cons_subquery.py`, `test_cons_correlated_subquery.py` |
| `error_handling/` | Parser errors, binder errors, infeasibility, solver-specific errors | `test_error_*.py` |
| `golden/` | The model-dump characterization oracle: what DeciDB *builds*, per backend | `golden/corpus.sql`, `golden/check.sh` |
| `edge_cases/` | Boundary conditions, data shapes, scale, JOIN sources, EXPLAIN plan, objective-shape tests | `test_edge_cases.py`, `benchmark/decide/test_large_scale.py` (not collected, does not run), `test_sql_joins.py`, `test_explain.py`, `test_obj_minimize.py`, `test_obj_complex_coeffs.py` |

## How to use

When implementing a new feature or fixing a bug in a tested area:
1. Check the area's `done.md` to understand existing coverage.
2. Check the area's `todo.md` for known gaps before writing new tests.
3. After adding/changing tests, update `done.md` to reflect new state.
4. Move items from `todo.md` to `done.md` as they are completed.

When planning test work from scratch, start with the HIGH-risk gaps across all
`todo.md` files — these are where silent correctness bugs are most likely.

## Coverage quality levels

- **oracle** (✓) — DeciDB output is compared against an independently-formulated gurobipy ILP via `compare_solutions`. The comparison checks both the objective value and the decision-variable vector, returning `identical` (same vector) or `optimal` (alternate optimum at the same objective). Catches silent optimality bugs — wrong coefficients, wrong constraint direction, rewrite errors, Big-M encoding bugs.
- **scope oracle** (✓) — the scope suites state the rows, decisions and clauses to `_scope_oracle.ScopedModel` as plain Python; `judge_scoped` checks that DeciDB's answer gives every class one value and satisfies every generated clause, and its objective must equal the oracle's optimum. For shapes the scope oracle cannot state, `_brute_force` enumerates every assignment. See [scope/done.md](scope/done.md).
- **constraint only** — Legacy tier. Test checks that constraints are satisfied and the result is non-empty, but does not verify optimality. Can miss bugs where the solver produces a valid but suboptimal solution due to incorrect formulation. Acceptable only for pure feasibility queries (no objective); new correctness tests must use **oracle**.
- **xfail** — Known defect or feature not yet implemented. Test is kept to auto-detect when fixed.
- **error test** — Verifies that an invalid query is rejected with the right error message.

### Forbidden: analytical / hand-computed closed-form assertions

Asserting hand-computed expected variable assignments (e.g. `expected = {1: 10.0, 2: 20.0, 3: 30.0}`) is **not a valid oracle**. Analytical checks only verify the instance the author thought through; an encoding bug that coincides with the expected answer passes silently. Every correctness test must formulate the same problem independently — in gurobipy via `oracle_solver` and compared via `compare_solutions`, or stated to `_scope_oracle` / `_brute_force` and judged there. For non-linear objectives (QP/QCQP), pass `decidb_objective_fn` to `compare_solutions` to evaluate the objective on DeciDB's variable values. See `02_operations/oracle.md` for the canonical pattern and `tests/_oracle_helpers.py` for shared primitives.

## Risk priorities for gaps

HIGH-risk gaps involve:
- Optimizer rewrites interacting with another feature (`per`+bilinear, Big-M+`per`, Q matrix+`per`)
- Linearization auxiliary variable indexing across features
- Boundary conditions that could crash or hang

MEDIUM-risk gaps involve:
- Documented features used standalone without tests
- Untested but plausible feature pairs
- Error paths without explicit coverage

LOW-risk gaps involve:
- Edge cases unlikely in practice
- Stress tests of structural limits
- Error-message quality / regex tightening
