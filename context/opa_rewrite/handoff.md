# Sami-OPA: handoff and review

*Written 2026-11-16. Code references are at commit `7837e300be` unless stated.*

## Summary

Two branches implemented evaluation of DECIDE expressions: `Sami-ANR` and `Sami-OPA`. The owner chose **OPA**. OPA
passes its tests and gives right answers where ANR does not, but it is not finished: it follows four of the deck's
five steps, keeps the old pipeline around the new tree, and carries redundant code and tests. This document says what
is wrong, what to do about it, and how to work on the branch.

**Where the work stands (2026-11-16).**

- The branch `OPA-rewrites` was cut from mainline to hold the combined result.
- The local branch `Sami-OPA` is prepared for the merge: formatted like mainline, the node algebra deleted, and the
  wrong answer of A7 fixed. Its gate is green (merge_notes.md, section 4).
- **Next:** merge `Sami-OPA` into `OPA-rewrites`. [merge_notes.md](merge_notes.md) is the checklist.
- The rewrite described here starts once the merge is done.

Part B and the files in `review/` describe Sami's tip, `7837e300be`, as it was reviewed. Their line numbers predate
the formatting commit, so find code by name. Where the preparation changed something they describe, Part A says so.

## How to read this

| Part | Sections | What it is |
|---|---|---|
| **A. Handoff** | A1–A10 | What a successor needs to finish OPA. **Start here.** |
| **B. Evidence** | B1–B16 | The ANR-versus-OPA review that Part A rests on |

Files beside this one, in `context/opa_rewrite/`:

| File or folder | Contents |
|---|---|
| [merge_notes.md](merge_notes.md) | What to check while merging `Sami-OPA` into `OPA-rewrites` |
| `review/` | Nine full review reports, with the `file:line` evidence this document summarises |
| `repro/` | Reproducing queries for known defects, and the deck's running example |
| `tools/` | Benchmark and thread-test scripts, with baseline results in `tools/results/` |

Each finding in Part B carries a label saying how far to trust it:

| Label | Meaning |
|---|---|
| **[measured]** | The reviewer ran it |
| **[read]** | The reviewer read it in the code |
| **[agent]** | A review agent reported it; not re-checked |
| **[agent ✓]** | An agent reported it and the reviewer re-checked it |

## Terms

| Term | Meaning |
|---|---|
| **The deck** | The language proposal, `context/New_syntax.pdf`. Pages 77–119 give the one-pass algorithm |
| **E, G, O nodes** | The deck's tree nodes: E is a scoped expression (`when`, `per`), G a reducer (`sum`, `max`, with `by`), O a frame |
| **F node** | A fourth node kind OPA added for ABS, Boolean products, L0 and definitions. Not in the deck |
| **Step 5** | The deck's last step: write the solver model at the tree nodes, bottom-up |
| **Helper variable** | A solver variable standing for a nested reducer's value, e.g. `x12 = max(x1, x2)` |
| **Flat** | ANR's evaluator, deleted on OPA. OPA's comments still refer to it |
| **Stage 05 / 06 / 08** | The optimizer's rewrites / building the solver model / physical execution |
| **Direct solve** | Mainline's rules that answer some queries with plain SQL and no solver |
| **Mainline** | The branch `direct-solve-prototype` |
| **Oracle** | The independent Python solver the tests compare answers with |
| **Goldens** | Stored solver-model dumps under `test/decide/golden/` |

---

# Part A: Handoff

## A1. Decision and goal

- **Decision (owner, 2026-11-16):** continue with `Sami-OPA`. `Sami-ANR` is dropped.
- **Goal:** the deck's one-pass algorithm, all five steps.
- **Departing from the deck** is allowed only for a clearly better design, which the owner presents to his supervisor
  first.
- **We finish it ourselves.** The documentation on the branch is not trusted. Judge from code.

## A2. Where things are

**Branches and commits**

| What | Where |
|---|---|
| Working branch | `OPA-rewrites`, cut from mainline at `7865506c62`. The merge and the rewrite happen here |
| OPA, as Sami left it | `origin/Sami-OPA`, tip `7837e300be` (Nov 15) |
| OPA, prepared for the merge | local branch `Sami-OPA`, tip `f56845f560`: Sami's tip plus three preparation commits (format `9e3b0f9c82`, node algebra deleted `d88e1fe66d`, wrong answer fixed `f56845f560`). Details in [merge_notes.md](merge_notes.md), section 4. Not pushed |
| OPA before the Nov 14–15 work | `36f12dd089` (Nov 13) |
| Dropped branch | `origin/Sami-ANR`, `300e387bb1`. It is an ancestor of OPA |
| Mainline | `direct-solve-prototype`, `7865506c62` |
| Common fork point | `cec0e42f99` (Nov 2). Mainline is 404 commits past it; OPA is 224 |

**Files**

| What | Where |
|---|---|
| The deck | `context/New_syntax.pdf` in the main checkout. 119 pages, dated 2026-09-22. **Not in git** |
| Deck pages 77–119 as text | `.claude/worktrees/sami_review_files/deck/deck_pages_77_119.txt`. Local only, not in git |
| Language reference | `context/docs/language/guide.md` on `Sami-OPA`. Where it and the binary disagree, trust the binary |
| Project rules | `.claude/CLAUDE.md`. They bind all work here |
| Built worktrees | `.claude/worktrees/sami-opa` (on `Sami-OPA`), `sami-opa-nov13`, `sami-anr`. About 5 GB each. Local only |
| Raw review output | `.claude/worktrees/sami_review_files/`: fuzz logs, the suite-output comparison, a large set of test queries. Local only; all can be regenerated |

Everything under `.claude/worktrees/` is git-ignored and exists only on the machine where the review ran.

**Read first:** `.claude/CLAUDE.md`, then the deck pages 77–119, then in `review/`:
`02_deck_fidelity.md`, `01_architecture.md` and `03_execution_model_and_merge.md`.

## A3. How OPA works today

A DECIDE query on the solver path goes through these steps:

1. **Parse, bind, canonicalize** (stages 01–04). The binder computes each clause's signature.
   - Code: `src/planner/expression_binder/decide/`, `src/planner/decide/decide_canonicalizer.cpp:1802`
2. **Optimizer rewrites** (stage 05). They run on the bound clauses and fill side tables on `LogicalDecide`.
   **Every nested decision reducer becomes a helper variable here.**
   - Code: `src/optimizer/decide/decide_optimizer.cpp:98-157`; `AddValueHelper` in `decide_rewrite_scoped.cpp:63`
3. **The tree is built**, at physical planning, **after every rewrite**. Stage 05 then lowers each clause's terms into
   its node, and a program is compiled per node.
   - Code: `src/execution/physical_plan/plan_decide.cpp:62-73`; `src/planner/decide/decide_tree.cpp`;
     `LowerDecideTree` in `src/optimizer/decide/decide_linear_form.cpp:1466`; `decide_tree_compile.cpp:693`
4. **Rows are routed top-down** through the tree into per-node containers, one pass per thread (deck step 2).
   - Code: `Sink` in `physical_decide.cpp:409-450`; `DecideDescent::Append` in `decide_descent.cpp:1668`
5. **Threads are merged**, then nodes are computed **bottom-up** (deck step 3, data values only) and frames are built
   (deck step 4).
   - Code: `MergeAll` in `decide_descent.cpp:2181`; `decide_tree_columns.cpp`
6. **The emitter converts node results back to per-row columns** for the old model-building code.
   - Code: `decide_model_emitter.cpp`; called from `physical_decide.cpp:831-890`
