# Merge notes: Sami-OPA into OPA-rewrites

*Written 2026-11-16, before the merge. Tick the boxes as you go and record every decision in section 10.*

## What this file is for

`OPA-rewrites` was cut from `direct-solve-prototype`. The next step merges `Sami-OPA` into it. This file lists what
to check so that the merge is done properly, and so that the branch afterwards is a sound place to start the
rewrite described in [handoff.md](handoff.md).

Most of the detail comes from the integration review, [review/05_integration_with_mainline.md](review/05_integration_with_mainline.md).
Read it once in full before resolving anything. Its findings are an agent's reading of the two branches; items the
reviewer re-checked are marked ✓ here.

**Line numbers go stale.** Every `file:line` below was taken before the merge, and before `Sami-OPA` was reformatted.
Find code by function or identifier name.

## 1. The two sides

| | `OPA-rewrites` (ours) | `Sami-OPA` (theirs) |
|---|---|---|
| Starts as | `direct-solve-prototype` at `7865506c62`, plus one docs commit | local `Sami-OPA` at `f56845f560`: `origin/Sami-OPA` (`7837e300be`) plus the three preparation commits in section 4 |
| Common ancestor | `cec0e42f99` (2026-11-02) | same |
| Commits since | 404 | 227 |
| Brings | Direct-solve rules, one product form for degree-2 terms, one objective sense, source spans in messages, reformatted sources | The new language (nested reducers, frames, variant signatures) and the tree evaluator |

What each side changed that the other must absorb:

- **Mainline rewrote the term structures.** Every degree-2 term now travels as one `DecideProductGroup`. The old
  `bilinear_terms`, `quadratic_groups`, `squared_terms` and `DecideTermKind` are gone (commit `fd0994366d`).
- **Mainline has one objective sense.** The canonicalizer writes every objective as a minimization and keeps the
  written direction in `written_sense` (commit `7fde155940`).
- **Mainline deleted about 960 lines of `physical_decide.cpp`** and changed its behaviour in about ten commits
  (section 6, trap 11). OPA deleted most of that file and moved evaluation into the tree files.
- **OPA replaced the "determination" error with variant signatures** and removed the determination code entirely ✓.

## 2. What "done properly" means

The merge is finished when all of these hold:

- [ ] It builds with no warnings new to either parent.
- [ ] Every search in section 7 comes back empty.
- [ ] Every verification in section 8 passes, in every suite mode.
- [ ] Each trap in section 6 has its check ticked, with the proof recorded.
- [ ] Each mainline behaviour in trap 11 is shown to hold on the merged branch by mainline's own test for it.
- [ ] Every difference in query output against either parent is explained and logged (section 8.5).
- [ ] Direct solve still fires where it fired on mainline (section 8.4).
- [ ] `make format-check` reports only `src/include/duckdb/planner/logical_operator.hpp`.
- [ ] No test was weakened, skipped or re-recorded to pass. Tests that were deleted are listed in section 10 with
      the reason.
- [ ] `handoff.md` is updated as section 9 describes.

## 3. Rulings needed from the owner

Resolve these before touching the conflicts they govern. Do not pick one silently.

| # | Question | Governs | Status |
|---|---|---|---|
| R1 | Do variant signatures (OPA) replace the "determination" error (mainline)? | About ten mainline test files, `DecideScopeCheck` fields, `InKeyOrder`, BUG-54's code | **open** |
| R2 | On the solver path, do row checks and bound side tables run as plan operators (mainline) or inside the tree (OPA)? | `direct_row_checks.cpp`, `decide_optimizer.cpp` pass order, traps 7–9 | **open** |
| R3 | May the stored-output tests be deleted when the merge makes them stale? | `test_tree_emitter.py`, `tests/_expected.py`, `tests/expected/` (259 files) | **open**; reviewer recommends yes |
| R4 | Should `EXPLAIN` print the rows an ABS or L0 rewrite adds? | Four tests OPA changed; can wait until after the merge | **open** |

