# Shared engine — what exists today

Verified on 2026-10-12 on top of `4f3040f016`, with S1 and A1 registered: `make decide-test`,
`DECIDB_TEST_DIRECT_SOLVE=off make decide-test` and `DECIDB_VERIFY_SERIALIZER=1 make decide-test` each 2,538 passed (no
failures, no expected failures); `DECIDB_FORCE_SOLVER=highs make decide-test` 2,537 passed and one failure,
`test_norm_combined_l1_l2_objective`, which needs Gurobi; C++ `build/release/test/unittest "[decidb]"` 1,287 assertions in
45 cases. `test/decide/run_direct_matrix.sh` renews these numbers in one command (about 3 minutes; it prints one line per
check). Design: `../../architecture/`.

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
  could stream. Because every row is checked before any is released, rows are held in memory, wide parent-read columns
  included (`../../architecture/result_boundary.md`, "Memory of holding rows").
- **Shared builders.** `direct_builder.hpp` lists only helpers with two or more users: `DirectGuardEmptyAggregate`,
  `DirectActiveWhen`, `DirectWindowMatchingCount`, `DirectValidationBarrier`, `DirectScaledTerm` (sign and constant
  factor of a coefficient), `DirectIsNull`, `DirectInfeasibleErrorPredicate`, `DirectUnboundedErrorPredicate`,
  `DirectValidBoundPredicate` and `DirectValidNumberPredicate`. Which of several errors a query reports is not
  specified. The helpers only S1 uses (the scope projection `DirectProjectScope`, the bound validation
  `DirectValidateBounds`, `DirectWindowExtremum`, `DirectAnyCondition`) live in `src/optimizer/decide/direct/s1_support.{hpp,cpp}`
  in `namespace direct_s1`, and move to the builders when a second rule needs them. The exactness limit (2^53) is
  `DECIDE_MAX_EXACT_INTEGER` in the numeric-policy header (next bullet).
- **Numeric policy.** `src/include/duckdb/common/decide_numeric_policy.hpp` is the one home of the numeric facts that
  DuckDB's own evaluation cannot supply. It includes only `<cmath>`, so the planner, the optimizer and direct-solve
  rules, the model builder and the solver backends all read it without including each other. It names the solver's
  sentinel `DECIDE_SOLVER_INFINITY` (1e30: the builder's "no bound", the unset side of an absorbed bound, a one-sided
  row), the reading margin `DECIDE_EFFECTIVE_INFINITY` (1e20) with `DecideIsOpenAbove` and `DecideIsOpenBelow`,
  `DECIDE_MAX_EXACT_INTEGER` (2^53) with `DecideIsWithinExactRange`, `DECIDE_MAX_FRACTIONAL_MAGNITUDE` (2^52),
  `DECIDE_FIRST_DOUBLE_BEYOND_BIGINT` (2^63), and `DecideIsExactFiniteDouble` (a converted double is exact when it is
  finite and converts back to the original value), and `DecideStrictBound` (`x > 0.5` means `x >= 1`: the solver's
  absorption, its model builder's linear and quadratic rows, A1 and S1 all shift a strict bound with it, so none of them
  reads a strict comparison differently). `LogicalDecide::ABSORBED_*_UNSET`
  are the sentinel under the names the planner uses. An integer answer is returnable when, rounded half away from zero, it
  lies within the column's range (`DecideRoundedAnswerWithin`); beyond it both paths raise one message,
  `DecideWholeNumberRangeMessage`, which names the variable and its rounded value. A1 spells the test in SQL once
  (`WithinExactRange`, `abs(round(answer)) <= 2^53`) and builds the message at run time from its two halves around
  `printf('%.0f', round(answer))`, so the text matches the solver's character for character.
  What stays outside the header on purpose: bare `std::isfinite` and `std::isinf` that only reject NaN or infinity in
  input, a report or a diagnostic (they state no exactness policy); HiGHS's own infinite bound and large-matrix limits in
  `deterministic_naive.cpp`; Gurobi's 1e100; `INTEGER_SOLVER_CEILING` and the ray epsilon in `diagnostic_constants.hpp`
  (diagnostic-only thresholds). No hand-written range proof of DuckDB's overflow is left; the copies of DuckDB's absorption and
  numeric logic that remain are tasks in `todo.md`. `DirectCastMayThrow` and `DirectMayThrow` copy DuckDB's `CanThrow`
  on purpose: DuckDB's counts `TRY_CAST` as throwing, and fixing that would change DuckDB itself. Test: `test/common/test_decidb_numeric_policy.cpp`
  (`[numeric_policy]`, 5 cases).
