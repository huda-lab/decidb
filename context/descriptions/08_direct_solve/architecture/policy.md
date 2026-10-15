# Policy: modes, defaults, and what a user can notice

## The setting

`decide_direct_solve` is validated to `auto`, `off`, or `require`. The mode is resolved when the plan is built; a
prepared plan keeps its choice until it is rebound.

| Mode | Hit | Miss |
|---|---|---|
| `auto` (default) | direct plan | solver, silently |
| `off` | solver | solver |
| `require` | direct plan | error naming the clause, solver never runs |

`DIAGNOSE` always uses the solver (`require` + `DIAGNOSE` is an error). The test-only `DECIDB_FORCE_SOLVER` does the same.

## Why `auto` is the default

On 5M-row sources direct is about 15 to 100 times faster than Gurobi on the shapes S1 covers, uses far less memory, and
is neutral on small inputs. The numbers live in `rules/s1_uniform_matroid/done.md`. A bug in a proof would give a wrong
answer, not a slow one, so the guard is the test suite: run it with direct on and with `DECIDB_TEST_DIRECT_SOLVE=off`,
which turns every query back into a solver run.

## EXPLAIN and profiling

When a rule applies, the plan carries one **`EXPLAIN` annotation** (`DirectExplainAnnotation`): mode, rule, and one
fixed `condition` line per rule that names the problem class (S1: `min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi`;
A1: `min/max SUM(c*x), l <= x <= u, each row independent`). It holds no number from the query and no list of runtime
checks. It shows in logical and physical `EXPLAIN` and in profiling. When no rule applies under `auto`, nothing is
printed, so `EXPLAIN` of an ordinary DECIDE query is exactly what the solver path prints. To learn why no rule applied,
set `require`.

## What a user can notice

- **Ties.** A tied query can return a different, equally optimal assignment than the solver. Tests compare row count
  and primary objective, never the tied vector.
- **Strict bounds against a fraction.** A bare `x > 0.5` or `x < 2.5` on an `INT` or `BOOL` (no `WHEN`) gives `x >= 1`
  and `x <= 2` on the solver, in A1 and in S1, which take the shift from one function (`DecideStrictBound`), so `auto` and
  `off` agree on this shape.
- **Error wording and error choice.** Direct solve owes an error on the same inputs, and the solver's text for it (see
  `rules.md`), not the solver's pick among several errors. The NULL and NaN messages come from the functions the solver
  calls (`decide_value_messages.hpp`), so a NULL score, target, coefficient or bound names the NULL columns on the
  failing row as the solver does. Direct omits the solver's "at row N" on purpose: N is a position in scan order, not a
  key. Two messages stay direct solve's own: an S1 bound or fixed value that is a computed expression rather than one
  column (the plan counts bad rows over the whole input and has no failing row to name columns from), and an `=` bound
  that varies within a group (the solver names the first two values it meets in scan order).
- **Numeric edge cases.** Scores are compared to zero in the finite DOUBLE domain with no epsilon. Both solvers
  disagreed with each other near 1e-9, so the oracle and exact enumeration are the reference, and backend comparisons
  allow a measured gap.

## Test controls

`DECIDB_TEST_DIRECT_SOLVE=off` runs the whole suite on the solver path. Tests that assert a rule applies pin `require`;
the solver side of a differential test pins `off`; benchmarks pin one or the other so a measurement never silently uses
the other path.
