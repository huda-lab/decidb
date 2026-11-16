<!-- Full report of a review agent, saved 2026-11-16. Model output, not verified unless the main report (../handoff.md) marks a finding as re-checked. -->

## Fidelity report: OPA (tree) vs ANR (flat) against deck section 3, pp. 77-119

I changed no repository files and built nothing. I ran both prebuilt binaries only on the 6-row deck examples. The SQL files and dumps are in `/private/tmp/claude-501/-Users-hatim--claude-spares-spare-Ungos1/c596838a-2de5-42c6-8006-2a00850e535d/scratchpad/fidelity/`: `f1_max.sql`, `f1_sum.sql`, `f2.sql`, `opa_max.err`, `opa_f2.err` and `model_{opa,anr}_{max,sum}.txt`.

### 1. What the deck requires (steps 1-4 and p. 110)

- **Step 1, the annotated tree (pp. 80-81, grammar on pp. 28-30).**
  - Node kinds are E = scoped expression `(when, per)`, G = reducer `(fn, by)` and O = frame. The operator nodes (`≤ + × 0.10 D.c`) are tree nodes of the expression as written.
  - σ(E) = per key + direct tuple terms + direct `by` keys. Examples: root `(D.id, D.rid, S.m, D.c)`, `E(S.p,S.cid)` = `(S.cid)`, `E(TRUE,tid)` = `(tid, ship)` and `(tid, S.d)`.
  - An inner reducer groups within the current relation, which is its enclosing group (p. 30).
- **Step 2, top-down (pp. 83-91).** Each joined row is routed from the root once.
  - At an E: test `when`; if it holds, insert σ into the container and pass the row down.
  - At a G: take the `by` key and route into that key's operand E.
  - p. 91 end state:
    - root = {(d1,East,Air,30), (d1,West,Ground,50), (d2,West,Air,45)}
    - G(SUM,rid): East→{c1}, West→{c2,c4}. The row with p=F stops at E(S.p).
    - G(MAX,cid): c1→{(1,x1),(2,x2)}, c2→{(3,x3)}, c4→{(5,x5),(6,x6)}
    - G(SUM,m): Air→4 rows, Ground→2 rows
- **Step 3, bottom-up (pp. 93-97).** Each node is computed from its children's containers.
  - G(MAX): c1→max(x1,x2), c2→x3, c4→max(x5,x6). G(SUM,rid): East→max(x1,x2), West→x3+max(x5,x6).
  - Data G: Air→75, Ground→15.
  - The root composes one row per σ.
- **Step 4, frames (pp. 99-108).**
  - O(within, over) is filled top-down: within → over → payload signature.
  - Bottom-up, `at(previous)` shifts each timeline (A: 1→NULL, 2→x1).
  - The root emits per-row rows and drops those that read NULL. Result: x2≥x1, x4≥x3, x5≥x4, x6≥x5.
- **Containers (p. 110).**
  - G = `unordered_map<by-key, E>`.
  - E = `vector<sig without tid>` for per row, `unordered_set<sig>` for per K or per ().
  - O = `unordered_map<within, ordered_map<over, sig>>`.

### 2. Requirement table

OPA paths are relative to `/Users/hatim/Code/decidb/.claude/worktrees/sami-opa`; ANR paths to `.../sami-anr`.

