# Stage 05 — Optimizer: open work

Open work in the optimizer passes. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## OPT-16 — A non-finite frame's message suggests a `when` the binder refuses

- **Decision needed.** What the message suggests for a frame, since `when isfinite(<frame>):` cannot be written.
- **Problem.** Over `f(id, v)` = (1, 1.0), (2, 2.0), (3, 'inf'), (4, 1.0), read as `t`, with `x(REAL)`,
  `x BETWEEN 0 AND 10` and `MINIMIZE SUM(ABS(x - at(previous else 0: t.v) over (t.id)))` raises "DECIDE:
  at(previous else 0: t.v) over (t.id) is NaN or infinite on some rows. Skip those rows with 'when
  isfinite(at(previous else 0: t.v) over (t.id)):'." Writing that `when` gives "Binder Error: DECIDE: a frame selector
  is not allowed in when, per, by, within or an order key" (D80, `ValidateNoFrame` in `decide_binder.cpp`).
- **Options.**
  1. The binder accepts a frame in a clause's `when`. Cost: re-ruling D80, whose reason is that a frame reads rows by
     a timeline the scope chooses, and computing frames before the `when` filter.
  2. The message suggests nulling the value inside the frame's argument (`CASE WHEN isfinite(t.v) THEN t.v END`). A
     point frame then selects NULL and its instance is skipped (deck p. 50); a range frame leaves the value out of
     its window, which is a different query.
  3. The message names the frame and suggests no fix.
- **Recommendation.** 2 for a point frame, the shape that raises here: it is writable today and skips what the `when`
  would. An architect review settles whether a range frame takes the same text.
- **Ruling (owner, 2026-11-18).** Option 2 for a point frame. A range frame's text is the one the architect review
  settles; a choice the review leaves open goes to the owner.
- **Change.** A frame's non-finite message suggests only what the binder accepts.
- **Done when.** A test writes the suggested fix and gets an answer; `./test/decide/run_tests.sh -n 2 -k
  "invalid_value or frame"` passes.

## OPT-17 — Solver-path checks copy the bound with every tag stripped

- **Problem.** `ExpressionBuilder::Of` drops every tag of what it copies. The bound-side rewrite
  (`decide_rewrite_rhs_bounds.cpp`) builds its VALUE_IS_VALID checks and `NanMessage` with it, so a check reads a
  bound without its query-wide, reducer or frame tags. The ABS value check read a point frame over every row for this
  reason. No failing query is known: a frame bound returns before any check, and a frame under a reducer goes through
  `MakeFrameAggregate`.
- **Change.** A check the tree reads keeps the tags of what it copies (`calc.Wrap(expr.Copy())`, as
  `AddAbsValueCheck` does), or the code shows that no tagged expression reaches a check built with `Of`.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "bound_side_tables or invalid_value or frame"` passes.

## OPT-18 — `abs(x - 2) <= avg(t.id)` on an INT reads the average as each row's id

- **Problem.** Over `t(id)` = 1, 2, 3, with `x(INT)`, `x >= 0 AND x <= 5 AND abs(x - 2) <= avg(t.id)` and
  `MAXIMIZE sum(x)` answers 3, 4, 5 in the default mode and with `SET decide_direct_solve = 'off'`. It should be
  4, 4, 4: the average is 2 on every row. `x(REAL)` answers 4, 4, 4, and so does the clause without `abs`.
  `sum(t.id) / 3` gives the same wrong answer; `max(t.id)` gives the right one (5, 5, 5).
- **Cause.** Not traced. The wrong answer needs an INT decision, ABS, and a bound that reads a reducer.
- **Change.** The bound reads the query-wide value on every row, whatever the decision's type.
- **Done when.** An oracle test of the query with `avg(t.id)` and with `sum(t.id) / 3` passes; `./test/decide/
  run_tests.sh -n 2 -k "<the new test> or abs or bound_side_tables"` passes.
