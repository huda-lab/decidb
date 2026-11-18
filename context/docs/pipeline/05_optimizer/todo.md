# Stage 05 — Optimizer: open work

Open work in the optimizer passes. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## OPT-16 — A NaN or infinite value's message suggests a `when` that changes the query

- **Problem.** Over `f(id, v)` = (1, 1.0), (2, 2.0), (3, 'inf'), (4, 1.0), read as `t`, with `x(REAL)`,
  `x BETWEEN 0 AND 10` and `MINIMIZE SUM(ABS(x - at(previous else 0: t.v) over (t.id)))` raises "DECIDE:
  at(previous else 0: t.v) over (t.id) is NaN or infinite on some rows. Skip those rows with 'when
  isfinite(at(previous else 0: t.v) over (t.id)):'." Writing that `when` gives "Binder Error: DECIDE: a frame selector
  is not allowed in when, per, by, within or an order key" (D80, `ValidateNoFrame` in `decide_binder.cpp`). A `when`
  drops rows before any frame is computed (`frame.rows = path.Rows()`, `decide_canonicalizer.cpp`), so a `when` that
  does run shifts every frame's timeline: `when isfinite(t.c):` on a plain column of a clause that holds a frame moves
  another row's `previous`. A NaN a range frame's window reads, or a reducer over frames reads, raises its own error
  (`DecideFrameNanMessage`) whose advice, a `when` on the column, shifts the timeline too.
- **Ruling (owner, 2026-11-18).** Every NaN or infinite value's message suggests replacing the value where it stands:
  `CASE WHEN isfinite(W) THEN W END`, and for a bound `CASE WHEN NOT isnan(W) THEN W END`, W the value as written.
  The NULL skips that row's instance and leaves every timeline as it is, for a plain value, a point frame (also with a
  non-finite `else`) and a range frame alike, so the text needs no branch on the value's kind. A NaN a frame's window
  or a reducer over frames reads makes that value NaN (COUNT counts it), and the ordinary value check raises it with
  the same text; `DecideFrameNanMessage` goes. NaN stays an error everywhere it is one.
- **Change.** `DecideInvalidValueMessage` gives the in-place edit for the COEFFICIENT and BOUND roles. Every place a
  frame's value is read (a coefficient, a bound, an ABS argument, a reducer over frames, a direct-solve plan) has the
  value check that raises on its NaN, and a window or frame reducer that reads a NaN yields NaN instead of raising.
- **Done when.** For each place that raises the message, a test writes the suggested edit and gets the oracle's
  answer; `./test/decide/run_tests.sh -n 2 -k "invalid_value or frame or error_precedence or bound_side_tables"`
  passes.

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
