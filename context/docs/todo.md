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

- [ ] Functions that behave unlike SQL: a data-only reducer skips NULL; audit the rest and bring each divergence to the owner — [binder § 1](pipeline/02_binder/todo.md#1-functions-that-behave-unlike-sql)

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

## B7. A `BOOL` decision as a factor on a reducer

- [ ] The design (VALUE shape: a helper `v` for `sum(x)`, then `b * v` through the bilinear rewrite) assumes an order
  the optimizer does not have: `RewriteBilinear` runs before `RewriteScopedReducers`, on purpose, so the value rewrite
  reads bodies whose `BOOL` products are already linear. A `b * v` the value rewrite makes would reach the linear form
  unlinearized. (1) How does `b * v` get linearized? (a) The value rewrite hands each product it makes to the bilinear
  rewrite's one-product linearization (one linearizer, order kept); (b) move `RewriteBilinear` after the value rewrite
  and let the value rewrite read `BOOL` products in bodies. Recommended: (a). (2) Is the objective lifted too
  (`MAXIMIZE b * sum(x)`)? It has no helper path. Recommended: constraints now; the objective as its own item. The
  `INT` pins (`test_canonicalize_scale.py::test_decision_factor_rejected`, `::test_decision_factor_in_objective_rejected`)
  stay as they are. — [canonicalizer § 1](pipeline/04_canonicalizer/todo.md#1-a-decision-as-a-factor-on-a-reducer)

## B6. Model builder: one encoding of "count once"

- [ ] Stage 08 zeroes every row but one of a term counted once, and the model builder encodes it again from
  `objective_term_reductions` / `linear_term_reductions`, reading a missing entry as "once, from row 0" (it caused a
  wrong answer, now fixed at the writer). Pick one encoding: (1) the builder sums every row and the reduction
  arrays leave `SolverInput`, or (2) the builder keeps them and refuses arrays whose sizes differ. Recommended: (1),
  one fact in one place. — [code_quality § count once](issues/code_quality.md#count-once-is-encoded-twice)

---

# Part C — Owner runs, or not ready

## B8. A quadratic SUM behind a helper decision

- [ ] Lifting the quadratic-row refusal (`sum(power(x, 2)) + gcap * s <= 30`) means a helper `v` for a quadratic
  `SUM`. Today every `SUM` helper is defined by an equality, `v = Σ e`, which for a square is a nonconvex quadratic
  equality, so a convex clause would become a nonconvex model (Gurobi spatial branch and bound; HiGHS refuses either
  way). (1) How is the helper defined? (a) Equality, emitted through the quadratic-row path: simplest, but the model
  class changes. (b) The epigraph form, the side the rows push against, as `MIN`/`MAX` helpers already do
  (`need_envelope`/`need_closing`): `v >= Σ e` when the rows push the term down, `v <= Σ e` when up, the equality only
  for `=`, `<>` or an unknown sign. The clause keeps its convexity, and nothing reads a helper's value back, so a slack
  `v` is harmless. Recommended: (b), applied to every `SUM`/`AVG` helper, linear ones included, so there is one rule;
  linear models change from `=` to `>=`/`<=` rows on one side (same answers, golden dumps that print them change).
  (2) Apply (b) to linear helpers too, or only to quadratic ones? Recommended: all, one rule.
  — [bugs § Limitations](issues/bugs.md#limitations-not-bugs)

## B10. One helper for a reducer several clauses hold: the definition's provenance

- [ ] Sharing a helper between clauses (`x <= 0.5 * sum(x) AND x >= 0.1 * sum(x)`) needs one rule for its definition
  row, which today carries one clause's `source_clause_id` and `removal_group_id`. (a) The definition belongs to no
  clause: a DROP of either clause leaves it, which is harmless (an equality on a free helper always holds), and
  DIAGNOSE reads it as part of the fixed model, like every untagged structural row
  (`decide_diagnostic_engines.cpp`). (b) The definition carries every reader's removal group and is omitted when the
  last one is dropped: a set of ids on a row, where every other row has one. Recommended: (a), the standard reading of
  a defined auxiliary (it is not a constraint of any clause), with no new field.
  — [optimizer § 1](pipeline/05_optimizer/todo.md#1-one-helper-for-a-reducer-that-several-clauses-hold)

## B12. A keyed decision beside a reducer: four design choices (architect review done)

- [ ] The shape is the aggregate shape with a tuple term of one value per group; it does not wait for B6. Choices:
  (1) The classifier needs the clause's `per` wrapper and the key lists, but the shape is recomputed from the bare
  comparison at five sites (`decide_linear_form.cpp`, `direct_ir.cpp`, `decide_rewrite_rhs_window.cpp`, twice in
  the canonicalizer). Decide once in `CanonicalizeTreeInternal` with the enclosing scope and stamp a tag (as
  `VALUE_REWRITE_TAG` is), or pass the wrapper to every site? Recommended: the tag.
  (2) What makes the coefficient (`cap` in `cap * open`) one value per group. A wrapper's DETERMINES check gives one
  per class, not per group (`per S.shipmentID: sum(ship) by (D) <= S.cap * open` passes it while `cap` varies in a
  depot). Admit when the wrapper key, the `by` key and the decision's `per` key are equal; also admit a query-wide
  coefficient with no wrapper? Without a wrapper a data coefficient stays VALUE (one constraint per row).
  Recommended: both cases.
  (3) One `CountsOnce(term)` test replaces the five copies in the model builder and `linearization_not_equal.cpp`:
  keep the "not row-scoped" guard, or throw on reduction arrays shorter than the terms? Recommended: keep the guard.
  (4) Extract `CountsOnce` first as its own change, or inside this item? Recommended: first.
  — [canonicalizer § 2](pipeline/04_canonicalizer/todo.md#2-a-keyed-decision-beside-a-reducer-without-a-helper)

## B13. `IN` under `per K:`: which scope keys the indicators

- [ ] Per class, the indicators need a decision scope keyed by the wrapper's `K`. Today a scope is made only by the
  binder (declarations, reducer keys) and by the value rewrite (helpers); the one key table
  (`pipeline/03_logical_plan/todo.md` §1, batch B1) is the planned single way to make one from any key. (a) Wait for
  B1 and key the indicators by `K`'s entry. (b) Now, for the case where `K` is the decision's own declared key: the
  indicators take the decision's scope and the rows keep the wrapper (as the MIN/MAX easy half keeps it); any other
  `K` keeps one set per row. Recommended: (a); (b) adds a special case that B1 removes.
  — [optimizer § 3](pipeline/05_optimizer/todo.md#3-in-under-per-k-once-per-class)

## B14. `=` on MIN/MAX: which check proves the moved equality holds

- [ ] The ruling quotes `max(e) = K` as written and suggests `max(e) = K'` when the other half still holds at `K'`.
  The clause's written comparison is in reach (`ConstraintSourceInfo::canonical_cmp`). The check is open. (a) Read
  the repair's own solution (`ReadElasticEdits` holds it): the equality holds when every instance's extremum over its
  members equals `K'`. The members are the easy half's rows (`2 * x_i <= 20`), which carry no instance identity under
  `by (g)` or `per g:` (the hard half is an extremum column per group), so the rows need a new instance id, and the
  check is conservative: it falls back whenever the solution does not happen to sit at `K'`. (b) Verify by one more
  solve, the original model with both halves (and the other findings' edits) moved, feasible or not: exact, the same
  for the lowered rows and Gurobi's native MAX, one solve per diagnosis that reports an `=` MIN/MAX. (c) Suggest a
  range instead, `max(2 * x) BETWEEN 6 AND 20`, which always repairs and needs no check; it is B16's mechanism.
  Recommended: (c) if B16 lands, else (b). The fallback is today's output, which names the half
  (`max(2 * x) >= 20` → `max(2 * x) >= 6`).
  — [infeasible § 1](diagnostics/infeasible/todo.md#1-an--on-a-minmax-is-echoed-as-the-half-its-finding-loosens)

## B15. Display text per occurrence: qualify over a join (D45), one name form, one key text per clause (architect review done)

- [ ] One root under three A7 lines: display text lives on shared objects (`EntityScopeInfo.table_alias`, a key
  reference's `name` set to its SQL spelling by `AddKeyColumn`, `decide_binder.cpp:390`) instead of on each
  occurrence. A table-index → alias map on the operator would go stale: DuckDB renumbers DECIDE's bindings
  (`ColumnBindingReplacer`, `RemoveUnusedColumns`) and the physical plan has none. Proposed: the binder gives each
  data column reference its bare name plus a relation tag pointing into `source_fragments` (over two or more
  relations only), `RenderSource` prints `relation.name`, and every `relations`/`no_relations` parameter goes; a
  reducer's written key is recorded per occurrence the same way (a new tag kind; `__source_fragment_` would replace
  the whole reducer). Needs B11 first: alias tags already leak into direct-plan EXPLAIN. Choices: (1) relation text in
  `source_fragments`, in a new `relation_names` registry (serialized field), or in the tag payload? Recommended:
  `source_fragments`. (2) Tag where each reference is bound, or in one pass after binding? Recommended: where bound.
  (3) Land the per-occurrence key text with this change or with the one key table (B1)? Recommended: with this
  change, before B1, which merges more keys into one entry. (4) Does a key written bare (`per d:`) echo qualified over
  a join (D45 qualifies columns; keys "keep as written")? Recommended: as written.
  — [logical plan § 2](pipeline/03_logical_plan/todo.md#2-columns-print-by-their-bare-name-over-a-join),
  [bugs § operands](issues/bugs.md#messages-operands-coefficients-and-keys)

## B16. A `BETWEEN` is quoted as written: six design calls (architect review done)

- [ ] `ExpandBetween` (`decide_constraints_binder.cpp`) splits `x BETWEEN 18 AND 25` into two comparisons before the
  source registry sees it, so a finding names one half and the paste-back loses the other. Design: keep one source id
  per half (one id for both is unsound: `FinalizeConstraintSourceInfo` writes canonical text, `rhs_kind` and
  `source_*` once per id, so one half overwrites the other) and link both to the written clause (a many-to-one
  source map, the Composite pattern): a binder tag pairs the halves, `written_*` holds the whole clause, one pass
  after `ReadElasticEdits` spells `x BETWEEN 10 AND 25` from the moved end and the partner's text, merging two moved
  ends into one edit (the paste-back cannot find the clause after the first). EXPLAIN groups the halves. Choices:
  (1) the link as fields on each half, or a separate written-clause vector? Recommended: fields. (2) When
  canonicalization moved a term (`x + 2 BETWEEN 18 AND 25`), quote the canonical `x BETWEEN 16 AND 23` or the
  written text with an offset? Recommended: as the comparisons are quoted today (canonical). (3) Both ends moved:
  `amount` is the sum, or one row per end? Recommended: one row, the sum. (4) The same mechanism serves B14 (c).
  (5) EXPLAIN's canonical line for a BETWEEN: one line `x >= 18 AND x <= 25`, or two rows under the clause?
  Recommended: two rows, as a split MIN/MAX shows. (6) DROP of one half of a `norm(...) BETWEEN`: drop the clause, or
  keep the other half? Recommended: drop the clause, as written. The test at `test_query_diagnostics_relation.py`
  mapping `x <= 5` to `x BETWEEN 0 AND 5` hides this bug and goes with the fix.
  — [bugs § Diagnosis text](issues/bugs.md#diagnosis-text)

## B17. The DecidB spelling: the acceptance test contradicts "no identifier changes"

- [ ] The item asks for zero `DecidB` matches outside `context/docs/` and for no identifier changes. 476 matches remain
  (65 in `src/`, most in `test/decide/`), and 130 of them are names: the test harness classes `DecidBCli` and
  `DecidBCliError` (129 uses), the workflow file `.github/workflows/DecidBRelease.yml`, and CMake's
  `project(DecidB)` (the build's project name). Choices: (a) rename those too (`DeciDBCli`, `DeciDBRelease.yml`,
  `project(DeciDB)`), one mechanical change; (b) comments and prose only, and the acceptance test excludes the names.
  Recommended: (a), since the test classes are internal and the spelling would otherwise stay. Do it after the held
  NULL item (B11) commits: four of its files carry the old spelling.
  — [operations](operations/todo.md#normalize-the-decidb-spelling-in-internal-comments)

## B18. One step chooses how a query is answered: seven calls (architect review done)

- [ ] Verified: the direct rules need the window rewrite's output and not the solver choice; `ChooseDecideSolver`
  needs neither, so one step can hold both without changing which queries go direct. Design: Strategy selection with
  fallback (try the direct rules, else choose a solver), the window rewrite as pass 0 inside `OptimizeDecide`, the
  policy gate and both `require` errors moved out of `direct_rule_driver.cpp` into the step. Making direct solve a
  solver-registry entry is not recommended: it rewrites the plan, it is not a solver session. Choices: (1) choose the
  backend lazily (only on the solver path) or eagerly? Recommended: lazily; eager makes every first direct query
  probe Gurobi's licence and changes the error for `require` plus an unknown `DECIDB_FORCE_SOLVER`. (2)
  `DECIDB_FORCE_SOLVER=""` is read two ways today: direct solve treats it as unset, `SelectSolverBackend` throws "is
  not a solver DeciDB knows". One reader must pick one. Recommended: empty means unset, with a test. (3) What
  `optimizer_ms` and the `optimizer.decide` span cover: the whole stage on both paths (direct queries start printing
  it), or a separate `direct_ms`? Recommended: the whole stage; update `pipeline_profiler.md` and `tpch_suite.md`.
  (4) `LogicalDecide::optimized` is documented as "the optimizer started rewriting", which the window rewrite already
  breaks; set it in `ChooseDecideSolver` and document it as "the solver formulation has begun", or rename it
  `formulated`? Recommended: keep the name, fix the doc. (5) Name and home: `ChooseDecideAnswer` in a new
  `decide_answer_choice.cpp`, or keep `decide_solver_gate.cpp`? Recommended: the new file. (6) Replace `phase.Next`
  with a `RunPass(name, fn)` helper in the same change? Two spans time the wrong passes today (`RewriteNorm` under
  `optimizer.tag_groups`, `RewriteAbs` under `optimizer.abs` with `TagAbsForBigM`). Recommended: yes. (7) Land after
  the held NULL item (B11): both rewrite `05_optimizer/done.md` § 1's "two steps run before these passes".
  — [code_quality § chosen in two places](issues/code_quality.md#how-a-query-is-answered-is-chosen-in-two-places)

## B19. One clause scope (D47): five calls (architect review done; the once-count part stays with B6/B12)

- [ ] Verified: behaviour can stay the same, nothing in D47 is serialized (so `DECIDB_VERIFY_SERIALIZER=1` cannot
  catch a regression; `EnumerateExpressions` and the `plan_decide.cpp` column harvest can), and the doc undercounts:
  eleven walkers track the enclosing `when`/`per`, not two; `ReducerValueLink` and `DecideObjective` hold scope copies
  too; a helper row is wrapped back two ways (`ScopeHelperRow` keeps the `when` only, the MIN/MAX easy half keeps the
  `per` too, which is B13's question and stays as is). Design: a borrowed `DecideEnclosingScope` view with one step
  function `EnterDecideScope` and a read-only leaf visitor in `decide_constraint_walk.hpp` (rewrites that replace
  nodes keep their own recursion and call the step); an owned `DecideClauseScope` built from the view, embedded in
  `DecideConstraint` and `ComposedMinMaxConstraint` (Introduce Parameter Object); one exported two-level splitter
  with three formatters. About 15 files. Choices: (1) `group_key` in the struct, as the doc says, or not? It is read
  off the comparison, not the wrapper, and the composed path can never hold one. Recommended: leave it out. (2) Under
  a wrapper, a plain AND starts a fresh scope in four walkers and carries it in five; a `when` inside a `when`
  overwrites in all but direct solve, which asserts it never happens. Nothing builds either shape today. One walker
  must pick: fresh scope, or make both shapes refused by the canonical-tree check C0 (with a C++ test)?
  Recommended: refuse in C0. (3) Field names: `per_key` holds a role, not a key; rename it `per`, `when_condition`
  becomes `scope.when`? Recommended: yes. (4) Convert the window rewrite and `RewriteInDomain` walkers now, direct
  solve's at H-07? Recommended: yes. (5) `DecideObjective` and `ReducerValueLink` stay outside D47 (D46 renames
  `per_columns`)? Recommended: yes. Order: after B11 commits (two shared files), before D46 (B20) and before B12 if B12
  passes the wrapper to every site.
  — [code_quality § parallel copies](issues/code_quality.md#a-clauses-scope-is-carried-in-parallel-copies)

## B20. Name the keyed scope keyed (D46): the names the ruling does not give

- [ ] Settled: `ROW / KEYED / GLOBAL` with values 0/1/2, `Entity()`/`Keyed()` merge, `Scalar()` becomes `Global()`,
  DIAGNOSE reports `keyed`, test files and markers renamed in the same change. Not given: (1) the two-level
  objective's tag and key field (`PER_CONSTRAINT_TAG`, `DecideObjective::per_columns`): "names that say so".
  Proposed: `TWO_LEVEL_OBJECTIVE_TAG`, `two_level_key`. (2) The test files: `test_entity_scope.py` and its
  `test_entity_scoped_*` tests → `test_keyed_scope.py` / `test_keyed_*`; `test_scalar_scope.py` →
  `test_global_scope.py`; `test_aggregate_local_when.py` → ? (3) The `per_clause` and `when*` markers in `pytest.ini`:
  which names and descriptions replace them? Order: after D47 (B19), which removes some renderer parameters D46
  would otherwise rename, and after B11 (shares `enums/decide.hpp`).
  — [code_quality § keyed](issues/code_quality.md#name-the-keyed-scope-keyed-everywhere), [testing § names](testing/todo.md#names-that-still-say-the-removed-spellings)

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