Reviewer's suggestion for R1: yes, variants replace the determination error. The owner chose OPA, and the variant
signature is the deck's σ. Mainline's determination tests are then deleted or rewritten to expect variants, each
logged in section 10. This is still a language ruling, so the owner confirms it.

Reviewer's suggestion for R2, for the merge only: keep OPA's tree evaluation on the solver path, port mainline's
fixes into it (traps 7–9), and leave the real decision to the rewrite. That is the smaller change, but it keeps two
implementations for now, which mainline's own comment argues against.

## 4. Baseline, recorded before the merge

Judge the merged branch against these.

| Measure | Mainline (`7865506c62`) | `Sami-OPA` as prepared |
|---|---|---|
| `make decide-test` | 7,288 passed | 10,407 passed |
| Golden check, Gurobi and HiGHS | identical, both | identical, both (and identical to `7837e300be`) |
| `unittest "[decidb]"` | 107 cases, 11,174 assertions, all pass | 85 cases, 3,419 assertions, all pass |
| Format check | only `logical_operator.hpp` | `logical_operator.hpp` and five files OPA never touched (below) |
| Fuzzers, seeds 1–2000, Gurobi | no fuzzers on mainline | `nests` 7,372 ok, 628 refused; `frames` 8,000 ok; `order_keys` 8,000 ok; `bounds` 15,998 ok and 2 false-infeasible, both seed 195 (handoff A7, defect 2) |

Preparation commits on `Sami-OPA`:

| Commit | What it does | Why before the merge |
|---|---|---|
| `9e3b0f9c82` | Formats 221 files the way mainline's FMT-1 commit (`da8b8941af`) does. Whitespace only | Without it nearly every file conflicts on layout |
| `d88e1fe66d` | Deletes the node algebra: 17 files, 1,640 lines removed, 221 added. No model changes | It is a copy of the term records, and it reads the structures mainline replaced. Deleted, it need not be ported |
| `f56845f560` | Fixes the wrong answer of handoff A7, defect 1, in `decide_rewrite_scoped.cpp`, with an oracle test | The file conflicts with mainline, so the fix had to exist on one side first |

**The five extra files in the format check.** `src/planner/bind_context.cpp`, `src/decidb/diagnostics/decide_router.cpp`,
`src/decidb/solver/probe_models.cpp`, `src/decidb/solver/solver_registry.cpp` and
`test/decide/tests/test_error_case_expression.py` are unformatted on `Sami-OPA` and unchanged there since the fork.
Mainline formatted them, so the merge takes mainline's copies with no conflict, and the merged branch must report
only `logical_operator.hpp`.

**Trial merge** (`git merge-tree --write-tree OPA-rewrites Sami-OPA`, which touches no checkout), measured on the
prepared branch:

- 151 files conflict: 148 by content and 3 as modify/delete (`decide_degree.cpp`, trap 3;
  `test_scope_check_plan.py` and `test_scope_determination.py`, trap 15).
- 1,379 conflict hunks. 598 are in the generated `src_backend_parser_gram.cpp`, which is regenerated, not resolved.
  That leaves 781 hunks in 147 files.
- The heaviest hand-resolved files: `physical_decide.cpp` (63 hunks), `ilp_model_builder.cpp` (47),
  `decide_binder.cpp` (31), `linearization_minmax.cpp` (23), `decide_constraints_binder.cpp` (18),
  `solver_input.hpp` (18), `decide_linear_form.cpp` (16), `decide_canonicalizer.cpp` (15).
- The full list is in [conflict_files.txt](conflict_files.txt).

**What the preparation changed that the merge must know:**

- `PhysicalDecide::BuildSolverInput` now sets `solver_input.sense = decide_sense`, and `FormulateModel` reads
  `flat_objective_agg` and `flat_objective_is_easy` from the operator. Both read the tree's algebra before. Trap 1
  applies to the first.
