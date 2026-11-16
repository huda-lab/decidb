# Codex suggestions for the OPA merge and rewrite

Written 2026-11-16 for Fable, following the owner's discussion with Codex. This supplements
[handoff.md](handoff.md) and [merge_notes.md](merge_notes.md). It records the owner's architectural intent separately
from Codex's implementation recommendations. It does not authorize a merge, settle the remaining rulings, or change
the agreed order: merge first, then rewrite.

Review basis: `OPA-rewrites` at `8f79d59c2c` and prepared local `Sami-OPA` at `f56845f560`. Both remote parent hashes
matched the merge notes when checked. The review inspected source and documentation; it did not build a merged
branch or run new solver suites.

## 1. The owner's intended architecture

The parser should construct the modified DECIDE tree. Binding, canonicalization, optimizer rewrites, and evaluation
should work from that central structure. Constructing it only after binding or after optimizer rewrites falls short
of this intent.

**This is feasible.** The parser knows the syntax of scopes, reducers, frames, operators, and references. It can
create explicit E/G/O structure immediately. Binding subsequently resolves names, types, keys, aggregate overloads,
and collations, and supplies the information needed to derive signatures.

```text
Parser / Transformer: construct the DECIDE structure
                         |
Binder: resolve names and types in that structure
                         |
Canonicalization and optimizer: analyze and transform it
                         |
              Choose an implementation
                 /                 \
       Direct relational plan    Compiled tree execution
                                         |
                                Node-based model generation
```

The parser's transformation layer is an appropriate place to create the C++ nodes; the libpg_query grammar need
not construct execution objects. Ordinary SQL expressions should continue to use DuckDB's expression machinery.

Codex initially made this sound more restrictive than intended. The parser **can** construct the tree. The fact
that names and types are unresolved at that point is normal compiler behavior, not an obstacle.

## 2. Recommended implementation boundaries

Use the standard compiler pattern of a **typed intermediate representation with explicit lowering**. The structural
nodes follow the Composite pattern, with traversal and rewrite passes over them.

1. **One authoritative semantic representation per phase.** Explicit scope, reducer, and frame nodes originate in
   parsing. Binding may update those nodes or produce bound counterparts; exact C++ object identity is an open
   implementation choice. Later stages must not rediscover the structure from flattened helper records.
2. **Separate meaning from execution state.** Keep scopes, decision identity, expressions, and source origins in the
   semantic representation. Derive compiled programs, physical column positions, per-thread containers, hash tables,
   solver indices, and sparse rows at the appropriate boundaries. These derived structures are legitimate compiler
   outputs; the problem is independently maintained copies of the same semantics.
3. **Integrate with DuckDB traversal.** Every SQL expression owned by the tree must remain visible to column binding,
   pruning, and generic optimizer visitors. Define copying, equality/hashing where needed, and serialization for the
   supported phase boundaries. Cover prepared statements and repeated execution.
4. **Define analysis invalidation.** A rewrite can invalidate signatures, degree, bounds, frame placement, or
   dependencies. Specify which analyses it preserves and which are recomputed. An early tree alone does not prevent
   the stale-shape bug discussed in handoff A4; correct rewrite contracts are also required.
5. **Make direct solve consume the same meaning.** Rules should analyze the bound tree, directly or through a derived
   analysis view. A successful rule may lower to ordinary relational operators; a miss must leave the solver fallback
   semantically unchanged. A second independently interpreted DECIDE model would defeat the central-tree goal.
   This is Codex's recommendation for implementing the owner's direction; the exact `DirectIR` migration is not
   designed here.
6. **Generate the model from node results.** Remove plan-time helper reconstruction and the emitter's scatter back
   to dense per-row inputs as their consumers move. Preserve whole-tree consumer analysis where formulation needs
   it. A MIN/MAX's formulation depends on its use and polarity, not only its member count.

Preserve the existing requirements for `DIAGNOSE` re-formulation against a changed box, shared helper consumers,
parallel correctness, and constructs beyond the deck. Do not bake Big-M constants into an earlier stage that cannot
recompute them correctly. Source spans and origin tracking must survive rewrites so diagnostics quote the query.

## 3. Evidence from the current code

Paths in this table are repository-relative. Tree-specific code refers to prepared `Sami-OPA`; find identifiers
again after the merge because line numbers and layouts will change.