7. **The model is built** (stage 06), unchanged from before the tree. It removes the helper variables it can.
   - Code: `BuildSolverInput` in `physical_decide.cpp:1166`; `InlineReducerValueLinks` in
     `src/decidb/formulation/linearization_reducer_value.cpp:293`; `FormulateModel` in `physical_decide.cpp:1397`

**Direct solve** sits outside this. A rule may answer the query before step 4 runs. It reads its own `DirectIR`
(`src/optimizer/decide/direct/direct_rule.hpp:183`) and never the tree.

### One clause exists in six forms [read]

The deck has one form: the tree. OPA has six. Sami's tip had a seventh, the node algebra (`DecideAlgebraNode`), a
checked copy of form 3; the preparation deleted it (`d88e1fe66d`).

1. The bound expression on `LogicalDecide`. Only `EXPLAIN` and planning read it.
2. Optimizer side tables on `LogicalDecide` (helper links, composed MIN/MAX specs, frames, ABS and product metadata).
3. Stage 05's flat term records (`DecideTerm`, `DecideConstraint`, held as `DecideBody` on each node).
4. Tree nodes and their compiled programs (`DecideNode`, `DecideProgram`).
5. The model builder's input: per-row coefficient columns and group ids (`SolverInput`, `EvaluatedConstraint`).
6. `DirectIR`, for direct solve.

### Debug switches

Environment variables the binary reads:

| Variable | Prints or does |
|---|---|
| `DECIDB_DUMP_TREE`, `DECIDB_DUMP_CONTAINERS`, `DECIDB_DUMP_BODIES`, `DECIDB_DUMP_VALUES` | The tree, its containers, each node's terms, the computed values |
| `DECIDB_DUMP_STEPS` with `DECIDB_DESCENT_CHUNK=1` | Replays the deck's row-by-row animation |
| `DECIDB_DESCENT_SPLIT=n` | Merges every n rows, simulating threads on one |
| `DECIDB_DUMP_MODEL=1` | The solver model |
| `DECIDB_PROFILE=1`, `DECIDB_BENCH=1` | Timing spans and counters |

## A4. Major problems

Problems 1–3 are the owner's. Problems 4–9 are the reviewer's additions.

### 1. The tree is not the central representation

- It is rebuilt from the plan after the optimizer has already flattened nested reducers.
- It sits beside five other forms of the same clause (A3).
- Its results are converted back to per-row columns. That converter is 23% of the tree evaluator's code.
- Evidence: B3, B7; `review/01`, `review/02`.

### 2. Step 5 is not built

- Nested decision reducers become helper variables at plan time. The model builder later removes the ones it can.
- A same-day attempt (`a4263883ff`) composed values at the nodes *beside* the model builder. It changed no model and
  was reverted. Step 5 has to **replace** the helper logic, not sit next to it.
- Evidence: B3, B7; `review/04`.

### 3. Redundant tests

- OPA added 141 test functions. Only 2 use the independent oracle.
- 75 compare with stored output of the deleted evaluator, or with the engine itself.
- 41 pin internal dump formats.
- Evidence: B9; `review/06`. What to keep and cut: A9.

### 4. The tip returns a wrong answer

**Fixed in preparation** (`f56845f560`). Kept here because the cause is a pattern the rewrite must remove.

- A nested-reducer query that is infeasible got an answer. ANR and OPA at Nov 13 are both correct.
- **Introduced by** `dcf1915038` (Nov 14), found by bisecting. That commit widened a path `017b1dfadf` had added
  for quadratic rows only. Whether the narrow form was already wrong for a quadratic nest was not tested.
- **Cause.** The canonicalizer decides each clause's shape once: one row per group (aggregate shape), or a helper
  decision per reducer (VALUE shape). The pass that rewrites a clause's bound runs next and treats the two shapes
  differently: for the aggregate shape it reduces a bound that varies inside a group to its tightest value. A later
  pass, `RewriteScopedReducers`, then kept an outer `sum` over a nest inside its clause, which turns a VALUE-shape
  clause into the aggregate shape **after** the bound's rewrite has run. The single row took the first row's bound.
- **Fix.** The outer sum is kept only where the bound is one value for the whole query, where both shapes are the
  same clause. Otherwise the clause takes the value-link path, as it did at Nov 13.
- **What the fix costs.** A product or square over a nest, under a bound that reads the rows, is now refused by name.
  It answered correctly at Nov 13 by a path later commits removed (A7, item 10).
- **For the rewrite.** A rewrite must not change a clause's shape after passes that depend on the shape have run.
  With the tree built before the rewrites (Phase 2), the shape is a property of the node and this cannot happen.
- Evidence: B4; `merge_notes.md` section 4. Reproducer: A7 item 1.

### 5. The thread merge is unverified

- Two invariants in `DecideDescent::Merge` are checked only by debug assertions, which vanish in a release build.
  One could give a wrong model, the other could crash.
- No test in the suite runs the tree on more than one real thread.
- The tip's own commit message reports 23 failures under `DECIDB_DESCENT_SPLIT=1`. Nobody has triaged them.
- Evidence: B12; `review/03`.

### 6. Error order has no specification

- Which error a query reports is defined as "what flat did", through a case list per node kind
  (`ErrorRanks`, `decide_tree.cpp:1277`).
- Flat is deleted, so the rule exists nowhere. Every new construct must invent a rank.
- Evidence: B7, B10.

### 7. OPA is far behind mainline, on foundations mainline replaced

- Mainline replaced the old degree-2 term structures with one product form, and made every objective a minimization.
- OPA still has 150 references to the old structures, 82 of them in tree-only files.
- Evidence: B11; `review/05`.

### 8. Direct solve is a second world

- It has its own IR, and runs row checks and bound side tables as plan operators.
- OPA moved the solver path's copies of those into the tree, so one mechanism became two.
- "The tree is the only representation" needs a ruling: does direct solve read the tree too?
- Evidence: B11. Decision: A6 item 2.

### 9. The deck does not cover most constructs

- It specifies SUM and MAX nests and frames only.
- ABS, products, L0, `<>`, AVG, norms, composed MIN/MAX and quadratic terms have no rule in the deck for writing the
  model at a node. OPA's F nodes only mirror the plan-time rewrites.
- Step 5 for these is new design, so each is a departure to present.
- Evidence: B3, B8.

### Smaller issues to fix along the way

- `EXPLAIN` lost the rows a rewrite adds, with no recorded decision (B8).
- Test hooks in production code: `getenv` on every chunk in `Sink`; `DECIDB_EVALUATOR_INJECT`; `DECIDB_TREE_INJECT`.
- About 1,700 lines of dead or duplicated code (B10). The largest part, the node algebra, is deleted; `review/07`
  lists the rest.
- `DecideNode` is one struct with 66 members. `DecideTree::Validate` has cyclomatic complexity 347.
- About 118 comment lines, and the branch's `context/docs`, describe behaviour by reference to flat.
- The parallel design brings no speed gain, and uses up to 28% more memory than ANR (B6).

## A5. Work plan

**Order (owner, 2026-11-16):** a short preparation on `Sami-OPA`, then the merge, then the rewrite on the merged
branch. The preparation is done. Every phase ends with the gate in A8.

### Phase 0. Stabilise the tip

Small work, no design decisions. Items 1 and 2 were done on `Sami-OPA` before the merge. Items 3 to 5 touch files
that do not gain from being done first, and are done on the merged branch.

1. **Fix the wrong answer.** ✅ Done, `f56845f560` (A4, problem 4).
2. **Delete the node algebra.** ✅ Done, `d88e1fe66d`. Goldens are byte-identical on both solvers. Composed MIN/MAX
   term specs, once parsed from the algebra's strings, are now `DecideComposedSpec` records beside each term's G
   node.
