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

## H-14 — Read a stored table twice instead of holding wide rows

- **Problem.** Direct solve gives the solver's outcome, which includes raising on a bad value in any input row even when the
  parent stops early (`LIMIT 1`: both paths raise on a `NaN` in row 4,999 of 5,000). So it checks every row before it
  releases any (`DirectValidationBarrier`: an ordinary projection plus a whole-input window) and holds the rows meanwhile.
  When the parent reads a wide column, that column is held too. A1 with a 512-byte payload read back peaks at 5,212 MiB at
  5,000,000 rows against 131 MiB for an unused payload (`../../architecture/result_boundary.md`).
- **Decision made.** Read twice, for stored tables only. Holding everything and relaxing the error promise were rejected.
- **Change.** When the source is a stored table and the parent reads columns the rule does not need, the plan reads the
  table twice: the first read carries only the columns the rule and the check need, plus `rowid`; the second reads the
  other columns and is joined to the answers on `rowid`. The plan stays ordinary DuckDB operators (scans, a join,
  projections); no DuckDB optimizer or execution code changes. The check still covers every input row in the first read.
  Joins, subqueries, aggregates and volatile sources have no stable row key and keep holding.
- **Done when.** For A1 and S1: results and errors match the solver with and without the second read, including a
  `LIMIT 1` query with a bad value in the last row; a source over a join still holds; the plan reads twice only for a
  stored table with a parent-read column the rule does not need. If this does not fit one sitting, propose a split before
  coding.

## H-16 — Re-measure memory with the built read-twice plan

- **Needs.** H-14.
- **Problem.** The "Read twice" column in `../../architecture/result_boundary.md` comes from a hand-written join, not the
  plan H-14 builds.
- **Change.** On a quiet machine, re-run that table with the built plan and replace the column, its description and the
  date.
- **Done when.** The "Read twice" numbers come from the built plan.

## N-04 — A1's bound absorption and the solver's are one function

- **Problem.** A1's `ProveBounds` (`a1_rule.cpp`) absorbs a per-row bound into a variable's interval with its own code; the
  solver path absorbs the same bound with other code. Only the differential tests guard that they agree.
- **Change.** One absorb function that both call. No behavior change. Keep to the absorption step: moving A1's per-row
  bound reading into a shared builder is S1-12 (`../s1/todo.md`).
- **Done when.** Both paths call the one function and the checks give the stamp's numbers.

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
