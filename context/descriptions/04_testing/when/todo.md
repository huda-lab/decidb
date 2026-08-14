# WHEN Clause Test Coverage — Todo

## Missing coverage

### Empty WHEN on MIN/MAX constraints — fixed and covered

Previously these shapes silently returned `OPTIMAL` with the constraint
vacated. That no longer happens: every empty aggregate (SUM/AVG as well as
MIN/MAX, composed or not) is now rejected before the solver runs with
`DECIDE empty row set for <agg> in <context>. An empty aggregate has no
well-defined value; check your WHEN clause.` (guard in
`src/execution/operator/decide/physical_decide.cpp:1064`).

Constraint-side coverage now exists in `test_edge_cases.py` (all assert the
shared `_EMPTY_WHEN_ERROR_REGEX`):

- Non-composed easy/hard: `test_{min,max}_{geq,leq}_constraint_when_empty`,
  `test_{max,min}_when_empty_constraint_hard`.
- Composed additive: `test_sum_plus_max_when_empty_silently_vacates_constraint`
  (the exact `SUM(x*v) + (MAX(x*v) WHEN <never>) <= K` shape), plus
  `test_mixed_empty_and_populated_when_terms_constraint` (SUM+SUM).
- Bare SUM/AVG: `test_sum_when_empty_rejected`, `test_avg_when_empty_rejected`.

No open gap remains here (the test docstrings still narrate the old
"silently OPTIMAL" behavior in a few places — harmless, but worth a cleanup
pass). Shared root cause history tracked in `../min_max/todo.md`.

### `decide_when_condition` grammar restrictions — covered

Closed by `test/decide/tests/test_when_grammar.py`:

- **Positive parenthesized constraint tests** (3): `WHEN (NOT w)`, `WHEN (tier = 'high')`, `WHEN (a + b > 5)` — all oracle-verified.
- **Positive parenthesized objective tests** (3): same 3 shapes — all oracle-verified.
- **Positive objective atomic-comparison equivalence** (1): unparenthesized
  `WHEN tier = 'high'` matches the parenthesized objective form.
- **Negative unparenthesized tests** (5): aggregate-local constraint
  `WHEN tier = 'high'` before its bound, plus `WHEN NOT w` and
  `WHEN a + b > 5` on both constraint and objective paths.
- **Parenthesization-hint pin** (1): `test_when_unparen_error_carries_paren_hint` asserts the appended hint text (`wrap the WHEN condition in parentheses`) so the `MaybeAppendDecideWhenHint` augmentation cannot be silently dropped.
