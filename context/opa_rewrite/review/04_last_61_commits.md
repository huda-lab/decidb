<!-- Full report of a review agent, saved 2026-11-16. Model output, not verified unless the main report (../handoff.md) marks a finding as re-checked. -->

## Assessment of `36f12dd089..7837e300be` (61 commits, src net +4546 / -1069)

I worked from `git show` and `git diff` only. I built nothing and ran no tests. All file:line references are at the tip `7837e300be`.

### The range is two different efforts

- **A. 57 overnight commits with no message bodies:** `251540dc1f` (11-14 23:15) through `67d5297fc1` (11-15 11:14).
- **B. 11 commits in the house style with validation notes:** `d16b7383f6` (11-15 12:30) through `7837e300be`. B mostly repairs, reverts or cleans up A, and adds two pieces of work independent of A.

### 1. Themes

**T1. F nodes and the reordered stage-05 pipeline** (`251540dc1f`, `017b1dfadf`, `dcf1915038`, `7595262bdd`, `2a92ac1e16`)

Added:
- `DecideNodeKind::FUNCTION` with four kinds: ABS, BOOLEAN_PRODUCT, L0_INDICATOR, DEFINITION.
- Tree builders for them (`decide_tree.cpp:385-730`).
- Five new alias tags (`enums/decide.hpp`, +48 lines).
- New LogicalDecide metadata: `abs_function_envelopes`, `bilinear_function_groups`, `l0_indicator_functions`, `definition_functions`.
- Descent support (`IsSetNode`, F containers).
- `FunctionAbsence` and `AbsEnvelope` in the emitter.

Removed:
- `LiftNestedReducers` (~100 lines, `decide_rewrite_scoped.cpp`).
- `DefineNonlinearReducerBodies`, renamed to `DecideTreeConstruction::PrepareDefinitionFunctions` (`decide_tree_definition.cpp`).
- `PhysicalDecide::bilinear_links`, `abs_maximize_links` and `tree.bilinear_links`.

Pipeline changes (`017b1dfadf`, `decide_optimizer.cpp`):
- RewriteAbs now runs after the value rewrite.
- RewriteBilinear runs three times; TagAbsForBigM runs twice.
- `ScopeOf` was changed.

**F nodes do not formulate anything.** The ABS auxiliary, the product auxiliary and the L0/definition helpers are still allocated at plan time. The F node is a shadow of those rewrites; the header comment says so ("P9's ABS shadow metadata… remains allocated at plan time", `decide_tree.hpp`). To make this work:
- Written expressions now stay in place with tags.
- Every `DecideTermSplitter` entry point must see through the tags (`FindVariable`, `ContainsVariable`, `TryGetBareDecideFactor`, `ExtractCoefficient…`, `AddAbs`, `Split`, `IsLinear`; `decide_term_split.cpp` +157). Each new F kind adds to that list.
- Definitions leave a `TRUE` placeholder constraint in the clause list.
- The scope-to-(per, key) block is copied four times (`decide_tree.cpp:476, 528, 608, 704`).
- Stage 06 is unchanged. The emitter rebuilds `BilinearLink` records from the F nodes (`decide_model_emitter.cpp:656-675`), and `abs_maximize_links` comes from them too (`physical_decide.cpp:1268`).

Within the range:
- **`ScopeOf`:** the reducer-helper hunk added by `017b1dfadf` was removed by `9014a4c3d0`. The `BOUND_AGGREGATE` hunk stays.
- **Nested-SUM special case:** `has_nested_outer` was added by `017b1dfadf` for quadratic bodies only, and widened by `dcf1915038` to every unfiltered, same-scope nested SUM.

**T2. Node algebra, `DecideAlgebraNode`** (`6a28410850`, then about 25 commits named "… from E/G algebra" or "… from node algebra", `7d80850e95`, `9c64e09601`)

Added:
- The struct (`decide_tree.hpp:26-50`).
- `BuildAlgebra` (`decide_tree.cpp:732-1136`, ~405 lines).
- About 450 lines of algebra checks in `DecideTree::Validate`.
- A `formula=` field in the tree dump, now pinned in `test_one_pass_tree.py`.
- Emitter helpers (`decide_model_emitter.cpp:28-310`).
- `value_nodes` arrays carried through stage 06 (`EvaluatedConstraint::value_nodes`, `ReducerValueMembers::value_nodes`, `objective_value_nodes`, `link_of_node` in `linearization_reducer_value.cpp`).

Removed:
- PhysicalDecide's copies of `composed_minmax_constraints`, `composed_minmax_objective_terms` and `reducer_value_links` (`9aefb448cb`, `5d09527199`, `plan_decide.cpp`).

