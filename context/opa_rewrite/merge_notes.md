# Merge notes: Sami-OPA into OPA-rewrites

*Written 2026-11-16, before the merge, and ticked during it. Every decision is in section 10; the merged branch's
baseline is section 4.1.*

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

- [x] It builds with no warnings new to either parent.
- [x] Every search in section 7 comes back empty. The third search finds mainline's own 23 lines, none on the
      operators (section 10).
- [x] Every verification in section 8 passes, in every suite mode.
- [x] Each trap in section 6 has its check ticked, with the proof recorded. Trap 6's debug-only assertions were not
      run: no current debug build exists (section 10).
- [x] Each mainline behaviour in trap 11 is shown to hold on the merged branch by mainline's own test for it.
- [x] Every difference in query output against either parent is explained and logged (section 8.5).
- [x] Direct solve still fires where it fired on mainline (section 8.4).
- [x] `make format-check` reports only `src/include/duckdb/planner/logical_operator.hpp`.
- [x] No test was weakened, skipped or re-recorded to pass. Tests that were deleted are listed in section 10 with
      the reason.
- [x] `handoff.md` is updated as section 9 describes.

## 3. Rulings needed from the owner

Resolve these before touching the conflicts they govern. Do not pick one silently.

| # | Question | Governs | Status |
|---|---|---|---|
| R1 | Do variant signatures (OPA) replace the "determination" error (mainline)? | About ten mainline test files, `DecideScopeCheck` fields, `InKeyOrder`, BUG-54's code | **Decided (owner, 2026-11-16): yes.** Variants replace the determination error |
| R2 | On the solver path, do row checks and bound side tables run as plan operators (mainline) or inside the tree (OPA)? | `direct_row_checks.cpp`, `decide_optimizer.cpp` pass order, traps 7–9 | **Decided for the merge (owner, 2026-11-16):** keep OPA's tree evaluation and port mainline's fixes into it. The lasting choice is the rewrite's (handoff A6, item 2) |
| R3 | May the stored-output tests be deleted when the merge makes them stale? | `test_tree_emitter.py`, `tests/_expected.py`, `tests/expected/` (259 files) | **Decided (owner, during the merge): yes.** The three are deleted (section 10) |
| R4 | Should `EXPLAIN` print the rows an ABS or L0 rewrite adds? | Four tests OPA changed; can wait until after the merge | **open**. The merge prints OPA's form, without them (section 10, section 8.5 kinds (3)) |

What R1 means in practice: the variant signature is the deck's σ. Mainline's determination tests are deleted or
rewritten to expect variants, each one logged in section 10.

What R2 means in practice: keep OPA's tree evaluation on the solver path, port mainline's
fixes into it (traps 7–9), and leave the real decision to the rewrite. That is the smaller change, but it keeps two
implementations for now, which mainline's own comment argues against.

## 4. Baseline

### 4.1 The merged branch

The rewrite judges itself against these. Measured on the merge commit's tree, on an Apple M3 Pro (11 cores), each
suite mode run as its own command with exit status 0.

| Measure | Merged `OPA-rewrites` |
|---|---|
| `make decide-test` | 8,806 passed, 0 skipped; golden check identical on Gurobi and HiGHS |
| `DECIDB_VERIFY_SERIALIZER=1 make decide-test` | 8,806 passed |
| `DECIDB_FORCE_SOLVER=highs make decide-test` | 8,806 passed |
| `DECIDB_TEST_DIRECT_SOLVE=off make decide-test` | 8,806 passed |
| `unittest "[decidb]"` | 114 cases, 11,564 assertions, all pass |
| Format check | only `logical_operator.hpp` |
| Fuzzers, seeds 1–2000, Gurobi | `nests` 7,372 ok, 628 refused; `frames` 8,000 ok; `bounds` 16,000 ok; `order_keys` 8,000 ok |
| Fuzzers, seeds 1–400, HiGHS | `nests` 1,436 ok, 164 refused; `frames` 1,600 ok; `bounds` 3,200 ok; `order_keys` 1,600 ok |
| `tools/threads_test.py 500000` | All eight shapes agree at 1 and 8 threads, with five states per 8-thread run, on the merge and on the prepared `Sami-OPA` binary |

The suite holds fewer tests than `Sami-OPA`'s 10,407: the R1 and R3 deletions and the test audit's trims, each a
row of section 10. The `nests` refusals are `Sami-OPA`'s (fractional values, AVG and MIN/MAX under `<>`). The
`bounds` fuzzer's two false-infeasible results of seed 195 are gone (section 10).

**The tree evaluator against the `Sami-OPA` parent** (`tools/ab_bench.py`, the method of handoff.md section B6;
non-solver time and peak memory):

| Workload | Rows | OPA non-solver ms | Merge non-solver ms | Change | OPA peak RSS MB | Merge peak RSS MB |
|---|---:|---:|---:|---:|---:|---:|
| `flat-sum` | 10^5 | 41.0 | 40.5 | -1% | 108 | 108 |
| `nested-1` | 10^5 | 59.7 | 58.4 | -2% | 217 | 217 |
| `nested-2` | 10^5 | 140.4 | 140.0 | 0% | 261 | 262 |
| `nested-3` | 10^5 | 1,212.8 | 1,218.5 | 0% | 841 | 842 |
| `variants-10` | 10^5 | 48.3 | 48.2 | 0% | 121 | 121 |
| `point` | 10^5 | 48.3 | 48.3 | 0% | 186 | 186 |
| `range-w1` | 10^5 | 66.7 | 66.5 | 0% | 206 | 206 |
| `range-w5` | 10^5 | 87.2 | 86.3 | -1% | 258 | 258 |
| `range-w50` | 10^5 | 321.7 | 316.5 | -2% | 1,128 | 1,128 |
| `range-step7` | 10^5 | 98.6 | 96.8 | -2% | 289 | 289 |
| `range-max` | 10^5 | 147.5 | 147.3 | 0% | 860 | 860 |
| `cyclic-repeats` | 10^5 | 76.4 | 76.0 | -1% | 226 | 226 |
| `flat-sum` | 10^6 | 196.2 | 195.0 | -1% | 772 | 772 |
| `nested-2` | 10^6 | 1,248.7 | 1,244.9 | 0% | 2,108 | 2,121 |
| `variants-10` | 10^6 | 296.6 | 277.6 | -6% | 911 | 912 |
| `point` | 10^6 | 282.4 | 285.0 | +1% | 1,564 | 1,564 |
| `range-w5` | 10^6 | 725.2 | 706.8 | -3% | 2,340 | 2,341 |
| `range-w50`, run 1 | 10^6 | 3,332.2 | 3,076.5 | -8% | 6,191 | 6,134 |
| `range-w50`, run 2 | 10^6 | 3,128.8 | 3,169.3 | +1% | 6,338 | 6,303 |

No workload moves outside noise. `range-w50` at 10^6 ran twice, and the two runs disagree in sign.

**Direct solve against mainline** (`benchmark/decide/direct_solve/run.py --compare`: each query's direct plan beside
its hand-written plan, best of 5; TPC-H SF 1, O1 at SF 0.32). Mainline's binary is `7865506c62`'s build.

| Query | SF | Mainline direct CPU ms | Merge direct CPU ms | Mainline direct/hand | Merge direct/hand | Answers, both binaries |
|---|---:|---:|---:|---:|---:|---|
| `a1` | 1 | 28.9 | 27.6 | 1.14 | 1.12 | identical |
| `a2` | 1 | 19.0 | 20.3 | 1.10 | 1.19 | identical |
| `a2_abs` | 1 | 102.8 | 101.6 | 0.98 | 1.20 | identical |
| `a2_abs_global` | 1 | 29.0 | 32.5 | 1.40 | 1.47 | identical |
| `a2_keyed` | 1 | 35.6 | 36.3 | 1.15 | 1.13 | identical |
| `n1` | 1 | 34.3 | 33.6 | 1.25 | 1.23 | identical |
| `n1_groups` | 1 | 46.2 | 46.0 | 1.17 | 1.16 | identical |
| `n1_weighted` | 1 | 65.9 | 65.5 | 1.40 | 1.39 | identical |
| `n1_weighted_groups` | 1 | 78.5 | 78.9 | 1.31 | 1.34 | identical |
| `r1` | 1 | 661.1 | 613.1 | 2.15 | 2.04 | differ by 1.7e-14 |
| `r1_global` | 1 | 430.4 | 431.4 | 1.00 | 0.95 | differ by 1.8e-14 |
| `r4` | 1 | 33.0 | 33.4 | 1.01 | 1.02 | identical |
| `r4_groups` | 1 | 46.7 | 46.2 | 0.95 | 0.93 | identical |
| `r4_keyed` | 1 | 71.2 | 74.4 | 0.68 | 0.70 | identical |
| `s1` | 1 | 141.0 | 140.6 | 0.97 | 0.98 | identical |
| `s1_global` | 1 | 201.3 | 198.1 | 1.70 | 1.67 | identical |
| `s2` | 1 | 142.0 | 143.6 | 1.48 | 1.46 | identical |
| `s3` | 1 | 279.5 | 273.0 | 1.41 | 1.37 | identical |
| `s3_global` | 1 | 282.3 | 280.3 | 1.05 | 1.07 | identical |
| `o1` | 0.32 | 125.9 | 127.0 | 2.84 | 2.75 | identical |

