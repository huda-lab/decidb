# Stage 05 — Optimizer: open work

Open work in the optimizer passes. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## OPT-8 — A square or product whose side holds a reducer and more raises an internal error

- **Problem.** Seen on 052b85d26d with `SET decide_direct_solve = 'off'`. Over `t(id, g, v)` = (1, 1, 1), (2, 1, 2), (3,
  2, 3), (4, 2, 4), with `x(REAL)`, `x <= v` and `MAXIMIZE sum(x)`, each of these constraints raises `INTERNAL Error:
  DECIDE bilinear factor 'sum(x)' is not affine in the decisions` (`'max(x)'` for the last two). `Sami-OPA`
  (`.claude/worktrees/sami-opa/build/release/decidb`) answers each; `round(sum(x), 4)` is:
  - `power(sum(x) - 3, 2) <= 4`: 5.0;
  - `power(sum(x) by (g) - 3, 2) <= 4`: 8.0;
  - `sum(power(max(x) by (g) - 1, 2)) <= 2`: 6.2361;
  - `sum((max(x) by (g) + 1) * x) <= 10`: 4.6056.
  - In the objective, `MINIMIZE 0*sum(x) + power(sum(x) - 5, 2)` raises the same internal error, where `Sami-OPA`
    refuses: "DECIDE objective contains a non-aggregate term: power((sum(x) - CAST(5 AS DOUBLE)), CAST(2 AS DOUBLE))." A
    bare reducer squared, `power(sum(x), 2) <= 4`, answers 2.0 on both. The guide documents these shapes (§ 5.5).
- **Cause.** `FindAndReplaceBilinear` in `src/optimizer/decide/decide_rewrite_bilinear.cpp` reads a side as a product
  factor unless `IsReducer` (a bare reducer), and `ReadAffine` throws on a side it cannot read. The comment "A reducer
  is always the whole side" does not hold for the language, where a side can be `sum(x) - 3`. `Sami-OPA` asks
  `ContainsReducer` there, and its `ReadAffine` returns false. OPT-10 has the same cause: code that assumes a reducer is
  a whole side or a whole term.
- **Change.** A side that contains a reducer is not read as affine here, so it takes the nested-reducer path, as on
  `Sami-OPA`.
