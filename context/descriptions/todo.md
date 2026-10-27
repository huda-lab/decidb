# Work queue: every open task in the docs

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty. "(decision)" marks a task that needs the owner's choice before code.

Read first: `00_project_overview/syntax_reference.md`, `08_direct_solve/README.md`, `06_issues/bugs/todo.md`.
Checks: [08_direct_solve/framework/todo.md § How to work](08_direct_solve/framework/todo.md#how-to-work-any-task-in-these-todo-files).

## 1. Merge follow-ups: direct solve reads the new language

Batches: A = H-05, H-06, H-07 (grouping); B = H-04, H-08 (scope); C = R1 `when` task.

- [ ] H-04 Keyed scope checks in the direct plan — [framework § H-04](08_direct_solve/framework/todo.md#h-04--keyed-scope-checks-in-the-direct-plan)
- [ ] H-05 Read a reducer's `by` key into the IR as a GROUP BY in the direct plan (decision; ~280 keyed cases are strict xfails on the direct path until then) — [framework § H-05](08_direct_solve/framework/todo.md#h-05--read-a-reducers-by-key-into-the-ir) (needs: H-04)
- [ ] S1 reads a `by` key again — [S1 § Read a `by` key](08_direct_solve/rules/s1_uniform_matroid/todo.md#read-a-by-key-again-counts-per-group) (needs: H-05)
- [ ] R1 reads a `by` key again — [R1 § Read a `by` key](08_direct_solve/rules/r1_continuous_knapsack/todo.md#read-a-by-key-again-one-budget-per-group) (needs: H-05)
- [ ] H-06 Two-level objective `outer(per k: inner(e) by (k))` — [framework § H-06](08_direct_solve/framework/todo.md#h-06--the-two-level-objective-outerper-k-innere-by-k)
- [ ] H-07 `per K:` on a constraint — [framework § H-07](08_direct_solve/framework/todo.md#h-07--per-k-on-a-constraint-one-constraint-per-class) (needs: H-04)
- [ ] H-08 VALUE-shape clauses (helper decisions) — [framework § H-08](08_direct_solve/framework/todo.md#h-08--value-shape-clauses-helper-decisions)
- [ ] `when` in front of a clause and inside its sum (R1, S1) — [R1 § A `when` in front](08_direct_solve/rules/r1_continuous_knapsack/todo.md#a-when-in-front-of-the-clause-and-inside-the-sum)
- [ ] H-03 Read a data bound through the bound-side window — [framework § H-03](08_direct_solve/framework/todo.md#h-03--read-a-data-bound-through-the-bound-side-window)

## 2. Deferred until after the merge: one bound reader (Seam 2)

- [ ] One classifier for a bound on one decision, with the scaled solver-path fold — [code_quality § classified three times](06_issues/code_quality/todo.md#a-bound-on-one-decision-is-classified-three-times)
- [ ] A1 reads a folded affine bound — [A1 § Read a folded affine bound](08_direct_solve/rules/a1_separable_box/todo.md#read-a-folded-affine-bound)
- [ ] S1 reads a folded affine bound — [S1 § Read a folded affine bound](08_direct_solve/rules/s1_uniform_matroid/todo.md#read-a-folded-affine-bound)
- [ ] Direct solve rounds a bound just below a whole number differently — [bugs § integer bound](06_issues/bugs/todo.md#direct-solve-reads-an-integer-bound-just-below-a-whole-number-more-strictly-than-both-backends)

## 3. Bugs: wrong answers

- [ ] Negative lower bound under a `when` stays non-negative (decision) — [bugs § negative bound under `when`](06_issues/bugs/todo.md#a-negative-lower-bound-under-a-when-leaves-the-decision-non-negative)
- [ ] Negative `IN` value with a negative upper bound refused — [bugs § `IN`, bounds and rounding](06_issues/bugs/todo.md#wrong-answers-or-refusals-in-bounds-and-rounding)
- [ ] Gurobi calls an infinite floor unbounded — [bugs § `IN`, bounds and rounding](06_issues/bugs/todo.md#wrong-answers-or-refusals-in-bounds-and-rounding)
- [ ] MIN/MAX over a squared or multiplied body modelled wrongly — [bugs § MIN/MAX bodies](06_issues/bugs/todo.md#wrong-answers-minmax-bodies-and-solver-tolerance)
- [ ] Gurobi accepts a row violated by 1 near 10⁹ coefficients — [bugs § solver tolerance](06_issues/bugs/todo.md#wrong-answers-minmax-bodies-and-solver-tolerance)
- [ ] Key grouping ignores collation (decision) — [bugs § collation](06_issues/bugs/todo.md#key-grouping-ignores-collation)

## 4. Bugs: errors and hangs

- [ ] `DECIDB_TIME_LIMIT` not enforced on HiGHS for large models — [bugs § HiGHS time limit](06_issues/bugs/todo.md#decidb_time_limit-is-not-enforced-on-highs-for-large-models)
- [ ] HiGHS at gap 0 cannot finish an entity-scoped test MIP — [bugs § gap of 0](06_issues/bugs/todo.md#with-a-gap-of-0-highs-cannot-finish-an-entity-scoped-mip-the-test-suite-runs)
- [ ] Correlated DECIDE (LATERAL, outer reference) raises an internal error — [bugs § outer query](06_issues/bugs/todo.md#a-decide-that-refers-to-an-outer-query-raises-an-internal-error)
- [ ] Lambda in a coefficient raises an internal error — [bugs § Coefficients and keys](06_issues/bugs/todo.md#coefficients-and-keys)
- [ ] Prepared-statement and table-macro parameters fail in a clause — [bugs § parameters](06_issues/bugs/todo.md#parameters-of-a-prepared-statement-or-a-table-macro-fail-in-a-decide-clause)
- [ ] Scaled `norm(e, 0)` non-canonical on debug builds — [bugs § scaled norm](06_issues/bugs/todo.md#a-scaled-norme-0-leaves-a-constraint-the-debug-verifier-calls-non-canonical)
- [ ] Scaled `AVG` non-canonical on debug builds — [bugs § scaled AVG](06_issues/bugs/todo.md#a-scaled-avg-leaves-a-constraint-the-debug-verifier-calls-non-canonical)

## 5. Bugs: messages and diagnosis text

- [ ] Subquery, condition or `by` key printed as DuckDB renders it — [bugs § as DuckDB renders it](06_issues/bugs/todo.md#messages-print-a-subquery-a-condition-or-a-by-key-as-duckdb-renders-it)
- [ ] Stage-05 messages name columns without their relation — [bugs § without their relation](06_issues/bugs/todo.md#messages-raised-while-choosing-a-formulation-name-columns-without-their-relation)
- [ ] Data comparison inside an operand bound as a constraint — [bugs § operands](06_issues/bugs/todo.md#messages-operands-coefficients-and-keys)
- [ ] Two keys over the same columns share one key text — [bugs § operands](06_issues/bugs/todo.md#messages-operands-coefficients-and-keys)
- [ ] Refusal inside a nested DECIDE is masked — [bugs § nested DECIDE](06_issues/bugs/todo.md#messages-nested-decide-and-refusals)
- [ ] Quadratic bound echoed with the moved constant — [bugs § Diagnosis text](06_issues/bugs/todo.md#diagnosis-text)
- [ ] `BETWEEN` echoes its lower half only — [bugs § Diagnosis text](06_issues/bugs/todo.md#diagnosis-text)
- [ ] Unbounded remedy names a column unquoted — [bugs § Diagnosis text](06_issues/bugs/todo.md#diagnosis-text)
- [ ] Quadratic clause reported once per row in query scope — [bugs § Diagnosis text](06_issues/bugs/todo.md#diagnosis-text)
- [ ] HiGHS expanded repair carries a 1e-5 cushion — [bugs § Diagnosis text](06_issues/bugs/todo.md#diagnosis-text)
- [ ] NULL slice prescribes a cap matching no row — [unbounded § 1](07_query_diagnostics/unbounded/todo.md#1-a-null-slice-prescribes-a-cap-that-matches-no-row) (bugs § Coefficients and keys points here)
- [ ] NULL class reads like the string `'NULL'` (decision) — [infeasible § 2](07_query_diagnostics/infeasible/todo.md#2-a-null-class-reads-like-the-string-null)
- [ ] `=` on MIN/MAX echoed as one half (decision) — [infeasible § 1](07_query_diagnostics/infeasible/todo.md#1-an--on-a-minmax-is-echoed-as-the-half-its-finding-loosens)
- [ ] EXPLAIN columns print by bare name over a join (decision) — [explain](03_expressivity/explain/todo.md#columns-print-by-their-bare-name-over-a-join)

## 6. Bugs: benchmark and test tooling

- [ ] Q10 on HiGHS large tier killed for memory, logged as timeout — [bugs § Q10](06_issues/bugs/todo.md#q10-on-highs-at-the-large-tier-is-killed-for-memory-and-the-profiler-records-it-as-a-timeout)
- [ ] One off-mode matrix run reported one outcome fewer — [bugs § one outcome fewer](06_issues/bugs/todo.md#one-matrix-run-of-decidb_test_direct_solveoff-reported-one-outcome-fewer)

## 7. Testing gaps

- [ ] No test sees a solver's optimality gap — [edge_cases](04_testing/edge_cases/todo.md#no-differential-test-is-large-enough-to-see-a-solvers-optimality-gap)
- [ ] `group_indices` drops NULL keys — [testing/scope § group_indices](04_testing/scope/todo.md#group_indices-drops-null-keys)

## 8. Code quality and naming

- [ ] One `DecideClauseScope` for a clause's scope copies (D47) — [code_quality § parallel copies](06_issues/code_quality/todo.md#a-clauses-scope-is-carried-in-parallel-copies)
- [ ] One step chooses direct solve, Gurobi or HiGHS — [code_quality § chosen in two places](06_issues/code_quality/todo.md#how-a-query-is-answered-is-chosen-in-two-places)
- [ ] One key table on `LogicalDecide` (decision) — [logical_plan § 1](01_pipeline/03_logical_plan/todo.md#1-one-key-table)
- [ ] One key-grouping path at execution (decision) — [execution § 1](01_pipeline/08_execution/todo.md#1-one-key-grouping-path-optional)
- [ ] Rename the keyed scope "keyed" everywhere (D46) — [expressivity/scope](03_expressivity/scope/todo.md#name-the-keyed-scope-keyed-everywhere)
- [ ] Rename test files and markers with removed spellings (decision) — [testing/scope § names](04_testing/scope/todo.md#names-that-still-say-the-removed-spellings)
- [ ] Normalize the DecidB spelling in comments — [operations](02_operations/todo.md#normalize-the-decidb-spelling-in-internal-comments)

## 9. Language: refusals to lift (all decisions)

- [ ] Nested aggregates: which shapes enter the language — [binder § 1](01_pipeline/02_binder/todo.md#1-room-for-nested-aggregates)
- [ ] Nested aggregates: the value rewrite recurses — [optimizer § 4](01_pipeline/05_optimizer/todo.md#4-a-reducer-inside-a-reducer) (needs: binder § 1)
- [ ] Seven bind-time refusals — [binder § 2](01_pipeline/02_binder/todo.md#2-bind-time-refusals-a-later-change-may-lift)
- [ ] Three stage-05 refusals — [optimizer § 5](01_pipeline/05_optimizer/todo.md#5-refusals-a-later-change-may-lift)
- [ ] `AVG` under `<>` beside another term — [canonicalizer § 1](01_pipeline/04_canonicalizer/todo.md#1-avg-under--beside-another-term-or-with-its-own-when-or-per)
- [ ] A decision as a factor on a reducer — [canonicalizer § 2](01_pipeline/04_canonicalizer/todo.md#2-a-decision-as-a-factor-on-a-reducer) (same as bugs § Limitations, first item)
- [ ] Quadratic row refuses a row-varying factor on a query-wide decision — [bugs § Limitations](06_issues/bugs/todo.md#limitations-not-bugs)
- [ ] A NULL key is not a class; its rows skip the keyed clause (reverses D5; scope questions open) — [scope § NULL key](03_expressivity/scope/todo.md#a-null-key-is-not-a-class-its-rows-skip-the-keyed-clause-reverses-d5)
- [ ] Functions that behave unlike SQL: audit, and decide on NULL in SUM/MIN/MAX/AVG (decision) — [sql_functions § unlike SQL](03_expressivity/sql_functions/todo.md#functions-that-behave-unlike-sql)
- [ ] SOCP (deferred, revisit on a concrete query) — [problem_types](03_expressivity/problem_types/todo.md#socp-second-order-cone-programming)

## 10. Smaller models

- [ ] One helper for a reducer several clauses hold (decision) — [optimizer § 1](01_pipeline/05_optimizer/todo.md#1-one-helper-for-a-reducer-that-several-clauses-hold)
- [ ] A keyed decision beside a reducer without a helper (decision) — [canonicalizer § 3](01_pipeline/04_canonicalizer/todo.md#3-a-keyed-decision-beside-a-reducer-without-a-helper)
- [ ] `IN` under `per K:` once per class — [optimizer § 3](01_pipeline/05_optimizer/todo.md#3-in-under-per-k-once-per-class)
- [ ] A box per helper instance (decision) — [optimizer § 2](01_pipeline/05_optimizer/todo.md#2-a-box-per-helper-instance) (needs: the helper-box measurement in 12)

## 11. Performance measurement (run last)

- [ ] Measurement overhead, a correctness check, a second host — [methodology](05_performance/01_methodology/todo.md)
- [ ] Backend discovery versus parsing and planning — [planning](05_performance/02_planning/todo.md)
- [ ] Rank extraction work; add the scope-check spans to `extraction` — [input and extraction](05_performance/03_input_and_extraction/todo.md)
- [ ] Rank formulation families; measure the shared helper box — [model construction](05_performance/04_model_construction/todo.md)
- [ ] Gurobi and HiGHS loading overhead — [backend loading](05_performance/05_backend_loading/todo.md)
- [ ] Scale ladders on both backends — [solver execution](05_performance/06_solver_execution/todo.md)
- [ ] Readback, temporary results and cleanup — [output and cleanup](05_performance/07_output_and_cleanup/todo.md)
- [ ] Where each workload stops fitting in memory — [memory and scaling](05_performance/08_memory_and_scaling/todo.md)

## 12. Direct solve: the second front end

- [ ] H-02 Front end for the new language — [framework § H-02](08_direct_solve/framework/todo.md#h-02--front-end-for-the-new-language)

## 13. Operations

- [ ] Report peak process memory on Windows — [operations](02_operations/todo.md#report-peak-process-memory-on-windows)