| Deck rule (page) | OPA | ANR |
|---|---|---|
| E/G/O node kinds (80) | **Matches, with an extra kind.** `DecideNodeKind {SCOPE, REDUCER, FRAME, FUNCTION}` (`src/include/duckdb/planner/decide/decide_tree.hpp:23`). F (ABS, BOOLEAN_PRODUCT, L0_INDICATOR, DEFINITION) is not in the deck. It shadows plan-time auxiliaries (`decide_tree.hpp:214-233`). | **Absent.** No node objects. `RegisterEvaluation` (`physical_decide.cpp:2448`) walks the operator's flat tables (keys, clauses, objective, composed specs, value links, frames, signatures, checks) and only registers row expressions. Its comment calls this "the annotated tree", which it is not. |
| Tree is the written expression; operators are nodes (80) | **Departs, incidental.** The tree is built after every optimizer rewrite (`plan_decide.cpp:62-73`, `decide_tree.cpp:213`). <br>• Operator nodes do not exist. Each root's `algebra` is rebuilt from stage 05's lowered linear form `Σ coef·var ≤ rhs` (`decide_tree.cpp:899-940`). <br>• The data reducer is moved into the RHS bound program: `-(0.10*#4)+c`. <br>• The E/G skeleton is rebuilt from rewrite artifacts: helper links become G (`Link`, `decide_tree.cpp:1960`), retained bound aggregates become G (`Record`, 2019), frames become O (2055). <br>• For the running example the skeleton matches p. 80 node for node. | **Absent.** Clauses are flat `DecideConstraint` term lists (`physical_decide.cpp:3903`). |
| σ = per key + direct tuple terms + direct by keys (81) | **Matches.** The binder signature is shared code (`decide_canonicalizer.cpp:1802-1890`), copied onto roots (`UseSignature`, `decide_tree.cpp:1491`); child reads add BY_KEY fields (`Read`, 1733). <br>Printed σ: <br>• root `[by (t.rid), by (t.m), t.c]`; D.id is the slot key <br>• per-row E `[ship]` and `[t.d]`; tid is the slot <br>Small departure: `E(S.p,S.cid)` gets `[max(ship) by (t.cid)]`, whose key is the path key (rid, cid), not S.cid. It splits nothing more because rid is fixed inside the parent G slot. | **Partial.** The same shared signature is computed at plan time. At run time `BuildSignatureIds` (`physical_decide.cpp:837`) hashes per key + fields into a variant id per row. That is equivalent to the E set, but it lives per clause, not per node. |
| Inner `by` groups within the enclosing group (30) | **Matches.** G keys are path keys: G(MAX) key = `t.rid, t.cid`. They come from the plan-time link's `group_key`. | **Matches.** Same path key from the shared rewrite (`link.group_key`). |
| Top-down: one pass, routed root to leaves (83-91) | **Matches; vectorized per chunk.** <br>• The descent runs in Sink (`physical_decide.cpp:313-345`, `DecideDescent::Append` `decide_descent.cpp:1668`). <br>• `Visit` (1613) applies `when` (`Gate`, 1487), then the key and slot (`HoldKeyed`, 1517), stores the entry, then recurses with (row, parent slot). Expressions run only on rows that reach the node (`RequireRowReads`, 477). <br>• With `DECIDB_DESCENT_CHUNK=1`, `DECIDB_DUMP_STEPS` reproduces the p. 84-91 animation. <br>Departures: <br>• set-E entries wait until every root has descended (`pending`, `InsertDistinct` 1257) <br>• a NULL per key does not stop a row reaching the children (SQL-forced) <br>• threads merge descents afterwards (`MergeAll`) | **Departs.** <br>• Sink only buffers (`physical_decide.cpp:2108`). <br>• Finalize makes one scan that evaluates every registered expression on every row (`DecideRowStore::Build`, `decide_row_store.hpp`), including rows a `when` would never route there. Errors are deferred and raised when a read reaches the row. <br>• Routing is emulated afterwards with per-clause and per-term row masks (`KeptRows`, `ReadTermFilters`). <br>• Bound data reducers are not in the pass at all (see the data-G row below). |
| E container: vector per row / set of distinct σ (110) | **Matches in substance, columnar layout.** `DecideNodeContainer` (`decide_descent.hpp:150`): per row, entries are rows. A set E keeps entries per distinct (slot, σ, carried columns, reads of G/O groups), and instances are (slot, σ). So entries can outnumber instances, which the deck does not have. The root holds exactly 3 instances, matching p. 91. | **Implicit.** A variant-id column over all rows (`BuildSignatureIds`); no container. |
| G container: map by-key → operand E (110) | **Matches topologically, with extras.** <br>• Slots are a hash dictionary on the path key; the operand E entries carry `parent_slots`. That flattens the nesting through path keys. <br>• Extra: the G also stores one entry per row reaching it, including rows the operand's `when` rejects (G(SUM,rid) West holds rows 2-5). <br>• The operand E matches p. 91 exactly: East→c1, West→c2, c4. | **Implicit.** `PerGroupCache` row→group id over all rows (`BuildGroupIds`, `physical_decide.cpp:439`), keyed by expression set and shared across clauses. |
| Bottom-up, data G folded at the node (96) | **Matches.** `DecideAggregateStates` (DuckDB aggregate states per slot), via `DecideTreeColumns::Folds` (`decide_tree_columns.hpp:27-55`). The root reads it through a "hole": 30 - 0.1·75 = 22.5. | **Departs.** Precomputed in SQL before the operator: `direct_row_checks.cpp:106` → `DirectAddBoundTables` builds CTE + HASH_GROUP_BY + HASH_JOIN SINGLE (seen in EXPLAIN), joined back as a column. That reads the source twice. |
| Bottom-up, decision G composed symbolically (94-95) | **Departs.** Decision reducers are turned into helper decisions per group at plan time (EXPLAIN: "becomes a helper decision per group", both reducers). <br>• A G over decisions holds a helper variable (`value_var`), not an expression. <br>• `DecideModelEmitter::ValueLinks` lists members and the shared formulation inlines linear and singleton helpers. <br>This is the step-5 departure you already established, and it also removes step 3 for decision values. | **Departs, same way.** `EvaluateReducerValueLinks` (`physical_decide.cpp:5272`). |
| Root evaluates one row per σ (97) | **Matches, then departs.** Root programs are computed per entry (VALUES dump: 22.50 / 48.50 / 37.50 on the 3 instances). Then the emitter, an adapter (`decide_model_emitter.hpp:87`), scatters them back into `num_rows`-sized dense columns with `row_group_ids` (`decide_model_emitter.cpp:952-971`). That is flat's `EvaluatedConstraint`, which goes to the shared `BuildSolverInput`/`FormulateModel`. | **Flat.** Dense per-row coefficient columns plus group ids per clause and term (`physical_decide.cpp:3903-4250`). |
| O container: within → ordered over → σ (100-105, 110) | **Matches, with a standard substitution.** O slots are a hash on the within key. Entries carry the order value and sort-key bytes. `Timelines` (`decide_descent.cpp:2197`) does a stable sort per slot at Finish, which is DuckDB window style instead of an ordered map. Duplicate over keys share one position and are flagged (D73), forced by SQL data. Dump: `timeline {1:1;2:1;}` and `{1:1;2:1;3:1;4:1;}`, matching p. 105. The deck's separate `at(previous,NULL)` child is merged into the O node (p. 110 itself writes "O → at/agg"). | **Departs in structure, same result.** `BuildDecideFrameMap` (`physical_decide_frames.cpp:210`): hash timeline ids over all rows, then a stable sort per timeline. All arrays are input-row-sized. No top-down fill. |
| Frame bottom-up: shift, then root reads (106-108) | **Matches, but reuses flat code.** `DecideTreeColumns::FrameMap` (`decide_tree_columns.cpp:637`) converts the O container into flat's input-row-sized `DecideFrameMap` and calls flat's `SelectDecideFramePeers` / `ComputeDecideFrameColumns`. The frame is then read as a column. | **Matches the result.** Same selection arithmetic, inline in `BuildDecideFrameMap`. |
| Not in the deck | <br>• F nodes <br>• error "ranks" (`DecideErrorRank` route / value / clause / when on every node, `decide_tree.hpp:182-190`, `ErrorRanks` `decide_tree.cpp:1277`), there only to raise the same first error as flat, in flat's order <br>• hidden data-check roots <br>• `holds_rows_in_no_class` slots for NULL keys <br>• `routed` / `reads` maps <br>• collation spellings <br>• multi-thread descent merge <br>• the bounds `ship <= 100` gets a root E, then is folded into the column box | Evaluator switch `DECIDB_EVALUATOR=flat\|verify\|tree` (`decide_evaluator.cpp:12`). `tree` throws "the tree evaluator is not built yet". `verify` runs flat twice. |

