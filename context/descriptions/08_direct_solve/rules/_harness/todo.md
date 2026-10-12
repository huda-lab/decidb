# Shared engine — open work

The engine is everything that is not one problem class: facts, coordinator, builders, result boundary, policy. What exists
today is in `done.md`.

## How to work any task in these todo files

- Each task stands alone. Read its **Problem**, make the change and run the checks. When it ships, its result moves to
  the owning `done.md` and the task is deleted from its `todo.md`.
- The baseline to compare against is the stamp at the top of `done.md`. Any other failure after your change is yours.
- Checks that every code task runs (from the repository root): `make decide-test`,
  `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`, `DECIDB_VERIFY_SERIALIZER=1 make decide-test`,
  `DECIDB_FORCE_SOLVER=highs make decide-test` (one failure that needs Gurobi is expected: `test_norm_combined_l1_l2_objective`),
  and `build/release/test/unittest "[decidb]"`. Rebuild first with `make release`. `test/decide/run_direct_matrix.sh` runs all
  five and prints one line each (about 3 minutes). Doc-only tasks need no checks.
- Do not loosen or delete a test to make it pass. If a test fails for a reason the task did not predict, stop and report it.
- Tasks marked **Decision needed** have two or more reasonable answers. Ask the owner before coding.
- The oracle tests cache solver results under the test's node id (`results/oracle_cache.json`). Do not rename a test
  function or move it to another file unless the task says so; a miss re-runs Gurobi, which needs a licence.
- The Bash tool is zsh. In `grep`, put `--include` flags before `-e PATTERN`; after a `--` they are read as file names.

## H-16 — Re-measure memory with the built read-twice plan

- **Problem.** The "Read twice" column in `../../architecture/result_boundary.md` comes from a hand-written join, not the
  built second read (`direct_second_read.cpp`).
- **Change.** On a quiet machine, re-run that table with the built plan and replace the column, its description and the
  date. Also time a narrow parent-read column (`SELECT id, x` on `src`) under `require` with the second read and with
  `SET disabled_optimizers = 'unused_columns'` (single read, rows held), at 1,000,000 and 5,000,000 rows. H-17 uses it.
- **Done when.** The "Read twice" numbers come from the built plan, and the narrow-column timing is recorded next to them.

## H-17 — Which parent-read columns take the second read

- **Decision needed.**
- **Needs.** H-16's narrow-column timing.
- **Problem.** Any stored column a parent reads and the rule does not use comes from the second read, whatever its width
  (`direct_second_read.cpp`). For a wide payload that saves most of the held memory. For a 4-byte `id` it saves about 4
  MB per 1,000,000 rows and adds a second scan and a join over every row, which may make the query slower.
- **Options.**
  1. Keep any column. Simplest; a narrow column pays the join.
  2. Only variable-width types (`VARCHAR`, `BLOB`, lists, structs, maps). No tuning number; fixed-width columns stay
     held.
  3. A byte threshold on the estimated width. Needs a number chosen from measurements.
- **Recommendation.** Option 2 if H-16 shows the narrow case measurably slower; option 1 if the difference is
  negligible.
- **Change.** Apply the chosen filter in the late step's choice of moved outputs; the rest of the plan is unchanged.
- **Done when.** A test shows a column of the excluded kind stays in the single read (one scan in `EXPLAIN`) and a
  wide column still moves; the matrix is at baseline.

## H-02 — Adapter for the new language

- **Needs.** A stable language branch.
- **Problem.** The rules read facts produced from the current syntax only (`direct_problem.cpp`;
  `../../architecture/facts.md`).
- **Change.** A second adapter produces the same facts from the new syntax. Extend the facts when semantics change; do not
  teach a rule parser spelling.
- **Done when.** The facts contract and the direct/solver tests run on both syntaxes where comparable.

## H-03 — Cost choice among competing direct plans

- **Needs.** Two rules that prove the same query on a real workload.
- **Problem.** The coordinator picks the cheapest proved candidate, but S1 and A1 never prove the same query, so the cost
  estimate is untested. The eager cost context costs under 0.5 ms of planning (`done.md`).
- **Change.** Re-measure the cost estimate on the workload that needs it. Cost never repairs a missing proof.
- **Done when.** Two proved plans compete on that workload and cost decides.