- **Shared admission predicates.** `DirectIsNumericDecisionFree`, `DirectSourceNumericColumn`,
  `DirectIsSourceOnlyNumeric`, `DirectIsSourceOnlyValue` (which admits a value that can raise), `DirectPartScale`,
  `DIRECT_CONSTANT`, `DIRECT_FINITE_CONSTANT` and `DirectConstraintFact::PlainSum` answer "may a rule use this value"
  once for all rules.
- **One evaluate-to-DOUBLE function.** `TryEvaluateFoldableDouble(context, expr, out, flags)` (`decide_cast_policy.hpp`)
  is the only code that folds a constant expression to a `DOUBLE`, for the planner, the rewrites and the direct rules.
  Callers differ only in the flags they pass, each one difference stated by name: `FOLD_STABLE` (no row, parameter or
  subquery can change it), `FOLD_ALLOW_BOOLEAN`, `FOLD_EXACT_FINITE` (through `DecideIsExactFiniteDouble`),
  `FOLD_CATCH_DUCKDB_ERRORS` and `FOLD_CATCH_ALL`. The planner passes `FOLD_PLAIN` and lets a conversion error reach its
  own error policy; the sign test for a scale factor and the `IN` rewrite catch everything; direct rules pass
  `DIRECT_CONSTANT` or `DIRECT_FINITE_CONSTANT`. One function means one path cannot accept a constant another rejects
  without a flag that says so.
