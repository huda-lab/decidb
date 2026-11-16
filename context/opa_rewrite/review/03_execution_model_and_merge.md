<!-- Full report of a review agent, saved 2026-11-16. Model output, not verified unless the main report (../handoff.md) marks a finding as re-checked. -->

## Review of the ANR (flat) and OPA (tree) execution models

I only read code: no edits, builds or test runs. Paths below are relative to these two roots:
- **ANR** = `/Users/hatim/Code/decidb/.claude/worktrees/sami-anr` (HEAD 300e387bb1)
- **OPA** = `/Users/hatim/Code/decidb/.claude/worktrees/sami-opa` (HEAD 7837e300be)

**Short answer:**
- **ANR is safe under parallelism because almost nothing runs in parallel.** Its whole evaluation is one single-threaded, uninterruptible pass in Finalize. That pass evaluates every expression on every row, ignoring `when`.
- **OPA has the better architecture.** It follows DuckDB's hash-aggregate pattern and is parallel in Sink. But its `Merge` depends on two planner invariants that nothing checks in a release build. I believe one can silently produce a wrong model and the other can crash.
- **The merge path is barely tested.** It only runs when a scan spans more than one row group on two or more threads, and the tests only simulate that with `SET threads = 1` plus `DECIDB_DESCENT_SPLIT`.

---

### ANR (flat)

**1. Sequence from Sink to solver input**
- **Declarations.** `ParallelSink()` returns true (`physical_decide.hpp:288`). There is no `SinkOrderDependent` override, so the default `false` applies: no order preservation, no batch index.
- **Sink.** Appends the chunk to a thread-local `ColumnDataCollection` (`physical_decide.cpp:2108-2113`).
- **Combine.** Takes `gstate.lock`, then `gstate.data.Combine(lstate.data)` (`2115-2124`).
- **Finalize, all on one thread with no locks** (`3017-3113`):
  - returns early if `num_rows == 0` (`3033`);
  - builds `DecideRowStore row_store` (`3056`);
  - runs `Evaluate` (`3115-3174`): `RegisterEvaluation` (`2448-2598`), then `row_store.Build()` (the only scan of the input), then frames, keyed mappings, scope checks, constraints, objective, composed clauses and value links;
  - then `BuildSolverInput`, `FormulateModel` and the solve (`3082-3111`).
- There is no parallelism inside Finalize: no `TaskExecutor` in ANR's operator code.

**2. Copies and memory**
- **Copies of each row:**
  - one in the local buffer;
  - one scan in `Build` (`decide_row_store.cpp:166-204`);
  - one copy per registered non-constant expression into its slot, a `Vector(type, num_rows+1)` (`AddSlot` `104-131`, copy at `82`);
  - one copy per kept input column (`169`, `195`).
  - Later loops read slots in place (`DecideRowWindows::Execute` `301-344`).
- **Per clause, per row:** `vector<idx_t>` group ids, `vector<bool>` masks, and a dense `CoefficientColumn` (8 B per row per term). There are 34 `num_rows`-sized allocation sites in `physical_decide.cpp`.
- **Per key:** a `PerGroupCache` entry with 8 B per row of ids plus `evaluated_rows` (`626-688`).
- **Per row in total:** O(input width + Σ distinct expression widths + 8·keys + 8·(clauses + terms)).
- **Per distinct expression:** one full-length Vector, plus a `std::map` `ErrorData` entry per failing row.
- **Peak:** after Build, during the clause loops. The store is a Finalize local, so it stays alive through formulation and the solve. Slot Vectors live on the plain heap, outside the buffer manager: `memory_limit` does not see them and they cannot spill.

**3. Time**
- `Build` is O(R·X), where X is the number of distinct registered expressions. Selectivity does not reduce it.
- **Grouping** (`BuildGroupIds` `439-613`):
  - a `GroupedAggregateHashTable` probe per row;
  - an O(G log G) sort over string sort keys;
  - then a `lower_bound` per row to find each row's group by address (`NumberGroupsByValue` `373`), which makes it O(R log G).
