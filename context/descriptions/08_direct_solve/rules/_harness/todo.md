# Shared engine — open work

The engine is everything that is not one problem class: facts, coordinator, builders, result boundary, policy. Cross-cutting
tasks live here. When a task ships, its result moves to `done.md` and the task is deleted from this file. H-01 (a second
rule) shipped on 2026-10-06. Old IDs are in parentheses so earlier notes still make sense.

H-04 to H-13 come from a full review of the direct-solve layer on 2026-10-07; H-04, H-05 and H-06 shipped on 2026-10-09
(session 2, with A1-06 and S1-14), H-09, H-10 and H-11 on the same day (session 3, with S1-10, S1-11 and A1-07), and H-07,
H-08 and H-12 on the same day (session 4). N-01 (the numeric-policy header) shipped the same day (session 6, stage A);
H-13 (the wide-row memory measurement) was measured the same day and left two open decisions, H-14 and N-02 below.
S1 and A1 tasks from the same review are in `../s1/todo.md` (S1-12 and S1-13) and `../a1/todo.md` (A1-05).

## How to work any task in these three todo files

- Each task stands alone. Read its **Problem**, make the change, run the checks, then move the result to `done.md`.
- Baseline on 2026-10-09 (after session 6 stage A): `make decide-test`,
  `DECIDB_TEST_DIRECT_SOLVE=off make decide-test` and `DECIDB_VERIFY_SERIALIZER=1 make decide-test` each give 2,322 passed
  (no failures, no expected failures). `DECIDB_FORCE_SOLVER=highs make decide-test` gives 2,321 passed and the one
  expected failure. C++ `build/release/test/unittest "[decidb]"` passes (1,276 assertions in 44 cases). Any other
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

## What remains

| Task | State | Needs |
|---|---|---|
| H-14 (wide rows held in memory) | Measured, decision open | Owner decision, below |
| N-02 (hand-written copies of DuckDB logic) | Surveyed, nothing changed | Owner picks which rows to act on, below |
| S1-13, A1-05 (re-measure and re-stamp) | Waiting | Run last, on a quiet machine, after the two above are settled |
| S1-12 | Not scheduled | A third per-row rule exists |
| H-02, H-03 | Not scheduled | See their entries |

## H-14 — Rows are held in memory until every row is checked (measured 2026-10-09; decision open)

- **Problem.** Direct solve must give the solver's outcome, which includes raising on a bad value in any input row even when
  the parent stops early (`LIMIT 1`; verified: both paths raise on a `NaN` in row 4,999 of 5,000). So it checks every row
  before it releases any (`DirectValidationBarrier`), and holds the rows meanwhile. When the parent reads a wide column,
  that column is held too. S1 cannot stream anyway (it ranks every row before it emits one); A1's answers are per row.
- **Measured** (peak footprint, MiB, 512-byte payload, `require` mode; `architecture/result_boundary.md` has the method and
  the S1/A1 repeats). A1 at 1,000,000 rows: control 32, held 1,049, read twice 80. A1 at 5,000,000: 131, 5,212, 316. S1 at
  5,000,000: 725, 3,362, 945 to 993. The solver path holds more than direct on the same query (1,655 MiB vs 1,049 for A1,
  1,012 vs 652 for S1 at 1,000,000 rows), so this is an existing cost, not a regression. It only shows when the query reads
  a wide column; an unread payload is dropped early (the control).
- **Options (owner decision).**
  1. *Keep holding.* No work. Cost: memory in proportion to the input for wide parent-read columns.
  2. *Read twice* when the source has a usable row key (a stored table's `rowid`): pass one reads and checks the narrow
     columns, pass two reads the wide table and joins the answers back by key. Keeps the full promise. Cost: a second plan
     shape in the result boundary; joins, subqueries and aggregates keep holding.
  3. *Relax the promise for A1*: an error is guaranteed only for rows the parent actually reads, so A1 streams. No new plan.
     Cost: a `LIMIT` query can pass over a late bad row where the solver raises. `policy.md` currently promises the same
     outcome as the solver, so this is a policy change.
- **Recommendation (from the measuring agent, not yet approved).** Option 2 for stored tables, after the numeric cleanup.
- **Done when.** The owner picks an option; if 2 or 3, a task is written here with its plan and tests. If 1, close this task
  and add one line to `architecture/result_boundary.md`.

## N-02 — Hand-written copies of DuckDB's logic (survey 2026-10-09; nothing changed)

Where code copies what DuckDB (or the solver) already computes, it can drift. The rule: use the engine's own evaluation at
the same points the solver does ("parity by construction"), not a hand-written proof. The numeric-policy header
(`src/include/duckdb/common/decide_numeric_policy.hpp`, N-01) took the number copies; these are the logic copies:

| Where | What it duplicates | Risk if it drifts | Fix |
|---|---|---|---|
| `ilp_model_builder.cpp` strict shifts (4 sites) | `DecideStrictBound` | Solver and builder disagree on `x > 0.5` again (the session 1 bug) | Move `DecideStrictBound` into the header (pure double math); call it from all four |
| `DirectCastMayThrow`, `DirectMayThrow` (`direct_expression.cpp`) | DuckDB's `CanThrow` (narrowing test, minus TRY_CAST; function-error check) | A rule admits a cast or function that raises, and the error order differs | Make DuckDB's `CanThrow` honour TRY_CAST (a change in DuckDB code); delete the copies |
| A1 `ProveBounds` (`a1_rule.cpp`) | The solver's bound absorption | A1 and the solver absorb a bound differently; only differential tests guard it | Call a shared absorb function |
| Four fold-to-DOUBLE helpers (`TryEvaluateFoldableDouble`, `...NoThrow`, `DirectFoldableDouble`, `DirectFiniteFoldableDouble`) | Each other | They differ on BOOL, finiteness, catch behaviour | One evaluate-and-convert function, differences as parameters |
| `DirectValidNumberPredicate` (`direct_builder.cpp`) | `physical_decide.cpp:298` (NaN-only vs not-finite), as SQL | The paths reject different values | A cross-check test; no clean construction |
| A1 "beyond the range DeciDB can return" text and its 2^53 test | `DecideSolutionToInteger` in `physical_decide.cpp` | Wording or range diverges | Share one message string and the range helper |

- **Recommendation.** Act on the first row now (small, pure, closes the session 1 follow-up). The rest are larger, and the
  `CanThrow` rows change DuckDB code; take them only if a rule needs them.
- **Done when.** The owner names the rows to act on; each becomes a task with its own tests, and the rest stay listed.

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
