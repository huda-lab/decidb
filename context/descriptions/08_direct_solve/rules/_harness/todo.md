# Shared engine — open work

The engine is everything that is not one problem class: facts, coordinator, builders, result boundary, policy. Cross-cutting
tasks live here. When a task ships, its result moves to `done.md` and the task is deleted from this file. H-01 (a second
rule) shipped on 2026-10-06. Old IDs are in parentheses so earlier notes still make sense.

H-04 to H-13 come from a full review of the direct-solve layer on 2026-10-07; H-04, H-05 and H-06 shipped on 2026-10-09
(session 2, with A1-06 and S1-14), and H-09, H-10 and H-11 on the same day (session 3, with S1-10, S1-11 and A1-07).
S1 and A1 tasks from the same review are in `../s1/todo.md` (S1-12 and S1-13) and `../a1/todo.md` (A1-05).

## How to work any task in these three todo files

- Each task stands alone. Read its **Problem**, make the change, run the checks, then move the result to `done.md`.
- Baseline on 2026-10-09 (working tree on top of `191789b282`, not committed, after session 3): `make decide-test`,
  `DECIDB_TEST_DIRECT_SOLVE=off make decide-test` and `DECIDB_VERIFY_SERIALIZER=1 make decide-test` each give 2,322 passed
  (no failures, no expected failures). `DECIDB_FORCE_SOLVER=highs make decide-test` gives 2,321 passed and the one
  expected failure. C++ `build/release/test/unittest "[decidb]"` passes (1,241 assertions in 40 cases). Any other
  failure after your change is yours.
- Checks that every code task runs (from the repository root, about 30 seconds each): `make decide-test`,
  `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`, `DECIDB_VERIFY_SERIALIZER=1 make decide-test`,
  `DECIDB_FORCE_SOLVER=highs make decide-test` (one failure that needs Gurobi is expected: `test_norm_combined_l1_l2_objective`),
  and `build/release/test/unittest "[decidb]"`. Rebuild first with `make release`. Doc-only tasks need no checks.
- Do not loosen or delete a test to make it pass. If a test fails for a reason the task did not predict, stop and report it.
- Tasks marked **Decision needed** have two or more reasonable answers. Ask the owner before coding; the options and a
  recommendation are written under the task.
- The oracle tests cache solver results under the test's node id (`results/oracle_cache.json`). Do not rename a test
  function or move it to another file unless the task says so; a miss re-runs Gurobi, which needs a licence.
- The Bash tool is zsh. In `grep`, put `--include` flags before `-e PATTERN`; after a `--` they are read as file names.

## Suggested batches

| Batch | Tasks | What it gets you |
|---|---|---|
| 1. Green and honest | A1-05 (re-stamp only; A1-01 and A1-04 shipped on 2026-10-09) | The A1 stamps match a run. About 15 minutes. |
| 2. Safety | done 2026-10-09 (H-04, H-05, H-06, A1-06, S1-14) | The empty-source test gap is closed, reading facts cannot throw, the NaN message names the right clause. |
| 3. Trim | done 2026-10-09 (S1-10, S1-11, H-10, H-11, A1-07, H-09) | Removes code nobody uses. |
| 4. Structure | H-07 part 2 (needs a decision), H-08, S1-13, H-12 | One mechanism for the solver's quirks, reusable tests, current measurements. Larger diffs. |
| Later | S1-12, H-13, A1-03 | Wait for a third per-row rule, or for one measurement. |

## H-07 — The solver's "rejected before any row is read" rule is copied three ways

**Part 1 done (2026-10-09), part 2 remains.** Part 1 is the strict-bound shift: `DecideStrictBound` in
`decide_bound_absorption.{hpp,cpp}` is the one copy, and the solver (comparisons and BETWEEN sides), A1 and S1 call it.
Part 2 is the domain test and the sentinels below: `DecideBoxContradictsDomain`, `SOLVER_LOWER_UNSET` and
`SOLVER_UPPER_UNSET`, and S1's `ConstantPinDomain` conditions. The rest of this entry is still the plan for part 2. The
model builder (`ilp_model_builder.cpp`, `ApplyComparisonSense` and the quadratic path) has three more hand-written
`floor(rhs) + 1` / `ceil(rhs) - 1` copies for strict constraint rows; they are correct, and they are in a lower layer
that does not include optimizer headers, so they were left alone.

