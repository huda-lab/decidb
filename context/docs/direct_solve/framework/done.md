# Shared engine — what exists today

Verified on 2026-11-08 at `73b52ab1d1`: `test/decide/run_direct_matrix.sh`, each of its four suites (auto, off,
serializer, highs) 7,020 passed; C++ `build/release/test/unittest "[decidb]"` 4,635 assertions in 85 cases. Design:
`../architecture/`.

## What it does

- **Setting and fallback.** The modes are in [policy.md](../architecture/policy.md). `ChooseDecideAnswer`
  (`decide_answer_choice.cpp`; Strategy selection with fallback) offers the IR to the rules after the bound-side
  rewrite and chooses a solver, lazily, only when none applies. A forced backend is `ForcedSolverName()` (empty is
  unset). With the DECIDE optimizer off, every mode refuses the query (`../../pipeline/05_optimizer/done.md`).
- **IR** ([ir.md](../architecture/ir.md)). The IR holds decision domains and scopes, each constraint with its reducer,
  the reducer's own `when` and `by` key (`DirectPart::by`, grouped on `DecideKeyElement::Grouping()`, the collated
  expression), qualifier, factor, the clause's `when`, right side (with its `reduction` when the bound-side rewrite
  left it raw), a `per K:` in front of an aggregate clause (`per_class` and `classes`: D6, the whole total bounded once
  per class), the objective, and the row checks the plan must run (`row_checks`). A key element that is NULL puts its
  row in no group (D5), except a relation column, where NULL is a value. Building the IR never throws; what it cannot
  model is not proved (`DirectIR::status` for the whole IR). No IR node is kept that no rule reads.
- **Rule interface.** `CheckCondition`, `Apply` and a fixed `Summary()` line for `EXPLAIN`, plus a shared Map step that
  checks the slot map. Rules are listed in `direct_registry.cpp`; a test seam replaces the list per connection. The
  rule driver first checks that the front end read the whole IR (`DirectIRIsReadable`: every
  constraint and the objective proved, a finite offset, a known sense); an IR that fails is offered to no rule, so each
  rule checks only what concerns its own class. Otherwise the driver runs CheckCondition for every rule (disjoint
  rules: [rules.md](../architecture/rules.md)).
- **Result adapter** ([result_adapter.md](../architecture/result_adapter.md)). It serializes, prunes outputs only when
  skipping is proved safe, and always keeps the rule's validated columns live. What a rule hands it (a child, a slot
  map that fits the DECIDE outputs, the validated columns and the source row id) is checked in release builds and a
  misfit raises an internal error, because rules are plug-ins and a broken one must fail loudly. Checks on plans the
  engine builds itself, in the adapter and in late materialization, are `D_ASSERT`s, which debug builds run.
- **Late materialization of a stored table** (`direct_late_materialization.cpp`; rules and measurements in
  [result_adapter.md](../architecture/result_adapter.md)). A rule whose plan streams sets
  `DirectSubstitute::pipelined` (A1), which keeps the single read over a source that is not copied.
- **Plan fragments.** `direct_plan_fragments.hpp` holds free functions that each return one plan fragment (the function
  form of a factory method). None of them knows which problem class it serves: among them `DirectActiveWhen`,
  `DirectWindowMatchingCount`, `DirectAllRowsValid`, `DirectScaledTerm` (sign and constant factor of a coefficient),
  `DirectSumOf` (the signed sum of proved values, each read as DOUBLE), `DirectOnKeptRows` and `DirectOverProjection`
  (a copy of a value re-pointed at a projection over the source). A rule assembles each projection of its plan in a
  `DirectProjectionStage`, which gives every column its binding as it is added and becomes a `LogicalProjection` over a
  child. S1, R1 and N1 also share `DirectProveSharedScope` (the clauses read together have
  one `by` key, `per` classes and `when`, else the rule declines) and `DirectAddInstanceFlag` (a `per K:` clause with
  no kept class has no instance, D5).
  - **A separable quadratic objective** (A1, N1 and A2: `a*x^2 + b*x` per decision) takes three fragments, in plan
    order. `DirectLayoutQuadraticPart` (P1) lays out a `DirectQuadraticPart` in the rule's first projection: its mask,
    the sum of its linear coefficients per decision, and for each square its weight `k`, slope `w` and constant `t` of
    `k*(w*x + t)^2`, each computed on the rows the part reads; a weight read from the data is checked as the solver
    checks it (NaN and infinity raise, NULL skips the row), and a constant one has no check. It appends the check of
    each value that can fail to the rule's list, and reads a constant one term at a time, never summed.
    `DirectFoldQuadratic` (P2) folds one decision's `a` and `b` over the parts, each scaled by its sign and factor, and
    returns the part of `b` the linear terms make, which the rule tests for a number past a DOUBLE; the rule applies the
    objective's sense. `DirectTurningPoint` returns the turning point `-b / (2a)`, or for a whole-numbered decision the
    first integer whose next step no longer improves (the smaller one on a tie), with its precision flag (Number-range
    warning, below).
  - **A group's aggregates** take one interface, `DirectGroupSink`. `DirectWindowSink` is its window form: each
    aggregate becomes a window over the whole partition, with the rule's group as the partition (S1, N1 and A2). R1's own
    sink adds each aggregate to its hash aggregate with `DirectAggregateOf`, which the bound side tables use too.