3. **Remove residue.** The list is in `review/07_dead_code_opa.md`, its sections 1 to 3. Put the debug hooks behind
   one setting that is read once.
4. **Settle the merge.**
   - Enforce or disprove invariants I5 and I6 (`review/03`).
   - Add tests on real threads. `tools/threads_test.py` is a starting point.
   - Triage the 23 `DECIDB_DESCENT_SPLIT=1` failures.
   - Release `gstate.descent` after `EvaluateTree`; nothing reads it afterwards.
5. **`EXPLAIN`.** Apply the owner's ruling (A6 item 4) and fix the four weakened tests to match.

### Phase 1. Join mainline

- ✅ OPA's tip is formatted like mainline (`9e3b0f9c82`).
- Do **one three-way merge** of `Sami-OPA` into `OPA-rewrites`. Do not rebase. Follow
  [merge_notes.md](merge_notes.md).
- Regenerate the grammar with `make grammar-build`.
- Traps, all detailed in `review/05`:
  - **Objective sense.** OPA reads `decide_sense`; mainline renamed it `written_sense` and already folded the flip
    into term signs. A plain rename flips twice, with no error.
  - **Serialization ids 221–223 collide** with mainline's new fields.
  - `DirectAggregateOf` changed signature on OPA; mainline has new callers.
  - Mainline's ABS value check would be dropped on OPA's solver path.
  - About eight behavioural mainline commits to `physical_decide.cpp` must be redone in the tree.

### Phase 2. Make the tree the representation (problem 1)

- Build the tree from the bound, canonical clauses, **before** the helper rewrite.
- Turn rewrites into transformations of the tree, or rules at a node.
- Keep one record of a clause body per node. Delete forms 2 and 3 of A3 as their readers move.
- Give error order its own written rule (A6 item 3).
- Split `DecideNode` by kind, as DuckDB's `Expression` does.

### Phase 3. Build step 5 (problem 2)

- Write the model at the nodes, bottom-up: a SUM passes its members up; a MAX allocates one variable per group that
  has more than one member.
- Delete the plan-time helper allocation, `InlineReducerValueLinks`, `PlaceHelpers`, the helper mappings, and the
  emitter's per-row scatter.
- Constraints the design must meet (`review/01`):
  - The helper-inlining rule (D89) and implied-bound propagation need every reader of a helper, which one node does
    not see.
  - `FormulateModel` must stay re-runnable for `DIAGNOSE` without reading data, so writing at the nodes must not bake
    in Big-M constants.
  - Variables are then allocated at run time, which changes naming in `EXPLAIN` and `DIAGNOSE`.
- During the change, run the old and new model building side by side and compare the solver models.

### Phase 4. Prune and add tests (problem 3)

Done alongside each phase. See A9.

### Phase 5. Rewrite the docs

Rewrite `context/docs` for stages 05, 06 and 08 in the present tense, with no reference to flat.

### Why Phase 1 comes before Phases 2 and 3

Phases 2 and 3 rewrite exactly the code that conflicts with mainline. They should be written against mainline's
product form, not against structures mainline has deleted. The cost is resolving some conflicts in code that will
then be replaced. Deleting the algebra first (Phase 0, done) reduced that cost.

## A6. Open decisions for the owner

1. **Order of work.** ✅ Decided (owner, 2026-11-16): join mainline before Phases 2 and 3, after a short
   preparation on `Sami-OPA`.
2. **Direct solve and the tree.** Does direct solve read the tree, or keep its own IR? And on the solver path, do row
   checks and bound side tables live in the plan (mainline) or in the tree (OPA)?
   - *Recommendation:* one mechanism on both paths. Which one needs its own study.
   - *For the merge only* (owner, 2026-11-16): the solver path keeps OPA's tree evaluation, with mainline's fixes
     ported into it. The lasting choice is still open.
3. **Error order.** Write down the inherited order as the rule, or adopt a simple one (clause order, then row) and
   let about 1,300 pinned error pairs change?
   - *Recommendation:* a simple written rule. It removes a case list.
4. **`EXPLAIN`.** Restore the rows an ABS or L0 rewrite adds, which the guide's section 9 promises, or approve OPA's
   shorter text?
   - *Recommendation:* restore, until step 5 changes what there is to print.
5. **Constructs the deck omits** (problem 9). What is the node rule for each?
   - *Recommendation:* design them, then present them as departures.
6. **Variant signatures.** ✅ Decided (owner, 2026-11-16): variants (D70) replace mainline's "determination" error.
   About ten mainline test files expect the error; the merge deletes or rewrites them.
7. **`per g: x <= 2` on a NULL key.** It constrains the NULL-key row, against the guide. Wrong on OPA, ANR and
   mainline.
   - *Recommendation:* fix the code. The guide's rule is the consistent one.
8. **"Constrains nothing" warning.** Should a clause that has classes but produces no model row warn? OPA is silent
   in one case where ANR warned.
   - *Recommendation:* warn.
9. **The `minmax_values` branch** in `InlineReducerValueLinks`. It changes the inlining policy and has no test.
   - *Recommendation:* drop it if the goldens allow. Step 5 replaces the function anyway.

## A7. Known defects

Reproducers are in `repro/`. Fuzz commands run from `test/decide/`.

**Confirmed**

| # | Defect | Where it occurs | Reproduce |
|---|---|---|---|
| 1 | **Fixed in `f56845f560`.** An outer `sum` over a nest, under a bound that reads the rows (`... >= t.cap - 1`), under `per row:` or `per ():`, kept one row's bound and answered an infeasible query | `7837e300be` only | `opa_wrong_answer_nested_per_row.sql`; `python -m fuzz nests --seed 669 --count 1`. Guarded by `test_a_bound_that_reads_the_rows_holds_on_each_under_a_whole_table_nest` |
| 2 | False "takes more than one value" on the direct-solve path; the solver path answers | OPA tip, OPA Nov 13 | `python -m fuzz bounds --seed 195 --count 1` |
| 3 | `per g: x <= 2` constrains the row whose `g` is NULL | OPA, ANR, mainline | `null_key_and_anr_defects.sql`, query S1 |
| 4 | An answer violates its constraint: Boolean product over a signed decision | **Mainline** and ANR. OPA fixed it in `e19ed66d5a` | `null_key_and_anr_defects.sql`, query D5 |
| 5 | An AVG whose coefficients sum to 6.999999999999999 is reported infeasible | recorded as D3 in `test/decide/fuzz/known.py`; no seed | none |

**Suspected, not reproduced**

| # | Defect | Source |
|---|---|---|
| 6 | Merge matches decision classes by a key holding thread-local ids. Possible wrong model | `review/03`, invariant I5 |
| 7 | Merge indexes out of bounds for a per-class F node under two reducers with different `by` keys | `review/03`, invariant I6 |
| 8 | A collation error in a frame's order key is blamed on the wrong row | `decide_descent.cpp:1361`; `review/01` |
| 9 | A "constrains nothing" warning is lost in one NULL-key shape | `review/09`, item D9b |

Items 6 and 7 did not show at 1 and 8 threads over 500,000 rows (B4). That does not clear them.

**Known limitation, introduced by the fix of item 1**

| # | Limitation | Where | Reproduce |
|---|---|---|---|
| 10 | `sum((x - 1) * max(x) by (t.h)) >= t.cap - 1`: a product or square over a nested reducer, under a bound that reads the rows, is refused ("squares or multiplies decisions inside nested reducers, which is not supported yet"). With a constant bound it answers | Prepared `Sami-OPA`. Answered correctly at `36f12dd089`; answered wrongly at `7837e300be` | `refused_quadratic_nest_row_bound.sql` |