- A composed MIN/MAX root keeps each term's sign, ease and scale in `DecideBody::composed_specs`
  (`DecideComposedSpec`), filled by `ComposedBodies` in `decide_tree_compile.cpp` from
  `op.composed_minmax_constraints[i].terms` and `op.composed_minmax_objective_terms`.
- The `value_nodes` fields that carried node identity to stage 06 are gone (`EvaluatedConstraint`,
  `ReducerValueMembers`, `ReducerValueLinkSpec`, `SolverInput::objective_value_nodes`). `DecideTree::value_nodes`
  stays.
- The tree dump has no `formula=` field.

Known defects on `Sami-OPA` that the merge is **not** expected to fix: see [handoff.md](handoff.md), section A7.

Performance baseline for the tree evaluator: `tools/results/` (method in handoff.md, section B6).

## 5. Procedure

### 5.1 Before starting

- [ ] `git status` is clean on `OPA-rewrites`; `git rev-parse Sami-OPA` matches the hash recorded in section 4.
- [ ] `git fetch origin` shows no new commits on `origin/Sami-OPA` or `origin/direct-solve-prototype`. If either
      moved, stop and tell the owner.
- [ ] Rulings R1 and R2 are answered.
- [ ] Capture every suite query's output on both parents, for the comparison in section 8.5:
      `test/decide/capture_suite_log.sh capture --modes auto --out <dir>` (the script exists on `Sami-OPA`).
- [ ] Turn on helpers for a long merge: `git config merge.conflictStyle zdiff3` and `git config rerere.enabled true`.

### 5.2 The merge itself

- [ ] One three-way merge, not a rebase: `git merge --no-ff --no-commit Sami-OPA` on `OPA-rewrites`.
- [ ] **Do not hand-resolve generated files.** Resolve their sources, then regenerate:

| Generated | Source of truth | Regenerate with |
|---|---|---|
| Grammar outputs (`gram.hpp`, `src_backend_parser_gram.cpp` and the rest) | The grammar sources, which merge cleanly | `make grammar-build` |
| `serialize_nodes.cpp`, `serialize_logical_operator.cpp` | `nodes.json`, `logical_operator.json` (trap 10) | `python3 scripts/generate_serialization.py` |
| Golden dumps `test/decide/golden/baseline.*.dump` | The merged binary | `test/decide/golden/capture.sh --solver <gurobi\|highs>`; **read the diff before keeping it** |

- [ ] Resolve in this order, so each layer compiles against the one below it:
  1. Shared headers and enums: `logical_decide.hpp`, the term records (`decide_prepared_model.hpp` on mainline,
     renamed `decide_tree_terms.hpp` on OPA), `enums/decide.hpp`, `decide_term_split.*`.
  2. Binder and canonicalizer.
  3. Optimizer: `decide_linear_form.cpp`, the rewrites, `decide_optimizer.cpp` pass order.
  4. Direct solve (`src/optimizer/decide/direct/`): keep mainline's code; add OPA's declines (trap 14).
  5. Execution: the tree files are OPA's. `physical_decide.cpp` takes OPA's structure and mainline's behaviours
     (trap 11).
  6. Formulation (`src/decidb/formulation/`).
  7. Serialization, then tests, then docs.
- [ ] Docs under `context/docs`: where both sides edited, keep mainline's text for mainline's features. OPA's docs
      describe behaviour by reference to a deleted evaluator and are due for a rewrite; do not polish them now.
- [ ] After each file is resolved, run the formatter on it.
- [ ] Commit the merge as one merge commit. Fixes found afterwards go in their own commits, not amended into it.

## 6. Known traps

Each trap has a check. Tick it only when the check has been run.

### Trap 1. Objective sense flips twice ✓

- **What:** OPA reads `decide_sense` to choose which way to push a MIN or MAX (`decide_rewrite_frames.cpp`,
  `decide_rewrite_scoped.cpp`), and since the preparation `PhysicalDecide::BuildSolverInput` hands it to the solver
  as the sense. There are 50 references on the prepared branch. Mainline renamed the field `written_sense` and **already folded the flip into the
  term signs**.
