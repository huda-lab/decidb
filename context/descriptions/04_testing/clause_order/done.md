# Clause-Order Test Coverage — Done

Tests live in `test/decide/tests/test_clause_order.py`.

Covers the two accepted positions of the `DECIDE` declaration — the paper's
split order (declaration between `SELECT` and `FROM`, constraints and objective
after the joins) and the single-block order (whole clause after `WHERE`). The
feature itself is described in `03_expressivity/decide/done.md` → "Two clause
orders" and `00_project_overview/syntax_reference.md` §1.

## Scenarios covered

- **Split order is correct, not just parseable** (`test_split_order_oracle`,
  oracle-verified): a knapsack over `customer JOIN nation` written in the paper's
  order. An oracle here is what rules out the declaration reaching the binder
  detached from its constraints — a shape that would still parse.
- **The two orders agree** (`test_orders_return_the_same_rows`): one problem with
  a unique optimum written both ways; rows compared directly, so a divergence
  shows up as a different selection rather than an alternate optimum.
- **The two orders produce the same plan** (`test_orders_produce_the_same_plan`):
  normalized `EXPLAIN` output compared across the orders. This is the direct
  statement of "nothing downstream of the parser distinguishes them".
- **Every declaration scope parses in the split slot**: one decision per row
  (throughout), `per (): cap(INT)` (`test_scalar_declaration_in_split_slot`, one
  shared value across rows) and `per n: keep(BOOL)`
  (`test_entity_scoped_declaration_in_split_slot`, one value per nation across the
  join's repeated rows, with a colon-form reducer `SUM(n: keep)`).
- **The `in_decide_clause` lexer flag is cleared between the slots**:
  `test_case_when_in_join_on_between_slots` and
  `test_case_when_in_where_between_slots`. This is the hazard the split order
  introduced — the declaration arms the DECIDE lexer context, and the SQL
  between it and `SUCH THAT` must not see it, or `CASE WHEN` would lex as the
  clause's `when`.
- **Malformed splits are parser errors** (`TestClauseOrderErrors`): a declaration
  in both slots, a declaration with no `SUCH THAT`, and a `SUCH THAT` with no
  declaration. Separately, `test_declaration_between_from_and_where_rejected`
  pins that there are exactly **two** slots — `FROM ... DECIDE ... WHERE ...` is
  not a third one, which is a plausible guess once the split order exists.
- **The remaining source shapes**: a subquery source
  (`test_split_order_over_subquery_source`, keyed `per t:` on the subquery, so it
  exercises the binding path of a keyed decision over a subquery), a `WITH` CTE
  (`test_split_order_over_cte_source`), and a three-table fan-out join
  (`test_split_order_three_table_fanout_join`, oracle-verified — the declaration
  slot closes before the first `JOIN` is parsed, so this is the widest gap the
  reassembly spans).
- **`when` and `per` in the split order**: a `when` in front of a constraint
  (`test_split_order_with_when`), `per K: ... by (K)` on a constraint
  (`test_split_order_with_per`, which also asserts the group cap holds), and the
  nested objective `min(per k: sum(...) by (k))`
  (`test_split_order_with_nested_per_objective`). Each is compared against the
  single-block spelling of the same query rather than asserted in isolation,
  since the claim under test is equivalence.

`test_declaration_in_both_slots_rejected` pins the duplicate-declaration message:
declare variables either before `FROM` or with `SUCH THAT`, not in both slots.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| split order | JOIN + WHERE | ✓ |
| split order | per-row declaration | ✓ |
| split order | keyed declaration (`per n: x`) | ✓ |
| split order | query-wide declaration (`per (): x`) | ✓ |
| split order | colon-form reducer `SUM(n: ...)` | ✓ |
| split order | `CASE WHEN` in an intervening `JOIN ... ON` | ✓ |
| split order | `CASE WHEN` in an intervening `WHERE` | ✓ |
| split order | `EXPLAIN` | ✓ |
| split order | subquery source / CTE source | ✓ |
| split order | three-table fan-out join | ✓ |
| split order | `when` in front of a constraint | ✓ |
| split order | `per` on a constraint | ✓ |
| split order | nested objective | ✓ |
