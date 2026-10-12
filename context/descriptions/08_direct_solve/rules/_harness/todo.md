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

## H-17 — Only variable-width parent-read columns take the second read

- **Problem.** Any stored column a parent reads and the rule does not use comes from the second read, whatever its width
  (`direct_second_read.cpp`). For a narrow column that costs S1 about 30% with no memory saved: its global top-K at
  5,000,000 rows takes 0.28 to 0.33 s reading a `BIGINT` key back, against 0.21 to 0.25 s with one read
  (`../../architecture/result_boundary.md` § Second read of a stored table). The owner chose to move only
  variable-width columns. A1 is a little faster with a narrow column moved (0.44 against 0.52 s at 5,000,000 rows) and
  gives that up; it stays about 6x faster than the solver.
- **Change.** In the late step's choice of moved outputs, move a column only when its type is variable-width
  (`VARCHAR`, `BLOB`, `BIT`, lists, arrays, structs, maps, unions); every fixed-width column stays in the single read.
  The rest of the plan is unchanged.
- **Done when.** A test shows a fixed-width parent-read column stays in the single read (one scan in `EXPLAIN`) and a
  `VARCHAR` column still moves (two scans); the existing second-read tests pass, and the matrix is at baseline.

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
