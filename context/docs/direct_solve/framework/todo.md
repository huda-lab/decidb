# Shared engine — open work

The engine is everything that is not one problem class: IR, rule driver, plan fragments, runtime checks, bound reader,
result adapter, policy.
What exists today is in `done.md`. Direct solve declines each language construct it cannot yet model; the
solver path answers those queries, so none of them is wrong, only slower.

## How to work any task in these todo files

- Each task stands alone. Read its **Problem**, make the change and run the checks. When it ships, its result moves to
  the owning `done.md` and the task is deleted from its `todo.md`.
- The baseline to compare against is the stamp at the top of `done.md`. Any other failure after your change is yours.
- Checks that every code task runs (from the repository root): `make decide-test`,
  `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`, `DECIDB_VERIFY_SERIALIZER=1 make decide-test`,
  `DECIDB_FORCE_SOLVER=highs make decide-test`,
  and `build/release/test/unittest "[decidb]"`. Rebuild first with `make release`. `test/decide/run_direct_matrix.sh` runs all
  five and prints one line each (about 3 minutes). Doc-only tasks need no checks.
- Do not loosen or delete a test to make it pass. If a test fails for a reason the task did not predict, stop and report it.
- Tasks marked **Decision needed** have two or more reasonable answers. Ask the owner before coding.
- The oracle tests cache solver results under the test's node id (`results/oracle_cache.json`). Do not rename a test
  function or move it to another file unless the task says so; a miss re-runs Gurobi, which needs a licence.
- The Bash tool is zsh. In `grep`, put `--include` flags before `-e PATTERN`; after a `--` they are read as file names.
- Stop and ask also when a golden changes that the task did not name, or a user-facing message needs wording the
  doc does not give. A task that turns out to need the owner records its question as **Decision needed**.
- A behavior change is judged by an oracle or the solver path, on both backends. A structural change gets a
  `decidb-architect` review.
- Never without the owner: large-tier benchmarks or other long CPU-bound runs, regenerating goldens before reading the
  diff, pushing, new branches or worktrees, `git stash`.

## H-06 — The two-level objective `outer(per k: inner(e) by (k))`

- **Problem.** The objective is `MINIMIZE sum(per id: max(w*x) by (id))`. `ReadObjective`
  (`direct_ir.cpp`) declines on `IsTwoLevelObjectiveWrapper`. The nested-reducer reading (`DirectPart::inner` in
  `ReadReducer`) exists, but no query reaches it: the binder rejects a nested reducer anywhere else.
- **Change.** Read the two-level objective into the IR through the nested-reducer reading.
- **Done when.** `test_decidb_direct_ir.cpp`, "declines a two-level objective", reads the inner reducer. The nested order-0 norm case returns: an inner not-proved part leaves the outer not proved.
- **Needs (owner, 2026-10-29).** A rule that takes the shape. Read into the IR with no rule taking it, the reading was
  reached only by an IR test, so it was removed; this task waits for A2–A4 (`../todo.md`).

## H-08 — VALUE-shape clauses (helper decisions)

- **Decision needed.** The rule and clause shape this is for. No rule's definition takes a VALUE-shape clause, so
  reading the helper into the IR alone reaches no code.
- **Problem.** `ReadConstraint` (`direct_ir.cpp`) declines a leaf tagged `VALUE_REWRITE_TAG`. The solver builds a
  helper decision from `reducer_value_links`.
- **Options.** 1. Leave it until a rule names the shape it needs; the task then becomes that rule's. 2. The owner names
  a rule and shape now.
- **Recommendation.** 1.
- **Change.** Read the helper decision and its definition into the IR, so a rule that applies can take the clause.
- **Done when.** A differential case with a VALUE-shape clause runs on both paths with the same answer.

## H-37 — Undo two unrelated formatting changes

