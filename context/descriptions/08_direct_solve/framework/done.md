# Shared engine — what exists today

Verified on 2026-10-21 on top of `9eb61e407e`, with the right-hand-side rewrite and S1 and A1 registered: `make
decide-test`, `DECIDB_TEST_DIRECT_SOLVE=off make decide-test` and `DECIDB_VERIFY_SERIALIZER=1 make decide-test` each 3,110
passed (no failures, no expected failures); `DECIDB_FORCE_SOLVER=highs make decide-test` 3,109 passed and one failure,
`test_norm_combined_l1_l2_objective`, which needs Gurobi; C++ `build/release/test/unittest "[decidb]"` 1,847 assertions in
55 cases. `test/decide/run_direct_matrix.sh` renews these numbers in one command (about 3 minutes; it prints one line per
check). Design: `../architecture/`.

## What it does

- **Setting and fallback.** `decide_direct_solve` is `auto` (default), `off` or `require`. When a rule applies, its
  substitute replaces the DECIDE node before solver selection; otherwise the node is left for the solver. `DIAGNOSE` and
  a forced backend use the solver and conflict with `require`. A prepared plan keeps its selection until rebound.
  `require` also fails when the DECIDE optimizer is off.
- **IR.** One read-only front end turns the bound tree into the IR: decision domains and scopes, each constraint with
  aggregate, filter, qualifier, factor, `PER`/`WHEN`, right side, and the objective. Parts the binder admits and no rule
  models are *not proved*, and building the IR never throws: `norm(e, 0)` (it needs indicator variables) and any norm
  tag the front end does not expect are not-proved parts. Shapes the binder never produces are `D_ASSERT`s
  (`../architecture/ir.md`). No IR node is kept that no rule reads. It uses the same term splitter as the solver path.
- **Rule interface.** `CheckCondition`, `Apply` and a fixed `Summary()` line for `EXPLAIN`, plus a shared Map step that
  checks the slot map. Rules are listed in `direct_registry.cpp`; a test seam replaces the list per connection. The
  rule driver first checks that the front end read the whole IR (`DirectIRIsReadable`: decisions, source, every
  constraint and the objective proved, a finite offset, a known sense); an IR that fails is offered to no rule, so each
  rule checks only what concerns its own class. Otherwise the driver runs CheckCondition for every rule. Rules are disjoint, so at most one applies to a query; two that apply
  raise an internal error naming both rules.
- **Result adapter** (`DIRECT_RESULT_ADAPTER` in `EXPLAIN`). Keeps original bindings and types, serializes, prunes
  outputs only when skipping is proved safe, and always keeps the rule's validated columns live. What a rule hands it
  (a child, a slot map that fits the DECIDE outputs, the validated columns and the source row id) is checked in release
  builds and a misfit raises an internal error, because rules are plug-ins and a broken one must fail loudly. Checks on
  plans the engine builds itself, in the adapter and in late materialization, are `D_ASSERT`s, which debug builds run.
  `DirectValidationBreaker` builds the all-rows read for a plan that could stream. Because every row is checked before
  any is released, rows are held in memory (`../architecture/result_adapter.md`, "Memory of holding rows").