**T3. Node-owned helper contract.** Several commits copy each helper's label and envelope/closing sides onto the G or O node, put `qualifier_scope` on the G node, and make the frame code and `ReportInlinedHelpers` read the node instead of the `ReducerValueLink` copy: `ca1ee742cb`/`e7af54eb4b` (same subject twice), `71c40221a8`, `e83e72f385`, `f98b9126eb`, `0915c66276`, `5d09527199`.

**T4. Qualified-reducer reads.** These iterated three times inside the range:
- `08073f2236` added `QualifiedRows`, which looks up the operand for the whole scope.
- `7412f04713` deleted it because it picked a sibling occurrence.
- `33fd8a43b9` and `b9400ca010` added `DecideTerm::qualifier_reducer` and `DecideTermSite::qualifier_operand`, so a term reads its own reducer occurrence.
- `6a48906855` reordered how qualified coefficients are evaluated.
- **None of these commits adds a test.**

**T5. Stage 06 and assembly changes in shared code:**
- **MIN/MAX inlining:** `5a491e6cb0` and `ce7d0ccbdf` add `minmax_values` and a `stays_explicit` branch in `InlineReducerValueLinks` (`linearization_reducer_value.cpp:575-598`). For a keyed MIN/MAX whose groups all hold one member, this replaces the old path that dropped the column after the `InliningAddsNoNonzero` check: the helper now goes to `written_in`. No test and no stated bug.
- **Tighter bounds:** `12055e3f40` runs `DeriveReducerValueBounds` before implied-bound propagation. It now runs twice (`physical_decide.cpp:1316` and `:1542`). The golden change is `3964b76f26`: a helper's upper bound goes from 40 to 25.
- **ABS envelope rows:** `IsPlainLinearRow` now accepts them, and `LinearizeAbsMaximize` keeps `lhs_is_aggregate`. Both compensate for ABS now running after the value rewrite.
- **First input row:** `21b46615c2` and `060652ac6d` keep one input row per sink thread instead of fetching chunk 0 again.

**T6. Composition at nodes, added and then fully reverted.** `a4dcbd3386`, `4d4607cd8a`, `9f3ea37f76`, `35b0209bfc`, `89afaa7519`, `f01631abf1`, `67d5297fc1` and the fix `15e3786ac2` added about 535 lines (emitter +440, `ilp_model_builder` +36, `solver_input` +28). `a4263883ff` removed them all. `git diff 9419d261ae a4263883ff -- src` leaves only `9014a4c3d0`'s `ScopeOf` hunk. The tests those commits added stay in `test_anr_inlining.py` as model pins.

**T7. Group B fixes and cleanups:**
- `d16b7383f6` repairs the C++ unittest build, which `ce7d0ccbdf` had broken.
- `9014a4c3d0`, `e19ed66d5a`, `7837e300be`: the bug fixes covered in section 3.
- `0d60b4f111` deletes `PerGroupCache` and `ResolveGroupNames`, which were left empty when the flat evaluator was deleted. Independent of A.
- `169386c6b1` consolidates five absence checks into one `DecideAbsence` class. Mostly independent of A.
- `449b2ffe26` and `9c92e029e8` add container and step dumps, the L2 check (`DecideDescent::RequireRowReads`) and the L6 tests. These are tooling.

### 2. Net effect and whether each structure replaced or sits beside

Largest changes in `src`:

| File | Added | Removed |
|---|---|---|
| `decide_tree.cpp` | 1567 | 35 |
| `decide_model_emitter.cpp` | 1035 | 240 |
| `physical_decide.cpp` | 176 | 78 |
| `decide_descent.cpp` | 167 | 14 |
| `decide_term_split.cpp` | 157 | 3 |
| `decide_absence.cpp` + `.hpp` (new) | 218 | 0 |
| `decide_rewrite_abs.cpp` | 128 | 36 |
| `linearization_reducer_value.cpp` | 113 | 28 |
| `decide_linear_form.cpp` | 108 | 11 |
| `decide_rewrite_scoped.cpp` | 63 | 116 |

Whether each new structure replaced an older one:

| Structure | Verdict |
|---|---|
| F nodes | **Beside** the plan-time rewrites. They replaced only the carriers of bilinear and ABS-maximize links. |
| `DecideAlgebraNode` | **Beside.** It is built from the compiled records (`BuildAlgebra` reads `body.clause->lhs_terms`, `body.linear`, `body.members`, `body.rhs`, `op.composed_*`, `op.flat_objective_*`, `tree->objective_sense`). |
| Node-owned helper contract (T3) | **Replaced** PhysicalDecide's runtime copies. |
| `DecideAbsence` | **Replaced** five scattered checks. |
| `value_nodes` arrays | **Beside.** Stage 06 throws unless they agree with the variable-based lookup (`linearization_reducer_value.cpp` `link(...)` and `candidate(...)`). |

