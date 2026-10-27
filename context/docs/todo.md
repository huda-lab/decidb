# Work queue: every open task in the docs

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `language/guide.md`, `direct_solve/README.md`, `issues/bugs.md`.
Checks: [direct_solve/framework/todo.md § How to work](direct_solve/framework/todo.md#how-to-work-any-task-in-these-todo-files).

The queue has three parts. **Part A** an agent works without the owner. **Part B** needs the owner once, before
coding, at the question written on the line. **Part C** the owner runs, or is not ready.

## Rules for working without the owner (Part A)

- **Done** is the nested task's "Done when" or "Test" line. A behavior change is judged by an oracle or the solver
  path, on both backends.
- **Stop and ask** when a fix needs a choice the nested doc does not settle, a test fails in a way the task did not
  predict, a golden changes that the task did not name, or a user-facing message needs wording the doc does not give.
  Never change what a test checks to make it pass. A task that turns out to need the owner moves to Part B with its
  question.
- **Never without the owner:** large-tier benchmarks or other long CPU-bound runs, regenerating goldens before reading
  the diff, pushing, new branches or worktrees, `git stash`.
- **Per item:** root-cause fix; a `decidb-architect` review for a structural change; re-run the tests; move the shipped
  text into the owning `done.md`; delete the line here; commit (no push).
- **Per batch:** `make decide-test`; `DECIDB_FORCE_SOLVER=highs make decide-test` for solver changes;
  `DECIDB_VERIFY_SERIALIZER=1 make decide-test` for serialization changes; `DECIDB_TEST_DIRECT_SOLVE=off make
  decide-test` for direct-solve changes.
- **Order:** top to bottom. A line marked *(needs: …)* waits for that item.

---

# Part A — Agent works alone

## A3. Direct solve reads the new language: grouping

Every line waits for B1 (the NULL-key rule changes how a key becomes groups) and B5 (H-04).

- [ ] H-05 Read a reducer's `by` key into the IR as a GROUP BY in the direct plan (key on the reducer; ~280 keyed cases are strict xfails on the direct path until then) — [framework § H-05](direct_solve/framework/todo.md#h-05--read-a-reducers-by-key-into-the-ir) (needs: B5, B1)
- [ ] S1 reads a `by` key again — [S1 § Read a `by` key](direct_solve/rules/s1_uniform_matroid/todo.md#read-a-by-key-again-counts-per-group) (needs: H-05)
- [ ] R1 reads a `by` key again — [R1 § Read a `by` key](direct_solve/rules/r1_continuous_knapsack/todo.md#read-a-by-key-again-one-budget-per-group) (needs: H-05)
- [ ] H-06 Two-level objective `outer(per k: inner(e) by (k))` — [framework § H-06](direct_solve/framework/todo.md#h-06--the-two-level-objective-outerper-k-innere-by-k) (needs: B1)
- [ ] H-07 `per K:` on a constraint — [framework § H-07](direct_solve/framework/todo.md#h-07--per-k-on-a-constraint-one-constraint-per-class) (needs: B5, B1)

## A5. Language: refusals to lift

Each is ruled; the nested section says which shapes lift and which stay refused.

- [ ] Lift `norm(D:)` and the strict comparison over a fractional MIN/MAX — [binder § 1](pipeline/02_binder/todo.md#1-bind-time-refusals-a-later-change-may-lift)
- [ ] Lift a decision beside MIN/MAX in the objective — [optimizer § 4](pipeline/05_optimizer/todo.md#4-refusals-a-later-change-may-lift)
- [ ] A `BOOL` decision as a factor on a reducer — [canonicalizer § 1](pipeline/04_canonicalizer/todo.md#1-a-decision-as-a-factor-on-a-reducer) (same as bugs § Limitations, first item)
- [ ] Quadratic row refuses a row-varying factor on a query-wide decision — [bugs § Limitations](issues/bugs.md#limitations-not-bugs)
- [ ] Functions that behave unlike SQL: a data-only reducer skips NULL; audit the rest and bring each divergence to the owner — [binder § 2](pipeline/02_binder/todo.md#2-functions-that-behave-unlike-sql)

## A6. Smaller models

- [ ] One helper for a reducer several clauses hold — [optimizer § 1](pipeline/05_optimizer/todo.md#1-one-helper-for-a-reducer-that-several-clauses-hold)
- [ ] A keyed decision beside a reducer without a helper (plan-time key comparison) — [canonicalizer § 2](pipeline/04_canonicalizer/todo.md#2-a-keyed-decision-beside-a-reducer-without-a-helper)
- [ ] `IN` under `per K:` once per class — [optimizer § 3](pipeline/05_optimizer/todo.md#3-in-under-per-k-once-per-class)

## A7. Messages and diagnosis text

- [ ] `=` on MIN/MAX echoed as one half (quote as written) — [infeasible § 1](diagnostics/infeasible/todo.md#1-an--on-a-minmax-is-echoed-as-the-half-its-finding-loosens)
- [ ] EXPLAIN columns print by bare name over a join (qualify over a join) — [logical plan § 2](pipeline/03_logical_plan/todo.md#2-columns-print-by-their-bare-name-over-a-join)
- [ ] Two keys over the same columns share one key text — [bugs § operands](issues/bugs.md#messages-operands-coefficients-and-keys)
- [ ] A column reference is named in two forms (key SQL spelling vs bare name) — [bugs § operands](issues/bugs.md#messages-operands-coefficients-and-keys)
- [ ] `BETWEEN` echoes its lower half only — [bugs § Diagnosis text](issues/bugs.md#diagnosis-text)

## A8. Testing gaps

- [ ] `MAX(x) <= MAX(v)` has no test — [testing § MAX bound](testing/todo.md#maxx--maxv-has-no-test)
- [ ] DIAGNOSE of a hard `MIN`/`MAX` over a keyed decision has no test — [testing § DIAGNOSE MIN/MAX](testing/todo.md#diagnose-of-a-hard-minmax-over-a-keyed-decision-has-no-test)

## A9. Code quality and naming

- [ ] One `DecideClauseScope` for a clause's scope copies (D47) — [code_quality § parallel copies](issues/code_quality.md#a-clauses-scope-is-carried-in-parallel-copies)
- [ ] One step chooses direct solve, Gurobi or HiGHS — [code_quality § chosen in two places](issues/code_quality.md#how-a-query-is-answered-is-chosen-in-two-places)
- [ ] Rename the keyed scope "keyed" everywhere (D46), with the test files and markers in the same change — [code_quality § keyed](issues/code_quality.md#name-the-keyed-scope-keyed-everywhere), [testing § names](testing/todo.md#names-that-still-say-the-removed-spellings)
- [ ] Normalize the DecidB spelling in comments — [operations](operations/todo.md#normalize-the-decidb-spelling-in-internal-comments)

---

# Part B — Owner checkpoint before coding

The agent may read code and investigate; it writes no fix until the owner answers the question on the line.

## B0. Direct solve: one bound reader (Seam 2)

One rule for "a bound on one decision", read at plan time by the optimizer, the model builder and direct solve.
Owner answers, before coding:

1. The shared rounding rule (a whole-numbered decision admits `n` when `coefficient * n` passes the written
   comparison in doubles) changes two backend answers: `x <= c/0.1` with `c = 4.3` and `x <= 42.9999999` go from 43
   to 42 on Gurobi and HiGHS, as direct solve answers today; `0.1*x <= 4.3` stays 43. Accept? (Recommended: yes;
   the alternative copies the backends' integrality tolerance into the model.)
2. Does "every bound on one decision folds" cover a data coefficient (`cap * x <= 10`)? A row with `cap = 0` has no
   quotient. (Recommended: no; only a coefficient known before the query runs.)

- [ ] One classifier for a bound on one decision, with the scaled solver-path fold — [code_quality § classified three times](issues/code_quality.md#a-bound-on-one-decision-is-classified-three-times)
- [ ] A1 reads a folded affine bound — [A1 § Read a folded affine bound](direct_solve/rules/a1_separable_box/todo.md#read-a-folded-affine-bound)
- [ ] S1 reads a folded affine bound — [S1 § Read a folded affine bound](direct_solve/rules/s1_uniform_matroid/todo.md#read-a-folded-affine-bound)
- [ ] Direct solve rounds a bound just below a whole number differently — [bugs § integer bound](issues/bugs.md#direct-solve-reads-an-integer-bound-just-below-a-whole-number-more-strictly-than-both-backends)

## B1. NULL keys and key grouping (one batch; blocks A3)

Ruled: a NULL key is not a class, one numbering, keys follow collation. All four touch the same grouping code.
Owner answers, before coding:

1. **Numbering.** Today a keyed decision's classes are numbered first-seen over every row (`BuildEntityMappings`), and
   each `by`/`per K:` clause renumbers the groups it keeps densely, in first-kept-row order
   (`LookupOrBuildPerGroupIds`). Recommended: one id per class, first-seen over every row, and each clause reads the
   classes it keeps as a subsequence in that order, so equal keys get equal ids in every clause. A dump changes only
   where a `when` drops a class's first row (its groups then come out in a different order).
2. **NULL keyed-decision error.** A row whose key is NULL under `DECIDE per g: x(INT)` raises. Recommended text:
   *"DECIDE: 'g' is NULL on some rows, so they have no 'x' (declared per g). Add WHERE g IS NOT NULL."*
3. **Golden diff.** Expected to change: entries whose keys hold NULL (the NULL class leaves `by`, `per K:` and
   `per`-in-reducer clauses) and entries where a `when` drops a class's first row (question 1). Entries 22, 23, 38,
   39, 56, 61, 62, 65, 70 and 91–94 print ids. The agent shows the diff, entry by entry, before re-capturing.

- [ ] A NULL key is not a class; its rows skip the keyed clause (reverses D5; closes infeasible § 2) — [execution § 2](pipeline/08_execution/todo.md#2-a-null-key-is-not-a-class-reverses-d5)
- [ ] One key table on `LogicalDecide` (one numbering; re-capture goldens) — [logical_plan § 1](pipeline/03_logical_plan/todo.md#1-one-key-table)
- [ ] One key-grouping path at execution (with the key table) — [execution § 1](pipeline/08_execution/todo.md#1-one-key-grouping-path-optional)
- [ ] Key grouping ignores collation (follow the collation) — [bugs § collation](issues/bugs.md#key-grouping-ignores-collation)
- [ ] `group_indices` drops NULL keys (both oracle helpers drop a NULL key) — [testing § group_indices](testing/todo.md#group_indices-drops-null-keys)

## B2. Wrong answers whose fix has an open choice

- [ ] MIN/MAX over a squared or multiplied body modelled wrongly (ruled: refuse the wrong directions) — owner approves the exact list of refused forms and the message — [bugs § MIN/MAX bodies](issues/bugs.md#wrong-answers-minmax-bodies-and-solver-tolerance)
- [ ] Gurobi accepts a row violated by 1 near 10⁹ coefficients (ruled: warn) — owner sets the span threshold and where the warning shows — [bugs § solver tolerance](issues/bugs.md#wrong-answers-minmax-bodies-and-solver-tolerance)
- [ ] Gurobi calls an infinite floor unbounded — owner rules what an infinite bound means: refuse it, or read it as no bound — [bugs § infinite bounds](issues/bugs.md#wrong-answers-infinite-bounds)
- [ ] HiGHS at gap 0 cannot finish an entity-scoped test MIP — owner approves the proposed `decide_mip_gap` setting (a new user setting) — [bugs § gap of 0](issues/bugs.md#with-a-gap-of-0-highs-cannot-finish-an-entity-scoped-mip-the-test-suite-runs)

- [ ] Negative lower bound under a `when` stays non-negative (ruled: applies to the kept rows) — owner picks the
  representation. Only an unconditional constant bound replaces the default floor of 0 today (a data bound
  `x >= lo` only intersects, D41), and `SolverInput` has one box per decision, so a floor lowered on some rows needs
  either (1) hidden `x >= 0` clauses on the rows or classes no such `when` keeps (DIAGNOSE must keep them rigid and
  unquoted), or (2) a floor per instance in the stage 06/08 contract, which item 19 (a box per helper instance) also
  needs. Recommended: (2). A1 must read the same floor. — [bugs § negative bound under `when`](issues/bugs.md#a-negative-lower-bound-under-a-when-leaves-the-decision-non-negative)

## B3. Messages whose fix has an open route

- [ ] Stage-05 messages name columns without their relation — owner picks: carry relation names into stage 05, or move the refusal to stage 04 — [bugs § without their relation](issues/bugs.md#messages-raised-while-choosing-a-formulation-name-columns-without-their-relation)
- [ ] Subquery, condition or `by` key printed as DuckDB renders it — the parser must keep each node's source span; owner approves the parser plan — [bugs § as DuckDB renders it](issues/bugs.md#messages-print-a-subquery-a-condition-or-a-by-key-as-duckdb-renders-it)

## B4. Investigate first, then bring the fix

The agent finds the cause alone and stops with a proposed fix.

- [ ] `DECIDB_TIME_LIMIT` not enforced on HiGHS for large models — the interrupt poll carries Ctrl-C only
  (`context.interrupted`), so overriding it does not bound the solve; HiGHS stopped checking its own `time_limit` in
  the phase it stalled in. Next: find that phase from the vendored source (`third_party/highs`; interrupt callbacks
  exist for MIP and simplex) and propose a deadline callback; only the owner's large-tier run (Q2, HiGHS) confirms
  the fix. — [bugs § HiGHS time limit](issues/bugs.md#decidb_time_limit-is-not-enforced-on-highs-for-large-models)
- [ ] Quadratic clause reported once per row in query scope — [bugs § Diagnosis text](issues/bugs.md#diagnosis-text)
- [ ] HiGHS expanded repair carries a 1e-5 cushion — [bugs § Diagnosis text](issues/bugs.md#diagnosis-text)

## B5. Direct solve: keyed scope checks, helper decisions, reading through the window

- [ ] H-04 Keyed scope checks in the direct plan — owner picks how both paths choose what a message names when the
  answer depends on row order: which failing group (several fail), and which two values a determination message
  quotes (`more than one value: 7, 5`; the solver quotes the first value seen, then the first that differs). A
  relational plan has no reliable row order; its existing checks raise the least message. Recommended: both paths
  name the least failing group and quote the least and greatest values, so the message never depends on row
  order; the solver's pins that quote another order change with it. — [framework § H-04](direct_solve/framework/todo.md#h-04--keyed-scope-checks-in-the-direct-plan)

- [ ] H-08 VALUE-shape clauses (helper decisions) — owner names the rule and clause shape it is for: no rule's
  definition takes a VALUE-shape clause, so reading the helper into the IR alone reaches no code. Recommended: leave
  it until a rule names the shape it needs (then the task is that rule's). — [framework § H-08](direct_solve/framework/todo.md#h-08--value-shape-clauses-helper-decisions)

- [ ] H-03 Read a data bound through the bound-side window — owner picks where the reduction lives. (1) The rewrite
  records each reduced bound (raw value, MIN/MAX/`=`, its value-check messages) as one value object; the solver path
  turns it into windows only when no rule takes the query, and a rule that reads through computes it in its own group
  aggregate (R1 already takes the group MIN/MAX of a data limit). (2) The rewrite installs the windows as today and a
  rule that reads through removes them, rebinding shared, stacked window slots. Recommended: (1), the one place that
  states the tightest bound; nothing is built to be torn down. Either way the value checks move into the rule's pass
  and must keep the solver's order (scope checks, then the bound's NULL/NaN, then weights). — [framework § H-03](direct_solve/framework/todo.md#h-03--read-a-data-bound-through-the-bound-side-window)

---

# Part C — Owner runs, or not ready

## C1. Long or memory-heavy runs (owner starts them)

- [ ] Q10 on HiGHS large tier killed for memory, logged as timeout — [bugs § Q10](issues/bugs.md#q10-on-highs-at-the-large-tier-is-killed-for-memory-and-the-profiler-records-it-as-a-timeout)
- [ ] No test sees a solver's optimality gap (needs a large dataset) — [testing § optimality gap](testing/todo.md#no-differential-test-is-large-enough-to-see-a-solvers-optimality-gap)
- [ ] A box per helper instance (deferred until the helper-box measurement below shows it matters) — [optimizer § 2](pipeline/05_optimizer/todo.md#2-a-box-per-helper-instance)

Performance measurement (run last):

- [ ] Measurement overhead, a correctness check, a second host — [methodology](performance/todo.md#methodology)
- [ ] Backend discovery versus parsing and planning — [planning](performance/todo.md#planning)
- [ ] Rank extraction work; add the scope-check spans to `extraction` — [input and extraction](performance/todo.md#input-and-extraction)
- [ ] Rank formulation families; measure the shared helper box — [model construction](performance/todo.md#model-construction)
- [ ] Gurobi and HiGHS loading overhead — [backend loading](performance/todo.md#backend-loading)
- [ ] Scale ladders on both backends — [solver execution](performance/todo.md#solver-execution)
- [ ] Readback, temporary results and cleanup — [output and cleanup](performance/todo.md#output-and-cleanup)
- [ ] Where each workload stops fitting in memory — [memory and scaling](performance/todo.md#memory-and-scaling)

## C2. Waiting on something

- [ ] One off-mode matrix run reported one outcome fewer (waits for a recurrence; the matrix now keeps its logs) — [bugs § one outcome fewer](issues/bugs.md#one-matrix-run-of-decidb_test_direct_solveoff-reported-one-outcome-fewer)
- [ ] Report peak process memory on Windows (needs a Windows host) — [operations](operations/todo.md#report-peak-process-memory-on-windows)
- [ ] H-02 Front end for the new language (owner confirms it still applies: it waited for "a stable language branch", and the merge brought one language) — [framework § H-02](direct_solve/framework/todo.md#h-02--front-end-for-the-new-language)
