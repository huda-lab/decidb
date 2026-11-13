# N1 (Euclidean ball) — open work

N1 keeps one sum of squares inside a limit by scaling the direction or the target onto the ball. The class is in
[definition.md](definition.md); what N1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task.

N1's class includes a factor that differs between rows, `SUM(POWER(SQRT(w) * x, 2)) <= B`, the catalogue's ellipsoid
support function: its plan has N1's operator tree (owner, 2026-11-13).

## N1-12 — A constant factor costs per-row work

- **Problem.** The plan carries the factor `q` as a column on every row (`LayoutRawValues`, `ComputeCoefficients`), so a
  constant `q` (every query with no weight) still computes the slope, `q`, its check, `q > 0`, `p = d / q` and
  `(q * p) * p` per row in both reads. `n1` at SF 1 takes 44.7 ms CPU against 35.3 ms with `q` in the radius (`done.md`
  § Performance). The driver runs DuckDB's `ExpressionRewriter` (constant folding, arithmetic and CASE simplification)
  over the rule's plan (`direct_rule_driver.cpp`), but it folds within one expression: a constant laid out as a column is
  a column reference to every projection above it.
- **Ruling (owner, 2026-11-13).** Constants fold in DuckDB's rewriter, which the driver already runs; no folding code of
  the rule's own. The plan stays one form: a slope that folds to a constant is a literal leaf, not a column, and every
  expression over `q` is built the same way from that leaf or from the column.
- **Done when.** `n1` and `n1_groups` measure within noise of the plan with `q` in the radius
  (`python3 benchmark/decide/direct_solve/run.py --compare --sf 1 --queries n1 n1_groups`);
  `./test/decide/run_tests.sh -n 2 -k "direct_n1"`; `build/release/test/unittest "N1 applies to the forms of its class"`.
