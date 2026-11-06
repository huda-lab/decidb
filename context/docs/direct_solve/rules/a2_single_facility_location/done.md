# A2 (single-facility location) — what works today

Verified on 2026-11-06, on top of `ea039b00b8`: `test/decide/run_direct_matrix.sh`, each of its four suites (auto, off, serializer, highs)
6,482 passed; C++ `build/release/test/unittest "[decidb]"` 4,410 assertions in 85 cases.
`test/decide/run_direct_matrix.sh` renews these numbers in one command (about 3 minutes; it prints one line per check).
Code: `src/optimizer/decide/direct/a2_single_facility_location_rule.cpp`. Class definition: [definition.md](definition.md).

## What it admits

One query-wide `REAL` decision (`per ():`), with:

- **The interval.** Constant bounds only, folded into one interval by the bound reader (`DirectReadBounds`): `BETWEEN`,
  one end with the default floor of 0 for the other, or an open lower end (`c >= -1e30`).
- **The objective.** One plain `SUM` of one square `K * (w*c + t)^2`, where `w` folds to a nonzero constant
  (`DirectFoldConstantSum`), `t` is a value of the source row, and `K` is positive once the objective's sense, the
  part's sign and its constant factor are applied. `POWER(c - v, 2)`, `POWER(v - c, 2)`, `POWER(2 * c - v, 2)`,
  `3 * SUM(...) / 2`, `MAXIMIZE -SUM(...)` and an objective constant (`+ 5`) all read this way, with no case list. An
  objective `when` and the sum's own `when` choose the rows the square reads.

**The answer.** The objective is `a*c^2 + b*c` with `a = SUM(K*w^2)` and `b = SUM(2*K*w*t)` over the rows the
objective keeps, the same fold A1 and N1 use (`DirectLayoutQuadraticPart`, `DirectFoldQuadratic`). Its minimum on the
interval is the turning point `-b / (2a)`, the mean of the values for `POWER(c - v, 2)`, clamped into the interval and
repeated on every source row. When the objective keeps no row, `a = 0` and every value is optimal; the answer is then
the point of the interval nearest 0 (owner ruling, 2026-11-06), the same clamp applied to 0. It adds no branch, stays
finite when the lower end is open, and equals A1's lowest value in the box whenever the lower bound is 0 or above.

A NULL value reads as no term on its row, as on the solver path. An empty interval raises the solver's infeasible
error on every row; the check is built over two constants, so DuckDB folds it. An empty source returns no rows,
whatever the interval says. A NaN or infinite value on a row the objective keeps raises the solver's error, and a sum
`a` or `b` past a DOUBLE raises the solver's "computes a number too large for a DOUBLE" with the objective as written.
Nothing is computed on a row a `when` drops.

## How it works

One read of the source, three projections and one window, no join:

1. P1, raw values: the rows the objective keeps, and the slope `w` and constant `t` of the square on each, with the
   check of every value that can fail.
2. P2: the row's `a` and `b`, zero on a row the objective drops, oriented as a minimum, and the row's checks.
3. W: one window over the whole input summing `a` and `b`. It is the aggregate of aggregate-then-repeat: the row's own
   columns are what the sums repeat on, so the source is read once. The window holds every row before it releases one,
   so each row's check has run on all of them first, as the solver reads every row before it answers.
4. P3: the clamped turning point on every row (`+ 0.0` turns `-0.0` into `0.0`), and the column that keeps the checks
   live: the row checks, the infeasible check, the two too-large checks and the warning.

## What it does not admit (falls back to the solver)

A query A2 does not apply to keeps its answer: it runs on the solver, and `require` raises one fixed error. Open work
on these shapes is in [todo.md](todo.md).

| Shape | Why |
|---|---|
| A bound read from the data, a bound under a `when` | The interval differs per row. |
| `per K:` | One center per key. |
| `SUM(w * POWER(c - v, 2))`, `POWER(w * c - v, 2)` with `w` a column | A weight or slope read from the data. |
| `ABS`, `MAX(ABS(...))`, norms | Other losses. |
| A linear term or a per-row constant beside the square, a second square, a second part | Not one square. |
| `MINIMIZE -SUM(...)`, `MAXIMIZE SUM(...)` of the square | Concave in the sense. |
| `AVG(...)` objective, no objective, a slope of 0, a square whose slope passes a DOUBLE | Not one plain `SUM` of one square. |
| A bound below the built-in floor of 0 (`c <= -1` with no lower bound) | The bound reader declines it so the solver reports it, as in A1. |
| `INT`, `BOOL`, a decision per row, two decisions | Not one query-wide `REAL`. |

## Where it differs from the solver

- **Its own warning.** The divisor is the summed square factor `a` in `-b / (2a)`, through `DirectTurningPoint`; the rule
  is the framework's [Number-range warning](../../framework/done.md).
- **Ties.** With no kept row, the solver may return any value of the interval; A2 returns the point nearest 0.
- **A square's constant past a DOUBLE** (`v = 1e200`): the solver raises, and A2 answers. The same holds for A1; the
  fix belongs in the shared layout ([framework todo](../../framework/todo.md)).

## Tests

- Conformance: the `A2` fixture in `test/decide/tests/_direct_rule_fixtures.py` (the worked example; near misses that
  decline and forms that apply; late NaN and too-large errors on the last of 5,000 rows).
- Differential oracle: `test/decide/tests/test_direct_a2_single_facility_location.py` (case table over objective
  spellings and intervals, the no-row-kept ruling, parent contexts, random problems, error and boundary tables, the
  infeasible text on both backends, the warning).
- Error text: `test_a2_errors_are_the_solvers_word_for_word` in `test_direct_user_facing.py`.
- C++: `A2AppliesShapes` and `A2DeclineShapes` in `test/common/test_decidb_direct_decline.cpp`, one decline per
  `CheckCondition` branch, and the partition test against every other rule.
