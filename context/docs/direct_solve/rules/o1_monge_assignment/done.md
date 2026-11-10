# O1 (Monge assignment) — what works today

Verified on 2026-11-10 at `5dff818acd`: `test/decide/run_direct_matrix.sh`, each of its four suites (auto, off,
serializer, highs) 6,602 passed; C++ `build/release/test/unittest "[decidb]"` 5,796 assertions in 102 cases. Code:
`src/optimizer/decide/direct/o1_monge_assignment_rule.cpp`. Class definition: [definition.md](definition.md).

## What it admits

A one-to-one matching of the rows of two tables, over their cross product:

```sql
SELECT l.id, r.id, x
FROM left_items l CROSS JOIN right_items r
DECIDE x(BOOL)
SUCH THAT per l: SUM(x) by (l) = 1
AND per r: SUM(x) by (r) = 1
MINIMIZE SUM(POWER(l.position - r.position, 2) * x);
```

- **The source.** DECIDE's own child is a cross product (the IR's `cross_product`). A `WHERE` on one side is admitted:
  filter pushdown has placed it inside that side.
- **The decision.** One `BOOL` per row, with no `per` of its own.
- **The constraints.** Exactly two, each `per K: SUM(x) by (K) = 1` with no `when`, the same key on `per` and `by`,
  every element of the key a column of one side (`per l`, `per l.id, l.position`), and the two keys on different sides.
  A key element that stands for a relation takes NULL as a value, so a class is a distinct row of its side and a
  duplicate row shares its class.
- **The objective.** One term, `k * cost * x`, where `k` folds to a constant (casts, `*`, unary `-`, a scale outside the
  `SUM`, and the sense all fold into one sign) and `cost` is one of:
  - `ABS(a - b)` or `POWER(a - b, 2)`, minimized with `k >= 0`: equal ranks;
  - `a * b`: equal ranks when the signed term is maximized, opposite ranks when it is minimized.

  `a` is a numeric column of one side and `b` of the other, in either order, with casts looked through.
- **The positions.** `a` and `b` are declared `NOT NULL` on the tables their sides scan directly (the IR's `not_null`).

**The answer.** Each side's classes are ranked by position; the pair of the left class and the right class of equal
rank (or opposite rank) gets `x = 1`, once per matched class pair, and every other pair gets 0. An empty side gives an
empty result, as the solver's empty model does.

**Errors, as on the solver path.** A NaN or infinite coefficient on any pair raises the solver's error, also under
`LIMIT 1` or `COUNT(*)`. Unequal class counts on two nonempty sides raise the solver's infeasible error.

## How it works

Group-then-join over three reads of the source (`DirectBuildOverRead` twice, `DirectBuildOverOneRead` once).

1. `RankClasses`, once per side: a hash aggregate on the side's key gives one row per class, and a window gives each
   class its `ROW_NUMBER` and the `COUNT(*)` of classes, both ordered by position (descending on the right side for
   opposite ranks), so the two share one sort.
2. `Assign`, on the third read of the pairs: the written coefficient on every pair (`DirectScaledTerm`, checked by
   `DirectCheckNumber`), then a join to each side's class answers (`DirectJoinClassAnswers`, which compares each key
   column by what a NULL means in it), then the class-count check (`DirectInfeasibleErrorPredicate`).
3. A `ROW_NUMBER` window partitioned by the two ranks picks one copy of each class pair; `x` is 1 where the ranks are
   equal and the number is 1.
4. The number and the coefficient stay as validated columns: no parent can prune them, so every pair is read and
   checked before a `LIMIT` returns.

`EXPLAIN` shows `O1_MONGE_ASSIGNMENT` and its summary line.

## What it does not admit (falls back to the solver)

A query O1 does not apply to keeps its answer: it runs on the solver, and `require` raises one fixed error. Open work
on these shapes is in [todo.md](todo.md).

| Shape | Why |
|---|---|
| A maximized distance, or a distance with `k < 0` | Opposite ranks are optimal there, but not proved for the rule; CAN-2 makes `MAXIMIZE` one sense. |
| `pow`, `**`, `^` | Only the name `power` is read; CAN-2 gives the power one spelling. |
| `TRY_CAST` around a position | It can make a NULL the table never holds. |
| A position column not declared `NOT NULL`, or read through a projection, outer join or computed expression | A NULL position makes its item free (`SUM` drops the term), and ranks no longer place it. |
| `per l.id` and other column keys | A column key puts a NULL in no class; the primary-key form is O1-7. |
| A `WHERE` reading both sides | It makes a join: not every pair exists. |
| A `when`, a third constraint, fixed pairs, a bound other than `= 1` | Not a plain matching. |
| A second objective term or part, `AVG`, no objective | Not one cost. |
| `INT` or `REAL` decisions, two decisions, a keyed decision | Not one `BOOL` per pair. |

## Where it differs from the solver

- **Ties.** Several matchings can be optimal (tied positions, duplicate rows); O1 may return a different one.
- **No number-range warning.** O1 only ranks and compares positions, so no step of its plan divides or loses precision
  (owner's ruling, 2026-11-10).

## Tests

- Conformance: the `O1` fixture in `test/decide/tests/_direct_rule_fixtures.py` (TPC-H `nation` × `region`; near
  misses that decline, forms that apply, the infeasible late error).
- Differential oracle: `test/decide/tests/test_direct_o1_monge_assignment.py`: every objective form, sign and spelling,
  duplicate rows, a NULL key value, tied, negative and integer positions, one-side `WHERE`s, unequal sizes, against the
  oracle; parent contexts; an empty side; a `WHERE` across both sides and nullable positions keep the solver; NaN
  positions fail on both paths; a NaN on the last of 71 × 71 pairs raises under `LIMIT 1` and `COUNT(*)`.
- Error text: `test_o1_monge_infeasible_error_is_the_solvers_error_word_for_word` in `test_direct_user_facing.py`.
- C++: `O1AppliesShapes` and `O1DeclineShapes` in `test/common/test_decidb_direct_decline.cpp`, one decline per
  `CheckCondition` branch, and the partition test against every other rule; the IR's `cross_product` and `not_null` in
  `test_decidb_direct_ir.cpp`.
- Join order: `test_a_parent_filter_stays_above_a_decide_over_a_join` in `test_nested_decide.py`.