**What the emitter takes from the algebra.** About 41 of its 68 `InternalException` throws are algebra-versus-record agreement checks (the base had 2 throws in total). Every use falls into one of three groups:

- **Checks that return the record's own value:** `ClauseLhsFormula`, `ClauseTermSource`, `ClauseComparison`, `AlgebraTermProgram`, `AlgebraProductVariable`, `AlgebraProductProgram`, `FunctionAlgebraOutput`, `AlgebraDecisionLeaves`, `ObjectiveSumFormula`, `ComposedTermsFormula`, and the member-coverage loops in `ValueLinks`, `FrameMembers` and `ComposedTerms`.
- **Copies of a field still on the node, tree or operator:**
  - `ClauseRhsProgram` returns `body.rhs`.
  - `ValueLinkAggregate` returns `node.aggregate`.
  - `AlgebraTermValueNode` returns `tree.value_nodes[var]` or the variable's F node.
  - The composed-clause comparison and right-hand side come from `node.facts` and `node.rhs`.
  - `TreeObjectiveSense`, `TreeFlatObjectiveAggregate` and `is_easy` (`physical_decide.cpp:69-139`, `:1611`) duplicate `PhysicalDecide::decide_sense`, `flat_objective_agg` and `flat_objective_is_easy`, which still exist.
- **The only real source:** each composed MIN/MAX term's aggregate name, sign, `is_easy` and scale expression (`decide_model_emitter.cpp:1966-2025`). These are parsed from a string such as `"composed_max_scaled_by"`. This dependency exists only because `9aefb448cb` deleted PhysicalDecide's `composed_minmax_*` fields.

From the older records the emitter still takes:
- every coefficient program, variable, `inputs`, `frame_read`, qualifier, filter and `avg_scale` (`DecideTerm`, `DecideTermSite`);
- clause structure and flags (`node.compiled.clause`);
- link members and sides (`ReducerValueLinkSpec`, built from `LogicalDecide::ReducerValueLink` via the node).

**Could the algebra be deleted at the tip without changing any model?** Yes, if the composed term specs are carried on the E node again, or PhysicalDecide's copies are restored, and the three objective fields are read from PhysicalDecide again. Also to remove:
- the `value_nodes` arrays and `link_of_node` in stage 06;
- the algebra parts of `Validate`;
- the `formula=` dump and its pinned tests.

That is roughly 1,200–1,300 lines.

**Could the older records be deleted?** No. The algebra is derived from them and lacks inputs, frame reads, qualifiers and filters.

### 3. Bugs fixed and risks introduced

**Real fixes:**
- **`e19ed66d5a` (pre-existing bug, shared code).** A floor written on a Boolean product was lost: `b * x >= -4` answered x = -5. The fix is in `linearization_bilinear_abs.cpp` (`FloorBilinearAuxiliaries`), `decide_rewrite_bilinear.cpp`, and the sink-state bound check. The bound check now walks `op.tree->function_nodes`, so it depends on F nodes. Adds 101 lines to `test_bilinear.py`.
- **`9014a4c3d0` (regression from `017b1dfadf`, shared optimizer).** ABS of a nested helper made one auxiliary per row: `sum(per t.g: abs(sum(x) by (t.g) - 4))` answered 9 instead of 10.
- **`7837e300be` (regression from `251540dc1f`, tree-only).** `DecideDescent::Merge` crashed whenever more than one thread scanned the input and the query had an F node per class or query-wide (example: 130,000 rows). **Every commit from `251540dc1f` to `169386c6b1` has this crash.** The message says that under `DECIDB_DESCENT_SPLIT=1`, 23 large-input and step-dump tests still fail at the tip. I could not tell whether those failed at the base.
- **`15e3786ac2`:** fixed a regression from `a4dcbd3386`, and both were reverted.
- **`d16b7383f6`:** the unittest target did not build from `ce7d0ccbdf` through `15e3786ac2`, about 35 commits.
- **T4 qualifier fixes:** these look like regressions from keeping nested reducers in place (dropping `LiftNestedReducers`), with no tests. I can't confirm whether the base had the bug.
- **`0d60b4f111`, `169386c6b1`:** cleanups with no behaviour change, per their messages.

**Model changes in shared code:**
- ABS auxiliaries are now one per group: 9×11 becomes 7×8 on Gurobi, 9×14 becomes 7×10 on HiGHS. Legitimate and smaller.
- One helper's upper bound tightens from 40 to 25 (golden). Legitimate.
- The `has_nested_outer` path changes how nested SUM-in-SUM clauses are lowered. It is a case-listed special path; its comment says "Keep this deliberately narrow while filtered/deeper cases…".
- The `minmax_values` branch changes stage-06 inlining policy with no test.
- EXPLAIN no longer prints ABS envelope rows or L0 link rows.

**Changes to existing test expectations:**

