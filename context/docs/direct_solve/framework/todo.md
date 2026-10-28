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
  `../architecture/ir.md`).
- **Change.** A second front end builds the same IR from the new syntax. Extend the IR when semantics change; do not
  teach a rule parser spelling.
- **Done when.** The IR contract and the direct/solver tests run on both syntaxes where comparable.

## H-03 — Read a data bound through the bound-side window

- **Problem.** The bound-side window rewrite (`RewriteRhsAggregates`, `decide_rewrite_rhs_window.cpp`) is the one
  evaluator of a data bound: it reduces a clause's bound to its group's tightest value and appends VALUE_IS_VALID scope
  checks for NULL, NaN and a disagreeing `=` bound. A direct rule reads the reduced column, so a data limit adds two
  windows under the rule, each copying every source column: the tightest-bound window and the window of the direct row
  checks (`AddRowChecks`).  R1's kept per-group plan could take the limit's MIN in the group aggregate it already runs. Measured
  on the wide R1 table (26 columns, 2,000,000 rows) with `SUM(w*x) <= cap * 500000`: about 1.0 s, against 0.33 s for
  the constant limit `1500000`; the two windows take about 0.35 s each.
- **Change.** Let a rule that applies read through the window: the rewrite records, per reduced bound, the raw value it
  reduced, and the rule takes the MIN or MAX in its own group aggregate, as the window would. The value checks raise the
  same messages in the same order, but inside the rule's own pass, so no window is left under it. Keep one place that
  states what the tightest bound is.
- **Done when.** `test_r1_a_limit_from_the_data_runs_the_plan_of_a_constant_limit[global-*]` passes (it is a strict
  xfail now) and the error order and messages are unchanged on both paths.

## H-04 — Keyed scope checks in the direct plan

- **Problem.** The key-less scope checks run under every rule's plan: D4 (a `when` keeps a row) and key-less D8, in
  `AddRowChecks` (`direct_rule_driver.cpp`), a window and a filter that keep the source's bindings. A check with a key
  makes the whole query decline (`DirectIR::status`).
- **Change.** Partition the window by the key for READS_A_ROW under `by`, and for DETERMINES. Quote the failing group
  in the message, as `PhysicalDecide::RunScopeChecks` does.
- **Done when.** For each check kind, the same message on both paths, including a NULL key value. H-05 and H-07 need
  this task.

## H-05 — Read a reducer's `by` key into the IR

- **Decision needed.**
- **Problem.** A per-group limit is `sum(x) by (dept) <= 2`: the grouping sits on the reducer, not on a
  wrapper around the clause. `ReadReducer` (`direct_ir.cpp`) declines on `TryParseGroupKeyTag`.
  `DirectIRScope::per_key_slots` and `DirectFrontEnd::SourceSlot` exist, but nothing fills them. The S1 and R1
  per-group logic is reached only through IR edits (`PerKeys` in `test/common/test_decidb_direct_decline.cpp`): the
  decline grids check the condition, and "S1 and R1 keyed plans answer as the solver does on the same `by` key" runs
  the plans against the solver. That test keeps every key non-NULL because both plans leave a NULL-key row ungrouped:
  with a key `g` NULL on rows 6 and 7, S1 under `SUM(x) <= 1` picks both and R1 fills both past the budget, where the
  solver's `by (g)` holds them to one group. Add the NULL group to that test with the fix.
- **Options.**
  1. Keep the key on the clause scope (`per_key_slots`). Smallest change; the kept per-group plans read it.
  2. Move the key onto the part (`DirectPart`), where the language puts it. Larger change; matches the grammar.
- **Ruling (owner, 2026-10-27).** Option 2: the key lives on the part, where `by` is written. It also fits the NULL-key
  rule (D5: a NULL key is no group of that key only).
- **Change.** Read both spellings: `C` with `by (g)` after each reducer (one clause per row, each reading its
  row's group), which allows a limit that varies by row. The
  removed-spelling message suggests `per g: C` with `by (g)`: one clause per class, which needs `g` to determine the
  limit. Honour the NULL-key rule D5 (a NULL key is in no group, as the kept plans have it) and D8 (a group the reducer's own `when`
  empties is an error, from H-04).
- **Plan shape (owner's direction).** A `by` key is a GROUP BY in the direct plan: the rule groups the covered rows by
  the key (a hash aggregate on the key columns, joined back to the rows on the key), as the per-group plans kept in the code do. It is not one clause per row and not a window per clause.
- **Tests waiting on this.** `direct_path` (`test/decide/tests/_direct_oracle_common.py`) makes the direct path of any
  case whose SQL holds ` by (` a strict xfail; its solver path is checked as usual. That is about 260 S1 grid cases (`S1_CASES` 77, `S1_OFFSET_CASES` 32,
  `S1_REDUCER_CASES` 151) and 22 R1 cases (`DATA_BUDGET_CASES` 13, `PIECE_CASES` 1, `SCOPED_CASES` 8). Delete the
  ` by (` branch of `direct_path` when S1 and R1 read the key; a case that then passes fails until the branch is gone.
- **Needs.** H-04.
- **Done when.** The IR carries the key for both spellings; the rule tasks `../rules/s1_uniform_matroid/todo.md` and
  `../rules/r1_continuous_knapsack/todo.md` § "Read a `by` key again" can start.

## H-06 — The two-level objective `outer(per k: inner(e) by (k))`

- **Problem.** The objective is `MINIMIZE sum(per id: max(w*x) by (id))`. `ReadObjective`
  (`direct_ir.cpp`) declines on `IsPerConstraintWrapper`. The nested-reducer reading (`DirectPart::inner` in
  `ReadReducer`) exists, but no query reaches it: the binder rejects a nested reducer anywhere else.
- **Change.** Read the two-level objective into the IR through the nested-reducer reading.
- **Done when.** `test_decidb_direct_ir.cpp`, "declines a two-level objective", reads the inner reducer. The nested order-0 norm case returns: an inner not-proved part leaves the outer not proved.

## H-07 — `per K:` on a constraint (one constraint per class)

- **Problem.** `CollectConstraints` (`direct_ir.cpp`) turns a `PER_CLASS_TAG` wrapper into one not-proved node.
- **Change.** Read `per g: sum(x) <= k` as D6 states it: the whole total bounded once per class of `g`, not a grouping
  of the reducer. The DETERMINES scope checks of the class come from H-04.
- **Needs.** H-04.
- **Done when.** A differential case per rule that reads it, on both paths, and the DETERMINES message on both paths.

## H-08 — VALUE-shape clauses (helper decisions)

- **Problem.** `ReadConstraint` (`direct_ir.cpp`) declines a leaf tagged `VALUE_REWRITE_TAG`. The solver builds a
  helper decision from `reducer_value_links`.
- **Change.** Read the helper decision and its definition into the IR, so a rule that applies can take the clause.
- **Done when.** A differential case with a VALUE-shape clause runs on both paths with the same answer.
