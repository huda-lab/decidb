# Shared engine — what exists today

Verified on 2026-10-29 at `c85704ad45`: `make decide-test`, `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`,
`DECIDB_VERIFY_SERIALIZER=1 make decide-test` and `DECIDB_FORCE_SOLVER=highs make decide-test` each 5,194 passed; C++
`build/release/test/unittest "[decidb]"` 2,869 assertions in 69 cases.
`test/decide/run_direct_matrix.sh` renews these numbers in one command (about 3 minutes; it prints one line per check). Design: `../architecture/`.

## What it does

- **Setting and fallback.** `decide_direct_solve` is `auto` (default), `off` or `require`. `ChooseDecideAnswer`
  (`decide_answer_choice.cpp`; Strategy selection with fallback) offers the IR to the rules after the bound-side
  rewrite and chooses a solver, lazily, only when none applies. `DIAGNOSE` and a forced backend (`ForcedSolverName()`;
  empty is unset) use the solver and conflict with `require`. A prepared plan keeps its selection until rebound.
  With the DECIDE optimizer off, every mode refuses the query (`../../pipeline/05_optimizer/done.md`).
- **IR.** One read-only front end turns the bound tree into the IR: decision domains and scopes, each constraint with
  its reducer, the reducer's own `when` and `by` key (`DirectPart::by`, grouped on `DecideKeyElement::Grouping()`, the
  collated expression), qualifier, factor, the clause's `when`, right side (with its `reduction` when the bound-side
  rewrite left it raw), a `per K:` in front of an aggregate clause (`per_class` and `classes`: D6, the whole total
  bounded once per class), and the objective, and the row checks the plan must run (`row_checks`: value checks and
  determination checks). A key element that is NULL puts its row in no group (D5), except a relation column, where
  NULL is a value. Parts the binder admits and no rule models are *not proved*, and building the IR never throws:
  `norm(e, 0)` (it needs indicator variables), any norm tag the front end does not expect, a `per K:` in front of a
  per-row clause, a clause whose reducer value became a helper decision and the two-level objective are not-proved
  parts; the determination of an entity-scoped decision, which only the DECIDE operator checks, makes the whole IR not
  proved (`DirectIR::status`). Shapes the binder never produces are `D_ASSERT`s
  (`../architecture/ir.md`). No IR node is kept that no rule reads. It uses the same term splitter as the solver path.
- **Rule interface.** `CheckCondition`, `Apply` and a fixed `Summary()` line for `EXPLAIN`, plus a shared Map step that
  checks the slot map. Rules are listed in `direct_registry.cpp`; a test seam replaces the list per connection. The
  rule driver first checks that the front end read the whole IR (`DirectIRIsReadable`: every
  constraint and the objective proved, a finite offset, a known sense); an IR that fails is offered to no rule, so each
  rule checks only what concerns its own class. Otherwise the driver runs CheckCondition for every rule. Rules are disjoint, so at most one applies to a query; two that apply
  raise an internal error naming both rules.
- **Result adapter** (`DIRECT_RESULT_ADAPTER` in `EXPLAIN`). Keeps DECIDE's columns and types under bindings it owns
  (`../architecture/result_adapter.md`), serializes, prunes outputs only when skipping is proved safe, and always keeps the
  rule's validated columns live. What a rule hands it (a child, a slot map that fits the DECIDE outputs, the validated
  columns and the source row id) is checked in release builds and a misfit raises an internal error, because rules are
  plug-ins and a broken one must fail loudly. Checks on plans the engine builds itself, in the adapter and in late
  materialization, are `D_ASSERT`s, which debug builds run. `DirectValidationBreaker` builds the all-rows read for a
  plan that could stream. Because every row is checked before any is released, rows are held in memory
  (`../architecture/result_adapter.md`, "Memory of holding rows").