- **Runtime checks.** `direct_runtime_checks.hpp` holds the predicates that raise an error while the plan runs, and
  their messages: `DirectErrorPredicate` (a BOOLEAN that raises a given message, fixed or computed per row),
  `DirectValidBoundPredicate` and `DirectNumberMessage`. The two state errors come from `DirectStateErrors {infeasible,
  unbounded}`, which the rule driver builds once per query from `DecideSolverRuns(ClassifyDecideModel(ir.model_shape))`
  and hands to every rule's `Apply`; `DirectInfeasibleErrorPredicate` and `DirectUnboundedErrorPredicate` take it. Their
  texts are `DecideInfeasibleMessage` and `DecideUnboundedMessage` (`decide_value_messages.hpp`, given `diagnose_runs`),
  which advise prefixing the query with `DIAGNOSE` only where `DIAGNOSE` would run. `DirectCheckNumber` checks a raw
  value a projection computes, naming the values it is made of, and is null when they are finite constants;
  `DirectRaiseTooLarge` checks a number computed from values checked before it. Every check that raises is built with
  `DirectRaiseUnless(ok, error)`, `DirectRaiseIf(bad, error)` (both return true or raise), `DirectRaiseMessage` (raises
  a per-row message where it is not NULL). A check of a value computed only on the rows a `when` keeps needs no mask of
  its own: the value is NULL on a dropped row, and `DirectCheckNumber` and `DirectValidBoundPredicate` pass a NULL.
  A list of checks is joined by `DirectAllHold`, a `CASE` chain in the order given, so an earlier check guards the later
  ones. `DirectWarnIf` and `DirectBelowNormalRange` build the warning, which is always true (Number-range warning,
  below). A rule raises in the solver's order within a group (scope checks, the bound's NaN, an `=` bound's
  disagreement, then the least row message); across failing groups the order may differ.
