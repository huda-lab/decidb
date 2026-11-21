# Stage 05 — Optimizer: open work

Open work in the optimizer passes. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## OPT-32 — An ABS over a product of two decisions drops the product

- **Problem.** A wrong answer, and a crash. With `decide_direct_solve='off'` and `x`, `y` INT in `[1, 3]` over two
  rows, `sum(abs(x * y - 4)) >= 7 MINIMIZE sum(x)` returns `x = y = 1` on both rows, where the sum is 6; over `[0, 3]`,
  `MAXIMIZE sum(abs(x * y - 4))` returns 0 on every row, where 10 is the optimum. The ABS rows are linear, and a
  product of two decisions neither of which is BOOL (`RewriteBilinear` takes a BOOL factor) is left out of them. With
  direct solve on, `MAXIMIZE sum(abs(x * y - c))` raises an internal error in `DirectFrontEnd::GuardCoefficients`,
  which reads a NULL coefficient.
- **Change.** The ABS rows keep the product: they are quadratic rows and take the path every quadratic constraint
  takes (Gurobi solves them; the solver gate refuses them elsewhere). If that needs a case of its own, the shape check
  refuses a product of decisions under ABS instead, as it refuses one under any other function. The direct front end
  declines the shape rather than reading a NULL.
- **Ruling (owner, 2026-11-21).** This order: keep the product where it fits the one constraint path, else refuse.
- **Done when.** Both examples agree with the oracle, or are refused with a `Not implemented Error`; the direct path
  answers or declines; `./test/decide/run_tests.sh -k "abs or quadratic"` passes.

## OPT-31 — A square outside a reducer in the objective is refused

- **Problem.** `MINIMIZE power(sum(x) - 3, 2)` (the squared distance of a total from a target) and `MINIMIZE (y -
  3) * (y - 3)` over a query-wide `y` are convex QPs, but the objective's linear form refuses any square outside a
  reducer (`decide_linear_form.cpp`, `'…' multiplies decisions outside a reducer, which is not supported`). The same
  square solves as a constraint (`power(sum(x) - 3, 2) <= 4`), and a product of two query-wide decisions is accepted
  in the objective.
- **Change.** A square of a linear expression outside a reducer becomes a quadratic objective term, as `sum(power(x -
  3, 2))` is: a reducer inside it becomes a helper (the value route), and the solver checks judge convexity and the
  installation as for any quadratic objective.
- **Done when.** Both examples agree with the oracle on Gurobi and HiGHS (continuous); `./test/decide/run_tests.sh -n
  2 -k "quadratic or objective"` passes.

## OPT-28 — Stage 05's passes share one enumerator and one polarity walker

- **Problem.** Stage 05 is 13 ordered passes over one tree and six side containers: 16 loops over side vectors, two
  polarity walkers (`WalkPolarity`, `CollectReducerSites`), two `ReadAffine`.
- **Change.** `LogicalDecide::EnumerateBodies(callback(DecideBody&))`, where a body carries its expression, push, rows,
  source alias and scope, with one fixed case per container, as DuckDB's `LogicalOperatorVisitor::EnumerateExpressions`
  has one per operator; every pass that walks bodies uses it. `CollectReducerSites` folds onto `WalkPolarity`.
  Bilinear's `ReadAffine` reads each factor with `DecideTermSplitter::Split` instead of its own reader.
- **Rulings (owner, 2026-11-21).** A BOOL decision factor counts as non-negative in every reader of a push (it is 0 or
  1); today `CollectReducerSites` counts it non-negative and `WalkPolarity` unknown. The splitter replaces bilinear's
  `ReadAffine`.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "rewrite or polarity or minmax"` passes.

## OPT-29 — No rewrite pass reruns without a cause

- **Problem.** `RewriteBilinear` runs three times; the objective is canonicalized 9 times, 1 at bind and 8 in stage
  05, and 4 of those follow passes that only add tags the canonicalizer does not read (bilinear, ABS).
- **Change.** The 4 reinstalls after tag-only passes go. The second bilinear run goes and `RewriteScopedReducers` hands
  the objective's products to the bilinear rewrite always; the third becomes a worklist hook in `FindAbsAndAllocateAux`
  that tags a product once the ABS helper it reads exists (as LLVM's InstCombine revisits a new value's users).
- **Ruling (owner, 2026-11-21).** Not one post-order traversal: the ABS encoding, value helpers, definitions, MIN/MAX,
  `<>` and IN depend on the whole clause or query, not on one node and its children.
- **Needs.** OPT-28.
- **Done when.** `RewriteBilinear` runs once (`DECIDB_PROFILE=1`); `./test/decide/run_tests.sh -n 2 -k "bilinear or abs
  or rewrite"` passes.