More oddities common to every branch: `review/09`, items S1 to S7.

## A8. Build, test, measure

Run from the root of the checkout being worked on.

**Build and test**

```bash
make release                                  # build, about 5 minutes with ccache
make decide-setup                             # create the Python test environment
./test/decide/run_tests.sh -k <pattern>       # focused tests; always pass -k or -m
make decide-test                              # goldens, then the whole suite; about 1 minute
build/release/test/unittest "[decidb]"        # C++ tests
```

On the tip, `make decide-test` passes 10,405 tests and the C++ run passes 85 cases.

**Extra runs for particular changes**

```bash
DECIDB_FORCE_SOLVER=highs make decide-test        # when model building changes
DECIDB_VERIFY_SERIALIZER=1 make decide-test       # when serialization changes
```

**Fuzzers** (about 3 minutes for 2,000 seeds)

```bash
cd test/decide && .venv/bin/python3 -m fuzz all --seed 1 --count 2000 --jobs 8
cd test/decide && .venv/bin/python3 -m fuzz all --seed 1 --count 400 --jobs 8 --solver highs
```

**Compare every suite query's output between two builds**

```bash
test/decide/capture_suite_log.sh capture --modes auto --out DIR
test/decide/suite_log.py OLD_DIR NEW_DIR --limit 400
```

### The gate for every change

- [ ] Suite and C++ tests pass.
- [ ] Goldens are identical, unless the change is meant to move them.
- [ ] Fuzzers show nothing new at 2,000 seeds.
- [ ] The real-thread test passes.
- [ ] No benchmark moves outside noise. Baseline numbers: B6.

### Environment traps [measured]

- **Gurobi package version.** A fresh test environment installs `gurobipy` 14.0, which the installed 13.0 license
  refuses. Fix: `test/decide/.venv/bin/python3 -m pip install "gurobipy==13.0.3"`. `requirements.txt` is not pinned.
- **Test database.** Tests need `decidb.db`. Copy `build/decidb.db` from the main checkout into the new checkout's
  `build/`.
- **Oracle cache** is per checkout: `test/decide/results/oracle_cache.json`.
- **Another binary.** Fuzzers and tests take one through `DECIDB_EXE_PATH`.

### Measuring

- `tools/ab_bench.py` runs the repository's 12 scaling workloads on several binaries, with runs interleaved. It
  reports whole-process time minus solver time.
- Do not use `benchmark/decide/anr_scaling.py` for comparisons. Its timer leaves out the row pass in `Sink`.
- `tools/threads_test.py` runs eight query shapes at 1 and 8 threads over a stored table.
- Both scripts hold absolute paths to the review worktrees. Edit `BINARIES` at the top.

## A9. Tests: keep, cut, add

OPA's behaviour is **not** identical to ANR's. It fixed real defects, and the regression tests for those stay. The
redundancy is elsewhere.

| Tests | Size | What to do |
|---|---|---|
| ANR's oracle-based files, inherited | about 90 files | **Keep.** They survive any internal rewrite |
| Regression tests for fixed defects | about 25 functions | **Keep** |
| Stored-output tests | 72 functions, 259 stored files | **Cut** |
| Dump-format pins | 41 functions | **Cut most** |
| Merge tests that simulate threads | 4 functions | **Replace** with real threads |
| Tests weakened on OPA | 8 | **Restore or delete**, each with a reason |

Notes on each row:

- **Regression tests to keep:** `test_fuzz_defects.py`, the additions to `test_bilinear.py` and `test_anr_nested.py`,
  and the fuzz smoke seeds.
- **Stored-output tests** are `test_tree_emitter.py` with `tests/_expected.py` and `tests/expected/`. They were
  recorded from the deleted evaluator and cannot be regenerated; re-recording is circular. Move any query worth
  keeping into the golden corpus, which already guards model text.
- **Dump-format pins** are `test_tree_bodies.py`, `test_one_pass_tree.py`, `test_one_pass_literal.py` and parts of
  others. Keep the few that check the deck's own pictures: the tree of page 81 and the containers of page 91.
- **Simulated-thread tests** are in `test_descent_merge.py`. Replace them with tests over several row groups on real
  threads.
- **Weakened tests** are listed in B9.

**To add:** an oracle test for each confirmed defect in A7; real-thread tests; and for step 5, a model comparison
between old and new model building while both exist.

**The rule to apply**, from `.claude/CLAUDE.md`: each decision point gets a test that reaches it and fails if the
branch is removed. When code is deleted, the tests that only reached it go in the same change.

## A10. What this review did not do

Details are in B15. In short:

- No bisect of the wrong answer.
- The two merge suspicions were neither reproduced nor disproved.
- Only the default suite mode was run.
- No TPC-H benchmark.
- No trial merge was built.

---

# Part B: Evidence

Branches compared: `Sami-ANR` at `300e387bb1` (Nov 8) and `Sami-OPA` at `7837e300be` (Nov 15). Both fork from
`direct-solve-prototype` at `cec0e42f99` (Nov 2). OPA is ANR plus 140 commits.

## B1. Why OPA over ANR

| Criterion | Result | Favors |
|---|---|---|
| Path to the deck's step 5 | OPA has the tree, row routing, containers and frames at run time. ANR has none; step 5 there means building them first and replacing about 3,000 lines (B3, B7) | **OPA**, decisively |
| Correctness | ANR: wrong answers and INTERNAL errors in six user-visible classes, 284 failing fuzz cases. OPA: 6 failing fuzz cases, one of them a new wrong answer (B4, B13) | **OPA** |
| Produces the deck's models | Both, byte-identical on the deck's example (B3) | tie |
| Suites | Both pass. 14,872 of 15,216 shared suite queries print identical output (B4) | tie |
| Performance | Within about 15% on most workloads; OPA up to 1.4x slower on the simplest, up to 28% more memory (B6) | ANR, slightly |
| Code size and clarity | OPA's evaluator is 52% larger and holds each clause several times (B5, B7, B10) | ANR |
| Cost to join mainline | OPA about twice ANR: est. 14–22 days against 6–10 (B11) | ANR |
| Tests of its own evaluator | 82% of OPA's new tests compare stored output or internal dumps (B9) | ANR |

The first row settles it. ANR's evaluator has no structure for step 5 to attach to, so choosing ANR would postpone
OPA's costs, not avoid them. If step 5 were dropped, ANR would be the better operator to keep.

**Why the tip and not Nov 13.** OPA's last 61 commits are mixed:

| | In the Nov 14–15 work |
|---|---|
| Worth keeping | ABS over a reducer builds one auxiliary per group, not per row (B6). A lost floor on a Boolean product is fixed, which mainline still gets wrong (B13). The no-value rule became one class |
| Regression | The wrong answer of A7 item 1 |
| Redundant | The node algebra, about 900–1,300 lines; three independent reviews agree it is a checked copy (B8, B10) |
| Undecided | `EXPLAIN` stopped printing rewrite rows; four tests were changed to match (B8) |

The fixes are built on the same changes as the redundant parts, so removing the bad parts from the tip is a smaller
job than redoing the fixes on Nov 13.

**Confidence.** High on the direction: eight independent reviews and the reviewer's measurements agree on the first
two rows. Medium on the effort estimates in B11, which are an agent's judgement.

## B2. What each branch is

| | ANR ("flat") | OPA ("tree") |
|---|---|---|
| Language work (nested reducers, frames, variants) | yes, 84 commits | same commits, inherited |
| Evaluator | one scan fills a row store, a column per distinct expression; later phases read it | a tree of E/G/O/F nodes; rows routed into node containers per thread, merged, computed bottom-up |
| Other evaluator present | scaffold for a second one (`DECIDB_EVALUATOR`, verify harness), not built | none; flat deleted |

