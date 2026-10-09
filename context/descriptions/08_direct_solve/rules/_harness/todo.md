# Shared engine — open work

The engine is everything that is not one problem class: facts, coordinator, builders, result boundary, policy. Cross-cutting
tasks live here; what exists today is in `done.md`. When a task ships, its result moves to `done.md` and the task is
deleted from this file.

## Open work across the engine, S1 and A1

| Task | State | Needs |
|---|---|---|
| H-14 (rows held in memory) | Decision open | Owner picks an option, below |
| N-02 (hand-written copies of DuckDB logic) | Decision open | Owner picks which rows to act on, below |
| H-15 (duplicate test fixture) | Ready | Nothing |
| S1-13 (`../s1/todo.md`) | Ready | A quiet machine and a Gurobi licence |
| A1-05 (`../a1/todo.md`) | Ready | A quiet machine |
| S1-12 (`../s1/todo.md`) | Not scheduled | A third per-row rule exists |
| H-02, H-03 | Not scheduled | See their entries |

## How to work any task in these todo files

- Each task stands alone. Read its **Problem**, make the change, run the checks, then move the result to `done.md` and
  delete the task.
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

## H-14 — Rows are held in memory until every row is checked

- **Decision needed.**
- **Problem.** Direct solve must give the solver's outcome, which includes raising on a bad value in any input row even when
  the parent stops early (`LIMIT 1`: both paths raise on a `NaN` in row 4,999 of 5,000). So it checks every row before it
  releases any (`DirectValidationBarrier`) and holds the rows meanwhile. When the parent reads a wide column, that column
  is held too. S1 cannot stream anyway (it ranks every row before it emits one); A1's answers are per row. The measurements
  are in `../../architecture/result_boundary.md`: A1 with a 512-byte payload read back peaks at 5,212 MiB at 5,000,000 rows
  against 131 MiB for an unused payload, and the solver path holds more than direct solve on the same query.
- **Options.**
  1. *Keep holding.* No work. Memory grows with the input for wide parent-read columns.
  2. *Read twice* when the source has a usable row key (a stored table's `rowid`): pass one reads and checks the narrow
     columns, pass two reads the wide table and joins the answers back by key. Keeps the full promise (316 MiB for A1 at
     5,000,000 rows). Cost: a second plan shape in the result boundary; joins, subqueries and aggregates keep holding.
  3. *Relax the promise for A1*: an error is guaranteed only for rows the parent actually reads, so A1 streams. No new plan.
     Cost: a `LIMIT` query can pass over a late bad row where the solver raises. `../../architecture/policy.md` promises the
     same outcome as the solver, so this is a policy change.
- **Recommendation.** Option 2, for stored tables.
- **Done when.** The owner picks an option. For 2 or 3, write its plan and tests here. For 1, delete this task and add one
  line to `../../architecture/result_boundary.md`.

## N-02 — Hand-written copies of DuckDB's logic

- **Decision needed.**
- **Problem.** Where code copies what DuckDB or the solver already computes, the copy can drift. The rule is parity by
  construction: use the engine's own evaluation at the same points the solver does, not a hand-written proof. Numeric limits
  live in `src/include/duckdb/common/decide_numeric_policy.hpp`; these are the logic copies that remain:

| Where | What it duplicates | Risk if it drifts | Fix |
|---|---|---|---|
| `ilp_model_builder.cpp` strict shifts (4 sites) | `DecideStrictBound` | Solver and builder disagree on `x > 0.5` | Move `DecideStrictBound` into the header (pure double math); call it from all four |
| `DirectCastMayThrow`, `DirectMayThrow` (`direct_expression.cpp`) | DuckDB's `CanThrow` (narrowing test, minus TRY_CAST; function-error check) | A rule admits a cast or function that raises, and the error order differs | Make DuckDB's `CanThrow` honour TRY_CAST (a change in DuckDB code); delete the copies |
| A1 `ProveBounds` (`a1_rule.cpp`) | The solver's bound absorption | A1 and the solver absorb a bound differently; only differential tests guard it | Call a shared absorb function |
| Four fold-to-DOUBLE helpers (`TryEvaluateFoldableDouble`, `...NoThrow`, `DirectFoldableDouble`, `DirectFiniteFoldableDouble`) | Each other | They differ on BOOL, finiteness, catch behaviour | One evaluate-and-convert function, differences as parameters |
| `DirectValidNumberPredicate` (`direct_builder.cpp`) | `physical_decide.cpp:298` (NaN-only vs not-finite), as SQL | The paths reject different values | A cross-check test; no clean construction |
| A1 "beyond the range DeciDB can return" text and its 2^53 test | `DecideSolutionToInteger` in `physical_decide.cpp` | Wording or range diverges | Share one message string and the range helper |

- **Recommendation.** Act on the first row (small, pure). The rest are larger, and the `CanThrow` rows change DuckDB code;
  take them only if a rule needs them.
- **Done when.** The owner names the rows to act on; each becomes a task with its own tests, and the rest stay listed.

## H-15 — Three test files define the shared autouse fixture themselves

- **Problem.** `test/decide/tests/_direct_oracle_common.py` holds the autouse fixture that lifts a forced backend
  (`_allow_direct_path`). `test_direct_fuzz.py`, `test_direct_rule_contract.py` and `test_direct_user_facing.py` each still
  define the same fixture in their own file.
- **Why it matters.** A fix to the fixture has to be made four times, and a new test file will copy it again.
- **Plan.** Import the fixture from `_direct_oracle_common.py` in the three files and delete their copies. Keep every test
  function's name and file (oracle results are cached by node id).
- **Done when.** The fixture is defined once and all five checks give the stamp's numbers. About 15 minutes.

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
- **Note.** The eager cost context costs under 0.5 ms of planning (`done.md`). If this becomes real, re-measure on the
  workload that needs it.