"Answers" compares the direct plan's answer with the hand plan's. The two R1 queries differ from their hand plan by
summation order on both binaries, under the script's limit of 1e-9. The two cells that moved by more than 10% were
re-run at best of 15 on both binaries: `a2_abs_global` direct CPU 29.7 and 29.0 ms on mainline, 29.0 and 29.5 on the
merge; `a2_abs` direct/hand 1.13 on mainline and 1.14 on the merge, each twice. Both were noise.

### 4.2 The parents, recorded before the merge

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

- [x] `git status` shows no change to a tracked file on `OPA-rewrites`; `git rev-parse Sami-OPA` matches the hash
      recorded in section 1. An untracked `AGENTS.md` at the repository root is the owner's and may stay: neither
      parent has that path. *Before the merge: only `?? AGENTS.md`; `Sami-OPA` at `f56845f560`.*
- [x] `git fetch origin` shows no new commits on `origin/Sami-OPA` or `origin/direct-solve-prototype`. If either
      moved, stop and tell the owner. *Before the merge and again before the commit: `7837e300be` and `7865506c62`.*
- [x] Rulings R1 and R2 are answered (section 3).
- [x] Capture every suite query's output on **both parents**, each from its own tests and its own binary, for the
      comparison in section 8.5. Write the captures to a folder outside the repository. *`Sami-OPA`: 15,697
      queries, 10,407 passed. Mainline: 12,244 queries, 7,289 passed. Both manifests name their commit and binary;
      `git status` was clean again after the hook was removed.*
  - **`Sami-OPA`:** in `.claude/worktrees/sami-opa`, run
    `test/decide/capture_suite_log.sh capture --modes auto --out <dir>/opa`.
  - **Mainline has neither the script nor the hook it needs** ✓. The log is written by `_log_query` in
    `test/decide/decidb_cli.py`, which mainline's file lacks, so copying the script alone captures nothing. Before
    the merge, in the main checkout: add the hook by hand (the constant `_SUITE_LOG_ENV`, the function `_log_query`
    and its one call after the query runs; read them with `git show Sami-OPA:test/decide/decidb_cli.py`), copy
    `capture_suite_log.sh` and `suite_log.py` from `Sami-OPA`, and capture to `<dir>/mainline`. Both sides changed
    `decidb_cli.py`, so the hunk will not apply as a patch.
  - Then restore `decidb_cli.py`, delete the two copied files, and confirm `git status` is clean again.
  - Check each capture's `manifest.txt`: it names the commit and the binary's checksum.
- [x] Turn on helpers for a long merge: `git config merge.conflictStyle zdiff3` and `git config rerere.enabled true`.

### 5.2 The merge itself

- [x] One three-way merge, not a rebase: `git merge --no-ff --no-commit Sami-OPA` on `OPA-rewrites`. *151 files
      conflicted, as the trial merge predicted.*
- [x] **Do not hand-resolve generated files.** Resolve their sources, then regenerate. *The grammar outputs by
      `scripts/generate_grammar.py`, the serializers by `scripts/generate_serialization.py`, the goldens by
      `capture.sh` with the diff read (section 10).*

| Generated | Source of truth | Regenerate with |
|---|---|---|
| Grammar outputs (`gram.hpp`, `src_backend_parser_gram.cpp` and the rest) | The grammar sources, which merge cleanly | `make grammar-build` |
| `serialize_nodes.cpp`, `serialize_logical_operator.cpp` | `nodes.json`, `logical_operator.json` (trap 10) | `python3 scripts/generate_serialization.py` |
| Golden dumps `test/decide/golden/baseline.*.dump` | The merged binary | `test/decide/golden/capture.sh --solver <gurobi\|highs>`; **read the diff before keeping it** |

- [x] Resolve in this order, so each layer compiles against the one below it:
  1. Shared headers and enums: `logical_decide.hpp`, the term records (`decide_prepared_model.hpp` on mainline,
     renamed `decide_tree_terms.hpp` on OPA), `enums/decide.hpp`, `decide_term_split.*`.
  2. Binder and canonicalizer.
  3. Optimizer: `decide_linear_form.cpp`, the rewrites, `decide_optimizer.cpp` pass order.
  4. Direct solve (`src/optimizer/decide/direct/`): keep mainline's code; add OPA's declines (trap 14).
  5. Execution: the tree files are OPA's. `physical_decide.cpp` takes OPA's structure and mainline's behaviours
     (trap 11).
  6. Formulation (`src/decidb/formulation/`).
  7. Serialization, then tests, then docs.
- [x] Docs under `context/docs`: where both sides edited, keep mainline's text for mainline's features. OPA's docs
      describe behaviour by reference to a deleted evaluator and are due for a rewrite; do not polish them now.
- [x] After each file is resolved, run the formatter on it. *Three files were missed and formatted after
      the full build; the check then named only `logical_operator.hpp` (section 10).*
- [x] Commit the merge as one merge commit. Fixes found afterwards go in their own commits, not amended into it. *This
      file is part of that commit.*

## 6. Known traps

Each trap has a check. Tick it only when the check has been run.

### Trap 1. Objective sense flips twice ✓

- **What:** OPA reads `decide_sense` to choose which way to push a MIN or MAX (`decide_rewrite_frames.cpp`,
  `decide_rewrite_scoped.cpp`), and since the preparation `PhysicalDecide::BuildSolverInput` hands it to the solver
  as the sense. There are 50 references on the prepared branch. Mainline renamed the field `written_sense` and **already folded the flip into the
  term signs**.
- **Danger:** it will not compile, and the obvious fix, renaming the field, compiles and silently flips twice.
- **Do:** at each site decide from the term's sign, or always push down. Never rename mechanically.
- [x] **Check:** `grep -rn "decide_sense" src` is empty. A MIN and a MAX reducer, each under MAXIMIZE and under
      MINIMIZE, in a constraint, in the objective and over a range frame, give the same objective as the prepared
      `Sami-OPA` binary. The oracle tests `test_anr_frames.py`, `test_anr_nested.py` and the min/max tests pass.
      *`decide_sense` remains only in mainline's own parser and binder lines, none on `LogicalDecide` or
      `PhysicalDecide` (section 7, section 10). The 12 queries match the `Sami-OPA` binary on every row.*

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
- [x] **Check:** the searches in section 7. `test_bilinear.py`, the quadratic tests, and mainline's
      `test_product_normal_form.py`, `test_formulation_arms.py`, `test_optimizer_arms.py` pass.

### Trap 3. `decide_degree` was deleted on mainline

- **What:** mainline replaced `decide_degree.cpp` with `DecideDegreeOf` in `decide_product_form.hpp` and a new
  `decide_shape_check.cpp` (commit `c8c491d359`). OPA still calls `DecideExpressionDegree`, and it added a rule to
  the deleted file: a definition-function tag counts as a decision.
- **Do:** move OPA's rule into `DecideDegreeOf`.
- [x] **Check:** `grep -rn "DecideExpressionDegree" src` is empty; tests of nonlinear reducer bodies pass
      (`test_minmax_body_definition.py`).

### Trap 4. Source fragments against source spans

- **What:** mainline deleted `LogicalDecide::source_fragments` and the fragment-tag API for `DecideSourceText`
  spans (commit `1cd018decc`). OPA's `DirectRetainRowChecks` writes `source_fragments`, and OPA uses the old API in
  about 15 places.
- [x] **Check:** `grep -rn "source_fragments" src` is empty; mainline's `test_message_source_spans.py` passes.
      *`source_fragments` remains only as mainline's own deleted-property marker in the serializer (section 10).*

### Trap 5. `DirectAggregateOf` changed signature

- **What:** OPA changed it to take a vector of arguments, for collated MIN and MAX. Mainline's new shared
  `DirectAggregateSink::Add` still passes one. No conflict marker; it fails to compile.
- [x] **Check:** it compiles; `test_collated_keys.py` and the direct-solve tests pass.

### Trap 6. `variable_scopes` fallback

- **What:** mainline removed a bounds fallback in `decide_optimizer.cpp` (scopes now run parallel to decisions).
  OPA's helper decisions rely on the fallback.
- [x] **Check:** nested-reducer tests pass with helpers present; no out-of-range read (run the nested tests on a debug
      build if one is available). *The nested tests pass in all four modes on the release build. `variable_scopes` is a
      `duckdb::vector`, which checks every index in a release build too and throws INTERNAL, so an out-of-range read
      would have failed them. No current debug build exists, so the debug-only assertions were not run (section 10).*

### Trap 7. The ABS value check is lost on the solver path (R2)

- **What:** mainline moved `install_row_checks` to run **after** the ABS rewrite, because the rewrite now adds a
  scope check (`AddAbsValueCheck`). OPA runs its replacement, `retain_row_checks`, **before** the ABS rewrite. After
  a plain merge that check sits in `scope_checks` and nothing consumes it.
- [x] **Check:** mainline's tests for this check pass with direct solve on **and off**. The producer came in
      `a100f25d98`; its tests are the ABS cases in `test_invalid_value_message.py` (a NaN or infinite value under
      ABS, pressed by the objective, under Big-M, and in a constraint) and the edits to
      `test_direct_a2_single_facility_location.py`. `AddAbsValueCheck` appears in no test file, so search `src`
      for the producer and read that commit's test changes.