- **Grouping is repeated.** `CachedKeyGroups` does an O(R) "covers" scan on every call and rebuilds the whole grouping whenever a clause reads the key on more rows (`634-686`). Worst case is O(C·R log G).
- Each clause makes about 10 more O(R) passes (for example `3999-4605`).
- **Error cliff.** If any row of a window raises, the whole window is redone row by row: a fresh `ExpressionExecutor`, a `DataChunk` slice, `Value` boxing and an `ErrorData` per failing row (`43-83`). This happens on rows a `when` would have dropped, because `Build` ignores `when`.
- `BuildSignatureIds` uses a nested `std::unordered_map` lookup per row for keyed decisions (`906-919`).

**4. Determinism**
- Row-order-independent: groups are numbered by key value. Windows are fixed `STANDARD_VECTOR_SIZE` slices of the global row order, so chunk boundaries do not matter.
- Order-dependent: per-row variables, output row order, and which error is raised first all follow the order rows land in `gstate.data`. That is the order threads Combine in, and nothing restores it.

**5. Concurrency**
- Shared state is touched only in Combine, under the lock. Zero-row threads are handled (`ColumnDataCollection::Combine` returns early on an empty collection).
- `DecideInputScans` is `thread_local` (`142-145`).
- `run_time_accounts` is a `mutable` member on the physical plan, written in Finalize (`physical_decide.hpp:166`). Both branches have it.
- I found no data races.

**6. Errors and interrupts**
- During the pass, `INTERRUPT`, `OUT_OF_MEMORY` and `FATAL` are rethrown (`StopsTheQuery` `35-38`); every other error is held per row and raised only when a read reaches that row.
- `std::bad_alloc` is not classed as OOM, so it is stored as a row error.
- The Finalize evaluation checks `context.interrupted` only inside frames (`physical_decide_frames.cpp:171,187,278`). The rest of the row pass and grouping cannot be interrupted.
- No partial state survives a failure: the store is a Finalize local.

**7. DuckDB analogue**
- It is a materialize-then-compute sink, like the old single-threaded `PhysicalWindow`, or `PhysicalColumnDataCollector` followed by a serial Finalize.
- It departs from DuckDB in four ways:
  - no parallel finalize;
  - no partitioning;
  - full-length Vectors outside the buffer manager;
  - group ids found by row address plus binary search, where DuckDB stores them in the hash table.

---

### OPA (tree)

**1. Sequence from Sink to solver input**
- **Declarations.** `ParallelSink()` returns true (`physical_decide.hpp:229`). Like ANR, no order dependence.
- **Sink** (`physical_decide.cpp:409-450`), per thread:
  - appends to a local `ColumnDataCollection`;
  - keeps the first row it sees;
  - runs `DecideDescent::Append` (`decide_descent.cpp:1668-1728`), which visits the tree on the chunk: gate (`when`) → key → hash probe → store (`Visit` 1613, `Gate` 1487, `HoldKeyed` 1517, `Store` 1376), then `InsertDistinct` for set Es (1257).
  - Two `getenv` calls run on every Sink chunk (`316`, `325`).
- **Combine** (`MergeDecideDescent` `380-395`), under `gstate.lock`:
  - `offset = gstate.data.Count()`, then `Combine`, then `descent_parts.emplace_back(descent, offset)`.
  - No merging happens here.
- **Finalize, one thread except the copy step:**
  - `MergeAll` (`771`; `decide_descent.cpp:2181-2195`): a serial `Merge` of parts 1..P into part 0, then a parallel `CopyMergedParts` using a `TaskExecutor` (`2115-2179`);
  - `Finish` (rank sorts and timelines, `2277-2293`);
  - `DecideTreeColumns` computes the bottom-up programs serially (vectorised per node, `decide_tree_columns.cpp:192-290`);
  - `DecideModelEmitter` runs serially (`physical_decide.cpp:865-890`);
  - then the same solve path as ANR.

