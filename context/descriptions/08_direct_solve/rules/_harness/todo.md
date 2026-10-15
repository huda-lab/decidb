# Shared engine — open work

The engine is everything that is not one problem class: IR, rule driver, builders, result adapter, policy. What exists
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

## H-02 — Front end for the new language

- **Needs.** A stable language branch.
- **Problem.** The rules read an IR built from the current syntax only (`direct_ir.cpp`;
  `../../architecture/ir.md`).
- **Change.** A second front end builds the same IR from the new syntax. Extend the IR when semantics change; do not
  teach a rule parser spelling.
- **Done when.** The IR contract and the direct/solver tests run on both syntaxes where comparable.