### Trap 8. Mainline's side-table fixes do not reach OPA's solver path (R2)

- **What:** OPA evaluates bound aggregates in the tree on the solver path. Mainline's fixes to the side tables land
  only on the direct path: NULL class keys and element-wise key comparison (`60566255a5`), BUG-45 (`5b14e8697b`).
- [x] **Check:** `test_bound_side_tables.py` (both sides rewrote it) and mainline's `test_direct_class_key.py` pass
      with direct solve on and off.

### Trap 9. Two fixes for one problem

- **What:** both sides fixed "a group with no instance" on 2026-11-13: mainline in `b5167002f8` and `a409a9503b`,
  OPA in `e153b7daf3` (`DecideGroupBound::held`). Mainline's new callers of `DirectTightestBound` use the default
  argument and get none of OPA's behaviour.
- [x] **Check:** mainline's `test_per_class_null_key_by_group.py` passes; the `bounds` fuzzer shows nothing new.

### Trap 10. Serialization ids collide

- **What:** in `LogicalDecide`, mainline uses id 221 for `objective_canonical_differs` and 222 for `source_text`.
  OPA uses 221 `signatures`, 222 `objective_signature`, 223 `frames`. Mainline also renamed 203 to `written_sense`
  and deleted 215.
- **Do:** renumber OPA's three to 223–225 and regenerate. `DecideScopeCheck` fields 110 and 111 depend on ruling R1.
- **Also check:** OPA's newer `LogicalDecide` vectors (`abs_function_envelopes`, `bilinear_function_groups`,
  `l0_indicator_functions`, `definition_functions`, `retained_*`) are not serialized. Find out whether they must be.
- [x] **Check:** `DECIDB_VERIFY_SERIALIZER=1 make decide-test` passes.

### Trap 11. Mainline behaviours that land in code OPA deleted

OPA's `physical_decide.cpp` no longer holds the code these commits edited. Each behaviour must be re-established
in the tree, descent or emitter, and proven by the test the commit itself added or changed
(`git show --stat <hash> -- test`).

| Done | Commit | Behaviour |
|---|---|---|
| [x] | `fd0994366d` | One product group carries every degree-2 term |
| [x] | `7fde155940` | One objective sense |
| [x] | `b5167002f8` | A `per K:` clause with a `by` reducer constrains only the groups its classes reach |
| [x] | `a409a9503b` | A composed MIN/MAX under `per K:` has no instance when no kept row has a class |
| [x] | `22e2a72fe1` | A constant coefficient that is NaN or infinite is named as such, on both paths |
| [x] | `1cd018decc` | A message that names a value quotes its span of the query |
| [x] | `5b14e8697b` | BUG-45: a written bound that reads data repairs in the data tier |
| [x] | `ae8ed8352d`, `b7743c0b5b` | Number-range warnings, one state per query for both paths |
| n/a (R1) | `bcc45b4e84` | BUG-54: determination message (only if R1 keeps the determination error) |
| [x] | `60566255a5` | The class key has no flag; a NULL puts a row in no class |

`88c5f87385`, `b0a489da4e` and `b20600d087` only deleted unreachable code; nothing to carry.

### Trap 12. R1's sink moved

- **What:** mainline moved R1's aggregate sink into the shared `DirectAggregateSink`. OPA edited the old code in
  `r1_continuous_knapsack_rule.cpp` for vector arguments.
- [x] **Check:** `test_direct_r1_continuous_knapsack.py` passes.

### Trap 13. The two-level objective

- **What:** OPA removed `BindTwoLevelObjective` in favour of nested reducers in the objective; mainline edited it
  for the sense change. Affects `decide_objective_binder.cpp`, `decide_rewrite_minmax.cpp`,
  `linearization_minmax.cpp`.
- **Do:** take OPA's form and apply mainline's sense rule to it (trap 1).
- [x] **Check:** objective tests with `outer(per k: inner(e) by (k))` pass under both MAXIMIZE and MINIMIZE.

### Trap 14. Direct solve must still fire

- **What:** in `direct_ir.cpp` keep mainline's `ReadKey`, table facts, `model_shape` and sign handling. Add OPA's
  declines for what the rules cannot answer: a scope with a signature, an objective signature, frames.
- [x] **Check:** section 8.4.

### Trap 15. Tests that expect the determination error (R1)

Affected on mainline: `test_scope_determination.py` and `test_scope_check_plan.py` (OPA deletes both),
`test_message_source_spans.py`, `test_qualified_reducer.py`, `test_scope_reducer_inner.py`, `test_collated_keys.py`,
`test_scope_parser.test`, `test_direct_s1_uniform_matroid.py`, `test_scope_combinations.py` with its oracle
`_scope_oracle.py`.

- [x] **Check:** each is resolved according to R1, and listed in section 10.

### Trap 16. OPA's stored snapshots go stale (R3)

- **What:** 259 files under `test/decide/tests/expected/` hold output recorded from a deleted evaluator. 99 hold
  `sense: maximize` model dumps, which trap 1's change invalidates.
- **Do not re-record them.** That would store whatever the merged binary prints and call it correct.
- [x] **Check:** per R3, the stored-output tests are deleted, or the owner has said otherwise. *Deleted (R3).*

### Trap 17. Model dumps change sign

- **What:** because every objective becomes a minimization, a model dump of a MAXIMIZE query shows
  `sense: minimize` with negated objective coefficients. This is expected, not a regression.
- [x] **Check:** golden diffs are read line by line; anything other than the sense line and objective signs is
      explained in section 10. *Read; see the golden-baselines row.*

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

- [x] `make release` succeeds.
- [x] `PATH="$PWD/test/decide/.venv/bin:$PATH" python3 scripts/format.py --all --check` reports only
      `logical_operator.hpp`. Never run `make format-fix`.
- [x] `git diff --check` is clean.

### 8.2 Suites, every mode

- [x] `make decide-test`
- [x] `build/release/test/unittest "[decidb]"`
- [x] `DECIDB_FORCE_SOLVER=highs make decide-test`
- [x] `DECIDB_VERIFY_SERIALIZER=1 make decide-test`
- [x] `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`

Run each mode as its own command and check its exit status. `test/decide/run_direct_matrix.sh` runs all four, but
do not treat it as the gate ✓: it passes the caller's `DECIDB_*` switches into every mode (a shell with
`DECIDB_FORCE_SOLVER` set makes its `auto` run a HiGHS run), and it judges a mode by the printed pytest summary, not
by the command's exit status. If you use it, clear the three mode switches first and read each log's last lines.
Fixing the script is its own commit, after the merge.

The merged suite should hold every test of both parents, less the ones deleted under R1 and R3.

### 8.3 Fuzzers

- [x] `cd test/decide && .venv/bin/python3 -m fuzz all --seed 1 --count 2000 --jobs 8`
- [x] The same with `--count 400 --solver highs`

Expected: nothing beyond the failures recorded in section 4.

### 8.4 Direct solve

- [x] All `test_direct_*` tests pass.
- [x] For a query each rule answers on mainline (R1, R4, S1–S3, A1–A2, N1, O1), `EXPLAIN` on the merged branch still
      shows the direct plan. Mainline's tests under `test/decide/tests/test_direct_*` name such queries.
- [x] The direct-solve benchmarks under `benchmark/decide/direct_solve/` run and do not regress against mainline.
      *Section 4.1.*

### 8.5 Every changed output is explained

- [x] Capture the merged branch's suite output and compare it with each parent's capture:
      `test/decide/suite_log.py <parent_dir> <merged_dir> --limit 400`.
- [x] Sort each difference into an expected kind and log the kinds in section 10. Expected kinds: the sense line and
      objective signs in model dumps; span-quoted messages; determination against variants (R1); a different but
      equally optimal assignment. **Anything else is a finding.**

### 8.6 The tree still follows the deck

- [x] `repro/deck_example/f1_max.sql`, `f1_sum.sql` and `f2.sql` give the models of the deck's pages 115, 119 and
      108. Compare with `repro/deck_example/model_opa_*.txt`, allowing for trap 17. *Each differs only by trap 17;
      `f2`'s answer is identical.*
- [x] `DECIDB_DUMP_TREE=1` on `f1_max.sql` still prints the tree of the deck's page 81. *Byte-identical to the
      `Sami-OPA` binary's (26 lines).*

### 8.7 Threads and performance

- [x] `tools/threads_test.py 500000` exits 0, with the merged binary added to its `BINARIES`. The script fails a shape
      when a run errors, when the objective differs between 1 and 8 threads, or when an 8-thread run combined fewer than
      two thread-local states. On the prepared `Sami-OPA` binary all eight shapes pass with five states. *Section 4.1.*
- This is a smoke test, not proof. Handoff A7 items 6 and 7 (invariants I5 and I6) and the 23
  `DECIDB_DESCENT_SPLIT=1` failures stay open after it passes; they are rewrite work (handoff A5, Phase 0, item 4).
- [x] `tools/ab_bench.py` against the prepared `Sami-OPA` binary: no workload moves outside noise (about 10%).
      Edit `BINARIES` at the top of both scripts first. *Section 4.1.*