**2. Copies and memory**
- **Per row:**
  - one copy in the local buffer;
  - per node the row reaches: 24 B of `rows`/`parent_slots`/`slots` plus its signature, carried and program columns copied (`DecideColumns`, allocated with `Allocator::Get(context)`, which is not buffer-managed);
  - per set E: a 16 B `routed` pair per row;
  - per keyed scope: 8 B in `decision_rows`;
  - per shared node: a `std::set` insert per row per window.
- **Per distinct slot, class or entry, per thread:** entries in a `DecideKeyMap`, a `GroupedAggregateHashTable` kept pinned in the buffer manager (`decide_key_map.cpp:32`). Total is O(T × distinct keys).
- **Merge makes another copy:** parts 1..P are copied into part 0.
- **Bottom-up adds:** one Vector per program per entry, plus the same `num_rows`-sized per-clause arrays as ANR (42 sites in the emitter).
- **Peak:** during `MergeAll`. All parts and the merged containers are alive together, since parts are freed only when `MergeAll` returns, and so are every thread's pinned maps. That is about 2× the container memory plus T × the hash tables.
- **Retention:** the merged descent and its maps stay in `gstate.descent` through the solve and the Source phase (`physical_decide.cpp:771,788`; never reset). Nothing reads it after `EvaluateTree`, because labels are materialised by value in `DecideGroupKeyValues` (`decide_model_emitter.cpp:321-336`).

**3. Time**
- **Sink:** O(Σ rows reaching each node), parallel. Expressions run only on gated rows. Each key is evaluated once per row per window (`KeyReads`). Each node does one hash probe per row; the slot id is held in the table's aggregate register, so there is no address search.
- **Large-constant spots in Sink:**
  - a `std::set` insert per row per shared node (`1628-1635`);
  - collated keys box every row into `Value`s, build sort keys from a `SetValue` per row, and hash into vector buckets (`767-777`, `852-865`).
- **Finalize:**
  - `Merge` is serial, O(Σ slots + set-E entries + rows of `decision_rows`). Each keyed class also builds a `std::string` from `type.ToString()` per column (`1772-1779`), which is heavy when there is a class per row.
  - The copy step is parallel.
  - `Finish` is O(S log S) per node; timelines are O(E log E).
- **Emitter:** `Variants` puts a `std::map<vector<Value>>` entry per entry and builds `Identity` per entry, so O(E log V) with `Value` boxing (`decide_model_emitter.cpp:1076-1147`). The objective calls it twice (`1724-1725`).

**4. Determinism**
- Row order follows Combine order, as in ANR.
- Part order is restored: offsets ascend and parts are merged in that order.
- Groups, classes and timelines are ranked by value in `Finish`.
- Restoration is incomplete in three places:
  - **Container numbering follows chunk boundaries.** `test_descent_merge.py:100-110` compares a split run with the same pieces read by one descent, not with the whole input.
  - **Enclosed nodes rank by creation order.** `DescentGroupRanks` sorts by the enclosing slot's creation id, not its rank (`309-331`, `322`). The consumers I checked only rank nodes that have no enclosing slot, so this is latent.
  - **Error ranks can drift with chunk contents.** ROUTE-block ranks are a per-visit counter. It does not advance on an empty selection (`520-522`) or on a short-circuited conjunction (`609-611`).

**5. Concurrency and Merge**
- **Concurrency is fine.** Descents are per thread. Shared plan objects are read-only. Combine holds the lock. The copy tasks write disjoint windows after a separate sizing pass finishes (`2120-2138`).
- **Zero-row and all-rows cases are fine.** A thread with zero rows contributes no part (`381-384`). If one thread sees every row there is a single part and no merge.
- **`Merge` (`1762-2085`) and `MergeAll`.** Each invariant it relies on, and whether it is established:

