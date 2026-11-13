# N1 (Euclidean ball) — open work

N1 keeps one sum of squares inside a limit by scaling the direction or the target onto the ball. The class is in
[definition.md](definition.md); what N1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task.

The tasks below widen N1 to a factor that differs between rows, `SUM(POWER(SQRT(w) * x, 2)) <= B`, the catalogue's
ellipsoid support function. Its plan has N1's operator tree, so it is N1's class (owner, 2026-11-13).

## N1-8 — Recognition: a row-valued slope inside the square

- **Problem.** N1 takes `SUM(q * x^2) <= B` only when `q` folds to one positive constant (`FoldSquareFactor`), so
  `SUM(POWER(SQRT(w) * x, 2)) <= B` and `SUM(POWER(w * x, 2)) <= B` run on the solver. The canonicalizer reads every
  square as a coefficient times a linear form squared (`DirectWeightedForm`), so `q` is the coefficient times the
  inner slope squared.
- **Change.** The limit's square admits a row-valued inner slope when its coefficient folds to a positive constant;
  `q` is then never negative by form, and a negative `w` under `SQRT` raises DuckDB's error on both paths. A
  row-valued coefficient (`SUM(w * POWER(x, 2))`, `SUM(w * x * x)`) declines: a negative `w` makes the limit not
  convex, and the solver answers it. A row-valued slope with a square in the objective declines. The objective's own
  square keeps a constant factor (`SquareIsConvex`). No canonicalizer change: folding the form to one `q` would lose
  the sign proof and the `SQRT` error.
- **Ruling (owner, 2026-11-13).** N1-8 and N1-9 land as one commit: the plan reads q as one constant, so recognition
  alone would apply the rule to forms it cannot compute.
- **Done when.** `build/release/test/unittest "N1 applies to the forms of its class"` with the weighted forms;
  `build/release/test/unittest "N1 declines each shape that is not exactly its class"` with a case for each new
  decline branch; `build/release/test/unittest "Every rule and every other rule never apply to the same query"` with
  the weighted shapes in N1's row.

## N1-9 — Plan: the factor per row in N1's two reads

- **Problem.** N1's plan reads `q` as one constant: the radius is `SQRT(B) / SQRT(q)` and `h` sums the squared
  directions. With `q` per row the row's value is `d / q`, `h = SUM(d^2 / q)` and the radius is `SQRT(B)`.
- **Change.** One form for every factor, constant or not: carry `q` in the raw values on the rows that read it
  (`DirectOnKeptRows`); the row's value is `d / q` and its share of `h` is `d * (d / q)`; keep N1's order of
  operations, `(value / SQRT(h)) * radius`. A row whose `q` is 0 or NULL adds nothing to `h` and takes N1's branch for
  a row with no square in the ball's sum: 0 when its direction is 0, otherwise unbounded, as the solver answers. `d / q`
  past a DOUBLE raises the rule's too-large error. Update `done.md` § What it admits and § How it works.
- **Needs.** N1-8, in the same commit.
- **Done when.** Oracle cases in `test_direct_n1_euclidean_ball.py`, built from the raw rows with each row's `q`: the
  worked example (1 and 1), unequal weights in both domains, MINIMIZE, a weight 0 with direction 0 and with a nonzero
  direction (unbounded), a NULL weight, `B = 0`, `by`, `per K:`, `when`, a NULL key; then
  `./test/decide/run_tests.sh -n 2 -k "direct_n1 or every_input_row_is_read"` and
  `DECIDB_FORCE_SOLVER=highs ./test/decide/run_tests.sh -n 2 -k "direct_n1"`.

## N1-10 — Standard suites and numeric boundaries for the weighted form

- **Problem.** The standard tests reach no row-valued factor, and N1's boundary table measures a plan with the factor
  in the radius, which N1-9 moves into each row's value.
- **Change.** Error table: a negative `w` under `SQRT` and a NaN weight fail with the same message on both paths.
  Boundary table: weights from 1e-150 to 1e150 against the solver alone; rerun the existing boundary rows with a
  constant factor other than 1 (`SUM(2 * POWER(x, 2))`). The conformance fixture gains a weighted applying form and the
  N1-8 declines as near misses; `test_direct_user_facing.py` gains a weighted shape; `test_direct_source_values.py`
  gains a weight on a row a `when` drops. Update `done.md` § Where it differs from the solver and § Verified by.
- **Needs.** N1-9.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct_n1 or direct_rule_conformance or direct_user_facing or
  direct_source_values"`.

## N1-11 — Measure the weighted form against the hand plan and Gurobi

- **Problem.** No benchmark query has a factor per row.
- **Change.** `n1_weighted` and `n1_weighted_groups` over TPC-H `partsupp`, weighted by `ps_availqty` (1 to 9,999,
  never 0), with hand plans in `benchmark/decide/direct_solve/hand/`; their rows join `done.md` § Performance.
- **Needs.** N1-9.
- **Done when.** `python3 benchmark/decide/direct_solve/run.py --compare --sf 1 --queries n1_weighted
  n1_weighted_groups`; `python3 benchmark/decide/direct_solve/run.py --sf 0.125 0.25 0.5 1 --queries n1_weighted
  n1_weighted_groups --modes direct gurobi`.