- **Danger:** it will not compile, and the obvious fix, renaming the field, compiles and silently flips twice.
- **Do:** at each site decide from the term's sign, or always push down. Never rename mechanically.
- [ ] **Check:** `grep -rn "decide_sense" src` is empty. A MIN and a MAX reducer, each under MAXIMIZE and under
      MINIMIZE, in a constraint, in the objective and over a range frame, give the same objective as the prepared
      `Sami-OPA` binary. The oracle tests `test_anr_frames.py`, `test_anr_nested.py` and the min/max tests pass.

### Trap 2. Product form ✓

- **What:** OPA's tree builder, compiler and emitter read `bilinear_terms`, `quadratic_groups`, `squared_terms`,
  `has_quadratic`, `DecideTermKind` and `SplitLinear`. Mainline deleted all of them for `DecideProductGroup`.
- **Size:** 224 references on the prepared branch ✓: 105 in `decide_linear_form.cpp`, 25 in `decide_term_split.cpp`,
  24 in `decide_model_emitter.cpp`, 19 in `decide_tree_compile.cpp`. The tree-only files merge with no conflict
  marker and then fail to compile.
- **Do:** carry the emitter's degree-2 path onto product groups. Carry OPA's additions to the term records
  (the `FRAME` kind, `qualifier_reducer`, `child`, `avg_group_key`) onto mainline's types.
- **Remember:** the emitter is what step 5 of the rewrite replaces. Port it the simplest way that passes; do not
  redesign it here.
- [ ] **Check:** the searches in section 7. `test_bilinear.py`, the quadratic tests, and mainline's
      `test_product_normal_form.py`, `test_formulation_arms.py`, `test_optimizer_arms.py` pass.

### Trap 3. `decide_degree` was deleted on mainline

- **What:** mainline replaced `decide_degree.cpp` with `DecideDegreeOf` in `decide_product_form.hpp` and a new
  `decide_shape_check.cpp` (commit `c8c491d359`). OPA still calls `DecideExpressionDegree`, and it added a rule to
  the deleted file: a definition-function tag counts as a decision.
- **Do:** move OPA's rule into `DecideDegreeOf`.
- [ ] **Check:** `grep -rn "DecideExpressionDegree" src` is empty; tests of nonlinear reducer bodies pass
      (`test_minmax_body_definition.py`).

### Trap 4. Source fragments against source spans

- **What:** mainline deleted `LogicalDecide::source_fragments` and the fragment-tag API for `DecideSourceText`
  spans (commit `1cd018decc`). OPA's `DirectRetainRowChecks` writes `source_fragments`, and OPA uses the old API in
  about 15 places.
- [ ] **Check:** `grep -rn "source_fragments" src` is empty; mainline's `test_message_source_spans.py` passes.

### Trap 5. `DirectAggregateOf` changed signature

- **What:** OPA changed it to take a vector of arguments, for collated MIN and MAX. Mainline's new shared
  `DirectAggregateSink::Add` still passes one. No conflict marker; it fails to compile.
- [ ] **Check:** it compiles; `test_collated_keys.py` and the direct-solve tests pass.

### Trap 6. `variable_scopes` fallback

- **What:** mainline removed a bounds fallback in `decide_optimizer.cpp` (scopes now run parallel to decisions).
  OPA's helper decisions rely on the fallback.
- [ ] **Check:** nested-reducer tests pass with helpers present; no out-of-range read (run the nested tests on a
      debug build if one is available).

### Trap 7. The ABS value check is lost on the solver path (R2)

- **What:** mainline moved `install_row_checks` to run **after** the ABS rewrite, because the rewrite now adds a
  scope check (`AddAbsValueCheck`). OPA runs its replacement, `retain_row_checks`, **before** the ABS rewrite. After
  a plain merge that check sits in `scope_checks` and nothing consumes it.
