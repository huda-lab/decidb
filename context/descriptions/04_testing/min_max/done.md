# MIN/MAX Aggregate Test Coverage — Done

> **Composed MIN/MAX coverage must use a multi-table join.** On a single-table `VALUES`
> source a logical column index and a chunk position coincide, so a composed path that
> read the wrong data column over a join would still pass.
> `test_composed_minmax_entity_scoped_multi_row` is the join-based guard; keep new
> composed tests on a join for the same reason.

Tests live in:
- `test/decide/tests/test_min_max.py` — primary MIN/MAX test file
- `test/decide/tests/test_min_max_multiterm.py` — row expressions that are not a single product term
- `test/decide/tests/test_per_objective.py` — nested objectives
- `test/decide/tests/test_aggregate_local_when.py` — a reducer's own `when` with MAX
- `test/decide/tests/test_per_interactions.py` — hard MIN/MAX constraints under `per` (per-group Big-M)
- `test/decide/tests/test_edge_cases.py` — empty row sets under MIN/MAX (see [`../edge_cases/done.md`](../edge_cases/done.md))

MIN/MAX linearization is performed by `DecideOptimizer::RewriteMinMax`. It
classifies each occurrence as **easy** (naturally per-row, no Big-M) or
**hard** (needs a global auxiliary variable and per-row binary indicators).

## Scenarios covered

All oracle-verified unless noted.

