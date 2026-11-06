# A2 (single-facility location) — what works today

Verified on 2026-11-07, on top of `e863cbcc1a`: `test/decide/run_direct_matrix.sh`, each of its four suites (auto, off, serializer, highs)
6,593 passed; C++ `build/release/test/unittest "[decidb]"` 4,531 assertions in 86 cases.
`test/decide/run_direct_matrix.sh` renews these numbers in one command (about 3 minutes; it prints one line per check).
Code: `src/optimizer/decide/direct/a2_single_facility_location_rule.cpp`. Class definition: [definition.md](definition.md).

## What it admits

One `REAL` decision shared by many rows, query-wide (`per ():`) or keyed (`per k:`, `per k, j:`, `per D:`), with:

- **The key.** One problem per class of the key. A row whose column element of the key is NULL is in no class: it has
  no decision, its `c` is NULL, and nothing on it enters the objective, as on the solver path. An element that stands
  for a relation (`per D:`) takes NULL as a value of its own. The IR carries the key as `DirectKeyElement`s
  (`DirectIRKeyedScope::key`, read by the same `ReadKey` as a `by` key), and the plan partitions by it through
  `DirectProjectScope`.
- **The interval.** Per class, the decision's box (the default floor of 0 or a written constant lower bound, and a
  constant upper bound) tightened by every bound clause: `MAX` of each clause's lower ends and `MIN` of its upper ends
  over the rows of the class it keeps (`DirectTightestBound`). Constant and data bounds are one reading; a constant
  folds. As on the solver path, a NULL bound is skipped, an infinite bound in the open direction is no bound, an
  infinite upper bound of `-inf` is infeasible, and a data lower bound does not lift the default floor of 0. A constant
  lower bound under a `when` lifts the floor for the whole class when some row of the class keeps it
  (`DirectStaticFloor` over a per-class `bool_or`).
- **The objective.** One plain `SUM` of one square `K * (w*c + t)^2`, where `w` folds to a nonzero constant
  (`DirectFoldConstantSum`), `t` is a value of the source row, and `K` is positive once the objective's sense, the
  part's sign and its constant factor are applied. `POWER(c - v, 2)`, `POWER(v - c, 2)`, `POWER(2 * c - v, 2)`,
  `3 * SUM(...) / 2`, `MAXIMIZE -SUM(...)` and an objective constant (`+ 5`) all read this way, with no case list. An
  objective `when` and the sum's own `when` choose the rows the square reads.

**The answer.** The objective is `a*c^2 + b*c` with `a = SUM(K*w^2)` and `b = SUM(2*K*w*t)` over the rows the
objective keeps, the same fold A1 and N1 use (`DirectLayoutQuadraticPart`, `DirectFoldQuadratic`). Its minimum on the
interval is the turning point `-b / (2a)`, the mean of the values for `POWER(c - v, 2)`, clamped into the interval and
repeated on every row of the class. When the objective keeps no row of a class, `a = 0` and every value is optimal; the
answer is then the point of the class's interval nearest 0 (owner ruling, 2026-11-06), the same clamp applied to 0. It adds no branch, stays
finite when the lower end is open, and equals A1's lowest value in the box whenever the lower bound is 0 or above.

A NULL value reads as no term on its row, as on the solver path. An empty interval raises the solver's infeasible
error on every row that has a class; over constant bounds the check is two constants, which DuckDB folds. An empty
source returns no rows, whatever the interval says. A NaN or infinite value on a row the objective keeps raises the
solver's error, and so does a NaN bound on a row its clause keeps, whether or not the row has a class. A sum
`a` or `b` past a DOUBLE raises the solver's "computes a number too large for a DOUBLE" with the objective as written.
Nothing is computed on a row a `when` drops.

## How it works

One read of the source, four projections and one window, no join. `PlanBuilder` has one method per stage:

1. Scope (`DirectProjectScope`, keyed decisions only): the row's class, whether it has one, and its key.
2. P1, raw values (`LayoutRawValues`): the rows the objective keeps, the slope `w` and constant `t` of the square on
   each (`DirectLayoutQuadraticPart`), and each bound clause's values on the rows it keeps
   (`DirectLayoutBoundClauses`, shared with A1), with the check of every value that can fail.