### 3. Running example on both binaries

The SQL, from `_anr_cases.py` F1, prefixed with `SET threads=1; SET decide_direct_solve='off';`:
```sql
SELECT tid, ship FROM (VALUES (1,'d1','East','Air','c1',true,20,30),(2,'d1','East','Air','c1',true,15,30),
 (3,'d1','West','Ground','c2',true,10,50),(4,'d1','West','Ground','c3',false,5,50),
 (5,'d2','West','Air','c4',true,25,45),(6,'d2','West','Air','c4',true,15,45)) t(tid,id,rid,m,cid,p,d,c)
DECIDE ship(INT)
SUCH THAT ship <= 100 AND
  per t.id: sum(when t.p per t.cid: max(ship) by (t.cid)) by (t.rid) + 0.10 * sum(t.d) by (t.m) <= t.c
MAXIMIZE sum(ship) ORDER BY tid;
```
The second variant replaces the inner `max` with `sum`. The models come from `DECIDB_DUMP_MODEL=1`.

**MAX inside SUM: the two binaries produce byte-identical models.**
- 8 columns: x1..x6 with ub 100 (the `ship <= 100` bound folded into the box), plus helpers col 6 = x12 and col 7 = x56. Both helpers have ub = 37.5; I did not find where that bound comes from.
- Three constraint rows:
  - `x12 ≤ 22.5`
  - `x3 + x56 ≤ 48.5`
  - `x3 + x56 ≤ 37.5`