| Invariant | Status |
|---|---|
| I1. Parts are in ascending order with contiguous row ranges | Holds. Set under the lock (`380-395`); `ColumnDataCollection::Combine` appends segments in order and resets the source. |
| I2. Part 0 has offset 0 | Holds in practice; only a `D_ASSERT` (`2183`). |
| I3. Non-set containers hold no thread-local ids in `values` | Holds. Decision columns are kept only for set nodes (`405`), so the raw `CopyRows` is correct. |
| I4. Set-E decision instances are rebased: keyed ones through `class_map`, per-row ones by `+offset` (`1988-2000`) | Holds only if no query-wide decision is in sigma. Checked by `D_ASSERT(!global)` alone (`1960`). `Read()` of an F child adds a DECISION field without a global check (`decide_tree.cpp:1740-1742`). |
| I5. Decision classes are matched across parts by (layout types, sort key) (`1780-1811`) | **Not established.** The sort key comes from the layout probe (`1150-1155`). That probe contains `Signature(definition)` (`1220`), which writes local row or class ids for DECISION fields (`939`). These are never rebased. |
| I6. A node's slots merge after its enclosing node's slots (`1911-1929`); `PartParent` needs a valid enclosing node | **Not established for scope nodes.** `FindGroups` records real parent slots for any scope node (`956`). `InsertDistinct` does not apply the MIXED mask that `Store` applies (`1326` vs `1458-1462`). `PartParent` checks only with a `D_ASSERT` (`2092`). |
| I7. Slot identity is the same in the per-thread probe and in the merge probe | Holds for enclosed nodes (`group_maps`) and keyed non-enclosed nodes (shared dictionary). For the empty-key non-enclosed path, `FindGroups` ignores the parent (`965-977`) but `Merge` uses parent+1 (`1839-1852`). These agree only while those parents are INVALID. |
| I8. New ids are sequential in probe order | Holds (`decide_key_map.cpp:53-57`; checks at `1893-1898`, `2028-2033`). |
| I9. `first_rows` of slots found again need no update | Holds, by I1. |
| I10. `reads` are rebased the same way `landed` wrote them | Holds (`1590-1596` vs `1964-1971`). |
| I11. Every collated slot has a spelling | Holds (`1554-1561`; assert at `1749`). |
| I12. `decision_rows` is never longer than the offset | Holds (`1806`). |
| I13. Error rows and block ranks are rebased | Holds (`decide_error_ledger.hpp:90-102`). |
| I14. No `Append` after a merge | Holds (`1669`). |

**My verdict on Merge:** it is correct for the shapes it was built and tested for. It is not correct in general, because I5 and I6 are left to the planner and are not checked in release.

**6. Errors and interrupts**
- Expression errors become per-row ledger entries, raised by the least rank at the end (`890`).
- `INTERRUPT`, `OOM`, `FATAL` and `INTERNAL` are rethrown on the first try (`545-548`). In the per-row retry only `INTERNAL` is rethrown directly, but `DecideErrorLedger::Add` rethrows the other three (`decide_error_ledger.hpp:79-81`).
- `Visit` checks for interrupts in Sink (`1620`). `Merge`, `Finish`, the bottom-up step and the emitter cannot be interrupted.
- An exception inside a copy task: `TaskExecutor` waits for all tasks before it rethrows, so no task is left holding dangling references.
- A partial descent after a failure is never read again.

**7. DuckDB analogue**
- It is `PhysicalHashAggregate`: thread-local tables in Sink, merged in Finalize.
- It departs in four ways:
  - no radix partitioning, so the map merge is a serial fold rather than partition-parallel;
  - entries are O(rows), not O(groups);
  - it also materialises the whole input, like Window or Order;
  - it keeps creation order to match flat numbering.

---

### Side by side