3. P2 (`ComputeCoefficients`): the row's `a` and `b`, zero on a row the objective drops, oriented as a minimum; each
   clause's ends on the row (`DirectClauseBounds`); and the row's checks.
4. W (`AggregateClasses`): one window partitioned by the key (none for a query-wide decision) holding the sums of `a`
   and `b`, each clause's tightest ends through `DirectWindowSink`, and the `bool_or` of each floor-lifting `when`. It is
   the aggregate of aggregate-then-repeat: the row's own columns are what the class's values repeat on, so the source
   is read once. The window holds every row before it releases one, so each row's check has run on all of them first,
   as the solver reads every row before it answers.
5. P3 (`ComputeInterval`): the class's interval as columns, with the floor.
6. P4 (`ComputeAnswer`): the clamped turning point on every row of the class (`DirectRowBox`; `+ 0.0` turns `-0.0` into
   `0.0`), NULL on a row with no class, and the column that keeps the checks live: the row checks, the infeasible
   check, the two too-large checks and the warning.

## What it does not admit (falls back to the solver)

A query A2 does not apply to keeps its answer: it runs on the solver, and `require` raises one fixed error. Open work
on these shapes is in [todo.md](todo.md).

| Shape | Why |
|---|---|
| `SUM(w * POWER(c - v, 2))`, `POWER(w * c - v, 2)` with `w` a column | A weight or slope read from the data. |
| `ABS`, `MAX(ABS(...))`, norms | Other losses. |
| A linear term or a per-row constant beside the square, a second square, a second part | Not one square. |
| `MINIMIZE -SUM(...)`, `MAXIMIZE SUM(...)` of the square | Concave in the sense. |
| `AVG(...)` objective, no objective, a slope of 0, a square whose slope passes a DOUBLE | Not one plain `SUM` of one square. |
| A bound below the built-in floor of 0 (`c <= -1` with no lower bound) | The bound reader declines it so the solver reports it, as in A1. |
| `INT`, `BOOL`, a decision per row, two decisions | Not one shared `REAL`. |

## Where it differs from the solver

- **Its own warning.** The divisor is the summed square factor `a` in `-b / (2a)`, through `DirectTurningPoint`; the rule
  is the framework's [Number-range warning](../../framework/done.md).
- **Ties.** With no kept row in a class, the solver may return any value of the interval; A2 returns the point nearest
  0.
- **A square's constant past a DOUBLE** (`v = 1e200`): the solver path expands the square and refuses once its
  constant `k*t^2` passes a DOUBLE; A2 answers the true center, which never reads that constant
  (`../../architecture/rules.md` § What every rule must match from the solver).

## Tests

- Conformance: the `A2` fixture in `test/decide/tests/_direct_rule_fixtures.py` (the worked example; near misses that
  decline and forms that apply, keyed and with data bounds among them; late NaN values and bounds, query-wide and
  keyed, and a too-large error, on the last of 5,000 rows).
- Differential oracle: `test/decide/tests/test_direct_a2_single_facility_location.py`: query-wide, keyed and
  data-bound case tables (several keys, a NULL key, a multi-column and a relation key, a class with no kept row, data
  upper and lower bounds, both under a `when`, a lifted floor, an empty interval), the no-row-kept ruling per class,
  parent contexts, random problems, error and boundary tables, the infeasible text on both backends, the warning.
- Error text: `test_a2_errors_are_the_solvers_word_for_word`, `test_a2_keyed_errors_are_the_solvers_word_for_word` and
  `test_a2_a_nan_data_bound_is_the_solvers_word_for_word` in `test_direct_user_facing.py`.
- C++: `A2AppliesShapes` and `A2DeclineShapes` in `test/common/test_decidb_direct_decline.cpp`, one decline per
  `CheckCondition` branch, and the partition test against every other rule; the keyed scope's key in
  `test_decidb_direct_ir.cpp`.