- Four definition rows: `x12 ≥ x1`, `x12 ≥ x2`, `x56 ≥ x5`, `x56 ≥ x6`.

This equals p. 115: helpers only for the two groups with more than one member, c2 → x3 used directly, and the 7.5 / 1.5 / 7.5 moved to the RHS. The deck writes `x = max(...)`. The code encodes it one-sidedly as ≥ rows, which is exact because max sits on the ≤ side with a positive coefficient. Optimum 218 on both.

**SUM inside SUM: identical on both.** 6 columns and 3 rows: `x1+x2 ≤ 22.5`, `x3+x5+x6 ≤ 48.5`, `x3+x5+x6 ≤ 37.5`. This equals p. 119. Both binaries still create two helper decisions at plan time; the formulation inlines them away.

**Frame example (F2, `inventory >= at(previous: inventory) over (t.period within t.product)`).** Identical on both: rows x2≥x1, x4≥x3, x5≥x4, x6≥x5. This equals p. 108.

**OPA tree versus p. 81** (`DECIDB_DUMP_TREE`, nodes 1-7):
- root E key=t.id, σ `[by rid, by m, t.c]`
- G(sum, t.rid) → E(when p, per cid) → G(max, key rid,cid) → E per row `[ship]`
- G(sum, t.m) → E per row `[t.d]`

That is the same skeleton as p. 81. The `≤ + × 0.10 D.c` nodes are absent, and the formula shows the stage-05 form with the data reducer on the RHS.

**OPA containers versus p. 91** (`DECIDB_DUMP_CONTAINERS` / `STEPS`):
- Matches: root, E(per cid), G(MAX) and G(SUM,m) as listed in section 1.
- Differs: G(SUM,rid) also lists row entries {0,1} and {2,3,4,5}, including the row that fails `when`.

ANR has no dump of this kind; it has no tree or containers to print.

### 4. Verdict

**OPA is far closer to the deck on steps 1-4.**
- It has real E/G/O nodes, a single top-down routed descent during Sink that reproduces the p. 84-91 animation, per-node containers matching p. 110 in substance, data reducers folded at G nodes, and frames filled into O containers.
- Its departures:
  - **Large: built after the rewrites.** The tree is reconstructed from the post-optimizer plan, so operator nodes are gone and bodies are stage 05's lowered terms.
  - **Large: decision reducers are helper variables.** There is no symbolic bottom-up value at a G over decisions.
  - **Large: the back end is flat's.** The emitter scatters node results into input-sized per-row columns and hands them to flat's shared formulation.
  - **Medium: error ranks.** Every node carries rank metadata whose only job is to raise errors in flat's order.
  - **Small: frame selection reuses flat's input-sized `DecideFrameMap`.**
  - **Small, mostly SQL-forced:** extra per-row G entries, path-key σ fields, NULL-key slots, D73 duplicate positions.

**ANR is structurally far from the deck.**
- No tree and no top-down routing: everything is evaluated on every row, with masks afterwards.
- Bound data reducers are SQL side tables computed before the operator.
- Containers exist only implicitly, as per-row id columns (group ids, variant ids, frame maps).
- What does correspond to the deck: the one-scan row store (a "single pass" of expression evaluation, not routing), the hashed group / variant / timeline ids as stand-ins for the G / E / O containers, and the shared plan-time σ.

**What must be undone to build step 5 the deck's way:**
- **On OPA:**
  - Build the tree before the value rewrite (`RewriteScopedReducers` helpers) and before stage 05 lowering, so G nodes over decisions carry linear expressions upward and create a helper only where needed (a MAX over more than one member).
  - Restore real operator nodes, or at least compose child linear forms at the root, instead of reading stage 05's terms.
  - Replace the emitter adapter (num_rows columns into flat's `FormulateModel` and its helper inlining) with node-level row emission.
  - Fold the F nodes' plan-time auxiliaries into node formulation.
  - Keep the descent, containers, σ, folds and O containers; they fit.
- **On ANR:** effectively all of OPA's steps 1-4 must be built first: tree, descent, containers, and in-operator data folds replacing the side tables.

### 5. Could not determine

- Where both binaries get the helper upper bound 37.5 for x12, when the row bounds it to 22.5. Both produce the same value.
- Whether the descent merge is order-faithful with threads > 1 (all runs used `threads=1`).
- What the deck intends for a G group whose every row fails the operand's `when`. OPA keeps the G slot; the deck is silent.
- I read OPA's error-rank and F-node code only at the level needed to classify it, not line by line.
- I did not check which shapes beyond F1 and F2 reach the tree with the same node skeleton.
