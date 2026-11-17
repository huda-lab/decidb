# Stage 05 — Optimizer: open work

Open work in the optimizer passes. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## OPT-11 — `per g: x IN (...)` constrains the row whose `g` is NULL

- **Problem.** Seen on 052b85d26d with `SET decide_direct_solve = 'off'`. Over `t(id, g, v)` = (1, 'a', 3.0), (2, NULL,
  5.0), (3, 'b', 6.0), with `x(INT)`, `x <= 10 AND x >= 0 AND per g: x IN (1, 2)` and `MAXIMIZE SUM(v*x)`, the answer is
  2, 2, 2. It should be 2, 10, 2: a NULL key is in no class (guide § 6.2). Under the same `per`, `x BETWEEN 1 AND 2` and
  `abs(x - 1) <= 1` answer 2, 10, 2, and `x <> 10` answers 9, 10, 9.
- **Cause.** `RewriteInDomain` in `src/optimizer/decide/decide_rewrite_norm_in.cpp` emits its rows with the clause's
  `when` only and drops the `per`'s class. Both parent branches keep it; 4f4cb0acfc gives the sibling spellings their
  class.
- **Change.** The IN's rows keep the clause's `per`.
- **Done when.** An oracle test of `per g: x IN (...)` with a NULL-key row passes; `./test/decide/run_tests.sh -n 2 -k
  "<the new test> or in_domain or null_values"` passes.

## OPT-12 — A value check over a point frame never raises

- **Problem.** Seen on 03afab0d29 with `SET decide_direct_solve = 'off'`. Over `f(id, v)` = (1, 1.0), (2, 2.0), (3,
  'inf'), (4, 1.0), read as `t`, with `x(REAL)`, each of these answers "DECIDE optimization is infeasible. Prefix the
  query with DIAGNOSE to see which clause to change.":
  - `x BETWEEN 0 AND 10` and `MINIMIZE SUM(ABS(x - at(previous else 0: t.v) over (t.id)))`, and the same without
    `else 0`;
  - `x BETWEEN 0 AND 10 AND abs(x - at(previous else 0: t.v) over (t.id)) <= 100` and `MINIMIZE SUM(x)`;
  - `x <= 10 AND x >= at(previous else 0: t.v) over (t.id) - 100` and `MINIMIZE SUM(x)`.

  Each should name the frame, as these do: "DECIDE: at(previous else 0: t.v) over (t.id) is NaN or infinite on some
  rows. Skip those rows with 'when isfinite(at(previous else 0: t.v) over (t.id)):'." The same frame as a coefficient
  (`MINIMIZE SUM(at(previous else 0: t.v) over (t.id) * x)`) is named so, and a range frame
  (`sum(from previous to next: t.v) over (t.id)`) under ABS is named the same way.
- **Cause (narrowed, not proven).** The checks exist: `AddAbsValueCheck` (`decide_rewrite_abs.cpp`) collects the
  frame, which holds no decision and so splits as a constant term, and `decide_rewrite_rhs_bounds.cpp` makes the bound
  side's. Both are `VALUE_IS_VALID` scope checks, kept by `DirectRetainRowChecks` as `retained_row_checks`, which
  `decide_tree.cpp` lowers as hidden "data value check" nodes. A point-frame marker read inside such a node does not
  raise on row 4, whose frame value is infinite; the coefficient path and a range frame do.
- **Change.** A data value check reads a point frame's value at the row the frame selects, as a coefficient does.
- **Done when.** A test shows the four queries name the frame, and an oracle test answers the ABS query under
  `when isfinite(...)`; `./test/decide/run_tests.sh -n 2 -k "<the new tests> or invalid_value or abs or frame"`
  passes.
## OPT-13 — A `per` over a written column never folds into the box

- **Problem.** `decide_singleton_row_folding.cpp` (4f4cb0acfc) stops the box fold for any `per` key that holds a written
  column (null mode `NO_GROUP`), whether or not a key is NULL, so every `per t.g: x <= c` is a model row. The answers
  are the same; the cost is model size and speed. The commit's subject names the NULL key only.
- **Decision needed.** Keep it and say in done.md that a written `per` key always stays a row, or fold where no key is
  NULL.
- **Done when.** The owner's ruling is recorded and done.md states it; for folding, oracle tests with and without a
  NULL-key row pass, and `./test/decide/run_tests.sh -n 2 -k "singleton_row or message_qualifier"` passes.

## OPT-14 — Three helpers ask whether an expression holds a reducer

- **Problem.** Three functions answer "does this expression hold a reducer?", each with its own exceptions:

  | Function | A frame's marker | A definition body |
  |---|---|---|
  | `DecideCanonicalizer::ContainsReducer` (`decide_canonicalizer.cpp`; `TryMatchScaledAggregate`'s scale) | no | no |
  | `ContainsReducer` (`decide_rewrite_bilinear.cpp`, static; a product's sides) | yes | no |
  | `BoundExpressionContainsAggregate` (`decide_linear_form.cpp`; `ExtractAggregateObjectiveTerms`) | no | yes |

  The bilinear one counts a frame's marker because the side test it replaced (`IsReducer`: the side is an aggregate)
  did. `x * (at(previous else 0: x) over (t.id) + 1) <= 5` and three other frame products answer the same whether
  or not the marker counts.
- **Change.** One function, in the canonicalizer's header, used by all three. **Decision needed:** whether a frame's
  marker over decisions counts as a reducer for the bilinear rewrite; the answer decides whether the bilinear rewrite
  or the value rewrite takes `x * at(previous: x)`.
- **Done when.** One definition remains; `./test/decide/run_tests.sh -n 2 -k "bilinear or quadratic or frame or
  scope_objective or canonical"` passes.

## OPT-15 — A data reducer under ABS in a constraint raises an internal error

- **Problem.** Seen on 03afab0d29, in the default mode and with `SET decide_direct_solve = 'off'`. Over `t(id, g)` =
  (1, 'a'), (2, 'a'), (3, 'b'), with `x(INT)`, `x <= 3 AND abs(x - max(t.id) by (t.g)) <= 2` and `MAXIMIZE sum(x)`
  gives "INTERNAL Error: DECIDE constraint in the VALUE shape reached term extraction: the value rewrite did not
  replace its reducers in '(abs((x - CAST(max(id) AS BIGINT))) <= CAST(2 AS BIGINT))'", from `ReadDecideClauseFacts`
  at physical planning. `abs(x - max(t.id)) <= 2` raises the same. Without `abs`, `x - max(t.id) by (t.g) <= 2`
  answers 3, 3, 3. Found by phase 5's lane B.
- **Cause (not proven).** A function that reads a reducer makes the clause the VALUE shape, and the value rewrite holds
  only reducers over decisions in helpers, so a reducer over data reaches term extraction as a raw aggregate.
- **Change.** A reducer over data under ABS is read as data, as it is without `abs`.
- **Done when.** An oracle test of both queries passes; `./test/decide/run_tests.sh -n 2 -k "<the new test> or abs or
  scope_generation"` passes.