- **Row checks, one implementation** (`direct_row_checks.cpp`), in the shape of DuckDB's uncorrelated scalar subquery.
  One `DirectSourceReader` serves the whole plan: the rule driver builds it for the rule's reads
  (`DirectApplicabilityCondition::source_reads`) plus one per bound side table and one per check, so it decides once,
  from the real total, whether to recompute or materialize the source, the choice DuckDB makes for a CTE. A source
  that `DirectRepeatableSourceChain` accepts (non-volatile projections and filters over one repeatable DuckDB table
  scan) is copied once when `DirectSourceCopyIsCheaper` says so, else rebuilt per read with fresh table indexes, its
  pushed-down table filters copied, so each read is a parallel scan. A read of the copy costs `COPY_READ_COST` (0.4) of
  a re-scan, so a source read three times or more is copied, provided the copy (estimated rows times its row width,
  which leaves out the variable-width columns late materialization reads back) is at most `1/COPY_MEMORY_SHARE` (1/8)
  of the memory limit; with no row estimate it is re-scanned. Both constants come from interleaved timings on partsupp
  and a 20-column wide table at 10K to 1M rows: at 100K the copy took 68 ms against 96 (narrow) and 66 against 94
  (wide), and past an eighth of the memory limit it lost. At 10K rows on partsupp it is 14 ms slower, in R1's sort, not
  in the reads. On a copy of only the columns the plan reads (partsupp, lineitem and the wide table, 100K and 1M rows),
  the copy took 1.0 to 1.2 of the re-scans' time at two reads, 0.57 to 0.98 at three and 0.62 to 1.05 at four. Any
  constant from 1/3 to 1/2 copies at three reads and re-scans at two, so 0.4 stays. Any other source (a join, a
  subquery with an aggregate, a table function, a volatile expression) is one `LogicalMaterializedCTE`, defined through
  a projection that names its columns (a join under it may reorder its output in later passes) and read by
  `LogicalCTERef`s. Every read returns the same rows, and no plan depends on their order. `DirectAddRowChecks` gives each check its own
  read and a grouped aggregate over only the columns the check reads (counts, least and greatest value, the least label
  per key), then an ungrouped `arg_min(message, struct_pack(keys))` over the failing groups, which picks the least
  failing group, NULL keys last. The checks' verdict (`DirectAllHold`, true or raises) is the definition of a one-row
  materialized CTE, and the reader is gated on it (`DirectSourceReader::Gate`): every later read is
  `Filter[v](CrossProduct(rows, CTERef v))` under the read's forwarding projection, which keeps the rows' bindings. A
  gated read passes a row only once the CTE is computed, whichever side of the product is built first, so a check raises
  before any rule-built raise. Gates chain: a rule gates the reader again on its own verdict (A1), computed from reads
  the row checks gate, so a scope error raises before a rule's own, and every read after waits for the newest verdict
  alone; the verdict CTEs nest oldest outermost (`DirectSourceReader::Finish`). The solver path uses the same reader
  with one read plus the side tables' and the checks' (`DirectInstallRowChecks`); `PhysicalDecide::RunScopeChecks` keeps
  only an entity-scoped decision's determination. A rule builds a plan over one read with `DirectBuildOverRead`, or over
  its only read with `DirectBuildOverOneRead`.
