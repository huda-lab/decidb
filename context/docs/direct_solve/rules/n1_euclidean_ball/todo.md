# N1 (Euclidean ball) — open work

N1 keeps one sum of squares inside a limit by scaling the direction or the target onto the ball. The class is in
[definition.md](definition.md); what N1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task.

N1's class includes a factor that differs between rows, `SUM(POWER(SQRT(w) * x, 2)) <= B`, the catalogue's ellipsoid
support function: its plan has N1's operator tree (owner, 2026-11-13).

## N1-12 — A constant factor costs per-row work

- **Problem.** The plan carries the factor `q` as a column on every row (`ComputeCoefficients`), so a constant `q`
  (every query with no weight) still computes the slope, `q`, its check, `q > 0`, `p = d / q` and `(q * p) * p` per row
  in both reads. `n1` at SF 1 takes 44.7 ms CPU against 35.3 ms with `q` in the radius (`done.md` § Performance). The
  plan's expressions are built in the DECIDE optimizer, after DuckDB's `ExpressionRewriter` (constant folding, CASE
  simplification) has run, and `ExpressionBuilder` folds nothing.
- **Decision needed.** Where constants fold, with the plan kept in one form. (a) `ExpressionBuilder` folds an operation
  whose operands are all constants, as LLVM's IRBuilder does, and the plan takes a constant slope as a literal leaf
  instead of a column. (b) The rule runs DuckDB's `ExpressionRewriter` over the projections it builds, with the
  constant slope as a literal leaf. Either way the leaf is the one choice: a constant is not a row value.
- **Done when.** `n1` and `n1_groups` measure within noise of the plan with `q` in the radius
  (`run.py --compare --sf 1 --queries n1 n1_groups`); `./test/decide/run_tests.sh -n 2 -k "direct_n1"`.