- [ ] **Check:** find mainline's test for `AddAbsValueCheck` (`git log -S AddAbsValueCheck -- test`) and show it
      passes with direct solve off.

### Trap 8. Mainline's side-table fixes do not reach OPA's solver path (R2)

- **What:** OPA evaluates bound aggregates in the tree on the solver path. Mainline's fixes to the side tables land
  only on the direct path: NULL class keys and element-wise key comparison (`60566255a5`), BUG-45 (`5b14e8697b`).
- [ ] **Check:** `test_bound_side_tables.py` (both sides rewrote it) and mainline's `test_direct_class_key.py` pass
      with direct solve on and off.

### Trap 9. Two fixes for one problem

- **What:** both sides fixed "a group with no instance" on 2026-11-13: mainline in `b5167002f8` and `a409a9503b`,
  OPA in `e153b7daf3` (`DecideGroupBound::held`). Mainline's new callers of `DirectTightestBound` use the default
  argument and get none of OPA's behaviour.
- [ ] **Check:** mainline's `test_per_class_null_key_by_group.py` passes; the `bounds` fuzzer shows nothing new.

### Trap 10. Serialization ids collide

- **What:** in `LogicalDecide`, mainline uses id 221 for `objective_canonical_differs` and 222 for `source_text`.
  OPA uses 221 `signatures`, 222 `objective_signature`, 223 `frames`. Mainline also renamed 203 to `written_sense`
  and deleted 215.
- **Do:** renumber OPA's three to 223–225 and regenerate. `DecideScopeCheck` fields 110 and 111 depend on ruling R1.
- **Also check:** OPA's newer `LogicalDecide` vectors (`abs_function_envelopes`, `bilinear_function_groups`,
  `l0_indicator_functions`, `definition_functions`, `retained_*`) are not serialized. Find out whether they must be.
- [ ] **Check:** `DECIDB_VERIFY_SERIALIZER=1 make decide-test` passes.

### Trap 11. Mainline behaviours that land in code OPA deleted

OPA's `physical_decide.cpp` no longer holds the code these commits edited. Each behaviour must be re-established
in the tree, descent or emitter, and proven by the test the commit itself added or changed
(`git show --stat <hash> -- test`).

| Done | Commit | Behaviour |
|---|---|---|
| [ ] | `fd0994366d` | One product group carries every degree-2 term |
| [ ] | `7fde155940` | One objective sense |
| [ ] | `b5167002f8` | A `per K:` clause with a `by` reducer constrains only the groups its classes reach |
| [ ] | `a409a9503b` | A composed MIN/MAX under `per K:` has no instance when no kept row has a class |
| [ ] | `22e2a72fe1` | A constant coefficient that is NaN or infinite is named as such, on both paths |
| [ ] | `1cd018decc` | A message that names a value quotes its span of the query |
| [ ] | `5b14e8697b` | BUG-45: a written bound that reads data repairs in the data tier |
| [ ] | `ae8ed8352d`, `b7743c0b5b` | Number-range warnings, one state per query for both paths |
| [ ] | `bcc45b4e84` | BUG-54: determination message (only if R1 keeps the determination error) |
| [ ] | `60566255a5` | The class key has no flag; a NULL puts a row in no class |

`88c5f87385`, `b0a489da4e` and `b20600d087` only deleted unreachable code; nothing to carry.

### Trap 12. R1's sink moved

- **What:** mainline moved R1's aggregate sink into the shared `DirectAggregateSink`. OPA edited the old code in
  `r1_continuous_knapsack_rule.cpp` for vector arguments.
- [ ] **Check:** `test_direct_r1_continuous_knapsack.py` passes.

### Trap 13. The two-level objective

- **What:** OPA removed `BindTwoLevelObjective` in favour of nested reducers in the objective; mainline edited it
  for the sense change. Affects `decide_objective_binder.cpp`, `decide_rewrite_minmax.cpp`,
  `linearization_minmax.cpp`.