- **Late materialization of a stored table** (`direct_late_materialization.cpp`). The rule driver appends the table's
  `rowid` to a single-base-table source, and after unused-column removal a late step moves each parent-read
  variable-width column that the rule's plan only forwards to a second scan joined on `rowid`. Fixed-width columns,
  joins, aggregates, volatile or computed columns, a source that already reads `rowid`, and a pipelined plan
  (`DirectSubstitute::pipelined`, set by A1's constant problem) keep the single read. Rules and measurements:
  `../architecture/result_adapter.md`.
- **Plan fragments.** `direct_plan_fragments.hpp` holds free functions that each return one plan fragment (the function
  form of a factory method). It lists only helpers with two or more users: `DirectGuardEmptyAggregate`,
  `DirectActiveWhen`, `DirectWindowMatchingCount`, `DirectValidationBreaker`, `DirectScaledTerm` (sign and constant factor
  of a coefficient) and `DirectIsNull`. S1 and R1 also share `DirectProveSharedScope` (the clauses read together have
  one `by` key, `per` classes and `when`, else the rule declines) and `DirectAddInstanceFlag` (a `per K:` clause with
  no kept class has no instance, D5).
- **Runtime checks.** `direct_runtime_checks.hpp` holds the predicates that raise an error while the plan runs, and
  their messages: `DirectInfeasibleErrorPredicate`, `DirectUnboundedErrorPredicate`, `DirectValidBoundPredicate`,
  and `DirectNumberMessage`. Every check that raises is built with `DirectRaiseUnless(ok,
  error)` or `DirectRaiseIf(bad, error)`, which return true or raise, and a list of checks is joined by `DirectAllHold`, a
  `CASE` chain in the order given, so an earlier check guards the later ones. A rule raises in the solver's order within a group (scope checks,
  the bound's NaN, an `=` bound's disagreement, then the least row message); across failing groups the order may
  differ.
- **Row checks, one implementation** (`direct_row_checks.cpp`), in the shape of DuckDB's uncorrelated scalar
  subquery. One `DirectSourceReader` serves the whole plan: the rule driver builds it for the rule's reads
  (`DirectApplicabilityCondition::source_reads`) plus one per bound side table and one per check, so it decides once,
  from the real total, whether to recompute or materialize the source. `DirectAddRowChecks` gives each check its own
  read and a grouped aggregate over only the columns the check reads (counts, least and greatest value, the least label
  per key), then an ungrouped `arg_min(message, struct_pack(keys))` over the failing groups, which picks the least
  failing group, NULL keys last. The checks' verdict (`DirectAllHold`, true or raises) is the definition of a one-row
  materialized CTE, and the reader is gated on it (`DirectSourceReader::Gate`): every later read is
  `Filter[v](CrossProduct(read, CTERef v))`, whose `projection_map` keeps the read's bindings. A gated read passes a row
  only once the CTE is computed, whichever side of the product is built first, so a check raises before any rule-built
  raise. The solver path uses the same reader with one read plus the side tables' and the checks'
  (`DirectInstallRowChecks`); `PhysicalDecide::RunScopeChecks` keeps only an entity-scoped decision's determination. A
  rule that reads once builds over `DirectBuildOverOneRead`.
- **Per-row bound reader** (`direct_bound_reader.{hpp,cpp}`; A1 is its only user until R1 is built on it). It makes sure
  two rules cannot read a bound differently. Its header defines `POSITIVE_INFINITY` and `NEGATIVE_INFINITY` once for
  every direct-solve file. A per-row bound is read by the solver path's own classifier
  (`DecideReadSingletonRow`, stored on the IR node as `singleton_row` and `bound`), so direct solve admits the
  values the solver admits, as DuckDB evaluates the comparison (D64). `DirectIsSingletonRow` is the shape a
  per-row bound must have to be read: the classifier read it, on a declared decision. `DirectReadBounds` folds
  `variable_bound`s into each decision's static box (default `[0, inf)`, a BOOL kept in `{0, 1}`, ends of magnitude
  1e20 or more infinite): the written `(sense, k)` into a box the domain check reads, the admitted ends into the box the
  answer reads. Every other bound is a per-row clause carrying the classifier's admitted `lower` and `upper`
  expressions. `DirectClauseBounds` reads one clause's two ends on a row (no bound on a row its `when` drops), and
  `DirectRowBox` intersects them with the box. `DirectBoxIsEmpty` is true on a row whose box no value satisfies. `DirectReadLinearPart` reads one linear part (an objective part or a budget) into a `DirectLinearPart`: its sign and
  constant factor, and each linear term's decision, sign and proved coefficient; it skips other term kinds. Each rule keeps
  its own policy around it: S1 declines a filtered part, a non-linear term, a second decision and a coefficient that may
  raise beside other terms, and flattens the part per term; A1 reads constants and squares itself and sums a part's
  linear terms per decision before applying the part's sign and scale.