- **Problem.** The solver, before it reads any row, raises an error for a decision whose box contradicts its built-in
  domain: an upper bound below the default floor of 0, or a `BOOL` lower bound above 1
  (`physical_decide.cpp`, constructor of `DecideGlobalSinkState`, about lines 1273–1320). It raises even on an empty
  table. A rule that answered such a query itself would be wrong on an empty input. Today three places know this:
  the solver itself; A1, which copies the test and the solver's internal sentinel values (`SOLVER_LOWER_UNSET` and
  `SOLVER_UPPER_UNSET` in `a1_rule.cpp`, `ProveBounds`); and S1, which avoids the problem by declining some spellings
  (`ConstantPinDomain` in `s1_rule.cpp`, condition `!fact.scope.when`). The strict-bound shift was also copied
  into A1 (now shared, part 1).
- **Why it matters.** The copy in A1 was silently dead once (the empty-source tests that now guard it are in `done.md`), and nothing would notice if the solver's check
  changed. The strict-bound copy had to change in the same commit as the solver's rounding fix (A1-02), which it did.
- **Decision needed.** Two ways to get one source of truth:
  1. *Small:* put a pure function `DecideBoxContradictsDomain(lower, upper, is_bool)` next to `DecideStrictBound` and
     `DecideBoundIsAbsorbed` in `decide_bound_absorption.{hpp,cpp}`. `physical_decide.cpp`, A1 and S1 all call it. The
     sentinels live there too. About 1 hour.
  2. *Large:* the facts adapter computes each decision's box and a `static_conflict` flag from the same code, so a rule
     only reads the flag. About 4 hours and changes the facts contract.
  Recommendation: option 1. It removes the copies without growing the facts. Do option 2 only if a third rule needs the box.
- **Where.** `src/optimizer/decide/decide_bound_absorption.cpp`, `src/execution/operator/decide/physical_decide.cpp`,
  `a1_rule.cpp` (`ProveBounds`), `s1_rule.cpp` (`ConstantPinDomain`).
- **Plan (option 1).** Add the function; make the solver and A1 call them with no change in behavior; then make S1's
  `ConstantPinDomain` use them in place of its hand-written conditions. Every suite stays green at each step.
- **Done when.** The sentinel values and the domain test exist once, the solver, A1 and S1 call them, and all checks pass.

## H-08 — The three-way test file is S1-only, so A1 copied it

- **Problem.** `architecture/rules.md` tells a new rule to "add rows" to `test_direct_three_way.py`. That file is built
  for S1 (`test_s1_*` functions, `_s1_constraints`), so A1 got its own 1,331-line `test_direct_a1.py` with copies of the
  scaffold. Helpers with the same name in both files: `_allow_direct_path` and `_run` (identical bodies), and
  `_row`, `_literal`, `_values`, `_solve_with_oracle`, `_agree`, `_problem` (same job, different bodies). The same-shaped tests exist twice:
  parent contexts, empty source, nested decisions, error cases, boundary values, late bad rows.
- **Why it matters.** A third rule will copy it again, and the docs say something else. Fixes to the scaffold (a new
  context, a new error class) have to be made in each copy.
- **Where.** `test/decide/tests/test_direct_three_way.py`, `test_direct_a1.py`, `_direct_differential.py`, `architecture/rules.md` (item 2 of
  "The standard tests every rule ships").
- **Plan.**
  1. Create `test/decide/tests/_direct_oracle_common.py` for the helpers that are the same in both: the data literals
     (`_literal`, `_values`), `_run`, `_run_all`, `_outcome`, the `_allow_direct_path` fixture, and the generic "run on
     the oracle, `off` and `require`, then compare" skeleton, parameterized by a function that builds the oracle model
     and one that checks an assignment.
  2. Keep every test function in its current file with its current name. Only helpers move. This keeps the oracle cache
     valid (see the notes at the top of this file).
  3. Reword `rules.md` item 2: a rule adds its own case table and oracle-model builder and reuses the shared skeleton.
- **Done when.** The two files share one scaffold, the number of tests is unchanged (2,267 including xfails today), and no
  oracle result had to be recomputed. About 1–2 hours.

## H-12 — Docs that point at nothing, and a matrix that is slow to stamp

