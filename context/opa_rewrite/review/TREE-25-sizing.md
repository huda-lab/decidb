# TREE-25 — sizing of "terms split from the finished nodes"

Read against the tree at `b07735ce24` (after TREE-7, TREE-23, TREE-24). Line numbers are of that commit.

## 0. Verdict

- The form is buildable. It is the largest change left in Phase 5 beside TREE-8, and it moves three orders that the
  model dumps and goldens pin.
- Recommendation: option 2 for Phase 5. Keep the two readings, as design.md § 3.2.4 states them, and hand this
  note to ARCH-2 as an input. Phase 6 writes the model at the nodes (design.md § 4), which decides where a term is
  produced; building the node-side splitter before that design fixes the node contract risks building it twice.

## 1. The two readings today

| Reading | Reader | Input | Output |
|---|---|---|---|
| What a node reads | `DecideTreeBuilder::Lower` (`decide_tree.cpp`) | the bound clause | nodes, columns, sigma fields, lowered bodies |
| Coefficient algebra | `DecideLinearFormBuilder` (`decide_linear_form.cpp`) with `DecideTermSplitter` | the same bound clause | `DecideTerm`s, product groups |

They are joined by handing each atom with its G (TREE-7). Nothing is joined by address.

## 2. What the single reading removes

| Goes | Where | Size |
|---|---|---|
| `LowerFrames` and its eight overloads, `LowerFramesOnce`, `LowerFramesInPreparedModel` | `decide_linear_form.cpp:337-470` | about 135 lines |
| `RecordFallbackColumns` and the record pointers (`clauses`, `objective`, `links`) | `decide_linear_form.cpp:318-336, 977-982` | about 25 lines |
| One of `Translate` / `TranslateTree` | `decide_tree_compile.cpp:299, 423` | about 60 lines |
| One of `FrameColumn` / `FrameRead` | `decide_tree_compile.cpp:349, 368` | about 20 lines |
| Four agreement errors | `decide_linear_form.cpp:366`; `decide_tree_compile.cpp:355, 360, 381` | 4 internal errors |

About 240 lines and 4 internal errors.

## 3. What it adds

| Adds | Why | Cost |
|---|---|---|
| A leaf rule in `DecideTermSplitter` for a child read (a `BOUND_REF` past the input's columns) | a lowered body names a node where the clause named a reducer, a kept call or a frame | the splitter has 10 users (`grep "DecideTermSplitter "`); 9 never see a child read, so they carry a hook they do not use, or the splitter is split in two |
| Dispatch by node kind in the term half | `TryMatchScaledAggregate`, `DecideDefinitionOf` and the frame class match the bound clause's shapes, which a lowered body no longer holds | the three arms of `Clause` and the two of `AnalyzeObjective` are rewritten |
| The written text of a term kept beside its lowered form | `DecideProgram::written` and the messages that quote a coefficient read the bound expression | a field per program, or a map from lowered to written |

Estimated at 400 to 600 lines changed across `decide_term_split.cpp`, `decide_linear_form.cpp`,
`decide_tree_compile.cpp` and `decide_tree.cpp`. Not measured by a prototype.

## 4. Orders that move

| Order | Today | After | Who pins it |
|---|---|---|---|
| Frame-column slots | the term half registers `READ` and `FALLBACK` at split time, then `VALUE`/`REDUCED`/`DEFINED` over every record, then each `AGGREGATE` | node order | model dumps, the frame goldens |
| Like terms | two reducers with equal filters in one clause merge (`TermsAreLike` does not compare `child`) | per node, so they stop merging | model row coefficients in the dumps |
| Programs on an O | the order the term half meets the frame | node order | `test_one_pass_tree.py` tree dumps |
| Message text | quotes the bound expression | must be carried | error tests that quote a coefficient |

Each is a change a user can see only in `EXPLAIN` text, model dumps or a message, but each needs its goldens
inspected one by one.

## 5. Why not now

1. Phase 6's rule is "a SUM or AVG passes its members up, a MIN or MAX allocates per group" (design.md § 4). That is a
   term produced at a node. ARCH-2 has two open choices on the contract with the model builder. A node-side
   splitter built now would be shaped by today's records (`DecideConstraint::terms`), which Phase 6 replaces.
2. What Phase 5 set out to remove is gone: the second lowering and every join by address. What is left is one
   analysis the rewrites share (the splitter) run on the clause, beside the walk that builds nodes. Two analyses of
   one structure is the compiler norm (a type checker and a code generator both walk the tree).
3. The four agreement errors it would remove are in the frame registry. They are reachable only by a builder bug,
   and the frame tests cover both dialects.

## 6. Owner's decision

- Option 1: build it in Phase 5, with this note's § 2 as its Done-when and each moved order of § 4 inspected.
- Option 2 (recommended): keep two readings in Phase 5; ARCH-2 reads this note and decides the node-side form with
  the model contract.