| | ANR | OPA |
|---|---|---|
| Parallel work | Append only | Gate, key, probe and store in Sink; the merge copy |
| Serial work | Whole evaluation, O(X·R + C·R log G) | `Merge`, `Finish`, bottom-up, emitter: O(slots + entries + C·R) |
| Expressions evaluated on | Every row | Rows that reach the node |
| Grouping | Hash table + address binary search, rebuilt per row set | Slot id in the hash register, once per thread |
| Memory peak | Input + full slot Vectors + per-clause arrays, held through the solve | Input + all parts + merged copy + T × pinned maps; merged descent held to end of query |
| Buffer-managed | Input only | Input and key maps (pinned); containers are not |
| Depends on row order | Yes (per-row variables, errors) | Yes, plus chunk-boundary numbering |
| Concurrency risk | None | Merge invariants I5 and I6 |
| Interrupts | Not checked during the row pass | Checked in Sink; not in Finalize |
| Tested with more than one thread | Not needed | Only through `SET threads = 1` + `DECIDB_DESCENT_SPLIT` (`test_descent_merge.py:14,72`) |

---

### Defects and risks, by severity

1. **High, OPA (suspected).** I5: decision-class identity across parts embeds local instance ids.
   - **Trigger:** a keyed helper (ABS, L0 or product F, or a definition) whose definition signature reads a per-row decision, or a decision keyed by another key. Input larger than one row group, threads ≥ 2.
   - **Effect:** the same class split in two, or two distinct classes merged into one. Either way the model is silently wrong.
2. **High, OPA (suspected).** I6: a per-K or `per ()` F node read inside the operands of two reducers with different `by` keys gets `MIXED_ENCLOSING`.
   - **Effect:** `PartParent` indexes `slot_map[MIXED_ENCLOSING]` → out-of-bounds read and a crash in release.
   - Even without a merge, that F's slots are keyed by parent slot ids taken from two different numbering spaces.
3. **High, OPA (process).** The merge path is reached only by large multi-threaded scans.
   - The last commit (7837e300be) fixed two crash classes found at 130,000 rows.
   - Its message reports 23 suite failures still open under `DECIDB_DESCENT_SPLIT=1`.
4. **Medium, ANR.** `Build` ignores `when` and falls back to row-at-a-time on any error.
   - **Example:** `when w > 0: x <= ln(w)`. Every window containing a `w <= 0` row runs row by row and stores an `ErrorData` per row, for rows that are never read.
5. **Medium, both.** Long single-threaded Finalize phases with no interrupt checks (details in section 6 of each branch).
6. **Medium, OPA.** About 2× container memory at the `MergeAll` peak, plus T × pinned maps. The merged descent is retained for no purpose until the query ends.
7. **Medium, ANR.** Full-length slot Vectors live outside the buffer manager: `memory_limit` does not see them, they cannot spill, and they are held through the solve. OPA's containers share the not-buffer-managed part of this.
8. **Medium, OPA.** The merge is serial and its cost grows with threads × distinct keys, including a string built per class.
9. **Low to medium, both.** No order restoration. With threads > 1 on large inputs, variable order and the reported error can change from run to run, which can change which of several tied optima the solver returns.
10. **Low, OPA.** Error-rank counter drift (see Determinism above).
11. **Low, OPA.** Enclosed ranks use creation ids rather than value ranks (latent).
12. **Low, OPA.** The I7 empty-key mismatch.
13. **Low, OPA.** Per-row large constants: `std::set` for shared nodes, collated `Value` boxing, `getenv` on every Sink chunk, and `Variants` called twice.
14. **Low, ANR.** O(R log G) group lookup, the repeated covers scan and rebuild, and the per-row `unordered_map`.
15. **Low, both.** The mutable `run_time_accounts` on the shared physical plan.

---

### Which execution model is sounder

