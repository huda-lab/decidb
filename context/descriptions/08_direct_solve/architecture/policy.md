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

Decided 2026-10-03. On 5M-row sources direct was about 15 to 100 times faster than Gurobi on the shapes S1 covers, used far less
memory, and was neutral on small inputs. The numbers live in `rules/s1/done.md`. A bug in a proof would give a wrong
answer, not a slow one, so the guard is the test suite: run it with direct on and with
`DECIDB_TEST_DIRECT_SOLVE=off`, which turns every query back into a solver run.

## EXPLAIN and profiling

A hit carries one **decision record**: mode, rule, proof summary, guards. It shows in logical and physical `EXPLAIN` and
in profiling. A miss under `auto` prints nothing, so `EXPLAIN` of an ordinary DECIDE query is exactly what the solver
path prints. To learn why a query missed, set `require`.

## What a user can notice

- **Ties.** A tied query can return a different, equally optimal assignment than the solver. Tests compare row count
  and primary objective, never the tied vector.
- **Error wording and error choice.** Decided 2026-10-05: direct solve owes an error on the same inputs, not the
  solver's text or the solver's pick among several errors (see `rules.md`). S1 keeps the solver's fixed messages (empty
  aggregate, non-finite score, infeasible) and words the rest plainly: a NULL score quotes the score expression, and a
  bad bound names its column. Direct omits the solver's "at row N" on purpose: N is a position in scan order, not a
  key. New rules do not copy wording.
- **Numeric edge cases.** Scores are compared to zero in the finite DOUBLE domain with no epsilon. Both solvers
  disagreed with each other near 1e-9, so the oracle and exact enumeration are the reference, and backend comparisons
  allow a measured gap.

## Test controls

`DECIDB_TEST_DIRECT_SOLVE=off` runs the whole suite on the solver path. Tests that assert a hit pin `require`; the
solver side of a differential test pins `off`; benchmarks pin one or the other so a measurement never silently uses the
other path.