- **Ruling (owner, 2026-11-17).** The objective form is refused with the message the language already gives a square or
  product outside a reducer in the objective (`language/errors.md`: "DECIDE objective: '...' multiplies decisions
  outside a reducer, which is not supported yet"), as `power(sum(x), 2)` gets.
- **Done when.** Oracle tests answer the four constraint shapes, and a test shows the objective form's refusal;
  `./test/decide/run_tests.sh -n 2 -k "<the new tests> or bilinear or quadratic"` passes.


## OPT-9 — ABS over a nested sum of group sums in the objective answers wrong or raises

- **Problem.** Seen on 052b85d26d with `SET decide_direct_solve = 'off'`. Over `t(id, k, cap)` = (1, 'a', 3), (2, 'a',
  3), (3, 'b', 2), with `x(INT)`, `x <= cap AND x >= 0`:
  - `MINIMIZE -sum(x) + 3*abs(sum(per k: sum(x) by (k)) - 4)` answers 3, 3, 2 (objective 4). 1, 3, 0 scores -4, so the
    answer is not optimal; the optimum totals 4, objective -4. `Sami-OPA` answers 2, 0, 2. With `sum(x)` inside the
    `abs` it answers 1, 3, 0.
  - `MAXIMIZE 0*sum(x) + abs(sum(per k: sum(x) by (k)) - 2)` raises `INTERNAL Error: DECIDE ABS envelope row for
    auxiliary 1 holds a raw aggregate: the value rewrite did not replace its reducers`, and the database is invalidated:
    the next statement fails with "database has been invalidated". The answer is 3, 3, 2; `Sami-OPA` answers 0, 0, 0,
    also wrong.
- **Cause.** The objective loop at the end of `RewriteScopedReducers` (`src/optimizer/decide/decide_rewrite_scoped.cpp`)
  tries `InlineSumOfGroupSums` on a nest before it reads `site.under_function`, so a nest a function reads is written in
  place and stays a raw aggregate inside the ABS. ac5067a5b4 and ea7f9216c7 assume every reducer a function reads has a
  helper; their C++ test builds its `SolverInput` by hand, so it cannot see this.
- **Change.** Recommended: the inline shortcut does not apply to a reducer a function reads.
- **Done when.** Oracle tests answer both queries; `./test/decide/run_tests.sh -n 2 -k "<the new tests> or scoped or
  abs"` passes.

## OPT-10 — A product of two reducers in the objective raises an internal error

- **Problem.** Seen on 052b85d26d with `SET decide_direct_solve = 'off'`. Over `t(id, cap)` = (1, 3), (2, 4), (3, 1),
  with `x(REAL)` and `x BETWEEN 0 AND cap`, `MAXIMIZE SUM(x) - sum(x) * max(x)` raises `INTERNAL Error: DECIDE term
  'sum(x)' reached term extraction unopened`. `Sami-OPA` refuses: "DECIDE objective contains a non-aggregate term:
  (sum(x) * max(x))."
- **Cause.** `ExtractAggregateObjectiveTerms` in `src/optimizer/decide/decide_linear_form.cpp` keeps mainline's form,
  which lacks `Sami-OPA`'s refusal of a term that is not a bare reducer but holds one. OPT-8 has the same cause.
- **Ruling (owner, 2026-11-17).** The term is refused with the message `language/errors.md` gives a product outside a
  reducer in the objective ("DECIDE objective: 'sum(x) * max(x)' multiplies decisions outside a reducer, which is not
  supported yet"), as `sum(x) * sum(x)` gets.
- **Change.** As ruled. While in this code, look for a third reader that assumes a reducer is a whole side or a whole
  term, and file it.
- **Done when.** A test shows the refusal; `./test/decide/run_tests.sh -n 2 -k "<the new test> or linear_form or
  objective"` passes.


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

## OPT-12 — A point frame under ABS over an infinite value answers "infeasible"

- **Problem.** Seen on 052b85d26d with `SET decide_direct_solve = 'off'`. Over `f(id, v)` = (1, 1.0), (2, 2.0), (3,
  'inf'), (4, 1.0), read as `t`, with `x(REAL)`, `x BETWEEN 0 AND 10` and `MINIMIZE SUM(ABS(x - at(previous else 0: t.v)
  over (t.id)))`, the answer is "DECIDE optimization is infeasible. Prefix the query with DIAGNOSE to see which clause
  to change." With `t.v` in place of the frame it names the value: "DECIDE: t.v is NaN or infinite on some rows. Skip
  those rows with 'when isfinite(t.v):'." A range frame, `sum(from previous to next: t.v) over (t.id)`, is named the
  same way.
- **Cause (suspected, not proven).** `AddAbsValueCheck` in `src/optimizer/decide/decide_rewrite_abs.cpp` collects only
  the terms the splitter calls constant, and a point-frame marker is not one.
- **Change.** The point frame's value is checked as the range frame's is.
- **Done when.** A test shows the point frame named as the range frame is, and an oracle test answers the query under
  `when isfinite(...)`; `./test/decide/run_tests.sh -n 2 -k "<the new tests> or invalid_value or abs"` passes.

## OPT-13 — A `per` over a written column never folds into the box

- **Problem.** `decide_singleton_row_folding.cpp` (4f4cb0acfc) stops the box fold for any `per` key that holds a written
  column (null mode `NO_GROUP`), whether or not a key is NULL, so every `per t.g: x <= c` is a model row. The answers
  are the same; the cost is model size and speed. The commit's subject names the NULL key only.
- **Decision needed.** Keep it and say in done.md that a written `per` key always stays a row, or fold where no key is
  NULL.
- **Done when.** The owner's ruling is recorded and done.md states it; for folding, oracle tests with and without a
  NULL-key row pass, and `./test/decide/run_tests.sh -n 2 -k "singleton_row or message_qualifier"` passes.
