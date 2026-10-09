# Shared engine — what exists today

Verified on 2026-10-06 on top of `db4fc9d4f3` with S1 and A1 registered (working tree, not yet committed):
`DECIDB_TEST_DIRECT_SOLVE=off make decide-test` 2,257 passed and 6 expected failures (the strict-bound cases in
`../a1/done.md`); `make decide-test` and the serializer run the same, plus seven existing tests that assert something only
the solver path produces (`../a1/todo.md`, A1-01); the forced-HiGHS run has one failure, `test_norm_combined_l1_l2_objective`,
which needs Gurobi; C++ `[decidb]` 1,227 assertions in 39 cases. Design: `../../architecture/`.

## What it does

- **Setting and fallback.** `decide_direct_solve` is `auto` (default), `off` or `require`. A hit replaces the DECIDE node
  before solver selection; a miss leaves it for the solver. `DIAGNOSE` and a forced backend use the solver and conflict
  with `require`. A prepared plan keeps its selection until rebound. `require` also fails when the DECIDE optimizer is off.
- **Facts.** One read-only adapter turns the bound tree into facts: decision domains and scopes, each constraint with
  reducer, filter, qualifier, factor, `PER`/`WHEN`, right side, and the objective. Unmodeled parts are *unknown* with a
  reason, and reading the facts never throws: `norm(e, 0)` (it needs indicator variables) and any norm tag the reader does
  not expect are unknown parts. No fact is kept that no rule reads. It uses the same term splitter as the solver path.
- **Rule contract.** Match, Prove, Cost, Explain, Rewrite, plus a shared Map step that checks the output slots. Rules are
  listed in `direct_registry.cpp`; tests can replace the list per connection. The cheapest proved rule wins, ties go to
  the first registered. `Cost` sees the estimated source row count without disturbing cached estimates.
- **Result boundary.** Keeps original bindings and types, serializes, prunes outputs only when skipping is proved safe,
  and always keeps the rule's validation slots live. `DirectValidationBarrier` builds the all-rows read for a plan that
  could stream.
- **Shared builders.** `direct_builder.hpp` lists only helpers with two or more users: `DirectGuardEmptyAggregate`,
  `DirectActiveWhen`, `DirectWindowMatchingCount`, `DirectValidationBarrier`, `DirectScaledTerm` (sign and constant
  factor of a coefficient), `DirectIsNull`, `DirectInfeasibleErrorPredicate`, `DirectUnboundedErrorPredicate`,
  `DirectValidBoundPredicate` and `DirectValidNumberPredicate`. Which of several errors a query reports is not
  specified. The helpers only S1 uses (the scope projection `DirectProjectScope`, the bound validation
  `DirectValidateBounds`, `DirectWindowExtremum`, `DirectAnyCondition`) live in `src/optimizer/decide/direct/s1_support.{hpp,cpp}`
  in `namespace direct_s1`, and move to the builders when a second rule needs them. `DIRECT_MAX_EXACT_INTEGER` (2^53) in
  `direct_expression.hpp` is the one copy of the exactness limit, used by S1 and A1.
- **Shared admission predicates.** `DirectIsNumericDecisionFree`, `DirectSourceNumericColumn`,
  `DirectIsSourceOnlyNumeric`, `DirectIsSourceOnlyValue` (which admits a value that can raise), `DirectPartScale`,
  `DirectFoldableDouble` and `DirectConstraintFact::PlainSum` answer "may a rule use this value" once for all rules.