- **Number messages.** A NULL number names the NULL source columns on the failing row, in the solver's wordings: one
  column (`column "a" is NULL. Impute it with COALESCE(a, 0)...`), several (`columns "a", "b" and "c" are NULL`), or,
  when no column is NULL, the score's own text (`TRY_CAST(s AS DOUBLE) is NULL`). The message is a per-row expression
  (`DirectValidNumberPredicate` in `direct_builder.cpp`) that `error()` evaluates only on the failing row. A multi-term score
  with no NULL column stays generic, since the solver quotes one failing term and direct sums the terms. A NaN or infinite
  number raises the solver's fixed message, whose first words name the clause the user wrote it in: `objective
  coefficient` or `constraint coefficient`, from the `DirectNumberKind` the rule passes (S1 and A1's objective values:
  objective; A1's coefficient on the left of a bound: constraint). A source value read as a coefficient must be finite;
  one read as a bound (a count limit, a pin, a per-row lower or upper bound) only has to be a number, because an
  infinite bound means "no bound" or "nothing satisfies it". NaN and NULL raise in every slot. Direct applies this in SQL
  (`DirectValidNumberPredicate`, `DirectValidBoundPredicate`) and the solver in C++ (`ExtractDoubleColumn`); the two
  cannot share code, so `test_direct_source_values.py` puts NaN, both infinities and NULL in each of 27 slots S1 and A1
  read and asserts the same error or rows on `require`, `off` and forced HiGHS. Tests:
  `test_computed_score_null_error_names_the_columns`, `test_null_score_no_column_explains_quotes_the_score`,
  `test_null_score_message_is_not_built_when_no_row_is_null` in `test_direct_user_facing.py`. The cost on clean data is not
  measured.
- **Decision record.** On a hit, mode, rule and one fixed proof line per rule appear in logical and physical `EXPLAIN` and
  profiling: S1 `min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi`, A1 `min/max SUM(c*x), l <= x <= u, each row
  independent`. There is no guards line and no number from the query. `require` explains a miss; a miss under `auto`
  prints nothing. Nothing persists a serialized plan across versions, so `DIRECT_RESULT_EXTENSION` is unchanged.
  `test_serializer_explain_and_model_dump` checks the rule and the proof line in the logical, physical and profiled plans.
- **The solver's domain check, once.** The solver raises, before it reads any row, for a decision whose box
  contradicts its built-in domain (an upper bound below the default floor of 0, or a `BOOL` lower bound above 1). That test
  is `DecideBoxContradictsDomain(lower, upper, is_bool)` in `decide_bound_absorption.{hpp,cpp}`.
  The solver's pre-check (`DecideGlobalSinkState`), A1 (`ProveBounds`) and S1 (`ConstantPinDomain`) call it. The starting
  box is `LogicalDecide::ABSORBED_LOWER_UNSET` and `ABSORBED_UPPER_UNSET`, defined once; the solver, the absorption pass and
  A1 read them there. Direct solve still only declines these queries and lets the solver raise the error. S1 adds one
  condition of its own, a negative lower bound (it makes `x` signed on the solver path).
- **One test scaffold.** `test/decide/tests/_direct_oracle_common.py` holds what the oracle suites share: the autouse
  fixture that lifts a forced backend, `run`, `outcome`, `values_clause`, and `agree_with_oracle` (oracle model, `off`, `require`,
  then a rule-supplied check of one assignment). `test_direct_three_way.py` (S1) and `test_direct_a1.py` keep their case
  tables, oracle-model builders and assignment checks. Every direct suite imports the autouse fixture by name from the
  scaffold (pytest applies an autouse fixture only in a file that can see it), and none defines its own. A new rule's
  suite imports the scaffold (`architecture/rules.md`, item 2).
- **Matrix script.** `test/decide/run_direct_matrix.sh` runs the five checks and prints one summary line each, for
  example `auto: 2322 passed, 0 xfailed, 0 failed`. The comment above `DirectDomain` in `direct_rule.hpp` points at
  `architecture/facts.md`.
- **Cost estimate.** The coordinator builds the cost context (a cardinality estimate of the source plan) on the first
  proved rule, although both rules return `0.0` from `Cost`. Measured on `EXPLAIN` of an A1 query over a six-way join of
  100,000-row tables, 400 runs per mode: `decide_direct_solve='off'` 1.005 ms, `'auto'` 1.470 ms. Direct solve adds about
  0.47 ms of planning in total, which covers rule matching, the proof and the estimate together, so the estimate costs at
  most that. One query shape; the estimate grows with the number of operators in the source plan.

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

## Registering a rule

A rule registers with one line in `direct_registry.cpp` and one in the folder's `CMakeLists.txt`. The coordinator
(`direct_coordinator.cpp`) and the result boundary (`direct_result_boundary.cpp`) hold no rule-specific code. S1 and A1 are
materially different rules (decision types, output types, a quadratic objective, aggregate constraints or none, a barrier
or a streaming plan) and run on the same two central files. A fact a rule needs about the solver's own reading comes from
the solver's code, not a copy: `DirectConstraintFact::variable_bound` is the predicate `DecideBoundIsAbsorbed`
(`decide_bound_absorption.cpp`), so a rule can ask which bounds replace a variable's default box.

## Checked and fine

- **Result boundary** (`direct_result_boundary.cpp`): the extension operator, serialization and pruning hook keep DECIDE's
  bindings. These parent contexts give identical results under `auto`, `off` and `require`: `CREATE TABLE AS`,
  `INSERT ... SELECT`, a view, a window over the result, `UNION ALL` of an S1 and an A1 query, a joined source with a filter
  above, a self-join of results, an empty source and a prepared statement with a parameter.
- **Test-only hook:** `DirectRuleOverride` lets coordinator tests install stub rules.
- **A1 plan size:** each bound clause makes a large expression, but it scales cheaply. At 1,000,000 rows: 1 clause 0.08 s,
  3 clauses 0.13 s, 8 clauses with `WHEN` 0.26 s, 3 decisions and 8 clauses 0.29 s.

## Not built

- No second language adapter, and cost has never chosen between two real rules: S1 and A1 never prove the same query.
