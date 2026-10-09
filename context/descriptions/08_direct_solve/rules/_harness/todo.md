# Shared engine — open work

The engine is everything that is not one problem class: facts, coordinator, builders, result boundary, policy. Cross-cutting
tasks live here. When a task ships, its result moves to `done.md` and the task is deleted from this file. H-01 (a second
rule) shipped on 2026-10-06. Old IDs are in parentheses so earlier notes still make sense.

H-04 to H-13 come from a full review of the direct-solve layer on 2026-10-07; H-04, H-05 and H-06 shipped on 2026-10-09
(session 2, with A1-06 and S1-14), H-09, H-10 and H-11 on the same day (session 3, with S1-10, S1-11 and A1-07), and H-07,
H-08 and H-12 on the same day (session 4).
S1 and A1 tasks from the same review are in `../s1/todo.md` (S1-12 and S1-13) and `../a1/todo.md` (A1-05).

## How to work any task in these three todo files

- Each task stands alone. Read its **Problem**, make the change, run the checks, then move the result to `done.md`.
- Baseline on 2026-10-09 (working tree on top of `e60edc021b`, not committed, after session 4; the counts are the same as
  after session 3): `make decide-test`,
  `DECIDB_TEST_DIRECT_SOLVE=off make decide-test` and `DECIDB_VERIFY_SERIALIZER=1 make decide-test` each give 2,322 passed
  (no failures, no expected failures). `DECIDB_FORCE_SOLVER=highs make decide-test` gives 2,321 passed and the one
  expected failure. C++ `build/release/test/unittest "[decidb]"` passes (1,241 assertions in 40 cases). Any other
  failure after your change is yours.
- Checks that every code task runs (from the repository root, about 30 seconds each): `make decide-test`,
  `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`, `DECIDB_VERIFY_SERIALIZER=1 make decide-test`,
  `DECIDB_FORCE_SOLVER=highs make decide-test` (one failure that needs Gurobi is expected: `test_norm_combined_l1_l2_objective`),
  and `build/release/test/unittest "[decidb]"`. Rebuild first with `make release`. `test/decide/run_direct_matrix.sh` runs all
  five and prints one line each (about 3 minutes). Doc-only tasks need no checks.
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
| 4. Structure | done 2026-10-09 (H-07, H-08, H-12) | One mechanism for the solver's quirks, one test scaffold, a matrix script. S1-13 moved to batch 5. |
| Later | S1-12, H-13, A1-03 | Wait for a third per-row rule, or for one measurement. |

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
