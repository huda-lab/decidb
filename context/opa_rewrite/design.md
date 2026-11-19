# Planned design changes

Two changes follow Phases 0 to 4 of the queue: the tree becomes the one representation of a DECIDE clause (Phase 5), and
the solver model is written at the tree's nodes (Phase 6, the deck's step 5). Rulings: [decisions.md](decisions.md).

## 1. What the deck requires

The deck is `context/New_syntax.pdf`; pages 77 to 119 give the algorithm. `repro/deck_example.sql` is its running
example.

| Step | Rule | Pages |
|---|---|---|
| 1. The tree | Nodes are E (a scoped expression: `when`, `per`), G (a reducer: function, `by`) and O (a frame). The operators of the written expression (`<=`, `+`, `*`) are nodes too. An E's signature σ is its `per` key, its direct tuple terms and its direct `by` keys. An inner reducer groups inside its enclosing group | 28-30, 80-81 |
| 2. Top-down | Each joined row is routed from the root once. At an E: test `when`; if it holds, insert σ into the container and pass the row down. At a G: take the `by` key and route into that key's operand | 83-91 |
| 3. Bottom-up | Each node is computed from its children's containers. A G over data folds to a value; a G over decisions composes an expression (`max(x1, x2)`, `x3 + max(x5, x6)`). The root composes one row per σ | 93-97 |
| 4. Frames | An O is filled top-down by `within`, then `over`, then the payload's signature. Bottom-up, `at(previous)` shifts each timeline; the root drops rows that read no value | 99-108 |
| 5. The model | Written at the nodes, bottom-up. A SUM passes its members up. A MAX allocates one variable per group that has more than one member | 115, 119 |

Containers (p. 110): a G is a map from `by` key to its operand E; an E is a vector of signatures for `per row` and
a set of distinct signatures for `per K` and `per ()`; an O is a map from `within` to an ordered map of `over`.

## 2. Where the branch stands

| Step | On the branch |
|---|---|
| 1 | The tree exists, with a fourth node kind, F (ABS, Boolean product, L0, definition). It is built at physical planning, after every rewrite, so the operator nodes are gone and a node's body is stage 05's lowered terms |
| 2 | Built. Each thread routes its rows in `Sink`; the routes are merged in `Finalize` |
| 3 | Built for data values. A G over decisions holds a helper variable made at planning, not an expression |
| 4 | Built |
| 5 | Not built. Every nested decision reducer becomes a helper variable at planning (`AddValueHelper`); stage 06 removes the ones it can (`InlineReducerValueLinks`). The models still match the deck's on its example |

One clause exists in eight forms between planning and the solver: the bound expression with its marker strings and
side tables on `LogicalDecide`; the tree's nodes; the linear-form records inside the nodes; compiled programs;
per-thread containers; program value columns; the emitter's output; `SolverInput`. The deck has one, the tree.

## 3. Phase 5: the tree is the representation

**Target.** The parser builds the structure and every later stage works on it.

```text
Parser / transformer:  build the DECIDE structure (E, G, O nodes)
            |
Binder:                resolve names and types in it; derive signatures
            |
Canonicalizer, optimizer:  analyse and transform it
            |
     choose an implementation
        /              \
direct relational plan    compiled tree execution
                                  |
                       model written at the nodes (Phase 6)
```

- **Parser.** The transformer creates explicit scope, reducer and frame nodes. The grammar builds no execution
  objects, and ordinary SQL expressions inside the nodes stay DuckDB expressions. `DecideFrameExpression` is already
  a parsed node of this kind.
- **Binder.** Resolves names, types, keys, aggregate overloads and collations in that structure and derives each
  node's signature. It refuses only what needs the parsed tree (BIND-4).
- **Canonicalizer and optimizer.** Transform the bound tree or apply a rule at a node. No stage rediscovers
  structure from marker strings or helper records.
- **One home per fact.** The clause list is a list of clause records (ARCH-1). A reducer's `per` and `by`, a norm and
  a frame are fields or node classes, not markers. The linear-form records and the side tables go as their readers
  move. Nodes are split by kind, as DuckDB's `Expression` is.

**Pattern.** A typed intermediate representation with explicit lowering. DuckDB's own form is one structure in two
typed phases, a parsed class and a bound class, as `ParsedExpression` and `Expression` are. Use that unless there is
a reason not to.

**Contracts the design must state**

1. **Meaning apart from execution state.** Scopes, decision identity, expressions and source locations belong to
   the tree. Programs, column positions, containers, hash tables, solver indices and sparse rows are derived from it
   at a named boundary. Derived structures are fine; independently kept copies of the same meaning are the problem.
2. **DuckDB can see every expression.** Column binding, column pruning and the optimizer's visitors must reach each
   SQL expression the tree owns. Today `LogicalDecide::EnumerateExpressions` is the one list they use, and the tree
   avoids the question by being built after DuckDB's optimizer has run. This is the largest technical risk.
3. **Copy, equality and serialization** at each phase boundary, covering prepared statements and repeated execution
   (`DECIDB_VERIFY_SERIALIZER=1`).
4. **What each rewrite invalidates.** A rewrite can make signatures, degree, bounds, frame placement or
   dependencies stale. State which analyses it preserves and which are recomputed.
5. **An error is raised where it is found** (`../docs/language/errors.md` § How DECIDE throws an error). The first
   error the evaluation reaches stops the query, and no order among errors is owed (R10, R11). It points at the user's
   text when the expression in hand has a location, and nothing is built to give it one.