- **Late materialization of a stored table** (`direct_late_materialization.cpp`). The rule driver appends the table's
  `rowid` to a single-base-table source, and after unused-column removal a late step moves each parent-read
  variable-width column that the rule's plan only forwards to a second scan joined on `rowid`. Fixed-width columns,
  joins, aggregates, volatile or computed columns, a source that already reads `rowid`, and a pipelined plan
  (`DirectSubstitute::pipelined`, set by A1's constant problem) keep the single read. Rules and measurements:
  `../architecture/result_adapter.md`.
- **Plan fragments.** `direct_plan_fragments.hpp` holds free functions that each return one plan fragment (the function
  form of a factory method). It lists only helpers with two or more users: `DirectGuardEmptyAggregate`,
  `DirectActiveWhen`, `DirectWindowMatchingCount`, `DirectValidationBreaker`, `DirectScaledTerm` (sign and constant factor
  of a coefficient) and `DirectIsNull`.
- **Runtime checks.** `direct_runtime_checks.hpp` holds the predicates that raise an error while the plan runs, and
  their messages: `DirectInfeasibleErrorPredicate`, `DirectUnboundedErrorPredicate`, `DirectValidBoundPredicate`,
  `DirectValidNumberPredicate` and `DirectNullMessageOf`. Every check that raises is built with `DirectRaiseUnless(ok,
  error)` or `DirectRaiseIf(bad, error)`, which return true or raise, and a list of checks is joined by `DirectAllHold`, a
  `CASE` chain in the order given, so an earlier check (a NULL test) guards the later ones. Which of several errors a
  query reports is not specified.
- **Per-row bound reader** (`direct_bound_reader.{hpp,cpp}`; A1 is its only user until R1 is built on it). It makes sure
  two rules cannot read a bound differently. Its header defines `POSITIVE_INFINITY` and `NEGATIVE_INFINITY` once for
  every direct-solve file. `DirectReadBounds` folds `variable_bound`s into each decision's static box
  (default `[0, inf)`, a BOOL kept in `{0, 1}`, ends of magnitude 1e20 or more infinite) and lists every other bound as a
  per-row clause, an equality split into its two halves. `DirectClauseBounds` turns one clause's row values into a lower
  and an upper bound as the solver reads them. `DirectRowBox` intersects them with the box. `DirectReadsOneDecision` is the shape a per-row bound must have to be read
  (one unfiltered linear left side in one decision, no constant), and `DirectBoxIsEmpty` is true on a row whose box no
  value satisfies. `DirectReadLinearObjective` reads one objective part into a `DirectLinearObjectivePart`: its sign and
  constant factor, and each linear term's decision, sign and proved coefficient; it skips other term kinds. Each rule keeps
  its own policy around it: S1 declines a filtered part, a non-linear term, a second decision and a coefficient that may
  raise beside other terms, and flattens the part per term; A1 reads constants and squares itself and sums a part's
  linear terms per decision before applying the part's sign and scale.
- **`ExpressionBuilder`** (`direct_expression.hpp`; Fowler's Expression Builder) is a small fluent helper over bound
  expressions that passes them as `ExprPtr`. A1 uses it; S1 builds expressions by hand.
- **Rule-local helpers.** The helpers only S1 uses (`ProjectScope`, `ValidateBounds`, `WindowExtremum`, `AnyCondition`)
  live in `s1_uniform_matroid_helpers.{hpp,cpp}` in `namespace direct_s1_uniform_matroid`, and move to the shared
  files when a second rule needs them. Inside a rule's namespace, names drop the `Direct` and class prefixes.
- **Numeric policy.** `src/include/duckdb/common/decide_numeric_policy.hpp` is the one home of the numeric facts that
  DuckDB's own evaluation cannot supply. It includes only `<cmath>`, so the planner, the optimizer and direct-solve
  rules, the model builder and the solver backends all read it without including each other. It holds:
  - `DECIDE_SOLVER_INFINITY` (1e30), the solver's sentinel: the builder's "no bound", the unset side of a
    singleton-row bound, a one-sided row.
  - `DECIDE_EFFECTIVE_INFINITY` (1e20), the reading margin, with `DecideIsOpenAbove` and `DecideIsOpenBelow`.
  - `DECIDE_MAX_EXACT_INTEGER` (2^53) with `DecideIsWithinExactRange`; `DECIDE_MAX_FRACTIONAL_MAGNITUDE` (2^52);
    `DECIDE_FIRST_DOUBLE_BEYOND_BIGINT` (2^63).
  - `DecideIsExactFiniteDouble`: a converted double is exact when it is finite and converts back to the original value.
  - `DecideStrictBound`: `x > 0.5` means `x >= 1`. The solver's folding, its model builder's linear and quadratic rows,
    A1 and S1 all shift a strict bound with it, so none reads a strict comparison differently.
  - `DecideRoundedAnswerWithin`: an integer answer is returnable when, rounded half away from zero, it lies within the
    column's range. Beyond it both paths raise one message, `DecideWholeNumberRangeMessage`, which names the variable and
    its rounded value. A1 spells the test in SQL once (`WithinExactRange`, `abs(round(answer)) <= 2^53`) and builds the
    message at run time around `printf('%.0f', round(answer))`, so the text matches the solver's character for character.
  - `DecideBoundBox`, the box described below.

  Outside the header on purpose: bare `std::isfinite` and `std::isinf` that only reject NaN or infinity in input, a
  report or a diagnostic; HiGHS's own infinite bound and large-matrix limits in `deterministic_naive.cpp`; Gurobi's 1e100;
  `INTEGER_SOLVER_CEILING` and the ray epsilon in `diagnostic_constants.hpp` (diagnostic-only thresholds).
  `DirectCastMayThrow` and `DirectMayThrow` copy DuckDB's `CanThrow` on purpose: DuckDB's counts `TRY_CAST` as throwing,
  and fixing that would change DuckDB itself. Test: `test/common/test_decidb_numeric_policy.cpp` (`[numeric_policy]`, 5
  cases).
- **Shared admission predicates.** `DirectIsDecisionFreeDeterministic`, `DirectSourceNumericColumn`,
  `DirectIsSourceOnlyNumeric`, `DirectIsSourceOnlyValue` (which admits a value that can raise), `DirectPartScale`,
  `DIRECT_CONSTANT`, `DIRECT_FINITE_CONSTANT` and `DirectIRConstraint::PlainSum` answer "may a rule use this value"
  once for all rules.
- **One evaluate-to-DOUBLE function.** `TryEvaluateFoldableDouble(context, expr, out, flags)` (`decide_cast_policy.hpp`)
  is the only code that folds a constant expression to a `DOUBLE`, for the planner, the rewrites and the direct rules.
  Callers differ only in the flags they pass: `FOLD_STABLE` (no row, parameter or subquery can change it),
  `FOLD_ALLOW_BOOLEAN`, `FOLD_EXACT_FINITE` (through `DecideIsExactFiniteDouble`), `FOLD_CATCH_DUCKDB_ERRORS` and
  `FOLD_CATCH_ALL`. The planner passes `FOLD_PLAIN` and lets a conversion error reach its own error policy; the sign test
  for a scale factor and the `IN` rewrite catch everything; direct rules pass `DIRECT_CONSTANT` or
  `DIRECT_FINITE_CONSTANT`.
- **Right-hand-side aggregates are window columns** (`decide_rewrite_rhs_window.cpp`, called by
  `DecideOptimizer::Optimize` before the direct rules, in every mode). In a reduced constraint, a `COUNT`, `MIN`, `MAX`,
  `SUM` or `AVG` of one numeric source expression on the right side, with no relation qualifier of its own, becomes a
  column of the DECIDE source: the same DuckDB aggregate as a window over the clause's `PER` keys and covered rows. A
  reducer's own `WHEN` (`SUM(x) <= SUM(cap) WHEN active`) becomes the window's `FILTER`, so it scopes the reducer apart
  from the clause; a group the `WHEN` empties raises the solver's empty-row-set error (`DecideEmptyRowSetMessage` in
  `common/enums/decide.hpp`, the one text the fold and the direct-solve plans raise), `COUNT` included, and only on a
  group the solver evaluates. A `COUNT(*)` with its own `WHEN` counts a constant that carries that error. A `WHEN` on
  both the clause and the reducer cannot be written: the binder rejects it. The binder rejects `DISTINCT` and drops an
  `ORDER BY` inside these reducers, so neither reaches the rewrite. The solver and the direct rules read DuckDB's value,
  so `SUM(x) <= SUM(cap)` over the `BIGINT` values 2^60, 1, -2^60 reads the limit 1 on `off` and `require`; the solver's
  row-by-row `DOUBLE` fold reads 0. Arithmetic around the aggregate (`COUNT(*) / 2`) runs in DuckDB's types. A row the
  clause does not cover reads 0, so no overflow raises there, and a group where no row has a left-hand reducer active
  reads 0 because the solver never evaluates it. A NULL or NaN argument raises on every row, as the solver does: the
  NULL text is built by `DirectNullMessageOf` and names the argument's NULL columns; the NaN text lacks the solver's `
  at row N`. The solver's C++ fold (`EvaluateRhsReducerPerGroup`) still reads relation-qualified reducers, a reducer
  whose own `WHEN` the window cannot read over the source (a volatile one such as `random() < 2`), a written norm, a
  non-numeric argument and a clause whose left side holds no decision reducer. The window sits directly above the DECIDE
  source, so it appends columns and renumbers none (the physical plan's column naming reads through it). It ends late
  materialization for an S1 source with an aggregate limit, and a window over `PER` keys or a `WHEN` regroups rows, so
  output row order and the choice among equal optima are not guaranteed. Test: `test_rhs_aggregate_window.py`.
- **Number messages.** The NULL and NaN texts have one author for both paths, `decide_value_messages.hpp`
  (`DecideNullValueMessage`, `DecideInvalidValueMessage`, and `DecideCollectNamedColumns`, which decides which columns
  of a value are named).
  - A NULL number names the NULL source columns on the failing row: one column (`column "a" is NULL. Impute it with
    COALESCE(a, 0)...`), several (`columns "a", "b" and "c" are NULL`), or, when no column is NULL, the value's own text
    (`TRY_CAST(s AS DOUBLE) is NULL`). The value is the coefficient as the solver evaluates it: terms of one decision are
    merged by `DecideMergeLikeCoefficients`, which the solver's like-term collection also calls, so `SUM(a*x + b*x)`
    names `a` and `b` together.
  - In direct solve the message is a per-row expression (`DirectNullMessageOf`, used by `DirectValidNumberPredicate` and
    `DirectValidBoundPredicate`) that `error()` evaluates only on the failing row. A value's source columns are read from
    the stage above the one that carries the source (`DirectOverProjection`).
  - An S1 count bound or fixed value that is exactly one column raises the same fixed NULL and NaN messages; a computed
    one keeps `DirectInvalidBoundMessage`.
  - A NaN or infinite number raises the solver's message, whose first words name the clause: `objective coefficient`,
    `constraint coefficient` or `constraint right-hand side`, without the solver's ` at row N`. The rule passes the
    `DirectNumberKind` (S1 and A1's objective values: objective; A1's coefficient on the left of a bound: constraint).
  - A source value read as a coefficient must be finite; one read as a bound (a count limit, a fixed value, a per-row
    lower or upper bound) only has to be a number, because an infinite bound means "no bound" or "nothing satisfies it".
    NaN and NULL raise in every slot.
  - Direct applies this in SQL and the solver in C++ (`ExtractDoubleColumn`); the two cannot share code, so
    `test_direct_source_values.py` puts NaN, both infinities and NULL in each of 27 slots S1 and A1 read and asserts the
    same error or rows on `require`, `off` and forced HiGHS. Message tests are in `test_direct_user_facing.py`
    (`test_null_value_error_is_the_solvers_error_word_for_word` and
    `test_invalid_value_error_is_the_solvers_error_without_the_row_position` compare the full text of `require` with
    `off` for S1 and A1). The cost on clean data is not measured.
- **`EXPLAIN` annotation.** When a rule applies, mode, rule and one fixed condition line per rule appear in logical and
  physical `EXPLAIN` and profiling: S1 `min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi`, A1 `min/max SUM(c*x), l <= x
  <= u, each row independent`. There is no number from the query. `require` explains why no rule applied; under `auto`
  nothing is printed. `test_serializer_explain_and_model_dump` checks the rule and the condition line in the logical,
  physical and profiled plans.
- **The solver's domain check, once.** The solver raises, before it reads any row, for a decision whose box contradicts
  its built-in domain: an upper bound below the default floor of 0, any upper bound below 0 on a `BOOL` (its floor stays
  0 whatever lower bound is written), or a `BOOL` lower bound above 1. That test is `DecideBoxContradictsDomain(box,
  is_bool)` in `decide_singleton_row_folding.{hpp,cpp}`. For a non-negative `INT` or `REAL` the error quotes the clause;
  for a `BOOL` it is DECIDE's usual infeasible error, and under `DIAGNOSE` the check skips a `BOOL` so the elastic
  engine names the clause. The solver's pre-check (`DecideGlobalSinkState`), A1 (`ProveBounds`) and S1
  (`ConstantFixedDomain`) call it. Direct solve only declines these queries and lets the solver raise the error.
- **One box, one way to combine bounds.** A decision's unconditional bounds fold into a `DecideBoundBox`
  (`decide_numeric_policy.hpp`) through `DecideFoldSingletonRow`, which keeps the tightest value on each side. The
  solver's folding pass, the elastic engine's retained box, A1 (`ProveBounds`) and S1 (`ConstantFixedDomain`) all call
  it. The box keeps a `lower_written` flag apart from the value: a written lower bound of -1e30 or below is no bound,
  like any bound of magnitude 1e20 or more, and only a decision with no lower bound written takes the default floor of 0
  (`EffectiveLower`). No value is reserved to mean "unset".
- **One test scaffold.** `test/decide/tests/_direct_oracle_common.py` holds what the oracle suites share: the autouse
  fixture that lifts a forced backend, `run`, `outcome`, `values_clause`, `sql_literal` (one spelling of a constant: a `CAST` to the column's type, so a
  column's type does not depend on which row comes first, with infinities and NaN spelled as strings), and
  `agree_with_oracle` (oracle model, `off`,
  `require`, then a rule-supplied check of one assignment). `test_direct_s1_uniform_matroid.py` and
  `test_direct_a1_separable_box.py` keep their case tables, oracle-model builders and assignment checks. Every direct
  suite imports the autouse fixture by name from the scaffold (pytest applies an autouse fixture only in a file that can
  see it). A new rule's suite imports the scaffold (`architecture/rules.md`, item 2).
- **Matrix script.** `test/decide/run_direct_matrix.sh` runs the five checks and prints one summary line each, for
  example `auto: 2322 passed, 0 xfailed, 0 failed`. The golden capture (`test/decide/golden/capture.sh`, also run by `check.sh`) is single-threaded: a window over a `PER` key emits rows in a thread-dependent order, and the model dump follows the order the DECIDE operator reads.

## Registering a rule

A rule registers with one line in `direct_registry.cpp` and one in the folder's `CMakeLists.txt`. The rule driver
(`direct_rule_driver.cpp`) and the result adapter (`direct_result_adapter.cpp`) hold no rule-specific code. S1 and A1
are materially different rules (decision types, output types, a quadratic objective, aggregate constraints or none, a
breaker or a streaming plan) and run on the same two central files. An IR flag a rule needs about the solver's own
reading comes from the solver's code, not a copy: `DirectIRConstraint::variable_bound` is the predicate
`DecideIsSingletonRowBound` (`decide_singleton_row_folding.cpp`), so a rule can ask which bounds replace a variable's
default box.

## Verified by

- C++: `test_decidb_direct_ir.cpp` (every IR node kind and not-proved parts: an `OR` inside an `AND`, a per-row term the
  splitter cannot read, an order-0 norm in a reducer, and a norm tag it does not expect, each as a not-proved part, not
  an exception), `test_decidb_direct_rule_driver.cpp` (stub rules: one rule applying, fallback
  when no rule applies, a rule that applies beside one that does not, two rules applying as an internal error, an IR the
  front end could not read offered to no rule, mismatched slot, a shared plan fragment handed an input that does not
  fit, each way a substitute can misfit the adapter (no child, a slot too many, a missing prunable flag, a slot,
  validated column or row id slot beyond the child), breaker under `LIMIT 1` and `COUNT(*)`),
  `test_decidb_direct_decline.cpp` (each decline branch of S1's and A1's `CheckCondition` and of `DirectIRIsReadable`,
  tested on the IR: a probe rule installed through the rule driver's test seam receives the IR the driver reads from a
  small query that is only planned, edits it for the few shapes SQL cannot write, and asks S1 or A1 whether it
  applies; each case reaches an arm no other case of its rule reaches, and each rule has forms that apply). The IR comes
  from a query rather than from a hand-built struct because a hand-built IR has no resolved source columns, so the
  source-column checks would pass without checking anything.
- Python: `test_direct_rule_conformance.py` (one `RuleFixture` per rule, S1's and A1's: schema, all-rows read,
  serializer, prepared plans, near misses, `off`, forced backend, `DIAGNOSE`, an empty source, and constant bounds that
  contradict the decision's built-in domain: under `auto`, `off` and `require` each gives the solver's outcome, or
  `require` declines) and the parent-context rows of `test_direct_s1_uniform_matroid.py`. Both fuzz generators in
  `test_direct_fuzz.py` run about one query in ten over an empty source and add an impossible constant bound now and
  then; with A1's domain check disabled, the conformance test and the A1 fuzz fail. `test_direct_user_facing.py` checks
  which noun a NaN or infinite coefficient names.
- Result adapter parent contexts, identical under `auto`, `off` and `require`: `CREATE TABLE AS`, `INSERT ... SELECT`, a
  view, a window over the result, `UNION ALL` of an S1 and an A1 query, a joined source with a filter above, a self-join
  of results, an empty source and a prepared statement with a parameter.
- Branch coverage: `make decide-coverage` (repository root) builds `build/coverage` when it is missing (an instrumented
  copy of the build; `-fprofile-list` instruments only `src/optimizer/decide/direct/*.cpp` and
  `decide_rewrite_rhs_window.cpp`; `build/release` is untouched) and otherwise rebuilds what changed, runs the DECIDE
  pytest suite with one profile per test (`test/decide/run_coverage.sh`) and `unittest "[decidb]"` as one combined
  profile, and prints the report into `COVERAGE_RUN_DIR` (default `build/coverage/run`). A warm run takes about 4
  minutes. `coverage_report.py report DIR` prints, per file, per test function and per test, the arms reached and the
  arms only that test reaches (`--unreached` also lists every arm no test reaches), and writes `report.json` with each
  arm's condition text and function. `coverage_report.py compare OLD NEW` lists the arms OLD reaches and NEW does not
  (exit 1 when there are any) and the reverse, matching an arm by file, function, condition text and its occurrence in
  the function, so lines that move are not read as lost. 1,673 of 1,722 arms are reached. Branch coverage shows which
  branches a test runs, not whether its assertion would catch a wrong value there. A subset run by hand needs `-m`
  (the script passes a marker that matches every test), or `conftest.py` prunes the oracle cache.
- A1 plan size: each bound clause makes a large expression, but it scales cheaply. At 1,000,000 rows: 1 clause 0.08 s, 3
  clauses 0.13 s, 8 clauses with `WHEN` 0.26 s, 3 decisions and 8 clauses 0.29 s.