- **Problem.** (1) `src/include/duckdb/optimizer/decide/direct/direct_rule.hpp` (the comment above `DirectDomain`) says
  facts are described in `00_design/architecture.md#semantic-facts`. That file does not exist; the description is in
  `context/descriptions/08_direct_solve/architecture/facts.md`. (2) Each `done.md` carries a "Verified" stamp with its own
  commit and counts (S1 says 1,957 tests at `8c9420edb2`; A1 says 2,250; the harness says 2,257). They drift at every
  change because the five-command matrix has to be typed and summed by hand.
- **Why it matters.** A dangling pointer costs every new reader time, and a stamp nobody can cheaply renew stops being trusted.
- **Plan.** Fix the comment. Add `test/decide/run_direct_matrix.sh` that runs the five checks in `rules.md` one after
  another and prints one line each (`auto: 2260 passed, 6 xfailed, 1 failed`), and mention it in `rules.md` and each
  `done.md` stamp. Do not rewrite the stamps in this task; each rule's own task (S1-13, A1-05) renews its own.
- **Done when.** The comment points at a real file, the script runs the matrix and prints the summary lines. About 45 minutes.

## H-13 — Wide rows cost memory in proportion to the input (one question, two rules)

- **Problem.** S1 (`../s1/todo.md`, S1-05) and A1 (`../a1/todo.md`, A1-03) both keep whole rows alive until all of them
  have been checked or ranked, so a parent that reads a wide column makes memory grow with the input. For A1 at 1,000,000
  rows with a 512-byte payload the peak is 1.06 GiB, against 47 MiB when the payload is unused. S1's cause is "not isolated".
  They are probably the same question about the same shared barrier.
- **Why it matters.** One answer should settle both; two separate fixes could contradict each other.
- **Plan.** Run the experiments listed in S1-05 once, on one database, covering both rules. Add the A1 shape from
  `benchmark/decide/profile_direct_a1.py` (the wide payload case). Decide: keep holding rows (`DirectValidationBarrier`),
  or read the source twice. `architecture/result_boundary.md` says holding was chosen over a second read; record the
  measurement that confirms or reverses it.
- **Done when.** One recorded decision, in `architecture/result_boundary.md`, and the weak-spots paragraphs of `s1/done.md`
  and `a1/done.md` point at it; then delete S1-05 and A1-03.

## H-02 — ANR adapter for the new language (was NEXT-04)

- **Goal.** Produce the same facts from the new language, so the rules work for both syntaxes.
- **Depends.** A stable language branch.
- **Code.** `direct_problem.cpp` is the current-tree adapter; `../../architecture/facts.md`.
- **Done when.** The same facts contract and direct/solver tests run on both syntaxes where comparable. Extend the facts
  when semantics change; do not teach a rule parser spelling. Compatibility is not assumed until this passes.

## H-03 — Cost choice among competing direct plans (was NEXT-05)

- **Goal.** Choose between two proved direct plans by cost. The coordinator already picks the cheapest proved
  candidate, but the two rules so far (S1, A1) never prove the same query.
- **Done when.** Two proved plans compete on a real workload and cost decides. Cost never repairs a missing proof.
- **Note.** H-09 measured the eager cost context at under 0.5 ms of planning (`done.md`) and left it. If H-03 becomes real,
  re-measure on the workload that needs it.

## Reviewed on 2026-10-07 and left alone (do not redo)

- **Result boundary** (`direct_result_boundary.cpp`): the extension operator, serialization and pruning hook are needed to
  keep DECIDE's bindings. Parent contexts all agree with the solver: `CREATE TABLE AS`, `INSERT ... SELECT`, a view, a
  window over the result, `UNION ALL` of an S1 and an A1 query, a joined source with a filter above, a self-join of results,
  an empty source and a prepared statement with a parameter gave identical results under `auto`, `off` and `require`.
- **Test-only hooks:** `DirectRuleOverride` (coordinator tests install stub rules) is small. Keep it.
- **A1 plan size:** each bound clause makes a large expression, but it scales cheaply. At 1,000,000 rows: 1 clause 0.08 s,
  3 clauses 0.13 s, 8 clauses with `WHEN` 0.26 s, 3 decisions and 8 clauses 0.29 s. No action.