### 8.8 Known defects, re-run

- [x] Re-run each confirmed defect in handoff.md, section A7, and record its status on the merged branch. Expected: the
      product-floor defect, wrong on mainline, is now right; defect 1 stays fixed (the preparation fixed it, and
      `test_a_bound_that_reads_the_rows_holds_on_each_under_a_whole_table_nest` guards it); the others are unchanged.
      *Section 10: as expected, except that defect 2 is fixed on the merge; defect 5 has no reproducer and was not run.*

## 9. After the merge: ready to start the rewrite

- [x] Update `handoff.md`: section A2 (branches), A3 (where the tree is built, with new line numbers), A5 (the plan,
      with the merge done), A7 (defect statuses).
- [x] Re-find every `file:line` reference in `handoff.md` that the rewrite will rely on, by identifier. *Each still
      names its code; the one whose code the preparation deleted (`_old_spellings.py`) lost its line number.*
- [x] Record the merged branch's own baseline (suite counts, fuzz results, benchmarks) as the new section 4.
      *Section 4.1.*
- [x] List what the merge left deliberately rough, for the rewrite to replace: the emitter's port to product groups,
      and whatever R2's stop-gap left in two implementations. *Section 10, the rows marked Rough.*
- [ ] Remove the three review worktrees under `.claude/worktrees/` once nothing needs their binaries. *Open:
      `sami-opa` holds the OPA parent's binary, which the rewrite compares against. The owner decides when.*

## 10. Decisions and deviations

Append one line per decision made during the merge, per test deleted, and per unexpected difference.