- **Do:** take OPA's form and apply mainline's sense rule to it (trap 1).
- [ ] **Check:** objective tests with `outer(per k: inner(e) by (k))` pass under both MAXIMIZE and MINIMIZE.

### Trap 14. Direct solve must still fire

- **What:** in `direct_ir.cpp` keep mainline's `ReadKey`, table facts, `model_shape` and sign handling. Add OPA's
  declines for what the rules cannot answer: a scope with a signature, an objective signature, frames.
- [ ] **Check:** section 8.4.

### Trap 15. Tests that expect the determination error (R1)

Affected on mainline: `test_scope_determination.py` and `test_scope_check_plan.py` (OPA deletes both),
`test_message_source_spans.py`, `test_qualified_reducer.py`, `test_scope_reducer_inner.py`, `test_collated_keys.py`,
`test_scope_parser.test`, `test_direct_s1_uniform_matroid.py`, `test_scope_combinations.py` with its oracle
`_scope_oracle.py`.

- [ ] **Check:** each is resolved according to R1, and listed in section 10.

### Trap 16. OPA's stored snapshots go stale (R3)

- **What:** 259 files under `test/decide/tests/expected/` hold output recorded from a deleted evaluator. 99 hold
  `sense: maximize` model dumps, which trap 1's change invalidates.
- **Do not re-record them.** That would store whatever the merged binary prints and call it correct.
- [ ] **Check:** per R3, the stored-output tests are deleted, or the owner has said otherwise.

### Trap 17. Model dumps change sign

- **What:** because every objective becomes a minimization, a model dump of a MAXIMIZE query shows
  `sense: minimize` with negated objective coefficients. This is expected, not a regression.
- [ ] **Check:** golden diffs are read line by line; anything other than the sense line and objective signs is
      explained in section 10.

### Trap 18. Small tooling differences

- OPA adds `test/decide/fuzz/`, `capture_suite_log.sh` and `suite_log.py`; mainline changed the `Makefile` and has
  `run_direct_matrix.sh`. Keep all of them.
- A fresh test environment installs a `gurobipy` the license refuses; see handoff.md, section A8.

## 7. Searches that must come back empty

Run from the repository root once the merge builds. Each identifier was deleted by one side and is still used by the
other.

```bash
# Deleted by mainline
grep -rn -w -E "bilinear_terms|quadratic_groups|squared_terms|has_quadratic" src
grep -rn -w -E "DecideTermKind|SplitLinear|DecideExpressionDegree" src
grep -rn -w -E "decide_sense|source_fragments" src

# Deleted by OPA
grep -rn -w -E "DecidePreparedModel|DirectInstallRowChecks|DefineNonlinearReducerBodies|LiftNestedReducers" src

# Only if ruling R1 removes the determination error
grep -rn -E "DeterminationCheck|DecideDeterminesText|ReadDetermination|InKeyOrder" src
```

The node algebra was deleted during preparation. This must stay empty:

```bash
grep -rn -E "DecideAlgebra|BuildAlgebra" src
```

## 8. Verification

### 8.1 Build and format

- [ ] `make release` succeeds.
- [ ] `PATH="$PWD/test/decide/.venv/bin:$PATH" python3 scripts/format.py --all --check` reports only
      `logical_operator.hpp`. Never run `make format-fix`.
- [ ] `git diff --check` is clean.

### 8.2 Suites, every mode

- [ ] `make decide-test`
- [ ] `build/release/test/unittest "[decidb]"`
- [ ] `DECIDB_FORCE_SOLVER=highs make decide-test`
- [ ] `DECIDB_VERIFY_SERIALIZER=1 make decide-test`
- [ ] `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`, or `test/decide/run_direct_matrix.sh` for all four at once

The merged suite should hold every test of both parents, less the ones deleted under R1 and R3.

### 8.3 Fuzzers

- [ ] `cd test/decide && .venv/bin/python3 -m fuzz all --seed 1 --count 2000 --jobs 8`
- [ ] The same with `--count 400 --solver highs`