## B3. Against the deck (pp. 77–119) [read]

| Deck step | ANR | OPA |
|---|---|---|
| 1. Annotated E/G/O tree | no tree at run time | built, but derived after the optimizer has flattened nested reducers (`plan_decide.cpp:63`) |
| 2. Top-down row routing | one scan into columns, not per node | built, per thread |
| 3. Bottom-up evaluation | phases read the store | built for data values; decision values do not flow up as expressions |
| 4. Frames | built | built |
| 5. Model written at the nodes | **not built** | **not built** |

Step 5 in both: every nested decision reducer becomes a helper variable at plan time (`AddValueHelper`,
`decide_rewrite_scoped.cpp`); stage 06 inlines the ones it can (`InlineReducerValueLinks`). Introduced in ANR, kept
by OPA. Owner decision: step 5 is built later on the winner.

### Fidelity audit [agent, except where marked]

**Both binaries build the deck's models.** On the deck's running example the emitted models are byte-identical
between ANR and OPA [agent ✓] and equal pp. 115 (MAX in SUM: helpers only for the two groups with more than one
member), 119 (SUM in SUM: no helper survives) and 108 (frame). So the "same models" gate passes for both.

**OPA is far closer to the deck on steps 1–4.** Its tree for the example matches p. 81 node for node, its containers
match p. 91, and `DECIDB_DUMP_STEPS` at chunk size 1 replays the deck's pp. 84–91 animation.

OPA's departures from the deck:

| Size | Departure |
|---|---|
| large | The tree is rebuilt from the plan *after* optimizer rewrites, so the operator nodes (`≤ + ×`) are gone and node bodies are stage 05's lowered terms |
| large | A G over decisions holds a plan-time helper variable, not an expression (this is the missing step 5, and it also empties step 3 for decision values) |
| large | The emitter scatters node results back into input-sized per-row columns for the old formulation code |
| medium | Every node carries error-rank metadata whose only job is to raise errors in the deleted evaluator's order |
| small | Frame selection reuses the old input-sized frame map; a G also keeps rows its operand's `when` rejects; F nodes are not in the deck |

ANR against the deck: no tree, no routing (every expression is evaluated on every row, masked afterwards), no
containers (only per-row id columns). Bound data reducers are computed as SQL side tables before the operator,
which reads the source twice.

To build step 5 the deck's way:
- **On OPA:** build the tree before the helper rewrite and before stage 05 lowering; replace the emitter adapter with
  row emission at the nodes; fold the F nodes' plan-time auxiliaries in. The descent, containers, signatures, folds
  and frame containers stay.
- **On ANR:** build all of OPA's steps 1–4 first.

## B4. Correctness [measured]

| Check | ANR | OPA |
|---|---|---|
| Release build | clean | clean |
| C++ tests `[decidb]` | pass, 84 cases | pass, 85 cases |
| Golden model baselines (Gurobi, HiGHS) | identical | identical |
| Python suite (`make decide-test`) | **9,907 passed**, 57 s | **10,405 passed**, 53 s |
| Same query at 1 and 8 threads, 500k stored rows, 8 shapes | same objective | same objective, no crash |

### Suite output compared query by query [measured]

Of 15,216 queries that both suites run, **14,872 print identical output** and 28 differ:

- 16 are `EXPLAIN` text. OPA omits rewrite rows and plans no side-plan CTE.
- 9 are a different assignment on a query with several optima. The tests are oracle-judged and pass on both.
- 2 are numeric noise in a quadratic case.
- 1 is a `DIAGNOSE` result where OPA gives a repair and ANR says "undiagnosed".

A further 17 queries exist only on ANR (14 from its verify-harness test) and 294 only on OPA.

### Real threads [measured]

Eight query shapes over 500,000 stored rows, once at 1 thread and twice at 8 threads, on both binaries:

- The shapes: flat sum, MAX in SUM, ABS per class, keyed decision, point frame, range frame, and the two the
  execution review suspected (ABS of a keyed decision inside a class; a per-class ABS under two reducers with
  different `by` keys).
- Every shape gives the same objective at 1 and 8 threads, and the same value on ANR and OPA. No crash.
- OPA's profile confirms a real multi-part merge at 8 threads: 3.7 ms, against 0.01 ms at 1 thread.
- This does not disprove the two suspected merge defects (B12). It shows these queries do not trigger them.

### Fuzzing [measured]

Seeds 1–2000 per module under Gurobi (about 40,000 cases per run), seeds 1–400 under HiGHS. Each failing seed counts
4 or 8 cases, one per table spelling and direct-solve mode.

| Run | Wrong answer | INTERNAL error | Wrongly infeasible / error |
|---|---:|---:|---:|
| OPA's fuzzers on **OPA**, Gurobi | **4** (1 seed) | 0 | 2 (1 seed, direct path only) |
| OPA's fuzzers on **ANR**, Gurobi | 48 | 132 | 104 |
| ANR's own fuzzers on ANR, Gurobi | 4 (known D1) | 116 (known D4) | 0 |
| OPA's fuzzers on OPA, HiGHS | 0 | 0 | 0 |
| OPA's fuzzers on ANR, HiGHS | 0 | 20 | 32 |

OPA's fuzzers are stricter than ANR's: OPA lifted exclusions as it fixed defects, and encoded a ruling on `=` bounds
that ANR predates (most of ANR's 104).

**ANR's defects are real** [measured]: D1 and D2 are wrong answers (NULL `by` key under a bare MIN/MAX; `<>` with a
NULL bound drops a group); D4 is an INTERNAL error that invalidates the database (a range frame over a keyed
decision with no instance). OPA's fixes are 35–65 lines each, about half in tree-only files, so keeping ANR means
redoing them in flat.