| Date | What | Why | Who decided |
|---|---|---|---|
| 2026-11-16 | Preparation: two tests in `test_one_pass_tree.py` read a node's reads from the dump's `reads=` field instead of the deleted `formula=` text | Same queries and same property (a function's definition rows do not read it back; a product's function reads its reducer). Only the probe changed | Reviewer; owner to confirm |
| 2026-11-16 | Preparation: five defensive checks went with the node algebra (one producer per helper variable; decision index in range; Boolean-product body matches its fields; ease agrees with sense; empty helper link) | They checked the records through the algebra. An independent review found none reachable by today's tree construction. The rewrite replaces `Validate` | Reviewer |
| 2026-11-16 | Preparation: a product or square over a nested reducer, under a bound that reads the rows, is now refused by name ("squares or multiplies decisions inside nested reducers") | It answered wrongly at `7837e300be`. It answered correctly at `36f12dd089`, by a path later commits removed. Restoring that path is rewrite work | Reviewer; owner to confirm |
| 2026-11-16 | R1: variant signatures replace the determination error | The owner chose OPA; variants are the deck's signature mechanism | Owner |
| 2026-11-16 | R2, for the merge only: the solver path keeps OPA's tree evaluation, with mainline's fixes ported into it | The smaller change. The lasting choice belongs to the rewrite | Owner |
| 2026-11-16 | Checklist corrected after Codex's review: the mainline capture needs the `_log_query` hook; the matrix runner is not a gate; `threads_test.py` now fails properly; trap 7's lookup command was wrong | Each point was checked against the code and held | Reviewer |
| 2026-11-16 | Merge: `DecideProductGroup` replaces OPA's `BilinearConstraintTerm`, `QuadraticGroup`, `squared_terms` and the objective's `BilinearTerm`; OPA's per-term fields ride on it, and the splitter is mainline's degree-based one plus OPA's three readings (helper-tagged functions, `ExpandBilinearGroups`, frame terms) | Trap 2: mainline's representation is kept; OPA's evaluator is ported onto it (R2) | Merger |
| 2026-11-16 | Merge: trap 3, OPA's "a definition-function node is a decision of degree 1" moved into `DecideDegreeOf` | One reader decides degree for every pass | Merger |
| 2026-11-16 | Merge: `decide_tree_definition.cpp` (OPA's) carries mainline's one sense, `IsCheapLoneExtremum` over canonical atoms, `WithoutWrittenName` in `SameReducer`, and the helper row's source clause (`DefinitionFunction::source_clause_id`) | Mainline's fixes to the deleted `DefineNonlinearReducerBodies`, ported to the pass that replaced it | Merger |
| 2026-11-16 | Merge: pass order is OPA's with mainline's `RecordReducedBounds` and `TagAtomicRemovalGroups` first; `RetainRowChecks` runs after the late `RewriteAbs` so mainline's ABS value check is kept (trap 7) | Every pass that adds a check runs before the checks are kept | Merger |
| 2026-11-16 | Merge: the two-level objective path (`per_*` metadata, `SplitDecideTwoLevel`, PATH B in `linearization_minmax.cpp`) is deleted; OPA's nested reducers replace it | Trap 13 | Merger |
| 2026-11-16 | Merge: helper sharing (`Helpers::Find`) keys on mainline's five parts (reducer ignoring aliases, path key, own `per`, rows, clause's `per` key), with OPA's path key in place of the `by` key | Mainline's 2da24bdc35 on OPA's path-keyed helpers | Merger |
| 2026-11-16 | Merge: `decide_solver_gate.cpp` reads `DecideTree::HeldClauses`, not `Constraints()`: the gate runs before `DecideTreeConstruction::Finish` numbers the constraints | Hidden break found by the golden capture: a quadratic clause reached HiGHS | Merger |
| 2026-11-16 | Merge: OPA's `IsExtremumName` in `decide_tree_definition.cpp` deleted; mainline moved the same function to `decide_optimizer_internal.hpp` | Hidden compile break (ambiguous call in the unity build) | Merger |
| 2026-11-16 | Merge: `RewriteProduct` puts a re-spelled product's tags on the product under a cast back, not on the cast | Hidden break: mainline's re-canonicalization in `AddConstraint` moved OPA's Boolean-product tag onto a cast, so HiGHS refused `b * SUM(x)` clauses and Gurobi hit an INTERNAL error. A decision cast is transparent to every reader (`UnwrapDecideCasts`) | Merger |
| 2026-11-16 | Merge: `ToChunkReferences` copies each node and maps only its leaves (mainline's b20600d087 ported) | Missed silent port: section 6 said b20600d087 only deleted unreachable code; it also kept DuckDB's error location on the solver path | Merger |
| 2026-11-16 | Merge: `close_rounded_box` in `ilp_model_builder.cpp` kept (OPA) | One side's fix mainline did not touch. On `3 * x <= 0.3 AND x >= 0.1`, Gurobi still reports infeasible on both OPA and the merge (presolve tolerance); left rough, no test covers it | Merger |
| 2026-11-16 | Merge: serialization ids: OPA's `signatures`, `objective_signature`, `frames` renumbered 221-223 -> 223-225 after mainline's 221 and 222; `DecideScopeCheck::key_order` (111) marked deleted with `key` (110) | Id clash; R1 deletes the order's only reader | Merger |
| 2026-11-16 | Golden baselines regenerated | HiGHS results identical to mainline's. Gurobi differs on corpus #93 and #100 only: equally optimal assignments, matching OPA's. Dumps: 60 lines against mainline, all OPA's model changes; against OPA after flipping sense, one model (#91 `MAXIMIZE MAX(x + price)`), byte-identical to mainline's (one-sense rule) | Merger |
| 2026-11-16 | R1 rewrite: `test/decidb/test_scope_parser.test` takes OPA's variant result (200) for `SUM(D: ship) by (D.region)` | Mainline expected the determination error | Owner (R1) |
| 2026-11-16 | R1 deletions: `test_scope_check_plan.py`, `test_scope_determination.py` (with mainline's four BLOAT-25 additions), the `row_check` case in `test_direct_user_facing.py`, three `_assert_no_check_planned` checks and their helper in `test_collated_keys.py`, `test_a_by_key_that_is_a_case_is_quoted_as_written` in `test_message_source_spans.py` | Each expects the determination error or its planned check | Owner (R1) |
| 2026-11-16 | R1 rewrites to variants: `test_qualified_reducer.py` (two tests), `test_scope_reducer_inner.py` (one test and one error case), `test_direct_s1_uniform_matroid.py` (one assertion) | Mainline expected the determination error; OPA's tests expect variant answers | Owner (R1) |
| 2026-11-16 | Deleted with OPA: `test_scope_bare_clauses.py::test_data_reducer_scaling_a_decision_inside_a_reducer`; `test_direct_s1_uniform_matroid.py::test_s1_raises_the_checks_of_two_clauses_in_the_solvers_order` (mainline deleted it too) | A data reducer scaling a decision inside a reducer is a coefficient (S10) | Merger |
| 2026-11-16 | Adapted: four decline shapes in `test_decidb_direct_decline.cpp` (N1, S3, S2, R4) written as OPA's `ObjectiveByFlag` IR edit | The merged front end refuses `MAXIMIZE ... by (flag)` before the rule (D72), as OPA found for S1, A1 and R1 | Merger; owner to confirm |
| 2026-11-16 | Adapted: `test_decidb_helper_inlining.cpp` reads product groups in two cases where OPA had four product and square cases | One loop reads every product group | Merger; owner to confirm |
| 2026-11-16 | R1 deletions: `test_collated_keys.py::test_a_message_quotes_its_clauses_key_and_lists_the_values_in_that_order` (3 cases) and `::test_a_reducers_message_quotes_the_key_that_reducer_wrote` (2), with their helper `_written_order_source` | Each asserts only mainline's determination message (BUG-54, b504861a1d); variants replace determination | Owner (R1) |
| 2026-11-16 | Merge fix: `SwapObjectiveSummation` reads and writes a weighted square as `**` (`IsDecidePowerCall`, `DecideRebindOperator`), deleting its `power`/`pow`/`x * x` arms | Mainline spells every power `**` from stage 04; OPA's pass wrote `power` by name, which mainline's readers take as an opaque term (INTERNAL) | Merger |
| 2026-11-16 | Merge fix: `PrepareDefinitionFunctions` puts the definition-function tag under the casts; its refusal quotes the written reducer (`RenderDecideSource`, MSG-1) | Every reader looks through a cast before reading tags (as row 9); a message quotes what the user wrote, and mainline's own cases write `power(...)`, so they print the same | Merger |
| 2026-11-16 | Merge fix: `LinearizeAbsMaximize` gives its two link rows one coefficient label per entry | Mainline's builder reads the labels by position (BLOAT-30); OPA's link rows had none (INTERNAL on HiGHS) | Merger |
| 2026-11-16 | Merge fix: `DecideTermSplitter` is built from the `LogicalDecide`, so it always reads the node's product groups | `AddAbsValueCheck` (mainline) built one without them and read a group's function as opaque (INTERNAL on `norm(b * x, 0, 10)`); the direct front end omitted them too (harmless there: no group exists yet) | Merger |
| 2026-11-16 | Merge fix: `RewriteAvgInExpression` rewrites only an AVG over decisions (OPA's guard restored) | A data AVG is a frame's marker or a reducer reading a frame on OPA's side, computed after the frames; mainline's form turned it into a SUM (wrong answers) | Merger |
| 2026-11-16 | Merge fix: `DecideTermsEqual` compares with `DecideExpressionsEqual` | A frame marker names its frame only by its tag; mainline's comparison merged `at(previous: v)` and `at(next: v)` as like terms (INTERNAL "frame is in no clause") | Merger |
| 2026-11-16 | Merge fix: `AbsReadRows` combines the clause's and the reducer's `when` as a conjunction | Mainline's 9cfc398526 built a CASE, which hides the reducer's NULL test from OPA's tree gates (INTERNAL "cannot resolve source column") | Merger |
| 2026-11-16 | Merge fix: a composed MIN/MAX clause counts only the rows its `per K:` root holds in a class | Mainline's a409a9503b ported onto OPA's tree counted the root's unclassed slot, so a clause with every key NULL kept one instance and no class (wrong answer) | Merger |
| 2026-11-16 | Merge fix: `RemoveUnusedColumns::NarrowCTEReferences` rebuilds every positional projection map (a filter's, an order's, either side of a join's), children first | OPA's e5ba21194d gives the side-table join a positional right map, which the narrowing left stale (INTERNAL index error) | Merger |
| 2026-11-16 | Merge fix: `DecidePropagateImpliedBounds` boxes no upper end of -inf | OPA's 346f1fe8e7 keeps a bound under the floor; one of -inf reached the builder's finite-end check as TOO_LARGE, where mainline answers infeasible | Merger |
| 2026-11-16 | Test audit (duplicates across parents): removed `test_bilinear.py::test_a_bool_times_a_shifted_decision_distributes` (2 cases) | Same property and branch as base `test_boolean_times_an_affine_factor[a-constant-subtracted, a-constant-added]`, which judges every assignment on both solvers | Merger (owner's request to remove unnecessary tests) |
| 2026-11-16 | Test audit: removed `test_collated_keys.py::test_the_tree_folds_every_read_of_a_key_into_its_least_spelling`, `::TestACollatedReducerOverData::test_the_tree_folds_a_range_with_its_bound_reducer`, `::TestACollatedOrderKey::test_the_tree_names_a_position_by_its_least_spelling`, and `test_invalid_value_message.py::test_the_tree_names_a_root_as_flat_names_its_row[constraint_on_a_keyed_decision_by_group]` | Each was OPA's tree twin of a flat test; since 19784f71b6 dropped `DECIDB_EVALUATOR`, each runs the same query and assertion as the twin that stays | Merger (owner's request) |
| 2026-11-16 | Test audit: removed from `test_one_pass_execution.py` `test_tree_values_reach_the_decks_model` (3), `SIGNATURE_CASES[a-nested-reducer-value-is-its-key]`, `test_a_lowered_auxiliary_is_no_field_of_the_objective`, `test_a_member_test_is_no_field_of_a_per_clause` | Each runs the query of a test that stays: `test_anr_nested` (fixture answers, model sizes, the function-of-one-column key), `TestPoint` (F2 sizes), `test_abs_objective_sign_under_sense[maximize-reducer]` and `test_easy_extremum_own_when_binds_only_its_members[per-key]` (both judged on both solvers) | Merger (owner's request) |
| 2026-11-16 | Test audit: removed `test_one_pass_noside.py::test_generated_presence_tests_read_computed_children` (3) | Same fixtures as `test_anr_frames.py::TestPoint::test_the_engine_answers_each_fixture`, judged by the oracle; no direct rule takes them | Merger (owner's request) |
| 2026-11-16 | Test audit: removed `test_fuzz_defects.py::CASES[a-group-in-no-class-holds-no-instance]` | Both parents fixed this defect; `test_per_class_null_key_by_group::test_a_group_no_class_reaches_is_free` stays, oracle-judged in off and require | Merger (owner's request) |
| 2026-11-16 | Test audit: removed `test_anr_nested::test_a_nest_no_value_can_meet_is_infeasible[*-highs-lowered]` (5), `test_anr_inlining::TestIntoMembers::test_the_deck_writes_the_sums_in`, `test_anr_variants::test_a_class_is_one_instance_per_variant[F10_split]`, `test_scope_variants::test_an_objective_decision` and `::test_an_objective_value_and_by_key` | HiGHS declares no native construct, so `DECIDB_NATIVE_CONSTRUCTS=off` changes nothing there; the others repeat `test_the_running_example_is_a_helper_per_path_group[F1_sum]`, `test_explain_analyze_counts_the_variants`, and `test_scope_objective::test_objective_tuple_term_errors` | Merger (owner's request) |
| 2026-11-16 | Test audit: removed `test_one_pass_execution.py::test_an_ungrouped_root_is_one_model_row` | Base `test_ne_range_collapse::test_aggregate_ne_zero_collapses_to_a_single_tight_row` runs the same `SUM(x) <> 0` BOOL query lowered on the solver path and asserts the one row, its sense and its bound | Merger (owner's request) |
| 2026-11-16 | Deleted `test_tree_emitter.py`, `tests/_expected.py` and `tests/expected/` (259 stored files); `08_execution/done.md` no longer cites them | They compare against output recorded from OPA's deleted flat evaluator. On the merge, 99 ids differ only by mainline's single objective sense (a MAXIMIZE is stored negated) and 7 by mainline's error ranking; re-recording would store the merged output as its own reference, and the behaviour each pinned is judged by the oracle tests | Owner (R3) |
| 2026-11-16 | A3: a frame over decisions prints as DeciDB's spelling of the frame (lower case, defaults dropped) in a plan's layers, a helper's label and the frame column's name: `DecideFrameSpec::spelling` (serialized, `nodes.json` id 123), set by `BindDecideFrame` from the parsed frame's `ToString`; `DecideSourceNames`/`RenderContext` carry the frames and `RenderAggregate` prints the spelling for a marker the render does not quote. Messages keep the written span (`DecideFrameSpec::text`) | Mainline's plan rule reprints any node that reads a decision, and a frame marker's node is `first(e)`, which no one wrote; OPA printed its own spelling through a source-fragment tag the merge replaced with spans | Owner (A3, option P) |
| 2026-11-16 | T1: the direct-rule tests declare the solver path's DECIDE warnings, each computed from the case's rows (a2 `_agree`/`_agree_abs`/keyed/boundary, r4 `_warnings`, n1 objective and NULL-limit error case, o1 zero cost, r1 scoped objective, `test_reducer_filter` when-known, `test_large_number_warning[0]`); `agree_with_oracle` passes them on its infeasible/unbounded branch too, and `outcome` takes them | OPA's warnings ("the objective is 0 whatever the decisions", "'…' constrains nothing"), printed only on the solver path; OPA's a1 and r1 tests declare them the same way | Owner (section 2) |
| 2026-11-16 | T2: `test_error_precedence` gains an `abs_value` phase after `side_plan` (the `norm(x + t.gap, 0)` fault, now refused by mainline's ABS value check, a100f25d98); FORMULATION's fault is a Big-M row whose reach passes a DOUBLE (`max(x + t.far + t.far) >= 0`, far = 1e308); `SECOND_ABS` and `SECOND_EXTREMUM` take the written-order rows; `_finite_bound`, `EXTREMUM_ROW` and `SECOND_NORM` deleted | FINITE_BOUND was deleted on mainline (8b5322264b), and the ABS check moved the old fault earlier. The approved `>= 3` became `>= 0` so the fault-free table still answers (a MAX of zeros meets 0); the new phase's place was measured by running each pair | Owner (section 2) |
| 2026-11-16 | T3 and G9: `test_error_precedence` keeps `require` only for a case with no answer; the S1 side-plan `require` order rows deleted (2 + 9) | "Error order is not owed" (`direct_solve/framework/done.md`); S1 owes none (651fff4c76) | Owner |
| 2026-11-16 | T4: messages quote the written text: `test_anr_frames` (4 refusals), `test/decidb/test_frame_parser.test` (the `within ()` count message and five refusals), `test_invalid_value_message[row_of_an_abs_in_the_objective]`, `test_one_pass_noside` (plan read whitespace-free) | Mainline's MSG-1; mainline rewrote its own tests the same way (1cd018decc) | Owner (section 2) |
| 2026-11-16 | T5: `test_anr_inlining::TestIntoMembers` expects negated objective coefficients for a MAXIMIZE (5) | One objective sense (trap 17): the model minimizes | Owner (section 2) |
| 2026-11-16 | T6: `test_reducer_helper_sharing` reads six rows, not three | On three rows D89 writes the helper's sum into the rows, so there is no shared helper to count; on six the merge gives mainline's counts | Owner (section 2) |
| 2026-11-16 | T7: `test_written_text::test_a_reducer_in_a_macro_body_keeps_its_name_in_the_bound_check` asserts the run-time message (`the bound ts(cap) is NaN on some rows`) instead of the check's text in EXPLAIN | R2: the tree evaluates the check, so the plan holds no CASE that carries the message | Owner (section 2) |
| 2026-11-16 | T8: `test_function_aliases` data-aggregate refusal written as `avg(v) * SUM(x)` | OPA reads `SUM(avg(v) * x)` as a coefficient (S10); the refusal it tested still holds for a factor outside the reducer | Owner (section 2) |
| 2026-11-16 | T9 and A-4: `test_tree_bodies` member-test and frame regexes updated; `[a-square]`, `[a-product-of-one]`, `[objective-squares]` read the `group N ... square=` lines and `block=PRODUCTS` | Mainline's product groups replace OPA's SQUARES and `product` dump lines (trap 2) | Owner |
| 2026-11-16 | Q-293: `test_frame_parser.test` expects mainline's outer-column message for a frame ordered by `o.id` in a correlated DECIDE; `ExpressionBinder`'s `DECIDE_FRAME` case is an `InternalException`; its row in `language/errors.md` deleted, `02_binder/done.md` states the invariant | Probed both binaries over ten shapes (order key, `within`, payload, sibling clause, objective, WHERE, LATERAL, range reducer, plain SELECT): the parser builds a frame only in a DECIDE clause and the DECIDE binder refuses every outer column first (b98b621051). OPA reached the branch in four shapes and failed INTERNAL on a range reducer | Owner |
| 2026-11-16 | Q-T10: `test_invalid_value_message[reach_of_a_group_sum_extremum]` deleted (2) | Criterion (a): its subject, the two-level path, is deleted (trap 13). MINIMIZE now solves on Gurobi and HiGHS refuses the range; MAXIMIZE raises TOO_LARGE, but HiGHS names the helper's plan label (rough, below) | Owner |
| 2026-11-16 | A-1: `test_error_binder::test_objective_shape` two-level refusals deleted (7; the `count(x)` nest kept) | The merge answers four of them with OPA's nested reducers; the other three name code that no longer exists | Owner (section 3) |
| 2026-11-16 | A-2: `test_scope_explain_diagnose` two-level EXPLAIN tests deleted (3) | They print PATH B's accounts (deleted, trap 13); OPA's `test_explain_a_nested_objective_accounts_for_each_helper` covers the tree's | Owner (section 3) |
| 2026-11-16 | A-3: `test_error_binder::TestRefusalsInEverySpelling` reprint tests deleted (7 + `a-literal-that-reads-like-a-tag` + `a-subquery-at-bind`) | They test OPA's source-fragment reprint, which mainline's span quoting replaced; `test_message_source_spans` covers spans | Owner (section 3) |
| 2026-11-16 | A-5: `test_anr_row_order[a-side-plan]` deleted | No side plan exists after R2 | Owner (section 3) |
| 2026-11-16 | Rough (D-3): ABS over a reducer in the objective is read per row: `x <= cap (3, 3, 2) MAXIMIZE 0 * sum(x) + abs(sum(x) - 2)` answers 2 on OPA and the merge, 6 on mainline | An OPA bug, not from the merge; fixing it means designing in OPA's lowering (R2). For the rewrite: a failing test with the fix | Owner |
| 2026-11-16 | Rough: a helper link's TOO_LARGE names its plan label on HiGHS (`max(per id: sum(b * y) by (id))`), where Gurobi names the clause as written | `linearization_reducer_value.cpp` sets `spec.written = link.label`; both parents do the same. For the rewrite: name the written clause | Owner (Q-T10) |
| 2026-11-16 | Trim G2: `test_anr_nested::test_a_data_reducer_inside_a_nest` 272 -> 9 cases, the auditor's set: each outer kind and each outer scope, each inner scope, and `count` | The inner reducer over data is DuckDB's own aggregate (`MakeReducerColumn`), with no DeciDB branch on its kind; the outer kind (MIN closing or envelope), the outer scope, the inner `when` and `per`, and `count` each keep a case | Owner (G2) |
| 2026-11-16 | Trim G5: `test_anr_frames::TestRange::test_every_endpoint_pair` (432) replaced by `test_the_ends_of_a_range` (6), one case per branch set of the window listing | The auditor's simulation of `ListRangeWindows` over the fixture finds 20 branches in 43 branch sets; six cases cover every branch | Owner (G5) |
| 2026-11-16 | Trim G12: `TestATightestFrameBound[*-under-a-cast]` (11; the INT column `ci` deleted), `one_pass_tree` `[F1_sum]` (2) and `one_pass_literal` `[sum]` (1), `test_a_data_reducer_scaling_a_decision_inside_a_reducer["per row: "]` (5), `test_refused_nests[reducer-in-l0-norm]`, `fuzz_defects` equal-bound `[auto]` (3), `TestAnAbsUnderAWhen` `[rejected-first-by-key]`, `[kept-first-max-of-abs]` and the plain EXPLAIN's `[kept-first]`, and `test_scope_variants` `test_a_by_key_that_varies_inside_a_class`, `test_a_decision_outside_reducers`, `test_an_inner_per` | The tightest-bound tag rides on the comparison (206404db0a); the descent never reads an aggregate's name; `per row:` binds as no `per`; the norm refusal reads no order; `require` confirms no rule takes the equal-bound queries, so `auto` is `off`; a plain EXPLAIN reads no row; each `scope_variants` test repeats a scope path that F12, `test_scope_combinations`, `test_scope_reducer_inner` or `[by-key]` reaches | Owner (G12) |
| 2026-11-16 | Trim M2: a2 `test_a2_a_maximized_negated_maximum_absolute_loss_applies_and_agrees` (3), s3 `[at_least_maximized_negated_count]`, r4 `[budget_literal_on_the_left]`, `[capacity_with_the_floor_written]`, `[capacity_between_constants]` (their budget and box entries deleted), o1 `[abs_negated_outside]`, `product_normal_form` `_SQUARES["cast exponent"]` | One objective sense and the canonicalizer give each the plan of a case that stays (`max_odd_in_*`, `[at_least_worked_example]`, `[worked_example_the_budget_binds]`, `[three_rows_a_capacity_binds]`, `[capacity_a_constant]`, `[abs_min]`, `TestOneSpellingOfAWholeExponent[per row]`) | Owner (M2) |
| 2026-11-16 | Trim M7: `test_direct_rule_conformance` `off`/forced-backend and DIAGNOSE gates 9 -> 1 rule each | `ChooseDecideAnswer` passes those gates before `ApplyDirectSolveRules` | Owner (M7) |
| 2026-11-16 | Trim M8 (aliases): `test_function_aliases` 17 cases; each test keeps one or two spellings, and O1 keeps `@`, the alias its own suite does not write | `RespellDecideAliases` is one catalog lookup with one branch for every alias | Owner (M8) |
| 2026-11-16 | Trim G4: `test_anr_oracle_selfcheck` 963 -> 154. The scope-grid agreement 784 -> 8 (one cell per front, the decision, body, aggregate and inner rotating), the two-level objective 32 -> 4, `test_error` 9 -> 4 (the first case of each error kind); `test_optimum` (59) stays | These cases test the reference oracle, not DeciDB, so an instruction trace of `_anr_oracle.py` and `_scope_oracle.py` judged them: the kept cases reach every instruction the 963 reached. Without `test_optimum`, `[objective_absent]` and `[objective_one_timeline]` held 14 instructions nothing else reaches (an absent objective term, a product inside the objective), so it stays whole rather than as a list of two names | Owner (G4) |
| 2026-11-16 | Trim G10: `test_fuzz_smoke` 23 cases: the `ignores-nest-per` pair and its smoke seed, 11 of 13 `nests.refusals` shapes, the refusal's `>=` seed, `bounds`' two-values and `order_keys`' NULL placement, `runner.execute` on the real CLI (3), the INTERNAL stand-in of `failing_cli`, and the run on a Gurobi CLI | An instruction trace of `test/decide/fuzz`, `_anr_oracle.py` and `_scope_oracle.py` finds every instruction these reached also reached by a kept case (each module's oracle agreement and broken oracles, `test_judge`, the recorded failure's INTERNAL stand-in). The 20 cases that hold an instruction of their own stay, and so does `test_coverage_tooling`, whose host-dependent tests read a path the host does not take | Owner (G10) |
| 2026-11-16 | Coverage check for every trim and audit removal in this table. First pass: `make decide-coverage` with `COVERAGE_SOURCES` naming `optimizer/decide`, `planner/decide`, `planner/expression_binder/decide`, `execution/operator/decide` and `decidb`, before and after the trims (11,110 arms; `compare` finds none lost). That pass read only the files directly in each folder: `coverage_report.py`'s `_scope` globbed with pathlib, where `*` stops at `/`, while Clang instruments every file below. Second pass, over all 17,442 arms (the direct rules, formulation, diagnostics and both backends included): the after-trim suite's profiles merged into one, against each test gone since the index (2,078, re-run from the index's files on the same binary; among them the 92 tests G7 moved from a forced backend onto `decidb_cli`, in their old form, so a formulation arm one of them reached only with direct solve off would show) | One arm is reached only by a gone test, and that test is A-1's, not a trim's (the row marked Open). Three M8 cases came back on the first pass. The pure-Python tests (G4, G10) start no CLI, so an instruction trace judged them. The index predates T2, so the 13 `abs_value` pairs G8 removed were not re-run; their fault query still runs in the kept `side_plan-abs_value` and `abs_value-frames` pairs | Owner (coverage) |
| 2026-11-16 | Trim G1: `test_anr_nested::test_grid_cell` 768 -> 32, the auditor's set (Note 1): every level's kind and scope at depth 2 and 3, rotated so each pair of neighbours meets | The nest is built level by level; no arm reads two levels' kinds together that the 32 do not reach | Owner (G1) |
| 2026-11-16 | Trim G3: `test_anr_nested::test_a_constant_bound_on_a_nest` 48 -> 16, one sense per outer and inner kind, rotating | Each outer kind, inner kind and sense still meets; coverage finds no arm only the removed cells reach | Owner (G3) |
| 2026-11-16 | Trim G6: `test_anr_frames::TestRange::test_a_range_over_data_is_a_value` 28 cases: `timeline` and `reversed` with `else` or `all`, and every `max` | Each window, each fallback and the kept reducers stay; coverage finds no arm only the removed cases reach | Owner (G6) |
| 2026-11-16 | Trim G7: 82 HiGHS halves in `test_anr_nested`, `_composition`, `_inlining`, `_frames` and `test_scope_bare_clauses`. The 21 tests whose every HiGHS half goes take `decidb_cli`, the suite's backend (Gurobi by default, HiGHS under `DECIDB_FORCE_SOLVER=highs`); `test_a_nested_extremum_under_a_max_takes_its_push` keeps only its unbounded case. Six tests keep HiGHS for the cases the auditor named, through `_scope_cases.on_highs_too` | The HiGHS halves reach no arm the Gurobi halves and the pinned HiGHS tests do not; the HiGHS mode still runs every `decidb_cli` case on HiGHS. The kept HiGHS cases (the hard MIN/MAX, `<>`, the pushed-up ABS, the groups of no class, the negative nest bounds, the empty nests, `x >= -1`) hold no arm of their own either; they stay as the auditor named them, so the default run still judges HiGHS on those shapes | Owner (G7) |
| 2026-11-16 | Trim G8: `test_error_precedence::test_the_earlier_phase_raises` 133 -> 21 pairs: each phase against the next, and the held faults against their neighbours | The phases run in order and the first error ends the query, so adjacent pairs pin the whole order by transitivity | Owner (G8) |
| 2026-11-16 | Trim M1: mainline's two-level path tests `test_objective_group_sums_over_two_open_decisions_name_the_first`, `test_a_two_level_max_over_a_squared_body_is_stated_natively`, `test_a_two_level_objective_prints_its_sign_and_factor` (2) | Coverage finds no arm only they reach | Owner (M1) |
| 2026-11-16 | Trim M3: 15 error cases of a2, n1 and s3 whose messages `test_user_facing` pins word for word | Each message stays pinned by `test_user_facing`; coverage finds no arm only these reach | Owner (M3) |
| 2026-11-16 | Trim M4: 9 non-finite cases of a2, n1, r4 and s3, one per value kept | Each value passes one `std::isfinite` check, which NaN and both infinities take alike | Owner (M4) |
| 2026-11-16 | Trim M5: 56 data-only variants of s2 (laminar levels, caps, fixed rows, ties, `when`), s3, r4 and o1; S2's unused `LOSSES` data deleted | Each repeats a kept case's plan with other numbers | Owner (M5) |
| 2026-11-16 | Trim M6: 38 N1 cases: the minimized and slope spellings of the weighted form, the weight scales between 1e-150 and 1e150 other than the ends and 1, the solver-answer weights other than 1, and the source-value slots other than one per slot | The kept cases reach each slot and each scale's branch; coverage finds no arm only the removed ones reach | Owner (M6) |
| 2026-11-16 | Trim M8 (rest): 12 cases: `pow` and `^` squares, the function beside a reducer, three spellings of `test_strict_over_a_power` (it keeps `x ^ 2`), five `test_canonicalize_sign` and one `test_canonicalize_scale` case. Coverage put back `norm(x, 'max')`, `norm(x, 2, 3)` and the division beside a reducer, each the only test of one condition | The binder gives `pow`, `power` and `^` one function; coverage finds no arm only the other removed cases reach | Owner (M8) |
| 2026-11-16 | Trim M9: 6 HiGHS halves of `test_large_number_warning` (the small-number thresholds, the small quadratic term, the silent constraint) and of the strict bound on a fractional reducer. The warning tests keep their Gurobi halves on `decidb_cli_gurobi`; the refusal runs on `decidb_cli` | The warning reads the built model before either backend does (`ModelHoldsNumberOutOfRange`), and the refusal comes before any backend. The warning tests stay on a forced backend because a forced backend keeps direct solve off: on `decidb_cli`, A1 answers them without building a model, so nothing warns and the silent cases pass vacuously (the after-trim coverage run caught this) | Owner (M9) |
| 2026-11-16 | Unexpected difference: handoff A7 defect 2 (`python -m fuzz bounds --seed 195`) is fixed on the merge. Under `require` the merge answers 0; the prepared `Sami-OPA` binary still raises the false "takes more than one value" | Mainline's direct front end, which the merge keeps, reads the `=` bound per instance. Section 8.8 expected it unchanged | Merger |
| 2026-11-16 | Section 8.8 re-run on the merge: defect 1 stays fixed (its three queries are infeasible; `nests --seed 669` ok), defect 3 unchanged (the NULL-key row is still capped), defect 4 right (D5's `-5 <= -4` holds), limitation 10 unchanged; defect 5 has no reproducer and was not run | As expected, except defect 2 (above) | Merger |
| 2026-11-16 | Open: A-1 deleted `test_error_binder::test_objective_shape[sum(per k: max() by (k))]`, the only test of `decide_binder.cpp:1330`'s false side (`func.children.size() == 1`), the guard that keeps a reducer with no argument from reading `children[0]`. The merge answers it with "SUM expression must reference at least one DECIDE variable" | Re-adding the case with the merge's message would re-record an owner ruling, so it waits for the owner | Owner to decide |
| 2026-11-16 | Section 7's third search (`decide_sense\|source_fragments`) returns 23 lines, not none | They are mainline's own 23 lines, byte for byte: the parser's and binder's `decide_sense` (`SelectNode`, `BoundSelectNode`, the objective binder) and the serializer's deleted-property marker for `source_fragments`. Mainline deleted these names from the operator, and no hit is on `LogicalDecide` or `PhysicalDecide`. The other five searches are empty | Reviewer |
| 2026-11-16 | Format: `decide_tree_columns.cpp`, `decide_solver_gate.hpp` and `common/enums/decide.hpp` formatted with clang-format 11.0.1, one file at a time; the check then names only `logical_operator.hpp` | Whitespace only: two comments rewrapped (clang-format 11 counts an em dash as three columns) and one call's arguments put one per line | Reviewer |
| 2026-11-16 | Section 8.4: `EXPLAIN` of each of the 20 queries in `benchmark/decide/direct_solve/queries/` (R1, R4, S1–S3, A1, A2, N1, O1) on the merged binary, over TPC-H SF 0.0125, names the rule and holds no DECIDE operator. `test_direct_rule_conformance` checks the same on every rule's own fixture in each suite mode | Mainline's benchmark pins `require` on these queries, so each was direct there | Reviewer |
| 2026-11-16 | Build warnings: every warning in the merge's build logs (the first rebuilds of 277 and 229 translation units, and each later one) is a parent's: `fmt/format.h` and `httplib.hpp` literal operators (third party), `decide_profile.hpp:112` `std::uncaught_exception` and the rule-driver test's `-Wswitch` at line 138 (both mainline's lines, unchanged) | Section 2 asks for no warning new to either parent; none is | Reviewer |
| 2026-11-16 | Section 8.5, method: the merged suite captured on the final tree (14,635 queries, 8,806 passed) and compared with each parent's capture by `suite_log.py`. OPA's harness prints rows with `-json`, mainline's (kept) with `.mode quote`, so OPA's rows were rewritten in quote mode before comparing. Against mainline: 11,214 queries in both logs, 10,693 same, 2 unproven, 519 differ. Against OPA: 10,355 in both, 759 differ, 4 unproven. Each merged query that differs from one parent was checked against the other: 443 print as OPA does, 679 as mainline does, and the other 137 (19 printed by neither, 57 that only mainline's tests run, 61 that only OPA's run) were read one by one | A query that prints as one parent carries that parent's behaviour, which the merge took by a ruling or a section 10 row; the kinds below cover all 1,259 | Reviewer |
| 2026-11-16 | Section 8.5 kinds (1): R1, a determination error becomes the variant answer (361 queries) or OPA's objective message ("has more than one instance over the kept rows"); OPA's DECIDE warnings (T1); messages, plan labels and tree dumps quote the written text (`t.w * x`, not `(t.w * x)`; `count(*)`, not `count_star()`), mainline's spans; mainline's solver-gate wording, number-range wording, DIAGNOSE output, EXPLAIN JSON canonical lines and direct plans; DuckDB's error location under a data error (the b20600d087 port) | Expected kinds, each a ruling or a parent's behaviour the notes keep | Reviewer |
| 2026-11-16 | Section 8.5 kinds (2): equally optimal assignments, each test judged by its oracle and passing; read by hand: `SUM(b1 * b2)` 2 on all three, `SUM(b * x)` 200, `test_per_multi_variable` 29, `test_like_terms_over_reducers_with_different_by_keys_stay_apart` 7, the quadratic tests within 1e-4 (barrier tolerance) | A different but equally optimal assignment is an expected kind | Reviewer |
| 2026-11-16 | Section 8.5 kinds (3), EXPLAIN on the solver path: no side plan (`direct_source`, `direct_bound`, `direct_verdict` CTEs), since R2 evaluates on the tree; the DECIDE box lists OPA's accounts, so ABS and L0 link rows (`abs(x - 20) - x >= -20`) no longer print (ruling R4, still open, decides whether they should); a group bound read from data prints as written with no `↳` line, as mainline's `test_explain_group_bound_from_data_reads_as_written` requires (OPA printed its `min(...)` form); nested objectives list OPA's helper accounts (trap 13); table indexes shift by one and boxes widen | R2, R4 (open) and trap 13; the `↳` rule is mainline's test, which passes | Reviewer |
| 2026-11-16 | Section 8.5 kinds (4), OPA's behaviour kept: `norm(e, 2)` has type DOUBLE and `norm(e, 'inf')` DECIMAL(21,1) in the strict-comparison refusal (mainline DECIMAL(38,1)); on the solver path an `=` bound that varies names OPA's value pair (`9 and 14`, mainline `5 and 7`; R2); a nested objective's TOO_LARGE names the reducer, not `MINIMIZE ...` (trap 13); `DIAGNOSE` of `SUM(b * x) >= 30` (b BOOL) finds `x <= 5` -> `x <= 10`, where mainline reports no repair (OPA's Boolean-product helpers) | Each prints as the OPA parent prints it | Reviewer |
| 2026-11-16 | Section 8.5 kinds (5), neither parent's alone: the harness's exit status (mainline's `decidb_cli` return-code rule) on 7 queries whose message is unchanged; DuckDB's oids of its internal views shift (`duckdb_views()`: one below mainline's, two above OPA's); OPA's `decide_frame_limit` setting changes DuckDB's "Did you mean" for an unknown `decide_*` setting; S2's NaN-bound error names `cap` or `MAX(cap) by (div)` run to run on mainline and the merge alike (two checks, thread order; error order is not owed) | Explained; none is a defect of the merge | Reviewer |
| 2026-11-16 | Section 8.5, queries in one log only: tests deleted (each a row above) or brought by the other parent; R1 lets `test_scope_combinations::test_grid_cell` answer the 360 cells that raised the determination error, so their answer queries are new; the edited tests (T6, T7, T8, R1 rows) run their new queries | Explained by the test set | Reviewer |
| 2026-11-16 | Trap 1 check: a MIN and a MAX reducer, under MAXIMIZE and MINIMIZE, in a constraint, in the objective (`sum(agg(x) by (t.g))`) and over a range frame, 12 queries with direct solve off: every row of every answer equals the prepared `Sami-OPA` binary's | Nothing flips twice | Reviewer |
| 2026-11-16 | Trap 10, unserialized vectors: OPA's `abs_function_envelopes`, `bilinear_function_groups`, `l0_indicator_functions`, `definition_functions` and `retained_*` are filled only by optimizer passes; `Planner::VerifyPlan` serializes and reads the plan back before the optimizer runs (`planner.cpp:84`), so they need no serialization; `DECIDB_VERIFY_SERIALIZER=1 make decide-test` passes | Nothing reads them across a serialization | Reviewer |
| 2026-11-16 | Trap 11 check: each listed commit's added tests are on the merge and pass in all four modes, except `test_a2_a_maximized_negated_maximum_absolute_loss_applies_and_agrees` (7fde155940; trim M2, its eight sibling tests stay) and `test_a_by_key_that_is_a_case_is_quoted_as_written` (1cd018decc; R1 deletion); `_large_numbers.py` was deleted on mainline itself | BUG-54 (bcc45b4e84) does not apply: R1 removes the determination error | Reviewer |
| 2026-11-16 | Trap 6: the debug-only assertions were not run. `build/debug` dates from 2026-10-27, before both parents' work, and a current debug build was not made. The nested tests pass in all four modes on the release build, where `duckdb::vector` still checks every index, so an out-of-range `variable_scopes` read would throw INTERNAL | A debug build of the merge is a full rebuild | Not run |
| 2026-11-16 | Temporary adapter A: `BOUND_REDUCER_TAG` re-added to `common/enums/decide.hpp` as provenance only, stamped by `MarkReducerValue` in `decide_rewrite_rhs_bounds.cpp` (after `NameAsWritten`) and by `FrameAggregateRef` in `decide_linear_form.cpp` | Mainline deleted it once its last use was naming; OPA reads it for meaning (`DirectBoundTables::AddSignatureFields` counts a recorded reducer's value as its key alone; the tree's `carried`). Messages quote by location, never by the tag. For the rewrite: a field on the placeholder replaces the alias tag | Merger |
| 2026-11-16 | Rough, for the rewrite: the emitter's degree-2 path is carried onto `DecideProductGroup` the simplest way that passes (trap 2), not redesigned | Step 5 of the rewrite replaces the emitter | Merger |
| 2026-11-16 | Rough, for the rewrite (R2's stop-gap): bound aggregates and row checks have two implementations, OPA's tree on the solver path (`DecideDescent`, `EvaluateTree`) and mainline's side tables on the direct path (`DirectBoundTables`, `direct_row_checks.cpp`); mainline's side-table fixes (60566255a5, 5b14e8697b) hold on both because each was ported (trap 8) | R2 keeps both for the merge; the rewrite picks one | Owner (R2) |
| 2026-11-16 | Rough: in the suite's session mode an INTERNAL error invalidates the database, so the end marker never prints and the test waits out its 120 s timeout instead of failing at once | A harness defect on both parents (`decidb_cli._run_in_session`); it hid D1 as a timeout during the merge | Merger |
| 2026-11-16 | Rough: `DeriveReducerValueBounds` absorbs an overflowed reach into 1e30 instead of raising TOO_LARGE | Both parents; found while judging Q-T10 | Merger |
| 2026-11-16 | Rough: `test_error_binder[an-or-under-an-and]` does not reach the nested-OR arm its name promises; `TestRefusalsInEverySpelling::test_a_string_that_reads_as_a_tag_is_left_as_written` guards against fragment tags, which mainline removed, so nothing it guards remains | Candidates for the rewrite's test pass (Phase 4); outside the approved trims | Merger |
| 2026-11-16 | Rough: `test_anr_inlining.py` imports `At` and `dump_bounds` and uses neither | Present on OPA before the merge; no unrelated edits in the merge | Merger |
| 2026-11-16 | Checked: the trap 17 dump comparison used a sense-flipping script that negates `obj=`, `obj_constant` and `qobj`, not the `q i,j:v` entries; the corpus's only `q` entries belong to one MINIMIZE model, identical on OPA and the merge | Nothing was hidden | Reviewer |
| 2026-11-17 | Benchmarks (sections 8.4 and 8.7): `tools/ab_bench.py` puts the merge within noise of the `Sami-OPA` binary on all 19 runs, in time and peak memory; the direct-solve compare puts it within noise of mainline on all 20 queries, with the same answers. The two cells that moved by more than 10% (`a2_abs_global`'s direct CPU, `a2_abs`'s direct/hand ratio) were re-run at best of 15 and matched. The numbers are in section 4.1 | Nothing regressed | Reviewer |
| 2026-11-17 | After the merge, in its own commit: the written key order BUG-54 added (`13057cb54a`) is deleted. `FindOrCreateDecideKey` no longer computes it, `BindKey` and `FindOrCreateKeyScope` no longer pass it, and the qualified-reducer, group-key and per-key tags carry their index alone | Its only readers were the determination checks, which R1 deleted, so the order was carried and re-written but never read. The tests that read it went with R1 (above) | Merger |
| | | | |
