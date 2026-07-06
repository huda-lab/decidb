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
- **Negative unparenthesized constraint tests** (3): pin shape-specific parser errors per `../../03_expressivity/when/todo.md` table.
- **Negative unparenthesized objective tests** (2): `WHEN NOT w` (parser-level) and `WHEN a + b > 5` (binder-level). The third unparenthesized objective shape `WHEN tier = 'high'` is NOT a negative test because the reassociator handles it; that path is exercised in the parenthesized objective positive set.
- **Asymmetric-error sentinel** (1): `SUM(x*v) WHEN tier = 'high' <= 10` on a constraint pins the actual `syntax error at or near "<="` parser error. The earlier doc claim that this produces an `"LHS must be a DECIDE variable or SUM expression"` message was incorrect — that's a binder-level error path that doesn't fire here because the parser bails first.
- **Parenthesization-hint pin** (1): `test_when_unparen_error_carries_paren_hint` asserts the appended hint text (`wrap the WHEN condition in parentheses`) so the `MaybeAppendDecideWhenHint` augmentation cannot be silently dropped.

If the grammar widens or a constraint-side reassociator is added, the sentinel test will fail; the appropriate response is to delete it and convert the case to a positive test, not to relax the regex. See `../../03_expressivity/when/todo.md` for the full asymmetry table.