- **Problem.** `src/function/function_list.cpp` is generated (`scripts/generate_functions.py`) and skipped by
  `scripts/format.py`, yet about 260 of its lines are reflowed where only the `DecideWarnNumbersFun` entry should differ.
  `physical_decide.cpp` holds tab-indented lines inside a block indented with spaces, around `NumberRangeWarning
  number_range_warning;` and `number_range_warning.Check(elastic_model);`.
- **Change.** Regenerate `function_list.cpp` with the script; restore the spaces in `physical_decide.cpp`.
- **Done when.** `git diff master -- src/function/function_list.cpp` shows only the function's entry;
  `git diff --check` is clean; the build passes. No test run is needed beyond the build.

## H-38 — The plan helpers A1, N1 and R1 each copy

- **Problem.** `architecture/rules.md` § Keep the layer lean moves a helper to shared code once a second rule needs it.
  A1 and N1 carry identical `Stage` structs (R1 has `ProjectionStage`), and each of A1, N1 and R1 has its own `SumOf`,
  number check (`ValidNumber` in A1, `CheckNumber` in N1), `TooLarge`, and `And`/`Not` built from `calc.If`.
- **Change.** One projection stage in `direct_plan_fragments`; `DirectSumOf`; one number check that returns a nullable
  check (A1's shape) and `DirectRaiseTooLarge` in `direct_runtime_checks`; `ExpressionBuilder::And` and `::Not`. Each
  rule calls the shared form and drops its copy.
- **Done when.** No behavior change; `run_tests.sh -k "a1 or n1 or r1"` and C++ `[decidb]` pass; no rule file defines
  a helper the shared files hold.

## H-39 — One fragment for the separable square objective

- **Problem.** A1 and N1 each lay out a square's `w` and `t` columns in P1 and fold `k*w*w` and `2*k*w*t` into the row's
  `a` and `b` in P2, with the same mask-and-scale lambda (A1's per-decision fold, N1's `ComputeCoefficients`). N1's `Part`
  and A1's `Part`/`Term` hold the same reading in two layouts.
- **Change.** A shared reading of a part with linear terms, constants and squares (`DirectReadQuadraticPart`), a P1
  layout and a P2 fold in `direct_plan_fragments`. A1 calls the fold per decision, N1 for its one decision.
- **Needs.** H-36 and H-38.
- **Done when.** No behavior change; `run_tests.sh -k "a1 or n1"` and C++ `[decidb]` pass; neither rule file expands a
  square itself.

## H-40 — `DirectPart::IsPlainSum()`

- **Problem.** A1, N1, R1 and S1 each test a plain `SUM` objective part with the same four-way condition
  (`reducer == SUM`, no `inner`, no qualifier, no `by`). `DirectIRConstraint` has `PlainSum()`; `DirectPart` has none. A1
  alone reads the objective's `when` and the part filter itself instead of through `DirectObjectivePartKept`.
- **Change.** Add `DirectPart::IsPlainSum()` and call it in the four rules; A1 reads the kept rows through
  `DirectObjectivePartKept`.
- **Done when.** No behavior change; `run_tests.sh -k "a1 or n1 or r1 or s1"` passes.

## H-41 — The precision warning comes from the shared arithmetic

- **Problem.** Each rule picks its own warning sites: A1 tests `a` and `b` of `-b / (2*a)`, R1 tests `v / w`, N1 tests
  only `h`, so a tiny square factor warns under A1 and is silent under N1 on the same expression. N1's `LosesPrecision`
  restates `Answer`'s branch conditions. N1's warning reads only the rows a parent reads; A1's and R1's read every row.
  `architecture/rules.md` states neither the obligation nor which rows a warning reads.
- **Change.** The shared square fragment (H-39) returns its target with its precision flag, and N1's scale returns its
  answer with its flag, so a rule that reuses a closed form warns where it does. N1 computes "the row reads the scale"
  once for both the answer and the warning. `architecture/rules.md` § What every rule must match states the obligation
  and the row set. No new window or pass (owner's precision rule).
- **Needs.** H-39.
- **Done when.** A tiny square factor warns on A1 and N1 alike; the existing warning tests of A1, N1 and R1 pass.

## H-42 — One warning state per query for both paths

- **Problem.** The solver path says the warning once per `FinalizeSolveResult` (one `NumberRangeWarning` per DECIDE
  operator, `physical_decide.cpp`), though `ilp_solver.hpp` says "one instance lives for one query". The direct path
  says it once per query (`DecideNumberWarningState`, `decide_warn_numbers.cpp`). A query with two DECIDE nodes prints
  the line twice. `decide_numeric_policy.hpp` says DeciDB sets its tolerances "on every backend", but
  `DECIDE_OPTIMALITY_TOLERANCE` is set on Gurobi only; HiGHS keeps its tighter default.
- **Change.** Declare `DecideNumberWarningState` with a `Say()` in a header; `SolveModel` calls it through
  `SolveModelOptions` when the model holds such a number; delete `NumberRangeWarning`. Reword the tolerance comment to
  say which backend reads the optimality tolerance.
- **Done when.** A query with a solver DECIDE and a direct DECIDE that both warn prints one line; the warning tests pass
  on `DECIDB_FORCE_SOLVER=highs make decide-test` and `make decide-test`.

## H-43 — `decide_warn_numbers` is internal

- **Problem.** `decide_warn_numbers()` is listed by `duckdb_functions()` and a user can call it
  (`functions.json`, `generic_functions.hpp`). DuckDB names its plan-internal functions with `__internal_`
  (`compressed_materialization/functions.json`).
- **Change.** Rename it `__internal_decide_warn_numbers`; `DirectWarnIf` calls the new name.
- **Done when.** The warning tests pass; `SELECT * FROM duckdb_functions() WHERE function_name LIKE 'decide_warn%'` is
  empty.

## H-44 — The warning's text

- **Ruling (owner, 2026-11-06).** The line is `DECIDE: a very large or very small number may make this answer
  inaccurate. Scale its data toward 1.` on both paths.
- **Problem.** The line reads `very large or very small number detected - numerical precision may make the solution
  unstable`. It has no `DECIDE:` prefix, unlike every other DECIDE message, so a script's stderr does not say where it
  came from, and it gives no action. The too-large error ends "Scale its data down."
- **Change.** Replace the string in `decide_numeric_policy.hpp` and the tests' expected line (`decidb_cli.py`).
- **Done when.** `make decide-test` passes with the new line on both paths.

## H-45 — Framework docs describe the shared code as it is

- **Problem.** `done.md` says "A1 is its only user until R1 is built on it" and "A1 reads constants and squares itself";
  both are untrue. `architecture/rules.md` step 4 does not list `DirectReadConstantTerm`, `DirectReadSquareTerm`,
  `DirectObjectivePartKept`, `DirectWarnIf` or `DirectBelowNormalRange`. The stamp predates the shared code. A1's, R1's
  and N1's `done.md` each carry an "Its own warning" paragraph that restates one rule. `direct_bound_reader` holds
  objective-term readers that its name does not cover.
- **Change.** Correct both docs to the shared code after H-38 to H-41; one warning paragraph in the framework
  `done.md`, linked from each rule; state the bound reader's wider scope in its header and `done.md`.
- **Needs.** H-38, H-39, H-40, H-41.
- **Done when.** Every helper the rules call is listed once; no rule `done.md` restates the warning rule; doc-only.

## H-46 — Test helpers say "number range"

- **Problem.** `decidb_cli.py` (`_LARGE_NUMBER_WARNING`, `LargeNumberWarningMismatch`, `_settle_large_number_warning`),
  the `expect_large_numbers` and `large_numbers` arguments, and `_large_numbers.writes_large_number` also cover small
  numbers.
- **Change.** Rename them to the number-range wording. Do not rename any oracle test function (the cache keys on it).
- **Done when.** `make decide-test` passes; `grep -rn large_number test/decide` finds only test function and file names, which keep
  their names.