- **Easy cases** (`test_min_max.py`, plus keyed variants in `test_entity_scope.py` and a reducer's own `when` in `test_aggregate_local_when.py`): `MAX(expr) <= K` / `MIN(expr) >= K` bound every member on its own row, with column coefficients (`MAX(x * expr) <= K`), on INT vars, under a `when`, under `per g: ... by (g)` (the same per-row bounds), `when` + `per` together, keyed MAX/MIN, a reducer's own `when` on an easy MAX. Over a bare decision with no `when` and no reducer filter the bound is absorbed into the decision's box (D41), so a negative K makes the decision signed (`test_scope_bare_clauses.py`).
- **Hard cases (Big-M indicators)** (`test_min_max.py`, `test_per_interactions.py`, `test_entity_scope.py`, `test_aggregate_local_when.py`): `MAX(expr) >= K`, `MIN(expr) <= K`, equality for both; multiple MIN/MAX constraints in one query; MIN/MAX in both constraint and objective; keyed hard MAX; `per g:` variants with Big-M per group (`max(e) by (g) >= K`, `min(e) by (g) <= K`, `max(e) by (g) = K` combining easy + hard per group); hard MAX with a reducer's own `when`.
- **Objectives** (`test_min_max.py`): easy `MINIMIZE MAX(expr)` / `MAXIMIZE MIN(expr)` (plain, on INT, and under a `when` in front); hard `MAXIMIZE MAX(expr)` / `MINIMIZE MIN(expr)`.
- **Multi-term and constant row expressions** (`test_min_max_multiterm.py`): the inner expression is a linear combination, not a single product — `MAX((qty + 1) * x)` (distributes into two terms on one column), `MAX(qty * x + x)` (the same shape written out), `MAX(qty * x + 5)` (constant folds into the bound). Covered on the flat path (both directions), the composed path, and the nested objective's outer MIN with a **keyed** decision, where one column spans every row of a group so a single term is enough to repeat it. Each test pins the objective value against the oracle, so a linking row that is accepted but mis-linearized still fails.

### Nested MIN/MAX

`test_per_objective.py` covers `outer(per k: inner(e) by (k))` with outer ∈ {SUM,
MIN, MAX} and inner ∈ {SUM, MIN, MAX, AVG}: `sum(per k: max(...))` and
`sum(per k: min(...))` (both directions, easy and hard inner), `max(per k: sum(...))`,
`min(per k: sum(...))`, and `max`/`min`/`sum` over an inner `avg`.
`max(per g: max(c * x) by (g))` with mixed-sign coefficients is in `test_min_max.py`.
An outer AVG answers (`test_scope_objective.py`). Nested MIN/MAX in constraints, the
sides each level is pinned on, and a root bound reaching a nested closing link on HiGHS
are `test_anr_nested.py`'s (D73).

### Composed MIN/MAX (additive LHS/objective with mixed aggregate terms)

Oracle-verified in `test_min_max.py`: easy-direction additive constraints and
objectives; hard-direction constraints and objectives; scalar-multiplied terms;
and subtraction such as `MAX(...) - MIN(...) <= K`. A composed clause under a
`per` key (`per g: sum(x * v) by (g) + max(when w: x * v) by (g) <= 12`), under a
`when` in front, and against a column or subquery bound answers, judged by the
scope oracle (`test_composed_minmax_per_scope_answers`,
`test_composed_minmax_front_when_answers`, `test_composed_minmax_column_rhs_answers`,
`test_composed_minmax_subquery_rhs_answers`). Composed MIN/MAX expressivity:
`03_expressivity/such_that/done.md` and `03_expressivity/maximize_minimize/done.md`.

### Error cases (binder rejections)

`test_min_max.py`: `MAX(x) <> K` rejected (`test_max_notequal_error`). A square or a
product of decisions inside a MIN or MAX is refused, on both solvers, wherever its members
would be stated as linear rows -- the hard direction, `=`, a product of two INT decisions,
either term of a composed clause, the objective -- and the bound every member meets still
answers, `max(power(x, 2)) <= 16` among them (`test_a_min_or_max_over_a_square_is_refused`,
`test_a_square_under_a_bound_every_member_meets`). The
removed spellings of a grouped MIN/MAX objective are refused by the parser
(`syntax_reference.md` §10.3, `test_scope_removed_syntax.py`).

`<>` is the only outer comparison the composed path refuses, pinned by
`test_composed_minmax_outer_not_equal_rejected` (over a whole-numbered column so
the generic fractional-`<>` refusal does not fire first).

### Outer equality and `BETWEEN` on a composed MIN/MAX

`SUM(x*v) + MAX(x*v) = K` solves. An equality holds the composed sum from both
sides, so every `z_k` in it carries the envelope *and* the closing indicator
rows, and the outer row goes to the backend as one native `=`.
`test_composed_minmax_outer_equality_max` checks it against an oracle model that
spells that pinning out by hand; `test_composed_minmax_outer_equality_min`
covers the `MIN` arm on an instance where one selection is forced.

`BETWEEN` arrives at the composed path as its two directional halves, so both
are ordinary supported bounds and the constraint solves —
`test_composed_minmax_between_is_supported`. Equal endpoints mean the same thing
as `= K` and agree with it
(`test_composed_minmax_equality_via_degenerate_between`), but they are the more
expensive spelling: two comparisons allocate an auxiliary each, where `= K`
allocates one and pins both its sides.

### Empty row sets

Both empty-set errors are raised before the solver runs, so an empty set never leaves
a MIN/MAX auxiliary free (a vacuous constraint or a meaningless objective): a `when` in
front that keeps no row ("when c matches no rows.", D4) and a reducer whose own `when`
keeps no row ("max(when c: e) has no rows.", D8). The tests
are in `test_edge_cases.py` and cover flat objectives in all four directions, hard and
easy constraints, composed `sum(...) + max(when c: ...)`, mixed empty and populated
reducers, SUM and AVG, and a `per` class the `when` in front leaves empty (no
constraint for it) — see [`../edge_cases/done.md`](../edge_cases/done.md) → "Empty row
sets".

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| MIN/MAX (easy) | `when` | ✓ |
| MIN/MAX (hard) | a reducer's own `when` | ✓ |
| MIN/MAX (easy) | `per` (per-row bounds) | ✓ |
| MIN/MAX (easy) | `when` + `per` | ✓ |
| MIN/MAX (inner or outer) | nested objective | ✓ |
| MIN/MAX (easy) | keyed decision | ✓ |
| MIN/MAX (hard) | keyed decision | ✓ |
| MIN/MAX | INT variables | ✓ |
| MIN/MAX (hard) | `per` (per-group Big-M) | ✓ |
| composed MIN/MAX | `per`, `when` in front, column or subquery bound | ✓ (scope oracle) |
| MIN/MAX | multiple constraints in same query | ✓ |
| MIN/MAX | constraint + objective in same query | ✓ |