- **Number messages.** A NULL number names the NULL source columns on the failing row, in the solver's wordings: one
  column (`column "a" is NULL. Impute it with COALESCE(a, 0)...`), several (`columns "a", "b" and "c" are NULL`), or,
  when no column is NULL, the score's own text (`TRY_CAST(s AS DOUBLE) is NULL`). The message is a per-row expression
  (`DirectValidNumberPredicate` in `direct_builder.cpp`) that `error()` evaluates only on the failing row. A multi-term score
  with no NULL column stays generic, since the solver quotes one failing term and direct sums the terms. A NaN or infinite
  number raises the solver's fixed message, whose first words name the clause the user wrote it in: `objective
  coefficient` or `constraint coefficient`, from the `DirectNumberKind` the rule passes (S1 and A1's objective values:
  objective; A1's coefficient on the left of a bound: constraint). Tests:
  `test_computed_score_null_error_names_the_columns`, `test_null_score_no_column_explains_quotes_the_score`,
  `test_null_score_message_is_not_built_when_no_row_is_null` in `test_direct_user_facing.py`. The cost on clean data is not
  measured.
- **Decision record.** On a hit, mode, rule and one fixed proof line per rule appear in logical and physical `EXPLAIN` and
  profiling: S1 `min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi`, A1 `min/max SUM(c*x), l <= x <= u, each row
  independent`. There is no guards line and no number from the query. `require` explains a miss. Nothing persists a
  serialized plan across versions, so `DIRECT_RESULT_EXTENSION` did not change (the removed property id 210 stays a gap).
  `test_serializer_explain_and_model_dump` checks the rule and the proof line in the logical, physical and profiled
  plans. A
  miss under `auto` prints nothing.

- **Cost estimate (H-09, closed 2026-10-09, no code change).** The coordinator builds the cost context (a cardinality
  estimate of the source plan) on the first proved rule, although both rules return `0.0` from `Cost`. Measured on `EXPLAIN`
  of an A1 query over a six-way join of 100,000-row tables, 400 runs per mode: `decide_direct_solve='off'` 1.005 ms,
  `'auto'` 1.470 ms. Direct solve adds about 0.47 ms of planning in total, which covers rule matching, the proof and the
  estimate together, so the estimate costs at most that. That is under the task's threshold of about 1 ms, so the estimate
  stays eager. One query shape; the estimate grows with the number of operators in the source plan. H-03 is unaffected and
  stays unscheduled.

## Verified by

- C++: `test_decidb_direct_facts.cpp` (every fact kind and unknowns, and a norm tag it does not expect reading as an
  unknown part, not an exception), `test_decidb_direct_coordinator.cpp` (stub rules: hit,
  fallback with every reason, cheaper rule, tie, cost context, non-finite cost, mismatched slot, barrier under
  `LIMIT 1` and `COUNT(*)`).
- Python: `test_direct_rule_contract.py` (one `RuleFixture` per rule, S1's and A1's: schema, all-rows read, serializer, prepared
  plans, near misses, `off`, forced backend, `DIAGNOSE`, an empty source, and constant bounds that contradict the decision's
  built-in domain: under `auto`, `off` and `require` each gives the solver's outcome, or `require` declines) and the
  parent-context rows of `test_direct_three_way.py`. Both fuzz generators in `test_direct_fuzz.py` run about one query in ten
  over an empty source and add an impossible constant bound now and then; with A1's domain check disabled, the contract test
  and the A1 fuzz fail. `test_direct_user_facing.py` checks which noun a NaN or infinite coefficient names.

## What the second rule needed (H-01, closed 2026-10-06)

A1 registered with one line in `direct_registry.cpp` and one in the folder's `CMakeLists.txt`. The coordinator
(`direct_coordinator.cpp`) and the result boundary (`direct_result_boundary.cpp`) did not change. What did change is the
shared layer, as `architecture/rules.md` prescribes when a second rule needs a helper:

- Moved out of `s1_rule.cpp` into the builders and predicates, with S1's behavior unchanged and its suites green after each
  step: `DirectPartScale`, `DirectScaledTerm`, `DirectInfeasibleErrorPredicate`, `DirectActiveWhen`,
  `DirectFoldableDouble`, and a `clause` word on `DirectGuardEmptyAggregate`.
- New for per-row rules: `DirectUnboundedErrorPredicate`, `DirectValidBoundPredicate`, `DirectIsSourceOnlyValue`.
- One new fact, `DirectConstraintFact::variable_bound`, from a predicate extracted out of the solver's own bound absorption
  (`DecideBoundIsAbsorbed`, `decide_bound_absorption.cpp`), so a rule can ask which bounds replace a variable's default
  box without copying the solver's test. The solver's behavior is unchanged.
- `DirectValidationBarrier` has its first production use, in A1; before this only a stub test had run it.

The engine took a materially different rule (new decision types, new output types, a quadratic objective, no aggregate
constraint, a streaming plan) without a change to its two central files.

## Not yet true

- No second language adapter (H-02), and cost has never chosen between two real rules (H-03): S1 and A1 are disjoint.