**OPA's model is the sounder architecture.**
- It is DuckDB's own pattern for a parallel sink: thread-local hash tables with the slot id in the register.
- It does the O(R) row work in parallel, and only on rows that reach a node.
- It never evaluates an expression on a row that `when` drops.
- It materialises labels by value, so nothing points back into a temporary store.

**ANR is safer today only because it does nothing in parallel.** Its serial Finalize, its evaluate-every-row pass and its unaccounted memory cap how far it can scale.

**Recommendation:** choose OPA, on the condition that I5 and I6 are either enforced (rebase the class sort keys; key mixed-enclosure parents by enclosing node, or refuse such plans) or disproved by the tests below. Also release `gstate.descent` after `EvaluateTree`. If the decision has to be made before those tests run, ANR is the lower-risk choice for correctness.

---

### What reading could not settle, and the test I would run

1. **I5 reachability.** Run the suite with `DECIDB_DESCENT_SPLIT=2 make decide-test` and with `=1`, and triage the 23 known split failures for model differences rather than timeouts. Then add a `MODEL_CASES` entry to `test/decide/tests/test_descent_merge.py`, for example a keyed `per t.g` ABS of a decision keyed per `t.k`, and compare `DECIDB_DUMP_CONTAINERS` and `dump_model` between the whole input and splits 1, 2 and 3.
2. **I6 crash.** Add a case such as `DECIDE x(INT), per t.g: y(REAL) SUCH THAT sum(x + abs(y-2)) by (t.k) <= 20 AND sum(x + abs(y-2)) by (t.period) <= 30`. Run it under `DECIDB_DESCENT_SPLIT=2` on a debug build, where the `D_ASSERT` should fire, and on an ASan build.
3. **Real multi-thread determinism.** Build a 600k-row table (5 or more row groups) with a trivial model (`x <= 1` per row, maximise `sum(x)`). Run 10 times at `SET threads = 8` and once at `threads = 1`, and compare model dumps for both branches.
4. **Memory peak.** Use `/usr/bin/time -l` on a 1M-row keyed query at threads 1 and 8, with the profile phases `execution.descent_merge` and `execution.one_pass`.
5. **ANR error cliff.** Time `execution.one_pass` for `when w > 0: x <= ln(w)` on 1M rows with half of `w <= 0`, against all `w > 0`.
6. **Interrupt latency.** Send Ctrl-C during Finalize of a 10M-row DECIDE and time how long the query takes to stop.
7. **Error-rank drift.** Use a `when a OR b` node where `a` holds for every row in one chunk, plus two failing casts in different chunks. Compare the error text between default chunking and `DECIDB_DESCENT_CHUNK=1`.
8. **Scaling split.** Time the Sink phase, `execution.descent_merge` and `execution.tree_model` at threads 1/2/4/8 on a `per id` keyed decision, to see whether the serial merge dominates.

Items 3 to 8 are CPU-heavy. Keep the model trivial so the solver cost stays negligible.

Key files:
- `/Users/hatim/Code/decidb/.claude/worktrees/sami-anr/src/execution/operator/decide/physical_decide.cpp`
- `/Users/hatim/Code/decidb/.claude/worktrees/sami-anr/src/execution/operator/decide/decide_row_store.cpp`
- `/Users/hatim/Code/decidb/.claude/worktrees/sami-opa/src/execution/operator/decide/decide_descent.cpp`
- `/Users/hatim/Code/decidb/.claude/worktrees/sami-opa/src/execution/operator/decide/physical_decide.cpp`
- `/Users/hatim/Code/decidb/.claude/worktrees/sami-opa/src/execution/operator/decide/decide_model_emitter.cpp`
- `/Users/hatim/Code/decidb/.claude/worktrees/sami-opa/src/include/duckdb/common/decide_error_ledger.hpp`
- `/Users/hatim/Code/decidb/.claude/worktrees/sami-opa/test/decide/tests/test_descent_merge.py`