| Current code | What it establishes |
|---|---|
| `src/parser/transform/statement/transform_select_node.cpp`, `SelectNode` | Parsing currently stores declarations, constraints, and objective as separate parsed-expression fields |
| `src/parser/decide/decide_parsed_markers.cpp` | Scope and reducer syntax largely uses function markers |
| `src/include/duckdb/parser/expression/decide_frame_expression.hpp` | A dedicated parsed structural node already exists for frames; parser-created DECIDE nodes are feasible |
| `DecideBinder::BindDecideFrame` | Binding resolves payload types, fallback, order, and collation, then records separate frame metadata |
| `src/planner/binder/query_node/plan_select_node.cpp` | Canonicalization, frame placement, and signature construction currently operate before the executable tree exists |
| `src/execution/physical_plan/plan_decide.cpp`, `DecideTreeConstruction` | Sami constructs the tree after optimizer rewrites, then lowers terms into it |
| `DecideNode`, `DecideProgram` | The current tree mixes semantic structure with bound expressions and compiled execution details; moving its constructor into parsing is insufficient |
| `LogicalDecide::EnumerateExpressions` | DuckDB must see expressions held outside the ordinary clause fields; the new tree must supply this traversal |
| `AddValueHelper`, `InlineReducerValueLinks`, `DecideModelEmitter` | Helpers are still introduced before execution and simplified during formulation; the emitter adapts tree results to the existing model-builder contract |

Sami implements real routing, containers, signatures, data evaluation, and frames. The final model can match the
deck while still being generated by the older helper/formulation pipeline. Matching model output alone does not
prove the intended architecture has been implemented.

## 4. Gaps in the existing post-merge plan

The current handoff captures optimizer transformations and eventual node-based formulation, but does not fully
capture the owner's parser-first requirement.

1. **Expand Phase 2 to start at parsing.** Its current instruction, "Build the tree from the bound, canonical
   clauses," permits completing the phase without changing the parser or binder. Require explicit parsed nodes,
   binding of that structure, and canonicalization over its bound form.
2. **Resolve the lasting direct-solve boundary.** A6 item 2 still leaves tree versus separate IR open. Retain R2's
   temporary merge behavior, then specify how direct rules derive their analysis from the central representation.
3. **Add ownership and pass contracts.** Document semantic versus compiled state, traversal, copying, serialization,
   source origins, and invalidation of analyses. Avoid a single struct with unrelated fields for every phase.
4. **Make architectural acceptance explicit.** Require that semantic scopes/reducers/frames originate in parsing;
   that binding and rewrites use them; and that evaluation/model generation do not reconstruct them from helper
   lists. Add behavior tests at the parser/binder, rewrite, serialization, direct/fallback, and parallel boundaries.
5. **Expand Phase 5's documentation scope.** It currently names stages 05, 06, and 08. The parser, binder, logical
   planning/canonicalization, and direct-solve documentation must reflect this architecture too.

The precise node hierarchy and migration sequence remain design work. This note records the target and the missing
contracts, rather than claiming that a complete implementation design has already been agreed.

## 5. Guidance for Fable during the merge

Keep the architectural rewrite after the three-way merge. Preserve mainline's product-group representation,
canonical minimization convention, direct rules, source spans, and behavioral fixes while integrating OPA's language
and tree evaluation. The merge's temporary adapters do not establish the final architecture.

The read-only readiness review also found these preparation issues:

- **Parent query capture needs a mainline instrumentation step.** Mainline lacks both `capture_suite_log.sh` and
  the `_log_query` hook it needs. Copying the script alone is insufficient. Preserve each parent's own suite and
  capture outputs against its identified binary before changing its behavior.
- **The thread script is observational, not an automated gate.** `tools/threads_test.py` prints failures but exits
  successfully; an in-memory simulation confirmed this with every query failing. It displays whether descent
  merging occurred without requiring it. Require failure propagation and evidence that the merge path ran.
- **The matrix runner needs reliable mode isolation and exit handling.** `test/decide/run_direct_matrix.sh`
  inherits caller switches and does not preserve each pytest command's exit status. Clear conflicting mode switches
  and check command status as well as summaries before treating it as an acceptance gate.
- **R3 is still open.** Settle snapshot-test treatment before resolving those test conflicts. Codex recommends
  deleting obsolete evaluator snapshots, preserving independent regressions, and logging each deletion. This is a
  recommendation, not an owner ruling. R4 (`EXPLAIN`) remains deferred as documented.
- **Thread correctness remains unresolved.** Triage the 23 split-descent failures and establish or disprove I5/I6.
  Matching objectives on the smoke workloads does not discharge those requirements.
- **Trap 7's test lookup is ineffective.** `git log -S AddAbsValueCheck -- test` returns nothing. Search the producer
  in `src`, then inspect the associated test changes. Include the ABS overflow cases in
  `test_invalid_value_message.py` when proving the check survives on the solver path.

Use the existing merge checklist for the full verification gates. These suggestions supplement it; none of the
unperformed checks should be marked passed on the strength of this source review.