**Order** (ARCH-1 decides): front to back, with an adapter that lowers the new bound tree to today's tagged expressions
for the stages not yet converted. Errors follow R11: the first one reached is raised.

**Size** [measured]: parser about 1,300 lines, binder 3,600, the rest of planning 11,500, optimizer rewrites 7,800.

**Accepted when**

- scopes, reducers and frames originate in parsing, and binding and every rewrite use them;
- evaluation and model generation do not reconstruct them from helper lists or marker strings;
- no DECIDE tag constant remains in `enums/decide.hpp` (R20): the tag list is this phase's checklist, each tag
  replaced by a node, a record or a field, and the refusal of user names that contain a tag goes with the last one;
- behaviour tests exist at each boundary: parser and binder, rewrites, serialization, direct solve against its
  fallback, and parallel execution.

**A lesson that binds this phase.** Sami's tip answered an infeasible query because a rewrite turned a clause from
one shape into another after the pass that reduces its bound had run on the old shape. Building the tree earlier
does not prevent that by itself; contract 4 does.

## 4. Phase 6: the model is written at the nodes

- **The node rule.** A SUM or AVG passes its members up. A MIN or MAX allocates one variable per group that has more
  than one member, plus its rows. How a MIN or MAX is written depends on which way its clause or the objective
  pushes it (`ExtremumPush`), not only on its member count, so the analysis of a node's readers stays.
- **Rows per group, not per input row.** Today the emitter scatters node values into arrays as long as the input
  and stage 06 regroups them. [reproduced], `repro/clauses_times_rows.sql`: over 2 million rows, going from 1 clause
  to 40 takes evaluation from 44 ms to 638 ms and peak memory from 1.1 GB to 2.5 GB, while the solve goes from
  198 ms to 250 ms. `EmittedRows` and `ReducerValueMembers` in `solver_input.hpp` already have the per-group shape.
- **Delete:** the plan-time helper allocation, `InlineReducerValueLinks`, `PlaceHelpers`, the helper mappings, and
  the emitter's scatter with the row loops of `decide_value_extraction.cpp`.
- **Survives:** routing, containers, the key map, data programs and folds, frames, the no-value rule
  (`DecideAbsence`).

**Constraints the design must meet**

- Helper inlining and implied-bound propagation need every reader of a helper, which one node does not see (ARCH-2).
- `FormulateModel` stays a pure function of the evaluated clauses and a box, re-runnable for `DIAGNOSE` without
  reading data. Writing at the nodes must not bake in Big-M constants.
- Variables allocated at run time change names in `EXPLAIN` and `DIAGNOSE` (ARCH-2).
- The deck has no rule for ABS, products, L0, `<>`, AVG, norms or quadratic terms. Each needs one, and each is a
  departure to present (ARCH-2). The F nodes mirror the plan-time rewrites and write nothing yet.

**How to change it safely.** Run the old and the new model building side by side and compare the solver models
while both exist. An earlier attempt composed values at the nodes beside stage 06; it changed no model and removed
no code, and was reverted. Step 5 has to replace the helper logic, not sit next to it.

## 5. Direct solve and the tree

Direct solve reads its own `DirectIR`, built from `LogicalDecide` with the solver path's readers, and never the
tree. By R17, each rule reads the bound tree directly and `DirectIR` goes; this lands in Phase 5, once the parser
builds the tree before the rules run. A rule that fires lowers to ordinary relational operators and reads its bound
values from SQL side tables; a miss leaves the solver path's meaning unchanged. Keep the driver, the rule interface
and the shared readers; they are small and rule-agnostic.

## 6. Tests and docs

- **Keep** every test judged by the independent oracle, and the regression tests of fixed defects. They survive an
  internal rewrite.
- **Cut** a test when the code it alone reaches is deleted, in the same change. A test of dump format or of exact
  model text does not survive Phase 6; move a query worth keeping into the golden corpus.
- **Add** an oracle test for each defect task; for Phase 5, behaviour tests at each
  boundary the tree crosses; for Phase 6, the model comparison between old and new model building.
- **The rule** (`.claude/CLAUDE.md`): each decision point gets a test that reaches it and fails if the branch is
  removed.
- **Docs.** Each phase rewrites `context/docs` for the stages it touches, in the present tense. When Phase 6 is
  done, this file's content lives in the stage docs and this folder goes.

## 7. What Phases 5 and 6 replace

Cleaning this code before the phase that replaces it is work done twice.

| What the review found | Replaced by |
|---|---|
| Each clause is lowered twice at planning (`DecideTreeConstruction` and `LowerDecideTree`), joined by expression address; 82 internal cross-checks | Phase 5: the rewrites produce the tree |
| One AND expression for all clauses, with `when` and `per` as tagged conjunctions | Phase 5: the clause record (ARCH-1) |
| `norm` bound as a `SUM` with a patched return type; a frame as a tagged aggregate guarded at 28 sites | Phase 5: node classes |
| `DecideNode` is one struct of 67 fields; `DecideTree::Validate` has complexity 142 | Phase 5: nodes split by kind |
| The emitter scatters node values into arrays as long as the input; evaluation scales with clauses times rows (`repro/clauses_times_rows.sql`) | Phase 6 |
| Helpers are created at planning and removed by `InlineReducerValueLinks` (300 lines, complexity 90) | Phase 6 |