**OPA has a wrong answer that ANR does not** [measured] (nests seed 669; not in OPA's `known.py`). Reduced by hand:

```sql
SELECT id, x FROM (VALUES (1,'q',3,7),(2,'p',6,12),(3,'p',5,5)) t(id,h,wv,cap)
DECIDE x(INT)
SUCH THAT x <= 3 AND per row: sum(per (): max(x) by (t.h, t.wv)) >= t.cap - 1
MINIMIZE sum(x);
```

Row 2 needs the sum to be at least 11, and `x <= 3` over three rows gives at most 9, so the query is infeasible. ANR
says infeasible. OPA returns x = 0, 3, 3: it dropped row 2's constraint. The same happens with `sum` in place of
`max`. Without `per row:` on the clause OPA is correct.

**The Nov 14–15 work introduced it** [measured]: OPA built at `36f12dd089` (Nov 13) answers "infeasible" on all
three variants. The exact commit is not bisected; the suspect is the special path for nested sums
(`has_nested_outer`, commits `017b1dfadf` and `dcf1915038`) that B8 flagged as untested.

**OPA at Nov 13 is the cleanest of the three on the fuzzers** [measured]: with the tip's fuzzers, seeds 1–2000, it
has no wrong answer and no INTERNAL error in any module. Its only failure is the same direct-path seed (bounds 195)
the tip has.

OPA's other failing seed (bounds 195) raises "takes more than one value" on the direct-solve path only, where the
solver path answers. ANR raises it on both paths.

## B5. Size, complexity, tests, merge cost [measured]

| Check | ANR | OPA | Reading |
|---|---|---|---|
| Evaluator + formulation code (lizard NLOC) | 11.6k, 336 functions | 17.5k, 596 functions | OPA +52% |
| All DECIDE source lines | 54k | 63k | OPA +9k |
| Worst function | `EvaluateConstraints`: 654 lines, CCN 180 | `DecideTree::Validate`: 523 lines, CCN 347 | one monster each |
| Functions with CCN ≥ 30 | 36 | 44 | similar |
| `InlineReducerValueLinks` (shared, stage 06) | CCN 81 | CCN 104 | OPA grew it |
| Conflicted files merging with `direct-solve-prototype` | 106 | 115 | raw count misleads; see B11 (OPA about 2x) |
| Test files using the independent oracle | 90 of 139 | 90 of 147 | tie |
| Test functions compared with stored output of the deleted evaluator | 0 | 72 (1 file) | see B9 |
| Skips / expected failures added | 0 | 0 | tie |
| Comment lines defining behaviour as "what flat does" | n/a | about 118 | flat no longer exists there |

## B6. Performance [measured]

**Performance does not separate the branches.** OPA is within about 15% of ANR on most workloads, up to 1.4x slower
on the simplest one, and uses 0–28% more memory. OPA's parallel row pass does not make it faster than ANR.

Method: the repository's 12 scaling workloads (LPs, so the solve is cheap), the reviewer's harness, runs interleaved across
binaries, medians. Time is whole-process wall time minus solver time, in ms. The repository's own script times only
"optimizer + model construction", which leaves out OPA's row pass in `Sink`. Each figure includes about 25 ms of
process start. Apple M1 Pro, 11 cores, Gurobi 13.0.1.

| Workload | 10^5 rows: ANR | OPA | ratio | 10^6 rows: ANR | OPA | ratio | Peak memory ratio at 10^6 |
|---|---:|---:|---:|---:|---:|---:|---:|
| flat-sum | 36 | 41 | 1.16 | 140 | 196 | **1.40** | 1.19 |
| nested-1 | 71 | 60 | **0.85** | | | | |
| nested-2 | 144 | 145 | 1.01 | 1,321 | 1,273 | 0.96 | 1.10 |
| nested-3 | 1,301 | 1,313 | 1.01 | | | | 1.29 at 10^5 |
| variants-10 | 51 | 53 | 1.05 | 244 | 283 | 1.16 | 1.28 |
| point frame | 48 | 51 | 1.06 | 272 | 283 | 1.04 | 1.08 |
| range, width 1 | 60 | 69 | 1.15 | | | | |
| range, width 5 | 81 | 91 | 1.13 | 689 | 749 | 1.09 | 1.07 |
| range, width 50 | 341 | 355 | 1.04 | 3,614 | 3,871 | 1.07 | 1.00 |
| range, step 7 | 93 | 106 | 1.14 | | | | |
| range max | 149 | 154 | 1.03 | | | | |
| cyclic | 72 | 79 | 1.10 | | | | |

Five runs at 10^5, three at 10^6. Differences under about 10% are inside run-to-run noise at these sizes.

Stored table of 10^6 rows (so the scan can be parallel):

| Workload | Threads | ANR | OPA | ratio |
|---|---|---:|---:|---:|
| flat-sum | 1 | 149 | 208 | 1.39 |
| flat-sum | default (11) | 154 | 183 | 1.19 |
| nested-2 | 1 | 1,242 | 1,233 | 0.99 |
| nested-2 | default | 1,253 | 1,307 | 1.04 |
| point frame | default | 303 | 350 | 1.15 |

Threads help OPA's flat-sum by 12% and do nothing for ANR, as the designs predict, but OPA's serial merge and
emitter keep it behind.

Both grow linearly in rows on every workload measured. `nested-3` is the exception on both: about 35x for 10x rows.

**OPA at Nov 13 against OPA's tip:** within noise on time. The Nov 14–15 work added about 24% peak memory on
`nested-3` (843 → 1,041 MB at 10^5).

**One large difference that is model size, not evaluator speed.** For `abs(sum(x) by (g)) <= 5`, ANR and OPA at
Nov 13 build one auxiliary pair per *row*; OPA's tip builds one per *group* (commit `9014a4c3d0`, shared optimizer
code):

| 20,000 rows, 10 groups | Variables | Rows | Nonzeros |
|---|---:|---:|---:|
| ANR, and OPA at Nov 13 | 40,010 | 40,010 | 100,010 |
| OPA tip | 20,020 | 30 | 20,050 |

At 500,000 rows the query takes 13–17 s on ANR and 1.3–1.8 s on OPA's tip. This fix is worth having on any branch.

## B7. Architecture review [agent, except where marked]

Recommendation: **keep OPA, but clean up before building step 5.** "If step 5 were not planned, ANR would be the
simpler and more conventional operator to keep."

Why OPA for step 5: only OPA has nodes with identity and per-node containers at run time. ANR would first have to
grow them, which means rebuilding OPA's steps 1–4 and replacing about 3,000 lines of `physical_decide.cpp`.
Estimate (not measured): 60–70% of OPA's evaluator survives step 5.

**ANR problems**
- `EvaluateConstraints` is one 654-line function branching on about 35 clause flags.
- `RegisterEvaluation` must list by hand every expression a later phase reads; a miss is an internal error. This is
  a growing case list.
- All expression evaluation is single-threaded in `Finalize`, and runs on rows a `when` later drops.
- About 420 lines of scaffold for an evaluator that does not exist on the branch.

**OPA problems**
- **Each clause is held three times**: stage 05's flat term records, the compiled programs, and the node algebra.
  The algebra is built *from* the flat records, then checked against them in `Validate` and again in the emitter
  (68 internal-error checks [read]). It is keyed by strings (`"+"`, `"composed_sum"`, `"minimize"`).
- **The tree is not the source of truth for the model.** The emitter converts node values back into the same
  row-indexed structures ANR produces, for the unchanged stage 06.
- **Error ordering reproduces the deleted flat evaluator** through a per-node-kind case list (`DecideErrorRank`,
  8 fields; four ranks per node).
- `DecideNode` is one struct with 66 members, many valid for one node kind only.
- Test hooks in production code [agent ✓]: `DECIDB_EVALUATOR_INJECT` in `decide_tree.cpp:1153`; two `getenv` calls
  per chunk in `Sink` (`physical_decide.cpp:316, 325`).
- Likely bug: a collation error in a frame's order key is attributed to the wrong row
  (`decide_descent.cpp:1361`). Not yet confirmed.
- A step-5 attempt was built and removed on Nov 15 (`a4263883ff`): composing at nodes *beside* stage 06 changed no
  model and removed no code. Step 5 has to *replace* stage 06's helper logic.

No data race found by reading the per-thread descent or its merge.

**Clean-up the review asks for on OPA before step 5** (all net removals)
1. One representation of a clause body (either make the algebra the source, or delete it: about 1,000 lines).
2. Give error order its own rule instead of the flat evaluator's (about 300 lines).
3. Emit per instance, not per row.
4. Remove residue: dead code, hooks, duplicated readers (about 250 lines).

## B8. OPA's last 61 commits, Nov 14–15 (`36f12dd089..7837e300be`) [agent, except where marked]

Recommendation: **if OPA is kept, start from the tip with the node algebra removed**, not from the Nov 13 commit.

The range is two efforts: 57 overnight commits with no message bodies, then 11 commits that mostly repair, revert
or clean up the first 57.

**Remove**

- **Node algebra** (`DecideAlgebraNode`, about 1,300 lines). A copy derived from the older term records. Of the
  emitter's 68 internal-error checks, about 41 compare it with those records. It is the real source for one thing
  only: composed MIN/MAX term specs, parsed from strings like `"composed_max_scaled_by"`. Deleting it changes no model.