| Test | Assessment |
|---|---|
| `test_abs_linearization.py:1961` (model counts) | Legitimate model change, explained in the commit. |
| `golden baseline.*.dump` (40 to 25) | Legitimate. |
| `test_fuzz_defects.py` | Comment and id rename only; same expected answer. |
| `test_anr_nested.py` F1 rows | Strengthened: exact rows now pinned. |
| `test_explain.py:695` | **Weakened.** Changed from "the rows it became" (asserted at least 3 rewritten rows shown) to asserting the rows are absent. |
| `test_scope_explain_diagnose.py:69, :174` | **Weakened.** The row expectations became `None`. |
| `test_scope_explain_diagnose.py:241` | **Inverted** (envelope row must now be absent). |
| `test_tree_emitter.py:1482` (`test_a_helper_read_once_is_written_where_it_is_read`) | **Weakened.** Was full equality with the recorded EXPLAIN ANALYZE output; now three substring checks. |
| `_old_spellings.py` | Scanner now skips tree-dump fixtures. |
| `test_one_pass_tree.py` pins | Rewritten to include `formula=`. |

The weakened and inverted tests all follow from the EXPLAIN change. It was accepted inside `017b1dfadf` and `7f7747c1e8` without a decision recorded anywhere.

### 4. Does the F-node and algebra work lead toward step 5?

**The algebra is a detour.**
- It is a flattened copy of stage 05's term lists, not the deck's operator tree. On the pinned dump, `0.10 * sum(t.d)` is folded into the right-hand-side program.
- Every use either checks it against a record or copies a field.
- The one step-5-like code in the range, the reverted composition `a4dcbd3386`/`4d4607cd8a`, was written against `DecideTerm` and members, not the algebra (0–2 algebra lines per commit).
- Step 5 would build per-group linear forms bottom-up from `node.body`/`child_reads` (already at the base), the compiled records, and `ReducerValueMembers`.

**F nodes are half foundation, half detour.** Step 5 as stated concerns G nodes (a MAX helper per group, SUM added upward), not functions.

Step 5 could reuse:
- the FUNCTION node kind;
- F containers in the descent, including the merge-order fix;
- the parent-child ownership of an F's defining E rows;
- `FunctionAbsence` and `DecideAbsence`.

Step 5 would replace:
- the plan-time allocation of ABS, product and L0 auxiliaries;
- the tagged envelope and expansion expressions and the term-splitter tag plumbing;
- the `TRUE` placeholders.

Also reusable or in step 5's direction:
- **T3's node-owned helper contract:** step 5 builds on it directly.
- **Removing `LiftNestedReducers`:** nested reducers stay as G nodes, which step 5 needs. It came with the T4 qualifier churn and the `has_nested_outer` special case.
- **`minmax_values`:** step-5 policy applied inside stage 06, which contradicts the branch's own todo ("replace `InlineReducerValueLinks` … rather than composing records beside them").

### 5. Verdict

**Start from the tip with these pieces removed:**
1. All of `DecideAlgebraNode`: struct, `BuildAlgebra`, the algebra checks in `Validate`, the `formula=` dump and its pins, emitter lines 28-310 and the interleaved checks, the `Tree*Objective*` helpers. Carry the composed term specs on the E node.
2. The `value_nodes`, `objective_value_nodes` and `link_of_node` plumbing in stage 06.
3. The `minmax_values`/`stays_explicit` branch (`5a491e6cb0`, `ce7d0ccbdf`), unless the owner wants that policy. It changes D89 with no test.

**Decide with the owner:**
- The EXPLAIN change and the weakened tests: restore the rows and assertions, or approve the change.
- Whether `has_nested_outer` should stay as a special path.

**Keep:**
- F nodes (with the caveat above);
- the reorder and the `ScopeOf` fix;
- T3;
- `DecideAbsence` and the group-cache deletion;
- `e19ed66d5a`, `7837e300be`, the first-row retention;
- the dump and L2/L6 tooling.

**Why not `36f12dd089`:** it lacks the real fixes (product floor) and cleanups, and it still lifts nested reducers. Those fixes are built on the F-node changes (the bound check reads F nodes), so cherry-picking them would mean rewriting them.

**Why not the tip as is:** it carries about 1,300 lines of a redundant mirror, plus untested stage-06 policy and EXPLAIN changes.

### What I could not determine

- Whether the T4 qualifier bugs and the `DESCENT_SPLIT=1` failures existed at the base.
- Whether `has_nested_outer` and `minmax_values` change any emitted model in the current suite.
- The runtime and memory cost of the per-row F containers; no benchmark was run in the range.
- Whether the four new LogicalDecide metadata vectors need serialization.
- Whether DIAGNOSE output changed for ABS and L0 rows.
- The commit messages' claims that the full suite passes.