- **`ExpressionBuilder`** (`direct_expression.hpp`; Fowler's Expression Builder) is a small fluent helper over bound
  expressions that passes them as `ExprPtr`. It is the one way the direct-solve code and the right-hand-side rewrite
  build an expression: constants (`Num`, `Bool`, and `Constant` for any other type), columns, scalar functions,
  arithmetic, comparisons, `IsNull`, `Cast`, `Coalesce`, `If`, `Min`, `Max` and `Any`. It takes the `ClientContext`,
  which it binds functions with (as DuckDB's `FunctionBinder` does), so a rule's `CheckCondition`, a binder and the
  optimizer can all make one. The nodes it has no operation for are built by hand, each with a comment: the window
  expressions, and the typed NULLs in `LogicalDirectResultAdapter::PruneUnusedOutputs`, an overridden DuckDB method
  that gets no context. `Any` tests its first condition first; S1's fixed-value flags and the rewrite's left-side
  filter reverse their lists to test the last first.
- **Rule-local helpers.** A helper only one rule uses stays in that rule's files, in its namespace, and moves to the
  shared files when a second rule needs it. Inside a rule's namespace, names drop the `Direct` and class prefixes. The
  scope projection (`DirectProjectScope`), the check of a per-row bound read from the data (`DirectCheckRowBounds`) and
  `DirectWindowExtremum` are shared in `direct_plan_fragments.{hpp,cpp}`: any rule whose aggregate takes a `when` or a
  group key, or whose bound comes from the data, needs them.
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
- **Shared admission predicates.** `DirectIsDecisionFreeDeterministic`, `DirectIsSourceOnlyNumeric`,
  `DirectIsSourceOnlyNumericMayRaise`, `DirectIsSourceOnlyValue` (which admits a value that can raise), `DirectPartScale`,
  `DIRECT_CONSTANT`, `DIRECT_FINITE_CONSTANT` and `DirectIRConstraint::PlainSum` answer "may a rule use this value"
  once for all rules. `DirectIRConstraint::Membership` gives the rows an aggregate constraint covers: its sum's own
  `when` or the clause's `when`; with both, the rules decline (the solver reads both).
- **One evaluate-to-DOUBLE function.** `TryEvaluateFoldableDouble(context, expr, out, flags)` (`decide_cast_policy.hpp`)
  is the only code that folds a constant expression to a `DOUBLE`, for the planner, the rewrites and the direct rules.
  Callers differ only in the flags they pass: `FOLD_STABLE` (no row, parameter or subquery can change it),
  `FOLD_EXACT_FINITE` (through `DecideIsExactFiniteDouble`), `FOLD_CATCH_DUCKDB_ERRORS` and
  `FOLD_CATCH_ALL`. The planner passes `FOLD_PLAIN` and lets a conversion error reach its own error policy; the sign test
  for a scale factor and the `IN` rewrite catch everything; direct rules pass `DIRECT_CONSTANT` or
  `DIRECT_FINITE_CONSTANT`.
- **The bound-side rewrite is the one evaluator of a data bound** (`decide_rewrite_rhs_bounds.cpp`,
  `RewriteRhsAggregates`, called by `DecideOptimizer::Optimize` before the direct rules, in every mode). Every reducer
  over data on the right side of a clause (`COUNT`, `MIN`, `MAX`, `SUM`, `AVG`, a written norm, a keyed or colon
  reducer) becomes a placeholder column of the DECIDE source, recorded on the node (`LogicalDecide::bound_aggregates`;
  equal reducers share one). The path that answers the node builds each record as a side table of the source reader
  (`DirectAddBoundTables`, `DirectSourceReader::AddSideTable`): the same DuckDB aggregate over another read of the
  source, grouped by the reducer's `by` key, with the rows the clause's `when` keeps and the reducer's own `when` as its
  `FILTER`. The table has one row per group, is materialized once (`direct_bound`), and every read of the source joins
  it back by the key with a SINGLE join on IS NOT DISTINCT FROM, so a NULL key is a group. This is how DuckDB
  decorrelates a scalar subquery (FlattenDependentJoins). A keyed or colon reducer counts each class of its key once:
  an aggregate per group and class takes the class's body (`ARBITRARY`; a scope check proves the key determines it),
  and the reducer runs over the classes of each group. On a row whose written `by` column is NULL the column is NULL,
  so the instance is skipped (D5), and a row whose inner `per` column is NULL is in no class. A `per K:` in front of the
  clause groups no reducer (D6). A table may read the tables added before it. The binder rejects `DISTINCT` and drops
  an `ORDER BY` inside these reducers, so neither reaches the rewrite. A data norm of order 0 is the count of its
  non-zero rows, `SUM(ABS(SIGN(e)))`.
  - The bound of a clause in the aggregate shape holds when its group's tightest value does: the MIN of an upper
    bound, the MAX of a lower one, over the kept rows of the comparison's group; a row in no class of the clause's
    `per K:` (D5) gives no bound. An `=` bound must take one value per group. A constant, a query-wide value, a reducer
    whose `by` key is coarser than the group's, and a `<>` bound (which excludes each value it takes) are left alone.
    Every other data bound is one kind, a value computed from its group (`DecideGroupBound`, held by
    `DecideReducedBound`): the group's aggregates over per-row inputs, an upper and a lower limit over their results,
    and the checks of what they read and give. A raw bound that varies in its group is the MIN and MAX of its value
    (`DirectTightestBound`); a bound that reads data only through reducers of exactly the clause's group
    (`sum(x) by (g) <= sum(cap) by (g)`) is an expression over those reducers' aggregates. A rule adds the aggregates to
    the group aggregate it already runs, with no branch per bound (`DirectAddGroupBound` and a rule's
    `DirectGroupSink`: S1 a window, R1 its hash aggregate), so a direct plan has no side table for them. Only the
    solver path records their aggregates (`RecordGroupBound`, from `RecordReducedBounds`), and it returns the checks
    in the order a rule raises them: every bound's per-row reads, then each bound's NaN limit and `=` agreement. A
    reducer on another `by` key, a qualified reducer, a column beside a reducer and a sum with its own `when` are
    recorded on the direct path too (`DirectIR::bounds_to_record`), and read from side tables.
  - Every value a bound reads is checked first, by VALUE_IS_VALID scope checks appended after the planner's: a
    reducer's argument (or a norm's written body: `SIGN(NaN)` is 0) on the rows the reducer reads, the bound on the
    kept rows, and an `=` bound's agreement in its group. Each check is a per-row message, NULL on a valid row. Both
    paths make them with `DirectAddRowChecks` (above), so a scope error (a `per K:` that does not determine a value) is
    always reported before a bad value, and both paths raise the same text.
  - A data reducer on the bound side over exactly the clause's group is computed in the rule's group aggregate, not
    as a side table; one on another `by` key, qualified, or beside a column is a side table.
  - The solver and the direct rules read DuckDB's value, so `SUM(x) <= SUM(cap)` over the `BIGINT` values 2^60, 1,
    -2^60 reads the limit 1 on `off` and `require`. Arithmetic around the aggregate (`COUNT(*) / 2`) runs in DuckDB's
    types. On a row a reducer does not read, its argument is not computed (syntax reference §6.3). A read carries the
    tables' values after the source columns, so they renumber none, and the driver drops them from a rule's output
    with the source row id, one block of the read. A side table leaves a recomputable source recomputed. Output row order and the
    choice among equal optima are not guaranteed. Tests: `test_rhs_aggregate_window.py`, `test_bound_side_tables.py`.
- **NULL values.** A NULL is skipped where it is read, as on the solver path (deck p. 50). A reducer's NULL summand
  arrives as the tests `DecideCanonicalizer::SkipNullData` added to its filter; the IR moves them out of
  `part.filter` into the part's tests, and `GuardCoefficients` reads a coefficient as 0 on a row they fail, so the SUM
  adds nothing for it. A NULL limit or per-row bound is open: S1's `SourceLimit` reads it as no limit, R1's limits pass
  over it through `MIN`/`MAX`/`LEAST`/`GREATEST`, and `DirectCheckRowBounds` and the singleton-row box
  (`AdmittedInterval`) give the instance no bound. The driver strips every tag from the operators above the source
  (`StripTagsAbove`), so EXPLAIN never prints one.
- **Number messages.** Both paths raise one text, `DecideInvalidValueMessage` (`decide_value_messages.hpp`), from the
  value's `DecideValueRole` and its text as written: the cut query text, or `DirectIR::render` where none was cut
  (`NULL_GUARD_TAG` keeps a NULL guard out of the text). It names no row: a row's place in the input depends on the
  scan.
  - A coefficient DeciDB computes (like terms merged, a reducer's factor or divisor applied) keeps the values it is
    computed from (`DecideTerm::inputs` on the solver path, `DirectNumberNames` from `DirectNamesOf` in direct solve).
    Its check is one `isfinite` on the result; only the row it fails on reads the inputs. The first input a column
    holds that is not finite is named (COEFFICIENT, quoting the input as written); with none, DeciDB's arithmetic or a
    literal left the DOUBLE range, and the clause is named (TOO_LARGE). Only the first failing row is classified.
  - A number no answer reads is not computed: an objective's per-row constants are checked one term at a time, as the
    solver path reads them, and never summed.
  - A number that cannot fail gets no check: `DirectIsFiniteWhereKept` (A1, R1 and S1) evaluates a coefficient's
    signed, scaled sum at plan time, reading each value through its NULL guard (`DecideUnguarded`), and a finite
    constant plans no `isfinite`. A guard reads a column, so asking the guard instead would treat a literal (the `1` of
    `x` inside `POWER(x - c, 2)`, the `2` of `2 * x + p * y`) as data. A constant that is not finite keeps its runtime
    check, so a query over no rows still returns none, and `DirectNamesOf` names the clause (TOO_LARGE), as the solver
    path does.
  - A number the model computes from finite ones (like coefficients merged across rows, a square, fixed terms moved to
    the bound side) is TOO_LARGE past a DOUBLE and names its clause, or the objective as EXPLAIN prints it. The solver
    path grows every such sum through `AddCoefficient` (`ilp_model.hpp`); A1's answer checks and R1's checks of each
    price and each group's anchor spend and rooms raise the same text.
  - A source value read as a coefficient must be finite; one read as a bound (a count limit, a fixed value, a per-row
    lower or upper bound) only has to be a number, because an infinite bound means "no bound" or "nothing satisfies it".
    NaN raises in every slot.
  - Direct applies this in SQL and the solver in C++ (`ExtractDoubleColumn`); the two cannot share code, so
    `test_direct_source_values.py` puts an infinite value in each of 13 bound slots S1 and A1 read, where an infinity
    means "no bound" or "nothing satisfies it", and asserts the same error or rows on `require`, `off` and forced
    HiGHS. NaN reaches the same checks through each rule's error grid, and NULL through each rule's skip cases. Message
    tests are in `test_direct_user_facing.py`: `test_invalid_score_error_matches_solver_wording` and
    `test_a1_invalid_coefficient_names_the_value_the_user_wrote` and
    `test_r1_invalid_number_names_the_value_the_user_wrote` compare `require` with `off`. The cost on clean data is
    not measured.
- **`EXPLAIN` annotation.** When a rule applies, mode, rule and one fixed condition line per rule appear in logical and
  physical `EXPLAIN` and profiling: S1 `min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi`, A1 `min/max SUM(c*x), l <= x
  <= u, each row independent`. There is no number from the query. `require` explains why no rule applied; under `auto`
  nothing is printed. `test_serializer_explain_and_model_dump` checks the rule and the condition line in the logical,
  physical and profiled plans.
- **The solver's domain check, once.** The solver raises, before it reads any row, for a decision whose box contradicts
  its built-in domain: an upper bound below the default floor of 0, any upper bound below 0 on a `BOOL` (its floor stays
  0 whatever lower bound is written), or a `BOOL` lower bound above 1. The box is the bounds as written
  (`DecideWrittenBox` in `decide_singleton_row_folding.{hpp,cpp}`), and the error quotes the clause, under `DIAGNOSE`
  too. The solver's pre-check (`DecideGlobalSinkState`) raises it; direct solve reads the same box through
  `DirectContradictsDomain`, which `DirectReadBounds` (A1, R1) and S1 call, and only declines these queries so the
  solver raises the error.
- **One box, one way to combine bounds.** A decision's unconditional bounds fold into a `DecideBoundBox`
  (`decide_numeric_policy.hpp`) through `DecideFoldSingletonRow`, which keeps the tightest value on each side. The
  solver's folding pass, the written box of the domain check (`DecideWrittenBox`) and `DirectReadBounds` (A1, R1) all
  call it. The box keeps a `lower_written` flag apart from the value: a written lower bound of -1e30 or below is no bound,
  like any bound of magnitude 1e20 or more, and only a decision with no lower bound written takes the default floor of 0
  (`EffectiveLower`). No value is reserved to mean "unset".
- **One test scaffold.** `test/decide/tests/_direct_oracle_common.py` holds what the oracle suites share: the autouse
  fixture that lifts a forced backend, `run`, `outcome`, `values_clause`, `sql_literal` (one spelling of a constant: a `CAST` to the column's type, so a
  column's type does not depend on which row comes first, with infinities and NaN spelled as strings), and
  `agree_with_oracle` (oracle model, `off`,
  `require`, then a rule-supplied check of one assignment). `test_direct_s1_uniform_matroid.py` and
  `test_direct_a1_separable_box.py` keep their case tables, oracle-model builders and assignment checks. Every direct
  suite imports the autouse fixture by name from the scaffold (pytest applies an autouse fixture only in a file that can
  see it). A new rule's suite imports the scaffold (`../architecture/rules.md`, item 2).
- **Matrix script.** `test/decide/run_direct_matrix.sh` runs the five checks and prints one summary line each, for
  example `auto: 3089 passed, 0 xfailed, 0 failed (3089 outcomes)`; the outcome totals of the modes should agree. Logs
  stay in `test/decide/results/matrix_<time>/` (gitignored), or `$MATRIX_LOG_DIR`, and the last line prints the path.
  The golden capture (`test/decide/golden/capture.sh`, also run by `check.sh`) is single-threaded: with several threads
  a source's rows arrive in a thread-dependent order, and the model dump follows the order the DECIDE operator reads.

## Registering a rule

A rule registers with one line in `direct_registry.cpp` and one in the folder's `CMakeLists.txt`. The rule driver
(`direct_rule_driver.cpp`) and the result adapter (`direct_result_adapter.cpp`) hold no rule-specific code. S1 and A1
are materially different rules (decision types, output types, a quadratic objective, aggregate constraints or none, a
breaker or a streaming plan) and run on the same two central files. An IR flag a rule needs about the solver's own
reading comes from the solver's code, not a copy: `DirectIRConstraint::singleton_row`, `bound` and
`variable_bound` come from the classifier `DecideReadSingletonRow` (`decide_singleton_row_folding.cpp`), so a rule
asks the solver's own question about which values a bound admits and which bounds replace a variable's default box.

## Verified by

- C++: `test_decidb_direct_ir.cpp` (every IR node kind and not-proved parts: a per-row term the splitter cannot read, an order-0 norm in a reducer, and a norm tag it does not expect, each as a not-proved part, not
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
  serializer, prepared plans, near misses, `off`, forced backend, `DIAGNOSE` and an empty source) and the
  parent-context rows of `test_direct_s1_uniform_matroid.py`. With A1's domain check disabled,
  `test_bool_bound_outside_domain.py` (a bound against the domain over an empty source) and two cases of
  `test_error_infeasible.py` fail. `test_direct_user_facing.py` checks
  which noun a NaN or infinite coefficient names.
- Result adapter parent contexts, identical under `auto`, `off` and `require`: `CREATE TABLE AS`, `INSERT ... SELECT`, a
  view, a window over the result, `UNION ALL` of an S1 and an A1 query, a joined source with a filter above, a self-join
  of results, an empty source and a prepared statement with a parameter.
- Branch coverage: `make decide-coverage` (repository root) builds `build/coverage` when it is missing (an instrumented
  copy of the build; `-fprofile-list` instruments only `src/optimizer/decide/direct/*.cpp` and
  `decide_rewrite_rhs_bounds.cpp`; `build/release` is untouched) and otherwise rebuilds what changed, runs the DECIDE
  pytest suite with one profile per test (`test/decide/run_coverage.sh`) and `unittest "[decidb]"` as one combined
  profile, and prints the report into `COVERAGE_RUN_DIR` (default `build/coverage/run`). A warm run takes about 4
  minutes. `coverage_report.py report DIR` prints, per file, per test function and per test, the arms reached and the
  arms only that test reaches (`--unreached` also lists every arm no test reaches), and writes `report.json` with each
  arm's condition text and function. `coverage_report.py compare OLD NEW` lists the arms OLD reaches and NEW does not
  (exit 1 when there are any) and the reverse, matching an arm by file, function, condition text and its occurrence in
  the function, so lines that move are not read as lost. 1,857 of 1,864 arms are reached. Of the seven no test reaches, four are kept on purpose: `ref.depth == 0` in `DirectReferencesOnlySource`,
  which every caller's earlier determinism check rules out but which the function's contract promises on its own; the
  `default` arms of the `ExpressionType` switches in S1's `NormalizeCapacity` and `SatisfiesComparison`; and the
  extension-name check in late materialization's `VisitForAdapters`, which keeps an operator of another extension from
  being cast to the adapter. The other three are the nested-reducer reading in `ReadReducer` (`DirectPart::inner`),
  which no query reaches until the two-level objective is read (`todo.md` H-06). The keyed S1 and R1 plans run from
  queries in the S1 and R1 suites, and from an IR against the solver with NULL-key rows (`test/common/
  test_decidb_direct_decline.cpp`, "keyed plans"). Every other check follows one policy: a query reaches it and a test runs it; a
  check on what a rule hands the engine raises in release builds and a stub-rule C++ test reaches it; a check on a plan,
  IR or setting the engine builds itself is a `D_ASSERT`, which debug builds run; code no query reaches is deleted.
  Branch coverage shows which branches a test runs, not whether its assertion would catch a wrong value there, so 44
  one-line changes to value-handling code (a comparison, a constant, an operator) were each planted and run against the six `test_direct_*.py` files. Every direct-solve test case reaches an
  arm no other test reaches, catches one of these changes, or pins a behavior coverage does not measure (the solver
  path's wording, prepared plans, `DIAGNOSE`, an empty source). 43 changes are caught. No query can tell the other one
  apart: `inclusive <= 0.0` against `< 0.0` for a lower bound in S1's `NormalizeCapacity` (a bound of exactly 0 gives
  capacity 0 either way). A subset
  run by hand needs `-m` (the script passes a marker that matches every test), or `conftest.py` prunes the oracle cache.
- A1 plan size: each bound clause makes a large expression, but it scales cheaply. At 1,000,000 rows: 1 clause 0.08 s, 3
  clauses 0.13 s, 8 clauses with `when` 0.26 s, 3 decisions and 8 clauses 0.29 s.