- **`value_nodes` plumbing** in stage 06. Tree node ids carried into formulation; their only effect is a consistency
  exception.
- **`minmax_values` branch** in `InlineReducerValueLinks` (remove or decide). It changes the inlining policy for
  keyed MIN/MAX, with no test and no stated bug.

**Keep**

- **F nodes** (ABS, Boolean product, L0, definition). They mirror plan-time rewrites and formulate nothing yet, but
  the node kind, containers and merge are reusable for step 5. Cost: every term-splitter entry point must now see
  through their tags.
- **Helper contract moved onto G and O nodes.** It replaced the physical operator's duplicate copies; step 5 builds
  on it.
- **`DecideAbsence`.** Five scattered no-value checks became one class.

**Already gone**

- **Node composition** (535 lines). Added, then fully reverted the same day.

Real bug fixes in the range: a floor on a Boolean product was lost (`b * x >= -4` answered x = -5), fixed in shared
code. Two other fixes repair regressions the range itself introduced, including a **crash in the multi-thread merge
that every commit from `251540dc1f` to `169386c6b1` has**; the last commit fixes it.

Process signs: the C++ test target did not build for about 35 commits; the qualified-reducer logic was rewritten
three times with no test added.

**User-visible change accepted without a recorded decision [agent ✓]:** `EXPLAIN` no longer prints the rows an ABS
or L0 rewrite adds. Four existing tests were changed to match: `test_explain.py` (was "shows the rows it became",
now asserts they are absent), `test_scope_explain_diagnose.py` (two expectations became `None`, one inverted),
`test_tree_emitter.py:1482` (full-output equality became three substring checks). Owner call: restore or approve.

Tip commit message [read]: under `DECIDB_DESCENT_SPLIT=1` (merge after every row) 23 tests still fail, described as
large-input and step-dump tests. Not yet checked whether those are time limits or real failures.

## B9. Test quality [agent, except where marked]

Summary: OPA's suite protects the product better, because it inherits ANR's oracle tests in full and fixes four of
ANR's five known defects with regression tests. But OPA's own new tests are weak protection for its own evaluator.

OPA's 141 added test functions, by what they check:

| Kind | Count | Share |
|---|---:|---:|
| Independent oracle | 2 | 1% |
| Hand-stated answer, model or message | 23 | 16% |
| Stored output of the deleted flat evaluator, or the engine against itself | 75 | 53% |
| Internal dump formats, counters, injection hooks | 41 | 29% |

- **The stored expectations cannot be regenerated.** Re-recording at the tip records whatever the tree prints, so it
  is circular. Five stored model files have already been overwritten with tree output.
- **When step 5 lands**, 44 of these functions must be rewritten and 38 more compare exact model text that will
  probably change. ANR's ~90 oracle files are unaffected by internals.
- **No test runs the tree on more than one real thread.** Every merge test uses `SET threads = 1` and simulates
  threads by merging every n rows. The multi-thread crash fixed in the last commit was found outside the suite.
  The reviewer tested real threads directly (B4).
- ANR's dead weight if kept: `test_evaluator_verify.py` (8 tests of a harness comparing flat with itself).

Expectations weakened or flipped on OPA with no stated reason:
1. `EXPLAIN` rewrite rows: 4 tests (see B8) [agent ✓].
2. `test_one_pass.py:35`: "exactly one scan" became "greater than 0"; the docstring still claims one pass.
3. `test_direct_r1_continuous_knapsack.py:2226`: a loop over two memory limits now asserts the same outcome twice.
4. `_old_spellings.py:183`: the removed-syntax scanner got an exemption so pinned tree dumps pass it.
5. `test_scope_reducer_by.py`: a "key is computed once" test was deleted with no tree equivalent.

Fuzz regression tests exist on OPA for D1, D2, D4, D5. D3 (an AVG rounding case reported infeasible) is open on both.

## B10. Dead and duplicated code [agent]

**ANR: about 1,130 removable lines.**

| Group | Lines | Notes |
|---|---:|---|
| Evaluator-switch and verify scaffold | ~436 | Purely additive; comes out cleanly. Under `verify` it compares flat with itself |
| Dead code | ~265 | Most of it predates the branch (unused `CoefficientColumn` storage mode and members, unused direct-solve helpers) |
| Duplication inside the evaluator | ~375 | A refactor, not a deletion |
| Duplicate-fact fields | ~60 | |

ANR's duplication is the notable part. `EvaluateConstraints` (786 lines) and `EvaluateObjective` (259 lines) repeat
the same steps per term kind: five copies of the coefficient-evaluation loop, four inline copies of an existing
masking helper, the per-term `when` gathering three times on the constraint side and twice on the objective side.
`RegisterEvaluation` is a second traversal that must list every expression each clause shape reads, so a new shape
is added in two places.

Also on ANR: 50 comments cite deck pages, and several describe the code as "the annotated tree" with "top-down" and
"bottom-up" passes. The code is the flat store; that description is not accurate.

**OPA: about 1,700 removable lines** (about 2,200 if the debug dumps and the 8 test files reading them go too).

| Group | Lines | Notes |
|---|---:|---|
| Node algebra versus the term records | ~900 | The only design decision in the list. Nothing the emitter reads exists only in the algebra |
| Dead code, write-only fields, constant parameters | ~370 | Includes leftovers of the deleted evaluator's "second run" (`DecideEvaluationFields`, a `silent` flag that is always false) |
| Other facts held twice | ~200 | Clause identity in three places; the ABS link in three forms; three callback interfaces with one implementer each |
| Real duplication | ~210 | |

Three independent agents (architecture, last-61-commits, this one) reached the same conclusion about the node
algebra: it is a checked copy, and deleting it changes no model.

Behaviour specified by deleted code: about 118 comment lines define what the code must do as "what flat did". The
most load-bearing are the error-precedence rules (`DecideBlock`, the four ranks per node, `ErrorRanks`), which name
functions that no longer exist (`PhysicalDecide::Evaluate`, `EncodeSortKeys`). The code is live and tested, but its
specification exists nowhere else.

**Shared by both branches:** an unused storage mode in `CoefficientColumn` (~55 lines), `DirectCheckRowBounds`,
`GetSingleDecideVarIdx`, `ReadOnBudgetRows`, an unused `canonicalizer` built per query, and a 45-line
`ParamsToString` duplicated between the logical and physical operator.

## B11. Cost of joining `direct-solve-prototype` [agent, except where marked]

**OPA costs about twice ANR to integrate.** Estimates (judgement, not measured): ANR 6–10 focused days, OPA 14–22.

Most of the raw conflict is formatting: mainline reformatted all sources (commit `da8b8941af`). After formatting the
candidate first, real conflict in `src/` is:

| | Files | Hunks | Lines |
|---|---:|---:|---:|
| ANR | 45 | 183 | 3,884 |
| OPA | 53 | 271 | 7,269 |

Why OPA costs more:
- **Mainline rewrote what OPA rewrote.** Since the fork mainline replaced the degree-2 term structures
  (`bilinear_terms`, `quadratic_groups`) with one product form (`DecideProductGroup`), and made every objective a
  minimization. Both branches still use the old structures: ANR 105 references, OPA 150, of which 82 are in OPA's
  tree-only files [agent ✓]. OPA's emitter needs its degree-2 part re-designed, not renamed.
- **Mainline's execution fixes land in code OPA deleted.** About 8 behavioural commits to `physical_decide.cpp` must
  be re-done in the tree.