- **Bound and term reader** (`direct_bound_reader.{hpp,cpp}`; the name is the first half's). The file reads two things,
  and reads each once, so that no two rules read a bound or an objective term differently. Its header defines
  `POSITIVE_INFINITY` and `NEGATIVE_INFINITY` once for every direct-solve file.
  - **Per-row bounds on a decision.** A per-row bound is read by the solver path's own classifier
    (`DecideReadSingletonRow`, stored on the IR node as `singleton_row` and `bound`), so direct solve admits the
    values the solver admits, as DuckDB evaluates the comparison (D64). A per-row constraint is read as a bound only
    when the classifier read it as a bound on one decision (`singleton_row`). `DirectReadBounds` folds
    `variable_bound`s into each decision's static box (default `[0, inf)`, a BOOL kept in `{0, 1}`, ends of magnitude
    1e20 or more infinite): the written `(sense, k)` into a box the domain check reads, the admitted ends into the box
    the answer reads. Every other bound is a per-row clause carrying the classifier's admitted `lower` and `upper`
    expressions. `DirectStaticFloor` lowers a decision's static floor on the rows where a lower bound under a `when`
    replaces the default of 0. `DirectClauseBounds` reads one clause's two ends on a row (no bound on a row its `when`
    drops), and `DirectRowBox` intersects them with the box. `DirectBoxIsEmpty` is true on a row whose box no value
    satisfies.
  - **Terms of an objective part or a budget.** `DirectReadLinearPart` reads one linear part into a `DirectLinearPart`:
    its sign and constant factor, and each linear term's decision, sign and proved coefficient; it skips other term
    kinds. `DirectReadConstantTerm` reads a per-row constant. `DirectReadLinearForm` reads a linear form `w*x + t` in
    one decision (its `inner.linear` and `inner.constant`, with their names) and declines a form in two decisions.
    `DirectReadSquareTerm` reads a square of such a form under a weight, constant or read from the data, as signed
    values, and `DirectReadAbsTerm` reads `ABS` of such a form; both fill one `DirectWeightedForm`
    (`weight * f(inner)`, `f` set by the reader), which `DirectLayoutQuadraticPart` lays out.
    Neither judges the weight: a rule that needs a constant folds it. `DirectFoldConstantSum` folds signed values (a
    square's weight or slope) to the one constant every row shares, reading through a NULL guard, and fails on a value
    that is not a finite constant. `DirectObjectivePartKept` returns the rows a part reads, those that the objective's
    `when` and the part's own both keep. `DirectReadQuadraticPart` composes the four into a `DirectQuadraticPart`
    (linear terms, the rows kept, constants and squares); how many squares a rule admits, with which signs and weights,
    is the rule's to check. An `ABS` term stays out of it, so a rule that calls it sees an `ABS` as a term it does not
    read. The header also holds `DirectNamesOf` and `DirectIsFiniteWhereKept`, described under Number messages.
  - **Policy stays in the rule.** Each rule keeps its own policy around the readers, and no rule keeps a copy of one.
    S1 declines a filtered part, a non-linear term, a second decision and a coefficient that may raise beside other
    terms, and flattens the part per term. N1 and A2 fold the objective square's slope with `DirectFoldConstantSum`; A1
    and N1 fold its weight the same way and so decline one read from the data, which A2 takes.
- **`ExpressionBuilder`** (`direct_expression.hpp`; Fowler's Expression Builder) is a small fluent helper over bound
  expressions that passes them as `ExprPtr`. It is the one way the direct-solve code and the right-hand-side rewrite
  build an expression: constants (`Num`, `Bool`, and `Constant` for any other type), columns, scalar functions,
  arithmetic, comparisons, `IsNull`, `Cast`, `Coalesce`, `If`, `Min`, `Max`, `And`, `Not` and `Any`. `And` and `Not`
  are `CASE` forms that read a NULL condition as false, and `And` evaluates its second value only where the first holds. It takes the `ClientContext`,
  which it binds functions with (as DuckDB's `FunctionBinder` does), so a rule's `CheckCondition`, a binder and the
  optimizer can all make one. The nodes it has no operation for are built by hand, each with a comment: the window
  expressions, and the typed NULLs in `LogicalDirectResultAdapter::PruneUnusedOutputs`, an overridden DuckDB method
  that gets no context. `Any` tests its first condition first; S1's fixed-value flags and the rewrite's left-side
  filter reverse their lists to test the last first.
- **Rule-local helpers.** A helper only one rule uses stays in that rule's files, in its namespace, and moves to the
  shared files when a second rule needs it. Inside a rule's namespace, names drop the `Direct` and class prefixes. The
  scope projection (`DirectProjectScope`), the message of a per-row bound read from the data
  (`DirectRowBoundMessage`) and `DirectWindowExtremum` are shared in
  `direct_plan_fragments.{hpp,cpp}`: any rule whose aggregate takes a `when` or a group key, or whose bound comes from
  the data, needs them. `DirectRunningSum` (R1 and A2) is a window sum over the rows up to the current one in a given
  order, per partition. `DirectLayoutBoundClauses` (A1 and A2) lays out each per-row bound clause in a rule's first
  projection: its coefficient, right side and ends, each computed only on the rows the clause keeps, and the checks of
  the coefficient and the right side, which pass on a dropped row because both values are NULL there.
- **Numeric policy.** `src/include/duckdb/common/decide_numeric_policy.hpp` is the one home of the numeric facts that
  DuckDB's own evaluation cannot supply. It includes only `<cmath>`, so the planner, the optimizer and direct-solve
  rules, the model builder and the solver backends all read it without including each other. It holds:
  - `DECIDE_SOLVER_INFINITY` (1e30), the solver's sentinel: the builder's "no bound", the unset side of a
    singleton-row bound, a one-sided row.
  - `DECIDE_EFFECTIVE_INFINITY` (1e20), the reading margin, with `DecideIsOpenAbove` and `DecideIsOpenBelow`.
  - `DECIDE_MAX_EXACT_INTEGER` (2^53) with `DecideIsWithinExactRange`; `DECIDE_MAX_FRACTIONAL_MAGNITUDE` (2^52);
    `DECIDE_FIRST_DOUBLE_BEYOND_BIGINT` (2^63).
  - `DecideIsExactFiniteDouble`: a converted double is exact when it is finite and converts back to the original value.
  - `DecideStrictBound`: `x > 0.5` means `x >= 1`. The classifier's written box, the model builder's linear and
    quadratic rows and S1's count limits all shift a strict bound with it, so none reads a strict comparison
    differently.
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
  `DirectIsSourceOnlyNumericMayRaise`, `DirectIsSourceOnlyValue` (which admits a value that can raise),
  `DirectIsSourceOnlyPredicate` (a deterministic, nonthrowing BOOLEAN over source columns: a `when` the plan evaluates
  on every row), `DirectHasColumnReference`, `DirectRemapSourceReferences` (re-points source columns at a projection
  over the source), `DirectIsWholeNumbered` (an `INT` or `BOOL` decision), `DirectIsLowerBound` and
  `DirectIsUpperBound` (which comparisons bound a count from below and from above), `DirectPartScale`,
  `DIRECT_CONSTANT`, `DIRECT_FINITE_CONSTANT`, `DirectIRConstraint::PlainSum` and `DirectPart::IsPlainSum` answer "may a rule use this value"
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
  `RewriteRhsAggregates`, called by `DecideOptimizer::OptimizeDecide` before the direct rules, in every mode). Every reducer
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
    (`sum(x) by (g) <= sum(cap) by (g)`) is an expression over those reducers' aggregates. A rule proves it can compute
    the bound (`DirectGroupBoundIsReadable`), lays out the columns of each input (`DirectGroupLayout`,
    `DirectGroupInputs`) and adds the aggregates to the group aggregate it already runs, with no branch per bound
    (`DirectAddGroupBound` and a rule's `DirectGroupSink`: S1, N1 and A2 a window through `DirectWindowSink`, R1 its hash
    aggregate), so a direct plan has no side table for them. Only the
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
  arrives as the tests `DecideCanonicalizer::SkipNullData` added to its filter; the IR moves them out of `part.filter`
  into the part's tests, and `GuardCoefficients` reads a coefficient as 0 on a row they fail, so the SUM adds nothing
  for it: every coefficient, a square's weight and the implicit 1 of a term with no coefficient among them, each tagged
  as a guard so a rule that needs the constant reads through it (`DecideUnguarded`). A NULL limit or per-row bound is
  open: S1's `SourceLimit` reads it as no limit, R1's limits pass over it through `MIN`/`MAX`/`LEAST`/`GREATEST`, and
  `DirectRowBoundMessage` and the singleton-row box (`AdmittedInterval`) give the instance no bound. The driver strips
  every tag from the operators above the source (`StripTagsAbove`), so EXPLAIN never prints one.
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
  - A number that cannot fail gets no check: `DirectIsFiniteWhereKept` (S1 and R1, and `DirectCheckNumber` for A1 and
    N1) evaluates a coefficient's signed, scaled sum at plan time, reading each value through its NULL guard
    (`DecideUnguarded`), and a finite constant plans no `isfinite`. A guard reads a column, so asking the guard instead
    would treat a literal (the `1` of `x` inside `POWER(x - c, 2)`, the `2` of `2 * x + p * y`) as data. A constant that
    is not finite keeps its runtime check, so a query over no rows still returns none, and `DirectNamesOf` names the
    clause (TOO_LARGE), as the solver path does.
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
- **Number-range warning.** A direct plan builds no model, so a coefficient of 1e6 or more, or an objective
  coefficient of 1e-6 or less, prints nothing, where `off` warns; the solver path's thresholds read a model. A rule that
  divides by a number it computes warns instead when that divisor, or a nonzero numerator beside it, is below the normal
  range of a DOUBLE (about 2.2e-308, 0 included: `DirectBelowNormalRange`), since a quotient taken there has lost
  precision. `DirectWarnIf` takes that condition, prints the solver path's number-range warning through
  `__internal_decide_warn_numbers()`, once per query for every path (`DecideNumberWarningState`), and lets the row
  through. A square's turning point is one shared expression, `DirectTurningPoint`: it divides `-b` by `2a`, and its
  flag is set where the square factor `a` is not 0 and either `a` or a nonzero linear sum `b` is below the normal
  range. A1, N1 and A2 all warn from it, so a tiny square factor warns alike under each. N1's scale divides a row's
  value by `sqrt(h)`, the group's sum of squares, and warns where `h` is below the normal range on a row that reads the
  scale with a nonzero value. R1 has no turning point and warns on its own ratio, an item's price `v / w` with `v` not
  0.

  A warning reads the rows the rule's error checks read: every row the clause keeps, before any row is released (A1's
  verdict over all rows, R1's group aggregate, N1's target flag, which stands below N1's window). It does not depend on
  a `LIMIT` or a filter above the query, as the solver path's warning from the whole model does not. N1's scale warning
  from `h` is the one exception. `h` is a window over the ball's group, so that check stands above the window and reads
  only the rows a parent releases, as N1's own error for an `h` past a DOUBLE does. The flag reads the group's `h` and
  the row's own value, so a group is missed only when a parent releases none of its rows that have a nonzero value.
  Meeting the whole row set there would take a second pass over the rows. The precision rule forbids it: a plan warns
  and solves, and mitigates only through free reorderings or the architecture, with DuckDB's own DOUBLE handling as the
  bar.
- **`EXPLAIN` annotation** ([policy.md](../architecture/policy.md)). `test_serializer_explain_and_model_dump` checks
  the rule and the condition line in the logical, physical and profiled plans.
- **The solver's domain check, once.** The solver raises, before it reads any row, for a decision whose box contradicts
  its built-in domain: an upper bound below the default floor of 0, any upper bound below 0 on a `BOOL` (its floor stays
  0 whatever lower bound is written), or a `BOOL` lower bound above 1. The box is the bounds as written
  (`DecideWrittenBox` in `decide_singleton_row_folding.{hpp,cpp}`), and the error quotes the clause, under `DIAGNOSE`
  too. The solver's pre-check (`DecideGlobalSinkState`) raises it; direct solve reads the same box through
  `DirectContradictsDomain`, which `DirectReadBounds` and S1 call, and only declines these queries so the
  solver raises the error.
- **One box, one way to combine bounds.** A decision's unconditional bounds fold into a `DecideBoundBox`
  (`decide_numeric_policy.hpp`) through `DecideFoldSingletonRow`, which keeps the tightest value on each side. The
  solver's folding pass, the written box of the domain check (`DecideWrittenBox`) and `DirectReadBounds` all
  call it. The box keeps a `lower_written` flag apart from the value: a written lower bound of -1e30 or below is no bound,
  like any bound of magnitude 1e20 or more, and only a decision with no lower bound written takes the default floor of 0
  (`EffectiveLower`). No value is reserved to mean "unset".
- **One test scaffold.** `test/decide/tests/_direct_oracle_common.py` holds what the oracle suites share: the autouse
  fixture that lifts a forced backend, `run`, `outcome`, `values_clause`, `sql_literal` (one spelling of a constant: a `CAST` to the column's type, so a
  column's type does not depend on which row comes first, with infinities and NaN spelled as strings), and
  `agree_with_oracle` (oracle model, `off`,
  `require`, then a rule-supplied check of one assignment). Each rule's suite keeps its own case tables, oracle-model
  builder and assignment check. Every direct
  suite imports the autouse fixture by name from the scaffold (pytest applies an autouse fixture only in a file that can
  see it). A new rule's suite imports the scaffold (`../architecture/rules.md`, item 2).
- **Matrix script.** `test/decide/run_direct_matrix.sh` runs the five checks and prints one summary line each, for
  example `auto: 3089 passed, 0 xfailed, 0 failed (3089 outcomes)`; the outcome totals of the modes should agree. Logs
  stay in `test/decide/results/matrix_<time>/` (gitignored), or `$MATRIX_LOG_DIR`, and the last line prints the path.
  The golden capture (`test/decide/golden/capture.sh`, also run by `check.sh`) is single-threaded: with several threads
  a source's rows arrive in a thread-dependent order, and the model dump follows the order the DECIDE operator reads.

## Registering a rule

How to register one is in [rules.md](../architecture/rules.md) § Adding a rule. The rule driver
(`direct_rule_driver.cpp`) and the result adapter (`direct_result_adapter.cpp`) hold no rule-specific code: S1 and A1
are materially different rules (decision types, output types, a quadratic objective, aggregate constraints or none, a
rank window or a verified, streamed plan) and run on the same two central files.

## Verified by

- C++: `test_decidb_direct_ir.cpp` (every IR node kind and not-proved parts: a per-row term the splitter cannot read, an order-0 norm in a reducer, and a norm tag it does not expect, each as a not-proved part, not
  an exception), `test_decidb_direct_rule_driver.cpp` (stub rules: one rule applying, fallback
  when no rule applies, a rule that applies beside one that does not, two rules applying as an internal error, an IR the
  front end could not read offered to no rule, mismatched slot, a shared plan fragment handed an input that does not
  fit, each way a substitute can misfit the adapter (no child, a missing prunable flag, a slot,
  validated column or row id slot beyond the child), a pipelined check and a gated verdict under `LIMIT 1` and
  `COUNT(*)`),
  `test_decidb_direct_decline.cpp` (each decline branch of the five rules' `CheckCondition` and of `DirectIRIsReadable`,
  tested on the IR: a probe rule installed through the rule driver's test seam receives the IR the driver reads from a
  small query that is only planned, edits it for the few shapes SQL cannot write, and asks the rule whether it
  applies; each case reaches an arm no other case of its rule reaches, and each rule has forms that apply). The IR comes
  from a query rather than from a hand-built struct because a hand-built IR has no resolved source columns, so the
  source-column checks would pass without checking anything.
- Python: `test_direct_rule_conformance.py` (one `RuleFixture` per rule: schema, all-rows read,
  serializer, prepared plans, near misses, `off`, forced backend, `DIAGNOSE` and an empty source) and the
  parent-context rows of `test_direct_s1_uniform_matroid.py`. With A1's domain check disabled,
  `test_bool_bound_outside_domain.py` (a bound against the domain over an empty source) and two cases of
  `test_error_infeasible.py` fail. `test_direct_user_facing.py` checks
  which noun a NaN or infinite coefficient names.
- Result adapter parent contexts, identical under `auto`, `off` and `require`: `CREATE TABLE AS`, `INSERT ... SELECT`, a
  view, a window over the result, `UNION ALL` of an S1 and an A1 query, a joined source with a filter above, a self-join
  of results, an empty source and a prepared statement with a parameter.
- Branch coverage: `make decide-coverage` (repository root) builds `build/coverage` when it is missing (an instrumented
  copy of the build; `-fprofile-list` instruments the files `COVERAGE_SOURCES` names, by default
  `src/optimizer/decide/direct/*.cpp` and `decide_rewrite_rhs_bounds.cpp`; `build/release` is untouched; it needs Clang,
  `COVERAGE_CC` and `COVERAGE_CXX`, and the LLVM tools of that Clang's version, which `coverage_report.py` asks the
  compiler for) and otherwise rebuilds what changed, runs the DECIDE pytest suite with one profile per test
  (`test/decide/run_coverage.sh`) and `unittest "[decidb]"` as one combined profile, and prints the report into
  `COVERAGE_RUN_DIR` (default `build/coverage/run`). A warm run takes about 4 minutes. `coverage_report.py report DIR`
  prints, per file, per test function and per test, the arms reached and the arms only that test reaches (`--unreached`
  also lists every arm no test reaches), and writes `report.json` with each arm's condition text and function.
  `coverage_report.py compare OLD NEW` lists the arms OLD reaches and NEW does not (exit 1 when there are any) and the
  reverse, matching an arm by file, function, condition text and its occurrence in the function, so lines that move are
  not read as lost. Arms no test reaches, kept on purpose: `ref.depth == 0` in `DirectReferencesOnlySource`, which every
  caller's earlier determinism check rules out but which the function's contract promises on its own; the `default` arm
  of the `ExpressionType` switch in S1's `NormalizeCapacity`; and the extension-name check in late materialization's
  `VisitForAdapters`, which keeps an operator of another extension from being cast to the adapter. The nested-reducer
  reading in `ReadReducer` (`DirectPart::inner`) is reached by no query until the two-level objective is read (`todo.md`
  H-06). The keyed S1 and R1 plans run from queries in the S1 and R1 suites, and from an IR against the solver with
  NULL-key rows (`test/common/ test_decidb_direct_decline.cpp`, "keyed plans"). Every other check follows one policy: a
  query reaches it and a test runs it; a check on what a rule hands the engine raises in release builds and a stub-rule
  C++ test reaches it; a check on a plan, IR or setting the engine builds itself is a `D_ASSERT`, which debug builds
  run; code no query reaches is deleted. Branch coverage shows which branches a test runs, not whether its assertion
  would catch a wrong value there. One change no query can tell apart: `inclusive <= 0.0` against `< 0.0` for a lower
  bound in S1's `NormalizeCapacity` (a bound of exactly 0 gives capacity 0 either way). At `73b52ab1d1` the tests reach
  2,397 of the 2,450 arms in these files; `todo.md` § TEST-2 lists the other 53. A subset run by hand needs `-m` (the
  script passes a marker that matches every test), or `conftest.py` prunes the oracle cache.
- A1 plan size: each bound clause makes a large expression, but it scales cheaply. At 1,000,000 rows: 1 clause 0.08 s, 3
  clauses 0.13 s, 8 clauses with `when` 0.26 s, 3 decisions and 8 clauses 0.29 s.
