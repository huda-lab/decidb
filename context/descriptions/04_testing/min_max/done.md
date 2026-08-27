# MIN/MAX Aggregate Test Coverage — Done

> **Composed MIN/MAX coverage must use a multi-table join.** Every composed test predating
> 2026-08-08 used a single-table `VALUES` source, where a logical column index and a chunk
> position coincide — so the suite passed while the composed path read the wrong data column
> over any join. `test_composed_minmax_entity_scoped_multi_row` is the join-based regression;
> keep new composed tests on a join for the same reason.

Tests live in:
- `test/decide/tests/test_min_max.py` — primary MIN/MAX test file
- `test/decide/tests/test_min_max_multiterm.py` — row expressions that are not a single product term
- `test/decide/tests/test_per_objective.py` — nested aggregate PER objectives
- `test/decide/tests/test_aggregate_local_when.py` — aggregate-local WHEN with MAX
- `test/decide/tests/test_per_interactions.py` — hard MIN/MAX constraints with PER (per-group Big-M)

MIN/MAX linearization is performed by `DecideOptimizer::RewriteMinMax`. It
classifies each occurrence as **easy** (naturally per-row, no Big-M) or
**hard** (needs a global auxiliary variable and per-row binary indicators).

## Scenarios covered

All oracle-verified unless noted.

- **Easy cases** (`test_min_max.py`, plus entity-scoped variants in `test_entity_scope.py` and `test_aggregate_local_when.py`): `MAX(expr) <= K` / `MIN(expr) >= K` stripped to per-row, with column coefficients (`MAX(x * expr) <= K`), on INT vars, with WHEN, with PER (stripped as redundant), WHEN + PER composition, entity-scoped MAX/MIN, aggregate-local WHEN on easy MAX.
- **Hard cases (Big-M indicators)** (`test_min_max.py`, `test_per_interactions.py`, `test_entity_scope.py`, `test_aggregate_local_when.py`): `MAX(expr) >= K`, `MIN(expr) <= K`, equality for both; multiple MIN/MAX constraints in one query; MIN/MAX in both constraint and objective; entity-scoped hard MAX; PER variants with Big-M per group (`MAX >= K PER`, `MIN <= K PER`, `MAX = K PER` combining easy + hard per group); hard MAX + aggregate-local WHEN.
- **Objectives** (`test_min_max.py`): easy `MINIMIZE MAX(expr)` / `MAXIMIZE MIN(expr)` (plain, on INT, and with aggregate-local WHEN); hard `MAXIMIZE MAX(expr)` / `MINIMIZE MIN(expr)`.
- **Multi-term and constant row expressions** (`test_min_max_multiterm.py`): the inner expression is a linear combination, not a single product — `MAX((qty + 1) * x)` (distributes into two terms on one column), `MAX(qty * x + x)` (the same shape written out), `MAX(qty * x + 5)` (constant folds into the bound). Covered on the flat path (both directions), the composed path, and the PER outer-MIN path with an **entity-scoped** variable, where one column spans every row of a group so a single term is enough to repeat it. Each test pins the objective value against the oracle, so a linking row that is accepted but mis-linearized still fails.

### Nested aggregate + PER objectives

Tests in `test_per_objective.py` cover all 4 × 4 nesting combinations of
inner/outer ∈ {SUM, MIN, MAX, AVG} with PER: `SUM(MAX)`, `SUM(MIN)` (both
directions, including hard-outer shapes), `MAX(SUM)`, `MIN(SUM)`, plus
MAX(AVG), MIN(AVG), SUM(AVG) nested variants.

### Composed MIN/MAX (additive LHS/objective with mixed aggregate terms)

Oracle-verified in `test_min_max.py`: easy-direction additive constraints and
objectives; hard-direction constraints and objectives; scalar-multiplied terms;
and subtraction such as `MAX(...) - MIN(...) <= K`.

### Error cases (binder rejections)

`test_min_max.py` / `test_per_objective.py`: `MAX(x) <> K` rejected; flat
`MIN/MAX + PER` (ambiguous) rejected; and composed MIN/MAX rejected for an outer
`PER`/`WHEN` wrapper, a non-constant RHS, or equality at the outer comparison.

### Empty `WHEN` rejection (execution-time errors)

Previously tracked as a bug in `todo.md`: empty-WHEN on hard-direction MIN/MAX
silently floated the `z`/`z_k` auxiliary, making constraints vacuous and
objectives meaningless. Now rejected pre-solver by `RejectEmptyAggregate` in
`physical_decide.cpp`. See `03_expressivity/when/done.md` → "Empty Row Sets"
for the full rule.

Covered in `test_min_max.py`: hard objectives (`MAXIMIZE MIN(...) WHEN empty`
+ MAX/hard mirrors), hard constraints (`MAX WHEN empty >= K`,
`MIN WHEN empty <= K`), easy directions (`MAX <= K WHEN empty`,
`MIN >= K WHEN empty` + mirrors), composed `SUM + (MAX WHEN empty)`, mixed
empty + populated aggregate-local WHEN terms (constraint and objective),
SUM / AVG empty WHEN, and PER with one empty group (skip preserved,
`test_avg_per_constraint_with_empty_group`).

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| MIN/MAX (easy) | WHEN | ✓ |
| MIN/MAX (hard) | aggregate-local WHEN | ✓ |
| MIN/MAX (easy) | PER (stripped) | ✓ |
| MIN/MAX (easy) | WHEN + PER | ✓ |
| MIN/MAX (nested) | PER (objective) | ✓ |
| MIN/MAX (easy) | entity-scoped | ✓ |
| MIN/MAX (hard) | entity-scoped | ✓ |
| MIN/MAX | INT variables | ✓ |
| MIN/MAX (hard) | PER (per-group Big-M) | ✓ |
| MIN/MAX | multiple constraints in same query | ✓ |
| MIN/MAX | constraint + objective in same query | ✓ |