- **OPA's 259 stored test snapshots go stale.** 99 hold `sense: maximize` model dumps.
- **OPA splits a shared mechanism in two.** Mainline runs row checks and bound side tables as plan operators on both
  the solver and direct paths. OPA moves them into the tree on the solver path only. After a merge the ABS value
  check would be dropped on OPA's solver path unless handled.

Traps on both branches:
- **Objective sense.** Both read `decide_sense` to choose a MIN/MAX push direction; mainline renamed it
  `written_sense` and already folded the flip into term signs. A plain rename flips twice, silently.
- **Serialization ids 221–223 collide** with mainline's new fields. Renumber and regenerate.
- **Owner decision on language:** both branches replace mainline's "determination" error with variant signatures
  (D70). About 10 mainline test files expect the error.

Advice from the review: one three-way merge of the formatted candidate into a branch cut from mainline, not a rebase.
And: if the tree is the intended end state, choosing ANR now only postpones OPA's porting cost.

## B12. Execution model and concurrency [agent]

Recommendation: **OPA has the sounder model, on condition that two merge invariants are enforced or disproved.**
"If the decision has to be made before those tests run, ANR is the lower-risk choice for correctness."

| | ANR | OPA |
|---|---|---|
| Parallel work | appending rows only | `when`, key, hash probe and store in `Sink`; the merge copy |
| Serial work | the whole evaluation, in `Finalize` | merge of maps, bottom-up pass, emitter |
| Expressions evaluated on | every row, ignoring `when` | only rows that reach the node |
| Grouping | hash table, then a binary search per row; rebuilt when a clause reads the key on more rows | slot id held in the hash table, once per thread |
| Memory peak | input + one full-length vector per distinct expression, held through the solve | input + every thread's containers + the merged copy; merged descent held to end of query for no reader |
| Closest DuckDB operator | materialize, then a serial `Finalize` | hash aggregate (thread-local tables merged in `Finalize`), without radix partitioning |
| Concurrency risk | none; nothing runs in parallel | the merge |

ANR is safe under threads because it does nothing in parallel. Its error cliff: any row that raises makes the whole
2,048-row window re-run row by row, including rows a `when` would drop.

OPA's `DecideDescent::Merge` was read line by line against 14 invariants. Twelve hold. Two are left to the planner
and checked only by debug assertions:
- **Suspected silent wrong model:** decision classes are matched across threads by a sort key that embeds
  thread-local instance ids, never rebased. Trigger: a keyed ABS/L0/product helper whose definition reads a per-row
  or differently-keyed decision, input over one row group, two or more threads.
- **Suspected crash:** a per-class F node read under two reducers with different `by` keys indexes a slot map out
  of bounds.

Neither is confirmed. The reviewer ran the agent's example shapes at 1 and 8 threads over 500k rows: no crash and identical
objectives (B4). The suspicion stands as a code-reading finding without a reproducing query.

Smaller findings on OPA: container numbering follows chunk boundaries; no interrupt checks in the merge, bottom-up
pass or emitter (ANR checks only inside frames); `getenv` on every chunk in `Sink`.

## B13. User-facing behaviour [agent, except where marked]

About 4,500 queries through both binaries (hand-written, test literals, golden corpus, generated scripts). Where
both solve a query, answers are row-for-row identical apart from one equal-objective tie. No independent oracle was
used here, so agreement is not proof of correctness.

**OPA behaves better for a user.** The agent found no OPA-only crash or wrong answer (its queries did not include
the nested-reducer shape of B4). ANR has six classes of serious failure:

| # | On ANR | OPA |
|---|---|---|
| D1 | INTERNAL error that invalidates the session: a range frame over a keyed decision with a NULL-key row | answers |
| D2 | INTERNAL error on `sum(abs(sum(abs(x))))` | answers |
| D3 | Wrong answer: `per K: sum(x) by (G) <= literal` constrains groups no class reads [agent ✓] | right (mainline is right too) |
| D4 | Wrong answer: `min(per g: x)` reads the row whose `g` is NULL | right |
| D5 | Answer violates its own constraint: BOOL product over a signed decision [agent ✓] | right (mainline is **wrong** here too) |
| D6 | False "infeasible", with a false reason, for a BOOL factor on a reducer over signed decisions | answers |

Where OPA is worse or arguable:
- Drops a "constrains nothing" warning in one NULL-key shape where ANR warns.
- `EXPLAIN` omits the solver rows for `abs` and the outer-helper line for a nest in a constraint; the guide's section 9
  promises them. OPA's plan is cleaner elsewhere (no side-plan CTEs).
- Which of two errors is reported differs in 15 of 1,258 ordered pairs, all involving a NaN read by a range frame.

Where OPA is better beyond the table: collated keys are named by their least spelling as the guide says; cast errors
carry a caret in four more places.

**Wrong on both, and on mainline** [agent ✓]: `per g: x <= 2` constrains the row whose `g` is NULL, against the
guide (its section 6.2). `per g: x <= cap` and `when id > 0 per g: x <= 2` get it right. Not a difference between the
branches; a defect to fix or a guide line to change.

Owner decisions the agent raised: the NULL-key rule above; whether a clause with classes but no model row should
warn; what `EXPLAIN` should show for rewrites and data bounds; whether "which error first" is a promise.

## B14. Environment note [measured]

A fresh test environment installs `gurobipy` 14.0 (`requirements.txt` says `>=12.0`), which the 13.0 license refuses,
so `make decide-setup` fails on a new checkout of either branch and of mainline. The reviewer installed 13.0.3 inside the two
review worktrees only. Worth pinning in `requirements.txt`.

## B15. Not done

- **No bisect** of the wrong answer inside the Nov 14–15 commits; only "correct at Nov 13, wrong at the tip".
- **The two suspected merge defects (B12) were not reproduced or disproved.** No debug or sanitizer build was run.
- **The 23 `DECIDB_DESCENT_SPLIT=1` failures** the tip commit reports were not triaged.
- **Suite modes:** only the default mode was run on each branch. Not run: `DECIDB_FORCE_SOLVER=highs`,
  `DECIDB_VERIFY_SERIALIZER=1`, `DECIDB_TEST_DIRECT_SOLVE=off`. HiGHS was covered by fuzzing only.
- **No TPC-H benchmark and no coverage run.** Benchmarks used the 12 synthetic scaling workloads.
- **No trial merge was built.** Integration effort (B11) is an estimate from reading conflicts.
- **The deck** used was `context/New_syntax.pdf` (119 pages, 2026-09-22), which is not in git. If Sami worked from a
  newer copy, B3 should be re-read against it.

## B16. Appendix: full agent reports

In `review/`. Each is a review agent's complete report, with `file:line` evidence that this
document only summarises. They are model output; trust a finding as far as its label in this document goes.

| File | Covers | Summarised in |
|---|---|---|
| `01_architecture.md` | Design of both evaluators; what step 5 needs on each; ranked clean-up proposals P1–P6 | B7 |
| `02_deck_fidelity.md` | Deck requirements against both branches; the running example on both binaries | B3 |
| `03_execution_model_and_merge.md` | Sink/Combine/Finalize on both; the 14 merge invariants; eight tests to run | B12 |
| `04_last_61_commits.md` | The Nov 14–15 commits by theme; what to keep and remove | B8 |
| `05_integration_with_mainline.md` | Conflict classification per file; hidden semantic collisions; effort | B11 |
| `06_test_quality.md` | Every changed or added test, classified; the weakened ones | B9 |
| `07_dead_code_opa.md` | Dead, duplicated and twice-stored code on OPA, with line counts | B10 |
| `08_dead_code_anr.md` | The same for ANR | B10 |
| `09_user_facing_behaviour.md` | About 4,500 queries through both binaries; D1–D12, S1–S7 | B13 |
