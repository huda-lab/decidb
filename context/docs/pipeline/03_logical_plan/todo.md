# Stage 03 — Logical plan: open work

Open work in what `EXPLAIN` prints. Finished work is in [done.md](done.md) § 6. How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## PLAN-4 — EXPLAIN's rewrite rows print text the query did not write

- **Problem.** Seen on 052b85d26d with `SET decide_direct_solve = 'off'`. Over `s(id, region, v)` = (1, 'n', 2), (2,
  'n', 3), (3, 's', 1), `EXPLAIN SELECT id, ship FROM s DECIDE ship(REAL) SUCH THAT ship <= 5 AND per region:
  sum(abs(ship - v)) by (region) <= 5 MAXIMIZE sum(ship)` prints the rewrite rows `↳ when true: abs(ship - v) - ship >=
  -v` and `↳ when true: abs(ship - v) + ship >= 0 + v`: a `when true:` the query did not write, no `per region`, and `0
  + v`. The review also reports an L0 row printing `M * (x <> 0)` and `0.0001 * (x <> 0)`; not re-run here. The rows
  come from 1e5bb16177 and ff4726945b.
- **Cause.** `RenderFunctionRow` in `src/planner/decide/decide_source_provenance.cpp` prints the row's raw scope and
  bound. `test_explain.py::test_abs_clause_shows_its_source_and_account` asserts counts `>= 4` and `>= 3`, so it cannot
  see this.
- **Change.** Each rewrite row prints its scope as the clause wrote it and its bound without the rewrite's zero terms. A
  constant the query did not write prints by the name the user writes or sets it with.
- **Ruling (owner, 2026-11-17).** L0's link rows print `M` where M comes from the bounds, the number as written where
  the query writes `norm(e, 0, M)`, and `decide_l0_tolerance` for the tolerance: `M * (x <> 0) - x >= 0`,
  `abs(x) - decide_l0_tolerance * (x <> 0) >= 0`.
- **Done when.** `test_abs_clause_shows_its_source_and_account` has exact expectations, and a test of the `per region`
  shape shows `per region` and no `when true:`; `./test/decide/run_tests.sh -n 2 -k explain` passes.