Expected: nothing beyond the failures recorded in section 4.

### 8.4 Direct solve

- [ ] All `test_direct_*` tests pass.
- [ ] For a query each rule answers on mainline (R1, R4, S1–S3, A1–A2, N1, O1), `EXPLAIN` on the merged branch still
      shows the direct plan. Mainline's tests under `test/decide/tests/test_direct_*` name such queries.
- [ ] The direct-solve benchmarks under `benchmark/decide/direct_solve/` run and do not regress against mainline.

### 8.5 Every changed output is explained

- [ ] Capture the merged branch's suite output and compare it with each parent's capture:
      `test/decide/suite_log.py <parent_dir> <merged_dir> --limit 400`.
- [ ] Sort each difference into an expected kind and log the kinds in section 10. Expected kinds: the sense line and
      objective signs in model dumps; span-quoted messages; determination against variants (R1); a different but
      equally optimal assignment. **Anything else is a finding.**

### 8.6 The tree still follows the deck

- [ ] `repro/deck_example/f1_max.sql`, `f1_sum.sql` and `f2.sql` give the models of the deck's pages 115, 119 and
      108. Compare with `repro/deck_example/model_opa_*.txt`, allowing for trap 17.
- [ ] `DECIDB_DUMP_TREE=1` on `f1_max.sql` still prints the tree of the deck's page 81.

### 8.7 Threads and performance

- [ ] `tools/threads_test.py 500000`: every shape gives the same objective at 1 and 8 threads.
- [ ] `tools/ab_bench.py` against the prepared `Sami-OPA` binary: no workload moves outside noise (about 10%).
      Edit `BINARIES` at the top of both scripts first.

### 8.8 Known defects, re-run

- [ ] Re-run each confirmed defect in handoff.md, section A7, and record its status on the merged branch. Expected:
      the product-floor defect, wrong on mainline, is now right; defect 1 stays fixed (the preparation fixed it, and
      `test_a_bound_that_reads_the_rows_holds_on_each_under_a_whole_table_nest` guards it); the others are unchanged.

## 9. After the merge: ready to start the rewrite

- [ ] Update `handoff.md`: section A2 (branches), A3 (where the tree is built, with new line numbers), A5 (the plan,
      with the merge done), A7 (defect statuses).
- [ ] Re-find every `file:line` reference in `handoff.md` that the rewrite will rely on, by identifier.
- [ ] Record the merged branch's own baseline (suite counts, fuzz results, benchmarks) as the new section 4.
- [ ] List what the merge left deliberately rough, for the rewrite to replace: the emitter's port to product groups,
      and whatever R2's stop-gap left in two implementations.
- [ ] Remove the three review worktrees under `.claude/worktrees/` once nothing needs their binaries.

## 10. Decisions and deviations

Append one line per decision made during the merge, per test deleted, and per unexpected difference.

| Date | What | Why | Who decided |
|---|---|---|---|
| 2026-11-16 | Preparation: two tests in `test_one_pass_tree.py` read a node's reads from the dump's `reads=` field instead of the deleted `formula=` text | Same queries and same property (a function's definition rows do not read it back; a product's function reads its reducer). Only the probe changed | Reviewer; owner to confirm |
| 2026-11-16 | Preparation: five defensive checks went with the node algebra (one producer per helper variable; decision index in range; Boolean-product body matches its fields; ease agrees with sense; empty helper link) | They checked the records through the algebra. An independent review found none reachable by today's tree construction. The rewrite replaces `Validate` | Reviewer |
| 2026-11-16 | Preparation: a product or square over a nested reducer, under a bound that reads the rows, is now refused by name ("squares or multiplies decisions inside nested reducers") | It answered wrongly at `7837e300be`. It answered correctly at `36f12dd089`, by a path later commits removed. Restoring that path is rewrite work | Reviewer; owner to confirm |
| | | | |
