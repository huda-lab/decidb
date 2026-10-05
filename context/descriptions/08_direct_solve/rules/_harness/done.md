# Shared engine — what exists today

Verified at `a1ac47407e` on 2026-10-05: `make decide-test` 1,883 passed (direct on and with
`DECIDB_TEST_DIRECT_SOLVE=off`), serializer run 1,883 passed, C++ `[decidb]` 907 assertions in 34 cases.
The forced-HiGHS run has one failure, `test_norm_combined_l1_l2_objective`, which needs Gurobi; the other 1,882 passed.
Design: `../../architecture/`.

## What it does

- **Setting and fallback.** `decide_direct_solve` is `auto` (default), `off` or `require`. A hit replaces the DECIDE node
  before solver selection; a miss leaves it for the solver. `DIAGNOSE` and a forced backend use the solver and conflict
  with `require`. A prepared plan keeps its selection until rebound. `require` also fails when the DECIDE optimizer is off.
- **Facts.** One read-only adapter turns the bound tree into facts: decision domains and scopes, each constraint with
  reducer, filter, qualifier, factor, `PER`/`WHEN`, right side and provenance, and the objective. Unmodeled parts are
  *unknown* with a reason. It uses the same term splitter as the solver path.
- **Rule contract.** Match, Prove, Cost, Explain, Rewrite, plus a shared Map step that checks the output slots. Rules are
  listed in `direct_registry.cpp`; tests can replace the list per connection. The cheapest proved rule wins, ties go to
  the first registered. `Cost` sees the estimated source row count without disturbing cached estimates.
- **Result boundary.** Keeps original bindings and types, serializes, prunes outputs only when skipping is proved safe,
  and always keeps the rule's validation slots live. `DirectValidationBarrier` builds the all-rows read for a plan that
  could stream.
- **Shared builders.** `DirectProjectScope`, `DirectGuardEmptyAggregate` and `DirectValidateBounds` give every aggregate
  rule the solver's `PER`/`WHEN`, NULL-key bypass, empty-aggregate and bound-validation behavior and error order.
- **Shared admission predicates.** `DirectIsNumericDecisionFree`, `DirectSourceNumericColumn`,
  `DirectIsSourceOnlyNumeric` and `DirectConstraintFact::PlainSum` answer "may a rule use this value" once for all rules.
- **NULL score message.** A NULL score names the NULL source columns on the failing row, in the solver's wordings: one
  column (`column "a" is NULL. Impute it with COALESCE(a, 0)...`), several (`columns "a", "b" and "c" are NULL`), or,
  when no column is NULL, the score's own text (`TRY_CAST(s AS DOUBLE) is NULL`). The message is a per-row expression
  (`DirectNullScoreMessage` in `direct_builder.cpp`) that `error()` evaluates only on the failing row. A multi-term score
  with no NULL column stays generic, since the solver quotes one failing term and direct sums the terms. Tests:
  `test_computed_score_null_error_names_the_columns`, `test_null_score_no_column_explains_quotes_the_score`,
  `test_null_score_message_is_not_built_when_no_row_is_null` in `test_direct_solve.py`. The cost on clean data is not
  measured.
- **Decision record.** On a hit, mode, rule, proof and guards appear in logical and physical `EXPLAIN` and profiling. A
  miss under `auto` prints nothing.

## Verified by

- C++: `test_decidb_direct_facts.cpp` (every fact kind and unknowns), `test_decidb_direct_coordinator.cpp` (stub rules: hit,
  fallback with every reason, cheaper rule, tie, cost context, non-finite cost, mismatched slot, barrier under
  `LIMIT 1` and `COUNT(*)`).
- Python: `test_direct_rule_contract.py` (31 tests, all through S1's fixture: schema, all-rows read, serializer, prepared
  plans, near misses, `off`, forced backend, `DIAGNOSE`) and `test_direct_solve.py` parent-context cases.

## Not yet true

- No second rule, so reuse of the engine is unproven (H-01).
- No second language adapter (H-02) and cost has never chosen between rules (H-03).
